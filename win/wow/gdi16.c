/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: GDI functions for Win16 programs whose structures have 16-bit
 * fields: LOGFONT, LOGPEN, LOGBRUSH, BITMAP and TEXTMETRIC, and arrays of
 * points, widths and spacings. A device-independent bitmap's header is
 * the same in both worlds; it is copied to native memory, which keeps it
 * aligned, as is a printer's DEVMODE. Printing escapes convert their data
 * and the abort procedure.
 */
#include "api.h"
#include <commdlg.h>

/* --- logical objects ---------------------------------------------------------- */
/* LOGFONT: height, width, escapement, orientation and weight (shorts),
 * eight bytes from italic to pitch and family, the face name. */
#define LOGFONT16 (10+8+LF_FACESIZE)
void LogFontIn16(const BYTE *p,LOGFONT *l) {
    memset(l,0,sizeof(*l));
    l->lfHeight=(short)get16(p); l->lfWidth=(short)get16(p+2); l->lfEscapement=(short)get16(p+4);
    l->lfOrientation=(short)get16(p+6); l->lfWeight=(short)get16(p+8);
    l->lfItalic=p[10]; l->lfUnderline=p[11]; l->lfStrikeOut=p[12]; l->lfCharSet=p[13];
    l->lfOutPrecision=p[14]; l->lfClipPrecision=p[15]; l->lfQuality=p[16]; l->lfPitchAndFamily=p[17];
    memcpy(l->lfFaceName,p+18,LF_FACESIZE); l->lfFaceName[LF_FACESIZE-1]=0;
}
void LogFontOut16(BYTE *p,const LOGFONT *l) {
    put16(p,(WORD)l->lfHeight); put16(p+2,(WORD)l->lfWidth); put16(p+4,(WORD)l->lfEscapement);
    put16(p+6,(WORD)l->lfOrientation); put16(p+8,(WORD)l->lfWeight);
    p[10]=l->lfItalic; p[11]=l->lfUnderline; p[12]=l->lfStrikeOut; p[13]=l->lfCharSet;
    p[14]=l->lfOutPrecision; p[15]=l->lfClipPrecision; p[16]=l->lfQuality; p[17]=l->lfPitchAndFamily;
    memcpy(p+18,l->lfFaceName,LF_FACESIZE);
}
DWORD W16_CreateFontIndirect(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); LOGFONT l;
    if(!p) return 0;
    LogFontIn16(p,&l);
    return HGDI16(CreateFontIndirect(&l));
}
/* LOGPEN: style, width (a POINT of shorts), color. */
DWORD W16_CreatePenIndirect(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); LOGPEN l;
    if(!p) return 0;
    l.lopnStyle=get16(p); l.lopnWidth.x=(short)get16(p+2); l.lopnWidth.y=(short)get16(p+4); l.lopnColor=get32(p+6);
    return HGDI16(CreatePenIndirect(&l));
}
/* LOGBRUSH: style, color, hatch style (or a pattern bitmap). */
DWORD W16_CreateBrushIndirect(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); LOGBRUSH l;
    if(!p) return 0;
    l.lbStyle=get16(p); l.lbColor=get32(p+2);
    l.lbHatch=l.lbStyle==BS_PATTERN?(ULONG_PTR)HGDI32(get16(p+6)):get16(p+6);
    return HGDI16(CreateBrushIndirect(&l));
}
/* BITMAP: type, width, height and bytes per row (shorts), planes and bits
 * per pixel (bytes), the bits (a far pointer). */
