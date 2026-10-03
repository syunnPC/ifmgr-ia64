/* SPDX-License-Identifier: GPL-2.0-or-later
 * EMM386.SYS: DEVICE=EMM386.SYS [memory] [FRAME=9000|/P9000], as Windows
 * 3.0's expanded memory manager is configured. VDM then gives the 8086
 * programs it runs LIM EMS 4.0 (vdm/ems.c) with memory kilobytes of
 * expanded memory (16-32768 in 16 KiB pages, 256 when not given). The page
 * frame is 9000h, the top 64 KiB of conventional memory, which programs
 * then do without: C0000h-EFFFFh, where a PC's frame goes, is no memory on
 * Itanium machines, so a FRAME or /P naming another segment is an invalid
 * parameter. Without the IA-32 instruction set the driver does not load.
 */
#include "../apps/runtime.h"
/* DOS takes no handle calls while DEVICE= loads: messages go to the
 * console through IO.SYS. */
static const IoServices *console_io;
static void to_console(const void *p,size_t n) {console_io->console_write(console_io->context,p,n);}
static int keyword(const char *p,const char *word) {
    while(*word) if(upper(*p++)!=*word++) return 0;
    return 1;
}
static int word_end(const char *p) {return !*p || *p==' ' || *p=='\t';}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const IoServices *io=dos_io_services(); const char *p=app_dos->command_tail(); unsigned kb=256; int have_kb=0;
    if(io) {console_io=io; print_sink=to_console;}
    for(;;) {
        while(*p==' ' || *p=='\t') p++;
        if(!*p) break;
        const char *start=p;
        if(*p>='0' && *p<='9' && !have_kb) {
            unsigned v=0,n=0; while(*p>='0' && *p<='9' && n<6) {v=v*10+(unsigned)(*p++-'0'); n++;}
            if(word_end(p) && v>=16 && v<=32768) {kb=v-v%16; have_kb=1; continue;}
        } else if(keyword(p,"FRAME=9000") && word_end(p+10)) {p+=10; continue;}
        else if(keyword(p,"/P9000") && word_end(p+6)) {p+=6; continue;}
        while(*p && *p!=' ' && *p!='\t') p++;
        print("EMM386: Invalid parameter - ");
        for(;start<p;start++) print("%c",*start);
        print("\n"); return EFI_INVALID_PARAMETER;
    }
    if(!io || !(io->capabilities&IO_CAP_IA32)) {
        print("EMM386: no IA-32 instruction set for 8086 programs; EMS is not installed\n");
        return EFI_UNSUPPORTED;
    }
    if(app_dos->size<offsetof(DosApi,installed)+sizeof(app_dos->installed) || !app_dos->installed) {
        print("Incorrect DOS version\n"); return EFI_INCOMPATIBLE_VERSION;
    }
    u32 value=kb;
    if(app_dos->installed(DOS_INSTALLED_EMS,&value,NULL)) return EFI_UNSUPPORTED;
    print("EMM386: LIM EMS 4.0 for 8086 programs, %uK, page frame at 9000h\n",(unsigned long long)kb);
    return EFI_SUCCESS;
}
