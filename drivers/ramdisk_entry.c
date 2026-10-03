/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../apps/runtime.h"
#include "ramdisk.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    DosBlockSpec block; DosDeviceSpec control; ramdisk_specs(&block,&control);
    if(dos_device_register(&control) || dos_block_register(&block)) return EFI_LOAD_ERROR;
    return EFI_SUCCESS;
}
