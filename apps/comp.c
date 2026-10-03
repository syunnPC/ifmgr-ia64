/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * COMP: MS-DOS 4 CMD/COMP counterpart.
 *   COMP [d:][path][name1] [d:][path][name2]
 *
 * Pair source matches with target names using COPY-style wildcard
 * substitution; a target drive/directory preserves the source name.
 * Report size differences or up to ten byte mismatches (hex offset/values,
 * v4.0 COMP.SKL messages 6-8). Check source Ctrl+Z termination. Prompt for
 * missing names and whether to compare more files.
 */
#include "util.h"
static void compare_files(const char *one,const char *two) {
    static u8 a[4096],b[4096]; unsigned h1,h2; u32 size1,size2,at=0,ga,gb,mismatches=0; u8 last=0; int e;
    print("%s and %s\n",one,two);
    e=dos_open(one,0,0,&h1); if(e) {extended_error(e==DE_PATH?DE_PATH:e==DE_ACCESS?DE_ACCESS:DE_NOFILE,NULL); return;}
    e=dos_open(two,0,0,&h2); if(e) {dos_close(h1); extended_error(e==DE_PATH?DE_PATH:e==DE_ACCESS?DE_ACCESS:DE_NOFILE,NULL); return;}
    dos_seek(h1,0,2,&size1); dos_seek(h2,0,2,&size2); dos_seek(h1,0,0,&ga); dos_seek(h2,0,0,&gb);
    if(size1!=size2) print("Files are different sizes\n");
    else {
        while(at<size1 && mismatches<10) {
            if(dos_read(h1,a,sizeof(a),&ga) || dos_read(h2,b,sizeof(b),&gb) || !ga || ga!=gb) break;
            for(u32 i=0;i<ga && mismatches<10;i++) if(a[i]!=b[i]) {
                print("Compare error at OFFSET %x\nFile 1 = %x\nFile 2 = %x\n",
                      (unsigned long long)(at+i),(unsigned long long)a[i],(unsigned long long)b[i]);
                mismatches++;
            }
            last=a[ga-1]; at+=ga;
        }
        if(mismatches>=10) print("10 Mismatches - ending compare\n");
        else if(!mismatches) print("Files compare ok\n");
        if(at>=size1 && size1 && last!=0x1a) print("Eof mark not found\n");
    }
    dos_close(h1); dos_close(h2);
}
static int compare_set(const char *first,const char *second) {
    char dir1[DOS_PATH_MAX],dir2[DOS_PATH_MAX],name1[13],name2[13],full1[DOS_PATH_MAX],full2[DOS_PATH_MAX],target[13];
    DosFind f; int e,any=0;
    e=file_spec(first,dir1,name1,1);
    if(!e) e=file_spec(second,dir2,name2,1);
    if(e) {extended_error(e==DE_DRIVE?DE_DRIVE:DE_PATH,NULL); return e;}
    strcopy(full1,sizeof(full1),dir1); strappend(full1,sizeof(full1),name1);
    for(e=dos_find_first(full1,0,&f);!e;e=dos_find_next(&f)) {
        any=1;
        map_name(name2,f.name,target);
        strcopy(full1,sizeof(full1),dir1); strappend(full1,sizeof(full1),f.name);
        strcopy(full2,sizeof(full2),dir2); strappend(full2,sizeof(full2),target);
        compare_files(full1,full2);
        print("\n");
    }
    if(!any) {extended_error(DE_NOFILE,NULL); return DE_NOFILE;}
    return 0;
}
static void ask_name(const char *prompt,char *out,unsigned cap) {
    u8 line[130]={128};
    print("%s",prompt);
    out[0]=0;
    if(dos_line_input(line)) return;
    unsigned n=line[1]; if(n>=cap) n=cap-1;
    memcpy(out,line+2,n); out[n]=0;
    print("\n");
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[DOS_PATH_MAX],first[DOS_PATH_MAX]="",second[DOS_PATH_MAX]=""; const char *p=app_dos->command_tail();
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {parse_error(PARSE_PARAMETER,arg); return 1;}
        if(!first[0]) strcopy(first,sizeof(first),arg);
        else if(!second[0]) strcopy(second,sizeof(second),arg);
        else {parse_error(PARSE_PARAMETER,arg); return 1;}
    }
    for(;;) {
        if(!first[0]) ask_name("\n\nEnter primary filename\n",first,sizeof(first));
        if(!second[0]) ask_name("\n\nEnter 2nd filename or drive id\n",second,sizeof(second));
        if(first[0] && second[0]) compare_set(first,second);
        for(;;) {
            DosRegs r={.ax=0x0c01};
            print("Compare more files (Y/N) ?");
            if(dos_call(&r)) return 0;
            char c=(char)upper((int)(r.ax&0xff));
            print("\n");
            if(c=='N') return 0;
            if(c=='Y') break;
        }
        first[0]=second[0]=0;
    }
}
