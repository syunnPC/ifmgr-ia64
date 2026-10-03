/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * SHARE: native counterpart of MS-DOS 4 CMD/SHARE.
 *   SHARE [/F:space] [/L:locks]
 * File sharing and locking are always MSDOS.SYS's, as they are DOS 4's once
 * SHARE is loaded; SHARE checks its parameters (1 to 65535) and counts as
 * installed from its first run (DosApi installed), after which it says
 * "SHARE already installed" (v4.0 USA-MS.MSG COMMON2) as DOS 4's does.
 */
#include "util.h"
static int number(const char *s,u32 *out) {
    u32 n=0; if(!*s) return 0;
    for(;*s;s++) {if(*s<'0' || *s>'9' || n>65535) return 0; n=n*10+(u32)(*s-'0');}
    *out=n; return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[64]; const char *p=app_dos->command_tail(); int f=0,l=0; u32 value,one=1,before=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        char c=(char)upper(arg[1]);
        if(arg[0]!='/' || (c!='F' && c!='L') || arg[2]!=':') {parse_error(arg[0]=='/'?PARSE_SWITCH:PARSE_PARAMETER,arg); return 255;}
        if((c=='F' && f) || (c=='L' && l)) {parse_error(PARSE_SWITCH,arg); return 255;}
        if(!number(arg+3,&value)) {parse_error(PARSE_FORMAT,arg); return 255;}
        if(!value || value>65535) {parse_error(PARSE_RANGE,arg); return 255;}
        if(c=='F') f=1; else l=1;
    }
    if(dos_installed(DOS_INSTALLED_SHARE,NULL,&before)) return 255;
    if(before) {to_stderr(1); print("SHARE already installed\n"); to_stderr(0); return 255;}
    dos_installed(DOS_INSTALLED_SHARE,&one,NULL);
    return 0;
}
