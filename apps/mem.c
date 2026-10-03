/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * MEM: MS-DOS 4 CMD/MEM counterpart (MEM.C).
 *   MEM [/PROGRAM | /DEBUG]
 *
 * Report DosApi arena memory; count MEM's own image as free when finding
 * the largest executable block. EMM386.SYS supplies per-VDM EMS (shown
 * free here); HIMEM.SYS owns XMS, unavailable through INT 15h. Both options
 * list arena blocks. Firmware images/drivers lie outside the arena, so
 * /DEBUG has no extra blocks. Messages: v4.0 USA-MS.MSG.
 */
#include "util.h"

static void say_bytes(u64 n,const char *text) {print("%10u %s\n",(unsigned long long)n,text);}
static void hex6(u32 v) {print("%06x",(unsigned long long)v);}
static void field(const char *s,unsigned width) {unsigned n=0; for(;s[n] && n<width;n++) print("%c",s[n]); for(;n<width;n++) print(" ");}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    if(app_dos->size<offsetof(DosApi,arena)+sizeof(app_dos->arena) || !app_dos->arena) {
        to_stderr(1); print("Incorrect DOS version\n"); to_stderr(0); return 1;
    }
    /* One switch at most: /PROGRAM or /DEBUG. */
    char arg[DOS_PATH_MAX]; const char *p=app_dos->command_tail(); int level=0,seen=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        for(char *s=arg;*s;s++) *s=(char)upper(*s);
        if(arg[0]!='/') {parse_error(PARSE_PARAMETER,arg); return 1;}
        if(strcmp(arg,"/PROGRAM") && strcmp(arg,"/DEBUG")) {parse_error(PARSE_SWITCH,arg); return 1;}
        if(seen++) {parse_error(PARSE_TOO_MANY,arg); return 1;}
        level=arg[1]=='D'?2:1;
    }
    DosInfo info={0}; u32 self=dos_query(&info)?0:info.pid;
    u64 total=0,largest=0,run=0;
    if(level) {
        print("\n  Address     Name          Size       Type \n");
        print("  \xc4\xc4\xc4\xc4\xc4\xc4\xc4     \xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4     \xc4\xc4\xc4\xc4\xc4\xc4     \xc4\xc4\xc4\xc4\xc4\xc4\n");
    }
    for(u32 i=0;;i++) {
        DosArenaBlock b={.size=sizeof(b)};
        int e=app_dos->arena(i,&b);
        if(e==DE_NOMORE) break;
        if(e) {extended_error(e,NULL); return 1;}
        total+=((u64)b.paragraphs+1)*16;
        /* A run of free blocks, MEM's own image among them. */
        int open=b.kind==DOS_ARENA_FREE || (b.kind==DOS_ARENA_PROGRAM && b.owner==self);
        run=open?run+((u64)b.paragraphs+(run?1:0))*16:0;
        if(run>largest) largest=run;
        if(!level) continue;
        static const char *const kinds[]={"-- Free --","System Data","Program","Data"};
        print("  "); hex6(b.paragraph*16); print("      ");
        field(b.kind==DOS_ARENA_FREE?"":b.name,8); print("     ");
        hex6(b.paragraphs*16); print("     ");
        field(b.kind<4?kinds[b.kind]:"",10); print("\n");
    }
    print("\n\n");
    say_bytes(total,"bytes total memory");
    say_bytes(total,"bytes available");
    say_bytes(largest,"largest executable program size");
    u32 v=0;
    if(!dos_installed(DOS_INSTALLED_EMS,NULL,&v) && (v&0xffff)) {
        print("\n");
        say_bytes((u64)(v&0xffff)*1024,"bytes total EMS memory");
        say_bytes((u64)(v&0xffff)*1024,"bytes free EMS memory");
    }
    if(!dos_installed(DOS_INSTALLED_XMS,NULL,&v) && (v&DOS_XMS_ON)) {
        print("\n");
        say_bytes((u64)DOS_EXTENDED_KB*1024,"bytes total extended memory");
        say_bytes(0,"bytes available extended memory");
    }
    return 0;
}