DWORD W16_CreateBitmapIndirect(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); BITMAP b;
    if(!p) return 0;
    b.bmType=0; b.bmWidth=(short)get16(p+2); b.bmHeight=(short)get16(p+4); b.bmWidthBytes=(short)get16(p+6);
    b.bmPlanes=p[8]; b.bmBitsPixel=p[9]; b.bmBits=Lin16(get32(p+10));
    return HGDI16(CreateBitmapIndirect(&b));
}
DWORD W16_GetObject(Args16 *a) {
    HGDIOBJ h=HGDI32(a->a[0]); int size=(int)a->a[1],n; BYTE buffer[LOGFONT16],*out=(BYTE *)PTR(a->a[2]);
    switch(GetObjectType(h)) {
    case OBJ_PEN: {
        LOGPEN l;
        if(!GetObject(h,sizeof(l),&l)) return 0;
        put16(buffer,(WORD)l.lopnStyle); put16(buffer+2,(WORD)l.lopnWidth.x); put16(buffer+4,(WORD)l.lopnWidth.y);
        put32(buffer+6,l.lopnColor); n=10; break;
    }
    case OBJ_BRUSH: {
        LOGBRUSH l;
        if(!GetObject(h,sizeof(l),&l)) return 0;
        put16(buffer,(WORD)l.lbStyle); put32(buffer+2,l.lbColor);
        put16(buffer+6,l.lbStyle==BS_PATTERN?HGDI16(l.lbHatch):(WORD)l.lbHatch); n=8; break;
    }
    case OBJ_FONT: {
        LOGFONT l;
        if(!GetObject(h,sizeof(l),&l)) return 0;
        LogFontOut16(buffer,&l); n=LOGFONT16; break;
    }
    case OBJ_PAL: {
        WORD count;
        if(!GetObject(h,sizeof(count),&count)) return 0;
        put16(buffer,count); n=2; break;
    }
    case OBJ_BITMAP: {
        BITMAP b;
        if(!GetObject(h,sizeof(b),&b)) return 0;
        put16(buffer,0); put16(buffer+2,(WORD)b.bmWidth); put16(buffer+4,(WORD)b.bmHeight); put16(buffer+6,(WORD)b.bmWidthBytes);
        buffer[8]=(BYTE)b.bmPlanes; buffer[9]=(BYTE)b.bmBitsPixel; put32(buffer+10,0); n=14; break;
    }
    default: return 0;
    }
    if(!out) return (DWORD)n;
    if(size<n) n=size<0?0:size;
    memcpy(out,buffer,(size_t)n);
    return (DWORD)n;
}
/* TEXTMETRIC: eight shorts (height to weight), nine bytes (italic to
 * character set), overhang and the digitized aspect (shorts). */
#define TEXTMETRIC16 31
static void textmetric_out(BYTE *o,const TEXTMETRIC *t) {
    const TEXTMETRIC m=*t;
    put16(o,(WORD)m.tmHeight); put16(o+2,(WORD)m.tmAscent); put16(o+4,(WORD)m.tmDescent);
    put16(o+6,(WORD)m.tmInternalLeading); put16(o+8,(WORD)m.tmExternalLeading); put16(o+10,(WORD)m.tmAveCharWidth);
    put16(o+12,(WORD)m.tmMaxCharWidth); put16(o+14,(WORD)m.tmWeight);
    o[16]=m.tmItalic; o[17]=m.tmUnderlined; o[18]=m.tmStruckOut; o[19]=m.tmFirstChar; o[20]=m.tmLastChar;
    o[21]=m.tmDefaultChar; o[22]=m.tmBreakChar; o[23]=m.tmPitchAndFamily; o[24]=m.tmCharSet;
    put16(o+25,(WORD)m.tmOverhang); put16(o+27,(WORD)m.tmDigitizedAspectX); put16(o+29,(WORD)m.tmDigitizedAspectY);
}
DWORD W16_GetTextMetrics(Args16 *a) {
    TEXTMETRIC m; BYTE *o=(BYTE *)PTR(a->a[1]);
    if(!o || !GetTextMetrics((HDC)HGDI32(a->a[0]),&m)) return 0;
    textmetric_out(o,&m);
    return 1;
}
/* Fonts to a 16-bit callback: LOGFONT and TEXTMETRIC (NEWTEXTMETRIC for
 * EnumFontFamilies, whose additions are zero for raster fonts) in the
 * task's scratch memory; returns 0 to stop. */
typedef struct {Task16 *task; DWORD proc,lp; WORD tm_bytes;} FontEnum16;
static int CALLBACK font_proc(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    static const BYTE sizes[4]={4,4,2,4}; FontEnum16 *e=(FontEnum16 *)lp; Task16 *t=e->task;
    WORD mark=t->scratch_top; DWORD args[4]; BYTE *l16,*t16; int r=0;
    if((l16=(BYTE *)Scratch16(t,LOGFONT16,&args[0]))!=NULL && (t16=(BYTE *)Scratch16(t,e->tm_bytes,&args[1]))!=NULL) {
        LogFontOut16(l16,lf); textmetric_out(t16,tm);
        args[2]=(WORD)type; args[3]=e->lp;
        r=(short)LOWORD(Call16(t,e->proc,4,args,sizes));
    }
    t->scratch_top=mark;
    return r;
}
static DWORD enum_fonts(Args16 *a,WORD tm_bytes) {
    FontEnum16 e; e.task=a->task; e.proc=a->a[2]; e.lp=a->a[3]; e.tm_bytes=tm_bytes;
    if(!e.proc) return 0;
    return (WORD)EnumFonts((HDC)HGDI32(a->a[0]),(LPCSTR)PTR(a->a[1]),font_proc,(LPARAM)&e);
}
DWORD W16_EnumFonts(Args16 *a) {return enum_fonts(a,TEXTMETRIC16);}
DWORD W16_EnumFontFamilies(Args16 *a) {return enum_fonts(a,TEXTMETRIC16+10);}

