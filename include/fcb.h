/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_FCB_PRIVATE_H
#define DOS_FCB_PRIVATE_H
#include "dos.h"
/* FCBs own private SFT references, never application handle-table entries. */
typedef struct {u32 size; u16 date,time; unsigned drive,device;} FcbInfo;
void fcb_reset(void);
int fcb_reap(u32);
int fcb_call(DosRegs *,void *,u32);
int fcb_ref_open(const char *,u8,int,unsigned *,FcbInfo *);
int fcb_ref_close(unsigned,u32,const FcbInfo *,const FcbInfo *);
int fcb_ref_io(unsigned,int,u32,void *,u32,u32 *,FcbInfo *);
int fcb_path_info(const char *,u8,FcbInfo *);
int fcb_find(unsigned,u8,const u8[11],DosFind *,Node *,int);
int fcb_mutate(const DosFind *,const Node *,const u8 *);
int fcb_drive_exists(unsigned);
int fcb_label_create(unsigned,const u8[11],FcbInfo *);
#endif
