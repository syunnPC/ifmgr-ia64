/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * GRAFTABL: native counterpart of MS-DOS 4 CMD/GRAFTABL.
 *   GRAFTABL [437 | 850 | 860 | 863 | 865] | /STA | ?
 * Chooses the code page whose characters 128-255 8086 programs see in CGA
 * graphics modes (VDM's video BIOS holds the tables; INT 2Fh B000h answers
 * once GRAFTABL has run). Without a code page the active one is taken when
 * it is one of these, else 437. /STA shows the code page in effect and ?
 * the parameters. Messages follow v4.0 GRAFTABL.SKL; the exit code is 0
 * for a first table, 1 for a replaced one, 2 for status without one, 3 for
 * a parameter error.
 */
#include "util.h"
static const u32 pages[]={437,850,860,863,865};
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[32]; const char *p=app_dos->command_tail(); u32 page=0,before=0; int status_only=0,any=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(any++) {parse_error(PARSE_TOO_MANY,arg); return 3;}
        if(!strcmp(arg,"?")) {
            print("\nDOS command line parameters supported:\n\n    /STA - Request Status only\n    ?    - Display this summary of parameters\n\n"
                  "    Code Pages available:\n    437  - USA Graphic Character Set\n    850  - Multi-lingual Graphic Character Set\n"
                  "    860  - Portuguese Graphic Character Set\n    863  - Canadian French Graphic Character Set\n    865  - Nordic Graphic Character Set\n");
            return 0;
        }
        if(arg[0]=='/') {
            if(upper(arg[1])!='S' || upper(arg[2])!='T' || upper(arg[3])!='A' || arg[4]) {parse_error(PARSE_SWITCH,arg); return 3;}
            status_only=1; continue;
        }
        for(const char *d=arg;*d;d++) {if(*d<'0' || *d>'9' || page>9999) {parse_error(PARSE_PARAMETER,arg); return 3;} page=page*10+(u32)(*d-'0');}
        unsigned i=0; while(i<ARRAY_SIZE(pages) && pages[i]!=page) i++;
        if(i==ARRAY_SIZE(pages)) {parse_error(PARSE_VALUE,arg); return 3;}
    }
    dos_installed(DOS_INSTALLED_GRAFTABL,NULL,&before);
    if(status_only) {
        if(before) print("Active Code Page: %u\n",(unsigned long long)before); else print("Active Code Page: None\n");
        return before?0:2;
    }
    if(!page) {
        u16 active=0,system=0; page=437;
        if(!dos_code_page(&active,&system)) for(unsigned i=0;i<ARRAY_SIZE(pages);i++) if(pages[i]==active) page=active;
    }
    if(before) print("Previous Code Page: %u\n",(unsigned long long)before);
    dos_installed(DOS_INSTALLED_GRAFTABL,&page,NULL);
    print("Active Code Page: %u\n",(unsigned long long)page);
    return before?1:0;
}
