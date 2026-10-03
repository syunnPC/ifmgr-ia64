/* SPDX-License-Identifier: GPL-2.0-or-later
 * Resident native character loopback driver and ABI exerciser.
 */
#include "../apps/runtime.h"
#include "loop_device.h"
typedef struct Cookie {struct Cookie *next; u64 serial;} Cookie;
static const IoServices *io;
static u8 *bytes;
static Cookie *cookies;
static unsigned head,used,opens,closes,reentry,last_pid;
static u64 last_cookie;
static int request(void *context,DosDeviceRequest *r) {
    (void)context;
    DosRegs probe={.ax=0x3000}; if(dos_call(&probe)==DE_BUSY) reentry++;
    last_pid=r->pid;
    if(r->command==DOS_DEV_INIT) {
        io=r->io;
        if(!io || !io->alloc_pages || !io->free_pages) return DE_FUNCTION;
        int e=io->alloc_pages(io->context,1,(void **)&bytes); if(e) return e;
        if(!strcmp(r->arguments,"/FAIL")) return DE_IO;
        used=strlen(r->arguments); memcpy(bytes,r->arguments,used); return 0;
    }
    if(r->command==DOS_DEV_FINISH) {
        while(cookies) {Cookie *next=cookies->next; io->free_pages(io->context,cookies,1); cookies=next;}
        if(bytes) {io->free_pages(io->context,bytes,1); bytes=NULL;}
        if(io) {const char *text="LOOPDRV.SYS: finished\n"; io->console_write(io->context,text,strlen(text));}
        return 0;
    }
    if(r->command==DOS_DEV_OPEN) {
        Cookie *cookie; int e=io->alloc_pages(io->context,1,(void **)&cookie); if(e) return e;
        cookie->serial=++opens; cookie->next=cookies; cookies=cookie; r->cookie=cookie; return 0;
    }
    if(r->command==DOS_DEV_CLOSE) {
        Cookie **p=&cookies; while(*p && *p!=r->cookie) p=&(*p)->next;
        if(!*p) return DE_HANDLE;
        Cookie *cookie=*p; *p=cookie->next; io->free_pages(io->context,cookie,1); closes++; return 0;
    }
    Cookie *cookie=cookies; while(cookie && cookie!=r->cookie) cookie=cookie->next;
    if(!cookie) return DE_HANDLE;
    last_cookie=cookie->serial;
    switch(r->command) {
    case DOS_DEV_READ:
        while(r->transferred<r->count && used) {
            ((u8 *)r->buffer)[r->transferred++]=bytes[head++]; head%=4096; used--;
        }
        return 0;
    case DOS_DEV_PEEK:
        if(!used) return DE_NOTREADY;
        *(u8 *)r->buffer=bytes[head]; return 0;
    case DOS_DEV_WRITE: case DOS_DEV_IOCTL_WRITE:
        while(r->transferred<r->count && used<4096) {
            bytes[(head+used++)%4096]=((u8 *)r->buffer)[r->transferred++];
        }
        return r->transferred==r->count?0:DE_FULL;
    case DOS_DEV_INPUT_STATUS: r->ready=used!=0; return 0;
    case DOS_DEV_OUTPUT_STATUS: r->ready=used<4096; return 0;
    case DOS_DEV_GENERIC_IOCTL:
        if(r->control!=0x8000 || r->count) return DE_FUNCTION;
        head=used=0; return 0;
    case DOS_DEV_INPUT_FLUSH: head=used=0; return 0;
    case DOS_DEV_OUTPUT_FLUSH: return 0;
    case DOS_DEV_IOCTL_READ: {
        if(r->count<sizeof(LoopStats)) return DE_FUNCTION;
        LoopStats stats={opens,closes,opens-closes,used,reentry,last_pid,last_cookie};
        memcpy(r->buffer,&stats,sizeof(stats)); r->transferred=sizeof(stats); return 0;
    }
    default: return DE_FUNCTION;
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    DosDeviceSpec spec={.version=DOS_DEVICE_ABI,.size=sizeof(spec),.name="LOOP",
        .attributes=DOS_DEVICE_CHAR|DOS_DEVICE_IOCTL|DOS_DEVICE_OPEN_CLOSE|DOS_DEVICE_GENERIC,
        .capabilities=DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE,.request=request};
    return dos_device_register(&spec)?EFI_LOAD_ERROR:EFI_SUCCESS;
}
