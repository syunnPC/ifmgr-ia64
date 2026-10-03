/* SPDX-License-Identifier: GPL-2.0-or-later
 * Raster fonts from Wine (vendor/wine-fonts: System, Fixedsys, Courier,
 * MS Sans Serif, Small Fonts) and public-domain X.Org misc-fixed (Terminal
 * in the OEM code page and extra Courier sizes). Helv and unavailable
 * families, including Tms Rmn, map to MS Sans Serif.
 *
 * Choose the largest height not exceeding the request, including integer
 * scales up to 8x; negative height excludes internal leading. Preserve
 * aspect ratio unless width is specified. Missing bold uses double drawing;
 * missing italic slants rows. Heights, metrics and extents use DC logical
 * units; scaling mapping modes trigger font matching for that DC.
 */
#include "gdip.h"
#include "fontfile.h"
#include "font_6x10.h"
#include "font_9x15.h"
#include "font_9x15B.h"
#include "font_terminal.h"
#include "font_system.h"
#include "font_fixedsys.h"
#include "font_courier.h"
#include "font_sans13.h"
#include "font_sans16.h"
#include "font_sans20.h"
#include "font_small.h"

#define FACE(n,N,bold,prop,charset) {&n##_bits[0][0],n##_widths,N##_HEIGHT,N##_ASCENT,N##_LEADING,N##_AVG,N##_MAX,bold,prop,charset}
enum {F_SYSTEM,F_FIXEDSYS,F_TERMINAL,F_COURIER10,F_COURIER13,F_COURIER15,F_COURIER15B,F_SANS13,F_SANS16,F_SANS20,F_SMALL,FACES};
static const FontFace faces[FACES]={
    FACE(font_system,FONT_SYSTEM,TRUE,TRUE,ANSI_CHARSET),
    FACE(font_fixedsys,FONT_FIXEDSYS,FALSE,FALSE,ANSI_CHARSET),
    FACE(font_terminal,FONT_TERMINAL,FALSE,FALSE,OEM_CHARSET),
    FACE(font_6x10,FONT_6X10,FALSE,FALSE,ANSI_CHARSET),
    FACE(font_courier,FONT_COURIER,FALSE,FALSE,ANSI_CHARSET),
    FACE(font_9x15,FONT_9X15,FALSE,FALSE,ANSI_CHARSET),
    FACE(font_9x15b,FONT_9X15B,TRUE,FALSE,ANSI_CHARSET),
    FACE(font_sans13,FONT_SANS13,FALSE,TRUE,ANSI_CHARSET),
    FACE(font_sans16,FONT_SANS16,FALSE,TRUE,ANSI_CHARSET),
    FACE(font_sans20,FONT_SANS20,FALSE,TRUE,ANSI_CHARSET),
    FACE(font_small,FONT_SMALL,FALSE,TRUE,ANSI_CHARSET),
};
/* A name and its sizes (and bold faces), smallest first. */
typedef struct {const char *name; BYTE family; const FontFace *const *sizes;} FaceName;
#define B(f) &faces[f]
static const FontFace *const system_sizes[]={B(F_SYSTEM),NULL},*const fixedsys_sizes[]={B(F_FIXEDSYS),NULL};
static const FontFace *const terminal_sizes[]={B(F_TERMINAL),NULL},*const small_sizes[]={B(F_SMALL),NULL};
static const FontFace *const courier_sizes[]={B(F_COURIER10),B(F_COURIER13),B(F_COURIER15),B(F_COURIER15B),NULL};
static const FontFace *const sans_sizes[]={B(F_SANS13),B(F_SANS16),B(F_SANS20),NULL};
#undef B
static const FaceName builtin[]={
    {"System",FF_SWISS|VARIABLE_PITCH,system_sizes},{"Fixedsys",FF_MODERN|FIXED_PITCH,fixedsys_sizes},
    {"Terminal",FF_MODERN|FIXED_PITCH,terminal_sizes},{"Courier",FF_MODERN|FIXED_PITCH,courier_sizes},
    {"Helv",FF_SWISS|VARIABLE_PITCH,sans_sizes},{"MS Sans Serif",FF_SWISS|VARIABLE_PITCH,sans_sizes},
    {"Tms Rmn",FF_ROMAN|VARIABLE_PITCH,sans_sizes},{"MS Serif",FF_ROMAN|VARIABLE_PITCH,sans_sizes},
    {"Small Fonts",FF_SWISS|VARIABLE_PITCH,small_sizes},
    {"Modern",FF_MODERN|VARIABLE_PITCH,sans_sizes},{"Roman",FF_ROMAN|VARIABLE_PITCH,sans_sizes},
    {"Script",FF_SCRIPT|VARIABLE_PITCH,sans_sizes},{"Symbol",FF_DECORATIVE|VARIABLE_PITCH,sans_sizes},
};
#define BUILTIN ((int)(sizeof(builtin)/sizeof(builtin[0])))

