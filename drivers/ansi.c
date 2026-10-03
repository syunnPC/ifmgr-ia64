/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * ANSI.SYS: DEVICE=ANSI.SYS [/X] [/K] [/L], as MS-DOS 4's DEV/ANSI. While
 * it loads it sets DOS_INSTALLED_ANSI, and from then on MSDOS.SYS itself
 * reads the escape sequences in CON output and reassigns keys
 * (kernel/ansi.c). /X tells the 101-key keyboard's extended keys from their
 * counterparts for reassignment, /K takes the keyboard as an 84-key one
 * and /L keeps the screen's rows across mode changes; they are kept for
 * INT 2Fh's 1A02h. Anything else is an invalid parameter, shown from there
 * to the end of the line as DOS 4's is, and the driver is not loaded.
 */
#include "../apps/runtime.h"
/* DOS takes no handle calls while DEVICE= loads: messages go to the
 * console through IO.SYS. */
static const IoServices *console_io;
static void to_console(const void *p,size_t n) {console_io->console_write(console_io->context,p,n);}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *p=app_dos->command_tail(); u32 options=DOS_ANSI_ON;
    if((console_io=dos_io_services())!=NULL) print_sink=to_console;
    for(;;) {
        while(*p==' ' || *p=='\t' || *p==',' || *p==';' || *p=='=') p++;
        if(!*p) break;
        char c=p[0]=='/'?(char)upper(p[1]):0;
        u32 bit=c=='X'?DOS_ANSI_X:c=='K'?DOS_ANSI_K:c=='L'?DOS_ANSI_L:0;
        if(!bit || (p[2] && p[2]!=' ' && p[2]!='\t' && p[2]!='/' && p[2]!=',' && p[2]!=';' && p[2]!='=')) {
            print("Invalid parameter - %s\n",p); return EFI_INVALID_PARAMETER;
        }
        options|=bit; p+=2;
    }
    if(app_dos->size<offsetof(DosApi,installed)+sizeof(app_dos->installed) || !app_dos->installed) {
        print("Incorrect DOS version\n"); return EFI_INCOMPATIBLE_VERSION;
    }
    /* Refused without a text screen, or outside DEVICE=. */
    return app_dos->installed(DOS_INSTALLED_ANSI,&options,NULL)?EFI_UNSUPPORTED:EFI_SUCCESS;
}
