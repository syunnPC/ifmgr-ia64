/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bitmaps and block transfers: device-dependent bitmaps (1 or 32 bits per
 * pixel), BitBlt and StretchBlt with all 256 raster operations, PatBlt,
 * device-independent bitmaps (1, 4, 8, 24 and 32 bits, BI_RGB, RLE4/RLE8,
 * Windows and OS/2 headers), and the screen: the changed area and the
 * software cursor are sent by GdiFlush.
 */
#include "gdip.h"

Surface screen;
RECT dirty;
static DWORD *scratch;
static struct {
    int x,y,hot_x,hot_y,visible; RECT drawn;
    BYTE and_mask[GDI_CURSOR_SIZE*GDI_CURSOR_SIZE/8],xor_mask[GDI_CURSOR_SIZE*GDI_CURSOR_SIZE/8];
} cursor;

void mark(const RECT *r) {r_union(&dirty,&dirty,r);}
BOOL surface_alloc(Surface *s,int width,int height,BOOL mono) {
    if(width<1) width=1;
    if(height<1) height=1;
    s->bits=(DWORD *)gdi_alloc((DWORD)width*(DWORD)height*4);
    s->width=width; s->height=height; s->stride=width; s->mono=mono;
    return s->bits!=NULL;
}
void surface_free(Surface *s) {gdi_free(s->bits); s->bits=NULL;}
/* White when brighter than middle gray. */
DWORD to_mono(DWORD v) {
    return ((v>>16&0xff)*30+(v>>8&0xff)*59+(v&0xff)*11)>=128*100?0xffffff:0;
}
int bitmap_stride(int width,BOOL mono) {return mono?((width+15)/16)*2:width*4;}

/* --- device-dependent bitmaps ------------------------------------------------ */
static HBITMAP new_bitmap(int width,int height,BOOL mono,Bitmap **out) {
    Bitmap *b; HBITMAP h=(HBITMAP)make_object(OBJ_BITMAP,sizeof(Bitmap),(void **)&b);
    if(!h) return NULL;
    if(!surface_alloc(&b->s,width,height,mono)) {DeleteObject(h); return NULL;}
    if(out) *out=b;
    return h;
}
/* Rows of bmWidthBytes: 1 bit per pixel (1 = white) or 32-bit BGRX;
 * 24-bit rows (padded to a word) are also accepted when creating. */