/* --- font files ------------------------------------------------------------------------------- */
/* Fonts added from files (AddFontResource), a file at a time. A removed
 * file's names go, but its faces stay until GDI ends: fonts made from them
 * may still be selected. */
#define FILE_FONTS 32
#define LOADED_NAMES 64
#define NAME_SIZES 32
typedef struct FontFileEntry {
    struct FontFileEntry *next; char path[MAX_PATH]; int refs,count;
    FileFont fonts[FILE_FONTS]; FontFace faces[FILE_FONTS];
} FontFileEntry;
static FontFileEntry *font_files;
static FaceName loaded[LOADED_NAMES]; static int loaded_count;
static char loaded_text[LOADED_NAMES][LF_FACESIZE];
static const FontFace *loaded_sizes[LOADED_NAMES][NAME_SIZES+1];
/* The names of the files present, each size in height order. */
static void index_files(void) {
    FontFileEntry *e; int i,k,j,n;
    loaded_count=0;
    for(e=font_files;e;e=e->next) if(e->refs) for(i=0;i<e->count;i++) {
        const FileFont *f=&e->fonts[i]; const FontFace **sizes;
        for(k=0;k<loaded_count && lstrcmpi(loaded_text[k],f->name);k++) {}
        if(k==loaded_count) {
            if(loaded_count==LOADED_NAMES) continue;
            lstrcpyn(loaded_text[k],f->name,LF_FACESIZE);
            loaded[k].name=loaded_text[k]; loaded[k].family=f->family; loaded[k].sizes=loaded_sizes[k];
            loaded_sizes[k][0]=NULL; loaded_count++;
        }
        sizes=loaded_sizes[k];
        for(n=0;sizes[n];n++) {}
        if(n==NAME_SIZES) continue;
        for(j=n;j>0 && sizes[j-1]->height>e->faces[i].height;j--) sizes[j]=sizes[j-1];
        sizes[j]=&e->faces[i]; sizes[n+1]=NULL;
    }
}
static void free_file(FontFileEntry *e) {
    int i;
    for(i=0;i<e->count;i++) if(e->fonts[i].bits) gdi_free(e->fonts[i].bits);
    gdi_free(e);
}
/* A file's fonts, read once however often it is added. */
int WINAPI AddFontResource(LPCSTR file) {
    OFSTRUCT of; HFILE h; LONG size; BYTE *data; FontFileEntry *e; int i;
    if(!file || OpenFile(file,&of,OF_EXIST)==HFILE_ERROR) return 0;
    for(e=font_files;e;e=e->next) if(!lstrcmpi(e->path,of.szPathName) && e->refs) {e->refs++; return e->count;}
    if((h=_lopen(of.szPathName,OF_READ))==HFILE_ERROR) return 0;
    size=_llseek(h,0,2); _llseek(h,0,0);
    data=size>0 && size<0x400000?(BYTE *)gdi_alloc((DWORD)size):NULL;
    if(!data || _lread(h,data,(UINT)size)!=(UINT)size || !(e=(FontFileEntry *)gdi_alloc(sizeof(FontFileEntry)))) {
        _lclose(h); if(data) gdi_free(data); return 0;
    }
    _lclose(h);
    e->count=ReadFontFile(data,(DWORD)size,e->fonts,FILE_FONTS,gdi_alloc);
    gdi_free(data);
    if(!e->count) {free_file(e); return 0;}
    for(i=0;i<e->count;i++) {
        const FileFont *f=&e->fonts[i]; FontFace *c=&e->faces[i];
        c->bits=f->bits; c->widths=f->widths; c->height=f->height; c->ascent=f->ascent; c->leading=f->leading;
        c->avg=f->avg; c->max=f->max; c->bold=f->bold; c->proportional=f->proportional; c->charset=f->charset; c->words=f->words;
    }
    lstrcpyn(e->path,of.szPathName,sizeof(e->path)); e->refs=1;
    e->next=font_files; font_files=e;
    index_files();
    return e->count;
}
/* A path's file name. */
static LPCSTR file_title(LPCSTR path) {
    LPCSTR t=path;
    for(;*path;path++) if(*path=='\\' || *path==':' || *path=='/') t=path+1;
    return t;
}
/* The file added under that name: found as AddFontResource found it, else
 * by its file name alone (it may have gone from where it was). */
