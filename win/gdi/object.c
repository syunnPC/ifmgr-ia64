/* SPDX-License-Identifier: GPL-2.0-or-later
 * GDI objects and device contexts: handles, stock objects, pens, brushes,
 * regions, palettes, DCs, their attributes and clipping. The display has
 * no hardware palette: a palette maps PALETTEINDEX colors of the DCs it is
 * selected into, and realizing it changes nothing.
 */
#include "gdip.h"
/* Handles are 16-bit values, as Win16 programs see them (USER's and
 * KERNEL's moveable memory below). */
#define OBJECTS 8191
#define HANDLE_BASE 0x8004

static void *objects[OBJECTS];
HGDIOBJ stock[18];
static HBITMAP default_bitmap;

void *gdi_alloc(DWORD bytes) {return GlobalAlloc(GMEM_FIXED|GMEM_ZEROINIT|GMEM_SHARE,bytes);}
void gdi_free(void *p) {if(p) GlobalFree(p);}
static HGDIOBJ new_handle(void *o) {
    int i;
    for(i=0;i<OBJECTS;i++) if(!objects[i]) {objects[i]=o; return (HGDIOBJ)(ULONG_PTR)(HANDLE_BASE+i*4);}
    return NULL;
}
void *object(HGDIOBJ handle,WORD type) {
    ULONG_PTR v=(ULONG_PTR)handle; ObjHeader *o;
    if(v<HANDLE_BASE || (v-HANDLE_BASE)%4 || (v-HANDLE_BASE)/4>=OBJECTS) return NULL;
    o=(ObjHeader *)objects[(v-HANDLE_BASE)/4];
    return o && (!type || o->type==type)?o:NULL;
}
HGDIOBJ make_object(WORD type,UINT bytes,void **out) {
    ObjHeader *o=(ObjHeader *)gdi_alloc(bytes); HGDIOBJ h;
    if(!o) return NULL;
    o->type=type; o->owner=GetCurrentTask(); h=new_handle(o);
    if(!h) {gdi_free(o); return NULL;}
    if(out) *out=o;
    return h;
}
static void forget(HGDIOBJ h) {objects[((ULONG_PTR)h-HANDLE_BASE)/4]=NULL;}
DC *dc_of(HDC h) {return (DC *)object(h,OBJ_DC);}
DWORD pixel(COLORREF c) {return ((c&0xff)<<16)|(c&0xff00)|((c>>16)&0xff);}
COLORREF color(DWORD p) {return ((p>>16)&0xff)|(p&0xff00)|((p&0xff)<<16);}
/* An object's color keeps a palette index (PALETTEINDEX); PALETTERGB is RGB. */
static COLORREF object_color(COLORREF c) {return c>>24==1?c&0x0100ffff:c&0xffffff;}
DWORD dc_pixel(const DC *dc,COLORREF c) {
    if(c>>24==1) {
        const Palette *p=(const Palette *)object(dc->s.palette,OBJ_PAL); UINT i=LOWORD(c);
        c=p && i<(UINT)p->count?RGB(p->entries[i].peRed,p->entries[i].peGreen,p->entries[i].peBlue):0;
    }
    return pixel(c&0xffffff);
}

/* --- pens, brushes and regions ---------------------------------------------- */
HPEN WINAPI CreatePen(int style,int width,COLORREF c) {
    Pen *p; HPEN h=(HPEN)make_object(OBJ_PEN,sizeof(Pen),(void **)&p);
    if(h) {p->style=style; p->width=width<1?1:width; p->color=object_color(c);}
    return h;
}
HPEN WINAPI CreatePenIndirect(const LOGPEN FAR *l) {return l?CreatePen((int)l->lopnStyle,(int)l->lopnWidth.x,l->lopnColor):NULL;}
HBRUSH WINAPI CreateSolidBrush(COLORREF c) {
    Brush *b; HBRUSH h=(HBRUSH)make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b);
    if(h) {b->style=BS_SOLID; b->color=object_color(c);}
    return h;
}
static const BYTE hatches[6][8]={
    {0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00},{0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08},
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01},{0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80},
    {0x08,0x08,0x08,0x08,0xff,0x08,0x08,0x08},{0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81}};
