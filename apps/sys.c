/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * SYS: native counterpart of MS-DOS 4 CMD/SYS. DOS 4 SYS copies IO.SYS and
 * MSDOS.SYS (not COMMAND.COM) and writes the boot record; on EFI the boot
 * record's role is played by \EFI\BOOT\BOOTIA64.EFI, which is copied too.
 */
#include "maint.h"
static const struct {const char *path; u8 attr;} names[]={
    {"\\EFI\\BOOT\\BOOTIA64.EFI",0},{"\\IO.SYS",FA_RDONLY|FA_HIDDEN|FA_SYSTEM},{"\\MSDOS.SYS",FA_RDONLY|FA_HIDDEN|FA_SYSTEM}};
static int path_on(unsigned drive,const char *base,const char *name,char out[DOS_PATH_MAX]) {
    out[0]=(char)('A'+drive); out[1]=':'; out[2]=0;
    if(base && *base) {
        int e=strcopy(out,DOS_PATH_MAX,base); if(e) return e;
        size_t n=strlen(out); if(n && out[n-1]=='\\') out[n-1]=0;
    }
    return strappend(out,DOS_PATH_MAX,name);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256],*cursor=tail,*words[3]; unsigned count=0;
    strcopy(tail,sizeof(tail),app_dos->command_tail());
    for(char *w;(w=next_word(&cursor));) {if(count==2) {print("Too many parameters\n"); return 1;} words[count++]=w;}
    if(!count) {print("Required parameter missing\n"); return 1;}
    unsigned target,source=dos_current_drive(); char base[DOS_PATH_MAX]="";
    if(drive_argument(words[count-1],&target)) {print("Invalid drive specification\n"); return 1;}
    if(refuse_mapped(target,"SYS")) return 1;
    if(count==2) {
        int e=dos_canonical(words[0],base); if(e) {print("Invalid path\n"); return 1;}
        source=(unsigned)(base[0]-'A');
    }
    if(target==source) {print("Can not specify default drive\n"); return 1;}
    Volume v; int e=volume_boot(target,&v);
    if(e==DE_DRIVE) {print("Invalid drive specification\n"); return 1;}
    if(e) {print("Not able to SYS to %c: file system\n",'A'+target); return 1;}
    u8 *data[ARRAY_SIZE(names)]={0}; u32 sizes[ARRAY_SIZE(names)]; u64 needed=0;
    for(unsigned i=0;i<ARRAY_SIZE(names) && !e;i++) {
        char path[DOS_PATH_MAX]; e=path_on(source,base,names[i].path,path);
        if(!e) e=load_file(path,&data[i],&sizes[i]);
        if(!e) needed+=sizes[i];
    }
    if(e) {print("No system on default drive\n"); goto out;}
    DosDriveInfo info; e=dos_drive_info(target,&info);
    if(e) {print("Not able to SYS to %c: file system\n",'A'+target); goto out;}
    /* Replaced files return their clusters; the EFI directories need two. */
    u64 unit=(u64)info.sectors_per_cluster*512,free=(u64)info.free_clusters*unit;
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) {
        char path[DOS_PATH_MAX]; DosFind find; path_on(target,NULL,names[i].path,path);
        if(!dos_find_first(path,FA_HIDDEN|FA_SYSTEM,&find)) free+=(find.size+unit-1)/unit*unit;
    }
    u64 want=2*unit;
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) want+=(sizes[i]+unit-1)/unit*unit;
    if(want>free) {print("No room for system on destination disk\n"); e=DE_FULL; goto out;}
    char dir[DOS_PATH_MAX]; path_on(target,NULL,"\\EFI",dir);
    int x=dos_mkdir(dir); if(x && x!=DE_ACCESS && x!=DE_EXISTS) e=x;
    if(!e) {strappend(dir,sizeof(dir),"\\BOOT"); x=dos_mkdir(dir); if(x && x!=DE_ACCESS && x!=DE_EXISTS) e=x;}
    for(unsigned i=0;i<ARRAY_SIZE(names) && !e;i++) {
        char path[DOS_PATH_MAX]; path_on(target,NULL,names[i].path,path);
        e=store_file(path,data[i],sizes[i],names[i].attr);
    }
    if(e) print("Write failure, diskette unusable\n");
    else print("System transferred\n");
out:
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) if(data[i]) dos_free(data[i]);
    return e?1:0;
}
