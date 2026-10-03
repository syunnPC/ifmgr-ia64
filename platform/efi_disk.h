/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_EFI_DISK_H
#define DOS_EFI_DISK_H
#include "efi_support.h"
#include "io.h"
int efi_disks_init(EFI_SYSTEM_TABLE *,EFI_HANDLE,Disk *);
void efi_disks_close(void);
u32 efi_disk_count(void *);
int efi_disk_info(void *,u32,IoDiskInfo *);
u32 efi_physical_count(void *);
int efi_physical_info(void *,u32,IoDiskInfo *);
int efi_disk_location(void *,u32,u32 *,u64 *);
#endif
