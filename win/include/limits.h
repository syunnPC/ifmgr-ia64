/* SPDX-License-Identifier: GPL-2.0-or-later
 * Integer limits of WDK cl for IA-64 (LLP64: int and long are 32 bits). */
#ifndef _LIMITS_H
#define _LIMITS_H
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 0xff
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define MB_LEN_MAX 2
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 0xffff
#define INT_MIN (-2147483647-1)
#define INT_MAX 2147483647
#define UINT_MAX 0xffffffffU
#define LONG_MIN (-2147483647L-1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 0xffffffffUL
#define _I64_MIN (-9223372036854775807i64-1)
#define _I64_MAX 9223372036854775807i64
#define _UI64_MAX 0xffffffffffffffffui64
#endif
