/* SPDX-License-Identifier: GPL-2.0-or-later
 * WRITE document storage and .WRI I/O.
 * Text characters reference character/paragraph format tables; CR LF carries
 * the paragraph format. Documents also hold fonts, page layout and running
 * headers/footers. Screen layout uses page text width with horizontal
 * scrolling; printer pagination supplies page marks and Go To Page.
 *
 * .WRI files contain a 128-byte header, text, character/paragraph run pages,
 * section data/table and fonts. Running-head paragraphs precede body text.
 * Bitmap/metafile pictures occupy separate paragraphs and a picture table;
 * Move Picture/Size Picture change offset/scale. Clipboard supports pictures.
 * Format reference: libwps MSWrite.cpp (https://sourceforge.net/projects/libwps/);
 * no code is taken from it.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "write.h"
#define TWIPS 1440
#define MAX_CHPS 128
#define MAX_PAPS 250 /* each picture has one of its own */
#define MAX_FONTS 16
#define TABS 12
#define MARGIN_PX 12 /* the window's left margin, where page marks go */
#define CHP_BOLD 1
#define CHP_ITALIC 2
#define CHP_UNDERLINE 4
#define CHP_PAGE 8 /* the special character: the page number */
#define PAGE_CHAR 1
#define PAGE_BREAK 12
#define PICTURE_CHAR 0x1a /* a picture's paragraph: this, then its CR LF */
#define PICTURE_HEADER 40
#define PICTURE_BITMAP 0xe3 /* a picture's first word: else the metafile's mapping mode, 0x80 added */

typedef struct {BYTE flags,font,hps; signed char pos;} Chp;
typedef struct {BYTE jc; short right,left,first; WORD line; WORD tabs[TABS]; WORD decimal; WORD picture;} Pap; /* picture: 1 + its index */
/* A picture as Write keeps it in its paragraph: a 40-byte header (the
 * mapping mode or PICTURE_BITMAP, the metafile's extents, the offset from
 * the margin, the size in twips, a bitmap's BITMAP, the header and data
 * sizes, the scale in thousandths), then the metafile or the bitmap's rows. */
typedef struct {BYTE *data; DWORD size;} Picture;
static Picture *pictures; static int picture_count,picture_cap;
static Chp chps[MAX_CHPS]; static int chp_count;
static Pap paps[MAX_PAPS]; static int pap_count;
static char fonts[MAX_FONTS][LF_FACESIZE]; static BYTE font_families[MAX_FONTS]; static int font_count;
/* The document. */
static char *text; static BYTE *cfmt,*pfmt; static int length,capacity;
static int anchor,caret,pending=-1; /* the selection; a character format to type with */
static int page_w=12240,page_h=15840,m_left=1800,m_right=1800,m_top=1440,m_bottom=1440,first_page=1;
/* A header or footer: one line of text, # the page number, laid out with its
 * paragraph's indents, alignment and tab stops (centred by default). */
typedef struct {char text[128]; int distance; BOOL first; Pap pap;} Running;
static Running header={"",1080,FALSE,{1}},footer={"",1080,FALSE,{1}};
static HINSTANCE instance;
static HWND main_wnd;
static char file[260];
static BOOL modified;

static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
static void put16(BYTE *p,WORD v) {p[0]=(BYTE)v; p[1]=(BYTE)(v>>8);}
static void put32(BYTE *p,DWORD v) {put16(p,LOWORD(v)); put16(p+2,HIWORD(v));}

/* --- pictures ---------------------------------------------------------------------------- */
/* A picture's bytes, taken over: its number for a Pap (1 and up), 0 when there is no room. */
static int add_picture(BYTE *data,DWORD size) {
    if(size<PICTURE_HEADER) {GlobalFree(data); return 0;}
    if(picture_count==picture_cap) {
        int cap=picture_cap?picture_cap*2:8; Picture *n=(Picture *)GlobalAlloc(GPTR,(DWORD)cap*sizeof(Picture));
        if(!n) {GlobalFree(data); return 0;}
        if(pictures) {memcpy(n,pictures,(size_t)picture_count*sizeof(Picture)); GlobalFree(pictures);}
        pictures=n; picture_cap=cap;
    }
    pictures[picture_count].data=data; pictures[picture_count].size=size;
    return ++picture_count;
}
static void free_pictures(void) {
    int i;
    for(i=0;i<picture_count;i++) GlobalFree(pictures[i].data);
    picture_count=0;
}
static const Picture *picture_of(const Pap *p) {return p->picture && p->picture<=picture_count?&pictures[p->picture-1]:NULL;}
/* Its size in twips: a metafile's from its header, a bitmap's at 96 pixels an
 * inch, each scaled by thousandths (a scale of 10 or less is none). */
static void picture_base(const BYTE *b,int *w,int *h) {
    if(get16(b)==PICTURE_BITMAP) {*w=get16(b+18)*TWIPS/96; *h=get16(b+20)*TWIPS/96;}
    else {*w=get16(b+10); *h=get16(b+12);}
    *w=max(1,*w); *h=max(1,*h);
}
static void picture_size(const Picture *p,int *w,int *h) {
    const BYTE *b=p->data; int mx=get16(b+36),my=get16(b+38);
    if(mx<=10) mx=1000;
    if(my<=10) my=1000;
    picture_base(b,w,h);
    *w=max(1,MulDiv(*w,mx,1000)); *h=max(1,MulDiv(*h,my,1000));
}
/* A bitmap's rows (as GetBitmapBits gives them: monochrome, 4 planes, or
 * 1 plane of 4, 8, 24 or 32 bits; VGA colors for indexes) as a top-down
 * 32-bit DIB, the bits after the header. */
static BITMAPINFO *picture_dib(const Picture *p) {
    static const DWORD vga[16]={0,0x800000,0x8000,0x808000,0x80,0x800080,0x8080,0xc0c0c0,0x808080,0xff0000,0xff00,0xffff00,0xff,0xff00ff,0xffff,0xffffff};
    const BYTE *b=p->data,*bits=b+PICTURE_HEADER; int w=get16(b+18),h=get16(b+20),row=get16(b+22),planes=b[24],bpp=b[25],x,y,k;
    BITMAPINFO *info; DWORD *out;
    if(w<=0 || h<=0 || (DWORD)row*h*planes>p->size-PICTURE_HEADER) return NULL;
    if(!(info=(BITMAPINFO *)GlobalAlloc(GPTR,sizeof(BITMAPINFOHEADER)+(DWORD)w*h*4))) return NULL;
    info->bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info->bmiHeader.biWidth=w; info->bmiHeader.biHeight=-h;
    info->bmiHeader.biPlanes=1; info->bmiHeader.biBitCount=32;
    out=(DWORD *)((BYTE *)info+sizeof(BITMAPINFOHEADER));
    for(y=0;y<h;y++) for(x=0;x<w;x++) {
        const BYTE *r=bits+(DWORD)y*row*planes; DWORD v=0;
        if(planes>1) {for(k=0;k<planes && k<4;k++) if(r[k*row+x/8]&(0x80>>(x%8))) v|=1u<<k; v=vga[v&15];}
        else switch(bpp) {
        case 1: v=r[x/8]&(0x80>>(x%8))?0xffffff:0; break;
        case 4: v=vga[(x&1?r[x/2]:r[x/2]>>4)&15]; break;
        case 8: v=vga[r[x]&15]; break;
        case 24: v=r[x*3]|(DWORD)r[x*3+1]<<8|(DWORD)r[x*3+2]<<16; break;
        default: v=get32(r+x*4)&0xffffff;
        }
        out[(DWORD)y*w+x]=v;
    }
    return info;
}
/* The picture drawn into (x,y)-(x+w,y+h): a bitmap stretched; a metafile in
 * its mapping mode, an isotropic or anisotropic one filling the rectangle. */
static void draw_picture(HDC dc,const Picture *p,int x,int y,int w,int h) {
    const BYTE *b=p->data; int saved=SaveDC(dc);
    IntersectClipRect(dc,x,y,x+w,y+h);
    if(get16(b)==PICTURE_BITMAP) {
        BITMAPINFO *info=picture_dib(p);
        if(info) {
            SetStretchBltMode(dc,COLORONCOLOR);
            StretchDIBits(dc,x,y,w,h,0,0,(int)info->bmiHeader.biWidth,(int)-info->bmiHeader.biHeight,(BYTE *)info+sizeof(BITMAPINFOHEADER),info,DIB_RGB_COLORS,SRCCOPY);
            GlobalFree(info);
        }
    } else {
        DWORD size=min(get32(b+32),p->size-PICTURE_HEADER); HMETAFILE mf=SetMetaFileBitsEx(size,b+PICTURE_HEADER); int mm=get16(b)&0x7f;
        POINT corner[2];
        /* The rectangle on the device: the DC's own origin (scrolled) goes. */
        corner[0].x=x; corner[0].y=y; corner[1].x=x+w; corner[1].y=y+h; LPtoDP(dc,corner,2);
        if(mf) {
            SetMapMode(dc,mm?mm:MM_ANISOTROPIC); SetWindowOrgEx(dc,0,0,NULL);
            if((short)get16(b+2)>0 && (short)get16(b+4)>0) SetWindowExtEx(dc,(short)get16(b+2),(short)get16(b+4),NULL);
            SetViewportOrgEx(dc,corner[0].x,corner[0].y,NULL); SetViewportExtEx(dc,corner[1].x-corner[0].x,corner[1].y-corner[0].y,NULL);
            PlayMetaFile(dc,mf);
            DeleteMetaFile(mf);
        }
    }
    RestoreDC(dc,saved);
}

/* --- formats --------------------------------------------------------------------------- */
static int intern_chp(const Chp *c) {
    int i;
    for(i=0;i<chp_count;i++) if(!memcmp(&chps[i],c,sizeof(*c))) return i;
    if(chp_count==MAX_CHPS) return 0;
    chps[chp_count]=*c; return chp_count++;
}
static int intern_pap(const Pap *p) {
    int i;
    for(i=0;i<pap_count;i++) if(!memcmp(&paps[i],p,sizeof(*p))) return i;
    if(pap_count==MAX_PAPS) return 0;
    paps[pap_count]=*p; return pap_count++;
}
static int font_index(LPCSTR name,BYTE family) {
    int i;
    for(i=0;i<font_count;i++) if(!lstrcmpi(fonts[i],name)) return i;
    if(font_count==MAX_FONTS) return 0;
    lstrcpyn(fonts[font_count],name,LF_FACESIZE); font_families[font_count]=family;
    return font_count++;
}
static void reset_formats(void) {
    Chp c; Pap p;
    chp_count=pap_count=font_count=0; free_pictures();
    font_index("Helv",FF_SWISS);
    memset(&c,0,sizeof(c)); c.hps=24; intern_chp(&c);
    memset(&p,0,sizeof(p)); p.line=240; intern_pap(&p);
}

/* --- the text ----------------------------------------------------------------------------- */
static BOOL reserve(int n) {
    if(length+n<=capacity) return TRUE;
    {
        int cap=max(capacity*2,length+n+256); char *t; BYTE *c,*p;
        t=(char *)GlobalAlloc(GPTR,(DWORD)cap); c=(BYTE *)GlobalAlloc(GPTR,(DWORD)cap); p=(BYTE *)GlobalAlloc(GPTR,(DWORD)cap);
        if(!t || !c || !p) {if(t) GlobalFree(t); if(c) GlobalFree(c); if(p) GlobalFree(p); return FALSE;}
        if(length) {memcpy(t,text,(size_t)length); memcpy(c,cfmt,(size_t)length); memcpy(p,pfmt,(size_t)length);}
        if(text) {GlobalFree(text); GlobalFree(cfmt); GlobalFree(pfmt);}
        text=t; cfmt=c; pfmt=p; capacity=cap;
    }
    return TRUE;
}
static void insert(int at,const char *s,int n,int chp,int pap) {
    if(n<=0 || !reserve(n)) return;
    memmove(text+at+n,text+at,(size_t)(length-at)); memmove(cfmt+at+n,cfmt+at,(size_t)(length-at)); memmove(pfmt+at+n,pfmt+at,(size_t)(length-at));
    memcpy(text+at,s,(size_t)n); memset(cfmt+at,chp,(size_t)n); memset(pfmt+at,pap,(size_t)n);
    length+=n;
}
static void erase(int a,int b) {
    if(b<=a) return;
    memmove(text+a,text+b,(size_t)(length-b)); memmove(cfmt+a,cfmt+b,(size_t)(length-b)); memmove(pfmt+a,pfmt+b,(size_t)(length-b));
    length-=b-a;
}
/* A paragraph: from after the previous CR LF up to and including its own. */
static int para_start(int i) {while(i>0 && text[i-1]!='\n') i--; return i;}
static int para_end(int i) {while(i<length && text[i]!='\n') i++; return i<length?i+1:length;}
static int para_pap(int start) {
    int e=para_end(start);
    return e>start?pfmt[e-1]:0;
}
static BOOL picture_at(int i) {return i<length && text[i]==PICTURE_CHAR && picture_of(&paps[pfmt[i]]) && (!i || text[i-1]=='\n');}
/* The start of the picture's paragraph pos is in (at the picture or its
 * paragraph mark), or -1. */