static void put_bits(Surface *s,int bpp,const BYTE *bits,DWORD bytes) {
    int x,y,stride=bpp==1?((s->width+15)/16)*2:bpp==24?((s->width*3+1)/2)*2:s->width*4;
    for(y=0;y<s->height;y++) {
        const BYTE *row=bits+(ULONG_PTR)y*stride; DWORD *out=s->bits+(ULONG_PTR)y*s->stride;
        if((DWORD)(y+1)*(DWORD)stride>bytes) break;
        for(x=0;x<s->width;x++)
            if(bpp==1) out[x]=row[x/8]&(0x80>>(x%8))?0xffffff:0;
            else if(bpp==24) out[x]=row[x*3]|(DWORD)row[x*3+1]<<8|(DWORD)row[x*3+2]<<16;
            else out[x]=((const DWORD *)row)[x]&0xffffff;
    }
}
HBITMAP WINAPI CreateBitmap(int width,int height,UINT planes,UINT bpp,const void FAR *bits) {
    Bitmap *b; HBITMAP h; UINT depth=planes*bpp;
    if(width<0 || height<0 || (depth!=1 && depth!=24 && depth!=32)) return NULL;
    h=new_bitmap(width,height,depth==1,&b);
    if(h && bits) put_bits(&b->s,(int)depth,(const BYTE *)bits,0xffffffffU);
    return h;
}
HBITMAP WINAPI CreateBitmapIndirect(const BITMAP FAR *bm) {
    return bm?CreateBitmap((int)bm->bmWidth,(int)bm->bmHeight,bm->bmPlanes,bm->bmBitsPixel,bm->bmBits):NULL;
}
HBITMAP WINAPI CreateCompatibleBitmap(HDC h,int width,int height) {
    DC *dc=dc_of(h);
    if(!dc || width<0 || height<0) return NULL;
    return new_bitmap(width,height,dc->surface->mono,NULL);
}
HBITMAP WINAPI CreateDiscardableBitmap(HDC h,int width,int height) {return CreateCompatibleBitmap(h,width,height);}
/* A bitmap's dimension in 0.1 mm units: kept for the application only. */
BOOL WINAPI SetBitmapDimensionEx(HBITMAP h,int x,int y,LPSIZE old) {
    Bitmap *b=(Bitmap *)object(h,OBJ_BITMAP);
    if(!b) return FALSE;
    if(old) *old=b->dimension;
    b->dimension.cx=x; b->dimension.cy=y; return TRUE;
}
BOOL WINAPI GetBitmapDimensionEx(HBITMAP h,LPSIZE out) {
    Bitmap *b=(Bitmap *)object(h,OBJ_BITMAP);
    if(!b || !out) return FALSE;
    *out=b->dimension; return TRUE;
}
DWORD WINAPI SetBitmapDimension(HBITMAP h,int x,int y) {SIZE o; return SetBitmapDimensionEx(h,x,y,&o)?MAKELONG(o.cx,o.cy):0;}
DWORD WINAPI GetBitmapDimension(HBITMAP h) {SIZE s; return GetBitmapDimensionEx(h,&s)?MAKELONG(s.cx,s.cy):0;}
LONG WINAPI GetBitmapBits(HBITMAP h,LONG count,void FAR *out) {
    Bitmap *b=(Bitmap *)object(h,OBJ_BITMAP); int x,y,stride; LONG done=0; BYTE *p=(BYTE *)out;
    if(!b || !out || count<0) return 0;
    stride=bitmap_stride(b->s.width,b->s.mono);
    for(y=0;y<b->s.height && done+stride<=count;y++,done+=stride) {
        const DWORD *row=b->s.bits+(ULONG_PTR)y*b->s.stride; BYTE *o=p+done;
        if(b->s.mono) {
            memset(o,0,(size_t)stride);
            for(x=0;x<b->s.width;x++) if(row[x]) o[x/8]|=(BYTE)(0x80>>(x%8));
        } else memcpy(o,row,(size_t)stride);
    }
    return done;
}
LONG WINAPI SetBitmapBits(HBITMAP h,DWORD count,const void FAR *bits) {
    Bitmap *b=(Bitmap *)object(h,OBJ_BITMAP); DWORD stride;
    if(!b || !bits) return 0;
    stride=(DWORD)bitmap_stride(b->s.width,b->s.mono);
    put_bits(&b->s,b->s.mono?1:32,(const BYTE *)bits,count);
    return (LONG)(min(count/stride,(DWORD)b->s.height)*stride);
}

/* --- raster operations -------------------------------------------------------- */
/* Each bit of the operation index is one combination of pattern, source and
 * destination bits (pattern 4, source 2, destination 1). */
static DWORD rop3(BYTE op,DWORD p,DWORD s,DWORD d) {
    DWORD r=0; int i;
    switch(op) {
    case 0x00: return 0;
    case 0x33: return ~s&0xffffff;
    case 0x55: return ~d&0xffffff;
    case 0x5a: return p^d;
    case 0x66: return s^d;
    case 0x88: return s&d;
    case 0xbb: return (~s|d)&0xffffff;
    case 0xc0: return p&s;
    case 0xcc: return s;
    case 0xee: return s|d;
    case 0xf0: return p;
    case 0xff: return 0xffffff;
    }
    for(i=0;i<8;i++) if(op>>i&1) r|=(i&4?p:~p)&(i&2?s:~s)&(i&1?d:~d);
    return r&0xffffff;
}
static BOOL uses_source(BYTE op) {return ((op>>2^op)&0x33)!=0;}
static BOOL uses_pattern(BYTE op) {return ((op>>4^op)&0x0f)!=0;}
/* A source: a surface, and its background color, which becomes white when
 * color is copied to monochrome. */
typedef struct {const Surface *s; DWORD bk;} Source;
/* dest: device rectangle; src rectangle in source pixels (sw,sh may be
 * negative to mirror). Nearest-neighbour stretching. */
