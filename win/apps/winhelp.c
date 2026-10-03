/* SPDX-License-Identifier: GPL-2.0-or-later
 * Help viewer; hlpfile.c decodes topics. Layout handles paragraph wraps,
 * indents, tabs, alignment, inline/side pictures and table cells. Hotspots
 * jump, show popups or execute macros; Tab/Enter provides keyboard access.
 * Navigation uses contents, history, browse sequences and keyword search.
 * WINHELP.INI stores bookmarks per help file.
 *
 * USER's WinHelp sends registered WM_WINHELP to class MS_WINHELP. Copy the
 * request and process it asynchronously after the sender continues.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "hlpfile.h"
#include "winhelp.h"
#define GREEN RGB(0,128,0)
#define MARGIN_HP 12   /* around a topic, in half points */
#define DEFAULT_TAB 72 /* half an inch */
#define FLOAT_GAP 6    /* pixels between a side picture and the text */
#define HISTORY 40
#define BOOKMARKS 9
#define WM_REQUEST (WM_USER+1)
#define NO_TOPIC HELP_NO_TOPIC

static HINSTANCE instance;
static HWND main_wnd,view,popup,buttons[5];
static HCURSOR hand;
static UINT help_message;
static HelpFile *help;               /* the file shown */
static HelpTopic *topic;             /* the topic shown */
static DWORD contents=NO_TOPIC;      /* HELP_SETINDEX's, else the file's */
static int scroll_y,focus_spot=-1;
typedef struct {char path[MAX_PATH]; DWORD topic; int scroll;} Visit;
static Visit history[HISTORY]; static int history_count;
static char search_word[80];

/* --- devices: a help file's fonts for the screen or the printer ------------------------------ */
typedef struct {HFONT font; short widths[256]; short ascent,height; COLORREF color;} DevFont;
typedef struct {HDC dc; int dpi_x,dpi_y,count; const HelpFile *file; DevFont *fonts; BYTE *ready;} Device;
static Device screen,printer,popup_device;
static void device_reset(Device *d) {
    int i;
    for(i=0;i<d->count;i++) if(d->ready[i]) DeleteObject(d->fonts[i].font);
    if(d->fonts) GlobalFree((HGLOBAL)d->fonts);
    if(d->ready) GlobalFree((HGLOBAL)d->ready);
    d->fonts=NULL; d->ready=NULL; d->count=0; d->file=NULL;
}
/* Font n of the file (one past its last is the default: Helv, 10 points). */
static DevFont *device_font(Device *d,const HelpFile *file,int n) {
    static const BYTE families[6]={FF_DONTCARE,FF_MODERN,FF_ROMAN,FF_SWISS,FF_SCRIPT,FF_DECORATIVE};
    static DevFont none;
    LOGFONT lf; TEXTMETRIC tm; int w[256],i; HFONT old; const HelpFont *f; DevFont *df;
    if(d->file!=file) device_reset(d);
    if(!d->fonts) {
        d->count=(file?HelpFontCount(file):0)+1; d->file=file;
        d->fonts=(DevFont *)GlobalAlloc(GPTR,(DWORD)d->count*sizeof(DevFont)); d->ready=(BYTE *)GlobalAlloc(GPTR,(DWORD)d->count);
        if(!d->fonts || !d->ready) {device_reset(d); return &none;}
    }
    if(n<0 || n>=d->count-1) n=d->count-1;
    df=&d->fonts[n];
    if(d->ready[n]) return df;
    f=file?HelpFontAt(file,n):NULL;
    memset(&lf,0,sizeof(lf));
    lf.lfHeight=-MulDiv(f && f->half_points?f->half_points:20,d->dpi_y,144);
    lf.lfWeight=f && f->attributes&HELP_BOLD?FW_BOLD:FW_NORMAL;
    lf.lfItalic=(BYTE)(f && f->attributes&HELP_ITALIC);
    lf.lfUnderline=(BYTE)(f && f->attributes&(HELP_UNDERLINE|HELP_DOUBLE));
    lf.lfStrikeOut=(BYTE)(f && f->attributes&HELP_STRIKEOUT);
    lf.lfPitchAndFamily=(BYTE)(f && f->family<6?families[f->family]:FF_SWISS);
    lstrcpyn(lf.lfFaceName,f && f->face[0]?f->face:"Helv",LF_FACESIZE);
    df->font=CreateFontIndirect(&lf); df->color=f?f->color:0;
    old=(HFONT)SelectObject(d->dc,df->font);
    GetTextMetrics(d->dc,&tm); GetCharWidth(d->dc,0,255,w);
    for(i=0;i<256;i++) df->widths[i]=(short)w[i];
    df->ascent=(short)tm.tmAscent; df->height=(short)(tm.tmHeight+tm.tmExternalLeading);
    SelectObject(d->dc,old);
    d->ready[n]=1;
    return df;
}
static int px_x(const Device *d,int half_points) {return MulDiv(half_points,d->dpi_x,144);}
static int px_y(const Device *d,int half_points) {return MulDiv(half_points,d->dpi_y,144);}
/* A picture's size on the device: a bitmap's pixels are the screen's (96 to
 * the inch), a metafile's size is in hundredths of a millimetre. */
static void picture_size(const Device *d,const HelpPicture *p,int *w,int *h) {
    if(p->metafile) {
        *w=p->width>0?MulDiv(p->width,d->dpi_x,2540):d->dpi_x; *h=p->height>0?MulDiv(p->height,d->dpi_y,2540):d->dpi_y;
    } else {*w=MulDiv(p->width,d->dpi_x,96); *h=MulDiv(p->height,d->dpi_y,96);}
    *w=max(1,*w); *h=max(1,*h);
}
static void draw_picture(HDC dc,const HelpPicture *p,int x,int y,int w,int h) {
    int saved=SaveDC(dc);
    IntersectClipRect(dc,x,y,x+w,y+h);
    if(!p->metafile) {
        const BITMAPINFOHEADER *bh=(const BITMAPINFOHEADER *)p->data;
        DWORD colors=bh->biClrUsed?bh->biClrUsed:bh->biBitCount<=8?1u<<bh->biBitCount:0;
        SetStretchBltMode(dc,COLORONCOLOR);
        StretchDIBits(dc,x,y,w,h,0,0,(int)bh->biWidth,(int)bh->biHeight,p->data+bh->biSize+colors*4,(const BITMAPINFO *)p->data,DIB_RGB_COLORS,SRCCOPY);
    } else {
        HMETAFILE mf=SetMetaFileBitsEx(p->size,p->data);
        if(mf) {
            SetMapMode(dc,p->mm==MM_ISOTROPIC || p->mm==MM_ANISOTROPIC || !p->mm?MM_ANISOTROPIC:p->mm);
            SetWindowOrgEx(dc,0,0,NULL);
            if(p->width>0 && p->height>0) SetWindowExtEx(dc,p->width,p->height,NULL);
            SetViewportOrgEx(dc,x,y,NULL); SetViewportExtEx(dc,w,h,NULL);
            PlayMetaFile(dc,mf);
            DeleteMetaFile(mf);
        }
    }
    RestoreDC(dc,saved);
}

/* --- layout ---------------------------------------------------------------------------------- */
/* A topic laid out: pieces of text (a run's characters at..at+length in
 * one font), pictures (in the line, or at its side), and border rules,
 * each with the top and bottom of its line for paging. */
