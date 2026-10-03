/* SPDX-License-Identifier: GPL-2.0-or-later
 * Redirected drives (DOS_CAP_REDIRECT): a read-only file system outside DOS,
 * such as MSCDEX's ISO 9660, serving one drive letter. DOS keeps handles,
 * positions, searches and current directories; the redirector supplies
 * directory entries and file data. Callbacks run inside DOS and must not
 * reenter it. Paths are canonical and drive-relative ("\\" is the root,
 * "\\WIN\\SETUP.EXE"), upper case 8.3. Writes, creation, deletion, renaming
 * and attribute or time changes fail with DE_ACCESS; FAT-level functions
 * (disk I/O, DPBs, IOCTL 440Dh, FCB opens, sizes and changes) fail with
 * DE_REMOTE. FCB searches (11h/12h) are served.
 */
#ifndef DOS_REDIR_H
#define DOS_REDIR_H
#include "base.h"
typedef struct {
    u32 size;                 /* file bytes */
    u16 date,time;            /* DOS format */
    u8 attributes,reserved;   /* FA_* */
    u16 directory;            /* for directories: nonzero handle for next() */
    char name[13]; u8 padding[3];
} DosRedirEntry;
typedef struct DosRedirector {
    u32 size,reserved; void *context;
    int (*stat)(void *,const char *path,DosRedirEntry *);
    int (*open)(void *,const char *path,u64 *file,DosRedirEntry *);
    int (*read)(void *,u64 file,u32 offset,void *buffer,u32 count,u32 *done);
    int (*close)(void *,u64 file);
    /* The entries of a directory in order; *index starts at 0 and DE_NOMORE
     * ends. The root lists the volume label first, as FA_VOLUME. */
    int (*next)(void *,u16 directory,u32 *index,DosRedirEntry *);
    int (*space)(void *,u32 *clusters,u32 *free_clusters,u32 *cluster_bytes);
    /* Nonzero, and different for every medium; zero while none is ready.
     * A change invalidates open files, searches and current directories. */
    u32 (*generation)(void *);
} DosRedirector;
#endif