static void blit(DC *dc,const RECT *dest,const Source *src,int sx,int sy,int sw,int sh,DWORD rop) {
    BYTE op=(BYTE)(rop>>16); Brush *brush=(Brush *)object(dc->s.brush,OBJ_BRUSH);
    Surface *d=dc->surface; DWORD *copy=NULL; const DWORD *sbits=NULL; int sstride=0,sx0=0,sy0=0,i,x,y;
    int w=dest->right-dest->left,h=dest->bottom-dest->top;
    BOOL use_s=uses_source(op),use_p=uses_pattern(op),straight;
    DWORD text=dc_pixel(dc,dc->s.text),bk=dc_pixel(dc,dc->s.bk);
    RECT c;
    if(w<=0 || h<=0) return;
    straight=use_s && op==0xcc && sw==w && sh==h && src->s->mono==d->mono;
    if(use_s) {
        if(!src) return;
        sbits=src->s->bits; sstride=src->s->stride;
        if(src->s->bits==d->bits) {
            /* Overlapping copies on one surface read from a copy of the source. */
            int l=min(sx,sx+sw),t=min(sy,sy+sh),r=max(sx,sx+sw),b=max(sy,sy+sh);
            l=max(l,0); t=max(t,0); r=min(r,src->s->width); b=min(b,src->s->height);
            if(l>=r || t>=b) return;
            copy=(DWORD *)gdi_alloc((DWORD)(r-l)*(DWORD)(b-t)*4);
            if(!copy) return;
            for(y=t;y<b;y++) memcpy(copy+(ULONG_PTR)(y-t)*(r-l),src->s->bits+(ULONG_PTR)y*sstride+l,(size_t)(r-l)*4);
            sbits=copy; sstride=r-l; sx0=l; sy0=t;
        }
    }
    for(i=0;i<dc->eff.count;i++) if(r_intersect(&c,dest,&dc->eff.rects[i])) {
        for(y=c.top;y<c.bottom;y++) {
            DWORD *row=d->bits+(ULONG_PTR)y*d->stride; const DWORD *srow=NULL; int ry=y-dest->top,yy=0;
            if(use_s) {
                yy=sh>0?sy+(int)((LONGLONG)ry*sh/h):sy-1-(int)((LONGLONG)ry*-sh/h);
                if(yy<0 || yy>=src->s->height) continue;
                srow=sbits+(ULONG_PTR)(yy-sy0)*sstride;
            }
            if(straight) {
                int l=max(c.left,dest->left-sx),r=min(c.right,dest->left-sx+src->s->width);
                if(l<r) memcpy(row+l,srow+(sx+l-dest->left-sx0),(size_t)(r-l)*4);
                continue;
            }
            for(x=c.left;x<c.right;x++) {
                DWORD s=0,p=0;
                if(use_s) {
                    int rx=x-dest->left,xx=sw>0?sx+(int)((LONGLONG)rx*sw/w):sx-1-(int)((LONGLONG)rx*-sw/w);
                    if(xx<0 || xx>=src->s->width) continue;
                    s=srow[xx-sx0];
                    if(src->s->mono && !d->mono) s=s?bk:text;
                    else if(!src->s->mono && d->mono) s=s==src->bk?0xffffff:0;
                }
                if(use_p && brush && brush->style!=BS_NULL) {
                    p=brush_pixel(brush,dc,x,y);
                    if(p==0xffffffffU) p=bk;
                    if(d->mono) p=to_mono(p);
                }
                row[x]=rop3(op,p,s,row[x]);
            }
        }
        if(dc->display) mark(&c);
    }
    gdi_free(copy);
}
static void device_rect(RECT *r,DC *dc,int x,int y,int w,int h) {
    r_set(r,x,y,x+w,y+h); dev_rect(dc,r);
}
static BOOL source_of(Source *s,HDC h) {
    DC *dc=dc_of(h);
    if(!dc) return FALSE;
    s->s=dc->surface; s->bk=dc_pixel(dc,dc->s.bk);
    return TRUE;
}
BOOL WINAPI PatBlt(HDC h,int x,int y,int w,int hh,DWORD rop) {
    DC *dc=dc_of(h); RECT r;
    if(!dc || uses_source((BYTE)(rop>>16))) return FALSE;
    if(dc->meta) return mf_words(dc,META_PATBLT,6,(int)LOWORD(rop),(int)HIWORD(rop),hh,w,y,x);
    device_rect(&r,dc,x,y,w,hh); blit(dc,&r,NULL,0,0,0,0,rop);
    return TRUE;
}
BOOL WINAPI BitBlt(HDC h,int x,int y,int w,int hh,HDC from,int sx,int sy,DWORD rop) {
    DC *dc=dc_of(h);
    if(META_DC(dc) && uses_source((BYTE)(rop>>16))) return mf_blt(dc,x,y,w,hh,from,sx,sy,w,hh,rop,FALSE);
    return StretchBlt(h,x,y,w,hh,from,sx,sy,w,hh,rop);
}
BOOL WINAPI StretchBlt(HDC h,int x,int y,int w,int hh,HDC from,int sx,int sy,int sw,int sh,DWORD rop) {
    DC *dc=dc_of(h),*sdc; Source src; RECT r;
    if(!dc) return FALSE;
    if(!uses_source((BYTE)(rop>>16))) return PatBlt(h,x,y,w,hh,rop);
    if(dc->meta) return mf_blt(dc,x,y,w,hh,from,sx,sy,sw,sh,rop,TRUE);
    if(!source_of(&src,from)) return FALSE;
    sdc=dc_of(from);
    {
        /* Both rectangles in device pixels; opposite signs mirror the image. */
        int dx=dev_x(dc,x),dy=dev_y(dc,y),dw=dev_x(dc,x+w)-dx,dh=dev_y(dc,y+hh)-dy;
        int s0=dev_x(sdc,sx),t0=dev_y(sdc,sy),sdw=dev_x(sdc,sx+sw)-s0,sdh=dev_y(sdc,sy+sh)-t0;
        if(dw<0) {dx+=dw; dw=-dw; s0+=sdw; sdw=-sdw;}
        if(dh<0) {dy+=dh; dh=-dh; t0+=sdh; sdh=-sdh;}
        r_set(&r,dx,dy,dx+dw,dy+dh);
        blit(dc,&r,&src,s0,t0,sdw,sdh,rop);
    }
    return TRUE;
}

