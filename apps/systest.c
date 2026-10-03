/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    check("extended-dos-table",app_dos->size>=sizeof(*app_dos));
    if(failed) goto finish;
    const IoServices *io=app_dos->io;
    check("io-table",io && io->version==IO_ABI_VERSION && io->size>=sizeof(*io));
    if(failed) goto finish;
    void *page=NULL;
    int err=io->alloc_pages(io->context,1,&page);
    check("io-page-allocation",!err && page && !((uintptr_t)page&4095));
    if(!err && page) {memset(page,0xa5,4096); io->free_pages(io->context,page,1);}
    IoEvent event; err=io->poll_event(io->context,&event);
    check("nonblocking-input",!err || err==DE_NOTREADY);
    if(io->capabilities&IO_CAP_TIMER) {
        u64 before=io->ticks_ms(io->context); int waited=0;
        for(unsigned i=0;i<8 && io->ticks_ms(io->context)==before;i++) {
            io->poll_event(io->context,&event); waited=io->wait(io->context,20); if(waited) break;
        }
        check("timer-and-bounded-wait",!waited && io->ticks_ms(io->context)>before);
    } else {con_puts("[SKIP] firmware timer unavailable\n");}
    IoDisplay mode; err=io->display_info(io->context,&mode);
    if(io->capabilities&IO_CAP_GRAPHICS) {
        check("display-dimensions",!err && mode.width>=2 && mode.height>=2);
        if(!err) {
            IoPixel saved[4],pixels[4]={{0x17,0x35,0x53,0},{0x29,0x47,0x65,0},
                                      {0x41,0x63,0x85,0},{0x73,0x91,0xb3,0}},readback[4];
            err=io->display_blt(io->context,saved,0,0,2,2,2,1);
            check("display-read",!err);
            if(!err) {
                err=io->display_blt(io->context,pixels,0,0,2,2,2,0);
                if(!err) err=io->display_blt(io->context,readback,0,0,2,2,2,1);
                for(unsigned i=0;i<4;i++) readback[i].reserved=0;
                check("display-roundtrip",!err && !memcmp(pixels,readback,sizeof(pixels)));
                check("display-restore",!io->display_blt(io->context,saved,0,0,2,2,2,0));
                check("display-bounds",io->display_blt(io->context,pixels,mode.width,0,2,2,2,0)==DE_FUNCTION);
            }
        }
    } else check("headless-display-unavailable",err==DE_FUNCTION);
    if(!stricmp(app_dos->command_tail(),"key")) {
        con_puts("I/O key test: press K\n"); int found=0;
        for(unsigned i=0;i<500 && !found;i++) {
            err=io->poll_event(io->context,&event);
            if(!err && event.type==IO_EVENT_KEY && event.unicode=='K') found=1;
            if(!found && io->wait(io->context,10)) break;
        }
        check("io-key-event",found);
    }
    DosInfo before,child_info,after;
    check("dos-query",!dos_query(&before) && !before.in_dos);
    u32 child=0; err=app_dos->task_create(&child);
    check("create-dos-context",!err && child!=before.pid);
    if(!err) {
        check("select-dos-context",!app_dos->task_select(child));
        check("context-identity",!dos_query(&child_info) && child_info.pid==child);
        dos_set_errorlevel(73);
        void *block=NULL; check("context-memory",!dos_alloc(127,&block));
        check("restore-dos-context",!app_dos->task_select(before.pid));
        check("errorlevel-isolation",!dos_query(&after) && after.errorlevel==before.errorlevel);
        if(block) check("memory-ownership",dos_free(block)==DE_BLOCK);
        check("destroy-dos-context",!app_dos->task_destroy(child));
        check("context-memory-reclaimed",!dos_query(&after) && after.largest_paragraphs==before.largest_paragraphs);
        check("stale-context-rejected",app_dos->task_select(child)==DE_BLOCK);
    }
finish:
    print("SYSTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