/* Hatch lines take the brush color; the gaps show the DC's background. */
HBRUSH WINAPI CreateHatchBrush(int style,COLORREF c) {
    Brush *b; HBRUSH h; int x,y;
    if(style<HS_HORIZONTAL || style>HS_DIAGCROSS) return NULL;
    h=(HBRUSH)make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b);
    if(!h) return NULL;
    b->style=BS_HATCHED; b->color=object_color(c); b->hatch=style;
    for(y=0;y<8;y++) for(x=0;x<8;x++) b->pattern[y*8+x]=(hatches[style][y]&(0x80>>x))?1:0;
    return h;
}
HBRUSH WINAPI CreatePatternBrush(HBITMAP hb) {
    Bitmap *bm=(Bitmap *)object(hb,OBJ_BITMAP); Brush *b; HBRUSH h; int x,y;
    if(!bm) return NULL;
    h=(HBRUSH)make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b);
    if(!h) return NULL;
    b->style=BS_PATTERN; b->mono=bm->s.mono;
    for(y=0;y<8;y++) for(x=0;x<8;x++)
        b->pattern[y*8+x]=bm->s.bits[(ULONG_PTR)(y%bm->s.height)*bm->s.stride+(x%bm->s.width)];
    return h;
}
HBRUSH WINAPI CreateBrushIndirect(const LOGBRUSH FAR *l) {
    Brush *b; HBRUSH h;
    if(!l) return NULL;
    if(l->lbStyle==BS_HATCHED) return CreateHatchBrush((int)l->lbHatch,l->lbColor);
    if(l->lbStyle==BS_PATTERN) return CreatePatternBrush((HBITMAP)l->lbHatch);
    if(l->lbStyle==BS_DIBPATTERN) return CreateDIBPatternBrush((HGLOBAL)l->lbHatch,LOWORD(l->lbColor));
    h=(HBRUSH)make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b);
    if(h) {b->style=l->lbStyle==BS_NULL?BS_NULL:BS_SOLID; b->color=object_color(l->lbColor);}
    return h;
}
static Region *region(HRGN h) {return (Region *)object(h,OBJ_REGION);}
static int complexity(const RectList *l) {return !l->count?NULLREGION:l->count==1?SIMPLEREGION:COMPLEXREGION;}
HRGN WINAPI CreateRectRgn(int l,int t,int r,int b) {
    Region *g; RECT rc; HRGN h=(HRGN)make_object(OBJ_REGION,sizeof(Region),(void **)&g);
    if(!h) return NULL;
    r_set(&rc,min(l,r),min(t,b),max(l,r),max(t,b)); rl_set(&g->r,&rc);
    return h;
}
HRGN new_region(RectList *take) {
    Region *g; HRGN h=(HRGN)make_object(OBJ_REGION,sizeof(Region),(void **)&g);
    if(!h) {rl_free(take); return NULL;}
    g->r=*take; memset(take,0,sizeof(*take));
    return h;
}
HRGN WINAPI CreateRectRgnIndirect(const RECT FAR *r) {return r?CreateRectRgn(r->left,r->top,r->right,r->bottom):NULL;}
void WINAPI SetRectRgn(HRGN h,int l,int t,int r,int b) {
    Region *g=region(h); RECT rc;
    if(g) {r_set(&rc,min(l,r),min(t,b),max(l,r),max(t,b)); rl_set(&g->r,&rc);}
}
int WINAPI CombineRgn(HRGN dest,HRGN a,HRGN b,int mode) {
    Region *d=region(dest),*x=region(a),*y=region(b);
    if(!d || !x || (mode!=RGN_COPY && !y)) return ERROR;
    if(!rl_combine(&d->r,&x->r,y?&y->r:&x->r,mode)) return ERROR;
    return complexity(&d->r);
}
int WINAPI OffsetRgn(HRGN h,int dx,int dy) {Region *g=region(h); if(!g) return ERROR; rl_offset(&g->r,dx,dy); return complexity(&g->r);}
int WINAPI GetRgnBox(HRGN h,LPRECT box) {Region *g=region(h); if(!g || !box) return ERROR; rl_box(&g->r,box); return complexity(&g->r);}
BOOL WINAPI PtInRegion(HRGN h,int x,int y) {Region *g=region(h); return g && rl_contains(&g->r,x,y);}
BOOL WINAPI RectInRegion(HRGN h,const RECT FAR *r) {
    Region *g=region(h); int i; RECT c;
    if(!g || !r) return FALSE;
    for(i=0;i<g->r.count;i++) if(r_intersect(&c,&g->r.rects[i],r)) return TRUE;
    return FALSE;
}
BOOL WINAPI EqualRgn(HRGN a,HRGN b) {
    Region *x=region(a),*y=region(b); RectList d; BOOL same;
    if(!x || !y) return FALSE;
    memset(&d,0,sizeof(d));
    same=rl_combine(&d,&x->r,&y->r,RGN_XOR) && !d.count;
    rl_free(&d); return same;
}

