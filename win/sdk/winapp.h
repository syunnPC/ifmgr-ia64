/* SPDX-License-Identifier: GPL-2.0-or-later
 * WINAPP.LIB: helpers shared by the accessories (C89). Windows 3.0 had no
 * common dialogs; each program carried its own, and these are ours. Print
 * and Print Setup are COMMDLG's, as in Windows 3.1's accessories.
 */
#ifndef WINAPP_H
#define WINAPP_H
#include <windows.h>
#include "helpmenu.h"
/* The File Open and Save As dialogs. path holds the initial name (or a
 * spec such as "*.TXT") and receives the chosen file's full path; spec is
 * the files listed. TRUE when a file was chosen. */
BOOL FileOpenDialog(HWND owner,LPCSTR title,LPCSTR spec,LPSTR path,int size);
BOOL FileSaveDialog(HWND owner,LPCSTR title,LPCSTR spec,LPSTR path,int size);
/* The file name part of a path. */
LPCSTR FileTitle(LPCSTR path);
/* Whole files: a zero-terminated copy in GlobalAlloc memory, and writing one. */
LPSTR ReadWholeFile(LPCSTR path,DWORD *size);
BOOL WriteWholeFile(LPCSTR path,const void *data,DWORD size);
/* Printing: the printer Print Setup chose, or the default (NULL, said in a
 * message box, when there is none); Print Setup itself. PrintStart begins a
 * document with a Cancel box over the disabled owner; when it fails (said,
 * unless the user cancelled) the DC is gone. Between pages PrintCancelled
 * says whether to stop; PrintEnd ends or abandons the document and deletes
 * the DC. */
HDC PrinterDC(HWND owner);
void PrinterSetup(HWND owner);
/* The Print dialog for that printer: the page range and copies, and its DC
 * (NULL when cancelled). */
HDC PrintDialogDC(HWND owner,int first_page,int last_page,int *from,int *to,int *copies);
BOOL PrintStart(HDC dc,HWND owner,LPCSTR program,LPCSTR document);
BOOL PrintCancelled(void);
void PrintEnd(HDC dc,BOOL ok);
/* The Help menu's commands (helpmenu.h) for the program's help file; TRUE
 * when id was one of them. A program closing calls WinHelp(HELP_QUIT). */
BOOL HelpCommand(HWND owner,UINT id,LPCSTR file);
/* DIALOG templates built in memory (the File dialogs' and printing's Cancel
 * box): a string as the template's characters, and a control, DWORD-aligned
 * from the template's start base, its class the atom (0x80 button, 0x81
 * edit, 0x82 static, 0x83 list box). Each returns where the next part goes. */
WORD *TemplateText(WORD *p,LPCSTR s);
WORD *TemplateItem(WORD *p,const void *base,DWORD style,int x,int y,int cx,int cy,WORD id,WORD atom,LPCSTR text);
#endif