#define PIECE_TEXT 0
#define PIECE_PICTURE 1
#define PIECE_SIDE 2  /* a picture at the side, the text beside it */
#define PIECE_RULE 3
typedef struct {BYTE kind; short spot; WORD font; int x,y,w,h,top,bottom; DWORD at,length;} Piece;
typedef struct {Piece *pieces; int count,cap,width,height,right;} Layout;
static Layout shown,popup_layout;
typedef struct {
    Layout *l; Device *d; const HelpFile *file; const HelpTopic *t; const HelpPara *p;
    int col_left,col_right;     /* the page, or a table's cell */
    int left,right;             /* the paragraph's text, inside its indents */
    int x,y,line_from;          /* the pen, the line's top, its first piece */
    BOOL first,can_break;       /* the paragraph's first line; a space (or tab) before x */
    int tab_kind,tab_at,tab_from,tab_x; /* a right or centred stop, waiting for its text */
    int left_bottom,left_w,right_bottom,right_w; /* pictures at the sides */
    WORD font;
} Flow;
static Piece *add_piece(Flow *f,BYTE kind) {
    Layout *l=f->l; Piece *p;
    if(l->count==l->cap) {
        int cap=l->cap?l->cap*2:256; Piece *n=(Piece *)GlobalAlloc(GPTR,(DWORD)cap*sizeof(Piece));
        if(!n) return NULL;
        if(l->pieces) {memcpy(n,l->pieces,(size_t)l->count*sizeof(Piece)); GlobalFree((HGLOBAL)l->pieces);}
        l->pieces=n; l->cap=cap;
    }
    p=&l->pieces[l->count++]; memset(p,0,sizeof(*p));
    p->kind=kind; p->spot=-1;
    return p;
}
static BOOL in_line(const Piece *p) {return p->kind==PIECE_TEXT || p->kind==PIECE_PICTURE;}
static int line_left(const Flow *f) {
    int x=f->left+(f->first?px_x(f->d,f->p->first):0);
    if(f->y<f->left_bottom) x=max(x,f->col_left+f->left_w);
    return max(f->col_left,x);
}
static int line_right(const Flow *f) {
    int x=f->right;
    if(f->y<f->right_bottom) x=min(x,f->col_right-f->right_w);
    return max(line_left(f)+1,x);
}
static BOOL line_used(const Flow *f) {
    int i;
    for(i=f->line_from;i<f->l->count;i++) if(in_line(&f->l->pieces[i])) return TRUE;
    return FALSE;
}
static void begin_line(Flow *f) {f->line_from=f->l->count; f->x=line_left(f); f->tab_kind=-1; f->can_break=TRUE;}
static void shift(Flow *f,int from,int dx) {
    int i;
    for(i=from;i<f->l->count;i++) if(in_line(&f->l->pieces[i])) f->l->pieces[i].x+=dx;
}
/* A right or centred tab stop's text, now that it is all there. */
static void settle_tab(Flow *f) {
    int width=f->x-f->tab_x,dx;
    if(f->tab_kind<0) return;
    dx=(f->tab_kind==HELP_RIGHT?f->tab_at-width:f->tab_at-width/2)-f->tab_x;
    if(dx>0) {shift(f,f->tab_from,dx); f->x+=dx;}
    f->tab_kind=-1;
}
static void end_line(Flow *f) {
    int i,ascent=0,descent=0,height,dx; BOOL any=FALSE;
    settle_tab(f);
    for(i=f->line_from;i<f->l->count;i++) {
        Piece *p=&f->l->pieces[i];
        if(p->kind==PIECE_TEXT) {DevFont *df=device_font(f->d,f->file,p->font); ascent=max(ascent,df->ascent); descent=max(descent,df->height-df->ascent); any=TRUE;}
        else if(p->kind==PIECE_PICTURE) {ascent=max(ascent,p->h); any=TRUE;}
    }
    if(!any) {DevFont *df=device_font(f->d,f->file,f->font); ascent=df->ascent; descent=df->height-df->ascent;}
    height=ascent+descent;
    if(f->p->lines>0) height=max(height,px_y(f->d,f->p->lines));
    else if(f->p->lines<0) height=px_y(f->d,-f->p->lines);
    dx=line_right(f)-f->x;
    if(dx>0 && f->p->align!=HELP_LEFT) shift(f,f->line_from,f->p->align==HELP_CENTER?dx/2:dx);
    for(i=f->line_from;i<f->l->count;i++) {
        Piece *p=&f->l->pieces[i];
        if(p->kind==PIECE_TEXT) p->y=f->y+ascent-device_font(f->d,f->file,p->font)->ascent;
        else if(p->kind==PIECE_PICTURE) p->y=f->y+ascent-p->h;
        else continue;
        p->top=f->y; p->bottom=f->y+height;
        f->l->right=max(f->l->right,p->x+p->w);
    }
    f->y+=height; f->first=FALSE;
    begin_line(f);
}
/* Text: a word (and the spaces after it) at a time, on the next line when it does not fit. */
static void flow_text(Flow *f,const HelpRun *r) {
    const char *text=f->t->text; DWORD i=r->at,end=r->at+r->length;
    DevFont *df=device_font(f->d,f->file,r->font);
    f->font=r->font;
    while(i<end) {
        DWORD j=i,k; int w=0,inked; Piece *last;
        while(j<end && text[j]!=' ') j++;
        for(k=i;k<j;k++) w+=df->widths[(BYTE)text[k]];
        inked=w;
        while(j<end && text[j]==' ') w+=df->widths[(BYTE)text[j++]];
        if(f->x+inked>line_right(f) && f->can_break && line_used(f)) end_line(f);
        last=f->l->count>f->line_from?&f->l->pieces[f->l->count-1]:NULL;
        if(last && last->kind==PIECE_TEXT && last->font==r->font && last->spot==r->spot && last->at+last->length==i && last->x+last->w==f->x) {last->length+=j-i; last->w+=w;}
        else if((last=add_piece(f,PIECE_TEXT))!=NULL) {last->font=r->font; last->spot=r->spot; last->at=i; last->length=j-i; last->x=f->x; last->w=w;}
        f->x+=w; f->can_break=text[j-1]==' ';
        i=j;
    }
}
static void flow_tab(Flow *f) {
    int i,at=-1,kind=HELP_LEFT,step=px_x(f->d,DEFAULT_TAB);
    settle_tab(f);
    for(i=0;i<f->p->tabs;i++) {
        int t=f->col_left+px_x(f->d,f->p->tab[i]);
        if(t>f->x) {at=t; kind=f->p->tab_kind[i]; break;}
    }
    if(at<0) at=f->col_left+((f->x-f->col_left)/step+1)*step;
    if(at>line_right(f) && line_used(f)) {end_line(f); return;}
    if(kind==HELP_LEFT) f->x=at;
    else {f->tab_kind=kind; f->tab_at=at; f->tab_from=f->l->count; f->tab_x=f->x;}
    f->can_break=TRUE;
}
static void flow_picture(Flow *f,const HelpRun *r) {
    const HelpPicture *pic=&f->t->pictures[r->at]; int w,h; Piece *p;
    picture_size(f->d,pic,&w,&h);
    if(r->place==0) {
        if(f->x+w>line_right(f) && line_used(f)) end_line(f);
        if((p=add_piece(f,PIECE_PICTURE))==NULL) return;
        p->x=f->x; p->w=w; p->h=h; p->at=r->at; p->spot=r->spot;
        f->x+=w; f->can_break=TRUE;
        return;
    }
    if(line_used(f)) end_line(f);
    if((p=add_piece(f,PIECE_SIDE))==NULL) return;
    p->w=w; p->h=h; p->at=r->at; p->spot=r->spot; p->y=p->top=f->y; p->bottom=f->y+h;
    if(r->place==1) {p->x=f->left; f->left_bottom=f->y+h; f->left_w=p->x+w+FLOAT_GAP-f->col_left;}
    else {p->x=max(f->left,f->right-w); f->right_bottom=f->y+h; f->right_w=f->col_right-p->x+FLOAT_GAP;}
    f->l->right=max(f->l->right,p->x+w);
    begin_line(f);
}
static void rule(Flow *f,int x,int y,int w,int h) {
    Piece *p=add_piece(f,PIECE_RULE);
    if(p) {p->x=x; p->y=p->top=y; p->w=w; p->h=h; p->bottom=y+h;}
}
static void flow_paragraph(Flow *f,const HelpPara *p) {
    int i,top; const HelpRun *last;
    f->p=p; f->y+=px_y(f->d,p->above); top=f->y;
    f->left=f->col_left+px_x(f->d,p->left); f->right=max(f->left+px_x(f->d,24),f->col_right-px_x(f->d,p->right));
    f->first=TRUE; begin_line(f);
    for(i=0;i<p->runs;i++) {
        const HelpRun *r=&f->t->runs[p->first_run+i];
        switch(r->kind) {
        case RUN_TEXT: flow_text(f,r); break;
        case RUN_TAB: flow_tab(f); break;
        case RUN_BREAK: end_line(f); break;
        case RUN_PICTURE: if(r->at<(DWORD)f->t->picture_count) flow_picture(f,r); break;
        }
    }
    /* The last line, unless all there is beside a side picture is to come. */
    last=p->runs?&f->t->runs[p->first_run+p->runs-1]:NULL;
    if(line_used(f) || !last || last->kind!=RUN_PICTURE || last->place==0) end_line(f);
    if(p->border) {
        int thick=p->border&32?2:1,x0=max(f->col_left,min(f->left,f->left+px_x(f->d,p->first)))-3,x1=f->right+3,y0=top-2,y1=f->y+2;
        if(p->border&3) rule(f,x0,y0,x1-x0,thick);
        if(p->border&9) rule(f,x0,y1-thick,x1-x0,thick);
        if(p->border&5) rule(f,x0,y0,thick,y1-y0);
        if(p->border&17) rule(f,x1-thick,y0,thick,y1-y0);
        f->y+=4;
    }
    f->y+=px_y(f->d,p->below);
}
/* The topic for a width: paragraphs down the page; a table's cells start
 * their row together, the row as deep as its deepest cell. */