BOOL WINAPI RemoveFontResource(LPCSTR file) {
    OFSTRUCT of; FontFileEntry *e; BOOL found;
    if(!file) return FALSE;
    found=OpenFile(file,&of,OF_EXIST)!=HFILE_ERROR;
    for(e=font_files;e;e=e->next) if(e->refs && (found?!lstrcmpi(e->path,of.szPathName):!lstrcmpi(file_title(e->path),file_title(file)))) {
        if(!--e->refs) index_files();
        return TRUE;
    }
    return FALSE;
}
/* WIN.INI [fonts]: the files each entry names, added when GDI starts. */
void FontsInit(void) {
    char keys[2048],file[MAX_PATH]; const char *k;
    int n=GetProfileString("fonts",NULL,"",keys,sizeof(keys)-1);
    keys[n]=keys[n+1]=0;
    for(k=keys;*k;k+=lstrlen(k)+1) if(GetProfileString("fonts",k,"",file,sizeof(file))) AddFontResource(file);
}
void FontsEnd(void) {
    while(font_files) {FontFileEntry *e=font_files; font_files=e->next; free_file(e);}
    loaded_count=0;
}

/* --- matching --------------------------------------------------------------------------------- */
/* A name: one from a file before a built-in one. */
static const FaceName *named(const char *face) {
    int i;
    for(i=0;i<loaded_count;i++) if(!lstrcmpi(face,loaded[i].name)) return &loaded[i];
    for(i=0;i<BUILTIN;i++) if(!lstrcmpi(face,builtin[i].name)) return &builtin[i];
    return NULL;
}
/* Unknown faces are chosen by family and pitch. */
static const FaceName *choose_name(const LOGFONT *l) {
    const FaceName *n=l->lfFaceName[0]?named(l->lfFaceName):NULL;
    if(n) return n;
    if(l->lfCharSet==SYMBOL_CHARSET) return named("Symbol");
    if(l->lfCharSet==OEM_CHARSET) return named("Terminal");
    switch(l->lfPitchAndFamily&0xf0) {
    case FF_MODERN: return named("Courier");
    case FF_ROMAN: return named("Tms Rmn");
    case FF_SWISS: return named("Helv");
    case FF_SCRIPT: return named("Script");
    case FF_DECORATIVE: return named("Symbol");
    }
    if((l->lfPitchAndFamily&3)==FIXED_PITCH) return named("Courier");
    return named(l->lfFaceName[0]?"Helv":"System");
}
/* The largest size not taller than asked (cell height, or character height
 * for a negative height), a face at a whole multiple counting as that
 * multiple of its size and a smaller multiple winning a tie, else the
 * smallest; no height: 13 pixels. Of a size, the bold face when bold is
 * asked and there is one, else the other. The width is the face's at the
 * same multiple, or the multiple of its average width nearest a width
 * asked for. */