/* --- arrays -------------------------------------------------------------------- */
/* Points and integers of 16 bits, widened; small arrays stay on the stack. */
#define SMALL 256
static void *array_alloc(void *small,unsigned count,unsigned size) {
    return count<=SMALL?small:(void *)GlobalAlloc(GMEM_FIXED,(DWORD)count*size);
}
static void array_free(void *p,void *small) {if(p && p!=small) GlobalFree((HGLOBAL)p);}
static POINT *points_in(DWORD p,int n,POINT *small) {
    const BYTE *s=(const BYTE *)PTR(p); POINT *pt; int i;
    if(!s || n<=0 || !(pt=(POINT *)array_alloc(small,(unsigned)n,sizeof(POINT)))) return NULL;
    for(i=0;i<n;i++) {pt[i].x=(short)get16(s+i*4); pt[i].y=(short)get16(s+i*4+2);}
    return pt;
}
static int *ints_in(DWORD p,int n,int *small) {
    const BYTE *s=(const BYTE *)PTR(p); int *v,i;
    if(!s || n<=0 || !(v=(int *)array_alloc(small,(unsigned)n,sizeof(int)))) return NULL;
    for(i=0;i<n;i++) v[i]=(short)get16(s+i*2);
    return v;
}
static DWORD points_call(Args16 *a,BOOL (WINAPI *fn)(HDC,const POINT FAR *,int)) {
    POINT small[SMALL],*pt=points_in(a->a[1],(int)a->a[2],small); BOOL r;
    if(!pt) return 0;
    r=fn((HDC)HGDI32(a->a[0]),pt,(int)a->a[2]);
    array_free(pt,small);
    return (DWORD)r;
}
DWORD W16_Polygon(Args16 *a) {return points_call(a,Polygon);}
DWORD W16_Polyline(Args16 *a) {return points_call(a,Polyline);}
/* Polygons: the points of all of them, then a count for each. */
static int total_points(DWORD counts,int n,int *small,int **out) {
    int *c=ints_in(counts,n,small),i,total=0;
    if(!c) return 0;
    for(i=0;i<n;i++) total+=c[i];
    *out=c;
    return total;
}
DWORD W16_PolyPolygon(Args16 *a) {
    int csmall[SMALL],*c=NULL,total=total_points(a->a[2],(int)a->a[3],csmall,&c); POINT small[SMALL],*pt; BOOL r=FALSE;
    if(total>0 && (pt=points_in(a->a[1],total,small))!=NULL) {
        r=PolyPolygon((HDC)HGDI32(a->a[0]),pt,c,(int)a->a[3]);
        array_free(pt,small);
    }
    array_free(c,csmall);
    return (DWORD)r;
}
DWORD W16_CreatePolyPolygonRgn(Args16 *a) {
    int csmall[SMALL],*c=NULL,total=total_points(a->a[1],(int)a->a[2],csmall,&c); POINT small[SMALL],*pt; HRGN r=NULL;
    if(total>0 && (pt=points_in(a->a[0],total,small))!=NULL) {
        r=CreatePolyPolygonRgn(pt,c,(int)a->a[2],(int)a->a[3]);
        array_free(pt,small);
    }
    array_free(c,csmall);
    return HGDI16(r);
}
DWORD W16_CreatePolygonRgn(Args16 *a) {
    POINT small[SMALL],*pt=points_in(a->a[0],(int)a->a[1],small); HRGN r;
    if(!pt) return 0;
    r=CreatePolygonRgn(pt,(int)a->a[1],(int)a->a[2]);
    array_free(pt,small);
    return HGDI16(r);
}
/* LPtoDP and DPtoLP convert POINTs of shorts in place. */
static DWORD convert_points(Args16 *a,BOOL (WINAPI *fn)(HDC,LPPOINT,int)) {
    POINT small[SMALL],*pt=points_in(a->a[1],(int)a->a[2],small); BYTE *o=(BYTE *)PTR(a->a[1]); int i; BOOL r;
    if(!pt) return 0;
    r=fn((HDC)HGDI32(a->a[0]),pt,(int)a->a[2]);
    if(r) for(i=0;i<(int)a->a[2];i++) {put16(o+i*4,(WORD)pt[i].x); put16(o+i*4+2,(WORD)pt[i].y);}
    array_free(pt,small);
    return (DWORD)r;
}
DWORD W16_LPtoDP(Args16 *a) {return convert_points(a,LPtoDP);}
DWORD W16_DPtoLP(Args16 *a) {return convert_points(a,DPtoLP);}
/* LineDDA's callback: (x, y, data), Pascal. */
typedef struct {Task16 *task; DWORD proc,lp;} LineEnum16;
static void CALLBACK line_proc(int x,int y,LPARAM lp) {
    static const BYTE sizes[3]={2,2,4}; LineEnum16 *e=(LineEnum16 *)lp; DWORD args[3];
    args[0]=(WORD)x; args[1]=(WORD)y; args[2]=e->lp;
    Call16(e->task,e->proc,3,args,sizes);
}
DWORD W16_LineDDA(Args16 *a) {
    LineEnum16 e; e.task=a->task; e.proc=a->a[4]; e.lp=a->a[5];
    if(e.proc) LineDDA((int)a->a[0],(int)a->a[1],(int)a->a[2],(int)a->a[3],line_proc,(LPARAM)&e);
    return 0;
}
/* Relative coordinates were never implemented by Windows' display drivers. */
DWORD W16_SetRelAbs(Args16 *a) {(void)a; return 1;}
DWORD W16_GetRelAbs(Args16 *a) {(void)a; return 1;}
/* IsGDIObject: Windows 3.1's object type numbers, 0 for none. */
DWORD W16_IsGDIObject(Args16 *a) {
    switch(GetObjectType(HGDI32(a->a[0]))) {
    case OBJ_PEN: return 1;
    case OBJ_BRUSH: return 2;
    case OBJ_FONT: return 3;
    case OBJ_PAL: return 4;
    case OBJ_BITMAP: return 5;
    case OBJ_REGION: return 6;
    case OBJ_DC: return 7;
    default: return 0;
    }
}
DWORD W16_GetCharWidth(Args16 *a) {
    UINT first=(UINT)a->a[1],last=(UINT)a->a[2],i,n; int small[SMALL],*w; BYTE *o=(BYTE *)PTR(a->a[3]); BOOL r;
    if(!o || last<first) return 0;
    n=last-first+1;
    if(!(w=(int *)array_alloc(small,n,sizeof(int)))) return 0;
    r=GetCharWidth((HDC)HGDI32(a->a[0]),first,last,w);
    if(r) for(i=0;i<n;i++) put16(o+i*2,(WORD)w[i]);
    array_free(w,small);
    return (DWORD)r;
}
/* hdc, x, y, options, rectangle, string, count, spacing. */
DWORD W16_ExtTextOut(Args16 *a) {
    RECT r; BOOL has_rect=RectIn16(a->a[4],&r),result; int small[SMALL],*dx=NULL; UINT count=(UINT)a->a[6];
    if(a->a[7] && count && !(dx=ints_in(a->a[7],(int)count,small))) return 0;
    result=ExtTextOut((HDC)HGDI32(a->a[0]),(int)a->a[1],(int)a->a[2],(UINT)a->a[3],has_rect?&r:NULL,
                      (LPCSTR)PTR(a->a[5]),count,dx);
    array_free(dx,small);
    return (DWORD)result;
}
/* MulDiv rounds; it gives -32768 for a zero divisor or an overflow. */
DWORD W16_MulDiv(Args16 *a) {
    LONG n=(LONG)(int)a->a[0]*(LONG)(int)a->a[1],d=(LONG)(int)a->a[2],q;
    if(!d) return 0x8000;
    q=((n<0?-n:n)+(d<0?-d:d)/2)/(d<0?-d:d);
    if((n<0)!=(d<0)) q=-q;
    return q<-32768 || q>32767?0x8000:(WORD)q;
}

