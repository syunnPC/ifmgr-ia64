/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CODEPAGE_H
#define DOS_CODEPAGE_H
#include "dos_device.h"
/* CON code-page state inside MSDOS.SYS. The selected page translates CON
 * bytes to UTF-16 and keys back to bytes; NLS (AH=38h/65h/66h) is separate. */
void codepage_reset(void);
int codepage_lead(u8);
u16 codepage_unicode(u8,int,u8);
unsigned codepage_encode(u32,u8[2]);
u16 codepage_selected(void);
int codepage_check(u16);
/* A prepared page made CON's (a refused one keeps the old); then KEYB,
 * told as DISPLAY.SYS tells it, may refuse it: DOS_CP_NOT_IN_FILE (status
 * 08h), the display having switched. */
int codepage_select(u16);
int codepage_request(DosDeviceRequest *);
/* Validates one native table file; used by the CON driver and tests. */
int codepage_find(const u8 *,u32,u16,u32 *,u32 *,unsigned *);
#endif