#define MAX_SCALE 8
static void match(Font *f) {
    const FaceName *n=choose_name(&f->log); int height=f->log.lfHeight,k,scale,want=height<0?-height:height?height:13;
    BOOL bold=f->log.lfWeight>=FW_SEMIBOLD; const FontFace *best=NULL,*c; int best_size=0,best_scale=1;
    lstrcpy(f->name,n->name); f->family=n->family;
    for(k=0;(c=n->sizes[k])!=NULL;k++) {
        int size=height<0?c->height-c->leading:c->height;
        for(scale=1;scale<=MAX_SCALE;scale++) {
            int scaled=size*scale;
            if(scaled>want) break;
            if(!best || scaled>best_size || (scaled==best_size && (scale<best_scale ||
               (scale==best_scale && (c->height>best->height || (c->height==best->height && c->bold==bold)))))) {
                best=c; best_size=scaled; best_scale=scale;
            }
        }
    }
    if(!best) /* Nothing small enough: the smallest, bold as asked if it can be. */
        for(k=0;(c=n->sizes[k])!=NULL;k++) {
            if(best && c->height>best->height) break;
            if(!best || c->bold==bold) best=c;
        }
    f->face=best; f->sy=best_scale; f->sx=best_scale;
    if(f->log.lfWidth>0 && best->avg>0) {
        f->sx=(f->log.lfWidth+best->avg/2)/best->avg;
        if(f->sx<1) f->sx=1;
        if(f->sx>MAX_SCALE) f->sx=MAX_SCALE;
    }
    f->charset=(BYTE)(n->family==(FF_DECORATIVE|VARIABLE_PITCH) && n->sizes==sans_sizes?SYMBOL_CHARSET:best->charset);
    f->embolden=bold && !best->bold;
    f->italic=f->log.lfItalic!=0;
}
HFONT WINAPI CreateFontIndirect(const LOGFONT FAR *l) {
    Font *f; HFONT h;
    if(!l) return NULL;
    h=(HFONT)make_object(OBJ_FONT,sizeof(Font),(void **)&f);
    if(!h) return NULL;
    f->log=*l; f->log.lfFaceName[LF_FACESIZE-1]=0;
    if(!f->log.lfWeight) f->log.lfWeight=FW_NORMAL;
    match(f);
    return h;
}
HFONT WINAPI CreateFont(int height,int width,int escapement,int orientation,int weight,BYTE italic,BYTE underline,BYTE strikeout,
                        BYTE charset,BYTE out_precision,BYTE clip_precision,BYTE quality,BYTE pitch_family,LPCSTR face) {
    LOGFONT l;
    memset(&l,0,sizeof(l));
    l.lfHeight=height; l.lfWidth=width; l.lfEscapement=escapement; l.lfOrientation=orientation; l.lfWeight=weight;
    l.lfItalic=italic; l.lfUnderline=underline; l.lfStrikeOut=strikeout; l.lfCharSet=charset;
    l.lfOutPrecision=out_precision; l.lfClipPrecision=clip_precision; l.lfQuality=quality; l.lfPitchAndFamily=pitch_family;
    if(face) {int i; for(i=0;i<LF_FACESIZE-1 && face[i];i++) l.lfFaceName[i]=face[i];}
    return CreateFontIndirect(&l);
}
static Font *font_of(DC *dc,Font *scaled) {
    Font *f=(Font *)object(dc->s.font,OBJ_FONT);
    if(!f) f=(Font *)object(stock[SYSTEM_FONT],OBJ_FONT);
    if(dc->s.viewport_ext.y==dc->s.window_ext.y || !f->log.lfHeight) return f;
    *scaled=*f;
    scaled->log.lfHeight=f->log.lfHeight<0?-dev_h(dc,-f->log.lfHeight):dev_h(dc,f->log.lfHeight);
    if(!scaled->log.lfHeight) scaled->log.lfHeight=1;
    match(scaled);
    return scaled;
}
int WINAPI GetTextFace(HDC h,int count,LPSTR out) {
    DC *dc=dc_of(h); Font *f; int n; Font scaled;
    if(!dc || !out || count<=0) return 0;
    f=font_of(dc,&scaled); n=lstrlen(f->name);
    if(n>count-1) n=count-1;
    memcpy(out,f->name,(size_t)n); out[n]=0;
    return n;
}
static void metrics(const DC *dc,const Font *f,LPTEXTMETRIC tm) {
    memset(tm,0,sizeof(*tm));
    tm->tmHeight=log_h(dc,f->face->height*f->sy); tm->tmAscent=log_h(dc,f->face->ascent*f->sy);
    tm->tmDescent=tm->tmHeight-tm->tmAscent; tm->tmInternalLeading=log_h(dc,f->face->leading*f->sy);
    tm->tmAveCharWidth=log_w(dc,f->face->avg*f->sx); tm->tmMaxCharWidth=log_w(dc,f->face->max*f->sx);
    tm->tmWeight=f->face->bold || f->embolden?FW_BOLD:FW_NORMAL;
    tm->tmDigitizedAspectX=tm->tmDigitizedAspectY=96;
    tm->tmFirstChar=0x20; tm->tmLastChar=0xff; tm->tmDefaultChar=0x80; tm->tmBreakChar=0x20;
    tm->tmItalic=(BYTE)f->italic; tm->tmUnderlined=f->log.lfUnderline; tm->tmStruckOut=f->log.lfStrikeOut;
    /* TMPF_FIXED_PITCH is set for variable-pitch fonts. */
    tm->tmPitchAndFamily=(BYTE)((f->family&0xf0)|(f->face->proportional?TMPF_FIXED_PITCH:0)); tm->tmCharSet=f->charset;
}
BOOL WINAPI GetTextMetrics(HDC h,LPTEXTMETRIC tm) {
    DC *dc=dc_of(h); Font scaled;
    if(!dc || !tm) return FALSE;
    metrics(dc,font_of(dc,&scaled),tm);
    return TRUE;
}
/* EnumFonts: without a face name, each name once (its first size); with
 * one, each of its sizes (a bold face only when there is no other of its
 * height). Names from files come first. */