/* --- device-independent bitmaps ------------------------------------------------- */
static const RGBQUAD vga[16]={
    {0,0,0,0},{0,0,128,0},{0,128,0,0},{0,128,128,0},{128,0,0,0},{128,0,128,0},{128,128,0,0},{192,192,192,0},
    {128,128,128,0},{0,0,255,0},{0,255,0,0},{0,255,255,0},{255,0,0,0},{255,0,255,0},{255,255,0,0},{255,255,255,0}};
typedef struct {
    int width,height,bpp,stride; BOOL top_down; DWORD compression;
    int colors; DWORD palette[256];
    const BYTE *bits; BYTE *unpacked; /* RLE images are expanded to 8 bits */
} Dib;
static DWORD quad(const RGBQUAD *q) {return (DWORD)q->rgbRed<<16|(DWORD)q->rgbGreen<<8|q->rgbBlue;}
/* The palette is read from RGBQUADs (or OS/2 RGBTRIPLEs); palette indices
 * (DIB_PAL_COLORS) select from the 16 VGA colors, not the DC's palette. */
static BOOL dib_header(Dib *d,const BITMAPINFO *info,UINT usage) {
    int i;
    memset(d,0,sizeof(*d));
    if(!info) return FALSE;
    if(info->bmiHeader.biSize==sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREINFO *core=(const BITMAPCOREINFO *)info;
        d->width=core->bmciHeader.bcWidth; d->height=core->bmciHeader.bcHeight; d->bpp=core->bmciHeader.bcBitCount;
        d->colors=d->bpp<=8?1<<d->bpp:0;
        for(i=0;i<d->colors;i++)
            if(usage==DIB_PAL_COLORS) d->palette[i]=quad(&vga[((const WORD *)core->bmciColors)[i]&15]);
            else d->palette[i]=(DWORD)core->bmciColors[i].rgbtRed<<16|(DWORD)core->bmciColors[i].rgbtGreen<<8|core->bmciColors[i].rgbtBlue;
    } else if(info->bmiHeader.biSize>=sizeof(BITMAPINFOHEADER)) {
        const BITMAPINFOHEADER *bh=&info->bmiHeader;
        const BYTE *table=(const BYTE *)info+bh->biSize;
        d->width=(int)bh->biWidth; d->height=(int)bh->biHeight; d->bpp=bh->biBitCount; d->compression=bh->biCompression;
        if(d->height<0) {d->height=-d->height; d->top_down=TRUE;}
        d->colors=d->bpp<=8?(bh->biClrUsed && bh->biClrUsed<(DWORD)(1<<d->bpp)?(int)bh->biClrUsed:1<<d->bpp):0;
        for(i=0;i<d->colors;i++)
            if(usage==DIB_PAL_COLORS) d->palette[i]=quad(&vga[((const WORD *)table)[i]&15]);
            else d->palette[i]=quad((const RGBQUAD *)table+i);
    } else return FALSE;
    if(d->width<=0 || d->height<=0) return FALSE;
    if(d->bpp!=1 && d->bpp!=4 && d->bpp!=8 && d->bpp!=24 && d->bpp!=32) return FALSE;
    if(d->compression!=BI_RGB && !(d->compression==BI_RLE8 && d->bpp==8) && !(d->compression==BI_RLE4 && d->bpp==4)) return FALSE;
    d->stride=((d->width*d->bpp+31)/32)*4;
    return TRUE;
}
/* Run-length images become 8-bit rows, bottom-up as stored. */
static BOOL dib_unpack(Dib *d,const BYTE *in,int lines) {
    int x=0,y=0; BYTE *out;
    if(d->compression==BI_RGB) {d->bits=in; return TRUE;}
    out=(BYTE *)gdi_alloc((DWORD)d->width*(DWORD)lines);
    if(!out) return FALSE;
    while(y<lines) {
        int count=*in++,value=*in++,k;
        if(count) {
            for(k=0;k<count && x<d->width;k++,x++)
                out[(ULONG_PTR)y*d->width+x]=(BYTE)(d->bpp==8?value:k&1?value&15:value>>4);
        } else if(value==0) {x=0; y++;}
        else if(value==1) break;
        else if(value==2) {x+=*in++; y+=*in++;}
        else {
            for(k=0;k<value;k++,x++) {
                int v=d->bpp==8?in[k]:k&1?in[k/2]&15:in[k/2]>>4;
                if(x<d->width) out[(ULONG_PTR)y*d->width+x]=(BYTE)v;
            }
            in+=d->bpp==8?(value+1)&~1:((value+1)/2+1)&~1;
        }
    }
    d->unpacked=out; d->bits=out; d->bpp=8; d->stride=d->width;
    return TRUE;
}
/* Pixel x of the buffer's row (rows in storage order). */
static DWORD dib_pixel(const Dib *d,int row,int x) {
    const BYTE *r=d->bits+(ULONG_PTR)row*d->stride; int i;
    switch(d->bpp) {
    case 1: i=r[x/8]>>(7-x%8)&1; break;
    case 4: i=x&1?r[x/2]&15:r[x/2]>>4; break;
    case 8: i=r[x]; break;
    case 24: return (DWORD)r[x*3+2]<<16|(DWORD)r[x*3+1]<<8|r[x*3];
    default: return ((const DWORD *)r)[x]&0xffffff;
    }
    return i<d->colors?d->palette[i]:0;
}
/* Scan lines [start,start+lines) of the DIB as a top-down surface of the
 * whole image height; rows outside the given scans are left black. */
