/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static int handle_info(unsigned h) {
    DosRegs r={.ax=0x4400,.bx=h}; return app_call(&r);
}
static EFI_STATUS finish(const char *label) {
    print("%s: %u passed, %u failed\n",label,(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    if(app_dos->size<sizeof(*app_dos)) return EFI_UNSUPPORTED;
    char value[DOS_ENV_CAPACITY];
    const char *tail=app_dos->command_tail();
    if(!strcmp(tail,"boot-config")) {
        check("configured-shell-environment",!dos_env_get("COMSPEC",value,sizeof(value)) && !strcmp(value,"C:\\ENVTEST.EFI"));
        unsigned handles[3],h;
        for(unsigned i=0;i<3;i++) {
            int err=dos_open("NUL",0,0,&handles[i]); check("configured-files-slot",!err);
            if(err) return finish("BOOT-CONFIG");
        }
        check("configured-files-limit",dos_open("NUL",0,0,&h)==DE_HANDLES);
        for(unsigned i=0;i<3;i++) check("configured-files-close",!dos_close(handles[i]));
        return finish("BOOT-CONFIG");
    }
    if(!strcmp(tail,"child")) {
        check("child-inherited-value",!dos_env_get("inherit_test",value,sizeof(value)) && !strcmp(value,"Parent Value"));
        check("child-inherited-low-handle",!handle_info(5));
        check("child-excludes-high-handle",handle_info(25)==DE_HANDLE);
        check("child-environment-change",!dos_env_set("INHERIT_TEST","Child Value"));
        check("child-environment-delete",!dos_env_set("COMSPEC",NULL));
        return finish("ENVTEST-CHILD");
    }
    check("environment-set",!dos_env_set("Inherit_Test","Parent Value"));
    check("environment-case-insensitive",!dos_env_get("inherit_TEST",value,sizeof(value)) && !strcmp(value,"Parent Value"));
    strcopy(value,sizeof(value),"untouched");
    check("environment-small-buffer",dos_env_get("INHERIT_TEST",value,2)==DE_ENV && !strcmp(value,"untouched"));
    check("environment-invalid-name",dos_env_set("BAD=NAME","value")==DE_ENV);
    int found=0;
    for(u32 i=0;!dos_env_list(i,value,sizeof(value));i++) if(!strcmp(value,"INHERIT_TEST=Parent Value")) found++;
    check("environment-enumeration",found==1);
    char comspec[DOS_PATH_MAX]; int have_comspec=!dos_env_get("COMSPEC",comspec,sizeof(comspec));
    check("comspec-present",have_comspec);
    unsigned h; int err=dos_open("NUL",2,0,&h); check("parent-low-handle",!err && h==5);
    if(err || h!=5) return finish("ENVTEST");
    check("extend-handle-table",!dos_set_handle_count(40));
    check("duplicate-high-handle",!dos_dup2(h,25));
    check("shrink-open-handles-denied",dos_set_handle_count(20)==DE_HANDLES);
    err=dos_exec("C:\\ENVTEST.EFI","child");
    check("environment-exec",!err && !dos_get_errorlevel());
    check("child-preserves-parent-environment",!dos_env_get("INHERIT_TEST",value,sizeof(value)) && !strcmp(value,"Parent Value"));
    check("child-preserves-parent-comspec",have_comspec && !dos_env_get("COMSPEC",value,sizeof(value)) && !strcmp(value,comspec));
    check("parent-keeps-high-handle",!handle_info(25));
    check("close-high-handle",!dos_close(25)); check("close-low-handle",!dos_close(h));
    check("shrink-handle-table",!dos_set_handle_count(20));
    check("high-handle-after-shrink",handle_info(25)==DE_HANDLE);
    check("environment-delete",!dos_env_set("INHERIT_TEST",""));
    check("environment-absent",dos_env_get("INHERIT_TEST",value,sizeof(value))==DE_NOFILE);
    return finish("ENVTEST");
}
