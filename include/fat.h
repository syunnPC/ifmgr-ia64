/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_FAT_H
#define DOS_FAT_H
#include "dos_api.h"
struct FatTxPage;
typedef struct {u32 operation,sector,attempt,flags; int error;} FatIoError;
typedef unsigned (*FatErrorHandler)(void *,const FatIoError *);
typedef struct {
    Disk disk;
    u32 total, fat_start, fat_sectors, root_start, root_entries, data_start, clusters;
    u8 spc, copies, bits;
    u8 cache[512]; u32 cache_sector; int cache_valid;
    struct FatTxPage *tx_first,*tx_last;
    unsigned tx_depth;
    int tx_error,faulted;
    FatErrorHandler error_handler;
    void *error_context;
} Fat;
typedef struct {u8 raw[32]; u32 sector; u16 offset;} Node;
typedef struct {Node node; u32 pos;} FatFile;
/* A boot sector's BPB as FAT mounts it, on a disk of that many sectors;
 * DE_FORMAT otherwise. Sectors from the volume's start, bits 12 or 16. */
typedef struct {u32 spc,reserved,fats,root_entries,fat_sectors,total,root_start,data_start,clusters,bits;} FatBpb;
int fat_parse_bpb(const u8 *,u64,FatBpb *);
int fat_mount(Fat *, const Disk *);
int fat_mount_ex(Fat *,const Disk *,FatErrorHandler,void *);
int fat_get(Fat *, u32, u16 *);
int fat_set(Fat *, u32, u16);
int fat_eof(const Fat *, u16);
int fat_name83(const char *, u8[11]);
void fat_name(const Node *, char[13]);
u32 fat_size(const Node *);
u16 fat_cluster(const Node *);
int fat_next(Fat *, u16, u32 *, Node *);
int fat_lookup(Fat *, const char *, Node *);
int fat_create(Fat *, const char *, u8, Node *);
int fat_remove(Fat *, const char *, int);
int fat_rename(Fat *, const char *, const char *);
int fat_change_entry(Fat *,u16,const Node *,const u8 *);
int fat_create_label(Fat *,const u8[11],Node *);
int fat_read(Fat *, FatFile *, void *, u32, u32 *);
int fat_write(Fat *, FatFile *, const void *, u32, u32 *);
int fat_truncate(Fat *, FatFile *, u32);
int fat_replace(Fat *, FatFile *, u8);
int fat_sync_node(Fat *, Node *);
int fat_free_space(Fat *, u32 *);
void platform_fat_time(u16 *, u16 *);
int platform_fat_page(void **);
void platform_fat_free_page(void *);
#endif
