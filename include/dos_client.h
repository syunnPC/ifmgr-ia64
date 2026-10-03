/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CLIENT_H
#define DOS_CLIENT_H
#include "dos_api.h"
/* Application-side wrappers. No kernel, FAT or firmware headers are needed. */
int dos_client_bind(DosApi *);
int dos_call(DosRegs *);
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
int dos_set_dta(void *,u32);
int dos_get_dta(void **,u32 *);
int dos_fcb_call(unsigned,void *,u32 *,unsigned *);
int dos_fcb_parse(const char **,DosFcb *,unsigned,unsigned *);
int dos_getcwd(char[DOS_PATH_MAX]);
int dos_drive_cwd(unsigned,char[DOS_PATH_MAX]);
int dos_drive_info(u32,DosDriveInfo *);
int dos_select_drive(unsigned);
unsigned dos_current_drive(void);
int dos_chdir(const char *);
int dos_mkdir(const char *);
int dos_remove(const char *,int);
int dos_rename(const char *,const char *);
int dos_attribute(const char *,int,u8 *);
int dos_exec(const char *,const char *);
int dos_flush(void);
int dos_alloc(u32,void **);
int dos_free(void *);
int dos_query(DosInfo *);
unsigned dos_get_errorlevel(void);
void dos_set_errorlevel(unsigned);
int dos_datetime(unsigned[7]);
int dos_get_datetime(DosDateTime *);
int dos_set_date(unsigned,unsigned,unsigned);
int dos_set_time(unsigned,unsigned,unsigned,unsigned);
int dos_country_info(u16,u16,DosCountryInfo *);
int dos_country_set(u16);
int dos_code_page(u16 *,u16 *);
int dos_code_page_set(u16);
int dos_nls_table(u16,u16,unsigned,void *,u32,u32 *);
int dos_nls_case(void *,u32,int);
void dos_shutdown(void);
int dos_env_get(const char *,char *,u32);
int dos_env_set(const char *,const char *);
int dos_env_list(u32,char *,u32);
int dos_set_handle_count(unsigned);
int dos_break_handler(const DosBreakHandler *,DosBreakHandler *);
int dos_last_exit(DosExitInfo *);
int dos_break_check(int,int *);
/* AH=0Ah buffer: maximum byte count (including CR), template length, data.
 * Provide maximum+2 bytes. Returns length excluding CR in byte 1. A zero
 * maximum is a no-op. Native redirected character reads return DE_EOF at EOF. */
int dos_line_input(u8 *);
int dos_critical_handler(const DosCriticalHandler *,DosCriticalHandler *);
int dos_extended_error(DosExtendedError *);
const IoServices *dos_io_services(void);
int dos_redirect(u32 *,const DosRedirector *);
int dos_unredirect(u32);
int dos_device_register(const DosDeviceSpec *);
int dos_device_info(u32,DosDeviceInfo *);
int dos_block_register(const DosBlockSpec *);
int dos_block_info(u32,DosBlockInfo *);
/* Maintenance tools: drives are 0-based (A=0); sectors are 512 bytes. */
int dos_disk_read(u32,u64,u32,void *,u32 *);
int dos_disk_write(u32,u64,u32,const void *,u32 *);
int dos_volume_lock(u32,int);
int dos_physical_info(u32,DosPhysicalInfo *);
int dos_physical_read(u32,u64,u32,void *,u32 *);
int dos_physical_write(u32,u64,u32,const void *,u32 *);
void dos_restart(void);
int dos_device_params(unsigned,DosDeviceParams *);
int dos_set_device_params(unsigned,const DosDeviceParams *);
int dos_sector_io(unsigned,unsigned,DosSectorIo *);
int dos_media_id(unsigned,DosMediaId *,int);
int dos_get_dpb(unsigned,const DosDpb **);
/* AH=2Eh/54h. set=0 queries; enabled is 0 or 1 (system-wide). */
int dos_verify(int,int *);
/* AH=5Ah: directory gains a separator and eight hex digits; it needs
 * 13 spare bytes. Returns a read/write compatibility-mode handle. */
int dos_temp_file(char *,unsigned,unsigned *);
/* SUBST, JOIN and ASSIGN (DosApi drive_map, assign); full_path is a name in
 * its letter's terms (dos_canonical before DOS_CAP_DRIVE_MAP). */
int dos_full_path(const char *,char[DOS_PATH_MAX]);
int dos_drive_map(u32,const DosDriveMap *,DosDriveMap *);
int dos_assign(const u8 *,u8 *);
/* APPEND (DosApi append, append_task). */
int dos_append(const DosAppend *,DosAppend *);
int dos_append_task(u32,u32 *);
/* DosApi installed (DOS_INSTALLED_*): SHARE, FASTOPEN, NLSFUNC, GRAFTABL,
 * ANSI.SYS, HIMEM.SYS and EMM386.SYS since boot. */
int dos_installed(u32,const u32 *,u32 *);
#endif
