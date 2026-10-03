/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _STDDEF_H
#define _STDDEF_H
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned __int64 size_t;
#endif
typedef __int64 ptrdiff_t;
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(t,m) ((size_t)&(((t *)0)->m))
#endif
