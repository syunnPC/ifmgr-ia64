/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed,calls,action;
static int reentry_rejected;
static DosCriticalError observed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static unsigned critical(void *context,const DosCriticalError *event) {
    if(context!=&calls) return DOS_CRITICAL_FAIL;
    calls++; observed=*event;
    DosRegs r={.ax=0x3000}; reentry_rejected=dos_call(&r)==DE_BUSY;
    return action;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS result=app_init(st); if(EFI_ERROR(result)) return result;
    const char *mode=app_dos->command_tail(); unsigned h;
    if(!strcmp(mode,"abort-child")) {
        void *memory;
        if(dos_alloc(1024,&memory) || dos_open("C:\\CRIT.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h) ||
           dos_lock(h,0,0,4)) return 1;
        dos_mkdir("C:\\CRITDIR");
        check("abort-must-not-return",0); return 1;
    }
    action=!strcmp(mode,"retry")?DOS_CRITICAL_RETRY:!strcmp(mode,"ignore-meta")?DOS_CRITICAL_IGNORE:
           !strcmp(mode,"abort")?DOS_CRITICAL_ABORT:DOS_CRITICAL_FAIL;
    check("critical-capability",app_dos->size>=sizeof(DosApi) && (app_dos->capabilities&DOS_CAP_CRITICAL));
    DosCriticalHandler handler={critical,&calls},previous;
    int e=dos_critical_handler(&handler,&previous); check("install-handler",!e); if(e) return 1;
    if(!strcmp(mode,"abort")) {
        DosInfo before,after; check("arena-before",!dos_query(&before));
        e=dos_exec("C:\\CRITTEST.EFI","abort-child");
        DosExitInfo status; check("child-critical-exit",!e && !dos_last_exit(&status) && status.kind==DOS_EXIT_CRITICAL);
        check("inherited-handler",calls==1 && observed.pid!=before.pid && reentry_rejected);
        check("arena-reclaimed",!dos_query(&after) && before.largest_paragraphs==after.largest_paragraphs);
        e=dos_open("C:\\CRIT.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h); check("handles-released",!e);
        if(!e) {check("locks-released",!dos_lock(h,0,0,4)); check("close-handle",!dos_close(h));}
        u8 attr; check("aborted-directory-absent",dos_attribute("C:\\CRITDIR",0,&attr)==DE_NOFILE);
    } else {
        e=dos_mkdir("C:\\CRITDIR");
        DosExtendedError error; int query=dos_extended_error(&error);
        check("write-error-callback",calls==1 && observed.operation==DOS_CRITICAL_WRITE && observed.drive==2 && observed.attempt==1);
        check("DOS-reentry-rejected",reentry_rejected);
        check("metadata-ignore-disallowed",!(observed.allowed&(1U<<DOS_CRITICAL_IGNORE)));
        if(action==DOS_CRITICAL_RETRY) {check("retry-succeeds",!e); check("remove-directory",!dos_remove("C:\\CRITDIR",1));}
        else {
            check("failure-status",e==DE_CRITICAL);
            check("original-cause",!query && error.error==DE_IO && error.error_class==DOS_CLASS_HARDWARE &&
                  error.operation==DOS_CRITICAL_WRITE && (error.flags&DOS_ERROR_COMMIT) && !(error.flags&DOS_ERROR_UNRECOVERED));
            DosRegs r={.ax=0x5900}; check("INT21-extended-error",!dos_call(&r) && r.ax==DE_IO && r.cx==(DOS_LOCUS_DISK<<8));
            u8 attr; check("failed-directory-absent",dos_attribute("C:\\CRITDIR",0,&attr)==DE_NOFILE);
        }
    }
    check("restore-handler",!dos_critical_handler(&previous,NULL));
    print("CRITTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