/* --- stock objects, deletion, information ----------------------------------- */
HGDIOBJ WINAPI GetStockObject(int index) {
    return index>=0 && index<(int)(sizeof(stock)/sizeof(stock[0]))?stock[index]:NULL;
}
BOOL WINAPI DeleteObject(HGDIOBJ h) {
    ObjHeader *o=(ObjHeader *)object(h,0);
    if(!o || o->type==OBJ_DC) return FALSE;
    if(o->stock) return TRUE;
    if(o->type==OBJ_BITMAP) {
        Bitmap *b=(Bitmap *)o;
        if(b->selected) return FALSE;
        surface_free(&b->s);
    }
    if(o->type==OBJ_REGION) rl_free(&((Region *)o)->r);
    if(o->type==OBJ_METAFILE) metafile_free((Metafile *)o);
    else mf_deleted(h);
    forget(h); gdi_free(o);
    return TRUE;
}
DWORD WINAPI GetObjectType(HGDIOBJ h) {
    ObjHeader *o=(ObjHeader *)object(h,0);
    if(o && o->type==OBJ_DC && ((DC *)o)->meta) return OBJ_METADC;
    return o?o->type:0;
}
/* GetObject's answer: as much of the description as fits, or its size without a buffer. */
static int copy_out(void FAR *out,int size,const void *from,int bytes) {
    if(!out) return bytes;
    size=min(size,bytes); memcpy(out,from,(size_t)size); return size;
}
int WINAPI GetObject(HGDIOBJ h,int size,void FAR *out) {
    ObjHeader *o=(ObjHeader *)object(h,0);
    if(!o || size<0) return 0;
    switch(o->type) {
    case OBJ_PEN: {
        LOGPEN l; Pen *p=(Pen *)o;
        l.lopnStyle=(UINT)p->style; l.lopnWidth.x=p->width; l.lopnWidth.y=0; l.lopnColor=p->color;
        return copy_out(out,size,&l,(int)sizeof(l));
    }
    case OBJ_BRUSH: {
        LOGBRUSH l; Brush *b=(Brush *)o;
        l.lbStyle=(UINT)b->style; l.lbColor=b->color; l.lbHatch=(ULONG_PTR)b->hatch;
        return copy_out(out,size,&l,(int)sizeof(l));
    }
    case OBJ_FONT: return copy_out(out,size,&((Font *)o)->log,(int)sizeof(LOGFONT));
    case OBJ_PAL: {
        WORD n=(WORD)((Palette *)o)->count;
        return copy_out(out,size,&n,(int)sizeof(n));
    }
    case OBJ_BITMAP: {
        BITMAP l; Bitmap *b=(Bitmap *)o;
        l.bmType=0; l.bmWidth=b->s.width; l.bmHeight=b->s.height; l.bmPlanes=1;
        l.bmBitsPixel=(WORD)(b->s.mono?1:32); l.bmWidthBytes=bitmap_stride(b->s.width,b->s.mono); l.bmBits=NULL;
        return copy_out(out,size,&l,(int)sizeof(l));
    }
    }
    return 0;
}

