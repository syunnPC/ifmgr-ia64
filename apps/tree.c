/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * TREE: native counterpart of MS-DOS 4 CMD/TREE.
 *   TREE [d:][path] [/F] [/A]
 * The subdirectories below the given directory (the current one of the
 * drive by default), drawn with the line characters of v4.0 TREE.SKL's
 * message 7 (code page 437's 192, 196, 195 and 179) or, with /A, with
 * \ - + | for other code pages; /F lists each directory's files first.
 * Hidden and system entries are left out, as DOS 4's search attributes did.
 */
#include "maint.h"
#include "util.h"
static int files_too,subdirs_seen;
static char elbow='\xc0',dash='\xc4',tee='\xc3',bar='\xb3';
/* The next subdirectory of a search after the current one; 0 when none. */
static int next_subdir(DosFind *f,int first,const char *pattern) {
    int e=first?dos_find_first(pattern,FA_DIR,f):dos_find_next(f);
    for(;!e;e=dos_find_next(f)) if((f->attr&FA_DIR) && f->name[0]!='.') return 1;
    return 0;
}
static int has_subdir(const char *dir) {
    char pattern[DOS_PATH_MAX]; DosFind f;
    if(strcopy(pattern,sizeof(pattern),dir) || strappend(pattern,sizeof(pattern),"*.*")) return 0;
    return next_subdir(&f,1,pattern);
}
static int show(char *dir,char *prefix) {
    char pattern[DOS_PATH_MAX]; DosFind f,ahead; size_t base=strlen(dir),depth=strlen(prefix); int more,any=0;
    if(strcopy(pattern,sizeof(pattern),dir) || strappend(pattern,sizeof(pattern),"*.*")) return DE_PATH;
    if(files_too) {
        int below=has_subdir(dir);
        for(int e=dos_find_first(pattern,0,&f);!e;e=dos_find_next(&f)) {
            if(f.attr&(FA_DIR|FA_VOLUME)) continue;
            print("%s%c   %s\n",prefix,below?bar:' ',f.name); any=1;
        }
        if(any && below) print("%s%c\n",prefix,bar);
        else if(any) print("%s\n",prefix);
    }
    more=next_subdir(&f,1,pattern);
    while(more) {
        ahead=f; int last=!next_subdir(&ahead,0,pattern);
        subdirs_seen=1;
        print("%s%c%c%c%c%s\n",prefix,last?elbow:tee,dash,dash,dash,f.name);
        if(strappend(dir,DOS_PATH_MAX,f.name) || strappend(dir,DOS_PATH_MAX,"\\") || depth+4>=DOS_PATH_MAX) return DE_PATH;
        prefix[depth]=last?' ':bar; prefix[depth+1]=prefix[depth+2]=prefix[depth+3]=' '; prefix[depth+4]=0;
        int e=show(dir,prefix);
        dir[base]=0; prefix[depth]=0;
        if(e) return e;
        f=ahead; more=!last;
    }
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],spec[DOS_PATH_MAX]="",dir[DOS_PATH_MAX],prefix[DOS_PATH_MAX]="",label[12]; const char *p=app_dos->command_tail();
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/' && upper(arg[1])=='F' && !arg[2]) files_too=1;
        else if(arg[0]=='/' && upper(arg[1])=='A' && !arg[2]) {elbow='\\'; dash='-'; tee='+'; bar='|';}
        else if(arg[0]=='/') {parse_error(PARSE_SWITCH,arg); return 1;}
        else if(spec[0]) {parse_error(PARSE_TOO_MANY,arg); return 1;}
        else strcopy(spec,sizeof(spec),arg);
    }
    u8 attr=0;
    int e=dos_full_path(spec[0]?spec:".",dir);
    if(!e && strlen(dir)>3) e=dos_attribute(dir,0,&attr);
    if(!e && strlen(dir)>3 && !(attr&FA_DIR)) e=DE_PATH;
    if(e) {to_stderr(1); print(e==DE_DRIVE?"Invalid drive specification\n":"Invalid path\n"); to_stderr(0); return 1;}
    unsigned drive=(unsigned)(dir[0]-'A');
    if(!label_get(drive,label) && label[0]) {
        unsigned end=11; while(end && label[end-1]==' ') label[--end]=0;
        print("Directory PATH listing for Volume %s\n",label);
    } else print("Directory PATH listing\n");
    DosMediaId id={.size=sizeof(id)};
    if(!dos_media_id(drive,&id,0)) {char text[10]; serial_number(id.serial,text); print("Volume Serial Number is %s\n",text);}
    print("%s\n",dir);
    if(strlen(dir)>3 || dir[2]!='\\') strappend(dir,sizeof(dir),"\\");
    e=show(dir,prefix);
    if(e) {extended_error(e,NULL); return 1;}
    if(!subdirs_seen) print("No sub-directories exist\n\n");
    return 0;
}
