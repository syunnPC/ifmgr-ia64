/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_H
#define DOS_H
#include "fat.h"
#include "arena.h"
#define DOS_HANDLES 20
#define DOS_MAX_HANDLES 255
#define DOS_MAX_FILES 255
#define DOS_DEFAULT_FILES 64
#define DOS_MAX_LOCKS 256
#define DOS_PROCESSES 64 /* processes and task contexts: each Interface Manager task has one */
#define DOS_ARENA_BYTES (4*1024*1024)
/* dos_init's memory: the arena, then the process table. */
#define DOS_PROCESS_BYTES (DOS_PROCESSES*10*1024)
#define DOS_MEMORY_BYTES (DOS_ARENA_BYTES+DOS_PROCESS_BYTES)
/* MSDOS.SYS private interfaces; application code includes dos_api.h instead. */
extern Fat dos_volume;
extern Arena dos_arena;
int dos_init(const Disk *,void *);
int dos_attach_disks(const IoServices *);
int dos_block_register(const DosBlockSpec *);
int dos_block_info(u32,DosBlockInfo *);
int dos_drive_info(u32,DosDriveInfo *);
int dos_select_drive(unsigned);
unsigned dos_current_drive(void);
int dos_drive_cwd(unsigned,char[DOS_PATH_MAX]);
int dos_path(const char *,char[DOS_PATH_MAX]);
/* Open actions: 0=open, 1=create/truncate, 2=create exclusively. */
int dos_open(const char *,unsigned,unsigned,unsigned *);
int dos_open_ex(const char *,unsigned,u8,unsigned,unsigned *,unsigned *);
int dos_file_time(unsigned,int,u16 *,u16 *);
int dos_lock(unsigned,int,u32,u32);
int dos_canonical(const char *,char[DOS_PATH_MAX]);
int dos_close(unsigned);
int dos_read(unsigned,void *,u32,u32 *);
int dos_write(unsigned,const void *,u32,u32 *);
int dos_seek(unsigned,i64,unsigned,u32 *);
int dos_dup(unsigned,unsigned *);
int dos_dup2(unsigned,unsigned);
int dos_find_first(const char *,u8,DosFind *);
int dos_find_next(DosFind *);
int dos_chdir(const char *);
int dos_mkdir(const char *);
int dos_remove(const char *,int);
int dos_rename(const char *,const char *);
int dos_attribute(const char *,int,u8 *);
int dos_exec(const char *,const char *);
int dos_flush(void);
void dos_int21(DosRegs *);
const char *dos_command_tail(void);
u32 dos_pid(void);
int dos_print(u32,DosPrintRequest *);
u32 dos_idle(void);
int dos_keyb(u32,DosKeybRequest *);
u32 dos_keyb_key(const IoEvent *,u16 *);
int dos_program_path(char[DOS_PATH_MAX]);
int dos_arena_block(u32,DosArenaBlock *);
int platform_exec(const void *,u32,const char *,unsigned *);
int dos_get_datetime(DosDateTime *);
int dos_config_country(u16,u16,const char *);
int platform_console_key(IoEvent *,unsigned);
void platform_wait(u32);
void platform_exit(unsigned);
void platform_shutdown(void);
void platform_restart(void);
int platform_exit_resident(unsigned,u64 *);
int platform_image_load(const void *,u32,u64 *,u64 *,u64 *);
int platform_image_start(u64,const char *,unsigned *);
int platform_image_discard(u64);
void dos_restart(void);
int dos_redirect(u32 *,const DosRedirector *);
int dos_unredirect(u32);
int dos_disk_read(u32,u64,u32,void *,u32 *);
int dos_disk_write(u32,u64,u32,const void *,u32 *);
int dos_volume_lock(u32,int);
int dos_physical_info(u32,DosPhysicalInfo *);
int dos_physical_read(u32,u64,u32,void *,u32 *);
int dos_physical_write(u32,u64,u32,const void *,u32 *);
int dos_run(const IoServices *);
int dos_query(DosInfo *);
void dos_set_errorlevel(unsigned);
int dos_task_create(u32 *);
int dos_task_select(u32);
int dos_switch_hook(const DosSwitchHook *,DosSwitchHook *);
int dos_switch_away(u32);
int dos_full_path(const char *,char[DOS_PATH_MAX]);
int dos_drive_map(u32,const DosDriveMap *,DosDriveMap *);
int dos_assign(const u8 *,u8 *);
int dos_append(const DosAppend *,DosAppend *);
int dos_append_task(u32,u32 *);
int dos_installed(u32,const u32 *,u32 *);
int dos_task_destroy(u32);
void dos_bind_io(const IoServices *);
int dos_env_get(const char *,char *,u32);
int dos_env_set(const char *,const char *);
int dos_env_list(u32,char *,u32);
int dos_set_files(unsigned);
int dos_set_handle_count(unsigned);
int dos_break_handler(const DosBreakHandler *,DosBreakHandler *);
int dos_last_exit(DosExitInfo *);
void dos_set_break_check(int);
int dos_critical_handler(const DosCriticalHandler *,DosCriticalHandler *);
int dos_extended_error(DosExtendedError *);
/* Capture the first failure before continuing mandatory cleanup. */
int dos_preserve_error(int);
void dos_shutdown(void);
int dos_device_register(const DosDeviceSpec *);
int dos_device_info(u32,DosDeviceInfo *);
#endif