/* --- DCs ---------------------------------------------------------------------- */
static void default_state(DCState *s) {
    rl_free(&s->clip);
    memset(s,0,sizeof(*s));
    s->pen=(HPEN)stock[BLACK_PEN]; s->brush=(HBRUSH)stock[WHITE_BRUSH]; s->font=(HFONT)stock[SYSTEM_FONT];
    s->text=RGB(0,0,0); s->bk=RGB(255,255,255); s->bk_mode=OPAQUE; s->rop2=R2_COPYPEN; s->align=TA_LEFT|TA_TOP;
    s->poly_fill=ALTERNATE; s->stretch_mode=BLACKONWHITE; s->palette=(HPALETTE)stock[DEFAULT_PALETTE];
    map_reset(s);
}
void update_clip(DC *dc) {
    if(dc->s.clipped) rl_intersect(&dc->eff,&dc->vis,&dc->s.clip);
    else rl_copy(&dc->eff,&dc->vis);
}
DC *new_dc(HDC *out) {
    DC *dc; HDC h=(HDC)make_object(OBJ_DC,sizeof(DC),(void **)&dc);
    if(!h) return NULL;
    default_state(&dc->s); dc->dpi=96; *out=h; return dc;
}
void whole(DC *dc) {
    RECT r; r_set(&r,0,0,dc->surface->width,dc->surface->height);
    rl_set(&dc->vis,&r); update_clip(dc);
}
HDC WINAPI GdiCreateScreenDC(void) {
    HDC h; DC *dc=new_dc(&h);
    if(!dc) return NULL;
    dc->surface=&screen; dc->display=TRUE; whole(dc);
    return h;
}
/* A memory DC like a printer's has the printer's resolution. */
HDC WINAPI CreateCompatibleDC(HDC like) {
    HDC h; DC *dc=new_dc(&h),*other=dc_of(like); Bitmap *b=(Bitmap *)object(default_bitmap,OBJ_BITMAP);
    if(!dc) return NULL;
    if(other) dc->dpi=other->dpi;
    dc->bitmap=default_bitmap; dc->surface=&b->s; whole(dc);
    return h;
}
BOOL WINAPI DeleteDC(HDC h) {
    DC *dc=dc_of(h); int i;
    if(!dc) return FALSE;
    if(dc->bitmap) {Bitmap *b=(Bitmap *)object(dc->bitmap,OBJ_BITMAP); if(b && b->selected==dc) b->selected=NULL;}
    if(dc->printer) printer_free(dc);
    if(dc->meta) mf_free(dc);
    for(i=0;i<dc->saved_count;i++) rl_free(&dc->saved[i].clip);
    rl_free(&dc->s.clip); rl_free(&dc->vis); rl_free(&dc->eff);
    forget(h); gdi_free(dc);
    return TRUE;
}
void WINAPI GdiSetDCOrigin(HDC h,int x,int y) {DC *dc=dc_of(h); if(dc) {dc->origin.x=x; dc->origin.y=y;}}
void WINAPI GdiSetVisRects(HDC h,const RECT *rects,int count) {
    DC *dc=dc_of(h); int i; RECT all;
    if(!dc) return;
    r_set(&all,0,0,dc->surface->width,dc->surface->height);
    dc->vis.count=0;
    for(i=0;i<count;i++) {RECT c; if(r_intersect(&c,&rects[i],&all)) rl_add(&dc->vis,&c);}
    update_clip(dc);
}
HGDIOBJ WINAPI SelectObject(HDC h,HGDIOBJ handle) {
    DC *dc=dc_of(h); ObjHeader *o=(ObjHeader *)object(handle,0); HGDIOBJ old=NULL;
    if(!dc || !o) return NULL;
    if(dc->meta && o->type!=OBJ_REGION) return mf_select(dc,handle);
    switch(o->type) {
    case OBJ_PEN: old=dc->s.pen; dc->s.pen=(HPEN)handle; break;
    case OBJ_BRUSH: old=dc->s.brush; dc->s.brush=(HBRUSH)handle; break;
    case OBJ_FONT: old=dc->s.font; dc->s.font=(HFONT)handle; break;
    case OBJ_REGION: return (HGDIOBJ)(ULONG_PTR)SelectClipRgn(h,(HRGN)handle);
    case OBJ_BITMAP: {
        Bitmap *b=(Bitmap *)o,*was;
        if(dc->display || dc->printer || (b->selected && b->selected!=dc)) return NULL;
        was=(Bitmap *)object(dc->bitmap,OBJ_BITMAP);
        if(was && was->selected==dc) was->selected=NULL;
        old=dc->bitmap; dc->bitmap=(HBITMAP)handle; dc->surface=&b->s;
        if(handle!=default_bitmap) b->selected=dc;
        whole(dc); break;
    }
    default: return NULL;
    }
    return old;
}
int WINAPI SaveDC(HDC h) {
    DC *dc=dc_of(h); DCState *s;
    if(!dc || dc->saved_count==SAVED_DEPTH) return 0;
    if(dc->meta) mf_words(dc,META_SAVEDC,0);
    s=&dc->saved[dc->saved_count]; *s=dc->s; memset(&s->clip,0,sizeof(s->clip));
    rl_copy(&s->clip,&dc->s.clip);
    return ++dc->saved_count;
}
BOOL WINAPI RestoreDC(HDC h,int level) {
    DC *dc=dc_of(h);
    if(!dc) return FALSE;
    if(dc->meta) mf_words(dc,META_RESTOREDC,1,level);
    if(level<0) level=dc->saved_count+level+1;
    if(level<1 || level>dc->saved_count) return FALSE;
    while(dc->saved_count>level) rl_free(&dc->saved[--dc->saved_count].clip);
    rl_free(&dc->s.clip); dc->s=dc->saved[level-1]; dc->saved_count=level-1;
    update_clip(dc);
    return TRUE;
}
#define ATTRIBUTE(get,set,type,field,function) \
    type WINAPI get(HDC h) {DC *dc=dc_of(h); return dc?(type)dc->s.field:0;} \
    type WINAPI set(HDC h,type v) { \
        DC *dc=dc_of(h); type old; \
        if(!dc) return 0; \
        if(dc->meta) mf_value(dc,function,(DWORD)v); \
        old=(type)dc->s.field; dc->s.field=v; return old; \
    }