static int enum_name(DC *dc,const FaceName *n,BOOL all,FONTENUMPROC proc,LPARAM lp) {
    int k,r=1; const FontFace *c;
    for(k=0;(c=n->sizes[k])!=NULL;k++) {
        Font f; TEXTMETRIC tm;
        if(c->bold && ((k && n->sizes[k-1]->height==c->height) || (n->sizes[k+1] && n->sizes[k+1]->height==c->height))) continue;
        memset(&f,0,sizeof(f));
        lstrcpy(f.log.lfFaceName,n->name); f.log.lfHeight=c->height;
        f.log.lfWeight=c->bold?FW_BOLD:FW_NORMAL; match(&f);
        f.log.lfHeight=log_h(dc,f.face->height*f.sy); f.log.lfWidth=log_w(dc,f.face->avg*f.sx);
        f.log.lfCharSet=f.charset; f.log.lfPitchAndFamily=(BYTE)((f.family&0xf0)|(f.face->proportional?VARIABLE_PITCH:FIXED_PITCH));
        metrics(dc,&f,&tm);
        if(!(r=proc(&f.log,&tm,RASTER_FONTTYPE,lp))) return 0;
        if(!all) break;
    }
    return r;
}
int WINAPI EnumFonts(HDC h,LPCSTR face,FONTENUMPROC proc,LPARAM lp) {
    DC *dc=dc_of(h); int i,r=0;
    if(!dc || !proc) return 0;
    if(face) {const FaceName *n=named(face); return n?enum_name(dc,n,TRUE,proc,lp):0;}
    for(i=0;i<loaded_count;i++) if(!(r=enum_name(dc,&loaded[i],FALSE,proc,lp))) return 0;
    for(i=0;i<BUILTIN;i++) {
        int k;
        for(k=0;k<loaded_count && lstrcmpi(loaded[k].name,builtin[i].name);k++) {}
        if(k==loaded_count && !(r=enum_name(dc,&builtin[i],FALSE,proc,lp))) return 0;
    }
    return r;
}
/* Windows 3.1's EnumFontFamilies: EnumFonts's fonts, each given as an
 * ENUMLOGFONT with its full name and style (a raster font's TEXTMETRIC as
 * it is). */
