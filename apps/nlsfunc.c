/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * NLSFUNC: native counterpart of MS-DOS 4 CMD/NLSFUNC.
 *   NLSFUNC [[d:][path]filename]
 * Code page switching (CHCP, AH=6602h) is MSDOS.SYS's own; NLSFUNC checks
 * that the country information file named is there and counts as installed
 * from its first run (DosApi installed), after which it says "NLSFUNC
 * already installed" (v4.0 USA-MS.MSG COMMON2).
 */
#include "util.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[DOS_PATH_MAX],file[DOS_PATH_MAX]=""; const char *p=app_dos->command_tail(); u32 one=1,before=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/') {parse_error(PARSE_SWITCH,arg); return 1;}
        if(file[0]) {parse_error(PARSE_TOO_MANY,arg); return 1;}
        strcopy(file,sizeof(file),arg);
    }
    if(file[0]) {DosFind f; if(dos_find_first(file,0,&f)) {extended_error(DE_NOFILE,file); return 2;}}
    if(dos_installed(DOS_INSTALLED_NLSFUNC,NULL,&before)) return 1;
    if(before) {to_stderr(1); print("NLSFUNC already installed\n"); to_stderr(0); return 0x80;}
    dos_installed(DOS_INSTALLED_NLSFUNC,&one,NULL);
    return 0;
}
