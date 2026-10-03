/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "ramdisk.h"
static unsigned passed,failed;
static u8 data[6000],copy[6000];
static DosCriticalHandler previous;
static void check(const char *name,int ok) {print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;}
static EFI_STATUS finish(void) {
    dos_critical_handler(&previous,NULL);
    print("RAMTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
static void path(char out[DOS_PATH_MAX],unsigned drive,const char *leaf) {
    out[0]='A'+drive; out[1]=':'; out[2]='\\'; out[3]=0; strappend(out,DOS_PATH_MAX,leaf);
}
static int store_file(const char *name,const void *bytes,u32 count) {
    unsigned h; int e=dos_open(name,2,1,&h); if(e) return e;
    u32 n; e=dos_write(h,bytes,count,&n); if(!e && n!=count) e=DE_FULL;
    int close=dos_close(h); return e?e:close;
}
static int copy_file(const char *source,const char *destination) {
    unsigned a,b; int e=dos_open(source,0,0,&a); if(e) return e;
    e=dos_open(destination,2,1,&b); if(e) {dos_close(a); return e;}
    u8 buffer[1024]; u32 n,w;
    while(!(e=dos_read(a,buffer,sizeof(buffer),&n)) && n) {
        e=dos_write(b,buffer,n,&w); if(!e && w!=n) e=DE_FULL; if(e) break;
    }
    int close=dos_close(a); if(!e) e=close; close=dos_close(b); return e?e:close;
}
static int control(unsigned h,u32 command,unsigned unit,u32 *value) {
    DosRegs r={.ax=0x440c,.bx=h,.cx=command,.di=unit,.dx=(uintptr_t)value,.si=value?4:0}; return dos_call(&r);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    DosCriticalHandler none={0}; dos_critical_handler(&none,&previous);
    const char *tail=app_dos->command_tail(); DosBlockInfo block;
    int capable=app_dos->size>=offsetof(DosApi,block_info)+sizeof(app_dos->block_info) &&
        (app_dos->capabilities&DOS_CAP_BLOCK_DRIVERS);
    check("block-driver-capability",capable); if(!capable) return finish();
    if(!strcmp(tail,"absent")) {
        check("failed-block-not-published",dos_block_info(0,&block)==DE_NOMORE);
        unsigned h; check("failed-control-not-published",dos_open("RAMCTL",2,0,&h)==DE_NOFILE);
        return finish();
    }
    int e=dos_block_info(0,&block); check("RAM-provider",!e && !strcmp(block.name,"RAMDISK") && block.units==2);
    if(e || block.units!=2) return finish();
    unsigned drives[2],count=0;
    for(unsigned i=0;i<DOS_DRIVES;i++) if(block.drive_mask&(1U<<i)) {if(count<2) drives[count]=i; count++;}
    check("two-assigned-drives",count==2 && drives[0]>=3 && drives[1]>drives[0]); if(count!=2) return finish();
    print("RAMTEST drives: %c: %c:\n",'A'+drives[0],'A'+drives[1]);
    check("registration-is-boot-only",dos_block_register(NULL)==DE_ACCESS);
    DosDriveInfo info;
    check("first-FAT-volume",!dos_drive_info(drives[0],&info) && info.fat_bits==(!strcmp(tail,"16")?16U:12U));
    check("second-FAT-volume",!dos_drive_info(drives[1],&info) && info.free_clusters==info.total_clusters);
    unsigned ctl=0,h=0; e=dos_open("RAMCTL",2,0,&ctl); check("open-RAM-control",!e); if(e) return finish();
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(u8)(i*17+3);
    char a[DOS_PATH_MAX],b[DOS_PATH_MAX],cwd[DOS_PATH_MAX]; u32 n,pos;
    path(a,drives[0],"WORK"); check("RAM-directory",!dos_mkdir(a));
    path(a,drives[0],"WORK\\DATA.BIN"); check("RAM-write",!store_file(a,data,sizeof(data)));
    e=dos_open(a,2,0,&h); check("RAM-open",!e); if(e) return finish();
    check("RAM-read",!dos_read(h,copy,sizeof(copy),&n) && n==sizeof(copy) && !memcmp(data,copy,n));
    check("RAM-seek",!dos_seek(h,512,0,&pos) && pos==512);
    check("RAM-subrange",!dos_read(h,copy,1025,&n) && n==1025 && !memcmp(copy,data+512,n));
    path(b,drives[1],"KEEP.BIN"); check("cross-unit-copy",!copy_file(a,b));
    check("copy-to-boot-volume",!copy_file(b,"C:\\RAMBACK.BIN"));
    path(a,drives[0],"EXIT37.EFI"); check("native-image-on-RAM",!copy_file("C:\\EXIT37.EFI",a));
    DosExitInfo exit; e=dos_exec(a,""); check("EXEC-from-RAM",!e && !dos_last_exit(&exit) && exit.code==37);
    char batch[]="@echo off\necho RAM-BATCH > X:\\BATCH.TXT\n"; *strchr(batch,'X')='A'+drives[1];
    path(a,drives[0],"RUN.BAT"); check("RAM-batch-file",!store_file(a,batch,sizeof(batch)-1));
    char command[160]="/C "; strappend(command,sizeof(command),a);
    check("RAM-batch-run",!dos_exec("C:\\COMMAND.COM",command));
    path(a,drives[1],"BATCH.TXT"); unsigned read;
    e=dos_open(a,0,0,&read); check("RAM-batch-result",!e);
    if(!e) {check("RAM-batch-data",!dos_read(read,copy,sizeof(copy),&n) && n==10 && !memcmp(copy,"RAM-BATCH\n",10)); dos_close(read);}
    path(a,drives[0],"WORK\\*.*"); DosFind find; check("RAM-search",!dos_find_first(a,0,&find));
    DosInfo own; u32 child=0; check("parent-context",!dos_query(&own));
    check("create-context",!app_dos->task_create(&child)); check("select-context",!app_dos->task_select(child));
    path(a,drives[0],"WORK"); check("context-directory",!dos_chdir(a));
    check("restore-parent",!app_dos->task_select(own.pid));
    check("RAM-media-reset",!control(ctl,RAMDISK_RESET,0,NULL));
    check("old-handle-invalidated",dos_read(h,copy,1,&n)==DE_CHANGED && !n);
    check("old-search-invalidated",dos_find_next(&find)==DE_CHANGED);
    check("select-reset-context",!app_dos->task_select(child));
    check("reset-context-directory",!dos_drive_cwd(drives[0],cwd) && !strcmp(cwd,"\\"));
    check("reap-context",!app_dos->task_select(own.pid) && !app_dos->task_destroy(child));
    check("close-stale-handle",dos_close(h)==DE_CHANGED && dos_close(h)==DE_HANDLE);
    path(a,drives[0],"EXIT37.EFI"); check("reset-cleared-files",dos_open(a,0,0,&h)==DE_NOFILE);
    path(a,drives[1],"KEEP.BIN"); e=dos_open(a,0,0,&h); check("other-unit-retained",!e);
    if(!e) {check("other-unit-data",!dos_read(h,copy,sizeof(copy),&n) && n==sizeof(copy) && !memcmp(copy,data,n)); dos_close(h);}
    u32 protect=1; check("protect-unit",!control(ctl,RAMDISK_PROTECT,1,&protect));
    check("read-only-volume",!dos_drive_info(drives[1],&info) && (info.flags&DOS_DRIVE_READONLY));
    path(a,drives[1],"NEW.TXT"); check("reject-protected-write",dos_open(a,2,1,&h)==DE_READONLY);
    check("reject-protected-reset",control(ctl,RAMDISK_RESET,1,NULL)==DE_READONLY);
    protect=0; check("unprotect-unit",!control(ctl,RAMDISK_PROTECT,1,&protect));
    check("write-after-unprotect",!store_file(a,"OK",2));
    check("close-control",!dos_close(ctl)); check("flush-volumes",!dos_flush());
    return finish();
}
