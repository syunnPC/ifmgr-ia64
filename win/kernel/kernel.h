/* SPDX-License-Identifier: GPL-2.0-or-later
 * KERNEL.DLL internals. C89.
 */
#ifndef KERNEL_INTERNAL_H
#define KERNEL_INTERNAL_H
#define KERNEL_BUILD
#include <windows.h>
#include <winstart.h>
#include <winhost.h>
#include <string.h>
void MemoryInit(void);
void MemoryShutdown(void);
void MemoryFreeTask(wh_u32 task);
void FileInit(HINSTANCE kernel);
void AtomsTaskEnded(void *task);
/* USER: a DOS program set aside, as an icon until the user switches back to it. */
WINUSERAPI void WINAPI DosAway(LPCSTR title);
#endif
