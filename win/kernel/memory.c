/* SPDX-License-Identifier: GPL-2.0-or-later
 * Global and local memory. Blocks never move: a moveable handle names a slot
 * holding the block's address and lock count, a fixed handle is the address.
 * A moveable handle is a 16-bit value, as Win16 sources keep one in a WORD
 * or in half of a DDE message's lParam: a slot's index times 4 above
 * SLOT_BASE, between USER's handles and GDI's.
 * Memory belongs to the allocating task and is freed when the task ends,
 * unless allocated with GMEM_SHARE/GMEM_DDESHARE (or outside any task).
 * Local and global heaps are the same heap, as in Win32.
 */
#include "kernel.h"
#define CHUNK_BYTES (256*1024)
#define MAGIC_USED 0x4b4d454dU
#define MAGIC_FREE 0x45455246U
#define HEADER_LARGE 0x80000000U
#define SLOTS 4096
#define SLOT_USED 1U
#define SLOT_DISCARDED 2U
#define SLOT_BASE 0x4004
typedef char slots_below_gdi[SLOT_BASE+(SLOTS-1)*4<0x8004?1:-1];

typedef struct Header {
    wh_u64 size;               /* usable bytes after the header */
    wh_u32 magic,flags,owner,slot; /* slot: moveable handle index + 1 */
    struct Header *next,*prev; /* free list (by address) or used list */
    wh_u64 reserved;
} Header;
typedef struct Chunk {struct Chunk *next; wh_u64 bytes; wh_u32 large,reserved; wh_u64 pad;} Chunk;
typedef struct {void *ptr; wh_u32 locks,flags,owner,bytes;} Slot;

static Chunk *chunks;
static Header *free_list,*used_list;
static Slot slots[SLOTS];
static unsigned next_slot;

static void link_used(Header *h) {
    h->prev=NULL; h->next=used_list; if(used_list) used_list->prev=h; used_list=h;
}
static void unlink_used(Header *h) {
    if(h->prev) h->prev->next=h->next; else used_list=h->next;
    if(h->next) h->next->prev=h->prev;
}
static BYTE *end_of(Header *h) {return (BYTE *)(h+1)+h->size;}
/* Address-ordered insertion; neighbours that touch are merged. */
static void insert_free(Header *h) {
    Header *before=NULL,*after=free_list;
    while(after && after<h) {before=after; after=after->next;}
    h->magic=MAGIC_FREE; h->flags=0; h->owner=0; h->slot=0;
    if(after && end_of(h)==(BYTE *)after) {h->size+=sizeof(Header)+after->size; after=after->next;}
    h->next=after;
    if(before && end_of(before)==(BYTE *)h) {before->size+=sizeof(Header)+h->size; before->next=after;}
    else if(before) before->next=h;
    else free_list=h;
}
static int grow(void) {
    Chunk *c=(Chunk *)wh_alloc(CHUNK_BYTES); Header *h;
    if(!c) return 0;
    c->next=chunks; c->bytes=CHUNK_BYTES; c->large=0; chunks=c;
    h=(Header *)(c+1); h->size=CHUNK_BYTES-sizeof(Chunk)-sizeof(Header);
    insert_free(h); return 1;
}
static Header *heap_alloc(wh_u64 bytes) {
    wh_u64 need=(bytes+15)&~(wh_u64)15;
    Header **link,*h;
    if(!need) need=16;
    if(need>CHUNK_BYTES/2) {
        Chunk *c=(Chunk *)wh_alloc(sizeof(Chunk)+sizeof(Header)+need);
        if(!c) return NULL;
        c->next=chunks; c->bytes=sizeof(Chunk)+sizeof(Header)+need; c->large=1; chunks=c;
        h=(Header *)(c+1); h->size=need; h->flags=HEADER_LARGE; h->magic=MAGIC_USED;
        return h;
    }
    for(;;) {
        for(link=&free_list;(h=*link)!=NULL;link=&h->next) if(h->size>=need) {
            if(h->size>=need+sizeof(Header)+64) {
                Header *rest=(Header *)((BYTE *)(h+1)+need);
                rest->size=h->size-need-sizeof(Header); rest->magic=MAGIC_FREE;
                rest->flags=rest->owner=rest->slot=0; rest->next=h->next;
                h->size=need; *link=rest;
            } else *link=h->next;
            h->magic=MAGIC_USED; h->flags=0; return h;
        }
        if(!grow()) return NULL;
    }
}
static void heap_free(Header *h) {
    h->magic=0;
    if(h->flags&HEADER_LARGE) {
        Chunk **link,*c=(Chunk *)h-1;
        for(link=&chunks;*link;link=&(*link)->next) if(*link==c) {*link=c->next; break;}
        wh_free(c,c->bytes);
        return;
    }
    insert_free(h);
}

