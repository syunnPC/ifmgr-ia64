/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: Win16 memory and selectors.
 *
 * Global memory: a handle is the block's selector (consecutive selectors
 * for blocks over 64 KiB), so GlobalLock returns selector:0. Blocks are
 * fixed in place; reallocating copies the block and moves the selectors to
 * the new memory. Blocks belong to the task that allocated them.
 *
 * Local memory: a heap inside a segment (the automatic data segment after
 * the data and the stack, up to its 64 KiB end, or any segment given to
 * LocalInit). Blocks have a four-byte header (size including the header,
 * then flags with bit 0 for "used"); a handle is the near offset of the
 * data, and moveable blocks do not move.
 */
#include "api.h"

/* --- global ------------------------------------------------------------------ */
#define BLOCKS 512
static struct {WORD sel; DWORD size; void *memory; Task16 *owner;} blocks[BLOCKS];
static int block_index(WORD sel) {
    unsigned i;
    sel|=7;
    for(i=0;i<BLOCKS;i++) if(blocks[i].sel && blocks[i].sel==sel) return (int)i;
    return -1;
}
static DWORD block_bytes(DWORD size) {return size?(size+15)&~15UL:16;}
WORD GlobalAlloc16(Task16 *t,UINT flags,DWORD size) {
    unsigned i; void *p; WORD sel;
    (void)flags;
    for(i=0;i<BLOCKS && blocks[i].sel;i++) {}
    if(i==BLOCKS || size>0xff0000UL) return 0;
    if(!(p=Alloc16(block_bytes(size)))) return 0;
    if(!(sel=SelAllocTiled(SelLinear(p),block_bytes(size),SEL_DATA))) {Free16(p,block_bytes(size)); return 0;}
    blocks[i].sel=sel; blocks[i].size=size; blocks[i].memory=p; blocks[i].owner=t;
    return sel;
}
static void block_free(unsigned i) {
    SelFreeTiled(blocks[i].sel,block_bytes(blocks[i].size));
    Free16(blocks[i].memory,block_bytes(blocks[i].size));
    memset(&blocks[i],0,sizeof(blocks[i]));
}
void GlobalTaskEnded16(Task16 *t) {
    unsigned i;
    for(i=0;i<BLOCKS;i++) if(blocks[i].sel && blocks[i].owner==t) block_free(i);
}
BOOL GlobalFree16(WORD sel) {int i=block_index(sel); if(i<0) return FALSE; block_free((unsigned)i); return TRUE;}
DWORD GlobalSize16(WORD sel) {int i=block_index(sel); return i<0?0:blocks[i].size;}
DWORD W16_GlobalAlloc(Args16 *a) {return GlobalAlloc16(a->task,(UINT)a->a[0],a->a[1]);}
DWORD W16_GlobalFree(Args16 *a) {return GlobalFree16((WORD)a->a[0])?0:a->a[0];}
DWORD W16_GlobalLock(Args16 *a) {return block_index((WORD)a->a[0])<0?0:MAKELONG(0,(WORD)a->a[0]|7);}
DWORD W16_GlobalUnlock(Args16 *a) {(void)a; return 0;}
DWORD W16_GlobalSize(Args16 *a) {return GlobalSize16((WORD)a->a[0]);}
DWORD W16_GlobalHandle(Args16 *a) {int i=block_index((WORD)a->a[0]); return i<0?0:MAKELONG(blocks[i].sel,blocks[i].sel);}
DWORD W16_GlobalFlags(Args16 *a) {(void)a; return 0;}
DWORD W16_LockSegment(Args16 *a) {return a->a[0]==0xffff?Seg16(a->task,3):a->a[0];}
DWORD W16_UnlockSegment(Args16 *a) {(void)a; return 0;}
DWORD W16_GlobalReAlloc(Args16 *a) {
    int i=block_index((WORD)a->a[0]); DWORD size=a->a[1],old; void *p; unsigned n_old,n_new;
    if(i<0) return 0;
    old=blocks[i].size;
    n_old=(unsigned)((block_bytes(old)+0xffff)>>16); n_new=(unsigned)((block_bytes(size)+0xffff)>>16);
    if(!(p=Alloc16(block_bytes(size)))) return 0;
    memcpy(p,blocks[i].memory,old<size?old:size);
    if((UINT)a->a[2]&GMEM_ZEROINIT && size>old) memset((BYTE *)p+old,0,size-old);
    if(n_new==n_old) SelSetTiled(blocks[i].sel,SelLinear(p),block_bytes(size),SEL_DATA);
    else {
        WORD sel=SelAllocTiled(SelLinear(p),block_bytes(size),SEL_DATA);
        if(!sel) {Free16(p,block_bytes(size)); return 0;}
        SelFreeTiled(blocks[i].sel,block_bytes(old)); blocks[i].sel=sel;
    }
    Free16(blocks[i].memory,block_bytes(old));
    blocks[i].memory=p; blocks[i].size=size;
    return blocks[i].sel;
}
DWORD W16_GlobalDOSAlloc(Args16 *a) {(void)a; return 0;}

