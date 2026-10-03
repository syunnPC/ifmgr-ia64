/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EFI_VGA_H
#define EFI_VGA_H
#include "efi_support.h"
#include "io.h"
int efi_vga_init(EFI_SYSTEM_TABLE *); /* 1 when a VGA-compatible adapter is there */
int efi_vga_text(void); /* mode 3, keeping the adapter's state */
int efi_vga_port(u32 port,u8 *value,int write); /* 3B0h-3DFh */
/* Console text, bytes or UTF-16 when wide, with attr or keeping the cells'
 * attributes (attr<0); nowrap keeps it in the last column. */
void efi_vga_console(const void *text,size_t n,int wide,int attr,int nowrap);
void efi_vga_shadow(const void *text,size_t n,int wide,u8 attr,int nowrap); /* text the console shows */
void efi_vga_shadow_locate(unsigned col,unsigned row);
void efi_vga_shadow_erase(unsigned col,unsigned row,unsigned cells,u8 attr);
void efi_vga_shadow_clear(void);
int efi_vga_shadow_save(void *out,u32 size); /* the text the console shows, and its cursor */
int efi_vga_shadow_restore(const void *in,u32 size); /* kept again, and drawn on the firmware console */
void efi_vga_clear(u8 attr); /* the displayed page, the cursor home */
void efi_vga_erase(unsigned col,unsigned row,unsigned cells,u8 attr); /* cells from there, the cursor kept */
int efi_vga_intensity(void); /* attribute bit 7 is a bright background */
int efi_vga_size(unsigned *cols,unsigned *rows); /* 0 in a graphics mode */
int efi_vga_where(unsigned *col,unsigned *row);
void efi_vga_locate(unsigned col,unsigned row);
void efi_vga_show_cursor(int on); /* the cursor shown or hidden (start line bit 5) */
void efi_vga_console_mode(void); /* a text mode, mode 3 unless one is set */
void efi_vga_leave(void); /* the adapter's state before efi_vga_text, for a GOP mode */
#endif
