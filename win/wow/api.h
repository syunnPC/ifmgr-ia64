/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: tables of the Win16 functions each system module provides. */
#ifndef WOW_API_H
#define WOW_API_H
#include "wow.h"
typedef DWORD (*Api16Fn)(Args16 *);
typedef struct {WORD ordinal; const char *name; const char *args; BYTE flags; Api16Fn fn;} Api16;
#define A_RET16 1 /* returns AX only */
#define A_CDECL 2 /* C calling convention: the caller removes the arguments */
#define A_REGS 4  /* register function: no stack arguments, returns itself */
#define PTR(x) ((void *)(ULONG_PTR)(x))
extern const Api16 kernel_api[],user_api[],gdi_api[],commdlg_api[],shell_api[],mmsystem_api[],lzexpand_api[],ver_api[],
    win87em_api[],keyboard_api[],sound_api[],system_api[],toolhelp_api[],pscript_api[];
extern const unsigned kernel_count,user_count,gdi_count,commdlg_count,shell_count,mmsystem_count,lzexpand_count,ver_count,
    win87em_count,keyboard_count,sound_count,system_count,toolhelp_count,pscript_count;
void Return16(Task16 *t,WORD bytes);
DWORD W16_Unimplemented(Args16 *a); /* a function without an implementation: 0 */
/* Conversions for generated wrappers (convert.c): structures at linear
 * addresses of 16-bit memory, brushes (a system color index + 1 below
 * 0x100), and instance or module handles. */
BOOL RectIn16(DWORD p,RECT *r);
void RectOut16(DWORD p,const RECT *r);
BOOL PointIn16(DWORD p,POINT *pt);
void PointOut16(DWORD p,const POINT *pt);
void SizeOut16(DWORD p,const SIZE *s);
HBRUSH Brush32(WORD h);
HINSTANCE Instance32(WORD h);
extern HINSTANCE wow_instance;
DWORD CreateStructOut16(Task16 *t,HWND h,const CREATESTRUCT *c,BYTE **s); /* in scratch memory */
void LogFontOut16(BYTE *p,const LOGFONT *l); /* LOGFONT with 16-bit fields (gdi16.c) */
void LogFontIn16(const BYTE *p,LOGFONT *l);
/* Metafiles (gdi16.c): a Win16 metafile is a global block of its bits. */
HMETAFILE Metafile32(WORD h); /* a native one made from it, or NULL */
WORD Metafile16(Task16 *t,HMETAFILE mf,BOOL keep); /* its bits in a new block; the native one goes unless kept */
/* The native instance handle of a Win16 module: its module handle (a
 * selector) in a range no native image uses. */
#define INSTANCE32(sel) ((HINSTANCE)(ULONG_PTR)(0x7f000000UL|(sel)))
/* wsprintf with 16-bit arguments from the stack at sel:args. */
int Format16(char *out,unsigned size,LPCSTR format,WORD sel,WORD args);
#endif
