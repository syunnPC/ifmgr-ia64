/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static EFI_STATUS finish(const char *label) {
    print("%s: %u passed, %u failed\n",label,(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    char path[DOS_PATH_MAX],data[8]; u32 n,pos;
    if(!strcmp(app_dos->command_tail(),"child")) {
        check("child-drive",dos_current_drive()==3);
        check("child-D-directory",!dos_drive_cwd(3,path) && !strcmp(path,"\\DRVCHECK"));
        check("child-C-directory",!dos_drive_cwd(2,path) && !strcmp(path,"\\DRVCHECK"));
        check("child-inherited-C-handle",!dos_read(5,data,1,&n) && n==1 && data[0]=='C');
        check("child-changes-C-directory",!dos_chdir("C:\\"));
        check("child-changes-D-directory",!dos_chdir("D:\\"));
        check("child-changes-drive",!dos_select_drive(2));
        return finish("DRIVETEST-CHILD");
    }
    DosDriveInfo c,d; unsigned hc,hd;
    int err=dos_drive_info(2,&c); check("boot-drive-info",!err && (c.flags&DOS_DRIVE_PRESENT));
    err=dos_drive_info(3,&d); check("second-drive-info",!err && (d.flags&DOS_DRIVE_PRESENT));
    if(err) return finish("DRIVETEST");
    check("drive-capability",app_dos->capabilities&DOS_CAP_DRIVES);
    check("create-C-directory",!dos_mkdir("C:\\DRVCHECK"));
    check("create-D-directory",!dos_mkdir("D:\\DRVCHECK"));
    check("C-directory",!dos_chdir("C:\\DRVCHECK"));
    check("D-directory-keeps-default-drive",!dos_chdir("D:\\DRVCHECK") && dos_current_drive()==2);
    check("per-drive-relative-path",!dos_canonical("D:FILE.TXT",path) && !strcmp(path,"D:\\DRVCHECK\\FILE.TXT"));
    err=dos_open("C:FILE.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&hc); check("open-C-file",!err && hc==5);
    if(err) return finish("DRIVETEST");
    err=dos_open("D:FILE.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&hd); check("open-D-file",!err);
    if(err) return finish("DRIVETEST");
    check("write-C-file",!dos_write(hc,"C",1,&n) && n==1);
    check("write-D-file",!dos_write(hd,"D",1,&n) && n==1);
    check("C-lock",!dos_lock(hc,0,0,1)); check("independent-D-lock",!dos_lock(hd,0,0,1));
    check("D-content",!dos_seek(hd,0,0,&pos) && !dos_read(hd,data,1,&n) && n==1 && data[0]=='D');
    DosRegs r={.ax=0x4400,.bx=hd}; check("handle-drive-info",!dos_call(&r) && r.dx==3);
    check("cross-drive-rename",dos_rename("C:FILE.TXT","D:OTHER.TXT")==DE_NOTSAME);
    check("C-unlock",!dos_lock(hc,1,0,1)); check("D-close",!dos_close(hd));
    check("second-D-file",!dos_open("D:MORE.TXT",2,2,&hd)); check("close-second-D-file",!dos_close(hd));
    DosFind find;
    check("D-search",!dos_find_first("D:*.TXT",0,&find) && !strcmp(find.name,"FILE.TXT"));
    check("select-D",!dos_select_drive(3)); check("select-C",!dos_select_drive(2));
    check("continue-D-search",!dos_find_next(&find) && !strcmp(find.name,"MORE.TXT"));
    u32 child,parent=0; DosInfo info;
    check("query-parent",!dos_query(&info)); parent=info.pid;
    err=app_dos->task_create(&child); check("create-task",!err);
    if(!err) {
        check("select-task",!app_dos->task_select(child));
        check("task-selects-D",!dos_select_drive(3)); check("task-changes-C-cwd",!dos_chdir("C:\\"));
        check("select-parent",!app_dos->task_select(parent));
        check("parent-drive-isolated",dos_current_drive()==2);
        check("parent-cwd-isolated",!dos_drive_cwd(2,path) && !strcmp(path,"\\DRVCHECK"));
        check("destroy-task",!app_dos->task_destroy(child));
    }
    check("seek-inherited-handle",!dos_seek(hc,0,0,&pos)); check("EXEC-from-D",!dos_select_drive(3));
    check("EXEC-D-image",!dos_exec("D:\\DRVTEST.EFI","child") && !dos_get_errorlevel());
    check("EXEC-drive-side-effect",dos_current_drive()==2);
    check("EXEC-C-cwd-side-effect",!dos_drive_cwd(2,path) && !strcmp(path,"\\"));
    check("EXEC-D-cwd-side-effect",!dos_drive_cwd(3,path) && !strcmp(path,"\\"));
    check("inherited-position",!dos_seek(hc,0,1,&pos) && pos==1);
    check("close-C",!dos_close(hc));
    check("remove-C-file",!dos_remove("C:\\DRVCHECK\\FILE.TXT",0));
    check("remove-D-file",!dos_remove("D:\\DRVCHECK\\FILE.TXT",0));
    check("remove-D-search-file",!dos_remove("D:\\DRVCHECK\\MORE.TXT",0));
    check("remove-C-directory",!dos_remove("C:\\DRVCHECK",1));
    check("remove-D-directory",!dos_remove("D:\\DRVCHECK",1));
    return finish("DRIVETEST");
}