ATTRIBUTE(GetTextColor,SetTextColor,COLORREF,text,META_SETTEXTCOLOR)
ATTRIBUTE(GetBkColor,SetBkColor,COLORREF,bk,META_SETBKCOLOR)
ATTRIBUTE(GetBkMode,SetBkMode,int,bk_mode,META_SETBKMODE)
ATTRIBUTE(GetTextAlign,SetTextAlign,UINT,align,META_SETTEXTALIGN)
ATTRIBUTE(GetROP2,SetROP2,int,rop2,META_SETROP2)
ATTRIBUTE(GetPolyFillMode,SetPolyFillMode,int,poly_fill,META_SETPOLYFILLMODE)
ATTRIBUTE(GetStretchBltMode,SetStretchBltMode,int,stretch_mode,META_SETSTRETCHBLTMODE)
int WINAPI SetTextCharacterExtra(HDC h,int extra) {
    DC *dc=dc_of(h); int old;
    if(!dc) return 0;
    if(dc->meta) mf_value(dc,META_SETTEXTCHAREXTRA,(DWORD)extra);
    old=dc->s.extra; dc->s.extra=extra; return old;
}
DWORD WINAPI SetBrushOrg(HDC h,int x,int y) {
    DC *dc=dc_of(h); DWORD old;
    if(!dc) return 0;
    old=MAKELONG(dc->s.brush_origin.x,dc->s.brush_origin.y); dc->s.brush_origin.x=x; dc->s.brush_origin.y=y;
    return old;
}
BOOL WINAPI SetBrushOrgEx(HDC h,int x,int y,LPPOINT old) {
    DC *dc=dc_of(h);
    if(!dc) return FALSE;
    if(old) *old=dc->s.brush_origin;
    dc->s.brush_origin.x=x; dc->s.brush_origin.y=y; return TRUE;
}
DWORD WINAPI GetCurrentPosition(HDC h) {DC *dc=dc_of(h); return dc?MAKELONG(dc->s.pos.x,dc->s.pos.y):0;}
DWORD WINAPI GetDCOrg(HDC h) {DC *dc=dc_of(h); return dc?MAKELONG(dc->origin.x,dc->origin.y):0;}
COLORREF WINAPI GetNearestColor(HDC h,COLORREF c) {DC *dc=dc_of(h); return dc?color(dc_pixel(dc,c)):c&0xffffff;}
BOOL WINAPI GetBrushOrgEx(HDC h,LPPOINT out) {
    DC *dc=dc_of(h);
    if(!dc || !out) return FALSE;
    *out=dc->s.brush_origin; return TRUE;
}
DWORD WINAPI GetBrushOrg(HDC h) {DC *dc=dc_of(h); return dc?MAKELONG(dc->s.brush_origin.x,dc->s.brush_origin.y):0;}
DWORD WINAPI SetMapperFlags(HDC h,DWORD flags) {
    DC *dc=dc_of(h); DWORD old;
    if(!dc) return (DWORD)-1;
    if(dc->meta) mf_value(dc,META_SETMAPPERFLAGS,flags);
    old=dc->s.mapper_flags; dc->s.mapper_flags=flags; return old;
}
DWORD WINAPI GetAspectRatioFilter(HDC h) {(void)h; return 0;}
BOOL WINAPI GetAspectRatioFilterEx(HDC h,LPSIZE out) {
    if(!dc_of(h) || !out) return FALSE;
    out->cx=out->cy=0; return TRUE;
}
BOOL WINAPI UnrealizeObject(HGDIOBJ h) {return object(h,0)!=NULL;}
/* The devices: the display and the PSCRIPT printer. */
HDC WINAPI CreateDC(LPCSTR driver,LPCSTR device,LPCSTR output,const void FAR *init) {
    if(driver && !lstrcmpi(driver,"DISPLAY")) return GdiCreateScreenDC();
    return printer_dc(driver,device,output,(const DEVMODE FAR *)init,FALSE);
}
HDC WINAPI CreateIC(LPCSTR driver,LPCSTR device,LPCSTR output,const void FAR *init) {
    if(driver && !lstrcmpi(driver,"DISPLAY")) return GdiCreateScreenDC();
    return printer_dc(driver,device,output,(const DEVMODE FAR *)init,TRUE);
}

