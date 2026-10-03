/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef long time_t;
#endif
typedef unsigned short _ino_t;
typedef short _dev_t;
typedef long _off_t;
#define ino_t _ino_t
#define dev_t _dev_t
#define off_t _off_t
#endif
