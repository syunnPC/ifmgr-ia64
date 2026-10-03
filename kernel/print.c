/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Resident PRINT queue, based on DOS 4 PRINT_R.ASM.
 * After installation, DOS calls allow /M ticks of work (55 ms each) once
 * /S ticks have elapsed; keyboard waits supply idle time (INT 28h).
 * A device busy for /U ticks ends the turn. Tabs expand to 8-column stops;
 * EOF/^Z ends a file with FF. Cancel/read-error notices use CR/FF/BEL.
 * Spool handles are outside task handle tables; critical errors fail without
 * calling a handler (dos_spool_*).
 */
#include "device.h"
#include "print.h"
#define QUEUE_MAX 32U
#define BUFFER_MAX 16384U
#define TICK_MS 55U
#define OUT_MAX 512U

static struct {
    int installed,held,file_open,device_open,ending;
    char device[9];
    u32 buffer_bytes,busy_ms,max_ms,slice_ms;
    unsigned entries,count; /* the queue's size and use; queue[0] is printed first */
    char queue[QUEUE_MAX+1][DOS_PRINT_ENTRY];
    unsigned file,list; /* SFT references */
    u8 buffer[BUFFER_MAX]; u32 used,next;
    u8 out[OUT_MAX]; u32 out_used,out_next; /* bytes for the device, translated */
    unsigned column; u32 errors;
    u64 next_turn;
} spool;

void print_reset(void) {memset(&spool,0,sizeof(spool));}
static u64 now(void) {
    const IoServices *io=platform_io_services();
    return io && io->ticks_ms?io->ticks_ms(io->context):0;
}
static void notice(const char *text) {
    while(*text && spool.out_used<OUT_MAX) spool.out[spool.out_used++]=(u8)*text++;
}
static void notice_end(void) {notice("\r\f\a");}
/* The queue's first entry is done or canceled: the next moves up. */
static void dequeue(unsigned index) {
    if(index>=spool.count) return;
    for(unsigned i=index;i+1<spool.count;i++) memcpy(spool.queue[i],spool.queue[i+1],DOS_PRINT_ENTRY);
    spool.count--; memset(spool.queue[spool.count],0,DOS_PRINT_ENTRY);
}
static void close_file(void) {
    if(spool.file_open) dos_spool_close(spool.file);
    spool.file_open=spool.ending=0; spool.used=spool.next=0; spool.column=0;
}
static void close_device(void) {
    if(spool.device_open) dos_spool_close(spool.list);
    spool.device_open=0;
}

/* One step: 1 when something was done, 0 when there is nothing to do, a
 * negative DOS error when the device would not take a byte. */
static int step(void) {
    if(spool.out_next<spool.out_used) {
        if(!spool.device_open) {
            int e=dos_spool_open(spool.device,1,&spool.list); if(e) return -e;
            spool.device_open=1;
        }
        u32 done=0; int e=dos_spool_write(spool.list,spool.out+spool.out_next,spool.out_used-spool.out_next,&done);
        spool.out_next+=done;
        if(spool.out_next==spool.out_used) spool.out_next=spool.out_used=0;
        if(done) {spool.errors=0; return 1;}
        spool.errors++;
        return -(e?e:DE_NOTREADY);
    }
    if(spool.ending) { /* the form feed is out */
        close_file(); dequeue(0);
        if(!spool.count || spool.held) close_device();
        return 1;
    }
    if(spool.held || !spool.count) {
        if(!spool.file_open) close_device();
        return 0;
    }
    if(!spool.file_open) {
        int e=dos_spool_open(spool.queue[0],0,&spool.file);
        if(e) { /* gone since it was queued */
            notice("\r\n\r\n**********\r\nFile not found\r\n"); notice(spool.queue[0]); notice_end();
            dequeue(0); return 1;
        }
        spool.file_open=1; spool.used=spool.next=0; spool.column=0;
    }
    if(spool.next>=spool.used) {
        u32 got=0; int e=dos_spool_read(spool.file,spool.buffer,spool.buffer_bytes,&got);
        if(e) {
            notice("\r\n\r\n**********\r\n"); notice(dos_error(e)); notice(" error reading file\r\n");
            notice(spool.queue[0]); notice_end();
            close_file(); dequeue(0); return 1;
        }
        if(!got) {notice("\f"); spool.ending=1; return 1;}
        spool.used=got; spool.next=0;
    }
    while(spool.next<spool.used && spool.out_used<OUT_MAX-8) {
        u8 c=spool.buffer[spool.next++];
        if(c==0x1a) {notice("\f"); spool.ending=1; break;}
        if(c=='\t') {
            unsigned spaces=8-(spool.column&7);
            while(spaces--) spool.out[spool.out_used++]=' ';
            spool.column=(spool.column|7)+1; continue;
        }
        if(c=='\r') spool.column=0;
        else if(c=='\b') {if(spool.column) spool.column--;}
        else if(c>=32) spool.column++;
        spool.out[spool.out_used++]=c;
    }
    return 1;
}
/* A turn of at most budget ms: the time to wait before the next turn,
 * 0 when there is nothing to print. */
