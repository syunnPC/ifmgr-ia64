/* SPDX-License-Identifier: GPL-2.0-or-later
 * PE loader environment for EFI hosts: image memory from IO.SYS pages below
 * 2 GiB, files read through DOS, instruction cache flushes and DllMain calls.
 */
#ifndef WIN_HOST_ENV_H
#define WIN_HOST_ENV_H
#include "runtime.h"
#include "pe.h"
extern u64 host_pages_out;
extern unsigned host_files_out;
void host_env(PeEnv *,const IoServices *,const char *system_dir);
/* Pages below 2 GiB, for what programs address. */
int host_low_pages(u32 pages,void **out);
void host_low_free(void *memory,u32 pages);
#endif
