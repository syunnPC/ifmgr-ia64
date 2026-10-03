/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CONSOLE_H
#define DOS_CONSOLE_H
#include "io.h"
typedef struct {
    void *context;
    int (*read)(void *,u8 *);
    int (*write)(void *,const void *,u32);
    unsigned column;
} ConsoleLineIo;
void console_reset(void);
int console_byte(int,int,u8 *);
int console_flush(void);
int console_line(u8 *,const ConsoleLineIo *);
/* CON bytes in the selected code page, written as UTF-16 through IO.SYS. */
void console_write(const void *,u32);
void console_write_plain(const void *,u32); /* the same, escape sequences as text */
void console_codepage_changed(void);
void platform_console_text(const u16 *,size_t);
/* ANSI.SYS (kernel/ansi.c) and IO.SYS's text screen under it. */
void ansi_reset(void);
u32 ansi_options(void);
void ansi_set_options(u32);
void ansi_write(const u8 *,u32);
int ansi_typed(u8 *,int);
int ansi_key(u8,u8);
void ansi_flush(void);
int ansi_request(unsigned,void *,u32,u32 *);
int platform_text_available(void);
int platform_text_query(IoTextScreen *);
int platform_text_locate(u32,u32);
int platform_text_attribute(u32,u32);
int platform_text_erase(u32,u32,u32);
int platform_text_mode(u32);
#endif
