/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * FIND: native counterpart of MS-DOS 4 CMD/FIND.
 *   FIND [/V] [/C] [/N] "string" [[d:][path]filename ...]
 * The lines of the files, or of standard input, that contain the string
 * (letter case counts; "" in it stands for a quote): with /V those that do
 * not, with /C only how many, with /N each after its line number in
 * brackets. Each file is headed "---------- NAME" as it was typed; one that
 * cannot be opened is reported and the next taken. Errors of FIND used as a
 * filter begin with "FIND: ". The exit code is 2 after a parameter error.
 */
#include "util.h"
static int invert,count_only,numbers,named;
static char text[256]; static unsigned text_length;
static void fail(unsigned which,const char *arg) {
    if(!named) {to_stderr(1); print("FIND: "); to_stderr(0);}
    parse_error(which,arg);
}
static void out(const char *s,unsigned n) {u32 done; dos_write(1,s,n,&done);}
static int contains(const char *line,unsigned n) {
    if(text_length>n) return 0;
    for(unsigned i=0;i+text_length<=n;i++) if(!memcmp(line+i,text,text_length)) return 1;
    return 0;
}
static void search(unsigned handle,const char *name) {
    static LineReader reader; static char line[4096];
    unsigned n,number=0,matched=0;
    if(name) {print("\n---------- %s",name); if(!count_only) print("\n");}
    lines_open(&reader,handle);
    while(lines_next(&reader,line,sizeof(line),&n)) {
        number++;
        if(contains(line,n)==invert) continue;
        matched++;
        if(count_only) continue;
        if(numbers) print("[%u]",(unsigned long long)number);
        out(line,n); out("\r\n",2);
    }
    if(count_only) print("%s%u\n",name?": ":"",(unsigned long long)matched);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *p=app_dos->command_tail(); char files[DOS_PATH_MAX*8]; unsigned used=0; int have_text=0;
    /* Switches anywhere, the quoted string, then file names. */
    for(;;) {
        while(*p==' ' || *p=='\t') p++;
        if(!*p) break;
        if(*p=='/') {
            char c=(char)upper(p[1]);
            if((c!='V' && c!='C' && c!='N') || (p[2] && p[2]!=' ' && p[2]!='\t' && p[2]!='/')) {
                char bad[16]; unsigned k=0; while(p[k] && p[k]!=' ' && p[k]!='\t' && k<sizeof(bad)-1) {bad[k]=p[k]; k++;} bad[k]=0;
                fail(PARSE_SWITCH,bad); return 2;
            }
            if(c=='V') invert=1; else if(c=='C') count_only=1; else numbers=1;
            p+=2; continue;
        }
        if(!have_text) {
            if(*p!='"') {fail(PARSE_FORMAT,NULL); return 2;}
            for(p++;;p++) {
                if(!*p) {fail(PARSE_FORMAT,NULL); return 2;}
                if(*p=='"') {if(p[1]!='"') {p++; break;} p++;}
                if(text_length<sizeof(text)-1) text[text_length++]=*p;
            }
            text[text_length]=0; have_text=1; continue;
        }
        /* A file name, kept until every argument is read. */
        unsigned k=0;
        while(p[k] && p[k]!=' ' && p[k]!='\t' && p[k]!='/') k++;
        if(used+k+1>sizeof(files)) {fail(PARSE_TOO_MANY,NULL); return 2;}
        memcpy(files+used,p,k); files[used+k]=0; used+=k+1; p+=k; named=1;
    }
    if(!have_text) {fail(PARSE_MISSING,NULL); return 2;}
    if(!used) {search(0,NULL); return 0;}
    for(unsigned at=0;at<used;at+=(unsigned)strlen(files+at)+1) {
        char *name=files+at; unsigned h;
        for(char *q=name;*q;q++) *q=(char)upper(*q);
        int e=dos_open(name,0,0,&h);
        if(e) {extended_error(e==DE_ACCESS?DE_ACCESS:DE_NOFILE,name); continue;}
        search(h,name); dos_close(h);
    }
    return 0;
}
