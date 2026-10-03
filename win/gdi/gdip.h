/* SPDX-License-Identifier: GPL-2.0-or-later
 * GDI.DLL internals. Every surface is 32-bit BGRX; a monochrome bitmap keeps
 * black (0) and white (0xffffff) pixels and is converted with the DC's text
 * and background colors when blitted to or from color, as in Windows. C89.
 */
#ifndef GDI_PRIVATE_H
#define GDI_PRIVATE_H
#define GDI_BUILD
#include <windows.h>
#include <winhost.h>
#include <string.h>
#include "gdi.h"

typedef struct {WORD type,stock; HTASK owner;} ObjHeader; /* type: OBJ_xxx; owner: the task that made it, or none */
typedef struct {DWORD *bits; int width,height,stride; BOOL mono;} Surface;
/* Rectangle lists: disjoint rectangles, used for regions and clipping. */
typedef struct {int count,capacity; RECT *rects;} RectList;
typedef struct {ObjHeader h; int style,width; COLORREF color;} Pen;
typedef struct {ObjHeader h; int style; COLORREF color; int hatch; DWORD pattern[64]; BOOL mono;} Brush;
/* A bitmap font: per character, rows of words 32-bit words (the most
 * significant bit at the pen position; 0 is one) and the advance; the
 * internal leading is part of the height above the characters; the average
 * width is the font's own. */
typedef struct {
    const unsigned int *bits; const unsigned char *widths;
    int height,ascent,leading,avg,max; BOOL bold,proportional; BYTE charset; int words;
} FontFace;
/* A logical font and what it was matched to: the face's name and family,
 * the whole multiples its glyphs are drawn at across and down, and bold or
 * italic made from the regular glyphs when the face lacks them. */
typedef struct {
    ObjHeader h; LOGFONT log; const FontFace *face;
    char name[LF_FACESIZE]; BYTE family,charset; BOOL embolden,italic; int sx,sy;
} Font;
typedef struct {ObjHeader h; Surface s; struct DC *selected; SIZE dimension;} Bitmap;
typedef struct {ObjHeader h; RectList r;} Region;
typedef struct {ObjHeader h; int count; PALETTEENTRY entries[256];} Palette;
typedef struct {ObjHeader h; DWORD size; BYTE *bits;} Metafile; /* the bits in Windows' metafile format */
typedef struct Meta Meta; /* a metafile being recorded (metafile.c) */
typedef struct {
    HPEN pen; HBRUSH brush; HFONT font; COLORREF text,bk;
    int bk_mode,rop2,poly_fill,stretch_mode,extra; UINT align; POINT pos,brush_origin;
    RectList clip; BOOL clipped; /* application clip, device coordinates */
    /* The mapping mode: logical (x - window origin) * viewport extent /
     * window extent + viewport origin, then the DC's origin. */
    int map_mode; POINT window_org,window_ext,viewport_org,viewport_ext;
    int break_extra,break_count; /* SetTextJustification */
    HPALETTE palette; DWORD mapper_flags;
} DCState;
#define SAVED_DEPTH 8
typedef struct Printer Printer;
typedef struct DC {
    ObjHeader h; DCState s;
    Surface *surface; HBITMAP bitmap; BOOL display;
    Printer *printer; /* a printer's DC: the page and the job (print.c) */
    Meta *meta; /* a metafile DC: calls are recorded, nothing is drawn */
    int dpi; /* dots per inch, for the metric and English mapping modes */
    POINT origin;
    RectList vis;  /* where output may go, device coordinates */
    RectList eff;  /* vis and the application clip */
    DCState saved[SAVED_DEPTH]; int saved_count;
} DC;

/* map.c: logical coordinates to device coordinates and back, and lengths
 * along each axis (positive); a rectangle comes out ordered. */
