/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <efi.h>
#include "dos.h"
/* The only EFI-specific entry shim linked into MSDOS.SYS. */
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_GUID guid=IO_SERVICES_GUID; IoServices *io;
    EFI_STATUS e=st->BootServices->LocateProtocol(&guid,NULL,(void **)&io);
    if(EFI_ERROR(e)) return e;
    int result=dos_run(io);
    return result?EFI_LOAD_ERROR:EFI_SUCCESS;
}
