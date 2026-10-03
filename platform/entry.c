/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <efi.h>
EFI_STATUS efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);
/* GNU-EFI's IA-64 crt0 relocates this image before calling _entry. */
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) { return efi_main(image,st); }
