/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_LOOP_DEVICE_H
#define DOS_LOOP_DEVICE_H
#include "base.h"
/* LOOPDRV.SYS control-read result. Generic control 8000h clears its FIFO. */
typedef struct {u32 opens,closes,live,queued,reentry,pid; u64 cookie;} LoopStats;
#endif
