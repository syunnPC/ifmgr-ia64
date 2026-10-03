/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * XCOPY: MS-DOS 4 CMD/XCOPY counterpart.
 *   XCOPY source [target] [/A] [/D:date] [/E] [/M] [/P] [/S] [/V] [/W]
 *
 * Default target is the current directory; source directories match all
 * files except hidden/system entries. /S creates subdirectories as needed;
 * /E includes empty ones; /A selects archive files; /M also clears archive.
 * /D selects dates at/after the country-formatted date. /P confirms files;
 * /V verifies writes; /W waits for a key. Prompt whether a missing target is
 * a file or directory. Messages: v4.0 XCOPY.SKL. Exit: 0=success, 1=no files,
 * 4=pre-copy error, 5=write error.
 */
#include "util.h"
static int recurse,empty_too,archive_only,clear_archive,prompt,verify,wait_first,since_set;
static u16 since; static u32 copied;
static char source_name[13],target_name[13];
static char key(void) {
    DosRegs r={.ax=0x0c08};
    if(dos_call(&r)) return 0;
    if(!(r.ax&0xff)) {r=(DosRegs){.ax=0x0800}; dos_call(&r); return 0;}
    return (char)(r.ax&0xff);
}
static int yes(const char *path) {
    for(;;) {
        print("%s (Y/N)?",path);
        DosRegs r={.ax=0x0c01}; if(dos_call(&r)) return 0;
        char c=(char)upper((int)(r.ax&0xff)); print("\n");
        if(c=='Y' || c=='N') return c=='Y';
    }
}
/* Whether a path begins with another, letter case aside. */
static int starts_with(const char *path,const char *head) {
    for(;*head;head++,path++) if(upper(*path)!=upper(*head)) return 0;
    return 1;
}
typedef struct {char source[DOS_PATH_MAX],target[DOS_PATH_MAX];} Pair;
static int copy_dir(Pair *at) {
    char pattern[DOS_PATH_MAX],from[DOS_PATH_MAX],to[DOS_PATH_MAX],name[13]; DosFind f; int e,made=0;
    size_t sb=strlen(at->source),tb=strlen(at->target);
    if(empty_too && recurse) {e=make_dirs(at->target); if(e) {extended_error(DE_ACCESS,NULL); print("Unable to create directory\n"); return 4;} made=1;}
    if(strcopy(pattern,sizeof(pattern),at->source) || strappend(pattern,sizeof(pattern),source_name)) {print("Path too long\n"); return 4;}
    for(e=dos_find_first(pattern,0,&f);!e;e=dos_find_next(&f)) {
        if(f.attr&(FA_DIR|FA_VOLUME|FA_HIDDEN|FA_SYSTEM)) continue;
        if((archive_only || clear_archive) && !(f.attr&FA_ARCHIVE)) continue;
        if(since_set && f.date<since) continue;
        map_name(target_name,f.name,name);
        if(strcopy(from,sizeof(from),at->source) || strappend(from,sizeof(from),f.name) ||
           strcopy(to,sizeof(to),at->target) || strappend(to,sizeof(to),name)) {print("Path too long\n"); return 4;}
        if(prompt && !yes(from)) continue;
        if(!made) {
            int m=make_dirs(at->target);
            if(m) {print("Unable to create directory\n"); return 4;}
            made=1;
        }
        char a[DOS_PATH_MAX],b[DOS_PATH_MAX];
        if(!dos_canonical(from,a) && !dos_canonical(to,b) && !stricmp(a,b)) {print("File cannot be copied onto itself\n"); return 4;}
        if(!prompt) print("%s\n",from);
        int c=copy_file(from,to);
        if(c==DE_FULL) {print("Insufficient disk space\n"); return 5;}
        if(c) {print("File creation error\n"); return 5;}
        if(clear_archive) {u8 attr=(u8)(f.attr&~FA_ARCHIVE); dos_attribute(from,1,&attr);}
        copied++;
    }
    if(e!=DE_NOMORE && e!=DE_NOFILE) {extended_error(e,NULL); return 4;}
    if(!recurse) return 0;
    if(strcopy(pattern,sizeof(pattern),at->source) || strappend(pattern,sizeof(pattern),"*.*")) return 4;
    for(e=dos_find_first(pattern,FA_DIR,&f);!e;e=dos_find_next(&f)) {
        if(!(f.attr&FA_DIR) || f.name[0]=='.') continue;
        if(strappend(at->source,DOS_PATH_MAX,f.name) || strappend(at->source,DOS_PATH_MAX,"\\") ||
           strappend(at->target,DOS_PATH_MAX,f.name) || strappend(at->target,DOS_PATH_MAX,"\\")) {print("Path too long\n"); return 4;}
        int r=copy_dir(at);
        at->source[sb]=0; at->target[tb]=0;
        if(r) return r;
    }
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],source[DOS_PATH_MAX]="",target[DOS_PATH_MAX]=""; const char *p=app_dos->command_tail();
    unsigned positional=0; static Pair pair; int e;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {
            char c=(char)upper(arg[1]); int *flag=NULL;
            if(c=='D' && arg[2]==':') {
                if(since_set) {parse_error(PARSE_SWITCH,arg); return 4;}
                /* The date runs on to the next blank. */
                char date[24]; unsigned n=0; const char *d=arg+3;
                while(*d && n<sizeof(date)-1) date[n++]=*d++;
                while(*p && *p!=' ' && *p!='\t' && n<sizeof(date)-1) date[n++]=*p++;
                date[n]=0;
                if(!parse_date(date,&since)) {print("Invalid date\n"); return 4;}
                since_set=1; continue;
            }
            if(arg[2]) {parse_error(PARSE_PARAMETER,arg); return 4;}
            flag=c=='A'?&archive_only:c=='E'?&empty_too:c=='M'?&clear_archive:c=='P'?&prompt:c=='S'?&recurse:c=='V'?&verify:c=='W'?&wait_first:NULL;
            if(!flag) {parse_error(PARSE_PARAMETER,arg); return 4;}
            if(*flag) {parse_error(PARSE_SWITCH,arg); return 4;}
            *flag=1; continue;
        }
        if(++positional>2) {print("Invalid number of parameters\n"); return 4;}
        strcopy(positional==1?source:target,DOS_PATH_MAX,arg);
    }
    if(!positional) {print("Invalid number of parameters\n"); return 4;}
    if(empty_too && !recurse) empty_too=0;
    e=file_spec(source,pair.source,source_name,1);
    if(e) {print(e==DE_DRIVE?"Invalid drive specification\n":"Invalid path\n"); return 4;}
    if(!target[0]) {
        e=dos_full_path(".",pair.target); if(!e && strlen(pair.target)>3) e=strappend(pair.target,DOS_PATH_MAX,"\\");
        strcopy(target_name,13,"*.*");
    } else {
        size_t n=strlen(target); u8 attr=0;
        int as_dir=target[n-1]=='\\' || target[n-1]==':';
        char full[DOS_PATH_MAX];
        e=dos_full_path(target,full);
        if(e) {print(e==DE_DRIVE?"Invalid drive specification\n":"Invalid path\n"); return 4;}
        if(!as_dir && !dos_attribute(full,0,&attr)) as_dir=(attr&FA_DIR)!=0;
        else if(!as_dir && !strchr(target,'*') && !strchr(target,'?')) {
            for(;;) {
                print("Does %s specify a file name\nor directory name on the target\n(F = file, D = directory)?",target);
                char c=(char)upper(key()); print("%c\n",c);
                if(c=='F') break;
                if(c=='D') {as_dir=1; break;}
            }
        }
        if(as_dir) {
            e=strcopy(pair.target,DOS_PATH_MAX,full); if(!e && strlen(pair.target)>3) e=strappend(pair.target,DOS_PATH_MAX,"\\");
            strcopy(target_name,13,"*.*");
        } else e=file_spec(target,pair.target,target_name,0);
    }
    if(e) {print("Invalid path\n"); return 4;}
    /* Cyclic by the true names, whatever letters lead there. */
    {
        char source_true[DOS_PATH_MAX],target_true[DOS_PATH_MAX];
        if(recurse && !dos_canonical(pair.source,source_true) && !dos_canonical(pair.target,target_true) &&
           starts_with(target_true,source_true)) {
            size_t n=strlen(source_true);
            if(source_true[n-1]=='\\' || !target_true[n] || target_true[n]=='\\') {print("Cannot perform a cyclic copy\n"); return 4;}
        }
    }
    int verified=0,old_verify=0;
    if(verify && !dos_verify(0,&old_verify)) {int on=1; verified=!dos_verify(1,&on);}
    if(wait_first) {print("Press any key to begin copying file(s)"); key(); print("\n");}
    print("Reading source file(s)...\n");
    int r=copy_dir(&pair);
    if(verified) dos_verify(1,&old_verify);
    if(!r && !copied) extended_error(DE_NOFILE,source);
    print("%u File(s) copied\n",(unsigned long long)copied);
    return (EFI_STATUS)(r?r:copied?0:1);
}
