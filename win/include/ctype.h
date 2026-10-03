/* SPDX-License-Identifier: GPL-2.0-or-later
 * Character classes of the C locale (ASCII), as functions. */
#ifndef _CTYPE_H
#define _CTYPE_H
int isalnum(int);
int isalpha(int);
int iscntrl(int);
int isdigit(int);
int isgraph(int);
int islower(int);
int isprint(int);
int ispunct(int);
int isspace(int);
int isupper(int);
int isxdigit(int);
int tolower(int);
int toupper(int);
int isascii(int);
int toascii(int);
#define __isascii isascii
#define __toascii toascii
#define _tolower(c) ((c)-'A'+'a')
#define _toupper(c) ((c)-'a'+'A')
#endif