static int picture_start(int pos) {
    int ps;
    if(pos<0 || pos>=length) return -1;
    ps=para_start(pos);
    return text[ps]==PICTURE_CHAR && picture_of(&paps[pfmt[ps]])?ps:-1;
}
/* A paragraph format for text next to at: the one before, unless a picture's. */
static int text_pap(int at) {int k=at>0?pfmt[at-1]:0; return paps[k].picture?0:k;}
/* Text left in front of a picture gets a paragraph of its own. */
static void keep_picture_paragraph(int at) {
    if(at>0 && picture_start(at)==at && text[at-1]!='\n') insert(at,"\r\n",2,cfmt[at-1],text_pap(at));
}
static int sel_min(void) {return min(anchor,caret);}
static int sel_max(void) {return max(anchor,caret);}
/* The format typed text takes: the one just set, or the character before. */
static int current_chp(void) {
    if(pending>=0) return pending;
    if(caret>0 && text[caret-1]!='\n') return cfmt[caret-1];
    if(caret<length) return cfmt[caret];
    return caret>0?cfmt[caret-1]:0;
}

/* --- undo: the whole document before the last change ----------------------------------------- */
static struct {char *text; BYTE *cfmt,*pfmt; int length,anchor,caret;} undo;
static void forget_undo(void) {
    if(undo.text) {GlobalFree(undo.text); GlobalFree(undo.cfmt); GlobalFree(undo.pfmt);}
    memset(&undo,0,sizeof(undo));
}
static void snapshot(void) {
    DWORD n=(DWORD)length+1;
    forget_undo();
    undo.text=(char *)GlobalAlloc(GPTR,n); undo.cfmt=(BYTE *)GlobalAlloc(GPTR,n); undo.pfmt=(BYTE *)GlobalAlloc(GPTR,n);
    if(!undo.text || !undo.cfmt || !undo.pfmt) {forget_undo(); return;}
    if(length) {memcpy(undo.text,text,(size_t)length); memcpy(undo.cfmt,cfmt,(size_t)length); memcpy(undo.pfmt,pfmt,(size_t)length);}
    undo.length=length; undo.anchor=anchor; undo.caret=caret;
}

/* --- devices: fonts and widths for the screen or the printer ---------------------------------- */
typedef struct {
    HDC dc; int dpi_x,dpi_y; BOOL printing; int page_number;
    HFONT fonts[MAX_CHPS]; short widths[MAX_CHPS][256]; short height[MAX_CHPS],ascent[MAX_CHPS]; BYTE ready[MAX_CHPS];
} Device;
static Device screen,printer;
static void device_reset(Device *d) {
    int i;
    for(i=0;i<MAX_CHPS;i++) if(d->ready[i]) {DeleteObject(d->fonts[i]); d->ready[i]=0;}
}
static void device_font(Device *d,int c) {
    LOGFONT lf; TEXTMETRIC tm; int w[256],i; HFONT old; const Chp *chp=&chps[c];
    if(d->ready[c]) return;
    memset(&lf,0,sizeof(lf));
    lf.lfHeight=-MulDiv(chp->pos?chp->hps*2/3:chp->hps,d->dpi_y,144);
    lf.lfWeight=chp->flags&CHP_BOLD?FW_BOLD:FW_NORMAL; lf.lfItalic=(BYTE)((chp->flags&CHP_ITALIC)!=0);
    lf.lfUnderline=(BYTE)((chp->flags&CHP_UNDERLINE)!=0);
    lstrcpyn(lf.lfFaceName,fonts[chp->font<font_count?chp->font:0],LF_FACESIZE);
    d->fonts[c]=CreateFontIndirect(&lf);
    old=(HFONT)SelectObject(d->dc,d->fonts[c]);
    GetTextMetrics(d->dc,&tm); GetCharWidth(d->dc,0,255,w);
    for(i=0;i<256;i++) d->widths[c][i]=(short)w[i];
    d->height[c]=(short)(tm.tmHeight+tm.tmExternalLeading); d->ascent[c]=(short)tm.tmAscent;
    SelectObject(d->dc,old);
    d->ready[c]=1;
}
static int px_x(const Device *d,int twips) {return MulDiv(twips,d->dpi_x,TWIPS);}
static int px_y(const Device *d,int twips) {return MulDiv(twips,d->dpi_y,TWIPS);}
/* The page number as the special character shows it. */
static void page_text(const Device *d,char *out) {
    if(d->printing) wsprintf(out,"%d",d->page_number); else lstrcpy(out,"(page)");
}
static int text_width(Device *d,int c,LPCSTR s) {int w=0; device_font(d,c); while(*s) w+=d->widths[c][(BYTE)*s++]; return w;}
/* The next tab stop after x (from the column's left edge): the paragraph's, else every half inch. */
static int next_tab(const Device *d,const Pap *p,int x) {
    int i,t;
    for(i=0;i<TABS && p->tabs[i];i++) if((t=px_x(d,p->tabs[i]))>x) return t;
    t=px_x(d,720);
    return (x/t+1)*t;
}
static int char_width(Device *d,int i,int x) {
    char page[16]; BYTE ch=(BYTE)text[i]; int c=cfmt[i];
    if(ch=='\r' || ch=='\n' || ch==PAGE_BREAK) return 0;
    if(ch=='\t') return next_tab(d,&paps[pfmt[i]],x)-x;
    if(ch==PICTURE_CHAR && picture_of(&paps[pfmt[i]])) {int w,h; picture_size(picture_of(&paps[pfmt[i]]),&w,&h); return px_x(d,w);}
    device_font(d,c);
    if(ch==PAGE_CHAR && (chps[c].flags&CHP_PAGE)) {page_text(d,page); return text_width(d,c,page);}
    return d->widths[c][ch];
}

/* --- layout: lines of a column, paragraph by paragraph -------------------------------------- */
typedef struct {int start,end,y,height,ascent,x;} Line;
typedef void (*LineSink)(const Line *,void *);
/* Lines from start to end, wrapped at width pixels; y counts down the column.
 * When the text ends with a paragraph mark (or is empty) the end has a line
 * of its own, for the caret. */
static int layout(Device *d,int start,int end,int width,int y,LineSink sink,void *data) {
    int ps=start;
    while(ps<end) {
        int pe=min(para_end(ps),end),i=ps; const Pap *p=&paps[para_pap(ps)]; BOOL first=TRUE;
        int left=px_x(d,p->left),right=px_x(d,p->right),first_x=px_x(d,p->left+p->first);
        if(picture_of(p)) {
            /* A picture's paragraph is one line as tall as the picture: at its
             * offset from the margin, or centred or at the right. */
            Line l; int w,h; picture_size(picture_of(p),&w,&h);
            w=px_x(d,w); l.start=ps; l.end=pe; l.y=y; l.height=l.ascent=max(1,px_y(d,h));
            l.x=p->jc==1?max(0,(width-w)/2):p->jc==2?max(0,width-w):px_x(d,(short)get16(picture_of(p)->data+8));
            if(sink) sink(&l,data);
            y+=l.height; ps=pe;
            continue;
        }
        do {
            Line l; int indent=first?first_x:left,avail=max(1,width-indent-right),x=0,j=i,brk=-1,used=0,c;
            l.start=i; l.height=0; l.ascent=0;
            while(j<pe) {
                BYTE ch=(BYTE)text[j]; int w;
                if(ch=='\r' || ch=='\n') {j++; continue;}
                if(ch==PAGE_BREAK) {if(j==i) j++; break;}
                w=char_width(d,j,indent+x);
                if(x+w>avail && j>i) {if(brk>i) j=brk; break;}
                c=cfmt[j]; device_font(d,c);
                if(d->height[c]>l.height) l.height=d->height[c];
                if(d->ascent[c]>l.ascent) l.ascent=d->ascent[c];
                x+=w;
                if(ch==' ' || ch=='\t') brk=j+1;
                if(ch!=' ') used=x;
                j++;
            }
            while(j<pe && (text[j]==' ' || text[j]=='\r' || text[j]=='\n')) j++;
            if(!l.height) {c=cfmt[pe-1]; device_font(d,c); l.height=d->height[c]; l.ascent=d->ascent[c];}
            l.height=MulDiv(l.height,p->line?p->line:240,240);
            l.end=j; l.y=y;
            l.x=max(0,indent+(p->jc==1?(avail-used)/2:p->jc==2?avail-used:0));
            if(sink) sink(&l,data);
            y+=l.height; i=j; first=FALSE;
        } while(i<pe);
        ps=pe;
    }
    if(end==length && (!length || text[length-1]=='\n')) {
        Line l; int c=current_chp();
        device_font(d,c);
        l.start=l.end=length; l.y=y; l.height=d->height[c]; l.ascent=d->ascent[c];
        l.x=px_x(d,paps[length?pfmt[length-1]:0].left);
        if(sink) sink(&l,data);
        y+=l.height;
    }
    return y;
}
/* The x of position at within a line, from its left. */
static int line_x(Device *d,const Line *l,int at) {
    int x=l->x,i;
    for(i=l->start;i<at && i<l->end;i++) x+=char_width(d,i,x);
    return x;
}

/* --- the screen ----------------------------------------------------------------------------- */
static Line *lines; static int line_count,line_cap,doc_height;
static int scroll_x,scroll_y,column_px; /* the column scrolls sideways when the window is narrower */
static int *page_starts,page_count; /* from the last pagination */
static void collect(const Line *l,void *data) {
    (void)data;
    if(line_count==line_cap) {
        int cap=line_cap?line_cap*2:256; Line *n=(Line *)GlobalAlloc(GPTR,(DWORD)cap*sizeof(Line));
        if(!n) return;
        if(lines) {memcpy(n,lines,(size_t)line_count*sizeof(Line)); GlobalFree(lines);}
        lines=n; line_cap=cap;
    }
    lines[line_count++]=*l;
}
static int client_height(void) {RECT r; GetClientRect(main_wnd,&r); return r.bottom;}
static int client_width(void) {RECT r; GetClientRect(main_wnd,&r); return r.right;}
static void update_scroll(void) {
    int range=max(0,doc_height-client_height()),range_x=max(0,MARGIN_PX+column_px+MARGIN_PX-client_width());
    if(scroll_y>range) scroll_y=range;
    if(scroll_x>range_x) scroll_x=range_x;
    SetScrollRange(main_wnd,SB_VERT,0,max(range,1),FALSE); SetScrollPos(main_wnd,SB_VERT,scroll_y,TRUE);
    SetScrollRange(main_wnd,SB_HORZ,0,max(range_x,1),FALSE); SetScrollPos(main_wnd,SB_HORZ,scroll_x,TRUE);
}
static void relayout(void) {
    line_count=0;
    column_px=px_x(&screen,page_w-m_left-m_right);
    doc_height=layout(&screen,0,length,column_px,0,collect,NULL);
    update_scroll();
}
static int line_of(int pos) {
    int i;
    for(i=0;i<line_count;i++) if(pos<lines[i].end || (pos==lines[i].end && (i==line_count-1 || lines[i].end==lines[i].start))) return i;
    return line_count?line_count-1:0;
}
static void place_caret(void) {
    int i=line_of(caret); const Line *l=&lines[i];
    if(!line_count || GetFocus()!=main_wnd) return;
    DestroyCaret(); CreateCaret(main_wnd,NULL,1,l->height); ShowCaret(main_wnd);
    SetCaretPos(MARGIN_PX+line_x(&screen,l,caret)-scroll_x,l->y-scroll_y);
}
static void show_caret(void) {
    int i=line_of(caret); const Line *l;
    if(!line_count) return;
    l=&lines[i];
    if(l->y<scroll_y) scroll_y=l->y;
    else if(l->y+l->height>scroll_y+client_height()) scroll_y=l->y+l->height-client_height();
    {
        /* Sideways too, a quarter of the window at a time. */
        int x=MARGIN_PX+line_x(&screen,l,caret),w=client_width();
        if(x-scroll_x<MARGIN_PX) scroll_x=max(0,x-MARGIN_PX-w/4);
        else if(x-scroll_x>w-4) scroll_x=x-w+w/4;
    }
    update_scroll(); InvalidateRect(main_wnd,NULL,TRUE); place_caret();
}
static void changed(void) {modified=TRUE; relayout(); show_caret();}
/* Character i of line l at x, the line's top at y: raised or lowered as its
 * format has it, the special character as the page number. */
