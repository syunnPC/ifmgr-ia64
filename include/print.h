/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PRINT_H
#define PRINT_H
#include "dos_api.h"
/* PRINT's resident part (kernel/print.c). */
void print_reset(void);
void print_tick(void);  /* at a DOS call */
u32 print_idle(void);   /* while waiting: ms until the next turn, 0 with nothing to print */
int print_request(u32,DosPrintRequest *);
/* MSDOS.SYS's files for it (dos.c): outside every program's handles, with
 * critical errors failed and the current program's error state kept
 * between begin and end. */
int dos_spool_begin(void);
void dos_spool_end(void);
int dos_spool_open(const char *path,int device,unsigned *reference);
int dos_spool_read(unsigned,void *,u32,u32 *);
int dos_spool_write(unsigned,const void *,u32,u32 *); /* a device's, without waiting */
void dos_spool_close(unsigned);
#endif