static BOOL dib_surface(Surface *s,Dib *d,const void *bits,UINT start,UINT lines) {
    int row,x;
    if(!bits || !surface_alloc(s,d->width,d->height,FALSE)) return FALSE;
    if(start>=(UINT)d->height) return TRUE;
    lines=min(lines,(UINT)d->height-start);
    if(!dib_unpack(d,(const BYTE *)bits,(int)lines)) {surface_free(s); return FALSE;}
    for(row=0;row<(int)lines;row++) {
        int scan=(int)start+row,y=d->top_down?scan:d->height-1-scan;
        DWORD *out=s->bits+(ULONG_PTR)y*s->stride;
        for(x=0;x<d->width;x++) out[x]=dib_pixel(d,row,x);
    }
    gdi_free(d->unpacked); d->unpacked=NULL;
    return TRUE;
}
/* A pattern brush from a packed DIB (the header, the colors, the bits): its
 * top-left eight by eight pixels, in the DIB's colors. */
HBRUSH WINAPI CreateDIBPatternBrushPt(const void FAR *packed,UINT usage) {
    const BITMAPINFO *info=(const BITMAPINFO *)packed; Dib d; Surface s; Brush *b; HBRUSH h; int x,y; DWORD table;
    if(!dib_header(&d,info,usage)) return NULL;
    table=info->bmiHeader.biSize+(DWORD)d.colors*(usage==DIB_PAL_COLORS?2:info->bmiHeader.biSize==sizeof(BITMAPCOREHEADER)?3:4);
    if(!dib_surface(&s,&d,(const BYTE *)packed+table,0,(UINT)d.height)) return NULL;
    h=(HBRUSH)make_object(OBJ_BRUSH,sizeof(Brush),(void **)&b);
    if(h) {
        b->style=BS_PATTERN;
        for(y=0;y<8;y++) for(x=0;x<8;x++) b->pattern[y*8+x]=s.bits[(ULONG_PTR)(y%s.height)*s.stride+(x%s.width)];
    }
    surface_free(&s);
    return h;
}
HBRUSH WINAPI CreateDIBPatternBrush(HGLOBAL packed,UINT usage) {
    const void *p=GlobalLock(packed); HBRUSH h=p?CreateDIBPatternBrushPt(p,usage):NULL;
    if(p) GlobalUnlock(packed);
    return h;
}
/* The scans are copied into the bitmap, converted to monochrome if it is;
 * scan 0 is the bitmap's bottom row (its top row for a top-down DIB). */
