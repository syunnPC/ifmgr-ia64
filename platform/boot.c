/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "efi_support.h"
#include "io.h"
static EFI_SYSTEM_TABLE *system_table;
void con_write(const void *data,size_t n) {efi_output(system_table,data,n);}
void con_puts(const char *s) {con_write(s,strlen(s));}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    system_table=st;
    /* Started again from DOS: there is one IO.SYS. */
    EFI_GUID io_guid=IO_SERVICES_GUID; void *running=NULL;
    if(!EFI_ERROR(st->BootServices->LocateProtocol(&io_guid,NULL,&running))) {
        con_puts("DOS bootstrap: DOS is already running\n");
        return (EFI_STATUS)1;
    }
    st->BootServices->SetWatchdogTimer(0,0,0,NULL);
    EFI_HANDLE io;
    EFI_STATUS e=efi_load_sibling(st,image,(const CHAR16 *)u"\\IO.SYS",&io);
    if(!EFI_ERROR(e)) e=st->BootServices->StartImage(io,NULL,NULL);
    if(EFI_ERROR(e)) print("IO.SYS boot failure: EFI %x\n",(unsigned long long)e);
    return e;
}
