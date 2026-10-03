/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * ATTRIB: native counterpart of MS-DOS 4 CMD/ATTRIB.
 *   ATTRIB [+R|-R] [+A|-A] [d:][path]filename [/S]
 * Shows or changes the read-only and archive attributes of the files that
 * match; directories, hidden and system files are left alone. /S takes the
 * subdirectories too, each before the files of its parent, as DOS 4 did.
 * A setting may come before or after the file name; the same one twice, or
 * one both set and cleared, is a parameter format error.
 */
#include "util.h"
static u8 set_mask,clear_mask; static int setting,done;
static int attrib_one(void *ctx,const char *dir,const DosFind *f) {
    char path[DOS_PATH_MAX]; u8 attr=f->attr; int e;
    (void)ctx;
    e=strcopy(path,sizeof(path),dir); if(!e) e=strappend(path,sizeof(path),f->name);
    if(e) return e;
    done=1;
    if(setting) {
        attr=(u8)((attr&~clear_mask)|set_mask);
        return dos_attribute(path,1,&attr);
    }
    /* Bits 7 to 0 in sixteen columns, A and R where theirs are set. */
    char shown[17]; memset(shown,' ',16); shown[16]=0;
    if(attr&FA_ARCHIVE) shown[2]='A';
    if(attr&FA_RDONLY) shown[7]='R';
    print("%s    %s\n",shown,path);
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],spec[DOS_PATH_MAX]="",dir[DOS_PATH_MAX],name[13];
    const char *p=app_dos->command_tail(); int recurse=0; unsigned plus=0,minus=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {
            if(upper(arg[1])!='S' || arg[2]) {parse_error(PARSE_SWITCH,arg); return 1;}
            if(recurse) {parse_error(PARSE_FORMAT,arg); return 1;}
            recurse=1;
        } else if((arg[0]=='+' || arg[0]=='-') && arg[1]) {
            u8 bit=upper(arg[1])=='R'?FA_RDONLY:upper(arg[1])=='A'?FA_ARCHIVE:0;
            unsigned *seen=arg[0]=='+'?&plus:&minus;
            if(!bit || arg[2]) {parse_error(PARSE_PARAMETER,arg); return 1;}
            if(*seen&bit) {parse_error(PARSE_FORMAT,arg); return 1;}
            *seen|=bit;
        } else if(spec[0]) {parse_error(PARSE_TOO_MANY,arg); return 1;}
        else strcopy(spec,sizeof(spec),arg);
    }
    if(!spec[0]) {parse_error(PARSE_MISSING,NULL); return 1;}
    if(plus&minus) {parse_error(PARSE_FORMAT,NULL); return 1;}
    set_mask=(u8)plus; clear_mask=(u8)minus; setting=plus || minus;
    int e=file_spec(spec,dir,name,0);
    if(!e) e=walk(dir,name,0,recurse,1,attrib_one,NULL);
    if(!e && !done) e=DE_NOFILE;
    if(e) {extended_error(e,spec); return 1;}
    return 0;
}
