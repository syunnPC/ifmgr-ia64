/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EFI_SUPPORT_H
#define EFI_SUPPORT_H
#include <efi.h>
#include "base.h"
void efi_output(EFI_SYSTEM_TABLE *,const void *,size_t);
void efi_output_text(EFI_SYSTEM_TABLE *,const u16 *,size_t);
EFI_STATUS efi_load_sibling(EFI_SYSTEM_TABLE *,EFI_HANDLE,const CHAR16 *,EFI_HANDLE *);
int efi_dos_error(EFI_STATUS);
unsigned efi_path_bytes(const void *); /* a device path's bytes before its end node; 0 when malformed */
#endif