int WINAPI SetDIBits(HDC h,HBITMAP hb,UINT start,UINT lines,const void FAR *bits,const BITMAPINFO FAR *info,UINT usage) {
    Bitmap *b=(Bitmap *)object(hb,OBJ_BITMAP); Dib d; Surface s; int x,done=0,scan;
    (void)h;
    if(!b || !dib_header(&d,info,usage) || !dib_surface(&s,&d,bits,start,lines)) return 0;
    for(scan=(int)start;scan<(int)start+(int)lines && scan<d.height;scan++,done++) {
        int from=d.top_down?scan:d.height-1-scan,to=d.top_down?scan:b->s.height-1-scan;
        if(to<0 || to>=b->s.height) continue;
        for(x=0;x<d.width && x<b->s.width;x++) {
            DWORD v=s.bits[(ULONG_PTR)from*s.stride+x];
            b->s.bits[(ULONG_PTR)to*b->s.stride+x]=b->s.mono?to_mono(v):v;
        }
    }
    surface_free(&s);
    return done;
}
HBITMAP WINAPI CreateDIBitmap(HDC h,const BITMAPINFOHEADER FAR *header,DWORD init,const void FAR *bits,const BITMAPINFO FAR *info,UINT usage) {
    DC *dc=dc_of(h); Dib d; HBITMAP hb;
    if(!dib_header(&d,(const BITMAPINFO *)header,DIB_RGB_COLORS)) return NULL;
    hb=new_bitmap(d.width,d.height,dc && dc->surface->mono,NULL);
    if(hb && (init&CBM_INIT) && bits) SetDIBits(h,hb,0,(UINT)d.height,bits,info,usage);
    return hb;
}
/* The nearest of the 16 VGA colors, then a 6x6x6 cube and 24 grays. */
static void standard_palette(DWORD *palette) {
    int i;
    for(i=0;i<16;i++) palette[i]=quad(&vga[i]);
    for(i=0;i<216;i++) palette[16+i]=(DWORD)(i/36*51)<<16|(DWORD)(i/6%6*51)<<8|(DWORD)(i%6*51);
    for(i=0;i<24;i++) palette[232+i]=(DWORD)(8+i*10)*0x010101;
}
static int nearest(const DWORD *palette,int count,DWORD v) {
    int i,best=0; long best_d=0x7fffffffL;
    for(i=0;i<count;i++) {
        long dr=(long)(v>>16&0xff)-(long)(palette[i]>>16&0xff),dg=(long)(v>>8&0xff)-(long)(palette[i]>>8&0xff),db=(long)(v&0xff)-(long)(palette[i]&0xff);
        long dist=dr*dr+dg*dg+db*db;
        if(dist<best_d) {best_d=dist; best=i; if(!dist) break;}
    }
    return best;
}
/* Without bits, only the header is filled in. Color tables are written for
 * 1, 4 and 8 bits per pixel. */
