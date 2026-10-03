/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EFI_PORTS_H
#define EFI_PORTS_H
#include "efi_support.h"
#include "io.h"
void efi_ports_init(EFI_SYSTEM_TABLE *,IoServices *);
void efi_ports_close(void);
#endif
