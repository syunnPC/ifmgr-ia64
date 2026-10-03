/* SPDX-License-Identifier: GPL-2.0-or-later
 * Variable arguments for WDK cl on IA-64: a variadic function's arguments
 * sit in consecutive 8-byte memory slots after the last named one. */
#ifndef _STDARG_H
#define _STDARG_H
typedef char *va_list;
#define _VA_SLOT(t) ((sizeof(t)+7)&~(sizeof(char *)-1))
#define va_start(ap,v) ((ap)=(va_list)&(v)+_VA_SLOT(v))
#define va_arg(ap,t) (*(t *)(((ap)+=_VA_SLOT(t))-_VA_SLOT(t)))
#define va_end(ap) ((ap)=(va_list)0)
#endif