static HGLOBAL handle_of(const Slot *s) {return (HGLOBAL)(ULONG_PTR)(SLOT_BASE+(ULONG_PTR)(s-slots)*4);}
static Slot *slot_of(HGLOBAL handle) {
    ULONG_PTR v=(ULONG_PTR)handle;
    if(v<SLOT_BASE || (v-SLOT_BASE)%4 || (v-SLOT_BASE)/4>=SLOTS) return NULL;
    return slots[(v-SLOT_BASE)/4].flags&SLOT_USED?&slots[(v-SLOT_BASE)/4]:NULL;
}
/* A fixed block's address; never a 16-bit value. */
static Header *header_of(const void *p) {
    Header *h;
    if((ULONG_PTR)p<0x10000 || ((ULONG_PTR)p&15)) return NULL;
    h=(Header *)p-1;
    return h->magic==MAGIC_USED?h:NULL;
}
static Slot *new_slot(void) {
    unsigned i;
    for(i=0;i<SLOTS;i++) {
        Slot *s=&slots[(next_slot+i)%SLOTS];
        if(!s->flags) {next_slot=(next_slot+i+1)%SLOTS; return s;}
    }
    return NULL;
}
static wh_u32 owner_for(UINT flags) {return flags&GMEM_SHARE?0:wh_task_current();}
static Header *allocate(UINT flags,wh_u64 bytes,wh_u32 owner) {
    Header *h=heap_alloc(bytes?bytes:1);
    if(!h) return NULL;
    h->flags|=flags&0xffff; h->owner=owner; h->slot=0;
    if(flags&GMEM_ZEROINIT) memset(h+1,0,(size_t)h->size);
    link_used(h); return h;
}
static void release(Header *h) {unlink_used(h); heap_free(h);}

