/* SPDX-License-Identifier: GPL-2.0-or-later
 * The C run-time library (win/crt): error numbers, as Microsoft C's. */
#ifndef _ERRNO_H
#define _ERRNO_H
extern int errno;
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define EDOM 33
#define ERANGE 34
#endif
