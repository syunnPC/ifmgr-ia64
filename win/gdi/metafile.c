/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows metafiles ([MS-WMF]): 18-byte header, word-sized records
 * (size, function, arguments in reverse call order), then an empty record.
 * Selected objects get table slots until deletion; drawn/clipping regions
 * get a slot for one record. Blits embed DIBs. CloseMetaFile produces memory
 * bits and, for disk metafiles, the file. PlayMetaFile uses a temporary
 * object table, deletes its objects and restores DC objects/clipping.
 * Access records bytewise because they are only word-aligned.
 */
#include "gdip.h"
#include <stdarg.h>
#define HEADER 18

struct Meta {
    BYTE *bits; DWORD size,cap,max_record; BOOL failed;
    HGDIOBJ *slots; int slot_cap,objects; /* the object in each slot; slots ever used */
    char path[MAX_PATH]; /* a disk metafile's file */
    struct Meta *next;
};
static Meta *recording; /* every metafile DC, for objects deleted while in their tables */

static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
static void put16(BYTE *p,WORD v) {p[0]=(BYTE)v; p[1]=(BYTE)(v>>8);}
static void put32(BYTE *p,DWORD v) {put16(p,LOWORD(v)); put16(p+2,HIWORD(v));}

/* --- recording ----------------------------------------------------------------------- */
static BOOL grow(Meta *m,DWORD bytes) {
    DWORD cap; BYTE *n;
    if(m->failed) return FALSE;
    if(m->size+bytes<=m->cap) return TRUE;
    cap=max(m->cap*2,m->size+bytes+4096);
    if(!(n=(BYTE *)gdi_alloc(cap))) {m->failed=TRUE; return FALSE;}
    if(m->bits) {memcpy(n,m->bits,m->size); gdi_free(m->bits);}
    m->bits=n; m->cap=cap;
    return TRUE;
}
/* A record with room for words parameter words, zeroed: where they go, or NULL. */
static BYTE *record(Meta *m,WORD function,DWORD words) {
    BYTE *r; DWORD size=3+words;
    if(!grow(m,size*2)) return NULL;
    r=m->bits+m->size; m->size+=size*2;
    memset(r,0,size*2); put32(r,size); put16(r+4,function);
    if(size>m->max_record) m->max_record=size;
    return r+6;
}
/* n words, in the record's order. */
BOOL mf_words(DC *dc,WORD function,int n,...) {
    va_list ap; BYTE *p=record(dc->meta,function,(DWORD)n); int i;
    if(!p) return FALSE;
    va_start(ap,n);
    for(i=0;i<n;i++) put16(p+2*i,(WORD)va_arg(ap,int));
    va_end(ap);
    return TRUE;
}
/* An attribute: one word, or two for a color or flags. */
void mf_value(DC *dc,WORD function,DWORD v) {
    if(function>>8==2) mf_words(dc,function,2,(int)LOWORD(v),(int)HIWORD(v));
    else mf_words(dc,function,1,(int)LOWORD(v));
}

/* The object table: the lowest free slot from first on. */
static int new_slot(Meta *m,int first) {
    int i=first;
    while(i<m->slot_cap && m->slots[i]) i++;
    if(i>=m->slot_cap) {
        int cap=max(m->slot_cap+16,i+1); HGDIOBJ *n=(HGDIOBJ *)gdi_alloc((DWORD)cap*sizeof(HGDIOBJ));
        if(!n) {m->failed=TRUE; return -1;}
        if(m->slots) {memcpy(n,m->slots,(size_t)m->slot_cap*sizeof(HGDIOBJ)); gdi_free(m->slots);}
        m->slots=n; m->slot_cap=cap;
    }
    if(i>=m->objects) m->objects=i+1;
    return i;
}
static int slot_of(const Meta *m,HGDIOBJ h) {
    int i;
    for(i=0;i<m->slot_cap;i++) if(m->slots[i]==h) return i;
    return -1;
}
/* A DIB of a surface's rectangle (what lies outside it black): 24 bits a
 * pixel, or 1 for monochrome, bottom-up. Its size; written to out unless NULL. */
