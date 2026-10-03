/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * ASSIGN: native counterpart of MS-DOS 4 CMD/ASSIGN.
 *   ASSIGN [x[:]=y[:] [...]] [/STA[TUS]]
 * Sends what is asked of drive x to drive y. Each run replaces MSDOS.SYS's
 * ASSIGN table (DosApi assign) with the pairs given, so that ASSIGN alone
 * ends every one; /STATUS lists those in effect, "Original x: set to y:"
 * (v4.0 ASSIGN.SKL's message 2). Both letters must name drives that can be
 * used.
 */
#include "util.h"
static int letter_of(const char *s,u8 *out) {
    char c=(char)upper(s[0]);
    if(c<'A' || c>'Z' || (s[1] && (s[1]!=':' || s[2]))) return 0;
    *out=(u8)(c-'A'); return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[DOS_PATH_MAX]; const char *p=app_dos->command_tail(); u8 table[DOS_DRIVES],now[DOS_DRIVES];
    int show=0,pending=-1; DosDriveInfo info;
    for(unsigned i=0;i<DOS_DRIVES;i++) table[i]=(u8)i;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {
            static const char word[]="STATUS"; unsigned n=1;
            while(arg[n] && word[n-1] && upper(arg[n])==word[n-1]) n++;
            if(n<4 || arg[n]) {parse_error(PARSE_SWITCH,arg); return 1;}
            show=1; continue;
        }
        u8 l;
        if(!letter_of(arg,&l)) {parse_error(PARSE_PARAMETER,arg); return 1;}
        if(pending<0) {pending=l; continue;}
        table[pending]=l; pending=-1;
    }
    if(pending>=0) {parse_error(PARSE_PARAMETER,NULL); return 1;}
    int changing=0; for(unsigned i=0;i<DOS_DRIVES;i++) if(table[i]!=i) changing=1;
    if(!show || changing) {
        /* The pairs are checked against the drives themselves. */
        u8 identity[DOS_DRIVES],before[DOS_DRIVES];
        for(unsigned i=0;i<DOS_DRIVES;i++) identity[i]=(u8)i;
        if(dos_assign(identity,before)) {parse_error(PARSE_PARAMETER,NULL); return 1;}
        for(unsigned i=0;i<DOS_DRIVES;i++) if(table[i]!=i && (dos_drive_info(i,&info) || dos_drive_info(table[i],&info))) {
            char bad[3]={(char)('A'+i),':',0};
            dos_assign(before,NULL); parse_error(PARSE_PARAMETER,bad); return 1;
        }
        if(dos_assign(table,NULL)) {dos_assign(before,NULL); parse_error(PARSE_PARAMETER,NULL); return 1;}
    }
    if(show && !dos_assign(NULL,now))
        for(unsigned i=0;i<DOS_DRIVES;i++) if(now[i]!=i) print("Original %c: set to %c:\n",(char)('A'+i),(char)('A'+now[i]));
    return 0;
}