static void layout_topic(Layout *l,Device *d,const HelpFile *file,const HelpTopic *t,int width) {
    Flow f; int i,margin=px_x(d,MARGIN_HP),row_top=0,row_bottom=0,last_column=-1;
    l->count=0; l->width=width; l->right=0; l->height=0;
    if(!t) return;
    memset(&f,0,sizeof(f)); f.l=l; f.d=d; f.file=file; f.t=t; f.y=px_y(d,MARGIN_HP)/2; f.font=0xffff;
    for(i=0;i<t->para_count;i++) {
        const HelpPara *p=&t->paras[i];
        if(p->column>=0) {
            int avail=max(1,width-2*margin);
            if(p->new_cell) {
                if(last_column<0) row_top=row_bottom=max(f.y,max(f.left_bottom,f.right_bottom));
                else if(p->column<=last_column) row_top=row_bottom;
                f.y=row_top; last_column=p->column;
            }
            if(p->relative) {f.col_left=margin+MulDiv(p->cell_left,avail,32767); f.col_right=f.col_left+MulDiv(p->cell_width,avail,32767);}
            else {f.col_left=margin+px_x(d,p->cell_left); f.col_right=f.col_left+px_x(d,p->cell_width);}
            f.col_right=max(f.col_right,f.col_left+px_x(d,24));
            flow_paragraph(&f,p);
            row_bottom=max(row_bottom,f.y);
        } else {
            if(last_column>=0) {f.y=max(f.y,row_bottom); last_column=-1;}
            f.col_left=margin; f.col_right=max(margin+px_x(d,24),width-margin);
            flow_paragraph(&f,p);
        }
    }
    if(last_column>=0) f.y=max(f.y,row_bottom);
    l->height=max(f.y,max(f.left_bottom,f.right_bottom))+px_y(d,MARGIN_HP)/2;
}
static void free_layout(Layout *l) {
    if(l->pieces) GlobalFree((HGLOBAL)l->pieces);
    memset(l,0,sizeof(*l));
}
/* The pieces drawn, moved by (dx,dy); those of spot focus framed. */
static void draw_layout(HDC dc,Device *d,const HelpFile *file,const HelpTopic *t,const Layout *l,int dx,int dy,const RECT *area,int focus) {
    int i,mode=SetBkMode(dc,TRANSPARENT);
    for(i=0;i<l->count;i++) {
        const Piece *p=&l->pieces[i]; RECT r;
        SetRect(&r,p->x+dx,p->y+dy,p->x+dx+p->w,p->y+dy+(p->kind==PIECE_TEXT?p->bottom-p->y:p->h));
        if(area && (r.bottom<=area->top || r.top>=area->bottom)) continue;
        if(p->kind==PIECE_TEXT) {
            DevFont *df=device_font(d,file,p->font); const HelpSpot *s=p->spot>=0?&t->spots[p->spot]:NULL; HFONT old=(HFONT)SelectObject(dc,df->font);
            SetTextColor(dc,s && s->visible?GREEN:df->color);
            TextOut(dc,r.left,r.top,t->text+p->at,(int)p->length);
            if(s && s->visible) {
                int y=r.top+df->ascent+1,x,w=p->w; DWORD k=p->at+p->length;
                while(k>p->at && t->text[k-1]==' ') w-=df->widths[(BYTE)t->text[--k]];
                if(s->kind==SPOT_POPUP) for(x=0;x<w;x+=2) SetPixel(dc,r.left+x,y,GREEN);
                else {HBRUSH b=CreateSolidBrush(GREEN); RECT u; SetRect(&u,r.left,y,r.left+w,y+1); FillRect(dc,&u,b); DeleteObject(b);}
            }
            SelectObject(dc,old);
        } else if(p->kind==PIECE_PICTURE || p->kind==PIECE_SIDE) draw_picture(dc,&t->pictures[p->at],r.left,r.top,p->w,p->h);
        else FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(focus>=0 && p->spot==focus) {
            if(p->kind==PIECE_TEXT) r.bottom=r.top+device_font(d,file,p->font)->height;
            DrawFocusRect(dc,&r);
        }
    }
    SetBkMode(dc,mode);
}
/* The hotspot at a point of the layout, or -1. */
static int spot_at(const Layout *l,int x,int y) {
    int i;
    for(i=0;i<l->count;i++) {
        const Piece *p=&l->pieces[i]; int bottom=p->kind==PIECE_TEXT?p->bottom:p->y+p->h;
        if(p->spot>=0 && x>=p->x && x<p->x+p->w && y>=p->y && y<bottom) return p->spot;
    }
    return -1;
}
static BOOL spot_rect(const Layout *l,int spot,RECT *out) {
    int i; BOOL found=FALSE;
    for(i=0;i<l->count;i++) {
        const Piece *p=&l->pieces[i]; RECT r;
        if(p->spot!=spot) continue;
        SetRect(&r,p->x,p->y,p->x+p->w,p->kind==PIECE_TEXT?p->bottom:p->y+p->h);
        if(found) UnionRect(out,out,&r); else *out=r;
        found=TRUE;
    }
    return found;
}

/* --- the topic window ---------------------------------------------------------------------- */
static int view_height(void) {RECT r; GetClientRect(view,&r); return r.bottom;}
static void update_scroll(void) {
    int range=max(0,shown.height-view_height());
    scroll_y=max(0,min(scroll_y,range));
    SetScrollRange(view,SB_VERT,0,range,FALSE);
    SetScrollPos(view,SB_VERT,scroll_y,TRUE);
}
/* The pointer: the hand over a hotspot under it. */
static void update_cursor(void) {
    POINT p;
    GetCursorPos(&p);
    if(popup || WindowFromPoint(p)!=view) return;
    ScreenToClient(view,&p);
    SetCursor(topic && spot_at(&shown,p.x,p.y+scroll_y)>=0?hand:LoadCursor(NULL,IDC_ARROW));
}
static void relayout(void) {
    RECT r; GetClientRect(view,&r);
    layout_topic(&shown,&screen,help,topic,r.right);
    update_scroll();
    InvalidateRect(view,NULL,TRUE);
    update_cursor();
}
static void scroll_to(int y) {
    int old=scroll_y;
    scroll_y=y; update_scroll();
    if(scroll_y!=old) InvalidateRect(view,NULL,TRUE);
}
static void update_buttons(void) {
    EnableWindow(buttons[0],help!=NULL);
    EnableWindow(buttons[1],history_count>0);
    EnableWindow(buttons[2],topic && topic->previous!=NO_TOPIC);
    EnableWindow(buttons[3],topic && topic->next!=NO_TOPIC);
    EnableWindow(buttons[4],help && HelpKeywordCount(help)>0);
}
static void set_title(void) {
    char title[200];
    if(!help) lstrcpy(title,"Help");
    else if(HelpTitle(help)[0]) lstrcpyn(title,HelpTitle(help),sizeof(title));
    else wsprintf(title,"Help - %s",FileTitle(HelpPath(help)));
    SetWindowText(main_wnd,title);
}
static void close_popup(void);
/* The topic shown, for Back. */
static void remember_topic(void) {
    if(!topic) return;
    if(history_count==HISTORY) {memmove(history,history+1,sizeof(Visit)*(HISTORY-1)); history_count--;}
    lstrcpyn(history[history_count].path,HelpPath(help),MAX_PATH);
    history[history_count].topic=topic->number; history[history_count].scroll=scroll_y;
    history_count++;
}
/* Show topic n of the file, remembering the one shown for Back. */
static void show_topic(DWORD n,BOOL remember,int scroll) {
    HelpTopic *t;
    close_popup();
    if(!help || !(t=HelpReadTopic(help,n))) {MessageBeep(0); return;}
    if(remember) remember_topic();
    HelpFreeTopic(topic); topic=t;
    focus_spot=-1; scroll_y=scroll;
    relayout();
    update_buttons();
}