void map_reset(DCState *);
int dev_x(const DC *,int x);
int dev_y(const DC *,int y);
void dev_point(const DC *,POINT *);
void dev_rect(const DC *,RECT *);
int dev_w(const DC *,int w);
int dev_h(const DC *,int h);
int log_x(const DC *,int x);
int log_y(const DC *,int y);
int log_w(const DC *,int w);
int log_h(const DC *,int h);
/* object.c */
extern HGDIOBJ stock[18];
HRGN new_region(RectList *take); /* a region that takes over the list */
DWORD dc_pixel(const DC *,COLORREF); /* palette indexes through the DC's palette */
extern Surface screen;
extern RECT dirty;
/* Fonts from WIN.INI [fonts], read after the stock fonts are made; freed at the end. */
void FontsInit(void);
void FontsEnd(void);
BOOL ObjectsInit(void); /* the stock objects */
void *gdi_alloc(DWORD bytes);
void gdi_free(void *);
HGDIOBJ make_object(WORD type,UINT bytes,void **out);
void *object(HGDIOBJ,WORD type);
DC *dc_of(HDC);
DC *new_dc(HDC *);
void whole(DC *); /* output may go anywhere on the surface */
void update_clip(DC *);
DWORD pixel(COLORREF);
COLORREF color(DWORD);
/* region.c */
void r_set(RECT *,int,int,int,int);
BOOL r_empty(const RECT *);
BOOL r_intersect(RECT *,const RECT *,const RECT *);
void r_union(RECT *,const RECT *,const RECT *);
BOOL rl_set(RectList *,const RECT *);
void rl_free(RectList *);
BOOL rl_copy(RectList *,const RectList *);
BOOL rl_add(RectList *,const RECT *);
BOOL rl_subtract(RectList *,const RECT *);
BOOL rl_intersect(RectList *out,const RectList *,const RectList *);
BOOL rl_combine(RectList *out,const RectList *,const RectList *,int mode);
void rl_offset(RectList *,int,int);
/* A shape's rows as a list: each row's spans, rows with the same spans merged. */
typedef struct {RectList *list; int first; int count;} RowBuilder;
void rows_begin(RowBuilder *,RectList *);
BOOL rows_add(RowBuilder *,int y,const int *spans,int n); /* spans: [left,right) pairs */
void rl_box(const RectList *,RECT *);
BOOL rl_contains(const RectList *,int,int);
/* draw.c */
void fill_solid(DC *,const RECT *,DWORD value,int rop2);
DWORD brush_pixel(const Brush *,const DC *,int x,int y);
/* print.c: the PSCRIPT driver. */
HDC printer_dc(LPCSTR driver,LPCSTR device,LPCSTR port,const DEVMODE FAR *,BOOL info);
void printer_free(DC *);
BOOL printer_caps(const DC *,int index,int *value);
/* metafile.c: what a metafile DC is asked to do, as records (each
 * function's parameters as the record has them, the call's in reverse). */
BOOL mf_words(DC *,WORD function,int n,...);
void mf_value(DC *,WORD function,DWORD value); /* one word, or two for a color or flags */
HGDIOBJ mf_select(DC *,HGDIOBJ); /* the old one */
void mf_deleted(HGDIOBJ);
BOOL mf_points(DC *,WORD function,const POINT *,int);
BOOL mf_poly_polygon(DC *,const POINT *,const int *,int);
BOOL mf_text(DC *,int x,int y,UINT options,const RECT *,LPCSTR,UINT count,const int *dx,BOOL ext);
BOOL mf_region(DC *,WORD function,HRGN,HBRUSH,int w,int h);
BOOL mf_blt(DC *,int x,int y,int w,int h,HDC from,int sx,int sy,int sw,int sh,DWORD rop,BOOL stretch);
BOOL mf_dib(DC *,WORD function,int x,int y,int w,int h,int sx,int sy,int sw,int sh,UINT start,UINT lines,
            const void *bits,const BITMAPINFO *,UINT usage,DWORD rop);
void mf_free(DC *);
void metafile_free(Metafile *);
#define META_DC(dc) ((dc) && (dc)->meta)
/* bitmap.c */
BOOL surface_alloc(Surface *,int,int,BOOL mono);
void surface_free(Surface *);
DWORD to_mono(DWORD);
int bitmap_stride(int width,BOOL mono);
void mark(const RECT *);
#endif
