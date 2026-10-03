/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    /* Deliberately leave a block allocated: EXEC must reclaim child ownership. */
    DosRegs r={.ax=0x4800,.bx=127}; if(app_call(&r)) return EFI_OUT_OF_RESOURCES;
    r=(DosRegs){.ax=0x4c25}; app_call(&r);
    return EFI_ABORTED;
}
