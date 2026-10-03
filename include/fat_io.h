/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_FAT_IO_H
#define DOS_FAT_IO_H
#include "fat.h"
/* Internal sector transactions. Nested operations join the outer operation.
 * Preimages live in pages supplied by IO.SYS; no firmware types enter FAT.
 * This handles reported I/O failures, not power loss: no on-disk journal exists.
 * A failed rollback latches faulted until the volume is checked and remounted. */
int fat_begin(Fat *);
int fat_end(Fat *,int);
int fat_sector_read(Fat *,u32,void *);
int fat_sector_write(Fat *,u32,const void *);
int fat_data_read(Fat *,u32,void *);
int fat_io_read(Fat *,u32,void *,u32);
int fat_io_flush(Fat *,u32);
/* System-wide VERIFY (AH=2Eh/54h): read back and compare each committed
 * sector. A mismatch is a write error and follows the critical-error path. */
extern int fat_verify_writes;
#endif