static DWORD surface_dib(const Surface *s,int l,int t,int w,int h,BYTE *out) {
    int bpp=s->mono?1:24,stride=((w*bpp+31)/32)*4,colors=s->mono?2:0,x,y;
    DWORD size=40+(DWORD)colors*4+(DWORD)stride*(DWORD)h;
    if(!out) return size;
    put32(out,40); put32(out+4,(DWORD)w); put32(out+8,(DWORD)h); put16(out+12,1); put16(out+14,(WORD)bpp);
    put32(out+20,(DWORD)stride*(DWORD)h);
    if(colors) {put32(out+40,0); put32(out+44,0xffffff); put32(out+32,2);}
    for(y=0;y<h;y++) {
        BYTE *row=out+40+colors*4+(ULONG_PTR)(h-1-y)*stride; int sy=t+y;
        for(x=0;x<w;x++) {
            int sx=l+x; DWORD p=sx>=0 && sx<s->width && sy>=0 && sy<s->height?s->bits[(ULONG_PTR)sy*s->stride+sx]:0;
            if(bpp==1) {if(p) row[x/8]|=(BYTE)(0x80>>(x%8));}
            else {row[x*3]=(BYTE)p; row[x*3+1]=(BYTE)(p>>8); row[x*3+2]=(BYTE)(p>>16);}
        }
    }
    return size;
}
/* A region's record: its rectangles cut into bands, each band's runs of x merged. */
static BOOL region_record(Meta *m,const RectList *r,int slot) {
    int *ys,ny=0,i,j,k,scans=0,most=0; int *xs; DWORD words; BYTE *p,*at; RECT box;
    ys=(int *)gdi_alloc((DWORD)(2*r->count+1)*sizeof(int)); xs=(int *)gdi_alloc((DWORD)(2*r->count+1)*sizeof(int));
    if(!ys || !xs) {gdi_free(ys); gdi_free(xs); m->failed=TRUE; return FALSE;}
    for(i=0;i<r->count;i++) {
        int v[2]; v[0]=r->rects[i].top; v[1]=r->rects[i].bottom;
        for(k=0;k<2;k++) {
            for(j=0;j<ny && ys[j]<v[k];j++) ;
            if(j<ny && ys[j]==v[k]) continue;
            memmove(ys+j+1,ys+j,(size_t)(ny-j)*sizeof(int)); ys[j]=v[k]; ny++;
        }
    }
    /* Two passes: the size, then the record. */
    for(k=0,p=NULL,at=NULL;k<2;k++) {
        words=11; scans=0;
        for(j=0;j+1<ny;j++) {
            int n=0,a,b;
            for(i=0;i<r->count;i++) if(r->rects[i].top<=ys[j] && r->rects[i].bottom>=ys[j+1]) {
                /* Insert [left,right) in order, merging. */
                int l=r->rects[i].left,rr=r->rects[i].right;
                for(a=0;a<n && xs[2*a+1]<l;a++) ;
                for(b=a;b<n && xs[2*b]<=rr;b++) {l=min(l,xs[2*b]); rr=max(rr,xs[2*b+1]);}
                memmove(xs+2*a+2,xs+2*b,(size_t)(n-b)*2*sizeof(int)); n-=b-a-1;
                xs[2*a]=l; xs[2*a+1]=rr;
            }
            if(!n) continue;
            if(at) {
                put16(at,(WORD)(2*n)); put16(at+2,(WORD)ys[j]); put16(at+4,(WORD)ys[j+1]);
                for(i=0;i<2*n;i++) put16(at+6+2*i,(WORD)xs[i]);
                put16(at+6+4*n,(WORD)(2*n)); at+=8+4*n;
            }
            words+=4+2*(DWORD)n; scans++; if(2*n>most) most=2*n;
        }
        if(!k) {
            if(!(p=record(m,META_CREATEREGION,words))) break;
            rl_box(r,&box);
            put16(p+2,6); put32(p+4,0x2f6); put16(p+8,(WORD)(words*2)); put16(p+10,(WORD)scans); put16(p+12,(WORD)most);
            put16(p+14,(WORD)box.left); put16(p+16,(WORD)box.top); put16(p+18,(WORD)box.right); put16(p+20,(WORD)box.bottom);
            at=p+22;
        }
    }
    gdi_free(ys); gdi_free(xs);
    if(!p) return FALSE;
    m->slots[slot]=(HGDIOBJ)1; /* held until the record that uses it deletes it */
    return TRUE;
}
/* An object's slot, written out first if the table does not have it. */
static int object_slot(Meta *m,HGDIOBJ h) {
    ObjHeader *o=(ObjHeader *)object(h,0); int slot; BYTE *p;
    if(!o) return -1;
    if((slot=slot_of(m,h))>=0) return slot;
    if((slot=new_slot(m,0))<0) return -1;
    switch(o->type) {
    case OBJ_PEN: {
        const Pen *pen=(const Pen *)o;
        if(!(p=record(m,META_CREATEPENINDIRECT,5))) return -1;
        put16(p,(WORD)pen->style); put16(p+2,(WORD)pen->width); put32(p+6,pen->color);
        break;
    }
    case OBJ_BRUSH: {
        const Brush *b=(const Brush *)o;
        if(b->style==BS_PATTERN) {
            /* The pattern as an 8 x 8 DIB. */
            Surface s; DWORD size;
            s.bits=(DWORD *)b->pattern; s.width=s.height=s.stride=8; s.mono=b->mono;
            size=surface_dib(&s,0,0,8,8,NULL);
            if(!(p=record(m,META_DIBCREATEPATTERNBRUSH,2+size/2))) return -1;
            put16(p,BS_PATTERN); put16(p+2,DIB_RGB_COLORS); surface_dib(&s,0,0,8,8,p+4);
        } else {
            if(!(p=record(m,META_CREATEBRUSHINDIRECT,4))) return -1;
            put16(p,(WORD)b->style); put32(p+2,b->color); put16(p+6,(WORD)b->hatch);
        }
        break;
    }
    case OBJ_FONT: {
        const LOGFONT *l=&((const Font *)o)->log;
        if(!(p=record(m,META_CREATEFONTINDIRECT,9+LF_FACESIZE/2))) return -1;
        put16(p,(WORD)l->lfHeight); put16(p+2,(WORD)l->lfWidth); put16(p+4,(WORD)l->lfEscapement);
        put16(p+6,(WORD)l->lfOrientation); put16(p+8,(WORD)l->lfWeight);
        p[10]=l->lfItalic; p[11]=l->lfUnderline; p[12]=l->lfStrikeOut; p[13]=l->lfCharSet;
        p[14]=l->lfOutPrecision; p[15]=l->lfClipPrecision; p[16]=l->lfQuality; p[17]=l->lfPitchAndFamily;
        memcpy(p+18,l->lfFaceName,LF_FACESIZE); p[18+LF_FACESIZE-1]=0;
        break;
    }
    case OBJ_PAL: {
        const Palette *pal=(const Palette *)o;
        if(!(p=record(m,META_CREATEPALETTE,2+2*(DWORD)pal->count))) return -1;
        put16(p,0x300); put16(p+2,(WORD)pal->count); memcpy(p+4,pal->entries,(size_t)pal->count*4);
        break;
    }
    default: return -1;
    }
    m->slots[slot]=h;
    return slot;
}
/* SelectObject and SelectPalette: the object's record (once), then the
 * selection; the DC keeps what is selected, for the old one to come back. */
