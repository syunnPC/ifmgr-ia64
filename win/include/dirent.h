/* SPDX-License-Identifier: GPL-2.0-or-later
 * Open Watcom's directory reading: opendir takes a directory or a pattern;
 * the entry readdir returns is the DIR itself. */
#ifndef _DIRENT_H
#define _DIRENT_H
typedef struct dirent {
    char d_attr; unsigned short d_time,d_date; long d_size; char d_name[13];
    void *d_search; int d_first; char d_pattern[260];
} DIR;
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
void rewinddir(DIR *);
#endif
