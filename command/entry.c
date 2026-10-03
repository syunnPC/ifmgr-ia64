/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../apps/runtime.h"
#include "command.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    if(app_dos->size<sizeof(*app_dos)) return EFI_UNSUPPORTED;
    return shell(app_dos->command_tail());
}