/* --- device-independent bitmaps ------------------------------------------------ */
#define BI_BITFIELDS16 3
typedef struct {BITMAPINFOHEADER h; DWORD colors[256];} Info;
/* The size of a BITMAPINFO: its header and color table; 0 when invalid. */
static DWORD info_size(const BYTE *s,UINT usage) {
    DWORD size=get32(s),colors,bpp,entry,bytes;
    if(size==sizeof(BITMAPCOREHEADER)) {bpp=get16(s+10); colors=bpp<=8?1u<<bpp:0; entry=usage==DIB_PAL_COLORS?2:3;}
    else if(size>=sizeof(BITMAPINFOHEADER) && size<=sizeof(BITMAPINFOHEADER)+64) {
        bpp=get16(s+14); colors=bpp<=8?(get32(s+32)?get32(s+32):1u<<bpp):0; entry=usage==DIB_PAL_COLORS?2:4;
        if(get32(s+16)==BI_BITFIELDS16) {colors=3; entry=4;}
    } else return 0;
    if(colors>256) colors=256;
    bytes=size+colors*entry;
    return bytes<=sizeof(Info)?bytes:0;
}
/* The BITMAPINFO at p copied to i. */
static const BITMAPINFO *info_in(DWORD p,Info *i,UINT usage) {
    const BYTE *s=(const BYTE *)PTR(p); DWORD bytes;
    if(!s || !(bytes=info_size(s,usage))) return NULL;
    memcpy(i,s,bytes);
    return (const BITMAPINFO *)i;
}
DWORD W16_CreateDIBitmap(Args16 *a) {
    Info header,info; const BITMAPINFO *h=info_in(a->a[1],&header,DIB_RGB_COLORS),*i=NULL;
    HBITMAP b;
    if(a->a[4]) i=info_in(a->a[4],&info,(UINT)a->a[5]);
    b=CreateDIBitmap((HDC)HGDI32(a->a[0]),h?&h->bmiHeader:NULL,a->a[2],PTR(a->a[3]),i,(UINT)a->a[5]);
    return HGDI16(b);
}
DWORD W16_SetDIBits(Args16 *a) {
    Info info; const BITMAPINFO *i=info_in(a->a[5],&info,(UINT)a->a[6]);
    if(!i) return 0;
    return (DWORD)SetDIBits((HDC)HGDI32(a->a[0]),(HBITMAP)HGDI32(a->a[1]),(UINT)a->a[2],(UINT)a->a[3],PTR(a->a[4]),i,(UINT)a->a[6]);
}
DWORD W16_GetDIBits(Args16 *a) {
    Info info; const BITMAPINFO *i=info_in(a->a[5],&info,(UINT)a->a[6]); int r; DWORD bytes;
    if(!i) return 0;
    r=GetDIBits((HDC)HGDI32(a->a[0]),(HBITMAP)HGDI32(a->a[1]),(UINT)a->a[2],(UINT)a->a[3],PTR(a->a[4]),(BITMAPINFO *)&info,(UINT)a->a[6]);
    bytes=info_size((const BYTE *)&info,(UINT)a->a[6]);
    memcpy(PTR(a->a[5]),&info,bytes?bytes:sizeof(BITMAPINFOHEADER));
    return (DWORD)r;
}
DWORD W16_SetDIBitsToDevice(Args16 *a) {
    Info info; const BITMAPINFO *i=info_in(a->a[10],&info,(UINT)a->a[11]);
    if(!i) return 0;
    return (DWORD)SetDIBitsToDevice((HDC)HGDI32(a->a[0]),(int)a->a[1],(int)a->a[2],(DWORD)(int)a->a[3],(DWORD)(int)a->a[4],
                                    (int)a->a[5],(int)a->a[6],(UINT)a->a[7],(UINT)a->a[8],PTR(a->a[9]),i,(UINT)a->a[11]);
}
DWORD W16_StretchDIBits(Args16 *a) {
    Info info; const BITMAPINFO *i=info_in(a->a[10],&info,(UINT)a->a[11]);
    if(!i) return 0;
    return (DWORD)StretchDIBits((HDC)HGDI32(a->a[0]),(int)a->a[1],(int)a->a[2],(int)a->a[3],(int)a->a[4],(int)a->a[5],
                                (int)a->a[6],(int)a->a[7],(int)a->a[8],PTR(a->a[9]),i,(UINT)a->a[11],a->a[12]);
}

