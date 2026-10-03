/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H
#include <efi.h>
#include "dos_client.h"
extern DosApi *app_dos;
EFI_STATUS app_init(EFI_SYSTEM_TABLE *);
static inline int app_call(DosRegs *r) {
    app_dos->int21(r); return r->flags&1?(int)r->ax:0;
}
#endif
