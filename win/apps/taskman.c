/* SPDX-License-Identifier: GPL-2.0-or-later
 * TASKMAN: the Task List, which USER starts for Ctrl+Esc, a double click
 * on the desktop and Switch To... on a system menu. It lists the running
 * programs' top-level windows (the one that was active first) to switch to
 * or end, and cascades or tiles the windows or arranges the icons; any of
 * these closes it.
 */
#include <windows.h>
#include "taskman.h"

static HWND list_dialog;

/* The windows the Task List shows and arranges: visible, unowned and
 * titled, in the order of the screen, not the Task List itself. */
static int windows(HWND *out,int max,BOOL arrange) {
    HWND h; int n=0; char title[2];
    for(h=GetWindow(GetDesktopWindow(),GW_CHILD);h && n<max;h=GetWindow(h,GW_HWNDNEXT)) {
        if(h==list_dialog || !IsWindowVisible(h) || GetWindow(h,GW_OWNER) || !GetWindowText(h,title,sizeof(title))) continue;
        if(arrange && IsIconic(h)) continue;
        out[n++]=h;
    }
    return n;
}
static BOOL any_icons(void) {
    HWND h;
    for(h=GetWindow(GetDesktopWindow(),GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT))
        if(h!=list_dialog && IsWindowVisible(h) && IsIconic(h) && !GetWindow(h,GW_OWNER)) return TRUE;
    return FALSE;
}
/* The screen less a row for icons when there are any. */
static void work_area(RECT *r) {
    SetRect(r,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
    if(any_icons()) r->bottom-=GetSystemMetrics(SM_CYICON)+GetSystemMetrics(SM_CYCAPTION)*2;
}
static void place(HWND h,int x,int y,int w,int ht) {
    if(IsZoomed(h)) ShowWindow(h,SW_RESTORE);
    SetWindowPos(h,NULL,x,y,w,ht,SWP_NOZORDER|SWP_NOACTIVATE);
}
/* From the bottom window up, each a caption lower and to the right. */
static void cascade(void) {
    HWND list[64]; RECT r; int n=windows(list,64,TRUE),i,step,w,h;
    if(!n) return;
    work_area(&r);
    step=GetSystemMetrics(SM_CYCAPTION)+GetSystemMetrics(SM_CYFRAME);
    w=max((r.right-r.left)/2,(r.right-r.left)-step*(n-1)); h=max((r.bottom-r.top)/2,(r.bottom-r.top)-step*(n-1));
    for(i=0;i<n;i++) {
        int k=n-1-i,x=(k*step)%max(step,(r.right-r.left)-w+1),y=(k*step)%max(step,(r.bottom-r.top)-h+1);
        place(list[i],r.left+x,r.top+y,w,h);
    }
}
/* Rows of equal windows, the top window first; a short last row has wider ones. */
static void tile(void) {
    HWND list[64]; RECT r; int n=windows(list,64,TRUE),columns,rows,i,row,column,in_row,height;
    if(!n) return;
    work_area(&r);
    for(columns=1;columns*columns<n;columns++) {}
    rows=(n+columns-1)/columns; height=(r.bottom-r.top)/rows;
    for(i=0,row=0;row<rows;row++) {
        in_row=row<rows-1?columns:n-columns*(rows-1);
        for(column=0;column<in_row;column++,i++) {
            int width=(r.right-r.left)/in_row;
            place(list[i],r.left+column*width,r.top+row*height,width,height);
        }
    }
}
static HWND chosen(HWND dlg) {
    LRESULT i=SendDlgItemMessage(dlg,IDC_TASKS,LB_GETCURSEL,0,0);
    return i<0?NULL:(HWND)SendDlgItemMessage(dlg,IDC_TASKS,LB_GETITEMDATA,(WPARAM)i,0);
}
static INT_PTR CALLBACK TaskProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    HWND h;
    switch(msg) {
    case WM_INITDIALOG: {
        HWND list[64]; RECT r; char title[128]; int n,i;
        list_dialog=dlg;
        n=windows(list,64,FALSE);
        for(i=0;i<n;i++) {
            LRESULT at;
            GetWindowText(list[i],title,sizeof(title));
            at=SendDlgItemMessage(dlg,IDC_TASKS,LB_ADDSTRING,0,(LPARAM)title);
            SendDlgItemMessage(dlg,IDC_TASKS,LB_SETITEMDATA,(WPARAM)at,(LPARAM)list[i]);
        }
        SendDlgItemMessage(dlg,IDC_TASKS,LB_SETCURSEL,0,0);
        GetWindowRect(dlg,&r);
        SetWindowPos(dlg,NULL,(GetSystemMetrics(SM_CXSCREEN)-(r.right-r.left))/2,
                     (GetSystemMetrics(SM_CYSCREEN)-(r.bottom-r.top))/3,0,0,SWP_NOSIZE|SWP_NOZORDER);
        return TRUE;
    }
    case WM_ACTIVATE:
        /* Another window taken: the Task List goes, as in Windows. */
        if(LOWORD(wp)==WA_INACTIVE && lp && !IsChild(dlg,(HWND)lp)) EndDialog(dlg,IDCANCEL);
        return FALSE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_TASKS:
            if(HIWORD(wp)!=LBN_DBLCLK) return FALSE;
            /* a double click switches */
        case IDC_SWITCH:
            EndDialog(dlg,IDOK);
            if((h=chosen(dlg))!=NULL && IsWindow(h)) {
                if(IsIconic(h)) ShowWindow(h,SW_RESTORE);
                SetActiveWindow(GetLastActivePopup(h));
            }
            return TRUE;
        case IDC_END:
            EndDialog(dlg,IDOK);
            if((h=chosen(dlg))!=NULL && IsWindow(h)) PostMessage(h,WM_CLOSE,0,0);
            return TRUE;
        case IDC_CASCADE: EndDialog(dlg,IDOK); cascade(); return TRUE;
        case IDC_TILE: EndDialog(dlg,IDOK); tile(); return TRUE;
        case IDC_ARRANGE: EndDialog(dlg,IDOK); ArrangeIconicWindows(GetDesktopWindow()); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
int PASCAL WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command_line,int show) {
    (void)previous; (void)command_line; (void)show;
    DialogBox(instance,"TASKLIST",NULL,TaskProc);
    return 0;
}
