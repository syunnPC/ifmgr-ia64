/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_RAMDISK_H
#define DOS_RAMDISK_H
#include "dos_block.h"
#include "dos_device.h"
#define RAMDISK_MAX_UNITS 8U
#define RAMDISK_RESET 0x8100U /* RAMCTL, DI unit: discard contents and create FAT. */
#define RAMDISK_PROTECT 0x8101U /* RAMCTL, DI unit, DX u32 readonly, SI 4. */
typedef struct {u32 size,units,pages,reserved; u32 generation[RAMDISK_MAX_UNITS];} RamDiskStats;
/* AH=4404h on a RAM drive reads RamDiskUnitInfo; AH=4405h writes a u32
 * read-only flag (same effect as RAMDISK_PROTECT for that unit). */
typedef struct {u32 size,unit,generation,readonly; u64 sectors;} RamDiskUnitInfo;
#endif
