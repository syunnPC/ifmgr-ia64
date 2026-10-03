/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DIRECT_H
#define _DIRECT_H
#include <stddef.h>
char *getcwd(char *,int);
int chdir(const char *);
int mkdir(const char *);
int rmdir(const char *);
int _getdrive(void);
int _chdrive(int);
#define _getcwd getcwd
#define _chdir chdir
#define _mkdir mkdir
#define _rmdir rmdir
#include <dirent.h>
#endif
