/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CLOCK_H
#define DOS_CLOCK_H
#include "io.h"
int dos_clock_read(IoDateTime *);
int dos_clock_write(const IoDateTime *,u32);
#endif
