/* SPDX-License-Identifier: GPL-2.0-or-later
 * HELPTEST: asks for help as programs do, through WinHelp (USER), one
 * request a click on its window (magenta, at the bottom right):
 *   1  HELP_CONTEXT, [MAP] number 100 of C:\TEST.HLP (WINHELP is started)
 *   2  HELP_KEY, the keyword "index"
 *   3  HELP_QUIT, and HELPTEST ends
 * Each says through OutputDebugString what WinHelp returned
 * ("HELPTEST: 1 ok" or "HELPTEST: 1 failed").
 */
#include <windows.h>

static int step;
static LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    char text[32]; BOOL ok=FALSE;
    switch(msg) {
    case WM_LBUTTONDOWN:
        switch(++step) {
        case 1: ok=WinHelp(h,"C:\\TEST.HLP",HELP_CONTEXT,100); break;
        case 2: ok=WinHelp(h,"C:\\TEST.HLP",HELP_KEY,(ULONG_PTR)"index"); break;
        case 3: ok=WinHelp(h,"C:\\TEST.HLP",HELP_QUIT,0); break;
        default: return 0;
        }
        wsprintf(text,"HELPTEST: %d %s",step,ok?"ok":"failed"); OutputDebugString(text);
        if(step==3) DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND: {
        RECT r; HBRUSH b=CreateSolidBrush(RGB(255,0,255));
        GetClientRect(h,&r); FillRect((HDC)wp,&r,b); DeleteObject(b);
        return 1;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HWND h;
    (void)command_line;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.lpszClassName="HelpTest";
        if(!RegisterClass(&wc)) return 0;
    }
    if(!(h=CreateWindow("HelpTest","Help Test",WS_POPUP|WS_BORDER,640,500,150,90,NULL,NULL,inst,NULL))) return 0;
    ShowWindow(h,show); UpdateWindow(h);
    while(GetMessage(&m,NULL,0,0)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