/* --- files and bookmarks --------------------------------------------------------------------- */
static void build_bookmarks(void);
/* A help file named by a program or a jump: "name>window" means the window
 * (there is only one); a name without a directory is looked for beside the
 * file shown, then where OpenFile looks. */
static BOOL find_help(LPCSTR name,char *path) {
    char plain[MAX_PATH],*p; OFSTRUCT of;
    lstrcpyn(plain,name,sizeof(plain));
    if((p=strchr(plain,'>'))!=NULL) *p=0;
    if(!plain[0]) {if(!help) return FALSE; lstrcpy(path,HelpPath(help)); return TRUE;}
    if(help && !strchr(plain,'\\') && !strchr(plain,':')) {
        char beside[MAX_PATH]; int n;
        lstrcpyn(beside,HelpPath(help),sizeof(beside)); n=(int)(FileTitle(beside)-beside);
        if(n+lstrlen(plain)<(int)sizeof(beside)) {
            lstrcpy(beside+n,plain);
            if(GetFileAttributes(beside)!=INVALID_FILE_ATTRIBUTES) {lstrcpy(path,beside); return TRUE;}
        }
    }
    if(OpenFile(plain,&of,OF_EXIST)==HFILE_ERROR) return FALSE;
    lstrcpyn(path,of.szPathName,MAX_PATH);
    return TRUE;
}
/* Another file (the topic shown remembered for Back, unless going back). */
static BOOL open_help(LPCSTR name,BOOL remember) {
    char path[MAX_PATH],text[MAX_PATH+64]; HelpFile *h;
    if(!find_help(name,path)) {
        wsprintf(text,"Cannot find the file %s.",name);
        MessageBox(main_wnd,text,"Help",MB_OK|MB_ICONEXCLAMATION);
        return FALSE;
    }
    if(help && !lstrcmpi(HelpPath(help),path)) return TRUE;
    if(!(h=HelpOpen(path))) {
        wsprintf(text,"%s is not a help file.",FileTitle(path));
        MessageBox(main_wnd,text,"Help",MB_OK|MB_ICONEXCLAMATION);
        return FALSE;
    }
    close_popup();
    if(remember) remember_topic();
    HelpFreeTopic(topic); topic=NULL;
    device_reset(&screen);
    HelpClose(help); help=h; contents=NO_TOPIC;
    set_title(); build_bookmarks();
    return TRUE;
}
static DWORD contents_topic(void) {return contents!=NO_TOPIC?contents:HelpContents(help);}
static void show_contents(void) {if(help) show_topic(contents_topic(),TRUE,0);}
static void go_back(void) {
    Visit v;
    if(!history_count) return;
    v=history[--history_count];
    if(open_help(v.path,FALSE)) show_topic(v.topic,FALSE,v.scroll);
    update_buttons();
}
/* Along the browse sequence: the next topic, or the previous one. */
static void browse(BOOL next) {DWORD n=!topic?NO_TOPIC:next?topic->next:topic->previous; if(n!=NO_TOPIC) show_topic(n,TRUE,0);}
static void bookmark_section(char *out) {lstrcpyn(out,FileTitle(HelpPath(help)),MAX_PATH); AnsiUpper(out);}
/* The bookmarks, as the Bookmark menu lists them: the keys of the file's section. */
static int bookmark_names(char *names,int size) {
    char section[MAX_PATH]; int n;
    if(!help) {names[0]=names[1]=0; return 0;}
    bookmark_section(section);
    n=GetPrivateProfileString(section,NULL,"",names,size-1,"WINHELP.INI");
    names[n]=names[n+1]=0;
    return n;
}
static void build_bookmarks(void) {
    HMENU menu=GetSubMenu(GetMenu(main_wnd),2); char names[1024]; const char *p; int i;
    while(GetMenuItemCount(menu)>1) DeleteMenu(menu,1,MF_BYPOSITION);
    bookmark_names(names,sizeof(names)-1);
    for(p=names,i=0;*p && i<BOOKMARKS;p+=lstrlen(p)+1,i++) {
        char item[80];
        if(!i) AppendMenu(menu,MF_SEPARATOR,0,NULL);
        wsprintf(item,"&%d %s",i+1,p);
        AppendMenu(menu,MF_STRING,IDM_BOOKMARK+i,item);
    }
}
static void go_bookmark(int index) {
    char names[1024],section[MAX_PATH]; const char *p; int i;
    bookmark_names(names,sizeof(names)-1); bookmark_section(section);
    for(p=names,i=0;*p && i<index;p+=lstrlen(p)+1,i++) {}
    if(*p) {
        UINT n=GetPrivateProfileInt(section,p,0,"WINHELP.INI");
        if(n<HelpTopicCount(help)) show_topic(n,TRUE,0);
    }
}
static INT_PTR CALLBACK BookmarkProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    char names[1024],name[64],section[MAX_PATH],number[16]; const char *p; int sel;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_NAME,topic?topic->title:"");
        SendDlgItemMessage(dlg,IDC_NAME,EM_LIMITTEXT,sizeof(name)-1,0);
        bookmark_names(names,sizeof(names)-1);
        for(p=names;*p;p+=lstrlen(p)+1) SendDlgItemMessage(dlg,IDC_MARKS,LB_ADDSTRING,0,(LPARAM)p);
        return TRUE;
    case WM_COMMAND:
        bookmark_section(section);
        switch(LOWORD(wp)) {
        case IDC_MARKS:
            if(HIWORD(wp)==LBN_SELCHANGE && (sel=(int)SendDlgItemMessage(dlg,IDC_MARKS,LB_GETCURSEL,0,0))>=0) {
                SendDlgItemMessage(dlg,IDC_MARKS,LB_GETTEXT,sel,(LPARAM)name); SetDlgItemText(dlg,IDC_NAME,name);
            }
            return TRUE;
        case IDC_DELETE:
            if((sel=(int)SendDlgItemMessage(dlg,IDC_MARKS,LB_GETCURSEL,0,0))>=0) {
                SendDlgItemMessage(dlg,IDC_MARKS,LB_GETTEXT,sel,(LPARAM)name);
                WritePrivateProfileString(section,name,NULL,"WINHELP.INI");
                SendDlgItemMessage(dlg,IDC_MARKS,LB_DELETESTRING,sel,0);
            }
            return TRUE;
        case IDOK:
            GetDlgItemText(dlg,IDC_NAME,name,sizeof(name));
            if(name[0] && topic) {
                char *e;
                for(e=name;*e;e++) if(*e=='=' || *e=='[' || *e==']') *e=' ';
                wsprintf(number,"%lu",topic->number);
                WritePrivateProfileString(section,name,number,"WINHELP.INI");
            }
            EndDialog(dlg,1);
            return TRUE;
        case IDCANCEL: EndDialog(dlg,0); return TRUE;
        }
        break;
    }
    return FALSE;
}

