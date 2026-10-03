/* SPDX-License-Identifier: GPL-2.0-or-later
 * HIMEM.SYS: DEVICE=HIMEM.SYS [/HMAMIN=m] [/NUMHANDLES=n], as Windows 3.0's
 * XMS driver is configured. VDM then gives the 8086 programs it runs XMS
 * 3.0 (vdm/xms.c): extended memory blocks below 4 GiB and, where IO.SYS
 * holds it, the high memory area; /HMAMIN is the least a program must ask
 * for to get it (0-63 KiB), /NUMHANDLES the handles (1-128, 32 when not
 * given). /MACHINE:, /SHADOWRAM: and /CPUCLOCK: concern a PC's hardware and
 * are taken and left alone. Without the IA-32 instruction set there is no
 * VDM and the driver does not load.
 */
#include "../apps/runtime.h"
/* DOS takes no handle calls while DEVICE= loads: messages go to the
 * console through IO.SYS. */
static const IoServices *console_io;
static void to_console(const void *p,size_t n) {console_io->console_write(console_io->context,p,n);}
static int number(const char *p,const char **end,unsigned *out) {
    unsigned v=0,n=0;
    while(*p>='0' && *p<='9' && n<6) {v=v*10+(unsigned)(*p++-'0'); n++;}
    *end=p; *out=v; return n>0;
}
static int keyword(const char *p,const char *word) {
    while(*word) if(upper(*p++)!=*word++) return 0;
    return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const IoServices *io=dos_io_services(); const char *p=app_dos->command_tail(); unsigned hma_min=0,handles=32;
    if(io) {console_io=io; print_sink=to_console;}
    for(;;) {
        while(*p==' ' || *p=='\t') p++;
        if(!*p) break;
        const char *start=p,*end; unsigned v;
        if(keyword(p,"/HMAMIN=") && number(p+8,&end,&v) && v<=63 && (!*end || *end==' ' || *end=='\t')) {hma_min=v; p=end; continue;}
        if(keyword(p,"/NUMHANDLES=") && number(p+12,&end,&v) && v>=1 && v<=128 && (!*end || *end==' ' || *end=='\t')) {handles=v; p=end; continue;}
        if(keyword(p,"/MACHINE:") || keyword(p,"/SHADOWRAM:") || keyword(p,"/CPUCLOCK:")) {while(*p && *p!=' ' && *p!='\t') p++; continue;}
        while(*p && *p!=' ' && *p!='\t') p++;
        print("HIMEM: Invalid parameter - ");
        for(;start<p;start++) print("%c",*start);
        print("\n"); return EFI_INVALID_PARAMETER;
    }
    if(!io || !(io->capabilities&IO_CAP_IA32) || io->size<offsetof(IoServices,range_free)+sizeof(io->range_free) || !io->alloc_pages_range) {
        print("HIMEM: no IA-32 instruction set for 8086 programs; XMS is not installed\n");
        return EFI_UNSUPPORTED;
    }
    if(app_dos->size<offsetof(DosApi,installed)+sizeof(app_dos->installed) || !app_dos->installed) {
        print("Incorrect DOS version\n"); return EFI_INCOMPATIBLE_VERSION;
    }
    u32 value=DOS_XMS_ON|hma_min<<8|handles<<16;
    if(app_dos->installed(DOS_INSTALLED_XMS,&value,NULL)) return EFI_UNSUPPORTED;
    print("HIMEM: XMS 3.0 for 8086 programs, %u handles\n",(unsigned long long)handles);
    print(io->capabilities&IO_CAP_HMA?"64K High Memory Area available\n":"High Memory Area unavailable\n");
    return EFI_SUCCESS;
}
