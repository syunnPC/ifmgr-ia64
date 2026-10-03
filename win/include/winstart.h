/* SPDX-License-Identifier: GPL-2.0-or-later
 * Start-up and system interfaces between the SDK's start-up code, KERNEL and
 * USER. Applications do not use these directly. C89.
 */
#ifndef _WINSTART_H
#define _WINSTART_H
#include <windows.h>
typedef struct tagWINSTARTINFO {HINSTANCE hInstance; LPSTR lpCmdLine; int nCmdShow;} WINSTARTINFO,*LPWINSTARTINFO;
/* Task signals delivered to the proc set with SetTaskSignalProc. */
#define SG_EXIT 0x0020
typedef void (CALLBACK *TASKSIGNALPROC)(HTASK,WORD);
WINBASEAPI BOOL WINAPI InitTask(LPWINSTARTINFO);
WINBASEAPI void WINAPI ExitProcess(UINT);
WINBASEAPI TASKSIGNALPROC WINAPI SetTaskSignalProc(HTASK,TASKSIGNALPROC);
WINUSERAPI BOOL WINAPI InitApp(HINSTANCE);
/* USER's ExitWindows ends Windows through KERNEL: every task ends, the
 * others first, and WIN.COM returns to DOS. */
WINBASEAPI void WINAPI ExitKernel(int);
/* Each task's HTASK and WINHOST task id. */
WINBASEAPI DWORD WINAPI GetTaskId(HTASK);
#endif