/* A DIB pattern brush: the packed DIB in a global block, copied to aligned memory. */
DWORD W16_CreateDIBPatternBrush(Args16 *a) {
    WORD sel=(WORD)a->a[0]; DWORD size=GlobalSize16(sel); void *copy; HBRUSH b=NULL;
    if(!size || !(copy=(void *)GlobalAlloc(GMEM_FIXED,size))) return 0;
    memcpy(copy,SelPointer(sel,0),size);
    b=CreateDIBPatternBrushPt(copy,(UINT)a->a[1]);
    GlobalFree((HGLOBAL)copy);
    return HGDI16(b);
}

/* --- metafiles ---------------------------------------------------------------------- */
/* A Win16 metafile handle is a global block of the metafile's bits, as in
 * Windows 3.0: GetMetaFileBits and SetMetaFileBits give the same block
 * back, and DeleteMetaFile frees it. GDI's own metafile is made from the
 * bits to play them; a metafile closed or read from a file comes back as a
 * new block of the task's. */
HMETAFILE Metafile32(WORD h) {
    DWORD size=h?GlobalSize16(h):0;
    return size?SetMetaFileBitsEx(size,(const BYTE *)SelPointer(h,0)):NULL;
}
WORD Metafile16(Task16 *t,HMETAFILE mf,BOOL keep) {
    UINT size=mf?GetMetaFileBitsEx(mf,0,NULL):0; WORD sel=0;
    if(size && (sel=GlobalAlloc16(t,0,size))!=0) GetMetaFileBitsEx(mf,size,SelPointer(sel,0));
    if(mf && !keep) DeleteMetaFile(mf);
    return sel;
}
DWORD W16_CloseMetaFile(Args16 *a) {return Metafile16(a->task,CloseMetaFile((HDC)HGDI32(a->a[0])),FALSE);}
DWORD W16_GetMetaFile(Args16 *a) {return Metafile16(a->task,GetMetaFile((LPCSTR)PTR(a->a[0])),FALSE);}
DWORD W16_DeleteMetaFile(Args16 *a) {return a->a[0] && GlobalSize16((WORD)a->a[0]) && GlobalFree16((WORD)a->a[0]);}
DWORD W16_GetMetaFileBits(Args16 *a) {return (WORD)a->a[0];}
DWORD W16_SetMetaFileBits(Args16 *a) {return (WORD)a->a[0];}
DWORD W16_PlayMetaFile(Args16 *a) {
    HMETAFILE mf=Metafile32((WORD)a->a[1]); BOOL r;
    if(!mf) return 0;
    r=PlayMetaFile((HDC)HGDI32(a->a[0]),mf);
    DeleteMetaFile(mf);
    return (DWORD)r;
}
DWORD W16_CopyMetaFile(Args16 *a) {
    HMETAFILE mf=Metafile32((WORD)a->a[0]),copy;
    if(!mf) return 0;
    copy=CopyMetaFile(mf,(LPCSTR)PTR(a->a[1]));
    DeleteMetaFile(mf);
    return Metafile16(a->task,copy,FALSE);
}
/* A record played with a 16-bit handle table: its handles widened and back. */
DWORD W16_PlayMetaFileRecord(Args16 *a) {
    BYTE *t16=(BYTE *)PTR(a->a[1]); UINT n=(UINT)a->a[3],i; HANDLETABLE *t;
    if(!t16 || !PTR(a->a[2]) || !(t=(HANDLETABLE *)GlobalAlloc(GPTR,(DWORD)(n+1)*sizeof(HGDIOBJ)))) return 0;
    for(i=0;i<n;i++) t->objectHandle[i]=HGDI32(get16(t16+2*i));
    PlayMetaFileRecord((HDC)HGDI32(a->a[0]),t,(METARECORD *)PTR(a->a[2]),n);
    for(i=0;i<n;i++) put16(t16+2*i,(WORD)HGDI16(t->objectHandle[i]));
    GlobalFree((HGLOBAL)t);
    return 0;
}
/* EnumMetaFile: GDI walks the records of its copy; the 16-bit callback gets
 * the same record in the block (the records are in step) and a 16-bit
 * handle table in the task's scratch memory, read back after. */
