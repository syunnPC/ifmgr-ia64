/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_EFI_CLOCK_H
#define DOS_EFI_CLOCK_H
#include <efi.h>
#include "io.h"
void efi_clock_init(EFI_RUNTIME_SERVICES *,IoServices *);
#endif
