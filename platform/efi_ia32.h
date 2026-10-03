/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_EFI_IA32_H
#define DOS_EFI_IA32_H
#include "efi_support.h"
#include "io.h"
void efi_ia32_init(EFI_SYSTEM_TABLE *,IoServices *,u64 itc_per_ms); /* 0: no time limits */
void efi_ia32_close(void);
#endif
