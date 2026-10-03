/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../apps/runtime.h"
#include "ports.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    for(unsigned i=0;i<PORT_DEVICE_COUNT;i++) {
        DosDeviceSpec spec; port_device_spec(i,&spec);
        if(dos_device_register(&spec)) return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}
