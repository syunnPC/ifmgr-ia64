/* SPDX-License-Identifier: GPL-2.0-or-later
 * Low-level files: a handle is KERNEL's HFILE (_lopen and the like). */
#ifndef _IO_H
#define _IO_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h> /* as Open Watcom's io.h */
int open(const char *,int,...);
int creat(const char *,int);
int read(int,void *,unsigned);
int write(int,const void *,unsigned);
int close(int);
long lseek(int,long,int);
long tell(int);
int eof(int);
long filelength(int);
int access(const char *,int);
int unlink(const char *);
int setmode(int,int);
int isatty(int);
int chsize(int,long);
/* access's modes. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _open open
#define _creat creat
#define _read read
#define _write write
#define _close close
#define _lseek lseek
#define _tell tell
#define _eof eof
#define _filelength filelength
#define _access access
#define _unlink unlink
#define _setmode setmode
#define _isatty isatty
#define _chsize chsize
#endif