HGLOBAL WINAPI GlobalAlloc(UINT flags,DWORD bytes) {
    Slot *s=NULL; Header *h=NULL; wh_u32 owner=owner_for(flags);
    if(flags&GMEM_MOVEABLE) {
        s=new_slot(); if(!s) return NULL;
        s->locks=0; s->owner=owner; s->bytes=0; s->ptr=NULL;
        s->flags=SLOT_USED|(bytes?0:SLOT_DISCARDED);
        if(!bytes) return handle_of(s);
    }
    h=allocate(flags,bytes,owner);
    if(!h) {if(s) s->flags=0; return NULL;}
    if(!s) return (HGLOBAL)(h+1);
    h->slot=(wh_u32)(s-slots)+1; s->ptr=h+1; s->bytes=bytes;
    return handle_of(s);
}
HGLOBAL WINAPI GlobalFree(HGLOBAL handle) {
    Slot *s=slot_of(handle); Header *h;
    if(s) {
        if(s->ptr) {h=header_of(s->ptr); if(h) release(h);}
        s->flags=0; s->ptr=NULL; return NULL;
    }
    h=header_of(handle);
    if(!h || h->slot) return handle;
    release(h); return NULL;
}
void FAR *WINAPI GlobalLock(HGLOBAL handle) {
    Slot *s=slot_of(handle);
    if(s) {if(!s->ptr) return NULL; if(s->locks<0xff) s->locks++; return s->ptr;}
    return header_of(handle)?(void *)handle:NULL;
}
BOOL WINAPI GlobalUnlock(HGLOBAL handle) {
    Slot *s=slot_of(handle);
    if(!s) return FALSE;
    if(s->locks) s->locks--;
    return s->locks!=0;
}
DWORD WINAPI GlobalSize(HGLOBAL handle) {
    Slot *s=slot_of(handle); Header *h=header_of(s?s->ptr:handle);
    if(s) return s->ptr?s->bytes:0;
    return h?(DWORD)h->size:0;
}
UINT WINAPI GlobalFlags(HGLOBAL handle) {
    Slot *s=slot_of(handle); Header *h;
    if(!s) return header_of(handle)?0:GMEM_DISCARDED;
    h=header_of(s->ptr);
    return s->locks|(s->ptr?0:GMEM_DISCARDED)|(h?h->flags&GMEM_DISCARDABLE:0);
}
HGLOBAL WINAPI GlobalHandle(LPCVOID p) {
    Header *h=header_of(p);
    if(!h) return NULL;
    return h->slot?handle_of(&slots[h->slot-1]):(HGLOBAL)p;
}
/* Fixed blocks move only with GMEM_MOVEABLE; moveable ones only when unlocked. */
HGLOBAL WINAPI GlobalReAlloc(HGLOBAL handle,DWORD bytes,UINT flags) {
    Slot *s=slot_of(handle); Header *h=header_of(s?s->ptr:handle),*n;
    wh_u64 keep;
    if(flags&GMEM_MODIFY) {
        if(h) h->flags=(h->flags&~(wh_u32)GMEM_DISCARDABLE)|(flags&GMEM_DISCARDABLE);
        return h || s?handle:NULL;
    }
    if(!h && !(s && !s->ptr)) return NULL;
    if(h && bytes<=h->size) {if(s) s->bytes=bytes; return handle;}
    if(!s && !(flags&GMEM_MOVEABLE)) return NULL;
    if(s && s->locks) return NULL;
    n=allocate((h?h->flags:0)|(flags&GMEM_ZEROINIT),bytes,h?h->owner:s->owner);
    if(!n) return NULL;
    if(h) {
        keep=h->size<n->size?h->size:n->size; memcpy(n+1,h+1,(size_t)keep);
        if(flags&GMEM_ZEROINIT && n->size>keep) memset((BYTE *)(n+1)+keep,0,(size_t)(n->size-keep));
        n->slot=h->slot; release(h);
    }
    if(s) {n->slot=(wh_u32)(s-slots)+1; s->ptr=n+1; s->bytes=bytes; s->flags&=~SLOT_DISCARDED; return handle;}
    return (HGLOBAL)(n+1);
}
DWORD WINAPI GlobalCompact(DWORD bytes) {(void)bytes; return 16UL*1024*1024;}

static UINT local_flags(UINT flags) {
    UINT g=flags&(LMEM_MOVEABLE|LMEM_ZEROINIT|LMEM_MODIFY);
    if((flags&LMEM_DISCARDABLE)==LMEM_DISCARDABLE) g|=GMEM_DISCARDABLE;
    return g;
}
HLOCAL WINAPI LocalAlloc(UINT flags,UINT bytes) {return GlobalAlloc(local_flags(flags),bytes);}
HLOCAL WINAPI LocalReAlloc(HLOCAL h,UINT bytes,UINT flags) {return GlobalReAlloc(h,bytes,local_flags(flags));}
HLOCAL WINAPI LocalFree(HLOCAL h) {return GlobalFree(h);}
void NEAR *WINAPI LocalLock(HLOCAL h) {return GlobalLock(h);}
BOOL WINAPI LocalUnlock(HLOCAL h) {return GlobalUnlock(h);}
UINT WINAPI LocalSize(HLOCAL h) {return (UINT)GlobalSize(h);}
UINT WINAPI LocalFlags(HLOCAL h) {return GlobalFlags(h);}
HLOCAL WINAPI LocalHandle(LPCVOID p) {return GlobalHandle(p);}

void MemoryInit(void) {chunks=NULL; free_list=used_list=NULL; memset(slots,0,sizeof(slots)); next_slot=0;}
void MemoryFreeTask(wh_u32 task) {
    Header *h=used_list,*next; unsigned i;
    while(h) {
        next=h->next;
        if(task && h->owner==task) {if(h->slot) slots[h->slot-1].flags=0; release(h);}
        h=next;
    }
    for(i=0;i<SLOTS;i++) if(slots[i].flags && slots[i].owner==task && !slots[i].ptr) slots[i].flags=0;
}
void MemoryShutdown(void) {
    Chunk *c=chunks,*next;
    while(c) {next=c->next; wh_free(c,c->bytes); c=next;}
    MemoryInit();
}