/* --- searching ------------------------------------------------------------------------------ */
static DWORD found_topic=NO_TOPIC;
static int keyword_matching(LPCSTR word) {return word[0]?HelpFindKeyword(help,word):-1;}
/* What a typed word selects: the first keyword it does not come after. */
static int keyword_nearest(LPCSTR word) {
    int i,n=HelpKeywordCount(help);
    if(!word[0]) return -1;
    for(i=0;i<n;i++) if(lstrcmpi(HelpKeyword(help,i),word)>=0) return i;
    return n-1;
}
static void show_topics(HWND dlg) {
    DWORD topics[256]; int n,i,k=(int)SendDlgItemMessage(dlg,IDC_WORDS,LB_GETCURSEL,0,0);
    char word[80];
    if(k<0) {GetDlgItemText(dlg,IDC_WORD,word,sizeof(word)); k=keyword_matching(word);}
    SendDlgItemMessage(dlg,IDC_TOPICS,LB_RESETCONTENT,0,0);
    if(k<0) {MessageBeep(0); return;}
    n=HelpKeywordTopics(help,k,topics,256);
    for(i=0;i<n;i++) {
        LPCSTR title=HelpTopicTitle(help,topics[i]);
        int at=(int)SendDlgItemMessage(dlg,IDC_TOPICS,LB_ADDSTRING,0,(LPARAM)(title[0]?title:"(Untitled)"));
        SendDlgItemMessage(dlg,IDC_TOPICS,LB_SETITEMDATA,at,(LPARAM)topics[i]);
    }
    SendDlgItemMessage(dlg,IDC_TOPICS,LB_SETCURSEL,0,0);
    EnableWindow(GetDlgItem(dlg,IDC_GOTO),n>0);
    if(n) {SendMessage(dlg,DM_SETDEFID,IDC_GOTO,0); SetFocus(GetDlgItem(dlg,IDC_TOPICS));}
}
static INT_PTR CALLBACK SearchProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    static BOOL typing; int i,k; char word[80];
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        for(i=0;i<HelpKeywordCount(help);i++) SendDlgItemMessage(dlg,IDC_WORDS,LB_ADDSTRING,0,(LPARAM)HelpKeyword(help,i));
        SendDlgItemMessage(dlg,IDC_WORD,EM_LIMITTEXT,sizeof(word)-1,0);
        EnableWindow(GetDlgItem(dlg,IDC_GOTO),FALSE);
        SetDlgItemText(dlg,IDC_WORD,search_word);
        if(search_word[0] && (k=keyword_nearest(search_word))>=0) {
            SendDlgItemMessage(dlg,IDC_WORDS,LB_SETCURSEL,k,0);
            if(!lstrcmpi(HelpKeyword(help,k),search_word)) show_topics(dlg);
        }
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_WORD:
            if(HIWORD(wp)==EN_CHANGE && !typing) {
                GetDlgItemText(dlg,IDC_WORD,word,sizeof(word));
                k=keyword_nearest(word);
                SendDlgItemMessage(dlg,IDC_WORDS,LB_SETCURSEL,k,0);
                if(k>=0) SendDlgItemMessage(dlg,IDC_WORDS,LB_SETTOPINDEX,k,0);
                SendMessage(dlg,DM_SETDEFID,IDC_SHOW,0);
            }
            return TRUE;
        case IDC_WORDS:
            if((k=(int)SendDlgItemMessage(dlg,IDC_WORDS,LB_GETCURSEL,0,0))>=0 && (HIWORD(wp)==LBN_SELCHANGE || HIWORD(wp)==LBN_DBLCLK)) {
                typing=TRUE; SetDlgItemText(dlg,IDC_WORD,HelpKeyword(help,k)); typing=FALSE;
                if(HIWORD(wp)==LBN_DBLCLK) show_topics(dlg);
            }
            return TRUE;
        case IDC_SHOW: show_topics(dlg); return TRUE;
        case IDC_TOPICS:
            if(HIWORD(wp)!=LBN_DBLCLK) return TRUE;
            /* fall through */
        case IDC_GOTO:
            if((k=(int)SendDlgItemMessage(dlg,IDC_TOPICS,LB_GETCURSEL,0,0))<0) return TRUE;
            found_topic=(DWORD)SendDlgItemMessage(dlg,IDC_TOPICS,LB_GETITEMDATA,k,0);
            GetDlgItemText(dlg,IDC_WORD,search_word,sizeof(search_word));
            EndDialog(dlg,1);
            return TRUE;
        case IDOK:
            /* Enter: Show Topics, or Go To once there are topics. */
            if(GetFocus()==GetDlgItem(dlg,IDC_TOPICS) && IsWindowEnabled(GetDlgItem(dlg,IDC_GOTO))) SendMessage(dlg,WM_COMMAND,IDC_GOTO,0);
            else show_topics(dlg);
            return TRUE;
        case IDCANCEL: EndDialog(dlg,0); return TRUE;
        }
        break;
    }
    return FALSE;
}
static void search(LPCSTR word) {
    if(!help || !HelpKeywordCount(help)) {MessageBeep(0); return;}
    if(word) lstrcpyn(search_word,word,sizeof(search_word));
    found_topic=NO_TOPIC;
    if(DialogBox(instance,"SEARCH",main_wnd,SearchProc)>0 && found_topic!=NO_TOPIC) show_topic(found_topic,TRUE,0);
    SetFocus(view);
}
/* A keyword's topic straight away when it leads to just one, else Search
 * (the keyword's topics listed, or the nearest keyword chosen). */
static void key(LPCSTR word) {
    DWORD topics[2]; int k;
    if(!help) return;
    k=keyword_matching(word);
    if(k>=0 && !lstrcmpi(HelpKeyword(help,k),word) && HelpKeywordTopics(help,k,topics,2)==1) {show_topic(topics[0],TRUE,0); return;}
    search(word);
}

/* --- popups --------------------------------------------------------------------------------- */
static HelpFile *popup_file;  /* the popup's file, when it is not the one shown */
static HelpTopic *popup_topic;
static void close_popup(void) {
    if(popup) {HWND w=popup; popup=NULL; ReleaseCapture(); DestroyWindow(w);}
    HelpFreeTopic(popup_topic); popup_topic=NULL;
    free_layout(&popup_layout); device_reset(&popup_device);
    if(popup_file) {HelpClose(popup_file); popup_file=NULL;}
}
static const HelpFile *popup_source(void) {return popup_file?popup_file:help;}
/* Topic n of file (help, or one opened for it) in a window under the point
 * (screen coordinates), as wide as its text needs up to half the screen's. */
static void show_popup(HelpFile *file,DWORD n,POINT at) {
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN),w,h,limit;
    HelpTopic *t;
    if(!file || !(t=HelpReadTopic(file,n))) {if(file && file!=help) HelpClose(file); MessageBeep(0); return;}
    close_popup();
    popup_topic=t; if(file!=help) popup_file=file;
    limit=max(sw/2,240);
    layout_topic(&popup_layout,&popup_device,popup_source(),t,limit);
    w=min(limit,popup_layout.right+px_x(&popup_device,MARGIN_HP))+2;
    layout_topic(&popup_layout,&popup_device,popup_source(),t,w-2);
    h=min(popup_layout.height+2,sh);
    at.x=max(0,min(at.x-w/4,sw-w)); at.y=at.y+4;
    if(at.y+h>sh) at.y=max(0,sh-h);
    popup=CreateWindow("WinHelpPopup",NULL,WS_POPUP|WS_BORDER,at.x,at.y,w,h,main_wnd,NULL,instance,NULL);
    if(!popup) {close_popup(); return;}
    ShowWindow(popup,SW_SHOWNOACTIVATE); UpdateWindow(popup);
    SetCapture(popup);
}

