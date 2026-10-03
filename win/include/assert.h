/* SPDX-License-Identifier: GPL-2.0-or-later
 * assert: a failed assertion ends the program with a message box. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void _assert(const char *,const char *,unsigned);
#define assert(e) ((e)?(void)0:_assert(#e,__FILE__,__LINE__))
#endif