static void draw_char(HDC dc,const Device *d,const Line *l,int i,int x,int y) {
    BYTE ch=(BYTE)text[i]; int c=cfmt[i];
    if(ch>=' ' || (ch==PAGE_CHAR && (chps[c].flags&CHP_PAGE))) {
        char page[16]; int up=chps[c].pos>0?-l->ascent/3:chps[c].pos<0?l->ascent/4:0;
        y+=l->ascent-d->ascent[c]+up;
        SelectObject(dc,d->fonts[c]);
        if(ch==PAGE_CHAR) {page_text(d,page); TextOut(dc,x,y,page,lstrlen(page));}
        else TextOut(dc,x,y,(LPCSTR)&text[i],1);
    }
}
static void paint_line(HDC dc,const Line *l) {
    Device *d=&screen; int x=l->x,i,y=l->y-scroll_y,a=max(sel_min(),l->start),b=min(sel_max(),l->end);
    if(picture_start(l->start)==l->start) {
        /* A picture, inverted when selected. */
        const Picture *pic=picture_of(&paps[pfmt[l->start]]); int w,h;
        picture_size(pic,&w,&h); w=px_x(d,w);
        draw_picture(dc,pic,MARGIN_PX+l->x,y,w,l->height);
        if(sel_min()<=l->start && sel_max()>l->start) {RECT r; SetRect(&r,MARGIN_PX+l->x,y,MARGIN_PX+l->x+w,y+l->height); InvertRect(dc,&r);}
        return;
    }
    for(i=l->start;i<l->end;i++) {
        int w=char_width(d,i,x);
        if(text[i]==PAGE_BREAK) {
            RECT r; SetRect(&r,MARGIN_PX,y+l->height/2,MARGIN_PX+column_px,y+l->height/2+1);
            FillRect(dc,&r,(HBRUSH)GetStockObject(GRAY_BRUSH));
        } else draw_char(dc,d,l,i,MARGIN_PX+x,y);
        x+=w;
    }
    /* The selection inverted; a selected paragraph mark shows as a small block. */
    if(sel_min()!=sel_max() && a<b) {
        RECT r; int sx=line_x(d,l,a),ex=line_x(d,l,b);
        if(b==l->end && b>l->start && (text[b-1]=='\n' || text[b-1]=='\r')) ex+=6;
        SetRect(&r,MARGIN_PX+sx,y,MARGIN_PX+max(ex,sx+1),y+l->height);
        InvertRect(dc,&r);
    }
}
/* --- moving and sizing a picture ------------------------------------------------------------- */
/* Move Picture and Size Picture: an outline follows the arrow keys (an eighth
 * of an inch a step) or the mouse; Enter or a click keeps it (a moved picture
 * is placed from the margin, a sized one scaled), Esc leaves it as it was. */
static int adjusting,adjust_at,adjust_x,adjust_w,adjust_h; /* the command, the picture's paragraph; twips */
static void adjust_rect(RECT *r) {
    const Line *l=&lines[line_of(adjust_at)]; int x=adjusting==IDM_MOVEPICTURE?px_x(&screen,adjust_x):l->x;
    SetRect(r,MARGIN_PX+x,l->y-scroll_y,MARGIN_PX+x+px_x(&screen,adjust_w),l->y-scroll_y+px_y(&screen,adjust_h));
}
static void start_adjust(int command) {
    int ps=picture_start(sel_min());
    if(ps<0 || !line_count) return;
    picture_size(picture_of(&paps[pfmt[ps]]),&adjust_w,&adjust_h);
    adjusting=command; adjust_at=ps; adjust_x=MulDiv(lines[line_of(ps)].x,TWIPS,screen.dpi_x);
    HideCaret(main_wnd);
    InvalidateRect(main_wnd,NULL,TRUE);
}
static void end_adjust(BOOL keep) {
    if(!adjusting) return;
    if(keep) {
        const Picture *pic=picture_of(&paps[pfmt[adjust_at]]); BYTE *data=(BYTE *)GlobalAlloc(GMEM_FIXED,pic->size); Pap p=paps[pfmt[adjust_at]]; int n,w,h;
        if(data) {
            memcpy(data,pic->data,pic->size);
            if(adjusting==IDM_MOVEPICTURE) {put16(data+8,(WORD)max(0,adjust_x)); p.jc=0;}
            else {picture_base(data,&w,&h); put16(data+36,(WORD)max(11,MulDiv(adjust_w,1000,w))); put16(data+38,(WORD)max(11,MulDiv(adjust_h,1000,h)));}
            if((p.picture=(WORD)add_picture(data,pic->size))!=0 && paps[n=intern_pap(&p)].picture) {
                snapshot(); memset(pfmt+adjust_at,n,3); modified=TRUE;
            }
        }
    }
    adjusting=0;
    relayout(); InvalidateRect(main_wnd,NULL,TRUE); ShowCaret(main_wnd); show_caret();
}
/* Keys and the mouse while adjusting; the column keeps the outline. */
static void adjust_by(int dx,int dy) {
    int step=TWIPS/8,column=page_w-m_left-m_right;
    if(adjusting==IDM_MOVEPICTURE) adjust_x=max(0,min(column-adjust_w,adjust_x+dx*step));
    else {adjust_w=max(step,min(column,adjust_w+dx*step)); adjust_h=max(step,adjust_h+dy*step);}
    InvalidateRect(main_wnd,NULL,TRUE);
}
static void adjust_to(int x,int y) {
    RECT r; int column=page_w-m_left-m_right; adjust_rect(&r);
    x+=scroll_x;
    if(adjusting==IDM_MOVEPICTURE) adjust_x=max(0,min(column-adjust_w,MulDiv(x-MARGIN_PX,TWIPS,screen.dpi_x)-adjust_w/2));
    else {adjust_w=max(TWIPS/8,min(column,MulDiv(x-r.left,TWIPS,screen.dpi_x))); adjust_h=max(TWIPS/8,MulDiv(y-r.top,TWIPS,screen.dpi_y));}
    InvalidateRect(main_wnd,NULL,TRUE);
}
static void paint(HDC dc,const RECT *area) {
    int i,p; HFONT old=(HFONT)SelectObject(dc,GetStockObject(SYSTEM_FONT));
    SetBkMode(dc,TRANSPARENT);
    for(i=0;i<line_count;i++) {
        const Line *l=&lines[i];
        if(l->y-scroll_y>=area->bottom || l->y+l->height-scroll_y<=area->top) continue;
        paint_line(dc,l);
        for(p=1;p<page_count;p++) if(page_starts[p]>=l->start && page_starts[p]<max(l->end,l->start+1)) {
            SelectObject(dc,GetStockObject(SYSTEM_FONT)); TextOut(dc,0,l->y-scroll_y,"\xbb",1);
        }
    }
    if(adjusting) {RECT r; adjust_rect(&r); DrawFocusRect(dc,&r);}
    SelectObject(dc,old);
}
static int position_at(int x,int y) {
    int i,pos,cx;
    if(!line_count) return 0;
    y+=scroll_y; x+=scroll_x-MARGIN_PX;
    for(i=0;i<line_count-1 && y>=lines[i].y+lines[i].height;i++) {}
    {
        const Line *l=&lines[i];
        cx=l->x; pos=l->start;
        while(pos<l->end && text[pos]!='\r' && text[pos]!='\n') {
            int w=char_width(&screen,pos,cx);
            if(x<cx+w/2) break;
            cx+=w; pos++;
        }
        if(pos==l->end && pos>l->start && i<line_count-1 && text[pos-1]==' ') pos--;
    }
    return pos;
}

/* --- moving and selecting ----------------------------------------------------------------------- */
static int step(int pos,int dir) {
    if(dir<0) {if(pos>=2 && text[pos-1]=='\n' && text[pos-2]=='\r') return pos-2; return pos>0?pos-1:0;}
    if(pos+1<length && text[pos]=='\r' && text[pos+1]=='\n') return pos+2;
    return pos<length?pos+1:length;
}
static void move(int pos,BOOL extend) {
    int ps;
    InvalidateRect(main_wnd,NULL,anchor!=caret);
    pos=max(0,min(length,pos));
    /* Not inside a picture's paragraph: past it going forward, else before it. */
    if((ps=picture_start(pos))>=0 && pos>ps) pos=pos>caret?para_end(ps):ps;
    caret=pos;
    if(!extend) anchor=caret;
    pending=-1;
    show_caret();
}
static void move_line(int dir,BOOL extend) {
    int i=line_of(caret),x=line_x(&screen,&lines[i],caret),j=i+dir;
    if(j<0 || j>=line_count) return;
    {
        const Line *l=&lines[j]; int pos=l->start,cx=l->x;
        while(pos<l->end && text[pos]!='\r' && text[pos]!='\n') {int w=char_width(&screen,pos,cx); if(x<cx+w/2) break; cx+=w; pos++;}
        move(pos,extend);
    }
}
static BOOL is_word(char c) {return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || (BYTE)c>=0xc0;}

/* --- editing ------------------------------------------------------------------------------------ */
/* A range that takes in any picture it touches, whole. */
static void whole_pictures(int *a,int *b) {
    int ps;
    if((ps=picture_start(*a))>=0) *a=ps;
    if(*b>*a && (ps=picture_start(*b-1))>=0) *b=max(*b,para_end(ps));
}
static void replace_selection(const char *s,int n) {
    int a=sel_min(),b=sel_max(),chp=current_chp(),pap;
    snapshot();
    whole_pictures(&a,&b);
    if(a!=b) {chp=cfmt[a]; if(pending>=0) chp=pending;}
    pap=a<length?pfmt[a]:(length?pfmt[length-1]:0);
    if(a<length && text[a]!='\n') pap=para_pap(para_start(a));
    if(paps[pap].picture) pap=text_pap(a);
    erase(a,b);
    insert(a,s,n,chp,pap);
    keep_picture_paragraph(a+n);
    anchor=caret=a+n; pending=-1;
    changed();
}
static void delete_char(int dir) {
    int a=sel_min(),b=sel_max();
    if(a==b) {if(dir<0) {if(!caret) return; a=step(caret,-1);} else {if(caret>=length) return; b=step(caret,1);}}
    whole_pictures(&a,&b);
    snapshot();
    /* Joining paragraphs: the joined one keeps the second's format (but a
     * picture's paragraph is not joined). */
    erase(a,b); keep_picture_paragraph(a); anchor=caret=a; pending=-1;
    changed();
}
/* A picture to the clipboard: a metafile as a picture (METAFILEPICT, its
 * extents in hundredths of a millimetre), a bitmap as a bitmap. */
static void put_picture(const Picture *pic) {
    const BYTE *b=pic->data; int w,h;
    picture_size(pic,&w,&h);
    if(get16(b)==PICTURE_BITMAP) {
        BITMAPINFO *info=picture_dib(pic); HDC screen_dc,mem; HBITMAP bm=NULL; HGDIOBJ old;
        if(!info) return;
        screen_dc=GetDC(NULL); mem=CreateCompatibleDC(screen_dc);
        bm=CreateCompatibleBitmap(screen_dc,(int)info->bmiHeader.biWidth,(int)-info->bmiHeader.biHeight);
        ReleaseDC(NULL,screen_dc);
        if(bm) {
            old=SelectObject(mem,bm);
            SetDIBitsToDevice(mem,0,0,(DWORD)info->bmiHeader.biWidth,(DWORD)-info->bmiHeader.biHeight,0,0,0,(UINT)-info->bmiHeader.biHeight,
                              (BYTE *)info+sizeof(BITMAPINFOHEADER),info,DIB_RGB_COLORS);
            SelectObject(mem,old);
            SetClipboardData(CF_BITMAP,bm);
        }
        DeleteDC(mem); GlobalFree(info);
    } else {
        DWORD size=min(get32(b+32),pic->size-PICTURE_HEADER); HMETAFILE mf=SetMetaFileBitsEx(size,b+PICTURE_HEADER);
        HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(METAFILEPICT)); METAFILEPICT *m;
        if(!mf || !g || !(m=(METAFILEPICT *)GlobalLock(g))) {if(mf) DeleteMetaFile(mf); if(g) GlobalFree(g); return;}
        m->mm=get16(b)&0x7f?get16(b)&0x7f:MM_ANISOTROPIC; m->hMF=mf;
        m->xExt=MulDiv(w,2540,TWIPS); m->yExt=MulDiv(h,2540,TWIPS);
        GlobalUnlock(g);
        SetClipboardData(CF_METAFILEPICT,g);
    }
}
static void copy_selection(void) {
    int a=sel_min(),b=sel_max(),ps=picture_start(a); HGLOBAL h; char *p;
    if(a==b || !OpenClipboard(main_wnd)) return;
    EmptyClipboard();
    /* A picture by itself goes as a picture; with text, pictures are left out. */
    if(ps==a && para_end(ps)>=b) put_picture(picture_of(&paps[pfmt[ps]]));
    else if((h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)(b-a+1)))!=NULL && (p=(char *)GlobalLock(h))!=NULL) {
        int i,n=0;
        for(i=a;i<b;i++) {
            if(picture_at(i)) {i=para_end(i)-1; continue;}
            p[n++]=text[i]==PAGE_CHAR?'#':text[i];
        }
        p[n]=0; GlobalUnlock(h);
        SetClipboardData(CF_TEXT,h);
    }
    CloseClipboard();
}
/* A picture's header: the kind, the extents, the size in twips, the data's size. */
static BYTE *new_picture(WORD kind,DWORD data,int w,int h) {
    BYTE *b=(BYTE *)GlobalAlloc(GPTR,PICTURE_HEADER+data);
    if(!b) return NULL;
    put16(b,kind); put16(b+10,(WORD)w); put16(b+12,(WORD)h);
    put16(b+30,PICTURE_HEADER); put32(b+32,data); put16(b+36,1000); put16(b+38,1000);
    return b;
}
/* A picture from a METAFILEPICT: its suggested size, or three inches wide in
 * its proportions (two inches high without them). */
