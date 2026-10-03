/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KEYB_H
#define KEYB_H
#include "dos_api.h"
/* KEYB's resident part (kernel/keyb.c). */
void keyb_reset(void);
int keyb_request(u32,DosKeybRequest *);
u32 keyb_key(const IoEvent *,u16 keys[DOS_KEYB_KEYS]);
u32 keyb_look(const IoEvent *,u16 keys[DOS_KEYB_KEYS]); /* the same, nothing changed */
/* CON has selected a code page (DISPLAY.SYS's INT 2Fh AD81h): KEYB's
 * table for it becomes active; DE_NOFILE when KEYB is installed without
 * one, its table left as it was. */
int keyb_code_page(u16);
#endif
