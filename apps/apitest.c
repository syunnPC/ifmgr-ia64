/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static int path_call(unsigned fn,const char *p) {
    DosRegs r={.ax=fn<<8,.dx=(uintptr_t)p}; return app_call(&r);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    DosRegs r={.ax=0x3000}; check("version-4.00",!app_call(&r)&&r.ax==4);
    check("mkdir",!path_call(0x39,"C:\\APIWORK"));
    if(failed) goto finish;
    r=(DosRegs){.ax=0x3c00,.dx=(uintptr_t)"C:\\APIWORK\\NATIVE.DAT"};
    int err=app_call(&r); unsigned h=r.ax; check("create",!err); if(err) goto cleanup;
    static u8 data[2500],readback[2500];
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(i*17+3)&255;
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=sizeof(data),.dx=(uintptr_t)data};
    check("write-across-clusters",!app_call(&r)&&r.ax==sizeof(data));
    r=(DosRegs){.ax=0x4200,.bx=h,.dx=0}; check("seek-start",!app_call(&r)&&!r.ax);
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=sizeof(readback),.dx=(uintptr_t)readback};
    check("read-binary-data",!app_call(&r)&&r.ax==sizeof(readback)&&!memcmp(data,readback,sizeof(data)));
    r=(DosRegs){.ax=0x4500,.bx=h}; err=app_call(&r); unsigned dup=r.ax; check("dup",!err);
    r=(DosRegs){.ax=0x4200,.bx=dup,.dx=7}; check("dup-shared-seek",!app_call(&r));
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=1,.dx=(uintptr_t)readback};
    check("dup-shared-offset",!app_call(&r)&&readback[0]==data[7]);
    r=(DosRegs){.ax=0x3e00,.bx=dup}; check("close-dup",!app_call(&r));
    r=(DosRegs){.ax=0x4200,.bx=h,.dx=517}; check("seek-truncate",!app_call(&r));
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=0}; check("zero-write-truncates",!app_call(&r));
    r=(DosRegs){.ax=0x4202,.bx=h,.dx=0}; check("size-after-truncate",!app_call(&r)&&r.ax==517);
    r=(DosRegs){.ax=0x4200,.bx=h,.dx=1500}; check("seek-beyond-eof",!app_call(&r));
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=1,.dx=(uintptr_t)data}; check("write-gap",!app_call(&r));
    r=(DosRegs){.ax=0x4200,.bx=h,.dx=517}; app_call(&r);
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=984,.dx=(uintptr_t)readback}; err=app_call(&r);
    int zero=!err && r.ax==984 && readback[983]==data[0];
    for(unsigned i=0;i<983;i++) if(readback[i]) zero=0;
    check("gap-zero-filled",zero);
    check("delete-open-denied",path_call(0x41,"C:\\APIWORK\\NATIVE.DAT")==DE_ACCESS);
    r=(DosRegs){.ax=0x6800,.bx=h}; check("commit",!app_call(&r));
    r=(DosRegs){.ax=0x3e00,.bx=h}; check("close",!app_call(&r));
    r=(DosRegs){.ax=0x3e00,.bx=h}; check("close-invalid-handle",app_call(&r)==DE_HANDLE);
    r=(DosRegs){.ax=0x5600,.dx=(uintptr_t)"C:\\APIWORK\\NATIVE.DAT",.di=(uintptr_t)"C:\\APIWORK\\RENAMED.DAT"};
    check("rename",!app_call(&r));
    DosFind dta;
    r=(DosRegs){.ax=0x1a00,.dx=(uintptr_t)&dta}; check("set-native-dta",!app_call(&r));
    r=(DosRegs){.ax=0x4e00,.dx=(uintptr_t)"C:\\APIWORK\\*.DAT"};
    check("find-first",!app_call(&r)&&dta.size==1501&&!strcmp(dta.name,"RENAMED.DAT"));
    r=(DosRegs){.ax=0x4f00}; check("find-next-end",app_call(&r)==DE_NOMORE);
    r=(DosRegs){.ax=0x4301,.cx=1,.dx=(uintptr_t)"C:\\APIWORK\\RENAMED.DAT"}; check("attribute-read-only",!app_call(&r));
    check("delete-readonly-denied",path_call(0x41,"C:\\APIWORK\\RENAMED.DAT")==DE_ACCESS);
    r=(DosRegs){.ax=0x4301,.cx=0,.dx=(uintptr_t)"C:\\APIWORK\\RENAMED.DAT"}; check("attribute-clear",!app_call(&r));
    check("delete",!path_call(0x41,"C:\\APIWORK\\RENAMED.DAT"));
    r=(DosRegs){.ax=0x4800,.bx=256}; err=app_call(&r); uintptr_t block=r.ax;
    check("allocate-native-memory",!err && block>0xffff);
    r=(DosRegs){.ax=0x4a00,.bx=512,.dx=block}; check("resize-memory",!app_call(&r));
    r=(DosRegs){.ax=0x4900,.dx=block}; check("free-memory",!app_call(&r));
    r=(DosRegs){.ax=0x4900,.dx=block}; check("double-free-rejected",app_call(&r)==DE_BLOCK);
    for(unsigned method=0;method<3;method++) {
        r=(DosRegs){.ax=0x5801,.bx=method}; check("set-allocation-strategy",!app_call(&r));
        r=(DosRegs){.ax=0x4800,.bx=19}; err=app_call(&r); block=r.ax;
        check("allocate-with-strategy",!err);
        if(!err) {r=(DosRegs){.ax=0x4900,.dx=block}; app_call(&r);}
    }
    r=(DosRegs){.ax=0x5801,.bx=0}; app_call(&r);
    r=(DosRegs){.ax=0x4800,.bx=0xffffffff}; check("oom-reports-largest",app_call(&r)==DE_NOMEM && r.bx>0);
    u64 largest=r.bx;
    r=(DosRegs){.ax=0x4b00,.dx=(uintptr_t)"C:\\EXIT37.EFI",.bx=(uintptr_t)""}; check("nested-exec",!app_call(&r));
    r=(DosRegs){.ax=0x4d00}; check("child-exit-code",!app_call(&r)&&r.ax==37);
    r=(DosRegs){.ax=0x4800,.bx=0xffffffff}; check("child-arena-reclaimed",app_call(&r)==DE_NOMEM&&r.bx==largest);
    r=(DosRegs){.ax=0xff00}; check("unsupported-api-rejected",app_call(&r)==DE_FUNCTION);
    r=(DosRegs){.ax=0x3600,.dx=3}; check("disk-free-space",!app_call(&r)&&r.cx==512&&r.bx>0);
cleanup:
    check("rmdir",!path_call(0x3a,"C:\\APIWORK"));
finish:
    print("APITEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
