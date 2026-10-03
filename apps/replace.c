/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * REPLACE: MS-DOS 4 CMD/REPLACE counterpart.
 *   REPLACE [d:][path]filename [d:][path] [/A] [/P] [/R] [/S] [/U] [/W]
 *
 * Default target is the current directory. /S searches below it; /A adds
 * missing files and conflicts with /S and /U. /P confirms; /R allows
 * read-only replacements; /U selects older targets; /W waits for a key.
 * Messages: v4.0 REPLACE.SKL. Exit: 2=no source, 11=parameter error.
 */
#include "util.h"
static int add,prompt,read_only,descend,update,wait_first; static u32 count;
static char source_dir[DOS_PATH_MAX],source_name[13];
static char key(void) {
    DosRegs r={.ax=0x0c08};
    if(dos_call(&r)) return 0;
    if(!(r.ax&0xff)) {r=(DosRegs){.ax=0x0100}; dos_call(&r); return 0;}
    return (char)(r.ax&0xff);
}
static int report(int e,const char *name) {
    extended_error(e==DE_PATH?DE_PATH:e==DE_ACCESS || e==DE_READONLY?DE_ACCESS:e==DE_DRIVE?DE_DRIVE:DE_NOFILE,name);
    return e;
}
/* One source file to one target path, after asking with /P. */
static int put(const DosFind *f,const char *target,const DosFind *existing) {
    char from[DOS_PATH_MAX],a[DOS_PATH_MAX],b[DOS_PATH_MAX];
    if(strcopy(from,sizeof(from),source_dir) || strappend(from,sizeof(from),f->name)) return DE_PATH;
    if(!dos_canonical(from,a) && !dos_canonical(target,b) && !stricmp(a,b)) {
        to_stderr(1); print("File cannot be copied onto itself\n"); to_stderr(0); return 0;
    }
    if(update && existing && (existing->date>f->date || (existing->date==f->date && existing->time>=f->time))) return 0;
    while(prompt) {
        print(add?"\nAdd %s? (Y/N)":"\nReplace %s? (Y/N)",target);
        char c=(char)upper(key()); print("%c\n",c?c:' ');
        if(c=='N') return 0;
        if(c=='Y') break;
    }
    print(add?"\nAdding %s\n":"\nReplacing %s\n",target);
    if(existing && (existing->attr&FA_RDONLY)) {
        u8 attr=(u8)(existing->attr&~FA_RDONLY);
        if(!read_only) return report(DE_ACCESS,target);
        int e=dos_attribute(target,1,&attr); if(e) return report(e,target);
    }
    int e=copy_file(from,target);
    if(e==DE_FULL) {to_stderr(1); print("Insufficient disk space\n"); to_stderr(0); return e;}
    if(e) return report(e,target);
    count++;
    return 0;
}
/* Every source file into one target directory. */
static int into(const char *dir) {
    char pattern[DOS_PATH_MAX],target[DOS_PATH_MAX]; DosFind f,there; int e;
    if(strcopy(pattern,sizeof(pattern),source_dir) || strappend(pattern,sizeof(pattern),source_name)) return DE_PATH;
    for(e=dos_find_first(pattern,FA_RDONLY|FA_ARCHIVE,&f);!e;e=dos_find_next(&f)) {
        if(f.attr&(FA_DIR|FA_VOLUME)) continue;
        if(strcopy(target,sizeof(target),dir) || strappend(target,sizeof(target),f.name)) return DE_PATH;
        int found=!dos_find_first(target,FA_RDONLY|FA_HIDDEN|FA_SYSTEM,&there);
        if(found==add) continue;
        int r=put(&f,target,found?&there:NULL); if(r) return r;
    }
    return e==DE_NOMORE || e==DE_NOFILE?0:e;
}
static int below(char *dir) {
    char pattern[DOS_PATH_MAX]; DosFind f; size_t base=strlen(dir); int e=into(dir);
    if(e || !descend) return e;
    if(strcopy(pattern,sizeof(pattern),dir) || strappend(pattern,sizeof(pattern),"*.*")) return DE_PATH;
    for(int r=dos_find_first(pattern,FA_DIR,&f);!r;r=dos_find_next(&f)) {
        if(!(f.attr&FA_DIR) || f.name[0]=='.') continue;
        if(strappend(dir,DOS_PATH_MAX,f.name) || strappend(dir,DOS_PATH_MAX,"\\")) return DE_PATH;
        e=below(dir); dir[base]=0;
        if(e) return e;
    }
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],source[DOS_PATH_MAX]="",target[DOS_PATH_MAX]="",dir[DOS_PATH_MAX];
    const char *p=app_dos->command_tail(); DosFind f; int e;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {
            char c=(char)upper(arg[1]);
            int *flag=arg[2]?NULL:c=='A'?&add:c=='P'?&prompt:c=='R'?&read_only:c=='S'?&descend:c=='U'?&update:c=='W'?&wait_first:NULL;
            if(!flag || *flag) {parse_error(PARSE_SWITCH,arg); return 11;}
            *flag=1;
        } else if(!source[0]) strcopy(source,sizeof(source),arg);
        else if(!target[0]) strcopy(target,sizeof(target),arg);
        else {parse_error(PARSE_TOO_MANY,arg); return 11;}
    }
    if(!source[0]) {to_stderr(1); print("Source path required\n"); to_stderr(0); return 11;}
    if(add && (descend || update)) {parse_error(PARSE_COMBINATION,NULL); return 11;}
    if(wait_first) {print("Press any key to continue . . .\n"); key();}
    e=file_spec(source,source_dir,source_name,1);
    if(e) {report(e,source); return 3;}
    e=dos_canonical(target[0]?target:".",dir);
    if(!e && strlen(dir)>3) e=strappend(dir,sizeof(dir),"\\");
    if(e) {report(e,target); return 3;}
    {
        char pattern[DOS_PATH_MAX];
        strcopy(pattern,sizeof(pattern),source_dir); strappend(pattern,sizeof(pattern),source_name);
        if(dos_find_first(pattern,FA_RDONLY|FA_ARCHIVE,&f)) {to_stderr(1); print("\nNo files found - %s\n",source); to_stderr(0); return 2;}
    }
    e=below(dir);
    if(count) print(add?"\n%u file(s) added\n":"\n%u file(s) replaced\n",(unsigned long long)count);
    else print(add?"\nNo files added\n":"\nNo files replaced\n");
    return e?(EFI_STATUS)(e==DE_NOFILE?2:e==DE_PATH?3:e):0;
}