/* --- palettes ---------------------------------------------------------------------- */
static Palette *palette_of(HPALETTE h) {return (Palette *)object(h,OBJ_PAL);}
HPALETTE WINAPI CreatePalette(const LOGPALETTE FAR *l) {
    Palette *p; HPALETTE h;
    if(!l || !l->palNumEntries || l->palNumEntries>256) return NULL;
    h=(HPALETTE)make_object(OBJ_PAL,sizeof(Palette),(void **)&p);
    if(!h) return NULL;
    p->count=l->palNumEntries; memcpy(p->entries,l->palPalEntry,sizeof(PALETTEENTRY)*(unsigned)p->count);
    return h;
}
HPALETTE WINAPI SelectPalette(HDC h,HPALETTE palette,BOOL background) {
    DC *dc=dc_of(h); HPALETTE old;
    (void)background;
    if(!dc || !palette_of(palette)) return NULL;
    if(dc->meta) return (HPALETTE)mf_select(dc,palette);
    old=dc->s.palette; dc->s.palette=palette; return old;
}
UINT WINAPI RealizePalette(HDC h) {DC *dc=dc_of(h); if(META_DC(dc)) mf_words(dc,META_REALIZEPALETTE,0); return 0;}
int WINAPI UpdateColors(HDC h) {(void)h; return 0;}
UINT WINAPI GetPaletteEntries(HPALETTE h,UINT start,UINT n,LPPALETTEENTRY out) {
    Palette *p=palette_of(h);
    if(!p) return 0;
    if(!out) return (UINT)p->count;
    if(start>=(UINT)p->count) return 0;
    n=min(n,(UINT)p->count-start); memcpy(out,p->entries+start,sizeof(PALETTEENTRY)*n);
    return n;
}
UINT WINAPI SetPaletteEntries(HPALETTE h,UINT start,UINT n,const PALETTEENTRY FAR *in) {
    Palette *p=palette_of(h);
    if(!p || !in || start>=(UINT)p->count || p->h.stock) return 0;
    n=min(n,(UINT)p->count-start); memcpy(p->entries+start,in,sizeof(PALETTEENTRY)*n);
    return n;
}
void WINAPI AnimatePalette(HPALETTE h,UINT start,UINT n,const PALETTEENTRY FAR *in) {
    Palette *p=palette_of(h); UINT i;
    if(!p || !in || p->h.stock) return;
    for(i=0;i<n && start+i<(UINT)p->count;i++) if(p->entries[start+i].peFlags&PC_RESERVED) p->entries[start+i]=in[i];
}
BOOL WINAPI ResizePalette(HPALETTE h,UINT n) {
    Palette *p=palette_of(h);
    if(!p || !n || n>256 || p->h.stock) return FALSE;
    if(n>(UINT)p->count) memset(p->entries+p->count,0,sizeof(PALETTEENTRY)*(n-(UINT)p->count));
    p->count=(int)n; return TRUE;
}
UINT WINAPI GetNearestPaletteIndex(HPALETTE h,COLORREF c) {
    Palette *p=palette_of(h); int i,best=0; DWORD best_d=0xffffffff;
    if(!p) return (UINT)-1;
    for(i=0;i<p->count;i++) {
        int r=(int)p->entries[i].peRed-GetRValue(c),g=(int)p->entries[i].peGreen-GetGValue(c),b=(int)p->entries[i].peBlue-GetBValue(c);
        DWORD d=(DWORD)(r*r+g*g+b*b);
        if(d<best_d) {best_d=d; best=i;}
    }
    return (UINT)best;
}
UINT WINAPI GetSystemPaletteEntries(HDC h,UINT start,UINT n,LPPALETTEENTRY out) {(void)h; (void)start; (void)n; (void)out; return 0;}
UINT WINAPI GetSystemPaletteUse(HDC h) {(void)h; return SYSPAL_STATIC;}
UINT WINAPI SetSystemPaletteUse(HDC h,UINT use) {(void)h; (void)use; return SYSPAL_STATIC;}
int WINAPI GetDeviceCaps(HDC h,int index) {
    DC *dc=dc_of(h); int value;
    if(dc && dc->printer && printer_caps(dc,index,&value)) return value;
    /* A metafile DC: the display's, but for the technology. */
    if(META_DC(dc)) {if(index==TECHNOLOGY) return DT_METAFILE; dc=NULL;}
    switch(index) {
    case DRIVERVERSION: return 0x300;
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZSIZE: return screen.width*254/960;
    case VERTSIZE: return screen.height*254/960;
    case HORZRES: return dc?dc->surface->width:screen.width;
    case VERTRES: return dc?dc->surface->height:screen.height;
    case BITSPIXEL: return dc && dc->surface->mono?1:32;
    case PLANES: return 1;
    case NUMBRUSHES: case NUMPENS: case NUMCOLORS: return -1;
    case NUMFONTS: return 6;
    case PDEVICESIZE: return sizeof(DC);
    case CLIPCAPS: return CP_RECTANGLE;
    case RASTERCAPS: return RC_BITBLT|RC_BITMAP64|RC_GDI20_OUTPUT|RC_DI_BITMAP|RC_DIBTODEV|RC_STRETCHBLT|RC_STRETCHDIB;
    case ASPECTX: case ASPECTY: return 36;
    case ASPECTXY: return 51;
    case LOGPIXELSX: case LOGPIXELSY: return dc?dc->dpi:96;
    case COLORRES: return 24;
    }
    return 0;
}

