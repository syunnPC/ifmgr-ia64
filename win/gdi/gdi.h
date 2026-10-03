/* SPDX-License-Identifier: GPL-2.0-or-later
 * GDI functions for USER only: window DCs, flushing, the cursor, object
 * owners and the printer's file prompt. C89.
 */
#ifndef GDI_INTERNAL_H
#define GDI_INTERNAL_H
#define GDI_VIS_MAX 256
#define GDI_CURSOR_SIZE 32
/* A DC on the screen: origin and clipping in screen coordinates. */
WINGDIAPI HDC WINAPI GdiCreateScreenDC(void);
WINGDIAPI void WINAPI GdiSetDCOrigin(HDC,int,int);
WINGDIAPI void WINAPI GdiSetVisRects(HDC,const RECT *,int);
/* Send what changed, and the cursor, to the screen. */
WINGDIAPI void WINAPI GdiFlush(void);
/* 32x32 cursor: AND and XOR masks, 4 bytes per row, most significant bit left. */
WINGDIAPI void WINAPI GdiSetCursor(const BYTE *,const BYTE *,int,int);
WINGDIAPI void WINAPI GdiMoveCursor(int,int,BOOL);
/* Objects belong to the task that made them and go when it ends. */
WINGDIAPI void WINAPI GdiTaskEnded(HTASK);
WINGDIAPI void WINAPI GdiSetOwner(HGDIOBJ,HTASK);
/* Printing to FILE: asks for a file name (USER's Print To File dialog);
 * FALSE when cancelled. */
typedef BOOL (CALLBACK *GDIFILEPROMPT)(LPSTR,int);
WINGDIAPI void WINAPI GdiSetFilePrompt(GDIFILEPROMPT);
/* A spooled document is queued: Print Manager is told (or started); FALSE
 * when it cannot be, and the document goes to its port at once. */
typedef BOOL (CALLBACK *GDISPOOLNOTIFY)(void);
WINGDIAPI void WINAPI GdiSetSpoolNotify(GDISPOOLNOTIFY);
#endif