static u32 turn(u32 budget) {
    if(!spool.installed || dos_spool_begin()) return 0;
    u64 start=now(),busy=0; u32 wait=0;
    for(;;) {
        int r=step(); u64 t=now();
        if(!r) break;
        if(r<0) {
            if(!busy) busy=t;
            if(t-busy>=spool.busy_ms) {wait=TICK_MS; break;}
        } else busy=0;
        if(t-start>=budget) {wait=1; break;}
    }
    dos_spool_end();
    return wait;
}
/* At a DOS call (the timer's turns). */
void print_tick(void) {
    if(!spool.installed || (!spool.count && spool.out_next==spool.out_used && !spool.device_open)) return;
    u64 t=now(); if(t<spool.next_turn) return;
    turn(spool.max_ms);
    spool.next_turn=now()+spool.slice_ms;
}
/* While a program waits (DOS's INT 28h): a turn, and how long to wait
 * before the next one; 0 when there is nothing to print. */
u32 print_idle(void) {
    if(!spool.installed) return 0;
    return turn(TICK_MS);
}

/* The last component of a path in FCB form, * filling with ?. */
static int fcb_name(const char *name,char out[11]) {
    memset(out,' ',11); unsigned i=0,limit=8;
    for(;*name;name++) {
        if(*name=='.') {if(limit==11) return 0; i=8; limit=11; continue;}
        if(*name=='*') {while(i<limit) out[i++]='?'; continue;}
        if(i<limit) out[i++]=upper(*name);
    }
    return 1;
}
static const char *leaf(const char *path) {
    const char *l=path;
    for(const char *p=path;*p;p++) if(*p=='\\' || *p=='/' || *p==':') l=p+1;
    return l;
}
/* A queued path against a cancel pattern: the directories alike, the names
 * as a search compares them. */
static int matches(const char *pattern,const char *path) {
    const char *pl=leaf(pattern),*ql=leaf(path);
    if(pl-pattern!=ql-path) return 0;
    for(const char *a=pattern,*b=path;a<pl;a++,b++) if(upper(*a)!=upper(*b)) return 0;
    char x[11],y[11];
    if(!fcb_name(pl,x) || !fcb_name(ql,y)) return 0;
    for(unsigned i=0;i<11;i++) if(x[i]!='?' && x[i]!=y[i]) return 0;
    return 1;
}
static u32 limit(u32 value,u32 low,u32 high,u32 standard) {return !value?standard:value<low?low:value>high?high:value;}

int print_request(u32 function,DosPrintRequest *r) {
    if(!r || r->size<sizeof(*r)) return DE_FUNCTION;
    if(function==DOS_PRINT_QUERY) return spool.installed?0:DE_FUNCTION;
    if(function==DOS_PRINT_INSTALL) {
        if(spool.installed) return DE_ACCESS;
        char name[9]; unsigned n=0;
        for(const char *p=r->device;*p && *p!=':' && n<8;p++) name[n++]=upper(*p);
        name[n]=0;
        if(!n) strcopy(name,sizeof(name),"PRN");
        unsigned id=device_find(name); const DosDeviceSpec *spec=device_spec(id);
        if(!id || !spec || !(spec->attributes&DOS_DEVICE_CHAR) || !(spec->capabilities&DOS_DEVICE_CAN_WRITE)) return DE_NOFILE;
        print_reset();
        strcopy(spool.device,sizeof(spool.device),name);
        spool.buffer_bytes=limit(r->buffer_bytes,512,BUFFER_MAX,512);
        spool.busy_ms=limit(r->busy_ticks,1,255,1)*TICK_MS;
        spool.max_ms=limit(r->max_ticks,1,255,2)*TICK_MS;
        spool.slice_ms=limit(r->slice_ticks,1,255,8)*TICK_MS;
        spool.entries=limit(r->queue_entries,4,QUEUE_MAX,10);
        spool.installed=1; return 0;
    }
    if(!spool.installed) return DE_FUNCTION;
    switch(function) {
    case DOS_PRINT_SUBMIT: {
        size_t n=strlen(r->path);
        if(!n || n>=DOS_PRINT_ENTRY) return DE_PATH;
        if(spool.count>=spool.entries) return DE_NOMEM;
        char *e=spool.queue[spool.count++]; memset(e,0,DOS_PRINT_ENTRY);
        for(size_t i=0;i<n;i++) e[i]=upper(r->path[i]);
        return 0;
    }
    case DOS_PRINT_CANCEL: case DOS_PRINT_CANCEL_ALL: {
        int found=0;
        for(unsigned i=spool.count;i--;) {
            if(function==DOS_PRINT_CANCEL && !matches(r->path,spool.queue[i])) continue;
            found=1;
            if(i==0 && spool.file_open) {
                close_file(); spool.out_used=spool.out_next=0;
                if(function==DOS_PRINT_CANCEL) {
                    notice("\r\n\nFile "); notice(spool.queue[0]); notice(" canceled by operator"); notice_end();
                }
            }
            dequeue(i);
        }
        if(function==DOS_PRINT_CANCEL_ALL && found) {notice("\r\n\nAll files canceled by operator"); notice_end();}
        return found || function==DOS_PRINT_CANCEL_ALL?0:DE_NOFILE;
    }
    case DOS_PRINT_STATUS:
        spool.held=1; r->queue=spool.queue[0]; r->errors=spool.errors;
        strcopy(r->device,sizeof(r->device),spool.device); return 0;
    case DOS_PRINT_RELEASE: spool.held=0; return 0;
    default: return DE_FUNCTION;
    }
}