/* --- selectors --------------------------------------------------------------- */
DWORD W16_AllocSelector(Args16 *a) {
    WORD from=(WORD)a->a[0];
    return SelValid(from)?SelAlloc(SelBase(from),SelLimit(from),SEL_DATA):SelAlloc(0,0,SEL_DATA);
}
DWORD W16_FreeSelector(Args16 *a) {SelFree((WORD)a->a[0]); return 0;}
DWORD W16_AllocCStoDSAlias(Args16 *a) {WORD s=(WORD)a->a[0]; return SelValid(s)?SelAlloc(SelBase(s),SelLimit(s),SEL_DATA):0;}
DWORD W16_AllocDStoCSAlias(Args16 *a) {WORD s=(WORD)a->a[0]; return SelValid(s)?SelAlloc(SelBase(s),SelLimit(s),SEL_CODE):0;}
DWORD W16_GetSelectorBase(Args16 *a) {return SelValid((WORD)a->a[0])?SelBase((WORD)a->a[0]):0;}
DWORD W16_GetSelectorLimit(Args16 *a) {return SelValid((WORD)a->a[0])?SelLimit((WORD)a->a[0]):0;}
DWORD W16_SetSelectorBase(Args16 *a) {
    WORD s=(WORD)a->a[0];
    if(SelValid(s)) SelSet(s,a->a[1],SelLimit(s),SEL_DATA);
    return s;
}
DWORD W16_SetSelectorLimit(Args16 *a) {
    WORD s=(WORD)a->a[0];
    if(SelValid(s)) SelSet(s,SelBase(s),a->a[1],SEL_DATA);
    return 0;
}

/* --- local ------------------------------------------------------------------- */
#define HEAPS 64
static struct {WORD sel,start,end;} heaps[HEAPS];
static int heap_of(WORD sel) {
    unsigned i;
    for(i=0;i<HEAPS;i++) if(heaps[i].sel && heaps[i].sel==(sel|7)) return (int)i;
    return -1;
}
BOOL LocalInit16(WORD sel,WORD start,WORD end) {
    int i=heap_of(sel); unsigned k; BYTE *seg; DWORD limit; Module16 *m;
    if(!SelValid(sel)) return FALSE;
    limit=SelLimit(sel); m=NeFromHandle(sel);
    if(i<0) {for(k=0;k<HEAPS && heaps[k].sel;k++) {} if(k==HEAPS) return FALSE; i=(int)k;}
    if(!start && m && m->instance==(sel|7)) {start=(WORD)m->heap_start; end=0;}
    if(!end || end>limit) end=(WORD)(limit>0xfffc?0xfffc:limit);
    start=(WORD)((start+3)&~3U); end=(WORD)(end&~3U);
    if(end<=start+8) return FALSE;
    heaps[i].sel=(WORD)(sel|7); heaps[i].start=start; heaps[i].end=end;
    seg=(BYTE *)SelPointer(sel,0);
    put16(seg+start,(WORD)(end-start)); put16(seg+start+2,0);
    if(m && m->instance==(sel|7)) put16(seg+6,start); /* pLocalHeap in the instance data */
    return TRUE;
}
void LocalTaskEnded16(Module16 *m) {
    unsigned i,k;
    for(i=0;i<HEAPS;i++) for(k=0;k<m->segments;k++) if(heaps[i].sel==m->seg[k].sel) heaps[i].sel=0;
}
/* The heap of the caller's DS, made on first use in the automatic data
 * segment; NULL with no heap. */
