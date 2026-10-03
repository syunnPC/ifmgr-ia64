/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * PRINT: native counterpart of MS-DOS 4 CMD/PRINT (PRINT_T.ASM).
 *   PRINT [/D:device] [/B:size] [/U:ticks] [/M:ticks] [/S:ticks] [/Q:size]
 *         [/T] [[d:][path]filename [/C] [/P] ...]
 * The resident part is MSDOS.SYS's (kernel/print.c, DosApi print). The
 * first run installs it with the list device /D names or the one asked for
 * ("Name of list device [PRN]: "), and with /B /U /M /S /Q; later runs take
 * those switches as invalid. Files are queued or, after /C, canceled (a /C
 * or /P right after a name applies to it too); wildcards queue each file
 * they match, /T cancels them all. The queue is shown at the end. The
 * messages are v4.0 PRINT.SKL's.
 */
#include "util.h"
#define BUSY_ERRORS 1500U /* v4.0 PRIDEFS.INC ERRCNT1 */

static DosPrintRequest request(void) {DosPrintRequest r; memset(&r,0,sizeof(r)); r.size=sizeof(r); return r;}
static int installed,canceling,full_shown;
static DosPrintRequest settings;
static int number(const char *s,u32 *out) {
    u32 n=0; if(!*s) return 0;
    for(;*s;s++) {if(*s<'0' || *s>'9' || n>100000) return 0; n=n*10+(u32)(*s-'0');}
    *out=n; return 1;
}
/* The resident part, asking for the device unless /D named it. */
static int install(int device_given) {
    if(installed) return 0;
    if(!device_given) {
        u8 line[11]={9,0};
        print("Name of list device [PRN]: ");
        int e=dos_line_input(line); print("\n");
        if(e) return e;
        unsigned n=line[1]; if(n>8) n=8;
        for(unsigned i=0;i<n;i++) settings.device[i]=(char)upper((char)line[2+i]);
        settings.device[n]=0;
    }
    int e=app_dos->print(DOS_PRINT_INSTALL,&settings);
    if(e==DE_NOFILE) {to_stderr(1); print("List output is not assigned to a device\n"); to_stderr(0); return e;}
    if(e) {extended_error(e,NULL); return e;}
    installed=1; print("Resident part of PRINT installed\n"); return 0;
}
static void queue_file(const char *full) {
    if(strlen(full)>=DOS_PRINT_ENTRY) {to_stderr(1); print("Pathname too long\n"); to_stderr(0); return;}
    unsigned h; int e=dos_open(full,DOS_OPEN_READ,0,&h);
    if(e) {extended_error(e,full); return;}
    dos_close(h);
    DosPrintRequest r=request(); strcopy(r.path,sizeof(r.path),full);
    e=app_dos->print(DOS_PRINT_SUBMIT,&r);
    if(e==DE_NOMEM) {
        if(!full_shown) {to_stderr(1); print("PRINT queue is full\n"); to_stderr(0);}
        full_shown=1; return;
    }
    if(e) extended_error(e,full);
    else full_shown=0;
}
static void file_arg(const char *spec) {
    char dir[DOS_PATH_MAX],name[13],full[DOS_PATH_MAX];
    int e=file_spec(spec,dir,name,0);
    if(e==DE_DRIVE) {to_stderr(1); print("Invalid drive specification\n"); to_stderr(0); return;}
    if(e || strcopy(full,sizeof(full),dir) || strappend(full,sizeof(full),name)) {
        to_stderr(1); print("Pathname too long\n"); to_stderr(0); return;
    }
    if(canceling) {
        DosPrintRequest r=request();
        if(strlen(full)>=DOS_PRINT_ENTRY) {to_stderr(1); print("Pathname too long\n"); to_stderr(0); return;}
        strcopy(r.path,sizeof(r.path),full);
        if(app_dos->print(DOS_PRINT_CANCEL,&r)) {to_stderr(1); print("File not in PRINT queue\n"); to_stderr(0);}
        return;
    }
    int wild=0; for(const char *p=name;*p;p++) if(*p=='*' || *p=='?') wild=1;
    if(!wild) {queue_file(full); return;}
    DosFind f; int found=0;
    for(e=dos_find_first(full,0,&f);!e;e=dos_find_next(&f)) {
        char one[DOS_PATH_MAX];
        if(strcopy(one,sizeof(one),dir) || strappend(one,sizeof(one),f.name)) continue;
        found=1; queue_file(one);
    }
    if(!found) extended_error(DE_NOFILE,full);
}
static void status(void) {
    DosPrintRequest r=request();
    if(app_dos->print(DOS_PRINT_STATUS,&r)) return;
    if(r.errors>=BUSY_ERRORS) print("Errors on list device indicate that it\nmay be off-line. Please check it.\n");
    if(!r.queue[0]) print("PRINT queue is empty\n");
    for(const char *q=r.queue;*q;q+=DOS_PRINT_ENTRY)
        print(q==r.queue?"\n\n  %s is currently being printed\n":"  %s is in queue\n",q);
    app_dos->print(DOS_PRINT_RELEASE,&r);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status_=app_init(st); if(EFI_ERROR(status_)) return status_;
    if(app_dos->size<offsetof(DosApi,idle)+sizeof(app_dos->idle) || !app_dos->print) {
        to_stderr(1); print("Incorrect DOS version\n"); to_stderr(0); return 1;
    }
    settings=request();
    installed=app_dos->print(DOS_PRINT_QUERY,&settings)==0;
    char arg[DOS_PATH_MAX],ahead[DOS_PATH_MAX]; const char *p=app_dos->command_tail(); int device_given=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {
            char c=(char)upper(arg[1]); u32 value=0;
            if((c=='T' || c=='C' || c=='P') && !arg[2]) {
                if(install(device_given)) return 1;
                if(c=='T') {DosPrintRequest r=request(); app_dos->print(DOS_PRINT_CANCEL_ALL,&r);}
                else canceling=c=='C';
                continue;
            }
            int setting=c=='D' || c=='B' || c=='U' || c=='M' || c=='S' || c=='Q';
            if(!setting || installed || arg[2]!=':') {parse_error(PARSE_SWITCH,arg); return 1;}
            if(c=='D') {
                unsigned n=0;
                for(const char *s=arg+3;*s && *s!=':' && n<8;s++) settings.device[n++]=(char)upper(*s);
                settings.device[n]=0; device_given=1; continue;
            }
            if(!number(arg+3,&value)) {parse_error(PARSE_FORMAT,arg); return 1;}
            u32 low=c=='B'?512:c=='Q'?4:1,high=c=='B'?16384:c=='Q'?32:255;
            if(value<low || value>high) {parse_error(PARSE_RANGE,arg); return 1;}
            if(c=='B') settings.buffer_bytes=value; else if(c=='U') settings.busy_ticks=value;
            else if(c=='M') settings.max_ticks=value; else if(c=='S') settings.slice_ticks=value;
            else settings.queue_entries=value;
            continue;
        }
        if(install(device_given)) return 1;
        /* A /C or /P that follows a name applies to it. */
        const char *after=next_arg(p,ahead,sizeof(ahead));
        if(after && ahead[0]=='/' && (upper(ahead[1])=='C' || upper(ahead[1])=='P') && !ahead[2]) {
            canceling=upper(ahead[1])=='C'; p=after;
        }
        file_arg(arg);
    }
    if(install(device_given)) return 1;
    status();
    return 0;
}