HGDIOBJ mf_select(DC *dc,HGDIOBJ h) {
    ObjHeader *o=(ObjHeader *)object(h,0); HGDIOBJ old; int slot;
    if(!o) return NULL;
    switch(o->type) {
    case OBJ_PEN: old=dc->s.pen; break;
    case OBJ_BRUSH: old=dc->s.brush; break;
    case OBJ_FONT: old=dc->s.font; break;
    case OBJ_PAL: old=dc->s.palette; break;
    default: return NULL;
    }
    if((slot=object_slot(dc->meta,h))<0 || !mf_words(dc,o->type==OBJ_PAL?META_SELECTPALETTE:META_SELECTOBJECT,1,slot)) return NULL;
    switch(o->type) {
    case OBJ_PEN: dc->s.pen=(HPEN)h; break;
    case OBJ_BRUSH: dc->s.brush=(HBRUSH)h; break;
    case OBJ_FONT: dc->s.font=(HFONT)h; break;
    default: dc->s.palette=(HPALETTE)h; break;
    }
    return old;
}
/* An object deleted: each table that has it lets it go. */
void mf_deleted(HGDIOBJ h) {
    Meta *m; int slot;
    for(m=recording;m;m=m->next)
        if((slot=slot_of(m,h))>=0) {
            BYTE *p=record(m,META_DELETEOBJECT,1);
            if(p) put16(p,(WORD)slot);
            m->slots[slot]=NULL;
        }
}
/* Polyline and Polygon: the count, then the points. */
BOOL mf_points(DC *dc,WORD function,const POINT *pt,int n) {
    BYTE *p=record(dc->meta,function,1+2*(DWORD)n); int i;
    if(!p) return FALSE;
    put16(p,(WORD)n);
    for(i=0;i<n;i++) {put16(p+2+4*i,(WORD)pt[i].x); put16(p+4+4*i,(WORD)pt[i].y);}
    return TRUE;
}
BOOL mf_poly_polygon(DC *dc,const POINT *pt,const int *counts,int n) {
    int i,total=0; BYTE *p,*at;
    for(i=0;i<n;i++) total+=counts[i];
    if(!(p=record(dc->meta,META_POLYPOLYGON,1+(DWORD)n+2*(DWORD)total))) return FALSE;
    put16(p,(WORD)n);
    for(i=0;i<n;i++) put16(p+2+2*i,(WORD)counts[i]);
    for(i=0,at=p+2+2*n;i<total;i++,at+=4) {put16(at,(WORD)pt[i].x); put16(at+2,(WORD)pt[i].y);}
    return TRUE;
}
/* TextOut (ext FALSE) and ExtTextOut. */
BOOL mf_text(DC *dc,int x,int y,UINT options,const RECT *rc,LPCSTR text,UINT count,const int *dx,BOOL ext) {
    BYTE *p; UINT i; DWORD chars=(count+1)/2;
    if(!ext) {
        if(!(p=record(dc->meta,META_TEXTOUT,3+chars))) return FALSE;
        put16(p,(WORD)count); memcpy(p+2,text,count); put16(p+2+2*chars,(WORD)y); put16(p+4+2*chars,(WORD)x);
        return TRUE;
    }
    if(!(options&(ETO_OPAQUE|ETO_CLIPPED))) rc=NULL;
    if(!(p=record(dc->meta,META_EXTTEXTOUT,4+(rc?4:0)+chars+(dx?count:0)))) return FALSE;
    put16(p,(WORD)y); put16(p+2,(WORD)x); put16(p+4,(WORD)count); put16(p+6,(WORD)options); p+=8;
    if(rc) {put16(p,(WORD)rc->left); put16(p+2,(WORD)rc->top); put16(p+4,(WORD)rc->right); put16(p+6,(WORD)rc->bottom); p+=8;}
    memcpy(p,text,count); p+=2*chars;
    if(dx) for(i=0;i<count;i++) put16(p+2*i,(WORD)dx[i]);
    return TRUE;
}
/* FillRgn, FrameRgn, InvertRgn, PaintRgn and SelectClipRgn: the region in
 * a slot for this record (never slot 0 for a clip, where 0 means none). */