int WINAPI GetDIBits(HDC h,HBITMAP hb,UINT start,UINT lines,void FAR *bits,BITMAPINFO FAR *info,UINT usage) {
    Bitmap *b=(Bitmap *)object(hb,OBJ_BITMAP); BITMAPINFOHEADER *bh; DWORD palette[256]; int colors,bpp,stride,row,x,done=0;
    (void)h; (void)usage;
    if(!b || !info || info->bmiHeader.biSize<sizeof(BITMAPINFOHEADER)) return 0;
    bh=&info->bmiHeader;
    if(!bh->biBitCount) bh->biBitCount=(WORD)(b->s.mono?1:24);
    bpp=bh->biBitCount;
    if(bpp!=1 && bpp!=4 && bpp!=8 && bpp!=24 && bpp!=32) return 0;
    stride=((b->s.width*bpp+31)/32)*4;
    bh->biWidth=b->s.width; bh->biHeight=b->s.height; bh->biPlanes=1; bh->biCompression=BI_RGB;
    bh->biSizeImage=(DWORD)stride*(DWORD)b->s.height; bh->biXPelsPerMeter=bh->biYPelsPerMeter=3780;
    colors=bpp<=8?1<<bpp:0; bh->biClrUsed=bh->biClrImportant=0;
    if(bpp==1) {palette[0]=0; palette[1]=0xffffff;}
    else if(bpp==4 || bpp==8) standard_palette(palette);
    for(x=0;x<colors;x++) {
        RGBQUAD *q=(RGBQUAD *)((BYTE *)info+bh->biSize)+x;
        q->rgbRed=(BYTE)(palette[x]>>16); q->rgbGreen=(BYTE)(palette[x]>>8); q->rgbBlue=(BYTE)palette[x]; q->rgbReserved=0;
    }
    if(!bits) return b->s.height;
    for(row=(int)start;row<(int)start+(int)lines && row<b->s.height;row++,done++) {
        const DWORD *in=b->s.bits+(ULONG_PTR)(b->s.height-1-row)*b->s.stride;
        BYTE *out=(BYTE *)bits+(ULONG_PTR)(row-(int)start)*stride;
        memset(out,0,(size_t)stride);
        for(x=0;x<b->s.width;x++) {
            DWORD v=in[x];
            switch(bpp) {
            case 1: if(to_mono(v)) out[x/8]|=(BYTE)(0x80>>(x%8)); break;
            case 4: out[x/2]|=(BYTE)(nearest(palette,16,v)<<(x&1?0:4)); break;
            case 8: out[x]=(BYTE)nearest(palette,256,v); break;
            case 24: out[x*3]=(BYTE)v; out[x*3+1]=(BYTE)(v>>8); out[x*3+2]=(BYTE)(v>>16); break;
            default: ((DWORD *)out)[x]=v;
            }
        }
    }
    return done;
}
/* Coordinates of the source are DIB coordinates: y counts from the bottom
 * of a bottom-up image. */
int WINAPI StretchDIBits(HDC h,int x,int y,int w,int hh,int sx,int sy,int sw,int sh,const void FAR *bits,const BITMAPINFO FAR *info,UINT usage,DWORD rop) {
    DC *dc=dc_of(h); Dib d; Surface s; Source src; RECT r;
    if(META_DC(dc)) return mf_dib(dc,META_STRETCHDIB,x,y,w,hh,sx,sy,sw,sh,0,0,bits,info,usage,rop)?(hh<0?-hh:hh):0;
    if(!dc || !dib_header(&d,info,usage) || !dib_surface(&s,&d,bits,0,(UINT)d.height)) return 0;
    src.s=&s; src.bk=0xffffff;
    if(w<0) {sx+=sw; sw=-sw;}
    if(hh<0) {sy+=sh; sh=-sh;}
    if(!d.top_down) {sy=d.height-sy-sh;}
    device_rect(&r,dc,x,y,w,hh);
    blit(dc,&r,&src,sx,sy,sw,sh,rop);
    surface_free(&s);
    return hh<0?-hh:hh;
}
int WINAPI SetDIBitsToDevice(HDC h,int x,int y,DWORD w,DWORD hh,int sx,int sy,UINT start,UINT lines,const void FAR *bits,const BITMAPINFO FAR *info,UINT usage) {
    DC *dc=dc_of(h); Dib d; Surface s; Source src; RECT r;
    if(META_DC(dc)) return mf_dib(dc,META_SETDIBTODEV,x,y,(int)w,(int)hh,sx,sy,0,0,start,lines,bits,info,usage,0)?(int)lines:0;
    if(!dc || !dib_header(&d,info,usage) || !dib_surface(&s,&d,bits,start,lines)) return 0;
    src.s=&s; src.bk=0xffffff;
    if(!d.top_down) sy=d.height-sy-(int)hh;
    x=dev_x(dc,x); y=dev_y(dc,y);
    r_set(&r,x,y,x+(int)w,y+(int)hh);
    blit(dc,&r,&src,sx,sy,(int)w,(int)hh,SRCCOPY);
    surface_free(&s);
    return (int)lines;
}

