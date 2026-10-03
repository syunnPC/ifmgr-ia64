/* SPDX-License-Identifier: GPL-2.0-or-later
 * Task host interface (TASKHOST and TASKAPP). A guest is a
 * subsystem-11 image: its efi_main registers an entry and returns
 * EFI_SUCCESS, so IO.SYS keeps it loaded; the host then runs the entry on
 * its own fiber and DOS task context and unloads the image afterwards.
 */
#ifndef DOS_TASKHOST_H
#define DOS_TASKHOST_H
#include "base.h"
#define TASK_HOST_GUID {0x6a4f7d20,0x3b1e,0x4c55,{0x9a,0x3e,0x74,0x61,0x73,0x6b,0x68,0x01}}
#define TASK_HOST_VERSION 1
typedef struct {u32 size,instance; const char *arguments;} TaskStart;
typedef struct {
    u32 version,size;
    /* Only during the guest's efi_main; one entry per image. */
    int (*register_task)(int (*entry)(const TaskStart *));
    /* Cooperative switch at a DOS safe point (InDOS == 0). */
    void (*yield)(void);
} TaskHost;
#endif