static int picture_from_metafile(const METAFILEPICT *m) {
    UINT size=GetMetaFileBitsEx(m->hMF,0,NULL); int w=3*TWIPS,h=2*TWIPS; BYTE *b;
    if(!size) return 0;
    if(m->xExt>0 && m->yExt>0 && (m->mm==MM_ISOTROPIC || m->mm==MM_ANISOTROPIC)) {w=MulDiv((int)m->xExt,TWIPS,2540); h=MulDiv((int)m->yExt,TWIPS,2540);}
    else if(m->xExt<0 && m->yExt<0) h=MulDiv(w,(int)-m->yExt,(int)-m->xExt);
    if(!(b=new_picture((WORD)(0x80|(m->mm&0x7f)),size,max(1,min(w,22*TWIPS)),max(1,min(h,22*TWIPS))))) return 0;
    if(m->xExt>0 && m->yExt>0) {put16(b+2,(WORD)min(m->xExt,0x7fff)); put16(b+4,(WORD)min(m->yExt,0x7fff));}
    GetMetaFileBitsEx(m->hMF,size,b+PICTURE_HEADER);
    return add_picture(b,PICTURE_HEADER+size);
}
/* A picture from a bitmap: its rows, monochrome or 24 bits a pixel. */
static int picture_from_bitmap(HBITMAP bm) {
    BITMAP info; LONG bytes; BYTE *raw,*b; int row,x,y; BOOL mono;
    if(!GetObject(bm,sizeof(info),&info) || info.bmWidth<=0 || info.bmHeight<=0) return 0;
    mono=info.bmPlanes*info.bmBitsPixel==1;
    bytes=info.bmWidthBytes*info.bmHeight;
    if(!(raw=(BYTE *)GlobalAlloc(GMEM_FIXED,(DWORD)bytes))) return 0;
    GetBitmapBits(bm,bytes,raw);
    row=mono?(int)info.bmWidthBytes:(int)((info.bmWidth*3+1)/2*2);
    if(!(b=new_picture(PICTURE_BITMAP,(DWORD)row*info.bmHeight,(int)info.bmWidth*TWIPS/96,(int)info.bmHeight*TWIPS/96))) {GlobalFree(raw); return 0;}
    put16(b+18,(WORD)info.bmWidth); put16(b+20,(WORD)info.bmHeight); put16(b+22,(WORD)row); b[24]=1; b[25]=(BYTE)(mono?1:24);
    for(y=0;y<info.bmHeight;y++) {
        BYTE *out=b+PICTURE_HEADER+(DWORD)y*row; const BYTE *in=raw+(DWORD)y*info.bmWidthBytes;
        if(mono) memcpy(out,in,(size_t)row);
        else for(x=0;x<info.bmWidth;x++) {out[x*3]=in[x*4]; out[x*3+1]=in[x*4+1]; out[x*3+2]=in[x*4+2];}
    }
    GlobalFree(raw);
    return add_picture(b,PICTURE_HEADER+(DWORD)row*info.bmHeight);
}
/* A picture in place of the selection, in a paragraph of its own. */
static void insert_picture(int number) {
    Pap p; int a,b,n; char mark[3];
    memset(&p,0,sizeof(p)); p.line=240; p.picture=(WORD)number;
    if(!number || !paps[n=intern_pap(&p)].picture) {MessageBeep(0); return;}
    snapshot();
    a=sel_min(); b=sel_max(); whole_pictures(&a,&b);
    erase(a,b);
    if(a>0 && text[a-1]!='\n') {insert(a,"\r\n",2,cfmt[a-1],text_pap(a)); a+=2;}
    mark[0]=PICTURE_CHAR; mark[1]='\r'; mark[2]='\n';
    insert(a,mark,3,current_chp(),n);
    anchor=caret=a+3; pending=-1;
    changed();
}
static BOOL can_paste(void) {
    return IsClipboardFormatAvailable(CF_TEXT) || IsClipboardFormatAvailable(CF_METAFILEPICT) || IsClipboardFormatAvailable(CF_BITMAP);
}
/* Text, else a picture (a metafile, else a bitmap). */
static void paste(void) {
    HANDLE h; LPCSTR s; char *buffer; int n,i,k=0;
    if(!OpenClipboard(main_wnd)) return;
    if(!IsClipboardFormatAvailable(CF_TEXT)) {
        const METAFILEPICT *m;
        if((h=GetClipboardData(CF_METAFILEPICT))!=NULL && (m=(const METAFILEPICT *)GlobalLock(h))!=NULL) {
            n=picture_from_metafile(m); GlobalUnlock(h);
            insert_picture(n);
        } else if((h=GetClipboardData(CF_BITMAP))!=NULL) insert_picture(picture_from_bitmap((HBITMAP)h));
    } else if((h=GetClipboardData(CF_TEXT))!=NULL && (s=(LPCSTR)GlobalLock(h))!=NULL) {
        n=lstrlen(s);
        if((buffer=(char *)GlobalAlloc(GPTR,(DWORD)n*2+1))!=NULL) {
            for(i=0;i<n;i++) {
                if(s[i]=='\n' && (!i || s[i-1]!='\r')) buffer[k++]='\r';
                buffer[k++]=s[i];
            }
            replace_selection(buffer,k);
            GlobalFree(buffer);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
}
/* Character formats: on the selection, or for what is typed next. */
typedef void (*ChpChange)(Chp *,int);
static void format_chars(ChpChange change,int arg) {
    int a=sel_min(),b=sel_max(),i;
    if(a==b) {Chp c=chps[current_chp()]; change(&c,arg); pending=intern_chp(&c); return;}
    snapshot();
    for(i=a;i<b;i++) {Chp c=chps[cfmt[i]]; change(&c,arg); cfmt[i]=(BYTE)intern_chp(&c);}
    changed();
}
static void chp_regular(Chp *c,int arg) {(void)arg; c->flags&=CHP_PAGE; c->pos=0;}
static void chp_toggle(Chp *c,int flag) {c->flags^=(BYTE)flag;}
static void chp_pos(Chp *c,int pos) {c->pos=(signed char)(c->pos==pos?0:pos);}
static void chp_size(Chp *c,int delta) {int h=c->hps+delta; c->hps=(BYTE)max(8,min(254,h));}
static void chp_font(Chp *c,int packed) {c->font=(BYTE)(packed&0xff); if(packed>>8) c->hps=(BYTE)(packed>>8);}
/* Paragraph formats: on every paragraph the selection touches. */
typedef void (*PapChange)(Pap *,int);
static void format_paras(PapChange change,int arg) {
    int a=para_start(sel_min()),b=sel_max(),ps,i;
    snapshot();
    for(ps=a;;) {
        int pe=para_end(ps),n; Pap p=paps[para_pap(ps)];
        change(&p,arg); n=intern_pap(&p);
        for(i=ps;i<pe;i++) pfmt[i]=(BYTE)n;
        if(pe>=length || pe>b || (pe==b && b>a)) break;
        ps=pe;
    }
    changed();
}
static void pap_normal(Pap *p,int arg) {WORD picture=p->picture; (void)arg; memset(p,0,sizeof(*p)); p->line=240; p->picture=picture;}
static void pap_jc(Pap *p,int jc) {p->jc=(BYTE)jc;}
static void pap_line(Pap *p,int line) {p->line=(WORD)line;}
static Pap new_pap; static void pap_set(Pap *p,int arg) {(void)arg; p->left=new_pap.left; p->first=new_pap.first; p->right=new_pap.right;}
static void pap_tabs(Pap *p,int arg) {(void)arg; memcpy(p->tabs,new_pap.tabs,sizeof(p->tabs)); p->decimal=new_pap.decimal;}

/* --- measurements in inches: ".5", "1.25", "-0.5" ----------------------------------------------- */
static void inches(char *out,int twips) {
    int v=twips<0?-twips:twips,whole=v/TWIPS,hundredths=(v%TWIPS*100+TWIPS/2)/TWIPS;
    if(hundredths==100) {whole++; hundredths=0;}
    wsprintf(out,hundredths?"%s%d.%02d\"":"%s%d\"",(LPCSTR)(twips<0?"-":""),whole,hundredths);
    if(hundredths && out[lstrlen(out)-2]=='0') {int n=lstrlen(out); out[n-2]='"'; out[n-1]=0;}
}
static BOOL parse_inches(LPCSTR s,int *out) {
    int sign=1,whole=0,frac=0,scale=1,digits=0;
    while(*s==' ') s++;
    if(*s=='-') {sign=-1; s++;}
    for(;*s>='0' && *s<='9';s++,digits++) whole=whole*10+(*s-'0');
    if(*s=='.') for(s++;*s>='0' && *s<='9';s++,digits++) if(scale<1000) {frac=frac*10+(*s-'0'); scale*=10;}
    while(*s==' ' || *s=='"') s++;
    if(*s || !digits || whole>22) return FALSE;
    *out=sign*(whole*TWIPS+frac*TWIPS/scale); return TRUE;
}

/* --- files: Write's .WRI ------------------------------------------------------------------------ */
#define WRI_PAGE 128
/* CHP: reserved (0 or 1 in Write's own files), bold, italic and the font's low six
 * bits, the size in half points, underline and the special character, the font's
 * high bits, the position. */
static int chp_bytes(const Chp *c,BYTE *out) {
    static const BYTE defaults[6]={1,0,24,0,0,0}; int n=6;
    out[0]=1; out[1]=(BYTE)((c->flags&CHP_BOLD?1:0)|(c->flags&CHP_ITALIC?2:0)|(c->font&0x3f)<<2); out[2]=c->hps;
    out[3]=(BYTE)((c->flags&CHP_UNDERLINE?1:0)|(c->flags&CHP_PAGE?0x40:0)); out[4]=(BYTE)(c->font>>6&7); out[5]=(BYTE)c->pos;
    while(n && out[n-1]==defaults[n-1]) n--;
    return n;
}
static void chp_from(const BYTE *p,int n,Chp *c) {
    BYTE b[6]={1,0,24,0,0,0}; int ftc;
    memcpy(b,p,(size_t)min(n,6));
    memset(c,0,sizeof(*c));
    c->flags=(BYTE)((b[1]&1?CHP_BOLD:0)|(b[1]&2?CHP_ITALIC:0)|(b[3]&1?CHP_UNDERLINE:0)|(b[3]&0x40?CHP_PAGE:0));
    ftc=b[1]>>2|(b[4]&7)<<6; c->font=(BYTE)(ftc<font_count?ftc:0); c->hps=b[2]?b[2]:24; c->pos=(signed char)b[5];
}
/* PAP: reserved (60 or 61 in Write's own files), the alignment, reserved (30), the
 * right, left and first-line indents, the line spacing, reserved, the running-head
 * bits, reserved, the tab stops. */
#define PAP_SIZE (22+4*TABS)
static int pap_bytes(const Pap *p,BYTE rhc,BYTE *out) {
    BYTE defaults[PAP_SIZE]; int n=PAP_SIZE,i;
    memset(defaults,0,sizeof(defaults)); defaults[0]=61; defaults[2]=30; defaults[10]=240;
    memcpy(out,defaults,PAP_SIZE);
    out[1]=p->jc; put16(out+4,(WORD)p->right); put16(out+6,(WORD)p->left); put16(out+8,(WORD)p->first);
    put16(out+10,p->line?p->line:240); out[16]=rhc;
    for(i=0;i<TABS && p->tabs[i];i++) {put16(out+22+4*i,p->tabs[i]); out[24+4*i]=(BYTE)(p->decimal&1<<i?3:0);}
    while(n && out[n-1]==defaults[n-1]) n--;
    return n;
}
static BYTE pap_from(const BYTE *bytes,int n,Pap *p) {
    BYTE b[PAP_SIZE]; int i,k=0;
    memset(b,0,sizeof(b)); b[0]=61; b[2]=30; b[10]=240;
    memcpy(b,bytes,(size_t)min(n,PAP_SIZE));
    memset(p,0,sizeof(*p));
    p->jc=(BYTE)(b[1]&3); p->right=(short)get16(b+4); p->left=(short)get16(b+6); p->first=(short)get16(b+8);
    p->line=get16(b+10); if(!p->line) p->line=240;
    for(i=0;i<14 && k<TABS;i++) {
        WORD pos=get16(b+22+4*i);
        if(!pos) break;
        p->tabs[k]=pos; if((b[24+4*i]&3)==3) p->decimal|=(WORD)(1<<k);
        k++;
    }
    return b[16];
}
/* A buffer that grows a page at a time. */
typedef struct {BYTE *data; DWORD size,cap; BOOL failed;} Out;
static BYTE *out_page(Out *o) {
    if(o->size+WRI_PAGE>o->cap) {
        DWORD cap=max(o->cap*2,o->size+WRI_PAGE*8); BYTE *n=(BYTE *)GlobalAlloc(GPTR,cap);
        if(!n) {o->failed=TRUE; return NULL;}
        if(o->data) {memcpy(n,o->data,o->size); GlobalFree(o->data);}
        o->data=n; o->cap=cap;
    }
    o->size+=WRI_PAGE;
    memset(o->data+o->size-WRI_PAGE,0,WRI_PAGE);
    return o->data+o->size-WRI_PAGE;
}
/* Runs of one property each as formatted-disk pages: fcFirst, then FODs (fcLim and
 * the property's offset from byte 4) from the front and properties from the back,
 * the FOD count in the last byte. */
typedef struct {DWORD lim; BYTE prop[PAP_SIZE]; int n;} Run;
static void write_fkps(Out *o,const Run *runs,int count) {
    int r=0; DWORD fc=WRI_PAGE;
    while(r<count) {
        BYTE *page=out_page(o); int fods=0,back=127;
        if(!page) return;
        put32(page,fc);
        while(r<count) {
            int need=6,offset=-1,k;
            if(runs[r].n) {
                /* The same property already on this page is shared. */
                for(k=back;k<127;k++) if(page[k]==runs[r].n && k+1+runs[r].n<=127 && !memcmp(page+k+1,runs[r].prop,(size_t)runs[r].n)) {offset=k; break;}
                if(offset<0) need+=1+runs[r].n;
            }
            if(4+6*(fods+1)>back-(need-6)) break;
            if(runs[r].n && offset<0) {back-=1+runs[r].n; page[back]=(BYTE)runs[r].n; memcpy(page+back+1,runs[r].prop,(size_t)runs[r].n); offset=back;}
            put32(page+4+6*fods,runs[r].lim); put16(page+8+6*fods,(WORD)(runs[r].n?offset-4:0xffff));
            fods++; fc=runs[r].lim; r++;
        }
        page[127]=(BYTE)fods;
    }
}
/* A character run to lim (the one before goes on when the property is the same); the count of runs. */
static int chp_run(Run *runs,int count,DWORD lim,const Chp *c) {
    Run *r=&runs[count];
    r->lim=lim; r->n=chp_bytes(c,r->prop);
    if(count && r[-1].n==r->n && !memcmp(r[-1].prop,r->prop,(size_t)r->n)) {r[-1].lim=lim; return count;}
    return count+1;
}
static void running_text(const Running *h,char *out,BYTE *special,int *n) {
    int i,k=0;
    for(i=0;h->text[i] && k<120;i++) {special[k]=h->text[i]=='#'; out[k++]=h->text[i]=='#'?PAGE_CHAR:h->text[i];}
    out[k++]='\r'; out[k++]='\n'; special[k-2]=special[k-1]=0;
    *n=k;
}
/* The text as the file has it, a picture's paragraph as the picture's bytes;
 * each byte's character format with it. The length; nothing written to NULL. */
static DWORD file_text(BYTE *out,BYTE *formats) {
    DWORD n=0; int i;
    for(i=0;i<length;) {
        if(picture_at(i)) {
            const Picture *p=picture_of(&paps[pfmt[i]]);
            if(out) {memcpy(out+n,p->data,p->size); memset(formats+n,cfmt[i],p->size);}
            n+=p->size; i=para_end(i); continue;
        }
        if(out) {out[n]=(BYTE)text[i]; formats[n]=cfmt[i];}
        n++; i++;
    }
    return n;
}
static BOOL save_to(LPCSTR path) {
    Out o; Run *runs; int count=0,i,n_head=0,n_foot=0,body,pn_para,pn_sep,pn_setb,pn_ffntb; BYTE *page,*bytes,*formats;
    char head[128],foot[128]; BYTE head_special[128],foot_special[128]; DWORD fc_mac,file_length=file_text(NULL,NULL),at; BOOL ok;
    memset(&o,0,sizeof(o));
    if(header.text[0]) running_text(&header,head,head_special,&n_head);
    if(footer.text[0]) running_text(&footer,foot,foot_special,&n_foot);
    body=n_head+n_foot;
    bytes=(BYTE *)GlobalAlloc(GPTR,file_length+1); formats=(BYTE *)GlobalAlloc(GPTR,file_length+1);
    runs=(Run *)GlobalAlloc(GPTR,(file_length+(DWORD)body+4)*sizeof(Run));
    if(!bytes || !formats || !runs) {if(bytes) GlobalFree(bytes); if(formats) GlobalFree(formats); if(runs) GlobalFree(runs); return FALSE;}
    file_text(bytes,formats);
    /* The header: identity, tool, the text's end, then the pages of each part. */
    page=out_page(&o);
    if(!page) {GlobalFree(runs); GlobalFree(bytes); GlobalFree(formats); return FALSE;}
    put16(page,0xbe31); put16(page+4,0xab00);
    /* The text, running heads first, padded to a page. */
    while(o.size<WRI_PAGE+(DWORD)body+file_length) if(!out_page(&o)) break;
    if(o.failed) {GlobalFree(runs); GlobalFree(bytes); GlobalFree(formats); if(o.data) GlobalFree(o.data); return FALSE;}
    memcpy(o.data+WRI_PAGE,head,(size_t)n_head); memcpy(o.data+WRI_PAGE+n_head,foot,(size_t)n_foot);
    memcpy(o.data+WRI_PAGE+body,bytes,file_length);
    fc_mac=WRI_PAGE+(DWORD)body+file_length;
    /* Character runs. */
    for(i=0;i<n_head+n_foot;i++) {
        Chp c; memset(&c,0,sizeof(c)); c.hps=24;
        if((i<n_head?head_special[i]:foot_special[i-n_head])) c.flags=CHP_PAGE;
        count=chp_run(runs,count,WRI_PAGE+i+1,&c);
    }
    for(at=0;at<file_length;at++) count=chp_run(runs,count,fc_mac-file_length+at+1,&chps[formats[at]]);
    GlobalFree(bytes); GlobalFree(formats);
    if(!count) {runs[0].lim=fc_mac; runs[0].n=0; count=1;}
    write_fkps(&o,runs,count);
    /* Paragraph runs: the running heads (odd and even pages, 1 for the footer, 8 on the
     * first page too; their indents from the paper's edges). */
    pn_para=(int)(o.size/WRI_PAGE); count=0;
    for(i=0;i<2;i++) {
        const Running *h=i?&footer:&header; Pap p=h->pap;
        if(!(i?n_foot:n_head)) continue;
        p.left=(short)(p.left+m_left); p.right=(short)(p.right+m_right);
        runs[count].lim=WRI_PAGE+(i?body:n_head); runs[count].n=pap_bytes(&p,(BYTE)(6|i|(h->first?8:0)),runs[count].prop); count++;
    }
    /* A picture's paragraph is its bytes, its format marked (0x10) as a picture's. */
    for(i=0,at=0;i<length;) {
        int e=para_end(i); BOOL picture=picture_at(i);
        at+=picture?picture_of(&paps[pfmt[i]])->size:(DWORD)(e-i);
        runs[count].lim=fc_mac-file_length+at; runs[count].n=pap_bytes(&paps[para_pap(i)],(BYTE)(picture?0x10:0),runs[count].prop);
        count++; i=e;
    }
    if(!count || runs[count-1].lim<fc_mac) {Pap p; memset(&p,0,sizeof(p)); p.line=240; runs[count].lim=fc_mac; runs[count].n=pap_bytes(&p,0,runs[count].prop); count++;}
    write_fkps(&o,runs,count);
    GlobalFree(runs);
    /* The section: the page and its margins, the page numbers, the running heads' places. */
    pn_sep=(int)(o.size/WRI_PAGE);
    if((page=out_page(&o))!=NULL) {
        page[0]=22; page[2]=2; put16(page+3,(WORD)page_h); put16(page+5,(WORD)page_w); put16(page+7,(WORD)first_page);
        put16(page+9,(WORD)m_top); put16(page+11,(WORD)(page_h-m_top-m_bottom)); put16(page+13,(WORD)m_left);
        put16(page+15,(WORD)(page_w-m_left-m_right)); put16(page+17,256);
        put16(page+19,(WORD)header.distance); put16(page+21,(WORD)(page_h-footer.distance));
    }
    /* The section table: one section to the text's end. */
    pn_setb=(int)(o.size/WRI_PAGE);
    if((page=out_page(&o))!=NULL) {
        put16(page,2); put16(page+2,2);
        put32(page+4,fc_mac-WRI_PAGE); put16(page+8,1); put32(page+10,(DWORD)pn_sep*WRI_PAGE);
        put32(page+14,fc_mac-WRI_PAGE+1); put16(page+18,0x7fff); put32(page+20,0xffffffff);
    }
    /* The font table: each font's family and name. */
    pn_ffntb=(int)(o.size/WRI_PAGE);
    if((page=out_page(&o))!=NULL) {
        int at=2;
        put16(page,(WORD)font_count);
        for(i=0;i<font_count;i++) {
            int len=lstrlen(fonts[i])+1;
            if(at+2+1+len+2>WRI_PAGE) {put16(page+at,0xffff); if(!(page=out_page(&o))) break; at=0;}
            put16(page+at,(WORD)(1+len)); page[at+2]=font_families[i]; memcpy(page+at+3,fonts[i],(size_t)len); at+=3+len;
        }
        if(page) put16(page+at,0);
    }
    if(o.failed) {if(o.data) GlobalFree(o.data); return FALSE;}
    put32(o.data+14,fc_mac); put16(o.data+18,(WORD)pn_para); put16(o.data+20,(WORD)pn_sep); put16(o.data+22,(WORD)pn_sep);
    put16(o.data+24,(WORD)pn_setb); put16(o.data+26,(WORD)pn_ffntb); put16(o.data+28,(WORD)pn_ffntb);
    put16(o.data+96,(WORD)(o.size/WRI_PAGE));
    ok=WriteWholeFile(path,o.data,o.size);
    GlobalFree(o.data);
    return ok;
}
/* Reads the runs of formatted-disk pages from page pn, calling found for each. */
typedef void (*RunFound)(DWORD first,DWORD lim,const BYTE *prop,int n);
static void read_fkps(const BYTE *data,DWORD size,DWORD pn,DWORD fc_mac,RunFound found) {
    DWORD fc=WRI_PAGE;
    for(;(pn+1)*WRI_PAGE<=size;pn++) {
        const BYTE *page=data+pn*WRI_PAGE; int fods=page[127],i;
        if(fods>20) fods=20;
        for(i=0;i<fods;i++) {
            DWORD lim=get32(page+4+6*i); WORD bf=get16(page+8+6*i); int n=0; const BYTE *prop=NULL;
            if(bf<123) {n=page[4+bf]; prop=page+5+bf; if(bf+4+1+n>127) n=0;}
            found(fc,min(lim,fc_mac),prop,n);
            if(lim>=fc_mac) return;
            fc=lim;
        }
        if(!fods) return;
    }
}
/* While loading: each character's running-head bits, from its paragraph. */
static BYTE *rhcs;
static void chp_found(DWORD first,DWORD lim,const BYTE *prop,int n) {
    Chp c; int idx; DWORD i;
    chp_from(prop,n,&c); idx=intern_chp(&c);
    for(i=first;i<lim;i++) if(i-WRI_PAGE<(DWORD)length) cfmt[i-WRI_PAGE]=(BYTE)idx;
}
static void pap_found(DWORD first,DWORD lim,const BYTE *prop,int n) {
    Pap p; BYTE rhc=pap_from(prop,n,&p); int idx=intern_pap(&p); DWORD i;
    for(i=first;i<lim;i++) if(i-WRI_PAGE<(DWORD)length) {pfmt[i-WRI_PAGE]=(BYTE)idx; rhcs[i-WRI_PAGE]=rhc;}
}
/* A running head's paragraph from the text into the header or footer, after
 * what its earlier paragraphs had (a space between); # for the page number. */
static void take_running(int start,int end) {
    Running *h=rhcs[start]&1?&footer:&header; int i,k=lstrlen(h->text),was=k;
    for(i=start;i<end && k<126;i++) {
        if(text[i]=='\r' || text[i]=='\n') continue;
        if(k==was && was) h->text[k++]=' ';
        h->text[k++]=text[i]==PAGE_CHAR && (chps[cfmt[i]].flags&CHP_PAGE)?'#':text[i];
    }
    h->text[k]=0;
    if(!was) {
        /* Write measures a running head's indents from the paper's edges. */
        h->first=(rhcs[start]&8)!=0; h->pap=paps[pfmt[end-1]];
        h->pap.left=(short)max(0,h->pap.left-m_left); h->pap.right=(short)max(0,h->pap.right-m_right);
    }
}
/* A picture's paragraph (its paragraph format says so) is a 40-byte header,
 * its sizes at 30 and 32, then the bitmap or metafile: the picture goes into
 * the table, and the text keeps PICTURE_CHAR and a paragraph mark for it. */
static void take_pictures(void) {
    int i=0,k=0;
    while(i<length) {
        if(rhcs[i]&0x10) {
            int e=length,n=0; BYTE *data; Pap p=paps[pfmt[i]];
            if(length-i>=PICTURE_HEADER) e=(int)min((DWORD)(length-i),(DWORD)get16((BYTE *)text+i+30)+get32((BYTE *)text+i+32))+i;
            if(e-i>=PICTURE_HEADER && (data=(BYTE *)GlobalAlloc(GMEM_FIXED,(DWORD)(e-i)))!=NULL) {
                memcpy(data,text+i,(size_t)(e-i));
                if((p.picture=(WORD)add_picture(data,(DWORD)(e-i)))!=0) n=intern_pap(&p);
            }
            if(paps[n].picture) {
                text[k]=PICTURE_CHAR; text[k+1]='\r'; text[k+2]='\n';
                cfmt[k+2]=cfmt[k+1]=cfmt[k]=cfmt[i]; pfmt[k+2]=pfmt[k+1]=pfmt[k]=(BYTE)n; rhcs[k+2]=rhcs[k+1]=rhcs[k]=0;
                k+=3;
            }
            i=e; continue;
        }
        text[k]=text[i]; cfmt[k]=cfmt[i]; pfmt[k]=pfmt[i]; rhcs[k]=rhcs[i]; k++; i++;
    }
    length=k;
}
static void reset_running(void) {
    Running *h[2]; int i;
    h[0]=&header; h[1]=&footer;
    for(i=0;i<2;i++) {h[i]->text[0]=0; h[i]->distance=1080; h[i]->first=FALSE; memset(&h[i]->pap,0,sizeof(Pap)); h[i]->pap.jc=1;}
}
static BOOL load(LPCSTR path) {
    DWORD size,fc_mac,pn_para,pn_sep,pn_ffntb,pn_mac,i; BYTE *data=(BYTE *)ReadWholeFile(path,&size); int body;
    if(!data) return FALSE;
    if(size<2*WRI_PAGE || (get16(data)!=0xbe31 && get16(data)!=0xbe32) || get16(data+4)!=0xab00) {
        /* Not Write's: the file as text. */
        reset_formats(); length=0;
        if(reserve((int)size)) for(i=0;i<size;i++) if(data[i]) insert(length,(const char *)data+i,1,0,0);
        GlobalFree(data);
        return TRUE;
    }
    fc_mac=get32(data+14); pn_para=get16(data+18); pn_sep=get16(data+22); pn_ffntb=get16(data+28); pn_mac=get16(data+96);
    if(fc_mac<WRI_PAGE || fc_mac>size) {GlobalFree(data); return FALSE;}
    reset_formats(); reset_running(); font_count=0;
    /* The font table: each entry's size, family and name; 0xFFFF goes on to the next page. */
    if(pn_ffntb && pn_ffntb<pn_mac && (pn_ffntb+1)*WRI_PAGE<=size) {
        DWORD pn=pn_ffntb,at=2;
        for(;;) {
            const BYTE *page=data+pn*WRI_PAGE; WORD cb;
            if(at+2>WRI_PAGE) break;
            cb=get16(page+at);
            if(!cb) break;
            if(cb==0xffff) {pn++; at=0; if((pn+1)*WRI_PAGE>size) break; continue;}
            if(at+2+cb>WRI_PAGE || cb<2) break;
            if(font_count<MAX_FONTS) {
                lstrcpyn(fonts[font_count],(LPCSTR)page+at+3,min((int)cb,LF_FACESIZE));
                font_families[font_count]=page[at+2]; font_count++;
            }
            at+=2+cb;
        }
    }
    if(!font_count) font_index("Helv",FF_SWISS);
    /* The section: the page, the margins, the page numbers, the running heads' places. */
    if(pn_sep && (pn_sep+1)*WRI_PAGE<=size && data[pn_sep*WRI_PAGE]>=22) {
        const BYTE *p=data+pn_sep*WRI_PAGE;
        page_h=get16(p+3); page_w=get16(p+5); first_page=(short)get16(p+7)>0?get16(p+7):1;
        m_top=get16(p+9); m_bottom=page_h-m_top-get16(p+11); m_left=get16(p+13); m_right=page_w-m_left-get16(p+15);
        header.distance=get16(p+19); footer.distance=page_h-get16(p+21);
        if(page_w<2880 || page_h<2880 || m_left<0 || m_right<0 || m_top<0 || m_bottom<0) {
            page_w=12240; page_h=15840; m_left=m_right=1800; m_top=m_bottom=1440;
        }
    }
    /* The text and its formats; running heads come off its front. */
    length=0;
    if(!reserve((int)(fc_mac-WRI_PAGE)+1) || !(rhcs=(BYTE *)GlobalAlloc(GPTR,fc_mac-WRI_PAGE+1))) {GlobalFree(data); return FALSE;}
    memcpy(text,data+WRI_PAGE,fc_mac-WRI_PAGE); length=(int)(fc_mac-WRI_PAGE);
    memset(cfmt,0,(size_t)length); memset(pfmt,0,(size_t)length);
    read_fkps(data,size,(fc_mac+WRI_PAGE-1)/WRI_PAGE,fc_mac,chp_found);
    read_fkps(data,size,pn_para,fc_mac,pap_found);
    take_pictures();
    for(body=0;body<length && (rhcs[body]&6);) {int e=para_end(body); take_running(body,e); body=e;}
    erase(0,body);
    GlobalFree(rhcs); rhcs=NULL;
    GlobalFree(data);
    return TRUE;
}

/* --- pagination and printing -------------------------------------------------------------------- */
/* The document's lines on a device, each with the page it falls on: a page
 * ends where the next line would pass the text's height, or at a page break. */
typedef struct {Line *lines; int *page_of; int count,cap,page,page_top,page_height;} Pages;
static void page_line(const Line *l,void *data) {
    Pages *p=(Pages *)data;
    if(p->count==p->cap) {
        int cap=p->cap?p->cap*2:256; Line *n=(Line *)GlobalAlloc(GPTR,(DWORD)cap*sizeof(Line)); int *g=(int *)GlobalAlloc(GPTR,(DWORD)cap*sizeof(int));
        if(!n || !g) {if(n) GlobalFree(n); if(g) GlobalFree(g); return;}
        if(p->lines) {memcpy(n,p->lines,(size_t)p->count*sizeof(Line)); memcpy(g,p->page_of,(size_t)p->count*sizeof(int)); GlobalFree(p->lines); GlobalFree(p->page_of);}
        p->lines=n; p->page_of=g; p->cap=cap;
    }
    if(p->count && l->y+l->height-p->page_top>p->page_height && l->y>p->page_top) {p->page++; p->page_top=l->y;}
    p->lines[p->count]=*l; p->page_of[p->count]=p->page; p->count++;
    if(l->start<length && text[l->start]==PAGE_BREAK) {p->page++; p->page_top=l->y+l->height;}
}
static int paginate(Device *d,Pages *p) {
    memset(p,0,sizeof(*p)); p->page_height=px_y(d,page_h-m_top-m_bottom);
    layout(d,0,length,px_x(d,page_w-m_left-m_right),0,page_line,p);
    return p->count?p->page+1:1;
}
static void free_pages(Pages *p) {if(p->lines) GlobalFree(p->lines); if(p->page_of) GlobalFree(p->page_of); memset(p,0,sizeof(*p));}
/* Where each page starts in the text, for Go To Page and the page marks. */
static void keep_page_starts(const Pages *p,int pages) {
    int i,k=0;
    if(page_starts) GlobalFree(page_starts);
    page_starts=(int *)GlobalAlloc(GPTR,(DWORD)(pages+1)*sizeof(int)); page_count=0;
    if(!page_starts) return;
    page_starts[0]=0; k=1;
    for(i=1;i<p->count && k<pages;i++) if(p->page_of[i]!=p->page_of[i-1]) page_starts[k++]=p->lines[i].start;
    page_count=k;
}
static BOOL open_printer(HDC dc) {
    memset(&printer,0,sizeof(printer));
    printer.dc=dc; printer.dpi_x=GetDeviceCaps(dc,LOGPIXELSX); printer.dpi_y=GetDeviceCaps(dc,LOGPIXELSY); printer.printing=TRUE;
    return printer.dpi_x>0 && printer.dpi_y>0;
}
static void repaginate(void) {
    HDC dc=PrinterDC(main_wnd); Pages p; int pages;
    if(dc && open_printer(dc)) {pages=paginate(&printer,&p); device_reset(&printer);}
    else pages=paginate(&screen,&p);
    if(dc) DeleteDC(dc);
    keep_page_starts(&p,pages); free_pages(&p);
    InvalidateRect(main_wnd,NULL,TRUE);
}
static void print_line(HDC dc,Device *d,const Line *l,int x0,int y0) {
    int x=l->x,i;
    if(picture_start(l->start)==l->start) {
        const Picture *pic=picture_of(&paps[pfmt[l->start]]); int w,h;
        picture_size(pic,&w,&h);
        draw_picture(dc,pic,x0+l->x,y0,px_x(d,w),l->height);
        return;
    }
    for(i=l->start;i<l->end;i++) {int w=char_width(d,i,x); draw_char(dc,d,l,i,x0+x,y0); x+=w;}
}
static int span_width(Device *d,LPCSTR s,int n) {int w=0; while(n-->0) w+=d->widths[0][(BYTE)*s++]; return w;}
/* A running head on one line: its text, # as the page number, from the indent
 * and aligned in the column; text after a tab goes to the next stop (lined up on
 * its decimal point, or its end, at a decimal stop). */
static void print_running(Device *d,HDC dc,const Running *h,int y,int x0,int width) {
    char line[160]; int i,k=0,x,left=px_x(d,h->pap.left),right=px_x(d,h->pap.right); HFONT old;
    for(i=0;h->text[i] && k<150;i++) {
        if(h->text[i]=='#') {char page[16]; page_text(d,page); lstrcpy(line+k,page); k+=lstrlen(page);}
        else line[k++]=h->text[i];
    }
    line[k]=0;
    device_font(d,0); old=(HFONT)SelectObject(dc,d->fonts[0]);
    x=max(0,left+px_x(d,h->pap.first));
    for(i=0;i<=k;) {
        int e=i,w,t,j; BOOL dec=FALSE;
        while(e<k && line[e]!='\t') e++;
        w=span_width(d,line+i,e-i);
        if(i) {
            for(j=0;j<TABS && h->pap.tabs[j];j++) if(px_x(d,h->pap.tabs[j])>x) {dec=(h->pap.decimal>>j)&1; break;}
            t=next_tab(d,&h->pap,x);
            if(dec) {int dot=0; while(i+dot<e && line[i+dot]!='.') dot++; t-=span_width(d,line+i,dot);}
            x=max(x,t);
        } else if(e==k && h->pap.jc==1) x=max(x,left+(width-left-right-w)/2);
        else if(e==k && h->pap.jc==2) x=max(x,width-right-w);
        TextOut(dc,x0+x,y,line+i,e-i);
        x+=w; i=e+1;
    }
    SelectObject(dc,old);
}
static void print_document(HWND h) {
    int from,to,copies,pages,page,c,i,x0,column; Pages p; POINT offset; HDC dc; char name[64]; BOOL ok=TRUE;
    lstrcpy(name,file[0]?FileTitle(file):"(Untitled)");
    if(!(dc=PrintDialogDC(h,first_page,first_page+999,&from,&to,&copies))) return;
    if(!open_printer(dc)) {DeleteDC(dc); return;}
    pages=paginate(&printer,&p);
    if(!PrintStart(dc,h,"Write",name)) {device_reset(&printer); free_pages(&p); return;}
    if(Escape(dc,GETPRINTINGOFFSET,0,NULL,&offset)<=0) offset.x=offset.y=0;
    x0=px_x(&printer,m_left)-offset.x; column=px_x(&printer,page_w-m_left-m_right);
    for(c=0;c<copies && ok;c++) for(page=0,i=0;page<pages && ok;page++) {
        int number=page+first_page,top;
        while(i<p.count && p.page_of[i]<page) i++;
        if(number<from || number>to) continue;
        printer.page_number=number;
        if(StartPage(dc)<=0 || PrintCancelled()) {ok=FALSE; break;}
        if(header.text[0] && (page || header.first)) print_running(&printer,dc,&header,px_y(&printer,header.distance)-offset.y,x0,column);
        if(footer.text[0] && (page || footer.first)) print_running(&printer,dc,&footer,px_y(&printer,page_h-footer.distance)-offset.y,x0,column);
        top=i<p.count?p.lines[i].y:0;
        for(;i<p.count && p.page_of[i]==page;i++) print_line(dc,&printer,&p.lines[i],x0,px_y(&printer,m_top)-offset.y+p.lines[i].y-top);
        if(EndPage(dc)<=0 || PrintCancelled()) ok=FALSE;
    }
    keep_page_starts(&p,pages);
    PrintEnd(dc,ok && !PrintCancelled());
    device_reset(&printer); free_pages(&p);
    InvalidateRect(main_wnd,NULL,TRUE);
}

/* --- finding and changing ----------------------------------------------------------------------- */
static char find_what[64],change_to[64];
static BOOL whole_word,match_case;
static char fold(char c) {return match_case?c:(char)(c>='a' && c<='z'?c-0x20:c);}
/* The next match from pos (wrapping to the start once); -1 if none. */
static int find_from(int pos) {
    int n=lstrlen(find_what),i,k,pass;
    if(!n) return -1;
    for(pass=0;pass<2;pass++) {
        for(i=pass?0:pos;i+n<=length && (pass==0 || i<pos);i++) {
            for(k=0;k<n && fold(text[i+k])==fold(find_what[k]);k++) {}
            if(k<n) continue;
            if(whole_word && ((i>0 && is_word(text[i-1])) || (i+n<length && is_word(text[i+n])))) continue;
            return i;
        }
    }
    return -1;
}
static BOOL find_next(void) {
    int at=find_from(sel_max());
    if(at<0) {MessageBox(main_wnd,"Search text not found.","Write",MB_OK|MB_ICONASTERISK); return FALSE;}
    anchor=at; caret=at+lstrlen(find_what); pending=-1;
    InvalidateRect(main_wnd,NULL,TRUE); show_caret();
    return TRUE;
}
static BOOL selection_matches(void) {
    int n=lstrlen(find_what),k,a=sel_min();
    if(sel_max()-a!=n || !n) return FALSE;
    for(k=0;k<n && fold(text[a+k])==fold(find_what[k]);k++) {}
    return k==n;
}
static void read_find(HWND dlg) {
    GetDlgItemText(dlg,IDC_WHAT,find_what,sizeof(find_what));
    if(GetDlgItem(dlg,IDC_TO)) GetDlgItemText(dlg,IDC_TO,change_to,sizeof(change_to));
    whole_word=IsDlgButtonChecked(dlg,IDC_WHOLE)!=0; match_case=IsDlgButtonChecked(dlg,IDC_CASE)!=0;
}
static INT_PTR CALLBACK FindProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_WHAT,find_what);
        if(GetDlgItem(dlg,IDC_TO)) SetDlgItemText(dlg,IDC_TO,change_to);
        CheckDlgButton(dlg,IDC_WHOLE,whole_word); CheckDlgButton(dlg,IDC_CASE,match_case);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDOK: read_find(dlg); if(!GetDlgItem(dlg,IDC_TO)) {EndDialog(dlg,IDOK); return TRUE;} find_next(); return TRUE;
        case IDC_CHANGE:
            read_find(dlg);
            if(selection_matches()) replace_selection(change_to,lstrlen(change_to));
            find_next(); return TRUE;
        case IDC_CHANGEALL: {
            int at,n=0,pos=0;
            read_find(dlg);
            if(!find_what[0]) return TRUE;
            snapshot();
            while((at=find_from(pos))>=0 && at>=pos) {
                int chp=cfmt[at],pap=pfmt[at];
                erase(at,at+lstrlen(find_what)); insert(at,change_to,lstrlen(change_to),chp,pap);
                pos=at+lstrlen(change_to); n++;
            }
            anchor=caret=min(caret,length);
            if(n) changed();
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK GoToProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL ok; UINT n;
    (void)lp;
    if(msg==WM_INITDIALOG) return TRUE;
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        n=GetDlgItemInt(dlg,IDC_PAGE,&ok,FALSE);
        if(!ok || (int)n<first_page || (int)n>=first_page+page_count) {MessageBox(dlg,"There is no such page.","Write",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
        EndDialog(dlg,(INT_PTR)n); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,0); return TRUE;}
    return FALSE;
}

/* --- the format dialogs ------------------------------------------------------------------------- */
static int font_sizes[32],font_size_count;
static int CALLBACK face_found(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    HWND list=(HWND)lp; (void)tm; (void)type;
    if(SendMessage(list,LB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)lf->lfFaceName)<0) SendMessage(list,LB_ADDSTRING,0,(LPARAM)lf->lfFaceName);
    return 1;
}
static int CALLBACK size_found(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    int points=MulDiv(tm->tmHeight-tm->tmInternalLeading,72,screen.dpi_y),i;
    (void)lf; (void)type; (void)lp;
    for(i=0;i<font_size_count;i++) if(font_sizes[i]==points) return 1;
    if(font_size_count<32 && points>0) font_sizes[font_size_count++]=points;
    return 1;
}
/* The sizes the face has on the screen, smallest first. */
static void fill_sizes(HWND dlg,LPCSTR face,int current) {
    HDC dc=GetDC(main_wnd); int i,j,at=0; char text_size[8];
    font_size_count=0; EnumFonts(dc,face,size_found,0); ReleaseDC(main_wnd,dc);
    for(i=0;i<font_size_count;i++) for(j=i+1;j<font_size_count;j++) if(font_sizes[j]<font_sizes[i]) {int t=font_sizes[i]; font_sizes[i]=font_sizes[j]; font_sizes[j]=t;}
    SendDlgItemMessage(dlg,IDC_POINTS,LB_RESETCONTENT,0,0);
    for(i=0;i<font_size_count;i++) {
        wsprintf(text_size,"%d",font_sizes[i]); SendDlgItemMessage(dlg,IDC_POINTS,LB_ADDSTRING,0,(LPARAM)text_size);
        if(font_sizes[i]<=current) at=i;
    }
    SendDlgItemMessage(dlg,IDC_POINTS,LB_SETCURSEL,(WPARAM)at,0);
}
static INT_PTR CALLBACK FontsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    const Chp *c=&chps[current_chp()]; char face[LF_FACESIZE]; LRESULT i;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        HDC dc=GetDC(main_wnd);
        EnumFonts(dc,NULL,face_found,(LPARAM)GetDlgItem(dlg,IDC_FACE)); ReleaseDC(main_wnd,dc);
        i=SendDlgItemMessage(dlg,IDC_FACE,LB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)fonts[c->font]);
        SendDlgItemMessage(dlg,IDC_FACE,LB_SETCURSEL,(WPARAM)(i<0?0:i),0);
        SendDlgItemMessage(dlg,IDC_FACE,LB_GETTEXT,(WPARAM)(i<0?0:i),(LPARAM)face);
        fill_sizes(dlg,face,c->hps/2);
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_FACE && HIWORD(wp)==LBN_SELCHANGE) {
            i=SendDlgItemMessage(dlg,IDC_FACE,LB_GETCURSEL,0,0);
            if(i>=0) {SendDlgItemMessage(dlg,IDC_FACE,LB_GETTEXT,(WPARAM)i,(LPARAM)face); fill_sizes(dlg,face,c->hps/2);}
            return TRUE;
        }
        if(LOWORD(wp)==IDOK || ((LOWORD(wp)==IDC_FACE || LOWORD(wp)==IDC_POINTS) && HIWORD(wp)==LBN_DBLCLK)) {
            int size;
            i=SendDlgItemMessage(dlg,IDC_FACE,LB_GETCURSEL,0,0);
            if(i<0) return TRUE;
            SendDlgItemMessage(dlg,IDC_FACE,LB_GETTEXT,(WPARAM)i,(LPARAM)face);
            i=SendDlgItemMessage(dlg,IDC_POINTS,LB_GETCURSEL,0,0);
            size=i>=0 && i<font_size_count?font_sizes[i]:c->hps/2;
            EndDialog(dlg,(INT_PTR)(font_index(face,FF_DONTCARE)|(size*2)<<8));
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,-1); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static BOOL read_inches(HWND dlg,int id,int *out) {
    char s[16];
    GetDlgItemText(dlg,id,s,sizeof(s));
    if(parse_inches(s,out)) return TRUE;
    MessageBox(dlg,"Enter a measurement in inches.","Write",MB_OK|MB_ICONEXCLAMATION);
    SetFocus(GetDlgItem(dlg,id));
    return FALSE;
}
static void show_inches(HWND dlg,int id,int twips) {char s[16]; inches(s,twips); SetDlgItemText(dlg,id,s);}
static INT_PTR CALLBACK IndentsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    const Pap *p=&paps[para_pap(para_start(sel_min()))]; int l,f,r;
    (void)lp;
    if(msg==WM_INITDIALOG) {show_inches(dlg,IDC_LEFTIND,p->left); show_inches(dlg,IDC_FIRSTIND,p->first); show_inches(dlg,IDC_RIGHTIND,p->right); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        if(!read_inches(dlg,IDC_LEFTIND,&l) || !read_inches(dlg,IDC_FIRSTIND,&f) || !read_inches(dlg,IDC_RIGHTIND,&r)) return TRUE;
        new_pap.left=(short)l; new_pap.first=(short)f; new_pap.right=(short)r;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK TabsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    const Pap *p=&paps[para_pap(para_start(sel_min()))]; int i,k=0,t;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        for(i=0;i<TABS && p->tabs[i];i++) {show_inches(dlg,IDC_TAB+i,p->tabs[i]); CheckDlgButton(dlg,IDC_DECIMAL+i,(p->decimal>>i)&1);}
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        memset(new_pap.tabs,0,sizeof(new_pap.tabs)); new_pap.decimal=0;
        for(i=0;i<TABS;i++) {
            char s[16]; GetDlgItemText(dlg,IDC_TAB+i,s,sizeof(s));
            if(!s[0]) continue;
            if(!parse_inches(s,&t) || t<=0) {MessageBox(dlg,"Enter tab positions in inches.","Write",MB_OK|MB_ICONEXCLAMATION); SetFocus(GetDlgItem(dlg,IDC_TAB+i)); return TRUE;}
            new_pap.tabs[k]=(WORD)t; if(IsDlgButtonChecked(dlg,IDC_DECIMAL+i)) new_pap.decimal|=(WORD)(1<<k);
            k++;
        }
        /* In order. */
        for(i=0;i<k;i++) {int j; for(j=i+1;j<k;j++) if(new_pap.tabs[j]<new_pap.tabs[i]) {WORD x=new_pap.tabs[i]; new_pap.tabs[i]=new_pap.tabs[j]; new_pap.tabs[j]=x;}}
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK PageLayoutProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int l,r,t,b; BOOL ok; UINT start;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        SetDlgItemInt(dlg,IDC_STARTPAGE,(UINT)first_page,FALSE);
        show_inches(dlg,IDC_MLEFT,m_left); show_inches(dlg,IDC_MRIGHT,m_right); show_inches(dlg,IDC_MTOP,m_top); show_inches(dlg,IDC_MBOTTOM,m_bottom);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        start=GetDlgItemInt(dlg,IDC_STARTPAGE,&ok,FALSE);
        if(!ok || !start || start>32767) {MessageBox(dlg,"Enter a page number.","Write",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
        if(!read_inches(dlg,IDC_MLEFT,&l) || !read_inches(dlg,IDC_MRIGHT,&r) || !read_inches(dlg,IDC_MTOP,&t) || !read_inches(dlg,IDC_MBOTTOM,&b)) return TRUE;
        if(l<0 || r<0 || t<0 || b<0 || l+r>page_w-TWIPS || t+b>page_h-TWIPS) {MessageBox(dlg,"The margins leave no room for the text.","Write",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
        first_page=(int)start; m_left=l; m_right=r; m_top=t; m_bottom=b;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static Running *editing_running;
static INT_PTR CALLBACK RunningProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    Running *h=editing_running; int d;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        if(h==&footer) SetWindowText(dlg,"Footer");
        SetDlgItemText(dlg,IDC_HEADTEXT,h->text); show_inches(dlg,IDC_DISTANCE,h->distance); CheckDlgButton(dlg,IDC_FIRSTPAGE,h->first);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        if(!read_inches(dlg,IDC_DISTANCE,&d)) return TRUE;
        GetDlgItemText(dlg,IDC_HEADTEXT,h->text,sizeof(h->text)); h->distance=d; h->first=IsDlgButtonChecked(dlg,IDC_FIRSTPAGE)!=0;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}

/* --- files -------------------------------------------------------------------------------------- */
static void set_title(void) {
    char title[300];
    wsprintf(title,"Write - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(main_wnd,title);
}
static void new_document(void) {
    reset_formats(); length=0; anchor=caret=0; pending=-1; forget_undo();
    page_w=12240; page_h=15840; m_left=m_right=1800; m_top=m_bottom=1440; first_page=1;
    reset_running();
    if(page_starts) {GlobalFree(page_starts); page_starts=NULL;}
    page_count=0;
    device_reset(&screen);
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.WRI");
    if(!FileSaveDialog(main_wnd,"Save As","*.WRI",path,sizeof(path))) return FALSE;
    if(!save_to(path)) {MessageBox(main_wnd,"Cannot write the file.","Write",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    lstrcpy(file,path); AnsiUpper(file); modified=FALSE; set_title();
    return TRUE;
}
static BOOL save(void) {
    if(!file[0]) return save_as();
    if(!save_to(file)) {MessageBox(main_wnd,"Cannot write the file.","Write",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    modified=FALSE; return TRUE;
}
static BOOL query_save(void) {
    char t[320];
    if(!modified) return TRUE;
    wsprintf(t,"Save current changes to %s?",file[0]?FileTitle(file):"(Untitled)");
    switch(MessageBox(main_wnd,t,"Write",MB_YESNOCANCEL|MB_ICONQUESTION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}
static void open_path(LPCSTR path) {
    new_document();
    if(!load(path)) {MessageBox(main_wnd,"Cannot read the file.","Write",MB_OK|MB_ICONEXCLAMATION); new_document();}
    else {lstrcpy(file,path); AnsiUpper(file);}
    device_reset(&screen); modified=FALSE; set_title(); scroll_x=scroll_y=0; relayout(); show_caret();
}

/* --- the main window ---------------------------------------------------------------------------- */
static void command(HWND h,UINT id) {
    char path[260]; INT_PTR r;
    if(id!=IDM_MOVEPICTURE && id!=IDM_SIZEPICTURE) end_adjust(FALSE);
    if(HelpCommand(h,id,"WRITE.HLP")) return;
    switch(id) {
    case IDM_NEW: if(!query_save()) return; new_document(); file[0]=0; modified=FALSE; set_title(); relayout(); show_caret(); return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.WRI");
        if(FileOpenDialog(h,"Open","*.WRI",path,sizeof(path))) open_path(path);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_PRINT: print_document(h); return;
    case IDM_PRINTSETUP: PrinterSetup(h); return;
    case IDM_REPAGINATE: repaginate(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_UNDO:
        if(undo.text) {
            char *t=text; BYTE *c=cfmt,*p=pfmt; int n=length,a=anchor,k=caret;
            text=undo.text; cfmt=undo.cfmt; pfmt=undo.pfmt; length=undo.length; anchor=undo.anchor; caret=undo.caret; capacity=undo.length+1;
            undo.text=t; undo.cfmt=c; undo.pfmt=p; undo.length=n; undo.anchor=a; undo.caret=k;
            changed();
        }
        return;
    case IDM_CUT: copy_selection(); if(anchor!=caret) delete_char(1); return;
    case IDM_COPY: copy_selection(); return;
    case IDM_PASTE: paste(); return;
    case IDM_MOVEPICTURE: case IDM_SIZEPICTURE: start_adjust(id); return;
    case IDM_FIND: if(DialogBox(instance,"FIND",h,FindProc)==IDOK) find_next(); return;
    case IDM_FINDNEXT: if(find_what[0]) find_next(); else command(h,IDM_FIND); return;
    case IDM_CHANGE: DialogBox(instance,"CHANGE",h,FindProc); return;
    case IDM_GOTOPAGE:
        if(!page_count) repaginate();
        r=DialogBox(instance,"GOTOPAGE",h,GoToProc);
        if(r>0 && r-first_page<page_count) move(page_starts[r-first_page],FALSE);
        return;
    case IDM_REGULAR: format_chars(chp_regular,0); return;
    case IDM_BOLD: format_chars(chp_toggle,CHP_BOLD); return;
    case IDM_ITALIC: format_chars(chp_toggle,CHP_ITALIC); return;
    case IDM_UNDERLINE: format_chars(chp_toggle,CHP_UNDERLINE); return;
    case IDM_SUPERSCRIPT: format_chars(chp_pos,6); return;
    case IDM_SUBSCRIPT: format_chars(chp_pos,-6); return;
    case IDM_REDUCE: format_chars(chp_size,-4); return;
    case IDM_ENLARGE: format_chars(chp_size,4); return;
    case IDM_FONTS:
        r=DialogBox(instance,"FONTS",h,FontsProc);
        if(r>=0) format_chars(chp_font,(int)r);
        return;
    case IDM_NORMAL: format_paras(pap_normal,0); return;
    case IDM_LEFT: format_paras(pap_jc,0); return;
    case IDM_CENTERED: format_paras(pap_jc,1); return;
    case IDM_RIGHT: format_paras(pap_jc,2); return;
    case IDM_JUSTIFIED: format_paras(pap_jc,3); return;
    case IDM_SINGLE: format_paras(pap_line,240); return;
    case IDM_ONEHALF: format_paras(pap_line,360); return;
    case IDM_DOUBLE: format_paras(pap_line,480); return;
    case IDM_INDENTS: if(DialogBox(instance,"INDENTS",h,IndentsProc)==IDOK) format_paras(pap_set,0); return;
    case IDM_TABS: if(DialogBox(instance,"TABS",h,TabsProc)==IDOK) format_paras(pap_tabs,0); return;
    case IDM_HEADER: editing_running=&header; if(DialogBox(instance,"HEADER",h,RunningProc)==IDOK) modified=TRUE; return;
    case IDM_FOOTER: editing_running=&footer; if(DialogBox(instance,"HEADER",h,RunningProc)==IDOK) modified=TRUE; return;
    case IDM_PAGELAYOUT: if(DialogBox(instance,"PAGELAYOUT",h,PageLayoutProc)==IDOK) {modified=TRUE; relayout(); InvalidateRect(h,NULL,TRUE); show_caret();} return;
    case IDM_ABOUT: MessageBox(h,"Write\n\nA word processor for Write documents.","About Write",MB_OK|MB_ICONASTERISK); return;
    }
}
static void init_menu(HMENU m) {
    UINT selected=anchor!=caret?MF_ENABLED:MF_GRAYED; const Chp *c=&chps[current_chp()]; const Pap *p=&paps[para_pap(para_start(sel_min()))];
    EnableMenuItem(m,IDM_UNDO,undo.text?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,IDM_CUT,selected); EnableMenuItem(m,IDM_COPY,selected);
    EnableMenuItem(m,IDM_PASTE,can_paste()?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,IDM_MOVEPICTURE,picture_start(sel_min())>=0?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,IDM_SIZEPICTURE,picture_start(sel_min())>=0?MF_ENABLED:MF_GRAYED);
    CheckMenuItem(m,IDM_REGULAR,!(c->flags&~CHP_PAGE) && !c->pos?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_BOLD,c->flags&CHP_BOLD?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_ITALIC,c->flags&CHP_ITALIC?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_UNDERLINE,c->flags&CHP_UNDERLINE?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_SUPERSCRIPT,c->pos>0?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_SUBSCRIPT,c->pos<0?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_LEFT,p->jc==0?MF_CHECKED:MF_UNCHECKED); CheckMenuItem(m,IDM_CENTERED,p->jc==1?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_RIGHT,p->jc==2?MF_CHECKED:MF_UNCHECKED); CheckMenuItem(m,IDM_JUSTIFIED,p->jc==3?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_SINGLE,p->line==240?MF_CHECKED:MF_UNCHECKED); CheckMenuItem(m,IDM_ONEHALF,p->line==360?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_DOUBLE,p->line==480?MF_CHECKED:MF_UNCHECKED);
}
static BOOL dragging;
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL shift=GetKeyState(VK_SHIFT)<0,ctrl=GetKeyState(VK_CONTROL)<0;
    switch(msg) {
    case WM_CREATE: main_wnd=h; return 0;
    case WM_SIZE: relayout(); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_SETFOCUS: place_caret(); return 0;
    case WM_KILLFOCUS: DestroyCaret(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        HideCaret(h); SetWindowOrgEx(dc,scroll_x,0,NULL); paint(dc,&ps.rcPaint); ShowCaret(h);
        EndPaint(h,&ps); return 0;
    }
    case WM_VSCROLL: {
        int page=client_height(),old=scroll_y;
        switch(LOWORD(wp)) {
        case SB_LINEUP: scroll_y-=16; break;
        case SB_LINEDOWN: scroll_y+=16; break;
        case SB_PAGEUP: scroll_y-=page; break;
        case SB_PAGEDOWN: scroll_y+=page; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: scroll_y=(short)HIWORD(wp); break;
        }
        if(scroll_y<0) scroll_y=0;
        update_scroll();
        if(scroll_y!=old) {InvalidateRect(h,NULL,TRUE); place_caret();}
        return 0;
    }
    case WM_HSCROLL: {
        int page=client_width()/2,old=scroll_x;
        switch(LOWORD(wp)) {
        case SB_LINEUP: scroll_x-=16; break;
        case SB_LINEDOWN: scroll_x+=16; break;
        case SB_PAGEUP: scroll_x-=page; break;
        case SB_PAGEDOWN: scroll_x+=page; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: scroll_x=(short)HIWORD(wp); break;
        }
        if(scroll_x<0) scroll_x=0;
        update_scroll();
        if(scroll_x!=old) {InvalidateRect(h,NULL,TRUE); place_caret();}
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int pos=position_at(LOWORD(lp),HIWORD(lp)),ps=picture_start(pos);
        if(adjusting) {end_adjust(TRUE); return 0;}
        SetFocus(h); SetCapture(h); dragging=TRUE;
        /* A click on a picture selects it. */
        if(ps>=0 && !shift) {anchor=caret=ps; move(para_end(ps),TRUE);}
        else move(pos,shift);
        return 0;
    }
    case WM_MOUSEMOVE:
        if(adjusting) {adjust_to((short)LOWORD(lp),(short)HIWORD(lp)); return 0;}
        if(dragging) {int pos=position_at((short)LOWORD(lp),(short)HIWORD(lp)); if(pos!=caret) move(pos,TRUE);}
        return 0;
    case WM_LBUTTONUP: if(dragging) {ReleaseCapture(); dragging=FALSE;} return 0;
    case WM_LBUTTONDBLCLK: {
        int a=position_at(LOWORD(lp),HIWORD(lp)),b=a;
        while(a>0 && is_word(text[a-1])) a--;
        while(b<length && is_word(text[b])) b++;
        while(b<length && text[b]==' ') b++;
        anchor=a; move(b,TRUE);
        return 0;
    }
    case WM_KEYDOWN:
        if(adjusting) {
            switch(wp) {
            case VK_LEFT: adjust_by(-1,0); break;
            case VK_RIGHT: adjust_by(1,0); break;
            case VK_UP: adjust_by(0,-1); break;
            case VK_DOWN: adjust_by(0,1); break;
            }
            return 0;
        }
        switch(wp) {
        case VK_LEFT: move(ctrl?caret:step(caret,-1),shift); if(ctrl) {int p=caret; while(p>0 && !is_word(text[p-1])) p--; while(p>0 && is_word(text[p-1])) p--; move(p,shift);} return 0;
        case VK_RIGHT: if(ctrl) {int p=caret; while(p<length && is_word(text[p])) p++; while(p<length && !is_word(text[p])) p++; move(p,shift);} else move(step(caret,1),shift); return 0;
        case VK_UP: move_line(-1,shift); return 0;
        case VK_DOWN: move_line(1,shift); return 0;
        case VK_HOME: move(ctrl?0:lines[line_of(caret)].start,shift); return 0;
        case VK_END: {
            const Line *l=&lines[line_of(caret)]; int e=l->end;
            while(e>l->start && (text[e-1]=='\n' || text[e-1]=='\r')) e--;
            move(ctrl?length:e,shift); return 0;
        }
        case VK_PRIOR: case VK_NEXT: {int n=max(1,client_height()/16); while(n--) move_line(wp==VK_PRIOR?-1:1,shift); return 0;}
        case VK_DELETE: delete_char(1); return 0;
        case VK_RETURN: if(ctrl) {char c=PAGE_BREAK; replace_selection(&c,1); return 0;} break;
        }
        break;
    case WM_CHAR:
        /* Enter keeps a picture's new place or size, Esc leaves it. */
        if(adjusting) {if(wp=='\r') end_adjust(TRUE); else if(wp==27) end_adjust(FALSE); return 0;}
        if(wp=='\b') {delete_char(-1); return 0;}
        if(wp=='\r') {replace_selection("\r\n",2); return 0;}
        if(wp=='\t' || wp>=' ') {char c=(char)wp; replace_selection(&c,1); return 0;}
        return 0;
    case WM_INITMENUPOPUP: init_menu((HMENU)wp); return 0;
    case WM_COMMAND: command(h,LOWORD(wp)); return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"WRITE.HLP",HELP_QUIT,0); device_reset(&screen); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"MSWRITE"); wc.hCursor=LoadCursor(NULL,IDC_IBEAM);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="MSWRITE"; wc.lpszClassName="MSWRITE_MENU";
        if(!RegisterClass(&wc)) return 0;
    }
    screen.dc=CreateCompatibleDC(NULL);
    {HDC dc=GetDC(NULL); screen.dpi_x=GetDeviceCaps(dc,LOGPIXELSX); screen.dpi_y=GetDeviceCaps(dc,LOGPIXELSY); ReleaseDC(NULL,dc);}
    new_document();
    main_wnd=CreateWindow("MSWRITE_MENU","Write",WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_HSCROLL,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    set_title(); relayout();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    while(command_line && *command_line==' ') command_line++;
    if(command_line && *command_line) open_path(command_line);
    accel=LoadAccelerators(inst,"MSWRITE");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    DeleteDC(screen.dc);
    return (int)m.wParam;
}
