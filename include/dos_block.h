/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_BLOCK_API_H
#define DOS_BLOCK_API_H
#include "io.h"
#define DOS_BLOCK_ABI 1U
enum {DOS_BLOCK_INIT=0,DOS_BLOCK_MEDIA=1,DOS_BLOCK_IOCTL_READ=3,DOS_BLOCK_READ=4,DOS_BLOCK_WRITE=8,
      DOS_BLOCK_FLUSH=11,DOS_BLOCK_IOCTL_WRITE=12,DOS_BLOCK_FINISH=256};
typedef struct {
    u32 size,generation,flags,sector_bytes;
    u64 sectors;
    u32 readonly,reserved;
} DosBlockMedia;
typedef struct {
    u32 size,command,pid,unit;
    u32 generation,count,transferred,units;
    u64 sector;
    void *buffer;
    const IoServices *io;
    const char *arguments;
} DosBlockRequest;
typedef struct {
    u32 version,size;
    char name[9]; u8 reserved[7];
    void *context;
    int (*request)(void *,DosBlockRequest *);
} DosBlockSpec;
typedef struct {
    u32 size,index,units,drive_mask;
    char name[9]; u8 reserved[7];
} DosBlockInfo;
/* Registration is permitted only in a DEVICE entry shim. INIT returns 1..26
 * units; all become visible together after initialization. MEDIA returns a
 * DosBlockMedia in buffer, with transferred == sizeof(DosBlockMedia). Flags
 * are IO_DISK_PRESENT/REMOVABLE; a missing medium is a successful MEDIA query.
 * Logical sectors are 512 bytes. READ/WRITE count and transferred are sectors.
 * IOCTL_READ/IOCTL_WRITE (AH=4404h/4405h) carry count/transferred in bytes,
 * with generation set to the last MEDIA result; unsupported commands return
 * DE_FUNCTION.
 * Each READ/WRITE/FLUSH MUST validate generation before accessing the medium,
 * including rollback writes. Generations are nonzero, increase on replacement,
 * removal or geometry/protection changes, and must never wrap or be reused.
 * FINISH releases resources even after failed INIT. Callbacks may use IO.SYS
 * but cannot call DOS. DOS owns FAT and drive letters; the name is diagnostic,
 * not a character device or filename. Block and character specs may coexist
 * in one module, with character INIT before block INIT and reverse FINISH.
 */
#endif