/* --- the screen and the cursor ---------------------------------------------------- */
static void cursor_rect(RECT *r) {
    if(!cursor.visible) {r_set(r,0,0,0,0); return;}
    r_set(r,cursor.x-cursor.hot_x,cursor.y-cursor.hot_y,cursor.x-cursor.hot_x+GDI_CURSOR_SIZE,cursor.y-cursor.hot_y+GDI_CURSOR_SIZE);
}
static void send(RECT r) {
    int x,y,w,h; RECT c;
    if(r.left<0) r.left=0;
    if(r.top<0) r.top=0;
    if(r.right>screen.width) r.right=screen.width;
    if(r.bottom>screen.height) r.bottom=screen.height;
    if(r.left>=r.right || r.top>=r.bottom) return;
    w=r.right-r.left; h=r.bottom-r.top;
    for(y=0;y<h;y++) memcpy(scratch+(ULONG_PTR)y*w,screen.bits+(ULONG_PTR)(r.top+y)*screen.stride+r.left,(size_t)w*4);
    cursor_rect(&c);
    if(r_intersect(&c,&c,&r)) for(y=c.top;y<c.bottom;y++) for(x=c.left;x<c.right;x++) {
        int cx=x-(cursor.x-cursor.hot_x),cy=y-(cursor.y-cursor.hot_y),bit=cy*GDI_CURSOR_SIZE+cx;
        DWORD *p=scratch+(ULONG_PTR)(y-r.top)*w+(x-r.left);
        if(!(cursor.and_mask[bit/8]&(0x80>>(bit%8)))) *p=0;
        if(cursor.xor_mask[bit/8]&(0x80>>(bit%8))) *p^=0xffffff;
    }
    wh_display_blt(scratch,(wh_u32)r.left,(wh_u32)r.top,(wh_u32)w,(wh_u32)h,(wh_u32)w);
}
void WINAPI GdiFlush(void) {
    static wh_u32 epoch; RECT now; wh_u32 e=wh_display_epoch();
    if(!screen.bits) return;
    /* The display was given to DOS and claimed again: send all of it. */
    if(e!=epoch) {epoch=e; r_set(&dirty,0,0,screen.width,screen.height); r_set(&cursor.drawn,0,0,0,0);}
    cursor_rect(&now);
    if(memcmp(&now,&cursor.drawn,sizeof(now))) {send(cursor.drawn); send(now); cursor.drawn=now;}
    if(!r_empty(&dirty)) {send(dirty); r_set(&dirty,0,0,0,0);}
}
void WINAPI GdiSetCursor(const BYTE *and_mask,const BYTE *xor_mask,int hot_x,int hot_y) {
    if(and_mask) memcpy(cursor.and_mask,and_mask,sizeof(cursor.and_mask));
    if(xor_mask) memcpy(cursor.xor_mask,xor_mask,sizeof(cursor.xor_mask));
    cursor.hot_x=hot_x; cursor.hot_y=hot_y;
    mark(&cursor.drawn); r_set(&cursor.drawn,0,0,0,0);
}
void WINAPI GdiMoveCursor(int x,int y,BOOL show) {cursor.x=x; cursor.y=y; cursor.visible=show;}

int WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    wh_u32 w,h;
    (void)instance; (void)reserved;
    if(reason==1) {
        if(wh_display_size(&w,&h)) return FALSE;
        screen.width=screen.stride=(int)w; screen.height=(int)h; screen.mono=FALSE;
        screen.bits=(DWORD *)wh_alloc((wh_u64)w*h*4); scratch=(DWORD *)wh_alloc((wh_u64)w*h*4);
        r_set(&dirty,0,0,0,0); memset(&cursor,0,sizeof(cursor));
        if(screen.bits && scratch && ObjectsInit()) {FontsInit(); return TRUE;}
    } else if(reason!=0) return TRUE;
    FontsEnd();
    if(screen.bits) wh_free(screen.bits,(wh_u64)screen.width*screen.height*4);
    if(scratch) wh_free(scratch,(wh_u64)screen.width*screen.height*4);
    screen.bits=scratch=NULL;
    return reason==0;
}