BOOL mf_region(DC *dc,WORD function,HRGN rgn,HBRUSH brush,int w,int h) {
    Meta *m=dc->meta; Region *g=(Region *)object(rgn,OBJ_REGION); int r,b=0; BYTE *p;
    if(function==META_SELECTCLIPREGION && !g) return mf_words(dc,function,1,0);
    if(!g) return FALSE;
    if(brush && (b=object_slot(m,brush))<0) return FALSE;
    if((r=new_slot(m,function==META_SELECTCLIPREGION?1:0))<0 || !region_record(m,&g->r,r)) return FALSE;
    switch(function) {
    case META_FILLREGION: mf_words(dc,function,2,r,b); break; /* these two are in the call's order */
    case META_FRAMEREGION: mf_words(dc,function,4,r,b,h,w); break;
    default: mf_words(dc,function,1,r);
    }
    if((p=record(m,META_DELETEOBJECT,1))!=NULL) put16(p,(WORD)r);
    m->slots[r]=NULL;
    return !m->failed;
}
/* BitBlt and StretchBlt from a DC: the source's pixels as a DIB. */
BOOL mf_blt(DC *dc,int x,int y,int w,int h,HDC from,int sx,int sy,int sw,int sh,DWORD rop,BOOL stretch) {
    DC *src=dc_of(from); int l,t,r,b; DWORD size; BYTE *p;
    if(!src || src->meta) return FALSE;
    l=dev_x(src,sx); t=dev_y(src,sy); r=dev_x(src,sx+sw); b=dev_y(src,sy+sh);
    if(r<l) {int v=l; l=r; r=v;}
    if(b<t) {int v=t; t=b; b=v;}
    if(r<=l || b<=t) return FALSE;
    size=surface_dib(src->surface,l,t,r-l,b-t,NULL);
    if(!stretch && r-l==w && b-t==h) {
        if(!(p=record(dc->meta,META_DIBBITBLT,8+size/2))) return FALSE;
        put32(p,rop); put16(p+8,(WORD)h); put16(p+10,(WORD)w); put16(p+12,(WORD)y); put16(p+14,(WORD)x);
        surface_dib(src->surface,l,t,r-l,b-t,p+16);
    } else {
        if(!(p=record(dc->meta,META_DIBSTRETCHBLT,10+size/2))) return FALSE;
        put32(p,rop); put16(p+4,(WORD)(b-t)); put16(p+6,(WORD)(r-l));
        put16(p+12,(WORD)h); put16(p+14,(WORD)w); put16(p+16,(WORD)y); put16(p+18,(WORD)x);
        surface_dib(src->surface,l,t,r-l,b-t,p+20);
    }
    return TRUE;
}
/* A DIB's header and colors, and its bits for lines scans. */
static DWORD dib_table(const BYTE *info,UINT usage) {
    DWORD size=get32(info),colors; WORD bpp;
    if(size==12) {bpp=get16(info+10); colors=bpp<=8?1u<<bpp:0; return 12+colors*(usage==DIB_PAL_COLORS?2:3);}
    bpp=get16(info+14); colors=size>=36 && get32(info+32)?get32(info+32):bpp<=8?1u<<bpp:0;
    if(colors>256) colors=256;
    return size+colors*(usage==DIB_PAL_COLORS?2:4);
}
static DWORD dib_image(const BYTE *info,DWORD lines) {
    DWORD size=get32(info),width,bpp;
    if(size==12) {width=get16(info+4); bpp=get16(info+10);}
    else {
        width=get32(info+4); bpp=get16(info+14);
        if(get32(info+16)!=BI_RGB && get32(info+20)) return get32(info+20);
    }
    return ((width*bpp+31)/32)*4*lines;
}
/* StretchDIBits (META_STRETCHDIB) and SetDIBitsToDevice (META_SETDIBTODEV). */
BOOL mf_dib(DC *dc,WORD function,int x,int y,int w,int h,int sx,int sy,int sw,int sh,UINT start,UINT lines,
            const void *bits,const BITMAPINFO *info,UINT usage,DWORD rop) {
    DWORD table,image,height; BYTE *p;
    if(!info || !bits) return FALSE;
    table=dib_table((const BYTE *)info,usage);
    height=info->bmiHeader.biSize==12?((const BITMAPCOREHEADER *)info)->bcHeight:(DWORD)(info->bmiHeader.biHeight<0?-info->bmiHeader.biHeight:info->bmiHeader.biHeight);
    image=dib_image((const BYTE *)info,function==META_SETDIBTODEV?lines:height);
    if(function==META_STRETCHDIB) {
        if(!(p=record(dc->meta,function,11+(table+image+1)/2))) return FALSE;
        put32(p,rop); put16(p+4,(WORD)usage); put16(p+6,(WORD)sh); put16(p+8,(WORD)sw); put16(p+10,(WORD)sy); put16(p+12,(WORD)sx);
        put16(p+14,(WORD)h); put16(p+16,(WORD)w); put16(p+18,(WORD)y); put16(p+20,(WORD)x); p+=22;
    } else {
        if(!(p=record(dc->meta,function,9+(table+image+1)/2))) return FALSE;
        put16(p,(WORD)usage); put16(p+2,(WORD)lines); put16(p+4,(WORD)start); put16(p+6,(WORD)sy); put16(p+8,(WORD)sx);
        put16(p+10,(WORD)h); put16(p+12,(WORD)w); put16(p+14,(WORD)y); put16(p+16,(WORD)x); p+=18;
    }
    memcpy(p,info,table); memcpy(p+table,bits,image);
    return TRUE;
}
void mf_free(DC *dc) {
    Meta *m=dc->meta,**at;
    if(!m) return;
    for(at=&recording;*at;at=&(*at)->next) if(*at==m) {*at=m->next; break;}
    gdi_free(m->bits); gdi_free(m->slots); gdi_free(m);
    dc->meta=NULL;
}

