/* SPDX-License-Identifier: GPL-2.0-or-later
 * The heap: blocks carved from fixed global memory (GlobalAlloc), first
 * fit, neighbours merged when freed. A block has a 16-byte header with its
 * size; free blocks are kept in address order, a used one's next is USED.
 * Each arena of global memory starts with its own header, so the heap can be
 * walked (_heapwalk, _heapchk). C89. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#define ARENA (64U*1024U)
typedef struct Block {size_t size; struct Block *next;} Block; /* size: the whole block; next: the next free one */
typedef struct Arena {struct Arena *next; size_t size;} Arena; /* size: the blocks' bytes */
#define USED ((Block *)1)
static Block *free_list;
static Arena *arenas;
static void insert(Block *b) {
    Block **p=&free_list;
    while(*p && *p<b) p=&(*p)->next;
    b->next=*p; *p=b;
    /* Merge with the next and with the one before. */
    if(b->next && (char *)b+b->size==(char *)b->next) {b->size+=b->next->size; b->next=b->next->next;}
    if(p!=&free_list) {
        Block *prev=(Block *)((char *)p-offsetof(Block,next));
        if((char *)prev+prev->size==(char *)b) {prev->size+=b->size; prev->next=b->next;}
    }
}
static int grow(size_t need) {
    size_t bytes=need+sizeof(Block)>ARENA?need+sizeof(Block):ARENA;
    Arena *a; Block *b;
    if(bytes>0xffffffffU-sizeof(Arena)) return 0;
    a=(Arena *)GlobalAlloc(GMEM_FIXED,(DWORD)(bytes+sizeof(Arena)));
    if(!a) return 0;
    a->size=bytes; a->next=arenas; arenas=a;
    b=(Block *)(a+1); b->size=bytes; insert(b);
    return 1;
}
void *malloc(size_t n) {
    Block **p,*b; size_t need;
    if(n>0x7fffffffU) {errno=ENOMEM; return NULL;}
    need=((n+15)&~(size_t)15)+sizeof(Block);
    for(;;) {
        for(p=&free_list;*p;p=&(*p)->next) {
            b=*p;
            if(b->size<need) continue;
            if(b->size>=need+2*sizeof(Block)) {
                Block *rest=(Block *)((char *)b+need);
                rest->size=b->size-need; rest->next=b->next; *p=rest; b->size=need;
            } else *p=b->next;
            b->next=USED;
            return b+1;
        }
        if(!grow(need)) {errno=ENOMEM; return NULL;}
    }
}
void free(void *m) {if(m) insert((Block *)m-1);}
size_t _msize(void *m) {return ((Block *)m-1)->size-sizeof(Block);}
void *calloc(size_t n,size_t size) {
    void *m;
    if(size && n>0x7fffffffU/size) {errno=ENOMEM; return NULL;}
    m=malloc(n*size); if(m) memset(m,0,n*size);
    return m;
}
void *realloc(void *m,size_t n) {
    void *r;
    if(!m) return malloc(n);
    if(!n) {free(m); return NULL;}
    if(n<=_msize(m)) return m;
    r=malloc(n); if(!r) return NULL;
    memcpy(r,m,_msize(m)); free(m);
    return r;
}
void *_expand(void *m,size_t n) {return m && n<=_msize(m)?m:NULL;}

/* Microsoft C's heap checks. */
static int check(void) {
    const Arena *a; const Block *b,*f=free_list;
    if(!arenas) return _HEAPEMPTY;
    for(a=arenas;a;a=a->next) {
        size_t at=0;
        for(b=(const Block *)(a+1);at<a->size;at+=b->size,b=(const Block *)((const char *)b+b->size))
            if(b->size<sizeof(Block) || b->size&15 || b->size>a->size-at || (b->next!=USED && b->next && b->next<=b)) return _HEAPBADNODE;
    }
    for(;f;f=f->next) if(f->next==USED) return _HEAPBADNODE;
    return _HEAPOK;
}
int _heapchk(void) {return check();}
int _heapset(unsigned fill) {
    Block *f; int r=check();
    if(r==_HEAPOK) for(f=free_list;f;f=f->next) memset(f+1,(int)fill,f->size-sizeof(Block));
    return r;
}
int _heapmin(void) {return 0;}
int _heapwalk(_HEAPINFO *info) {
    const Arena *a; const Block *b; int r=check();
    if(r!=_HEAPOK) return r;
    if(!info->_pentry) a=arenas,b=(const Block *)(arenas+1);
    else {
        b=(const Block *)info->_pentry-1;
        for(a=arenas;a && !((const char *)b>=(const char *)(a+1) && (const char *)b<(const char *)(a+1)+a->size);a=a->next) {}
        if(!a) return _HEAPBADPTR;
        b=(const Block *)((const char *)b+b->size);
        if((const char *)b>=(const char *)(a+1)+a->size) {
            if(!(a=a->next)) return _HEAPEND;
            b=(const Block *)(a+1);
        }
    }
    info->_pentry=(int *)(b+1); info->_size=b->size-sizeof(Block); info->_useflag=b->next==USED?_USEDENTRY:_FREEENTRY;
    return _HEAPOK;
}
