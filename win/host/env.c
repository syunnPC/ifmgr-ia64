/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "env.h"
void pe_flush_code(void *,u64);
u64 host_pages_out;
unsigned host_files_out;
static const IoServices *io;
/* Keep program memory below 2 GiB for pointers stored in 32-bit LONGs;
 * WOW segments also require addresses below 4 GiB. Use firmware allocations
 * when low, otherwise 4 MiB low-memory pools to reduce firmware range counts.
 * Requests over 1 MiB get dedicated ranges. */
#define LOW_START 0x110000ULL
#define LOW_END 0x80000000ULL
#define POOL_PAGES 1024
#define POOLS 256
typedef struct {u8 *base; u32 count; u8 used[POOL_PAGES/8];} Pool;
static Pool pools[POOLS];
static int high_memory;
static int pool_bit(const Pool *q,u32 i) {return q->used[i/8]>>(i%8)&1;}
static void pool_mark(Pool *q,u32 first,u32 pages,int on) {
    for(u32 i=first;i<first+pages;i++) if(on) q->used[i/8]|=(u8)(1<<(i%8)); else q->used[i/8]&=(u8)~(1<<(i%8));
    q->count=on?q->count+pages:q->count-pages;
}
static void *pool_take(u32 pages) {
    Pool *empty=NULL;
    for(unsigned n=0;n<POOLS;n++) {
        Pool *q=&pools[n];
        if(!q->base) {if(!empty) empty=q; continue;}
        if(POOL_PAGES-q->count<pages) continue;
        for(u32 i=0,run=0;i<POOL_PAGES;i++) {
            run=pool_bit(q,i)?0:run+1;
            if(run==pages) {pool_mark(q,i+1-pages,pages,1); return q->base+(size_t)(i+1-pages)*4096;}
        }
    }
    if(!empty || io->alloc_pages_range(io->context,POOL_PAGES,LOW_START,LOW_END,(void **)&empty->base)) return NULL;
    memset(empty->used,0,sizeof(empty->used)); empty->count=0;
    pool_mark(empty,0,pages,1); return empty->base;
}
static int pool_give(void *memory,u32 pages) {
    for(unsigned n=0;n<POOLS;n++) {
        Pool *q=&pools[n];
        if(!q->base || (u8 *)memory<q->base || (u8 *)memory>=q->base+POOL_PAGES*4096) continue;
        pool_mark(q,(u32)(((u8 *)memory-q->base)/4096),pages,0);
        if(!q->count) {io->free_pages(io->context,q->base,POOL_PAGES); q->base=NULL;}
        return 1;
    }
    return 0;
}
int host_low_pages(u32 pages,void **out) {
    *out=NULL;
    if(!high_memory) {
        int e=io->alloc_pages(io->context,pages,out); if(e) return e;
        if((u64)(uintptr_t)*out+(u64)pages*4096<=LOW_END) return 0;
        io->free_pages(io->context,*out,pages); *out=NULL; high_memory=1;
    }
    if(io->size<offsetof(IoServices,range_free)+sizeof(io->range_free) || !io->alloc_pages_range) return DE_NOMEM;
    if(pages>POOL_PAGES/4) return io->alloc_pages_range(io->context,pages,LOW_START,LOW_END,out);
    return (*out=pool_take(pages))!=NULL?0:DE_NOMEM;
}
void host_low_free(void *memory,u32 pages) {if(memory && !pool_give(memory,pages)) io->free_pages(io->context,memory,pages);}
static int env_alloc(void *ctx,u64 bytes,void **out) {
    (void)ctx; u32 pages=(u32)((bytes+4095)/4096);
    int e=host_low_pages(pages,out);
    if(!e) {memset(*out,0,(size_t)pages*4096); host_pages_out+=pages;}
    return e;
}
static void env_free(void *ctx,void *memory,u64 bytes) {
    (void)ctx; u32 pages=(u32)((bytes+4095)/4096);
    host_low_free(memory,pages); host_pages_out-=pages;
}
static int env_read(void *ctx,const char *path,void **data,u32 *size) {
    (void)ctx; unsigned h; u32 end,pos,got; void *buffer=NULL;
    int e=dos_open(path,DOS_OPEN_READ,0,&h); if(e) return e;
    e=dos_seek(h,0,2,&end);
    if(!e && !end) e=DE_FORMAT;
    if(!e) e=dos_alloc((end+15)/16,&buffer);
    if(!e) e=dos_seek(h,0,0,&pos);
    if(!e) {e=dos_read(h,buffer,end,&got); if(!e && got!=end) e=DE_IO;}
    dos_close(h);
    if(e) {if(buffer) dos_free(buffer); return e;}
    *data=buffer; *size=end; host_files_out++; return 0;
}
static void env_free_file(void *ctx,void *data,u32 size) {(void)ctx; (void)size; dos_free(data); host_files_out--;}
static void env_flush(void *ctx,void *memory,u64 bytes) {(void)ctx; pe_flush_code(memory,bytes);}
/* A GCC function pointer is a descriptor address, as in WDK images. */
static int env_dll_main(void *ctx,const void *descriptor,void *instance,u32 reason) {
    (void)ctx; return ((int (*)(void *,u32,void *))(uintptr_t)descriptor)(instance,reason,NULL);
}
void host_env(PeEnv *env,const IoServices *services,const char *system_dir) {
    io=services;
    *env=(PeEnv){NULL,env_alloc,env_free,env_read,env_free_file,env_flush,env_dll_main,system_dir};
}
