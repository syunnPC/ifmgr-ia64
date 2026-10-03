/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CONFIG_H
#define DOS_CONFIG_H
#include "base.h"
#define DOS_CONFIG_DEVICES 16
/* DEVICE= drivers, and INSTALL= programs (install set), run after them. */
typedef struct {char path[DOS_PATH_MAX],tail[256]; unsigned line; int install;} DosConfigDevice;
typedef struct {
    char shell[DOS_PATH_MAX],tail[256];
    unsigned files,warnings;
    int break_check;
    u16 country,code_page;
    char country_file[DOS_PATH_MAX];
    unsigned device_count;
    DosConfigDevice devices[DOS_CONFIG_DEVICES];
} DosConfig;
void config_defaults(DosConfig *);
int config_line(DosConfig *,char *);
int config_load(DosConfig *);
#endif
