/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_BASE_H
#define DOS_BASE_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define DOS_PATH_MAX 128
enum {
    DE_OK = 0, DE_FUNCTION = 1, DE_NOFILE = 2, DE_PATH = 3,
    DE_HANDLES = 4, DE_ACCESS = 5, DE_HANDLE = 6, DE_ARENA = 7,
    DE_NOMEM = 8, DE_BLOCK = 9, DE_ENV = 10, DE_FORMAT = 11, DE_MODE = 12, DE_DATA = 13,
    DE_DRIVE = 15, DE_CURRENT = 16, DE_NOTSAME = 17, DE_NOMORE = 18,
    DE_READONLY = 19, DE_IO = 23, DE_SEEK = 25,
    DE_SHARE = 32, DE_LOCK = 33, DE_CHANGED = 34, DE_LOCKS = 36, DE_EOF = 38, DE_FULL = 39,
    DE_EXISTS = 80, DE_CRITICAL = 83, DE_BUSY = 170, DE_NOTREADY = 21, DE_BREAK = 256,
    DE_REMOTE = 50 /* network request not supported: FAT-level call on a redirected drive */
};
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int stricmp(const char *, const char *);
char *strchr(const char *, int);
int strcopy(char *, size_t, const char *);
int strappend(char *, size_t, const char *);
char upper(char);
u16 rd16(const void *);
u32 rd32(const void *);
void wr16(void *, u16);
void wr32(void *, u32);
void con_puts(const char *);
void con_write(const void *, size_t);
int con_getch(void);
void con_clear(void);
void print(const char *, ...);
extern void (*print_sink)(const void *,size_t);
const char *dos_error(int);
#endif
