/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "loop_device.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;}
static int stats(unsigned h,LoopStats *out) {
    DosRegs r={.ax=0x4402,.bx=h,.cx=sizeof(*out),.dx=(uintptr_t)out}; return dos_call(&r);
}
static int clear(unsigned h) {DosRegs r={.ax=0x440c,.bx=h,.cx=0x8000}; return dos_call(&r);}
static int find_device(const char *name,DosDeviceInfo *info) {
    for(unsigned i=0;;i++) {
        int e=dos_device_info(i,info); if(e) return e;
        if(!strcmp(info->name,name)) return 0;
    }
}
static EFI_STATUS finish(void) {print("DEVTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed); return failed?1:0;}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *tail=app_dos->command_tail(); DosDeviceInfo info; unsigned h,duplicate; u32 n;
    check("device-capability",app_dos->size>=sizeof(DosApi) && (app_dos->capabilities&DOS_CAP_DEVICES));
    if(!strcmp(tail,"absent")) {
        check("failed-driver-not-published",find_device("LOOP",&info)==DE_NOMORE);
        check("failed-name-not-visible",dos_open("LOOP",2,0,&h)==DE_NOFILE); return finish();
    }
    if(!strcmp(tail,"abort-child")) {
        if(dos_open("LOOP",2,0,&h)) return 1;
        con_puts("DEVTEST: abort-ready\n"); DosRegs r={.ax=0x0800}; dos_call(&r);
        check("abort-must-not-return",0); return finish();
    }
    if(!strcmp(tail,"abort")) {
        int e=dos_open("LOOP",2,0,&h); check("parent-open",!e); if(e) return finish();
        LoopStats before,after; check("parent-stats",!stats(h,&before));
        e=dos_exec("C:\\DEVTEST.EFI","abort-child"); DosExitInfo exit;
        check("child-aborted",!e && !dos_last_exit(&exit) && exit.kind==DOS_EXIT_BREAK);
        check("aborted-cookie-released",!stats(h,&after) && after.live==before.live && after.opens==before.opens+1 && after.closes==before.closes+1);
        check("parent-close",!dos_close(h)); return finish();
    }
    if(!memcmp(tail,"child ",6)) {
        h=0; for(const char *p=tail+6;*p;p++) h=h*10+*p-'0';
        check("inherited-driver-handle",!dos_write(h,"child",5,&n) && n==5);
        check("independent-child-open",!dos_open("LOOP",2,0,&h));
        /* Leave this open: EXEC cleanup must issue CLOSE and free the cookie. */
        return finish();
    }
    check("console-enumeration",!dos_device_info(0,&info) && !strcmp(info.name,"CON"));
    check("null-enumeration",!dos_device_info(1,&info) && !strcmp(info.name,"NUL"));
    check("resident-driver-enumeration",!find_device("LOOP",&info));
    check("registration-is-boot-only",dos_device_register(NULL)==DE_ACCESS);
    int e=dos_open("c:\\loop.txt",2,0,&h); check("device-name-alias",!e); if(e) return finish();
    check("generic-control",!clear(h));
    DosRegs r={.ax=0x4400,.bx=h}; check("device-flags",!dos_call(&r) && (r.dx&0xc080)==0xc080);
    r=(DosRegs){.ax=0x4406,.bx=h}; check("empty-input-status",!dos_call(&r) && !r.ax);
    LoopStats before,after; check("control-read",!stats(h,&before) && before.live==1 && before.reentry);
    check("duplicate",!dos_dup(h,&duplicate));
    check("duplicate-does-not-open-again",!stats(h,&after) && after.opens==before.opens && after.live==1);
    check("write-driver",!dos_write(h,"ABC",3,&n) && n==3);
    r=(DosRegs){.ax=0x4406,.bx=h}; check("ready-input-status",!dos_call(&r) && r.ax==255);
    unsigned saved; check("save-stdin",!dos_dup(0,&saved)); check("redirect-stdin",!dos_dup2(h,0));
    r=(DosRegs){.ax=0x0b00}; check("nondestructive-status",!dos_call(&r) && r.ax==255);
    r=(DosRegs){.ax=0x0700}; check("redirected-character",!dos_call(&r) && r.ax=='A');
    check("restore-stdin",!dos_dup2(saved,0) && !dos_close(saved));
    char out[32]; check("duplicate-shares-stream",!dos_read(duplicate,out,2,&n) && n==2 && !memcmp(out,"BC",2));
    check("close-duplicate",!dos_close(duplicate));
    check("cookie-retained",!stats(h,&after) && after.live==1 && after.closes==before.closes && after.cookie==before.cookie);
    check("cooked-source",!dos_write(h,"ab\r",3,&n));
    r=(DosRegs){.ax=0x4401,.bx=h,.dx=0xc0}; check("cooked-mode",!dos_call(&r));
    check("cooked-line",!dos_read(h,out,sizeof(out),&n) && n==4 && !memcmp(out,"ab\r\n",4));
    r=(DosRegs){.ax=0x4401,.bx=h,.dx=0xe0}; check("raw-mode",!dos_call(&r));
    u32 child; DosInfo own; check("own-context",!dos_query(&own));
    check("context-create",!app_dos->task_create(&child));
    check("context-select",!app_dos->task_select(child));
    unsigned local; check("context-open",!dos_open("LOOP",2,0,&local));
    check("parent-select",!app_dos->task_select(own.pid));
    check("context-destroy",!app_dos->task_destroy(child));
    check("context-cookie-released",!stats(h,&after) && after.live==1 && after.opens==before.opens+1 && after.closes==before.closes+1);
    char argument[32]="child "; unsigned len=6,value=h; char digits[10]; unsigned count=0;
    do {digits[count++]='0'+value%10; value/=10;} while(value);
    while(count) argument[len++]=digits[--count];
    argument[len]=0;
    check("EXEC-driver-inheritance",!dos_exec("C:\\DEVTEST.EFI",argument));
    check("EXEC-cookie-released",!stats(h,&after) && after.live==1 && after.opens==before.opens+2 && after.closes==before.closes+2);
    check("child-data",!dos_read(h,out,sizeof(out),&n) && n==5 && !memcmp(out,"child",5));
    u8 full[4096]; memset(full,'X',sizeof(full));
    check("near-full-write",!dos_write(h,full,4090,&n) && n==4090);
    check("partial-device-write",dos_write(h,full,16,&n)==DE_FULL && n==6);
    r=(DosRegs){.ax=0x4407,.bx=h}; check("busy-output-status",!dos_call(&r) && !r.ax);
    check("clear-full-queue",!clear(h));
    r=(DosRegs){.ax=0x4403,.bx=h,.cx=3,.dx=(uintptr_t)"CTL"}; check("control-write",!dos_call(&r) && r.ax==3);
    check("control-data",!dos_read(h,out,sizeof(out),&n) && n==3 && !memcmp(out,"CTL",3));
    check("driver-close",!dos_close(h));
    check("no-open-descriptions",!find_device("LOOP",&info) && !info.open_descriptions);
    check("resident-image-not-executable",dos_exec("C:\\LOOPDRV.SYS","")==DE_FORMAT);
    return finish();
}
