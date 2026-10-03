/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_EFI_SERIAL_H
#define DOS_EFI_SERIAL_H
#include "efi_support.h"
#include "io.h"
void efi_serial_init(EFI_SYSTEM_TABLE *,IoServices *);
void efi_serial_close(void);
/* The firmware console's serial device, if one was identified: console text
 * for it while a graphical shell owns the screen. ASCII only, LF -> CR LF. */
int efi_serial_console_present(void);
void efi_serial_console_write(const void *,size_t);
void efi_serial_console_write_text(const u16 *,size_t);
#endif
