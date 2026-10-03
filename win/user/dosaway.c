/* SPDX-License-Identifier: GPL-2.0-or-later
 * A DOS program set aside (Alt+Tab, Alt+Esc or Ctrl+Esc while it waited for
 * a key): KERNEL's task for it shows an icon with the program's name until
 * the user switches back to it (a double click, Restore, the Task List's
 * Switch To). Interface Manager does not end while it is there.
 */
#include "user.h"

/* 16x16, shown at 32x32: a screen with a prompt. */
static const char *const dos_art[]={
    "KKKKKKKKKKKKKKKK","KNNNNNNNNNNNNNNK","KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KKWKWKKKKKKKKKKK","KKWWKKKKKKKKKKKK","KKWKWKKWWWKKKKKK","KKKKKKKKKKKKKKKK",
    "KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KKKKKKKKKKKKKKKK","KGGGGGGGGGGGGGGK","KKKKKKKKKKKKKKKK",NULL};
/* The window's state (its extra long): made, an icon, or to go back. */
#define AWAY_MADE 0
#define AWAY_ICON 1
#define AWAY_BACK 2
static LRESULT CALLBACK DosAwayProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    /* Restored any way, once it is an icon: back to the program. */
    case WM_QUERYOPEN: if(GetWindowLong(h,0)==AWAY_ICON) SetWindowLong(h,0,AWAY_BACK); return FALSE;
    case WM_SIZE: if(GetWindowLong(h,0)==AWAY_ICON && wp!=SIZE_MINIMIZED) SetWindowLong(h,0,AWAY_BACK); return 0;
    case WM_QUERYENDSESSION: case WM_CLOSE: {
        char text[160],title[64]; HWND active=GetActiveWindow();
        GetWindowText(h,title,sizeof(title));
        wsprintf(text,"%s is still running. Switch to it and end it first.",(LPSTR)title);
        MessageBox(h,text,"Interface Manager",MB_OK|MB_ICONEXCLAMATION);
        if(active && active!=h && IsWindow(active)) SetActiveWindow(active);
        return 0;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}
void WINAPI DosAway(LPCSTR title) {
    static BOOL registered; HWND h; MSG m;
    if(!registered) {
        Class *c;
        registered=RegisterSystemClass("DosApplication",DosAwayProc,0,sizeof(LONG),StockCursor(IDC_ARROW),COLOR_WINDOW+1)!=0;
        if(registered && (c=FindClass("DosApplication",NULL))!=NULL) c->wc.hIcon=ArtIcon(dos_art,2);
    }
    h=CreateWindow("DosApplication",title,WS_OVERLAPPEDWINDOW|WS_MINIMIZE,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,NULL,NULL);
    if(!h) return;
    ShowWindow(h,SW_SHOWMINNOACTIVE);
    SetWindowLong(h,0,AWAY_ICON);
    while(GetWindowLong(h,0)!=AWAY_BACK && GetMessage(&m,NULL,0,0)) {TranslateMessage(&m); DispatchMessage(&m);}
    DestroyWindow(h);
}
