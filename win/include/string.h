/* SPDX-License-Identifier: GPL-2.0-or-later
 * String functions: memset and the like from win/sdk/crt.c, the rest from
 * the C run-time library (win/crt), with Microsoft C's additions and the far
 * forms Windows 3.0 programs use. */
#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
void *memset(void *,int,size_t);
void *memcpy(void *,const void *,size_t);
void *memmove(void *,const void *,size_t);
int memcmp(const void *,const void *,size_t);
void *memchr(const void *,int,size_t);
size_t strlen(const char *);
char *strcpy(char *,const char *);
char *strncpy(char *,const char *,size_t);
char *strcat(char *,const char *);
int strcmp(const char *,const char *);
int strncmp(const char *,const char *,size_t);
char *strchr(const char *,int);
char *strrchr(const char *,int);
char *strncat(char *,const char *,size_t);
int strcoll(const char *,const char *);
size_t strxfrm(char *,const char *,size_t);
size_t strspn(const char *,const char *);
size_t strcspn(const char *,const char *);
char *strpbrk(const char *,const char *);
char *strstr(const char *,const char *);
char *strtok(char *,const char *);
char *strerror(int);
void *memccpy(void *,const void *,int,size_t);
int memicmp(const void *,const void *,size_t);
int stricmp(const char *,const char *);
int strnicmp(const char *,const char *,size_t);
char *strdup(const char *);
char *strupr(char *);
char *strlwr(char *);
char *strrev(char *);
char *strset(char *,int);
char *strnset(char *,int,size_t);
#define _memccpy memccpy
#define _memicmp memicmp
#define _stricmp stricmp
#define strcmpi stricmp
#define _strcmpi stricmp
#define _strnicmp strnicmp
#define _strdup strdup
#define _strupr strupr
#define _strlwr strlwr
#define _strrev strrev
#define _strset strset
#define _strnset strnset
#define _fmemset memset
#define _fmemcpy memcpy
#define _fmemmove memmove
#define _fmemcmp memcmp
#define _fmemchr memchr
#define _fmemccpy memccpy
#define _fmemicmp memicmp
#define _fstrlen strlen
#define _fstrcpy strcpy
#define _fstrncpy strncpy
#define _fstrcat strcat
#define _fstrncat strncat
#define _fstrcmp strcmp
#define _fstrncmp strncmp
#define _fstricmp stricmp
#define _fstrnicmp strnicmp
#define _fstrchr strchr
#define _fstrrchr strrchr
#define _fstrstr strstr
#define _fstrtok strtok
#define _fstrspn strspn
#define _fstrcspn strcspn
#define _fstrpbrk strpbrk
#define _fstrdup strdup
#define _fstrupr strupr
#define _fstrlwr strlwr
#define _fstrrev strrev
#define _fstrset strset
#define _fstrnset strnset
#define _nstrdup strdup
#define hmemcpy memcpy
#endif
