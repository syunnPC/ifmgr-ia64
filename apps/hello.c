/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    DosRegs r={.ax=0x3000}; app_call(&r);
    print("Hello from an IA-64 program. DOS API version %u.%02u\n",
          (unsigned long long)(r.ax&255),(unsigned long long)((r.ax>>8)&255));
    print("Command tail: %s\n",app_dos->command_tail());
    return EFI_SUCCESS;
}