/* --- hotspots and macros ------------------------------------------------------------------- */
static void print_topic(void);
static void jump(LPCSTR file,DWORD target,BOOL in_popup,BOOL hashed,POINT at) {
    /* target: a topic number of the file shown, else (another file's) its target. */
    if(!file || !file[0]) {
        if(in_popup) show_popup(help,target,at); else show_topic(target,TRUE,0);
        return;
    }
    if(in_popup) {
        char path[MAX_PATH]; HelpFile *h;
        if(!find_help(file,path) || !(h=help && !lstrcmpi(path,HelpPath(help))?help:HelpOpen(path))) {MessageBeep(0); return;}
        show_popup(h,hashed?HelpByTarget(h,target):target,at);
        return;
    }
    if(!open_help(file,TRUE)) return;
    {
        DWORD n=hashed?HelpByTarget(help,target):target;
        show_topic(n==NO_TOPIC?contents_topic():n,TRUE,0);
    }
}
/* A macro's arguments: quoted strings ("..." or `...') or plain words. */
static LPCSTR macro_argument(LPCSTR p,char *out,int size) {
    int n=0; char close=0;
    while(*p==' ') p++;
    if(*p=='"') close='"'; else if(*p=='`') close='\'';
    if(close) p++;
    while(*p && (close?*p!=close:*p!=',' && *p!=')')) {if(n<size-1) out[n++]=*p; p++;}
    if(close && *p) p++;
    out[n]=0;
    while(n && !close && out[n-1]==' ') out[--n]=0;
    while(*p==' ') p++;
    if(*p==',') p++;
    return p;
}
/* Macros (Windows 3.1's), the common ones: one or more, by ':' or ';'. */
static void run_macro(LPCSTR macro) {
    LPCSTR p=macro; POINT at;
    GetCursorPos(&at);
    while(*p) {
        char name[32],args[4][128]; int n=0,a=0;
        while(*p==' ' || *p==':' || *p==';') p++;
        if(!*p) break;
        while(*p && *p!='(' && *p!=':' && *p!=';' && *p!=' ') {if(n<31) name[n++]=*p; p++;}
        name[n]=0;
        while(*p==' ') p++;
        memset(args,0,sizeof(args));
        if(*p=='(') {
            p++;
            while(*p && *p!=')') {LPCSTR q=macro_argument(p,a<4?args[a]:args[3],128); a++; if(q==p) break; p=q;}
            if(*p==')') p++;
        }
        if(!lstrcmpi(name,"Contents")) show_contents();
        else if(!lstrcmpi(name,"Back")) go_back();
        else if(!lstrcmpi(name,"Search")) search(NULL);
        else if(!lstrcmpi(name,"Next")) browse(TRUE);
        else if(!lstrcmpi(name,"Prev")) browse(FALSE);
        else if(!lstrcmpi(name,"Exit")) PostMessage(main_wnd,WM_CLOSE,0,0);
        else if(!lstrcmpi(name,"About")) SendMessage(main_wnd,WM_COMMAND,IDM_ABOUT,0);
        else if(!lstrcmpi(name,"PrintTopic")) print_topic();
        else if(!lstrcmpi(name,"JumpContents") || !lstrcmpi(name,"JC")) {if(!args[0][0] || open_help(args[0],TRUE)) show_contents();}
        else if(!lstrcmpi(name,"JumpId") || !lstrcmpi(name,"JI") || !lstrcmpi(name,"PopupId") || !lstrcmpi(name,"PI")) {
            BOOL pop=name[0]=='P' || name[0]=='p'; char path[MAX_PATH]; HelpFile *h=help; DWORD t;
            if(args[0][0] && (!find_help(args[0],path) || (help && !lstrcmpi(path,HelpPath(help))))) args[0][0]=0;
            if(pop && args[0][0]) h=HelpOpen(path);
            else if(args[0][0] && !open_help(path,TRUE)) h=NULL;
            else h=help;
            if(!h || (t=HelpNamed(h,args[1]))==NO_TOPIC) {if(h && h!=help) HelpClose(h); MessageBeep(0);}
            else if(pop) show_popup(h,t,at);
            else show_topic(t,TRUE,0);
        } else if(!lstrcmpi(name,"JumpKeyword") || !lstrcmpi(name,"JK")) {if(!args[0][0] || open_help(args[0],TRUE)) key(args[1]);}
        else if(!lstrcmpi(name,"ExecProgram") || !lstrcmpi(name,"EP")) WinExec(args[0],args[1][0]?(UINT)(args[1][0]-'0'):SW_SHOWNORMAL);
        else MessageBeep(0);
    }
}
static void activate(const HelpTopic *t,int spot,POINT at) {
    const HelpSpot *s;
    if(spot<0 || spot>=t->spot_count) return;
    s=&t->spots[spot];
    if(s->kind==SPOT_MACRO) {char macro[128]; lstrcpyn(macro,s->macro,sizeof(macro)); run_macro(macro); return;}
    if(s->topic==NO_TOPIC && !s->file[0]) {MessageBeep(0); return;}
    {
        char file[80]; DWORD target=s->topic; BOOL in_popup=s->kind==SPOT_POPUP;
        lstrcpyn(file,s->file,sizeof(file));
        jump(file,target,in_popup,file[0]!=0,at);
    }
}

/* --- printing and copying ------------------------------------------------------------------- */
static void print_topic(void) {
    HDC dc; char name[160]; BOOL ok=TRUE; Layout l; int page_h,offset,width,x0,y0; POINT corner;
    if(!topic || !(dc=PrinterDC(main_wnd))) return;
    printer.dc=dc; printer.dpi_x=GetDeviceCaps(dc,LOGPIXELSX); printer.dpi_y=GetDeviceCaps(dc,LOGPIXELSY);
    width=GetDeviceCaps(dc,HORZRES)-printer.dpi_x; page_h=GetDeviceCaps(dc,VERTRES)-printer.dpi_y;
    memset(&l,0,sizeof(l));
    layout_topic(&l,&printer,help,topic,max(width,printer.dpi_x));
    lstrcpyn(name,topic->title[0]?topic->title:FileTitle(HelpPath(help)),sizeof(name));
    if(!PrintStart(dc,main_wnd,"Help",name)) {device_reset(&printer); free_layout(&l); return;}
    if(Escape(dc,GETPRINTINGOFFSET,0,NULL,&corner)<=0) corner.x=corner.y=0;
    x0=printer.dpi_x/2-corner.x; y0=printer.dpi_y/2-corner.y;
    for(offset=0;offset<l.height && ok;) {
        /* The page ends at the last line top that no other line straddles. */
        int cut=offset+page_h,best=-1,i,j; RECT area;
        if(cut<l.height) for(i=0;i<l.count;i++) {
            int c=l.pieces[i].top; BOOL clear=TRUE;
            if(c<=offset || c>cut || c<=best) continue;
            for(j=0;j<l.count && clear;j++) if(l.pieces[j].top<c && l.pieces[j].bottom>c) clear=FALSE;
            if(clear) best=c;
        }
        if(best>offset) cut=best;
        if(StartPage(dc)<=0 || PrintCancelled()) {ok=FALSE; break;}
        SaveDC(dc);
        IntersectClipRect(dc,x0,y0,x0+width,y0+(cut-offset));
        SetRect(&area,x0,y0,x0+width,y0+(cut-offset));
        draw_layout(dc,&printer,help,topic,&l,x0,y0-offset,&area,-1);
        RestoreDC(dc,-1);
        if(EndPage(dc)<=0 || PrintCancelled()) ok=FALSE;
        offset=cut;
    }
    PrintEnd(dc,ok && !PrintCancelled());
    device_reset(&printer); free_layout(&l);
}
/* The topic's text: tabs as tabs, a line break or paragraph's end as CR LF. */
static void copy_topic(void) {
    DWORD size=16,n=0; int i,k; HGLOBAL g; char *out;
    if(!topic) return;
    for(i=0;i<topic->run_count;i++) size+=topic->runs[i].kind==RUN_TEXT?topic->runs[i].length:2;
    size+=(DWORD)topic->para_count*2;
    if(!(g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,size)) || !(out=(char *)GlobalLock(g))) {if(g) GlobalFree(g); return;}
    for(i=0;i<topic->para_count;i++) {
        const HelpPara *p=&topic->paras[i];
        for(k=0;k<p->runs;k++) {
            const HelpRun *r=&topic->runs[p->first_run+k];
            if(r->kind==RUN_TEXT) {memcpy(out+n,topic->text+r->at,r->length); n+=r->length;}
            else if(r->kind==RUN_TAB) out[n++]='\t';
            else if(r->kind==RUN_BREAK) {out[n++]='\r'; out[n++]='\n';}
        }
        out[n++]='\r'; out[n++]='\n';
    }
    out[n]=0;
    GlobalUnlock(g);
    if(OpenClipboard(main_wnd)) {EmptyClipboard(); SetClipboardData(CF_TEXT,g); CloseClipboard();}
    else GlobalFree(g);
}