/* --- clipping ------------------------------------------------------------------ */
static int clip_result(DC *dc) {return complexity(&dc->eff);}
int WINAPI SelectClipRgn(HDC h,HRGN rgn) {
    DC *dc=dc_of(h); Region *g=region(rgn);
    if(!dc) return ERROR;
    if(dc->meta) return mf_region(dc,META_SELECTCLIPREGION,rgn,NULL,0,0)?SIMPLEREGION:ERROR;
    if(!rgn) {dc->s.clipped=FALSE; dc->s.clip.count=0;}
    else {
        if(!g) return ERROR;
        rl_copy(&dc->s.clip,&g->r); rl_offset(&dc->s.clip,dc->origin.x,dc->origin.y); dc->s.clipped=TRUE;
    }
    update_clip(dc); return clip_result(dc);
}
static void clip_start(DC *dc) {
    if(!dc->s.clipped) {RECT r; r_set(&r,-0x40000000,-0x40000000,0x40000000,0x40000000); rl_set(&dc->s.clip,&r); dc->s.clipped=TRUE;}
}
int WINAPI IntersectClipRect(HDC h,int l,int t,int r,int b) {
    DC *dc=dc_of(h); RectList one; RECT rc;
    if(!dc) return ERROR;
    if(dc->meta) return mf_words(dc,META_INTERSECTCLIPRECT,4,b,r,t,l)?SIMPLEREGION:ERROR;
    clip_start(dc);
    memset(&one,0,sizeof(one)); r_set(&rc,l,t,r,b); dev_rect(dc,&rc); rl_set(&one,&rc);
    rl_intersect(&dc->s.clip,&dc->s.clip,&one); rl_free(&one);
    update_clip(dc); return clip_result(dc);
}
int WINAPI ExcludeClipRect(HDC h,int l,int t,int r,int b) {
    DC *dc=dc_of(h); RECT rc;
    if(!dc) return ERROR;
    if(dc->meta) return mf_words(dc,META_EXCLUDECLIPRECT,4,b,r,t,l)?SIMPLEREGION:ERROR;
    clip_start(dc);
    r_set(&rc,l,t,r,b); dev_rect(dc,&rc); rl_subtract(&dc->s.clip,&rc);
    update_clip(dc); return clip_result(dc);
}
int WINAPI OffsetClipRgn(HDC h,int x,int y) {
    DC *dc=dc_of(h);
    if(!dc) return ERROR;
    if(dc->meta) return mf_words(dc,META_OFFSETCLIPRGN,2,y,x)?SIMPLEREGION:ERROR;
    if(!dc->s.clipped) return SIMPLEREGION;
    rl_offset(&dc->s.clip,dev_x(dc,x)-dev_x(dc,0),dev_y(dc,y)-dev_y(dc,0));
    update_clip(dc); return clip_result(dc);
}
int WINAPI GetClipBox(HDC h,LPRECT box) {
    DC *dc=dc_of(h);
    if(!dc || !box) return ERROR;
    rl_box(&dc->eff,box);
    {
        int l=log_x(dc,box->left),t=log_y(dc,box->top),r=log_x(dc,box->right),b=log_y(dc,box->bottom);
        r_set(box,min(l,r),min(t,b),max(l,r),max(t,b));
    }
    return clip_result(dc);
}
BOOL WINAPI PtVisible(HDC h,int x,int y) {DC *dc=dc_of(h); return dc && rl_contains(&dc->eff,dev_x(dc,x),dev_y(dc,y));}
BOOL WINAPI RectVisible(HDC h,const RECT FAR *r) {
    DC *dc=dc_of(h); RECT d,c; int i;
    if(!dc || !r) return FALSE;
    d=*r; dev_rect(dc,&d);
    for(i=0;i<dc->eff.count;i++) if(r_intersect(&c,&dc->eff.rects[i],&d)) return TRUE;
    return FALSE;
}