typedef struct {FONTENUMPROC proc; LPARAM lp;} Families;
static int CALLBACK family_font(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    const Families *f=(const Families *)lp; ENUMLOGFONT e;
    memset(&e,0,sizeof(e)); e.elfLogFont=*lf;
    lstrcpyn(e.elfFullName,lf->lfFaceName,LF_FULLFACESIZE);
    lstrcpy(e.elfStyle,lf->lfWeight>=FW_BOLD?(lf->lfItalic?"Bold Italic":"Bold"):lf->lfItalic?"Italic":"Regular");
    return f->proc(&e.elfLogFont,tm,type,f->lp);
}
int WINAPI EnumFontFamilies(HDC h,LPCSTR family,FONTENUMPROC proc,LPARAM lp) {
    Families f;
    if(!proc) return 0;
    f.proc=proc; f.lp=lp;
    return EnumFonts(h,family,family_font,(LPARAM)&f);
}
BOOL WINAPI GetCharWidth(HDC h,UINT first,UINT last,LPINT widths) {
    DC *dc=dc_of(h); UINT c; Font scaled;
    if(!dc || !widths || first>last) return FALSE;
    for(c=first;c<=last;c++) {const Font *f=font_of(dc,&scaled); widths[c-first]=log_w(dc,f->face->widths[c&0xff]*f->sx);}
    return TRUE;
}
int WINAPI GetTextCharacterExtra(HDC h) {DC *dc=dc_of(h); return dc?dc->s.extra:0;}
/* The advance after character i: the spacing given (logical), or the cell
 * width, the character extra, and the share of SetTextJustification's
 * extra space that falls to a break character. */
static int advance(const DC *dc,const Font *f,LPCSTR text,UINT i,const int *dx,int *breaks) {
    int a;
    if(dx) return dev_w(dc,dx[i]);
    a=f->face->widths[(BYTE)text[i]]*f->sx+dev_w(dc,dc->s.extra);
    if(dc->s.break_count>0 && (BYTE)text[i]==' ') {
        int extra=dev_w(dc,dc->s.break_extra);
        a+=extra/dc->s.break_count+((*breaks)++<extra%dc->s.break_count?1:0);
    }
    return a;
}
static int text_width(DC *dc,const Font *f,LPCSTR text,UINT count,const int *dx) {
    int width=0,breaks=0; UINT i;
    for(i=0;i<count;i++) width+=advance(dc,f,text,i,dx,&breaks);
    return width;
}
int WINAPI SetTextJustification(HDC h,int extra,int count) {
    DC *dc=dc_of(h);
    if(!dc) return 0;
    if(dc->meta) mf_words(dc,META_SETTEXTJUSTIFICATION,2,count,extra);
    dc->s.break_extra=count>0?extra:0; dc->s.break_count=count>0?count:0;
    return 1;
}
BOOL WINAPI GetTextExtentPoint(HDC h,LPCSTR text,int count,LPSIZE size) {
    DC *dc=dc_of(h); Font *f; Font scaled;
    if(!dc || !size) return FALSE;
    f=font_of(dc,&scaled);
    if(count<0) count=text?lstrlen(text):0;
    size->cx=log_w(dc,text_width(dc,f,text,(UINT)count,NULL)); size->cy=log_h(dc,f->face->height*f->sy);
    return TRUE;
}
DWORD WINAPI GetTextExtent(HDC h,LPCSTR text,int count) {
    SIZE s;
    if(!GetTextExtentPoint(h,text,count,&s)) return 0;
    return MAKELONG(s.cx,s.cy);
}
/* Glyph pixels inside one clipping rectangle, each a block of the font's
 * multiples; bold one pixel wider, italic slanting a pixel every four rows. */
