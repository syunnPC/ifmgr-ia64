/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFREG 0100000
#define S_IREAD 0000400
#define S_IWRITE 0000200
#define S_IEXEC 0000100
/* POSIX's permissions, as Open Watcom has them; files have only S_IWRITE's. */
#define S_IRUSR S_IREAD
#define S_IWUSR S_IWRITE
#define S_IXUSR S_IEXEC
#define S_IRWXU (S_IRUSR|S_IWUSR|S_IXUSR)
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXG (S_IRGRP|S_IWGRP|S_IXGRP)
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001
#define S_IRWXO (S_IROTH|S_IWOTH|S_IXOTH)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISCHR(m) (((m)&S_IFMT)==S_IFCHR)
#define _S_IFMT S_IFMT
#define _S_IFDIR S_IFDIR
#define _S_IFCHR S_IFCHR
#define _S_IFREG S_IFREG
#define _S_IREAD S_IREAD
#define _S_IWRITE S_IWRITE
#define _S_IEXEC S_IEXEC
struct stat {_dev_t st_dev; _ino_t st_ino; unsigned short st_mode; short st_nlink,st_uid,st_gid; _dev_t st_rdev; _off_t st_size; time_t st_atime,st_mtime,st_ctime;};
#define _stat stat
int stat(const char *,struct stat *);
int fstat(int,struct stat *);
#define _fstat fstat
#endif