/* --- metafile handles -------------------------------------------------------------------- */
static Metafile *metafile_of(HMETAFILE h) {return (Metafile *)object((HGDIOBJ)h,OBJ_METAFILE);}
/* A metafile's header is nine words; the records follow it. */
static BOOL valid(const BYTE *bits,DWORD size) {
    return bits && size>=HEADER && (get16(bits)==MEMORYMETAFILE || get16(bits)==DISKMETAFILE) && get16(bits+2)==9;
}
/* A metafile handle for bits it takes over. */
static HMETAFILE new_metafile(BYTE *bits,DWORD size) {
    Metafile *mf; HMETAFILE h=(HMETAFILE)make_object(OBJ_METAFILE,sizeof(Metafile),(void **)&mf);
    if(!h) {gdi_free(bits); return NULL;}
    mf->bits=bits; mf->size=size; put16(bits,MEMORYMETAFILE);
    return h;
}
static HMETAFILE copy_metafile(const BYTE *bits,DWORD size) {
    BYTE *copy;
    if(!valid(bits,size) || !(copy=(BYTE *)gdi_alloc(size))) return NULL;
    memcpy(copy,bits,size);
    return new_metafile(copy,size);
}
void metafile_free(Metafile *mf) {gdi_free(mf->bits); mf->bits=NULL;}
/* A disk metafile's file: the header says so. */
static BOOL write_file(LPCSTR path,const BYTE *bits,DWORD size) {
    BYTE head[HEADER]; HFILE f=_lcreat(path,0); BOOL ok;
    if(f==HFILE_ERROR) return FALSE;
    memcpy(head,bits,HEADER); put16(head,DISKMETAFILE);
    ok=_lwrite(f,head,HEADER)==HEADER && _lwrite(f,bits+HEADER,size-HEADER)==size-HEADER;
    _lclose(f);
    return ok;
}
HDC WINAPI CreateMetaFile(LPCSTR path) {
    HDC h=CreateCompatibleDC(NULL); DC *dc=dc_of(h); Meta *m;
    if(!dc) return NULL;
    if(!(m=(Meta *)gdi_alloc(sizeof(Meta))) || !grow(m,4096)) {gdi_free(m); DeleteDC(h); return NULL;}
    m->size=HEADER;
    if(path) lstrcpyn(m->path,path,MAX_PATH);
    m->next=recording; recording=m; dc->meta=m;
    /* Nothing is drawn on its surface. */
    dc->vis.count=0; update_clip(dc);
    return h;
}
HMETAFILE WINAPI CloseMetaFile(HDC h) {
    DC *dc=dc_of(h); Meta *m=dc?dc->meta:NULL; HMETAFILE mf=NULL;
    if(!m) return NULL;
    if(record(m,0,0) && !m->failed) {
        BYTE *b=m->bits;
        put16(b,MEMORYMETAFILE); put16(b+2,9); put16(b+4,0x300); put32(b+6,m->size/2);
        put16(b+10,(WORD)m->objects); put32(b+12,m->max_record); put16(b+16,0);
        if(!m->path[0] || write_file(m->path,b,m->size)) {mf=new_metafile(b,m->size); m->bits=NULL;}
    }
    DeleteDC(h);
    return mf;
}
BOOL WINAPI DeleteMetaFile(HMETAFILE h) {return metafile_of(h) && DeleteObject((HGDIOBJ)h);}
HMETAFILE WINAPI GetMetaFile(LPCSTR path) {
    HFILE f=_lopen(path,OF_READ); LONG size; BYTE *bits; HMETAFILE h=NULL; DWORD skip=0;
    if(f==HFILE_ERROR) return NULL;
    size=_llseek(f,0,2); _llseek(f,0,0);
    if(size>=HEADER && (bits=(BYTE *)gdi_alloc((DWORD)size))!=NULL) {
        if(_lread(f,bits,(UINT)size)==(UINT)size) {
            /* A placeable metafile has a 22-byte header of its own first. */
            if(get32(bits)==0x9ac6cdd7 && size>=22+HEADER) skip=22;
            h=copy_metafile(bits+skip,(DWORD)size-skip);
        }
        gdi_free(bits);
    }
    _lclose(f);
    return h;
}
HMETAFILE WINAPI CopyMetaFile(HMETAFILE h,LPCSTR path) {
    Metafile *mf=metafile_of(h);
    if(!mf || (path && !write_file(path,mf->bits,mf->size))) return NULL;
    return copy_metafile(mf->bits,mf->size);
}
UINT WINAPI GetMetaFileBitsEx(HMETAFILE h,UINT size,LPVOID out) {
    Metafile *mf=metafile_of(h);
    if(!mf) return 0;
    if(!out) return mf->size;
    size=min(size,mf->size); memcpy(out,mf->bits,size);
    return size;
}
HMETAFILE WINAPI SetMetaFileBitsEx(UINT size,const BYTE FAR *bits) {return copy_metafile(bits,size);}
HGLOBAL WINAPI GetMetaFileBits(HMETAFILE h) {
    Metafile *mf=metafile_of(h); HGLOBAL g; void *p;
    if(!mf || !(g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,mf->size))) return NULL;
    if((p=GlobalLock(g))!=NULL) {memcpy(p,mf->bits,mf->size); GlobalUnlock(g);}
    DeleteMetaFile(h);
    return g;
}
HMETAFILE WINAPI SetMetaFileBits(HGLOBAL g) {
    const BYTE *p=(const BYTE *)GlobalLock(g); HMETAFILE h=NULL;
    if(p) {h=copy_metafile(p,(DWORD)GlobalSize(g)); GlobalUnlock(g);}
    if(h) GlobalFree(g);
    return h;
}

