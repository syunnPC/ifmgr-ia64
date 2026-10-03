/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
DosApi *app_dos;
EFI_STATUS app_init(EFI_SYSTEM_TABLE *st) {
    EFI_GUID guid=DOS_API_GUID;
    EFI_STATUS e=st->BootServices->LocateProtocol(&guid,NULL,(void **)&app_dos);
    if(EFI_ERROR(e)) return e;
    if(dos_client_bind(app_dos)) return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
