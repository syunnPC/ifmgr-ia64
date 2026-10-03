/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shared raw-volume helpers for FORMAT, CHKDSK, SYS, LABEL, FDISK, DISKCOPY,
 * DISKCOMP and RECOVER (the labels and serial numbers for TREE and BACKUP too).
 * Volumes are read through native INT 25h/26h; nothing here mounts FAT.
 */
#ifndef APP_MAINT_H
#define APP_MAINT_H
#include "runtime.h"
typedef struct {
    unsigned drive;
    u32 total,reserved,fats,spf,root_entries,root_start,root_sectors,data_start;
    u32 spc,clusters,bits;
    u8 media,boot[512];
    u8 *fat; /* One FAT copy, (clusters+2) entries, owned by the caller task. */
} Volume;
int volume_boot(unsigned,Volume *);
int volume_fat(Volume *,unsigned);
int volume_save_fat(Volume *);
void volume_free(Volume *);
u32 fat_value(const Volume *,u32);
void fat_store(Volume *,u32,u32);
int fat_end(const Volume *,u32);
u32 fat_bad(const Volume *);
u32 fat_eof(const Volume *);
u32 cluster_lba(const Volume *,u32);
int volume_read(const Volume *,u32,u32,void *);
int volume_write(const Volume *,u32,u32,const void *);
/* Console helpers: messages go to standard output like DOS utilities. */
int ask_yes_no(const char *);
int read_text(const char *,char *,unsigned);
int drive_argument(const char *,unsigned *);
/* DOS 4's refusal of a SUBST or ASSIGNed letter, or a network drive
 * (USA-MS.MSG COMMON14 and COMMON12); nonzero after saying so. */
int refuse_mapped(unsigned drive,const char *utility);
void serial_number(u32,char[10]);
u32 new_serial(void);
void fat_timestamp(u16 *,u16 *);
void date_text(u16,u16,char[24]);
int same_text(const char *,const char *);
char *next_word(char **);
/* Volume labels use the DOS 4 LABEL character rules; out is blank-padded. */
int label_name(const char *,u8[11]);
int label_get(unsigned,char[12]);
int label_set(unsigned,const u8 *);
/* Whole-file copies for FORMAT /S and SYS. */
int load_file(const char *,u8 **,u32 *);
int store_file(const char *,const u8 *,u32,u8);
#endif