/* --- owners ------------------------------------------------------------------------ */
/* Objects a task made go when it ends: its DCs first, so its bitmaps are free. */
void WINAPI GdiTaskEnded(HTASK task) {
    int i,pass;
    if(!task) return;
    for(pass=0;pass<2;pass++) for(i=0;i<OBJECTS;i++) {
        ObjHeader *o=(ObjHeader *)objects[i];
        if(!o || o->stock || o->owner!=task || (pass==0)!=(o->type==OBJ_DC)) continue;
        if(o->type==OBJ_DC) DeleteDC((HDC)(ULONG_PTR)(HANDLE_BASE+i*4));
        /* A bitmap still selected into another task's DC stays. */
        else DeleteObject((HGDIOBJ)(ULONG_PTR)(HANDLE_BASE+i*4));
    }
}
/* USER's own objects (system color brushes, icons) belong to no task. */
void WINAPI GdiSetOwner(HGDIOBJ h,HTASK task) {ObjHeader *o=(ObjHeader *)object(h,0); if(o) o->owner=task;}

/* --- start-up ---------------------------------------------------------------------- */
static void set_stock(int index,HGDIOBJ h) {ObjHeader *o=(ObjHeader *)object(h,0); if(o) o->stock=1; stock[index]=h;}
static HFONT stock_font(const char *face,int height,int weight) {
    LOGFONT l; memset(&l,0,sizeof(l)); l.lfHeight=height; l.lfWeight=weight; strcpy(l.lfFaceName,face);
    return CreateFontIndirect(&l);
}
BOOL ObjectsInit(void) {
    static const COLORREF grays[5]={RGB(255,255,255),RGB(192,192,192),RGB(128,128,128),RGB(64,64,64),RGB(0,0,0)};
    Brush *b; Pen *p; Bitmap *bm; int i;
    memset(objects,0,sizeof(objects));
    for(i=0;i<5;i++) set_stock(i,CreateSolidBrush(grays[i]));
    set_stock(NULL_BRUSH,make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b)); b->style=BS_NULL;
    set_stock(WHITE_PEN,CreatePen(PS_SOLID,1,RGB(255,255,255)));
    set_stock(BLACK_PEN,CreatePen(PS_SOLID,1,RGB(0,0,0)));
    set_stock(NULL_PEN,make_object(OBJ_PEN,sizeof(Pen),(void **)&p)); p->style=PS_NULL; p->width=1;
    set_stock(SYSTEM_FONT,stock_font("System",0,FW_BOLD));
    set_stock(OEM_FIXED_FONT,stock_font("Terminal",0,FW_NORMAL));
    set_stock(ANSI_FIXED_FONT,stock_font("Courier",0,FW_NORMAL));
    set_stock(ANSI_VAR_FONT,stock_font("Helv",0,FW_NORMAL));
    set_stock(DEVICE_DEFAULT_FONT,stock[SYSTEM_FONT]);
    set_stock(SYSTEM_FIXED_FONT,stock_font("Fixedsys",0,FW_NORMAL));
    {
        /* The 20 colors Windows reserves: the 16 VGA ones and four more. */
        static const BYTE colors[20][3]={{0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},
            {192,192,192},{192,220,192},{166,202,240},{255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},
            {255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}};
        struct {WORD version,count; PALETTEENTRY e[20];} l;
        l.version=0x300; l.count=20;
        for(i=0;i<20;i++) {l.e[i].peRed=colors[i][0]; l.e[i].peGreen=colors[i][1]; l.e[i].peBlue=colors[i][2]; l.e[i].peFlags=0;}
        set_stock(DEFAULT_PALETTE,CreatePalette((const LOGPALETTE *)&l));
    }
    default_bitmap=make_object(OBJ_BITMAP,sizeof(Bitmap),(void **)&bm);
    if(!default_bitmap || !surface_alloc(&bm->s,1,1,TRUE)) return FALSE;
    bm->h.stock=1;
    for(i=0;i<=SYSTEM_FIXED_FONT;i++) if(i!=9 && !stock[i]) return FALSE;
    return TRUE;
}
