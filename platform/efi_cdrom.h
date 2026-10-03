/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_EFI_CDROM_H
#define DOS_EFI_CDROM_H
#include "efi_support.h"
#include "io.h"
void efi_cdrom_init(EFI_SYSTEM_TABLE *,IoServices *);
void efi_cdrom_close(void);
#endif