/* --- requests from programs ------------------------------------------------------------------ */
static void show_main(void) {
    if(IsIconic(main_wnd)) ShowWindow(main_wnd,SW_RESTORE);
    else ShowWindow(main_wnd,SW_SHOWNORMAL);
    BringWindowToTop(main_wnd); SetActiveWindow(main_wnd);
}
/* The windows that asked for help. As in Windows, HELP_QUIT takes the asking
 * one off, and Help ends when no other is left; from one that never asked
 * (Help started by the user, say) it does nothing. */
#define CLIENTS 32
static HWND clients[CLIENTS]; static int client_count;
static void add_client(HWND h) {
    int i;
    for(i=0;i<client_count;i++) if(clients[i]==h) return;
    if(h && client_count<CLIENTS) clients[client_count++]=h;
}
static BOOL remove_client(HWND h) {
    int i,k; BOOL found=FALSE;
    for(i=k=0;i<client_count;i++) {
        if(clients[i]==h) found=TRUE;
        else if(IsWindow(clients[i])) clients[k++]=clients[i];
    }
    client_count=k;
    return found;
}
static void request(const WINHLP *w,HWND owner) {
    const char *base=(const char *)w; LPCSTR file=w->ofsHelpFile?base+w->ofsHelpFile:""; LPCSTR data=w->ofsData?base+w->ofsData:"";
    POINT at; DWORD n;
    if(w->usCommand==HELP_QUIT) {if(remove_client(owner) && !client_count) DestroyWindow(main_wnd); return;}
    add_client(owner);
    switch(w->usCommand) {
    case HELP_HELPONHELP: show_main(); if(open_help("WINHELP.HLP",TRUE)) show_contents(); return;
    }
    if(!file[0] && !help) return;
    if(file[0] && !open_help(file,TRUE)) return;
    show_main();
    switch(w->usCommand) {
    case HELP_CONTEXT:
        if((n=HelpMapped(help,w->ulTopic))==NO_TOPIC) {
            MessageBox(main_wnd,"Help topic does not exist.","Help",MB_OK|MB_ICONEXCLAMATION);
            n=contents_topic();
        }
        show_topic(n,TRUE,0);
        break;
    case HELP_CONTEXTPOPUP:
        if((n=HelpMapped(help,w->ulTopic))==NO_TOPIC) {MessageBeep(0); break;}
        if(!topic) show_contents();
        GetCursorPos(&at); show_popup(help,n,at);
        break;
    case HELP_SETINDEX:
        if((n=HelpMapped(help,w->ulTopic))!=NO_TOPIC) contents=n;
        break;
    case HELP_FORCEFILE:
        if(!topic) show_contents();
        break;
    case HELP_KEY: case HELP_PARTIALKEY: key(data); break;
    case HELP_MULTIKEY: {
        const MULTIKEYHELP *m=(const MULTIKEYHELP *)data;
        if(w->ofsData && (m->mkKeylist=='K' || m->mkKeylist=='k')) key(m->szKeyphrase);
        else show_contents();
        break;
    }
    case HELP_COMMAND: {char macro[256]; lstrcpyn(macro,data,sizeof(macro)); if(!topic) show_contents(); run_macro(macro); break;}
    default: show_contents(); break;
    }
}

/* --- windows --------------------------------------------------------------------------------- */
static void click(int x,int y,BOOL in_popup) {
    POINT at; const Layout *l=in_popup?&popup_layout:&shown; const HelpTopic *t=in_popup?popup_topic:topic;
    int spot=t?spot_at(l,x,in_popup?y:y+scroll_y):-1;
    at.x=x; at.y=y; ClientToScreen(in_popup?popup:view,&at);
    if(spot<0) return;
    if(in_popup) {
        /* A popup's hotspot: the popup goes first. One from another file's
         * topic leads within that file, unless it names another. */
        HelpTopic *keep=popup_topic; HelpFile *file=popup_file; const HelpSpot *s=&keep->spots[spot]; char path[MAX_PATH];
        popup_topic=NULL; popup_file=NULL;
        close_popup();
        if(file && s->kind!=SPOT_MACRO && !s->file[0]) {
            if(s->kind==SPOT_POPUP) show_popup(file,s->topic,at);
            else {
                lstrcpyn(path,HelpPath(file),sizeof(path)); HelpClose(file);
                if(open_help(path,TRUE)) show_topic(s->topic,TRUE,0);
            }
        } else {
            if(file) HelpClose(file);
            activate(keep,spot,at);
        }
        HelpFreeTopic(keep);
        return;
    }
    focus_spot=spot;
    activate(topic,spot,at);
}
/* Tab: the next hotspot shown (Shift+Tab the one before), round to the first. */
static void focus_next(int dir) {
    RECT r; int n,i,s=focus_spot;
    if(!topic || !(n=topic->spot_count)) return;
    for(i=0;i<n;i++) {
        s+=dir;
        if(s<0) s=n-1; else if(s>=n) s=0;
        if(spot_rect(&shown,s,&r)) break;
    }
    if(i==n) return;
    focus_spot=s;
    if(r.top<scroll_y) scroll_to(r.top);
    else if(r.bottom>scroll_y+view_height()) scroll_to(r.bottom-view_height());
    InvalidateRect(view,NULL,TRUE);
}
static LRESULT CALLBACK ViewProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    PAINTSTRUCT ps; int line=px_y(&screen,32);
    switch(msg) {
    case WM_PAINT:
        BeginPaint(h,&ps);
        if(topic) draw_layout(ps.hdc,&screen,help,topic,&shown,0,-scroll_y,&ps.rcPaint,GetFocus()==h?focus_spot:-1);
        EndPaint(h,&ps);
        return 0;
    case WM_SIZE: if(topic) relayout(); return 0;
    case WM_VSCROLL:
        switch(LOWORD(wp)) {
        case SB_LINEUP: scroll_to(scroll_y-line); break;
        case SB_LINEDOWN: scroll_to(scroll_y+line); break;
        case SB_PAGEUP: scroll_to(scroll_y-view_height()+line); break;
        case SB_PAGEDOWN: scroll_to(scroll_y+view_height()-line); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: scroll_to((short)HIWORD(wp)); break;
        case SB_TOP: scroll_to(0); break;
        case SB_BOTTOM: scroll_to(shown.height); break;
        }
        return 0;
    case WM_KEYDOWN:
        if(popup) {close_popup(); return 0;}
        switch(wp) {
        case VK_UP: scroll_to(scroll_y-line); return 0;
        case VK_DOWN: scroll_to(scroll_y+line); return 0;
        case VK_PRIOR: scroll_to(scroll_y-view_height()+line); return 0;
        case VK_NEXT: scroll_to(scroll_y+view_height()-line); return 0;
        case VK_HOME: scroll_to(0); return 0;
        case VK_END: scroll_to(shown.height); return 0;
        case VK_TAB: focus_next(GetKeyState(VK_SHIFT)<0?-1:1); return 0;
        case VK_RETURN:
            if(topic && focus_spot>=0) {RECT r; POINT at; spot_rect(&shown,focus_spot,&r); at.x=r.left; at.y=r.bottom-scroll_y; ClientToScreen(h,&at); activate(topic,focus_spot,at);}
            return 0;
        }
        break;
    case WM_CHAR: case WM_SYSCHAR:
        if(wp=='<' || wp==',') {SendMessage(main_wnd,WM_COMMAND,IDB_PREVIOUS,0); return 0;}
        if(wp=='>' || wp=='.') {SendMessage(main_wnd,WM_COMMAND,IDB_NEXT,0); return 0;}
        break;
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT && topic) {
            POINT p; GetCursorPos(&p); ScreenToClient(h,&p);
            if(spot_at(&shown,p.x,p.y+scroll_y)>=0) {SetCursor(hand); return TRUE;}
        }
        break;
    case WM_LBUTTONDOWN: SetFocus(h); click((short)LOWORD(lp),(short)HIWORD(lp),FALSE); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: if(focus_spot>=0) InvalidateRect(h,NULL,TRUE); break;
    }
    return DefWindowProc(h,msg,wp,lp);
}
static LRESULT CALLBACK PopupProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    PAINTSTRUCT ps;
    switch(msg) {
    case WM_PAINT:
        BeginPaint(h,&ps);
        if(popup_topic) draw_layout(ps.hdc,&popup_device,popup_source(),popup_topic,&popup_layout,0,0,&ps.rcPaint,-1);
        EndPaint(h,&ps);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        POINT p; RECT r; p.x=(short)LOWORD(lp); p.y=(short)HIWORD(lp); GetClientRect(h,&r);
        if(msg==WM_LBUTTONDOWN && PtInRect(&r,p) && spot_at(&popup_layout,p.x,p.y)>=0) click(p.x,p.y,TRUE);
        else close_popup();
        return 0;
    }
    case WM_SETCURSOR: return TRUE;
    case WM_MOUSEMOVE: {
        POINT p; p.x=(short)LOWORD(lp); p.y=(short)HIWORD(lp);
        SetCursor(spot_at(&popup_layout,p.x,p.y)>=0?hand:LoadCursor(NULL,IDC_ARROW));
        return 0;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}
