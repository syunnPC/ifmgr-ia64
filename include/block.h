/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_BLOCK_PRIVATE_H
#define DOS_BLOCK_PRIVATE_H
#include "dos_block.h"
void block_reset(void);
int block_register(unsigned,const DosBlockSpec *);
int block_initialize(unsigned,const IoServices *,const char *,unsigned *);
void block_publish(unsigned);
void block_cancel(unsigned);
int block_finish(unsigned);
int block_pending(unsigned);
unsigned block_units(unsigned,u32[26]);
int block_media(u32,IoDiskInfo *);
int block_ioctl_transfer(u32,int,void *,u32,u32 *);
void block_drive(u32,unsigned);
int block_info(u32,DosBlockInfo *);
int dos_block_request(const DosBlockSpec *,DosBlockRequest *);
int dos_attach_block_drives(unsigned);
int dos_detach_block_drives(unsigned);
#endif
