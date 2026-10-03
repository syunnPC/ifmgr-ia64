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
    const char *path="C:\\FILETEST.DAT";
    char data[32]; u32 n,pos;
    if(!strcmp(app_dos->command_tail(),"child")) {
        DosRegs r={.ax=0x4400,.bx=6};
        check("child-excludes-private-handle",app_call(&r)==DE_HANDLE);
        check("child-inherited-seek",!dos_seek(5,0,0,&pos));
        check("child-cannot-read-parent-lock",dos_read(5,data,4,&n)==DE_LOCK && !n);
        check("child-cannot-unlock-parent",dos_lock(7,1,0,4)==DE_LOCK);
        check("child-lock-own-range",!dos_lock(5,0,8,2));
        return finish("FILETEST-CHILD");
    }
    unsigned a,b,result,private_handle,dup;
    int err=dos_open_ex(path,DOS_OPEN_RDWR|DOS_SHARE_DENY_NONE,0,0x10,&a,&result);
    check("extended-create",!err && a==5 && result==2);
    if(err || a!=5) return finish("FILETEST");
    check("write-shared-file",!dos_write(a,"0123456789abcdef",16,&n) && n==16);
    err=dos_open("NUL",DOS_OPEN_RDWR|DOS_OPEN_PRIVATE,0,&private_handle);
    check("open-private-handle",!err && private_handle==6);
    if(err || private_handle!=6) return finish("FILETEST");
    err=dos_dup(a,&dup); check("duplicate-handle",!err && dup==7);
    if(err || dup!=7) return finish("FILETEST");
    check("lock-parent-range",!dos_lock(a,0,0,4));
    check("duplicate-reads-own-lock",!dos_seek(dup,0,0,&pos) && !dos_read(dup,data,4,&n) && n==4 && !memcmp(data,"0123",4));
    err=dos_exec("C:\\FILETEST.EFI","child"); check("file-child-exec",!err && !dos_get_errorlevel());
    check("child-exit-releases-lock",!dos_lock(a,0,8,2));
    check("unlock-child-range",!dos_lock(a,1,8,2));
    err=dos_open_ex(path,DOS_OPEN_RDWR|DOS_SHARE_DENY_NONE,0,1,&b,&result);
    check("extended-open-existing",!err && result==1);
    if(err) return finish("FILETEST");
    check("separate-open-cannot-read-lock",dos_read(b,data,4,&n)==DE_LOCK && !n);
    check("independent-seek-unchanged-on-error",!dos_seek(b,0,1,&pos) && !pos);
    check("overlapping-lock-rejected",dos_lock(b,0,3,2)==DE_LOCK);
    check("wrong-unlock-rejected",dos_lock(a,1,0,3)==DE_LOCK);
    check("close-one-duplicate",!dos_close(dup));
    check("remaining-duplicate-keeps-lock",dos_read(b,data,4,&n)==DE_LOCK);
    check("unlock-parent-range",!dos_lock(a,1,0,4));
    check("read-after-unlock",!dos_read(b,data,4,&n) && n==4 && !memcmp(data,"0123",4));
    check("seek-append",!dos_seek(a,0,2,&pos) && pos==16);
    check("append",!dos_write(a,"more",4,&n) && n==4);
    check("other-open-sees-growth",!dos_seek(b,0,2,&pos) && pos==20);
    check("truncate",!dos_seek(b,3,0,&pos) && !dos_write(b,NULL,0,&n));
    check("other-open-sees-truncation",!dos_seek(a,0,2,&pos) && pos==3);
    u16 date=0x5d3d,time=0x4567,d,t;
    check("set-file-date-time",!dos_file_time(a,1,&date,&time));
    check("shared-file-date-time",!dos_file_time(b,0,&d,&t) && d==date && t==time);
    check("close-shared-handles",!dos_close(a) && !dos_close(b) && !dos_close(private_handle));
    err=dos_open(path,DOS_SHARE_DENY_WRITE,0,&a); check("open-deny-write",!err);
    if(err) return finish("FILETEST");
    check("deny-conflicting-writer",dos_open(path,DOS_OPEN_WRITE|DOS_SHARE_DENY_NONE,0,&b)==DE_SHARE);
    err=dos_open(path,DOS_SHARE_DENY_NONE,0,&b); check("allow-second-reader",!err);
    if(err) return finish("FILETEST");
    check("reopened-date-time",!dos_file_time(b,0,&d,&t) && d==date && t==time);
    check("close-readers",!dos_close(a) && !dos_close(b));
    err=dos_open_ex(path,DOS_OPEN_RDWR|DOS_SHARE_DENY_NONE,FA_HIDDEN,2,&a,&result);
    check("extended-replace",!err && result==3);
    if(err) return finish("FILETEST");
    check("replace-is-empty",!dos_seek(a,0,2,&pos) && !pos);
    check("close-replacement",!dos_close(a));
    u8 attr=0; check("replace-attributes",!dos_attribute(path,0,&attr) && attr==(FA_HIDDEN|FA_ARCHIVE));
    char canonical[DOS_PATH_MAX];
    check("canonical-path",!dos_canonical("c:\\test\\..\\filetest.dat",canonical) && !strcmp(canonical,path));
    check("delete-test-file",!dos_remove(path,0));
    return finish("FILETEST");
}
