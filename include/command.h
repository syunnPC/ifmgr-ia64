/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef COMMAND_H
#define COMMAND_H
#include "dos_client.h"
int shell_line(const char *);
/* Run a batch file with its parameter tail at nesting level 1..8. */
int shell_batch(const char *,const char *,unsigned);
int shell(const char *);
#endif