typedef struct {Task16 *task; DWORD proc,lp,at; WORD sel,dc;} MetaEnum16;
static int CALLBACK meta_proc(HDC dc,HANDLETABLE FAR *t,METARECORD UNALIGNED FAR *r,int n,LPARAM lp) {
    static const BYTE sizes[5]={2,4,4,2,4}; MetaEnum16 *e=(MetaEnum16 *)lp; Task16 *task=e->task;
    WORD mark=task->scratch_top; DWORD args[5]; BYTE *t16; int i,result=0;
    (void)dc;
    if((t16=(BYTE *)Scratch16(task,(WORD)(2*n+2),&args[1]))!=NULL) {
        for(i=0;i<n;i++) put16(t16+2*i,(WORD)HGDI16(t->objectHandle[i]));
        args[0]=e->dc; args[2]=MAKELONG(LOWORD(e->at),e->sel+HIWORD(e->at)*8); args[3]=(WORD)n; args[4]=e->lp;
        result=(short)LOWORD(Call16(task,e->proc,5,args,sizes));
        for(i=0;i<n;i++) t->objectHandle[i]=HGDI32(get16(t16+2*i));
    }
    task->scratch_top=mark;
    e->at+=get32((const BYTE *)r)*2;
    return result;
}
DWORD W16_EnumMetaFile(Args16 *a) {
    MetaEnum16 e; HMETAFILE mf=Metafile32((WORD)a->a[1]); BOOL r;
    if(!mf || !a->a[2]) {if(mf) DeleteMetaFile(mf); return 0;}
    e.task=a->task; e.proc=a->a[2]; e.lp=a->a[3]; e.sel=(WORD)a->a[1]; e.dc=(WORD)a->a[0];
    e.at=2*(DWORD)get16((const BYTE *)SelPointer(e.sel,0)+2);
    r=EnumMetaFile((HDC)HGDI32(a->a[0]),mf,meta_proc,(LPARAM)&e);
    DeleteMetaFile(mf);
    return (DWORD)r;
}

