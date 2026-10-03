/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * Copyright (c) Microsoft Corporation (translated MS-DOS 4 portions,
 * MIT License; see vendor/msdos4/LICENSE). Other code: GPL-2.0-or-later.
 * C translation of DOS/ALLOC.ASM: arena_next, Check_signature, Coalesce,
 * $ALLOC, $DEALLOC, $SETBLOCK, arena_free_process. 64-bit native pointers;
 * sizes remain in paragraphs, and each MCB still occupies one paragraph.
 */
#include "arena.h"
typedef struct {u32 owner, size; u8 signature, reserved[7];} MCB;
_Static_assert(sizeof(MCB)==16,"MCB paragraph");
static MCB *at(Arena *a,u32 n) {return (MCB *)(a->base+(u64)n*16);}
static int valid(Arena *a,u32 n) {
    if(n>=a->paragraphs) return DE_ARENA;
    MCB *m=at(a,n);
    if(m->size>=a->paragraphs-n) return DE_ARENA;
    u32 next=n+1+m->size;
    if(m->signature=='Z') return next==a->paragraphs?0:DE_ARENA;
    return m->signature=='M' && next<a->paragraphs?0:DE_ARENA;
}
static int coalesce(Arena *a,u32 n) {
    int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
    while(m->signature!='Z') {
        u32 next=n+m->size+1; e=valid(a,next); if(e) return e;
        MCB *b=at(a,next); if(b->owner) break;
        m->size+=b->size+1; m->signature=b->signature;
    }
    return 0;
}
void arena_init(Arena *a,void *base,u32 bytes) {
    a->base=base; a->paragraphs=bytes/16; a->method=0;
    MCB *m=at(a,0); memset(m,0,sizeof(*m)); m->size=a->paragraphs-1; m->signature='Z';
}
int arena_check(Arena *a,u32 *largest) {
    *largest=0;
    for(u32 n=0;;) {
        int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
        if(!m->owner) {e=coalesce(a,n); if(e) return e; if(m->size>*largest) *largest=m->size;}
        if(m->signature=='Z') return 0;
        n+=m->size+1;
    }
}
/* The index'th block (from 0): its MCB's paragraph, its size and owner;
 * DE_NOMORE past the last. */
int arena_block(Arena *a,u32 index,u32 *paragraph,u32 *size,u32 *owner) {
    for(u32 n=0;;index--) {
        int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
        if(!index) {*paragraph=n; *size=m->size; *owner=m->owner; return 0;}
        if(m->signature=='Z') return DE_NOMORE;
        n+=m->size+1;
    }
}
int arena_alloc(Arena *a,u32 paragraphs,u32 owner,void **out,u32 *largest) {
    *out=NULL; *largest=0; u32 first=UINT32_MAX,best=UINT32_MAX,last=UINT32_MAX;
    if(!owner || a->method>2) return DE_FUNCTION;
    for(u32 n=0;;) {
        int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
        if(!m->owner) {
            e=coalesce(a,n); if(e) return e;
            if(m->size>*largest) *largest=m->size;
            if(m->size>=paragraphs) {
                if(first==UINT32_MAX) first=n;
                if(best==UINT32_MAX || at(a,best)->size>m->size) best=n;
                last=n;
            }
        }
        if(m->signature=='Z') break;
        n+=m->size+1;
    }
    if(first==UINT32_MAX) return DE_NOMEM;
    u32 n=a->method==2?last:a->method==1?best:first; MCB *m=at(a,n);
    u32 left=m->size-paragraphs;
    if(left) {
        u32 newpos=n+(a->method==2?left:paragraphs+1); MCB *b=at(a,newpos);
        memset(b,0,sizeof(*b)); b->signature=m->signature; m->signature='M';
        if(a->method==2) {m->size=left-1; b->size=paragraphs; m=b; n=newpos;}
        else {m->size=paragraphs; b->size=left-1;}
    }
    m->owner=owner; *out=at(a,n+1); return 0;
}
static int find(Arena *a,void *ptr,u32 owner,u32 *found) {
    uintptr_t start=(uintptr_t)a->base,p=(uintptr_t)ptr;
    if(p<start+16 || p>start+(u64)a->paragraphs*16 || (p-start)%16) return DE_BLOCK;
    u32 target=(p-start)/16-1;
    for(u32 n=0;;) {
        int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
        if(n==target) {if(m->owner!=owner || !owner) return DE_BLOCK; *found=n; return 0;}
        if(m->signature=='Z' || n>target) return DE_BLOCK;
        n+=m->size+1;
    }
}
int arena_free(Arena *a,void *ptr,u32 owner) {
    u32 n; int e=find(a,ptr,owner,&n); if(e) return e;
    at(a,n)->owner=0; return 0;
}
int arena_resize(Arena *a,void *ptr,u32 size,u32 owner,u32 *largest) {
    u32 n; int e=find(a,ptr,owner,&n); if(e) return e;
    e=coalesce(a,n); if(e) return e; MCB *m=at(a,n); *largest=m->size;
    if(size>m->size) return DE_NOMEM;
    if(size<m->size) {
        MCB *b=at(a,n+size+1); memset(b,0,sizeof(*b));
        b->signature=m->signature; b->size=m->size-size-1;
        m->size=size; m->signature='M';
    }
    return 0;
}
int arena_free_process(Arena *a,u32 owner) {
    for(u32 n=0;;) {
        int e=valid(a,n); if(e) return e; MCB *m=at(a,n);
        if(m->owner==owner) m->owner=0;
        if(m->signature=='Z') return 0;
        n+=m->size+1;
    }
}