static BYTE *heap(Task16 *t,int *index) {
    WORD ds=Seg16(t,3); int i=heap_of(ds);
    if(i<0 && t->module && t->module->instance==(ds|7) && LocalInit16(ds,0,0)) i=heap_of(ds);
    *index=i;
    return i<0?NULL:(BYTE *)SelPointer(ds,0);
}
static void coalesce(BYTE *seg,WORD at,WORD end) {
    WORD size=get16(seg+at);
    if(get16(seg+at+2)&1) return;
    while(at+size<end && !(get16(seg+at+size+2)&1)) size=(WORD)(size+get16(seg+at+size));
    put16(seg+at,size);
}
static WORD local_alloc(BYTE *seg,int h,UINT flags,WORD bytes) {
    WORD need=(WORD)((bytes+4+3)&~3U),at;
    if(bytes>0xfff0) return 0;
    if(need<8) need=8;
    for(at=heaps[h].start;at<heaps[h].end;at=(WORD)(at+get16(seg+at))) {
        WORD size;
        coalesce(seg,at,heaps[h].end); size=get16(seg+at);
        if(!size) return 0;
        if(get16(seg+at+2)&1 || size<need) continue;
        if(size-need>=8) {put16(seg+at+need,(WORD)(size-need)); put16(seg+at+need+2,0); size=need;}
        put16(seg+at,size); put16(seg+at+2,1);
        if(flags&LMEM_ZEROINIT) memset(seg+at+4,0,(DWORD)size-4);
        return (WORD)(at+4);
    }
    return 0;
}
static BOOL local_valid(BYTE *seg,int h,WORD handle) {
    WORD at;
    for(at=heaps[h].start;at<heaps[h].end && get16(seg+at);at=(WORD)(at+get16(seg+at)))
        if(at+4==handle) return (get16(seg+at+2)&1)!=0;
    return FALSE;
}
DWORD W16_LocalInit(Args16 *a) {return LocalInit16((WORD)(a->a[0]?a->a[0]:Seg16(a->task,3)),(WORD)a->a[1],(WORD)a->a[2]);}
DWORD W16_LocalAlloc(Args16 *a) {int h; BYTE *seg=heap(a->task,&h); return seg?local_alloc(seg,h,(UINT)a->a[0],(WORD)a->a[1]):0;}
DWORD W16_LocalFree(Args16 *a) {
    int h; BYTE *seg=heap(a->task,&h); WORD handle=(WORD)a->a[0];
    if(!seg || !local_valid(seg,h,handle)) return handle;
    put16(seg+handle-2,0); return 0;
}
DWORD W16_LocalReAlloc(Args16 *a) {
    int h; BYTE *seg=heap(a->task,&h); WORD handle=(WORD)a->a[0],bytes=(WORD)a->a[1],need=(WORD)((bytes+4+3)&~3U),at,size,fresh;
    if(!seg || !local_valid(seg,h,handle) || bytes>0xfff0) return 0;
    if(need<8) need=8;
    at=(WORD)(handle-4); size=get16(seg+at);
    if(at+size<heaps[h].end) coalesce(seg,(WORD)(at+size),heaps[h].end);
    if(size<need && at+size<heaps[h].end && !(get16(seg+at+size+2)&1) && size+get16(seg+at+size)>=need) {
        size=(WORD)(size+get16(seg+at+size)); put16(seg+at,size);
    }
    if(size>=need) {
        if(size-need>=8) {put16(seg+at+need,(WORD)(size-need)); put16(seg+at+need+2,0); put16(seg+at,need);}
        return handle;
    }
    fresh=local_alloc(seg,h,(UINT)a->a[2],bytes);
    if(!fresh) return 0;
    memmove(seg+fresh,seg+handle,(DWORD)size-4);
    put16(seg+at+2,0);
    return fresh;
}
DWORD W16_LocalSize(Args16 *a) {int h; BYTE *seg=heap(a->task,&h); WORD handle=(WORD)a->a[0]; return seg && local_valid(seg,h,handle)?(DWORD)get16(seg+handle-4)-4:0;}
DWORD W16_LocalLock(Args16 *a) {return a->a[0]?MAKELONG((WORD)a->a[0],Seg16(a->task,3)):0;}
DWORD W16_LocalUnlock(Args16 *a) {(void)a; return 0;}
DWORD W16_LocalHandle(Args16 *a) {return a->a[0];}
DWORD W16_LocalFlags(Args16 *a) {(void)a; return 0;}
DWORD W16_LocalCompact(Args16 *a) {
    int h; BYTE *seg=heap(a->task,&h); WORD at,best=0;
    if(!seg) return 0;
    for(at=heaps[h].start;at<heaps[h].end && get16(seg+at);at=(WORD)(at+get16(seg+at))) {
        coalesce(seg,at,heaps[h].end);
        if(!(get16(seg+at+2)&1) && get16(seg+at)-4>best) best=(WORD)(get16(seg+at)-4);
    }
    return best;
}