/* --- printing ---------------------------------------------------------------------- */
/* DEVMODE has the same fields in both worlds; it is copied to aligned memory. */
static const DEVMODE *devmode_in(DWORD p16,DEVMODE *dm) {
    const BYTE *p=(const BYTE *)PTR(p16); UINT n;
    if(!p) return NULL;
    memset(dm,0,sizeof(*dm));
    n=get16(p+36);
    memcpy(dm,p,n<sizeof(*dm)?n:sizeof(*dm));
    return dm;
}
DWORD W16_CreateDC(Args16 *a) {
    DEVMODE dm;
    return HGDI16(CreateDC((LPCSTR)PTR(a->a[0]),(LPCSTR)PTR(a->a[1]),(LPCSTR)PTR(a->a[2]),devmode_in(a->a[3],&dm)));
}
DWORD W16_CreateIC(Args16 *a) {
    DEVMODE dm;
    return HGDI16(CreateIC((LPCSTR)PTR(a->a[0]),(LPCSTR)PTR(a->a[1]),(LPCSTR)PTR(a->a[2]),devmode_in(a->a[3],&dm)));
}
/* Abort procedures, (hdc, error) Pascal: one native procedure calls the
 * 16-bit one set for the DC. */
#define ABORTS 16
typedef struct {HDC dc; Task16 *task; DWORD proc;} Abort16;
static Abort16 aborts[ABORTS];
static BOOL CALLBACK abort16(HDC h,int code) {
    static const BYTE sizes[2]={2,2}; DWORD args[2]; int i;
    for(i=0;i<ABORTS;i++) if(aborts[i].proc && aborts[i].dc==h) {
        args[0]=HGDI16(h); args[1]=(WORD)code;
        return LOWORD(Call16(aborts[i].task,aborts[i].proc,2,args,sizes))!=0;
    }
    return TRUE;
}
static int set_abort16(Task16 *t,HDC h,DWORD proc) {
    int i,unused=-1;
    if(GetObjectType(h)!=OBJ_DC) return SP_ERROR;
    for(i=0;i<ABORTS && aborts[i].dc!=h;i++)
        if(unused<0 && (!aborts[i].proc || GetObjectType(aborts[i].dc)!=OBJ_DC)) unused=i;
    if(i==ABORTS) {if(unused<0) return SP_ERROR; i=unused;}
    aborts[i].dc=h; aborts[i].task=t; aborts[i].proc=proc;
    return SetAbortProc(h,proc?abort16:NULL);
}
/* Escapes whose data has 16-bit fields: ints, POINTs and the band's RECT of
 * shorts, and the abort procedure; the others carry bytes and strings. */
DWORD W16_Escape(Args16 *a) {
    HDC h=(HDC)HGDI32(a->a[0]); int code=(int)(LONG)a->a[1],count=(int)(LONG)a->a[2],n,r;
    const BYTE *in=(const BYTE *)PTR(a->a[3]); BYTE *out=(BYTE *)PTR(a->a[4]); POINT pt; RECT rc;
    switch(code) {
    case QUERYESCSUPPORT:
        if(!in) return 0;
        n=(short)get16(in);
        return (DWORD)Escape(h,code,sizeof(n),(LPCSTR)&n,NULL);
    case SETABORTPROC: return (DWORD)set_abort16(a->task,h,a->raw[3]);
    case SETCOPYCOUNT: case DRAFTMODE:
        n=in?(short)get16(in):1;
        r=Escape(h,code,sizeof(n),(LPCSTR)&n,&n);
        if(out) put16(out,(WORD)n);
        return (DWORD)r;
    case GETPHYSPAGESIZE: case GETPRINTINGOFFSET: case GETSCALINGFACTOR:
        r=Escape(h,code,0,NULL,&pt);
        PointOut16(a->a[4],&pt);
        return (DWORD)r;
    case NEXTBAND:
        r=Escape(h,code,0,NULL,&rc);
        RectOut16(a->a[4],&rc);
        return (DWORD)r;
    }
    return (DWORD)Escape(h,code,count,(LPCSTR)in,out);
}
/* DOCINFO: its size (a short), the document's name and the output (far pointers). */
DWORD W16_StartDoc(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[1]); DOCINFO d;
    memset(&d,0,sizeof(d)); d.cbSize=sizeof(d);
    if(p) {
        d.lpszDocName=(LPCSTR)Lin16(get32(p+2));
        if(get16(p)>=10) d.lpszOutput=(LPCSTR)Lin16(get32(p+6));
    }
    return (DWORD)StartDoc((HDC)HGDI32(a->a[0]),&d);
}
DWORD W16_SetAbortProc(Args16 *a) {return (DWORD)set_abort16(a->task,(HDC)HGDI32(a->a[0]),a->a[1]);}

