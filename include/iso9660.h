/* SPDX-License-Identifier: GPL-2.0-or-later
 * ISO 9660 for MSCDEX: the primary volume descriptor, directories and file
 * data, read through a 2048-byte sector callback. Names become DOS 8.3 (the
 * ";1" version and a trailing dot are dropped); entries that are not valid
 * 8.3 names, and multi-extent files, are skipped. Joliet and Rock Ridge are
 * not used.
 */
#ifndef DOS_ISO9660_H
#define DOS_ISO9660_H
#include "base.h"
#define ISO_SECTOR 2048U
typedef int (*IsoRead)(void *context,u32 sector,u32 count,void *buffer);
typedef struct {
    IsoRead read; void *context;
    u32 volume_sectors,root_sector,root_size;
    char label[12];
    u32 cached; /* sector in buffer, UINT32_MAX for none */
    u8 buffer[ISO_SECTOR];
} IsoVolume;
typedef struct {u32 sector,size; u16 date,time; u8 attributes; char name[13];} IsoEntry;
int iso_mount(IsoVolume *,IsoRead,void *);
/* Next entry of a directory extent; *offset (bytes) starts at 0. DE_NOMORE at the end. */
int iso_next(IsoVolume *,u32 sector,u32 size,u32 *offset,IsoEntry *);
/* "\\" is the root; components are upper-case 8.3 names. */
int iso_lookup(IsoVolume *,const char *path,IsoEntry *);
int iso_read(IsoVolume *,const IsoEntry *,u32 offset,void *buffer,u32 count,u32 *done);
#endif
