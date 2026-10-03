/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_NLS_PRIVATE_H
#define DOS_NLS_PRIVATE_H
#include "dos_api.h"
/* The caller retains the immutable database for the lifetime of its views. */
typedef struct {const u8 *data; u32 size;} NlsTable;
typedef struct {const u8 *data; u32 size,directory,count;} NlsDatabase;
typedef struct {u16 country,code_page; NlsTable info,upper,file_upper,file_chars,collate,dbcs;} NlsCountry;
int nls_database_open(NlsDatabase *,const void *,u32);
int nls_database_country(const NlsDatabase *,u16,u16,NlsCountry *);
int nls_country_lead(const NlsCountry *,u8);
u8 nls_country_upper(const NlsCountry *,u8,int);
int nls_country_case(const NlsCountry *,u8 *,u32,int);
int nls_country_file_char(const NlsCountry *,u8);
/* Resident, firmware-independent NLS state. Returned application tables are
 * copied; loading another database cannot invalidate an application pointer. */
int nls_reset(void);
int nls_load(u16,u16,const char *);
int nls_dispatch(DosRegs *);
int nls_lead(u8);
u8 nls_upper(u8,int);
int nls_file_char(u8);
unsigned nls_char_size(const char *);
char *nls_last_sep(char *,const char *);
#endif
