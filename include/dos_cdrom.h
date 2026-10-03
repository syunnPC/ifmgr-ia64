/* SPDX-License-Identifier: GPL-2.0-or-later
 * CD-ROM device drivers (EFICD.SYS): a character device, by default MSCD001,
 * whose IOCTL input (AH=4402h, DOS_DEV_IOCTL_READ) takes a DosCdControl.
 * DOS_CD_ENTRY hands back the driver's request routine so that a CD file
 * system (MSCDEX) can call it from inside DOS without reentering DOS, as
 * MSCDEX calls a CD driver's strategy routine. Sectors are 2048 bytes.
 */
#ifndef DOS_CDROM_H
#define DOS_CDROM_H
#include "dos_device.h"
#define DOS_CD_ABI 1U
enum {DOS_CD_STATUS=1,DOS_CD_READ=2,DOS_CD_ENTRY=3};
typedef struct {
    u32 size,code,unit,units;               /* units: drives this device serves */
    u32 generation,flags,sector_size,abi;   /* STATUS; flags: IO_DISK_PRESENT */
    u64 sectors;                            /* STATUS: capacity */
    u64 sector; u32 count,transferred;      /* READ, at the generation given */
    void *buffer;
    int (*request)(void *,DosDeviceRequest *); void *context; /* ENTRY */
} DosCdControl;
#endif
