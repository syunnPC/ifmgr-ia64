/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fat_io.h"
typedef struct {u32 lba; int attempted; u8 before[512],after[512];} Change;
typedef struct FatTxPage {
    struct FatTxPage *next,*previous;
    unsigned used;
    Change changes[3];
} FatTxPage;
_Static_assert(sizeof(FatTxPage)<=4096,"FAT transaction page exceeds one I/O page");
static unsigned io_error(Fat *f,u32 operation,u32 sector,int error,u32 attempt,u32 flags) {
    if(!f->error_handler || (error!=DE_IO && error!=DE_NOTREADY && error!=DE_READONLY &&
       error!=DE_SEEK && error!=DE_CHANGED)) return DOS_CRITICAL_FAIL;
    FatIoError info={operation,sector,attempt,flags,error};
    return f->error_handler(f->error_context,&info);
}
int fat_io_read(Fat *f,u32 lba,void *buffer,u32 flags) {
    for(u32 attempt=1;;attempt++) {
        int e=f->disk.read(f->disk.ctx,lba,buffer); if(!e) return 0;
        unsigned action=io_error(f,DOS_CRITICAL_READ,lba,e,attempt,flags);
        if(action==DOS_CRITICAL_IGNORE && (flags&DOS_ERROR_DATA) && (e==DE_IO || e==DE_SEEK)) {
            memset(buffer,0,512); return 0;
        }
        if(action!=DOS_CRITICAL_RETRY || e==DE_CHANGED || attempt==UINT32_MAX) return e;
    }
}
int fat_verify_writes;
static int io_write(Fat *f,u32 lba,const void *buffer) {
    for(u32 attempt=1;;attempt++) {
        int e=f->disk.write(f->disk.ctx,lba,buffer);
        if(!e && fat_verify_writes) {
            u8 check[512]; e=f->disk.read(f->disk.ctx,lba,check);
            if(!e && memcmp(check,buffer,512)) e=DE_IO;
        }
        if(!e) return 0;
        unsigned action=io_error(f,DOS_CRITICAL_WRITE,lba,e,attempt,DOS_ERROR_COMMIT);
        if(action!=DOS_CRITICAL_RETRY || e==DE_CHANGED || attempt==UINT32_MAX) return e;
    }
}
int fat_io_flush(Fat *f,u32 flags) {
    if(!f->disk.flush) return 0;
    for(u32 attempt=1;;attempt++) {
        int e=f->disk.flush(f->disk.ctx); if(!e) return 0;
        unsigned action=io_error(f,DOS_CRITICAL_FLUSH,UINT32_MAX,e,attempt,flags);
        if(action!=DOS_CRITICAL_RETRY || e==DE_CHANGED || attempt==UINT32_MAX) return e;
    }
}
static Change *find(Fat *f,u32 lba) {
    for(FatTxPage *p=f->tx_first;p;p=p->next)
        for(unsigned i=0;i<p->used;i++) if(p->changes[i].lba==lba) return &p->changes[i];
    return NULL;
}
int fat_sector_read(Fat *f,u32 lba,void *buf) {
    if(f->faulted || lba>=f->total) return DE_IO;
    Change *c=find(f,lba);
    if(c) {memcpy(buf,c->after,512); return 0;}
    return fat_io_read(f,lba,buf,0);
}
int fat_data_read(Fat *f,u32 lba,void *buf) {
    if(f->faulted || lba>=f->total) return DE_IO;
    Change *c=find(f,lba); if(c) {memcpy(buf,c->after,512); return 0;}
    return fat_io_read(f,lba,buf,DOS_ERROR_DATA);
}
int fat_sector_write(Fat *f,u32 lba,const void *buf) {
    if(f->faulted || lba>=f->total) return DE_IO;
    if(f->disk.readonly) return DE_READONLY;
    if(!f->tx_depth) return DE_FUNCTION;
    if(f->tx_error) return f->tx_error;
    Change *c=find(f,lba);
    if(!c) {
        FatTxPage *p=f->tx_last;
        if(!p || p->used==ARRAY_SIZE(p->changes)) {
            void *memory; int e=platform_fat_page(&memory);
            if(e) {f->tx_error=e; return e;}
            FatTxPage *next=memory; memset(next,0,sizeof(*next)); next->previous=p;
            if(p) p->next=next; else f->tx_first=next;
            f->tx_last=p=next;
        }
        c=&p->changes[p->used];
        int e=fat_io_read(f,lba,c->before,0);
        if(e) {f->tx_error=e; return e;}
        c->lba=lba; p->used++;
    }
    memcpy(c->after,buf,512);
    if(f->cache_valid && f->cache_sector==lba) f->cache_valid=0;
    return 0;
}
int fat_begin(Fat *f) {
    if(f->faulted) return DE_IO;
    if(f->disk.readonly) {io_error(f,DOS_CRITICAL_WRITE,UINT32_MAX,DE_READONLY,1,0); return DE_READONLY;}
    if(f->tx_error) return f->tx_error;
    f->tx_depth++; return 0;
}
static int rollback(Fat *f) {
    int failed=0;
    for(FatTxPage *p=f->tx_last;p;p=p->previous) for(unsigned i=p->used;i;i--) {
        Change *c=&p->changes[i-1]; if(!c->attempted) continue;
        int e=f->disk.write(f->disk.ctx,c->lba,c->before);
        if(e) {
            /* A device can reject a write without changing the sector, or
             * report failure after writing. Verify before declaring recovery. */
            u8 actual[512];
            if(f->disk.read(f->disk.ctx,c->lba,actual) || memcmp(actual,c->before,512)) failed=1;
        }
    }
    /* Recovery uses the original snapshot directly. Never run application
     * handlers while restoring before-images or verifying the restoration. */
    if(f->disk.flush && f->disk.flush(f->disk.ctx)) failed=1;
    return failed?DE_IO:0;
}
int fat_end(Fat *f,int error) {
    if(!f->tx_depth) return DE_FUNCTION;
    if(error && !f->tx_error) f->tx_error=error;
    if(--f->tx_depth) return f->tx_error;
    int e=f->tx_error,attempted=0;
    if(!e) {
        for(FatTxPage *p=f->tx_first;p && !e;p=p->next) for(unsigned i=0;i<p->used;i++) {
            Change *c=&p->changes[i];
            if(!memcmp(c->before,c->after,512)) continue;
            c->attempted=1; attempted=1;
            e=io_write(f,c->lba,c->after); if(e) break;
        }
        if(!e && attempted) e=fat_io_flush(f,DOS_ERROR_COMMIT);
        if(e && attempted && rollback(f)) f->faulted=1;
    }
    for(FatTxPage *p=f->tx_first;p;) {
        FatTxPage *next=p->next; platform_fat_free_page(p); p=next;
    }
    f->tx_first=f->tx_last=NULL; f->tx_error=0; f->cache_valid=0;
    return e;
}