static void glyphs(DC *dc,const Font *f,const RECT *c,int x,int y,LPCSTR text,UINT count,const int *dx,DWORD fg) {
    const FontFace *face=f->face; Surface *s=dc->surface; UINT i; int row,col,word,breaks=0,words=face->words?face->words:1;
    int sx=f->sx,sy=f->sy,height=face->height*sy;
    for(i=0;i<count;i++) {
        const unsigned int *glyph=face->bits+(ULONG_PTR)(BYTE)text[i]*face->height*words;
        if(x<c->right && x+face->max*sx+height/4+2>c->left)
            for(row=0;row<height;row++) {
                int py=y+row,shift=f->italic?(height-1-row)/4:0;
                if(py<c->top || py>=c->bottom) continue;
                for(word=0;word<words;word++) {
                    unsigned int bits=glyph[row/sy*words+word];
                    for(col=word*32;bits;col++,bits<<=1)
                        if(bits&0x80000000U) {
                            int px=x+col*sx+shift,end=px+sx+(f->embolden?1:0);
                            if(px<c->left) px=c->left;
                            if(end>c->right) end=c->right;
                            for(;px<end;px++) s->bits[(ULONG_PTR)py*s->stride+px]=fg;
                        }
                }
            }
        x+=advance(dc,f,text,i,dx,&breaks);
    }
}
BOOL WINAPI ExtTextOut(HDC h,int x,int y,UINT options,LPCRECT rect,LPCSTR text,UINT count,LPINT dx) {
    DC *dc=dc_of(h); Font *f,scaled; int width,i; RECT box,c; RectList saved; BOOL clipped=FALSE; DWORD fg,bg;
    if(!dc) return FALSE;
    if(!text) count=0;
    if(dc->meta) return mf_text(dc,x,y,options,rect,text,count,dx,TRUE);
    f=font_of(dc,&scaled);
    width=text_width(dc,f,text,count,dx);
    if(dc->s.align&TA_UPDATECP) {x=dc->s.pos.x; y=dc->s.pos.y;}
    if(dc->s.align&TA_UPDATECP) dc->s.pos.x+=(dc->s.align&TA_RIGHT)?-log_w(dc,width):log_w(dc,width);
    x=dev_x(dc,x); y=dev_y(dc,y);
    if((dc->s.align&TA_CENTER)==TA_CENTER) x-=width/2;
    else if(dc->s.align&TA_RIGHT) x-=width;
    if((dc->s.align&TA_BASELINE)==TA_BASELINE) y-=f->face->ascent*f->sy;
    else if(dc->s.align&TA_BOTTOM) y-=f->face->height*f->sy;
    fg=dc_pixel(dc,dc->s.text); bg=dc_pixel(dc,dc->s.bk);
    if(dc->surface->mono) fg=to_mono(fg);
    if(rect && (options&(ETO_OPAQUE|ETO_CLIPPED))) {
        RECT r=*rect; RectList one;
        dev_rect(dc,&r);
        if(options&ETO_OPAQUE) fill_solid(dc,&r,bg,R2_COPYPEN);
        if(options&ETO_CLIPPED) {
            saved=dc->eff; memset(&dc->eff,0,sizeof(dc->eff)); memset(&one,0,sizeof(one));
            rl_set(&one,&r); rl_intersect(&dc->eff,&saved,&one); rl_free(&one);
            clipped=TRUE;
        }
    }
    if(count) {
        int height=f->face->height*f->sy,ascent=f->face->ascent*f->sy;
        r_set(&box,x,y,x+width,y+height);
        if(dc->s.bk_mode==OPAQUE) fill_solid(dc,&box,bg,R2_COPYPEN);
        box.right+=height/4+1;
        for(i=0;i<dc->eff.count;i++) if(r_intersect(&c,&box,&dc->eff.rects[i])) {
            glyphs(dc,f,&c,x,y,text,count,dx,fg);
            if(dc->display) mark(&c);
        }
        if(f->log.lfUnderline || f->log.lfStrikeOut) {
            int under=min(ascent+f->sy,height-f->sy),strike=ascent-ascent/3;
            if(f->log.lfUnderline) {r_set(&box,x,y+under,x+width,y+under+f->sy); fill_solid(dc,&box,fg,R2_COPYPEN);}
            if(f->log.lfStrikeOut) {r_set(&box,x,y+strike,x+width,y+strike+f->sy); fill_solid(dc,&box,fg,R2_COPYPEN);}
        }
    }
    if(clipped) {rl_free(&dc->eff); dc->eff=saved;}
    return TRUE;
}
BOOL WINAPI TextOut(HDC h,int x,int y,LPCSTR text,int count) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return mf_text(dc,x,y,0,NULL,text?text:"",text && count>0?(UINT)count:0,NULL,FALSE);
    return ExtTextOut(h,x,y,0,NULL,text,count<0?0:(UINT)count,NULL);
}
