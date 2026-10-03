/* SPDX-License-Identifier: GPL-2.0-or-later
 * Microsoft C's DOS functions over KERNEL. There are no segments: FP_SEG is
 * 0 and FP_OFF the whole address. */
#ifndef _DOS_H
#define _DOS_H
#include <stddef.h>
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_VOLID 0x08
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
struct find_t {char reserved[21]; char attrib; unsigned short wr_time,wr_date; long size; char name[13];};
struct _dosdate_t {unsigned char day,month; unsigned short year; unsigned char dayofweek;};
struct _dostime_t {unsigned char hour,minute,second,hsecond;};
struct _diskfree_t {unsigned total_clusters,avail_clusters,sectors_per_cluster,bytes_per_sector;};
#define dosdate_t _dosdate_t
#define dostime_t _dostime_t
#define diskfree_t _diskfree_t
unsigned _dos_findfirst(const char *,unsigned,struct find_t *);
unsigned _dos_findnext(struct find_t *);
unsigned _dos_open(const char *,unsigned,int *);
unsigned _dos_creat(const char *,unsigned,int *);
unsigned _dos_close(int);
unsigned _dos_read(int,void *,unsigned,unsigned *);
unsigned _dos_write(int,const void *,unsigned,unsigned *);
void _dos_getdate(struct _dosdate_t *);
void _dos_gettime(struct _dostime_t *);
unsigned _dos_getdiskfree(unsigned,struct _diskfree_t *);
void _dos_getdrive(unsigned *);
void _dos_setdrive(unsigned,unsigned *);
unsigned _dos_getfileattr(const char *,unsigned *);
unsigned _dos_setfileattr(const char *,unsigned);
#define FP_SEG(p) 0
#define FP_OFF(p) ((unsigned long)(size_t)(p))
#define MK_FP(s,o) ((void *)(size_t)(o))
#endif
