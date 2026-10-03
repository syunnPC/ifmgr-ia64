/* SPDX-License-Identifier: GPL-2.0-or-later
 * W16DLL.DLL: a Windows 3.0 library in C (built with Open Watcom, large
 * model) for the Win16 tests: entry points that use the library's own data
 * segment and local heap, a call back into the program through
 * MakeProcInstance, and a window class whose procedure lives here. W16APP
 * imports it by name and loads it again with LoadLibrary.
 */
#include <windows.h>
static HINSTANCE library;
static int calls;
static char text[32];

int FAR PASCAL LibMain(HINSTANCE instance,WORD data,WORD heap,LPSTR command) {
    (void)data; (void)heap; (void)command;
    library=instance;
    lstrcpy(text,"from W16DLL");
    return 1;
}
int FAR PASCAL _export WEP(int type) {
    (void)type;
    OutputDebugString("W16DLL: WEP");
    return 1;
}
int FAR PASCAL _export DllAdd(int a,int b) {calls++; return a+b;}
int FAR PASCAL _export DllCalls(void) {return calls;}
LPSTR FAR PASCAL _export DllText(void) {return text;}
/* The library's own local heap. */
int FAR PASCAL _export DllHeap(void) {
    HLOCAL h=LocalAlloc(LMEM_FIXED|LMEM_ZEROINIT,100); char NEAR *p; int ok;
    if(!h) return 0;
    p=(char NEAR *)LocalLock(h);
    lstrcpy((LPSTR)p,"heap");
    ok=p[0]=='h' && p[4]==0 && LocalSize(h)>=100;
    LocalUnlock(h); LocalFree(h);
    return ok;
}
/* A procedure of the program, called with the program's data segment. */
int FAR PASCAL _export DllCallBack(FARPROC proc,int value) {
    return ((int (FAR PASCAL *)(int))proc)(value)+1;
}
LRESULT FAR PASCAL _export DllWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_USER+5) return 1000+calls+wp;
    return DefWindowProc(h,msg,wp,lp);
}
BOOL FAR PASCAL _export DllRegister(void) {
    WNDCLASS wc;
    wc.style=0; wc.lpfnWndProc=DllWndProc; wc.cbClsExtra=0; wc.cbWndExtra=0; wc.hInstance=library;
    wc.hIcon=NULL; wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszMenuName=NULL; wc.lpszClassName="W16DllClass";
    return RegisterClass(&wc)!=0;
}