/* --- PSCRIPT.DRV: the printer driver's own entry points ----------------------------------- */
/* Its setup dialog is COMMDLG's Print Setup for the printer, whose OK keeps
 * the settings through GDI's ExtDeviceMode; IDOK or IDCANCEL. */
static int setup_dialog(HWND owner,LPCSTR device,LPCSTR port) {
    PRINTDLG pd; HGLOBAL names; DEVNAMES *n; int a=lstrlen("PSCRIPT")+1,b=lstrlen(device)+1,c=lstrlen(port)+1,r;
    if(!(names=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,(DWORD)(sizeof(DEVNAMES)+a+b+c)))) return IDCANCEL;
    n=(DEVNAMES *)GlobalLock(names);
    n->wDriverOffset=sizeof(DEVNAMES); n->wDeviceOffset=(WORD)(sizeof(DEVNAMES)+a); n->wOutputOffset=(WORD)(sizeof(DEVNAMES)+a+b);
    lstrcpy((LPSTR)n+n->wDriverOffset,"PSCRIPT"); lstrcpy((LPSTR)n+n->wDeviceOffset,device); lstrcpy((LPSTR)n+n->wOutputOffset,port);
    GlobalUnlock(names);
    memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=owner; pd.Flags=PD_PRINTSETUP; pd.hDevNames=names;
    r=PrintDlg(&pd)?IDOK:IDCANCEL;
    if(pd.hDevMode) GlobalFree(pd.hDevMode);
    if(pd.hDevNames) GlobalFree(pd.hDevNames);
    return r;
}
DWORD W16_DeviceMode(Args16 *a) {
    LPCSTR device=(LPCSTR)PTR(a->a[2]),port=(LPCSTR)PTR(a->a[3]);
    setup_dialog(HWND32(a->a[0]),device?device:"PostScript Printer",port?port:"LPT1:");
    return 0;
}
/* ExtDeviceMode: the DEVMODEs copied through aligned memory; DM_PROMPT is the dialog. */
DWORD W16_ExtDeviceMode(Args16 *a) {
    BYTE *out16=(BYTE *)PTR(a->a[2]); LPSTR device=(LPSTR)PTR(a->a[3]),port=(LPSTR)PTR(a->a[4]);
    LPSTR profile=(LPSTR)PTR(a->a[6]); WORD mode=(WORD)a->a[7]; DEVMODE in,out; BOOL has_in; int r;
    if(!mode) return sizeof(DEVMODE);
    has_in=devmode_in(a->a[5],&in)!=NULL;
    if(mode&DM_PROMPT) {
        if(setup_dialog(HWND32(a->a[0]),device?device:"PostScript Printer",port?port:"LPT1:")!=IDOK) return IDCANCEL;
        mode=(WORD)(mode&~(DM_MODIFY|DM_PROMPT));
    }
    r=ExtDeviceMode(NULL,NULL,&out,device,port,has_in?&in:NULL,profile,mode);
    if((mode&DM_COPY) && out16 && r==IDOK) memcpy(out16,&out,sizeof(out));
    return (DWORD)r;
}
/* DeviceCapabilities: the paper sizes as POINTs of shorts; the rest are the same bytes. */
DWORD W16_DeviceCapabilities(Args16 *a) {
    LPCSTR device=(LPCSTR)PTR(a->a[0]),port=(LPCSTR)PTR(a->a[1]); WORD index=(WORD)a->a[2]; BYTE *out=(BYTE *)PTR(a->a[3]);
    DWORD n;
    if(index==DC_PAPERSIZE && out) {
        POINT sizes[32]; DWORD i;
        n=DeviceCapabilities(device,port,index,NULL,NULL);
        if(n==(DWORD)-1 || n>32) return n;
        DeviceCapabilities(device,port,index,(LPSTR)sizes,NULL);
        for(i=0;i<n;i++) {put16(out+i*4,(WORD)sizes[i].x); put16(out+i*4+2,(WORD)sizes[i].y);}
        return n;
    }
    return DeviceCapabilities(device,port,index,(LPSTR)out,NULL);
}