static int bar_height;
static void place_children(void) {
    RECT r; int i,x=4,gap=4; HDC dc=GetDC(main_wnd); HFONT old=(HFONT)SelectObject(dc,GetStockObject(SYSTEM_FONT)); TEXTMETRIC tm;
    GetTextMetrics(dc,&tm);
    bar_height=tm.tmHeight+16;
    GetClientRect(main_wnd,&r);
    for(i=0;i<5;i++) {
        char text[32]; SIZE s; int w;
        GetWindowText(buttons[i],text,sizeof(text));
        GetTextExtentPoint(dc,text,lstrlen(text),&s);
        w=max(s.cx+tm.tmAveCharWidth*3,tm.tmAveCharWidth*8);
        MoveWindow(buttons[i],x,4,w,bar_height-8,TRUE);
        x+=w+gap;
    }
    SelectObject(dc,old); ReleaseDC(main_wnd,dc);
    MoveWindow(view,0,bar_height+1,r.right,max(0,r.bottom-bar_height-1),TRUE);
}
static INT_PTR CALLBACK AboutProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) return TRUE;
    if(msg==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)) {EndDialog(dlg,0); return TRUE;}
    return FALSE;
}
static void command(int id) {
    char path[MAX_PATH];
    switch(id) {
    case IDM_OPEN:
        lstrcpy(path,"*.HLP");
        if(FileOpenDialog(main_wnd,"Open",path,path,sizeof(path)) && open_help(path,FALSE)) {history_count=0; show_contents();}
        break;
    case IDM_PRINT: print_topic(); break;
    case IDM_PRINTSETUP: PrinterSetup(main_wnd); break;
    case IDM_EXIT: PostMessage(main_wnd,WM_CLOSE,0,0); break;
    case IDM_COPY: copy_topic(); break;
    case IDM_DEFINE:
        if(topic && DialogBox(instance,"BOOKMARK",main_wnd,BookmarkProc)>=0) build_bookmarks();
        break;
    case IDM_HELPONHELP:
        if(find_help("WINHELP.HLP",path) && open_help(path,TRUE)) show_contents();
        else MessageBox(main_wnd,"Help on using Help (WINHELP.HLP) is not installed.","Help",MB_OK|MB_ICONINFORMATION);
        break;
    case IDM_ABOUT: DialogBox(instance,"ABOUT",main_wnd,AboutProc); break;
    case IDB_INDEX: show_contents(); break;
    case IDB_BACK: go_back(); break;
    case IDB_PREVIOUS: browse(FALSE); break;
    case IDB_NEXT: browse(TRUE); break;
    case IDB_SEARCH: search(NULL); break;
    default:
        if(id>=IDM_BOOKMARK && id<IDM_BOOKMARK+BOOKMARKS) go_bookmark(id-IDM_BOOKMARK);
    }
    if(id>=IDB_INDEX && id<=IDB_SEARCH && IsWindow(view)) SetFocus(view);
}
static LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    static const char *labels[5]={"&Index","&Back","Browse &<<","Browse &>>","&Search"};
    int i;
    if(msg==help_message && help_message) {
        /* The block is the sender's: a copy, done when it has gone on. */
        const WINHLP *w=(const WINHLP *)GlobalLock((HGLOBAL)lp); HGLOBAL copy;
        if(w && w->cbData>=sizeof(WINHLP) && (copy=GlobalAlloc(GMEM_MOVEABLE,w->cbData))!=NULL) {
            memcpy(GlobalLock(copy),w,w->cbData); GlobalUnlock(copy);
            PostMessage(h,WM_REQUEST,wp,(LPARAM)copy);
        }
        if(w) GlobalUnlock((HGLOBAL)lp);
        return TRUE;
    }
    switch(msg) {
    case WM_CREATE:
        main_wnd=h;
        for(i=0;i<5;i++) buttons[i]=CreateWindow("BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,10,10,h,(HMENU)(INT_PTR)(IDB_INDEX+i),instance,NULL);
        view=CreateWindow("WinHelpView",NULL,WS_CHILD|WS_VISIBLE|WS_VSCROLL,0,0,10,10,h,NULL,instance,NULL);
        place_children(); update_buttons();
        return 0;
    case WM_SIZE: place_children(); return 0;
    case WM_SETFOCUS: SetFocus(view); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT r;
        BeginPaint(h,&ps);
        GetClientRect(h,&r); r.top=bar_height; r.bottom=bar_height+1;
        FillRect(ps.hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h,&ps);
        return 0;
    }
    case WM_COMMAND: command(LOWORD(wp)); return 0;
    case WM_INITMENU:
        EnableMenuItem((HMENU)wp,IDM_PRINT,MF_BYCOMMAND|(topic?MF_ENABLED:MF_GRAYED));
        EnableMenuItem((HMENU)wp,IDM_COPY,MF_BYCOMMAND|(topic?MF_ENABLED:MF_GRAYED));
        EnableMenuItem((HMENU)wp,IDM_DEFINE,MF_BYCOMMAND|(topic?MF_ENABLED:MF_GRAYED));
        return 0;
    case WM_REQUEST: {
            WINHLP *w=(WINHLP *)GlobalLock((HGLOBAL)lp);
            if(w) {request(w,(HWND)wp); GlobalUnlock((HGLOBAL)lp);}
            GlobalFree((HGLOBAL)lp);
        return 0;
    }
    case WM_DESTROY:
        close_popup();
        HelpFreeTopic(topic); topic=NULL; HelpClose(help); help=NULL;
        free_layout(&shown); device_reset(&screen);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; HDC dc;
    instance=inst;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"WINHELP"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.lpszMenuName="WINHELP"; wc.lpszClassName="MS_WINHELP";
        if(!RegisterClass(&wc)) return 0;
        wc.lpfnWndProc=ViewProc; wc.hIcon=NULL; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName=NULL; wc.lpszClassName="WinHelpView";
        if(!RegisterClass(&wc)) return 0;
        wc.lpfnWndProc=PopupProc; wc.lpszClassName="WinHelpPopup";
        if(!RegisterClass(&wc)) return 0;
    }
    hand=LoadCursor(inst,"HAND");
    help_message=RegisterWindowMessage("WM_WINHELP");
    screen.dc=CreateCompatibleDC(NULL);
    dc=GetDC(NULL); screen.dpi_x=GetDeviceCaps(dc,LOGPIXELSX); screen.dpi_y=GetDeviceCaps(dc,LOGPIXELSY); ReleaseDC(NULL,dc);
    popup_device=screen;
    if(!CreateWindow("MS_WINHELP","Help",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL)) return 0;
    set_title();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    while(command_line && *command_line==' ') command_line++;
    if(command_line && *command_line && open_help(command_line,FALSE)) show_contents();
    accel=LoadAccelerators(inst,"WINHELP");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    DeleteDC(screen.dc);
    return (int)m.wParam;
}