/* --- playing ------------------------------------------------------------------------------ */
typedef struct {const BYTE *p; DWORD words;} Params;
static WORD P(const Params *a,DWORD i) {return i<a->words?get16(a->p+2*i):0;}
static int S(const Params *a,DWORD i) {return (short)P(a,i);}
static DWORD D(const Params *a,DWORD i) {return MAKELONG(P(a,i),P(a,i+1));}
static HGDIOBJ T(HANDLETABLE *t,UINT n,DWORD i) {return i<n?t->objectHandle[i]:NULL;}
static void keep(HANDLETABLE *t,UINT n,HGDIOBJ h) {
    UINT i;
    if(!h) return;
    for(i=0;i<n;i++) if(!t->objectHandle[i]) {t->objectHandle[i]=h; return;}
    DeleteObject(h);
}
/* The DIB from word at to the record's end, in aligned memory (freed by the caller). */
static BITMAPINFO *record_dib(const Params *a,DWORD at,UINT usage,const void **bits) {
    DWORD size; BYTE *copy;
    if(at+20>a->words) return NULL;
    size=(a->words-at)*2;
    if(!(copy=(BYTE *)gdi_alloc(size+4))) return NULL;
    memcpy(copy,a->p+2*at,size);
    if(dib_table(copy,usage)>=size) {gdi_free(copy); return NULL;}
    *bits=copy+dib_table(copy,usage);
    return (BITMAPINFO *)copy;
}
/* A Windows 2 bitmap in the record (BITMAP16, then its bits at word bits_at): monochrome only. */
static HBITMAP record_bitmap(const Params *a,DWORD at,DWORD bits_at) {
    int w=S(a,at+1),h=S(a,at+2); WORD row=P(a,at+3); BYTE planes=(BYTE)P(a,at+4),bpp=(BYTE)(P(a,at+4)>>8);
    BYTE *copy; HBITMAP b;
    if(planes!=1 || bpp!=1 || w<=0 || h<=0 || bits_at+((DWORD)row*h+1)/2>a->words) return NULL;
    if(!(copy=(BYTE *)gdi_alloc((DWORD)row*h))) return NULL;
    memcpy(copy,a->p+2*bits_at,(size_t)row*h);
    b=CreateBitmap(w,h,1,1,copy);
    gdi_free(copy);
    return b;
}
static void blt_bitmap(HDC h,HBITMAP b,int x,int y,int w,int hh,int sx,int sy,int sw,int sh,DWORD rop) {
    HDC mem=CreateCompatibleDC(h); HGDIOBJ old;
    if(!mem) return;
    old=SelectObject(mem,b);
    StretchBlt(h,x,y,w,hh,mem,sx,sy,sw,sh,rop);
    SelectObject(mem,old); DeleteDC(mem);
}
/* A region from its record: each scan's runs of x. */
static HRGN record_region(const Params *a) {
    RectList r; DWORD at=11; int scans=S(a,5),i,k; RECT rc;
    memset(&r,0,sizeof(r));
    for(i=0;i<scans && at+3<=a->words;i++) {
        int count=P(a,at),top=S(a,at+1),bottom=S(a,at+2);
        for(k=0;k+1<count;k+=2) {r_set(&rc,S(a,at+3+k),top,S(a,at+4+k),bottom); if(!r_empty(&rc)) rl_add(&r,&rc);}
        at+=4+(DWORD)count;
    }
    return new_region(&r);
}
static void palette_entries(HDC h,const Params *a,BOOL animate) {
    DC *dc=dc_of(h); UINT start=P(a,0),n=P(a,1); PALETTEENTRY *e;
    if(!dc || 2+2*(DWORD)n>a->words || !(e=(PALETTEENTRY *)gdi_alloc((DWORD)n*sizeof(PALETTEENTRY)+4))) return;
    memcpy(e,a->p+4,(size_t)n*4);
    if(animate) AnimatePalette(dc->s.palette,start,n,e); else SetPaletteEntries(dc->s.palette,start,n,e);
    gdi_free(e);
}
static POINT *record_points(const Params *a,DWORD at,int n) {
    POINT *pt; int i;
    if(n<=0 || at+2*(DWORD)n>a->words || !(pt=(POINT *)gdi_alloc((DWORD)n*sizeof(POINT)))) return NULL;
    for(i=0;i<n;i++) {pt[i].x=S(a,at+2*i); pt[i].y=S(a,at+2*i+1);}
    return pt;
}
BOOL WINAPI PlayMetaFileRecord(HDC h,LPHANDLETABLE t,LPMETARECORD mr,UINT n) {
    const BYTE *r=(const BYTE *)mr; Params a; Params *A=&a; const void *bits; BITMAPINFO *info;
    if(!r || !t) return FALSE;
    a.p=r+6; a.words=get32(r)>3?get32(r)-3:0;
    switch(get16(r+4)) {
    case META_SETBKCOLOR: SetBkColor(h,D(A,0)); break;
    case META_SETBKMODE: SetBkMode(h,S(A,0)); break;
    case META_SETMAPMODE: SetMapMode(h,S(A,0)); break;
    case META_SETROP2: SetROP2(h,S(A,0)); break;
    case META_SETPOLYFILLMODE: SetPolyFillMode(h,S(A,0)); break;
    case META_SETSTRETCHBLTMODE: SetStretchBltMode(h,S(A,0)); break;
    case META_SETTEXTCHAREXTRA: SetTextCharacterExtra(h,S(A,0)); break;
    case META_SETTEXTCOLOR: SetTextColor(h,D(A,0)); break;
    case META_SETTEXTJUSTIFICATION: SetTextJustification(h,S(A,1),S(A,0)); break;
    case META_SETWINDOWORG: SetWindowOrgEx(h,S(A,1),S(A,0),NULL); break;
    case META_SETWINDOWEXT: SetWindowExtEx(h,S(A,1),S(A,0),NULL); break;
    case META_SETVIEWPORTORG: SetViewportOrgEx(h,S(A,1),S(A,0),NULL); break;
    case META_SETVIEWPORTEXT: SetViewportExtEx(h,S(A,1),S(A,0),NULL); break;
    case META_OFFSETWINDOWORG: OffsetWindowOrgEx(h,S(A,1),S(A,0),NULL); break;
    case META_OFFSETVIEWPORTORG: OffsetViewportOrgEx(h,S(A,1),S(A,0),NULL); break;
    case META_SCALEWINDOWEXT: ScaleWindowExtEx(h,S(A,3),S(A,2),S(A,1),S(A,0),NULL); break;
    case META_SCALEVIEWPORTEXT: ScaleViewportExtEx(h,S(A,3),S(A,2),S(A,1),S(A,0),NULL); break;
    case META_LINETO: LineTo(h,S(A,1),S(A,0)); break;
    case META_MOVETO: MoveToEx(h,S(A,1),S(A,0),NULL); break;
    case META_EXCLUDECLIPRECT: ExcludeClipRect(h,S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_INTERSECTCLIPRECT: IntersectClipRect(h,S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_ARC: Arc(h,S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_CHORD: Chord(h,S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_PIE: Pie(h,S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_ELLIPSE: Ellipse(h,S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_RECTANGLE: Rectangle(h,S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_ROUNDRECT: RoundRect(h,S(A,5),S(A,4),S(A,3),S(A,2),S(A,1),S(A,0)); break;
    case META_PATBLT: PatBlt(h,S(A,5),S(A,4),S(A,3),S(A,2),D(A,0)); break;
    case META_SAVEDC: SaveDC(h); break;
    case META_RESTOREDC: RestoreDC(h,S(A,0)); break;
    case META_SETPIXEL: SetPixel(h,S(A,3),S(A,2),D(A,0)); break;
    case META_OFFSETCLIPRGN: OffsetClipRgn(h,S(A,1),S(A,0)); break;
    case META_FLOODFILL: FloodFill(h,S(A,3),S(A,2),D(A,0)); break;
    case META_EXTFLOODFILL: ExtFloodFill(h,S(A,4),S(A,3),D(A,1),P(A,0)); break;
    case META_TEXTOUT: {
        int count=S(A,0); DWORD chars=((DWORD)count+1)/2;
        if(count>0 && 3+chars<=a.words) TextOut(h,S(A,2+chars),S(A,1+chars),(LPCSTR)a.p+2,count);
        break;
    }
    case META_EXTTEXTOUT: {
        UINT count=P(A,2),options=P(A,3),i; DWORD at=4,chars=(count+1)/2; RECT rc; int *dx=NULL; char *text;
        if(options&(ETO_OPAQUE|ETO_CLIPPED)) {r_set(&rc,S(A,4),S(A,5),S(A,6),S(A,7)); at=8;}
        if(at+chars>a.words || !(text=(char *)gdi_alloc(count+1))) break;
        memcpy(text,a.p+2*at,count);
        if(at+chars+count<=a.words && (dx=(int *)gdi_alloc(count*sizeof(int)+4))!=NULL)
            for(i=0;i<count;i++) dx[i]=S(A,at+chars+i);
        ExtTextOut(h,S(A,1),S(A,0),options,at==8?&rc:NULL,text,count,dx);
        gdi_free(dx); gdi_free(text);
        break;
    }
    case META_POLYGON: case META_POLYLINE: {
        int count=S(A,0); POINT *pt=record_points(A,1,count);
        if(pt) {if(get16(r+4)==META_POLYGON) Polygon(h,pt,count); else Polyline(h,pt,count); gdi_free(pt);}
        break;
    }
    case META_POLYPOLYGON: {
        int polys=S(A,0),total=0,i,*counts; POINT *pt;
        if(polys<=0 || 1+(DWORD)polys>a.words || !(counts=(int *)gdi_alloc((DWORD)polys*sizeof(int)))) break;
        for(i=0;i<polys;i++) total+=counts[i]=P(A,1+i);
        if((pt=record_points(A,1+(DWORD)polys,total))!=NULL) {PolyPolygon(h,pt,counts,polys); gdi_free(pt);}
        gdi_free(counts);
        break;
    }
    case META_FILLREGION: FillRgn(h,(HRGN)T(t,n,P(A,0)),(HBRUSH)T(t,n,P(A,1))); break;
    case META_FRAMEREGION: FrameRgn(h,(HRGN)T(t,n,P(A,0)),(HBRUSH)T(t,n,P(A,1)),S(A,3),S(A,2)); break;
    case META_INVERTREGION: InvertRgn(h,(HRGN)T(t,n,P(A,0))); break;
    case META_PAINTREGION: PaintRgn(h,(HRGN)T(t,n,P(A,0))); break;
    case META_SELECTCLIPREGION: {
        HGDIOBJ rgn=T(t,n,P(A,0));
        SelectClipRgn(h,GetObjectType(rgn)==OBJ_REGION?(HRGN)rgn:NULL);
        break;
    }
    case META_SELECTOBJECT: {HGDIOBJ o=T(t,n,P(A,0)); if(o) SelectObject(h,o); break;}
    case META_SELECTPALETTE: {HGDIOBJ o=T(t,n,P(A,0)); if(o) SelectPalette(h,(HPALETTE)o,FALSE); break;}
    case META_REALIZEPALETTE: RealizePalette(h); break;
    case META_SETTEXTALIGN: SetTextAlign(h,P(A,0)); break;
    case META_SETMAPPERFLAGS: SetMapperFlags(h,D(A,0)); break;
    case META_ANIMATEPALETTE: palette_entries(h,A,TRUE); break;
    case META_SETPALENTRIES: palette_entries(h,A,FALSE); break;
    case META_RESIZEPALETTE: {DC *dc=dc_of(h); if(dc) ResizePalette(dc->s.palette,P(A,0)); break;}
    case META_DIBBITBLT:
        /* With a source, a DIB follows the destination; without one there is a
         * reserved word. The source's y is from the top, as BitBlt has it;
         * StretchDIBits counts a bottom-up DIB's from the bottom. */
        if(a.words>9 && (info=record_dib(A,8,DIB_RGB_COLORS,&bits))!=NULL) {
            int sh=S(A,4),sy=S(A,2);
            if(info->bmiHeader.biHeight>0) sy=info->bmiHeader.biHeight-sy-sh;
            StretchDIBits(h,S(A,7),S(A,6),S(A,5),sh,S(A,3),sy,S(A,5),sh,bits,info,DIB_RGB_COLORS,D(A,0));
            gdi_free(info);
        } else if(a.words<=9) PatBlt(h,S(A,8),S(A,7),S(A,6),S(A,5),D(A,0));
        break;
    case META_DIBSTRETCHBLT:
        if(a.words>11 && (info=record_dib(A,10,DIB_RGB_COLORS,&bits))!=NULL) {
            int sh=S(A,2),sy=S(A,4);
            if(info->bmiHeader.biHeight>0) sy=info->bmiHeader.biHeight-sy-sh;
            StretchDIBits(h,S(A,9),S(A,8),S(A,7),S(A,6),S(A,5),sy,S(A,3),sh,bits,info,DIB_RGB_COLORS,D(A,0));
            gdi_free(info);
        } else if(a.words<=11) PatBlt(h,S(A,10),S(A,9),S(A,8),S(A,7),D(A,0));
        break;
    case META_STRETCHDIB:
        if((info=record_dib(A,11,P(A,2),&bits))!=NULL) {
            StretchDIBits(h,S(A,10),S(A,9),S(A,8),S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),bits,info,P(A,2),D(A,0));
            gdi_free(info);
        }
        break;
    case META_SETDIBTODEV:
        if((info=record_dib(A,9,P(A,0),&bits))!=NULL) {
            SetDIBitsToDevice(h,S(A,8),S(A,7),P(A,6),P(A,5),S(A,4),S(A,3),P(A,2),P(A,1),bits,info,P(A,0));
            gdi_free(info);
        }
        break;
    case META_BITBLT: {
        HBITMAP b=record_bitmap(A,8,15);
        if(b) {blt_bitmap(h,b,S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),S(A,2),S(A,5),S(A,4),D(A,0)); DeleteObject(b);}
        break;
    }
    case META_STRETCHBLT: {
        HBITMAP b=record_bitmap(A,10,17);
        if(b) {blt_bitmap(h,b,S(A,9),S(A,8),S(A,7),S(A,6),S(A,5),S(A,4),S(A,3),S(A,2),D(A,0)); DeleteObject(b);}
        break;
    }
    case META_DELETEOBJECT: {
        HGDIOBJ o=T(t,n,P(A,0));
        if(o) {DeleteObject(o); t->objectHandle[P(A,0)]=NULL;}
        break;
    }
    case META_CREATEPENINDIRECT: {
        LOGPEN l; l.lopnStyle=P(A,0); l.lopnWidth.x=S(A,1); l.lopnWidth.y=S(A,2); l.lopnColor=D(A,3);
        keep(t,n,CreatePenIndirect(&l));
        break;
    }
    case META_CREATEBRUSHINDIRECT: {
        LOGBRUSH l; l.lbStyle=P(A,0); l.lbColor=D(A,1); l.lbHatch=P(A,3);
        if(l.lbStyle!=BS_SOLID && l.lbStyle!=BS_NULL && l.lbStyle!=BS_HATCHED) l.lbStyle=BS_SOLID;
        keep(t,n,CreateBrushIndirect(&l));
        break;
    }
    case META_DIBCREATEPATTERNBRUSH: {
        UINT usage=P(A,0)==BS_PATTERN?DIB_RGB_COLORS:P(A,1); HBRUSH b=NULL;
        if((info=record_dib(A,2,usage,&bits))!=NULL) {b=CreateDIBPatternBrushPt(info,usage); gdi_free(info);}
        keep(t,n,b?(HGDIOBJ)b:(HGDIOBJ)CreateSolidBrush(0));
        break;
    }
    case META_CREATEPATTERNBRUSH: {
        HBITMAP b=record_bitmap(A,0,16); HBRUSH brush=b?CreatePatternBrush(b):NULL;
        if(b) DeleteObject(b);
        keep(t,n,brush?(HGDIOBJ)brush:(HGDIOBJ)CreateSolidBrush(0));
        break;
    }
    case META_CREATEFONTINDIRECT: {
        LOGFONT l; DWORD face=a.words*2>18?min(a.words*2-18,LF_FACESIZE-1):0;
        memset(&l,0,sizeof(l));
        l.lfHeight=S(A,0); l.lfWidth=S(A,1); l.lfEscapement=S(A,2); l.lfOrientation=S(A,3); l.lfWeight=S(A,4);
        if(a.words>=9) {
            l.lfItalic=a.p[10]; l.lfUnderline=a.p[11]; l.lfStrikeOut=a.p[12]; l.lfCharSet=a.p[13];
            l.lfOutPrecision=a.p[14]; l.lfClipPrecision=a.p[15]; l.lfQuality=a.p[16]; l.lfPitchAndFamily=a.p[17];
        }
        memcpy(l.lfFaceName,a.p+18,face);
        keep(t,n,CreateFontIndirect(&l));
        break;
    }
    case META_CREATEPALETTE: {
        UINT count=P(A,1); LOGPALETTE *l;
        if(count && count<=256 && 2+2*(DWORD)count<=a.words && (l=(LOGPALETTE *)gdi_alloc(4+(DWORD)count*4))!=NULL) {
            l->palVersion=0x300; l->palNumEntries=(WORD)count; memcpy(l->palPalEntry,a.p+4,(size_t)count*4);
            keep(t,n,CreatePalette(l)); gdi_free(l);
        } else keep(t,n,CreatePalette(NULL));
        break;
    }
    case META_CREATEREGION: keep(t,n,record_region(A)); break;
    default: break; /* SETRELABS, ESCAPE, DRAWTEXT and anything newer */
    }
    return TRUE;
}
/* The records one by one: played, or given to proc (which may stop them);
 * the objects they made are deleted after, the DC's put back first. */
static BOOL walk(HDC h,HMETAFILE hmf,MFENUMPROC proc,LPARAM lp) {
    Metafile *mf=metafile_of(hmf); DC *dc=dc_of(h); HANDLETABLE *t; UINT n,i; DWORD at,size;
    HGDIOBJ pen,brush,font,palette; RectList clip; BOOL clipped,ok=TRUE;
    if(!mf || !dc) return FALSE;
    n=get16(mf->bits+10);
    if(!(t=(HANDLETABLE *)gdi_alloc((DWORD)(n+1)*sizeof(HGDIOBJ)))) return FALSE;
    pen=dc->s.pen; brush=dc->s.brush; font=dc->s.font; palette=dc->s.palette;
    memset(&clip,0,sizeof(clip)); rl_copy(&clip,&dc->s.clip); clipped=dc->s.clipped;
    for(at=get16(mf->bits+2)*2;at+6<=mf->size;at+=size*2) {
        BYTE *r=mf->bits+at;
        size=get32(r);
        if(!get16(r+4) || size<3 || at+size*2>mf->size) break;
        if(proc) {if(!proc(h,t,(METARECORD *)r,(int)n,lp)) {ok=FALSE; break;}}
        else PlayMetaFileRecord(h,t,(METARECORD *)r,n);
    }
    if((dc=dc_of(h))!=NULL) {
        SelectObject(h,pen); SelectObject(h,brush); SelectObject(h,font); SelectPalette(h,(HPALETTE)palette,FALSE);
        if(!dc->meta) {rl_copy(&dc->s.clip,&clip); dc->s.clipped=clipped; update_clip(dc);}
    }
    rl_free(&clip);
    for(i=0;i<n;i++) if(t->objectHandle[i]) DeleteObject(t->objectHandle[i]);
    gdi_free(t);
    return ok;
}
BOOL WINAPI PlayMetaFile(HDC h,HMETAFILE mf) {return walk(h,mf,NULL,0);}
BOOL WINAPI EnumMetaFile(HDC h,HMETAFILE mf,MFENUMPROC proc,LPARAM lp) {return proc && walk(h,mf,proc,lp);}
