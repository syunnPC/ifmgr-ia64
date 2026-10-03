/* SPDX-License-Identifier: GPL-2.0-or-later
 * CLIPBRD: the Clipboard viewer. It joins the viewer chain and shows text
 * (scrolling when it is long), a bitmap or a picture (a metafile); Delete
 * empties the clipboard.
 */
#include <windows.h>
#include "clipbrd.h"
#include "winapp.h"

static HWND next_viewer;
static int top_line;

/* A picture plays in its mapping mode; an isotropic or anisotropic one at its
 * suggested size (hundredths of a millimetre), or filling the window (in its
 * proportions when they are given as negative extents). */
static void show_picture(HDC dc,const RECT *r,const METAFILEPICT *p) {
    int saved=SaveDC(dc);
    SetMapMode(dc,(int)p->mm);
    if(p->mm==MM_ISOTROPIC || p->mm==MM_ANISOTROPIC) {
        int w=r->right,h=r->bottom;
        if(p->xExt>0 && p->yExt>0) {
            w=MulDiv((int)p->xExt,GetDeviceCaps(dc,LOGPIXELSX),2540); h=MulDiv((int)p->yExt,GetDeviceCaps(dc,LOGPIXELSY),2540);
        } else if(p->xExt<0 && p->yExt<0) {
            if((LONG)w*-p->yExt>(LONG)h*-p->xExt) w=MulDiv(h,(int)-p->xExt,(int)-p->yExt); else h=MulDiv(w,(int)-p->yExt,(int)-p->xExt);
        }
        SetViewportExtEx(dc,w,h,NULL);
    }
    PlayMetaFile(dc,p->hMF);
    RestoreDC(dc,saved);
}
static void paint(HWND h,HDC dc) {
    RECT r; HGLOBAL g; TEXTMETRIC tm;
    GetClientRect(h,&r);
    GetTextMetrics(dc,&tm);
    if(!OpenClipboard(h)) return;
    if((g=GetClipboardData(CF_TEXT))!=NULL) {
        const char *text=(const char *)GlobalLock(g); int line=0,y=0; const char *p=text;
        while(p && *p && y<r.bottom) {
            const char *e=p; while(*e && *e!='\r' && *e!='\n') e++;
            if(line>=top_line) {TabbedTextOut(dc,2,y,p,(int)(e-p),0,NULL,2); y+=(int)tm.tmHeight;}
            line++;
            if(*e=='\r') e++;
            if(*e=='\n') e++;
            p=e;
        }
        GlobalUnlock(g);
    } else if((g=GetClipboardData(CF_BITMAP))!=NULL) {
        HDC mem=CreateCompatibleDC(dc); BITMAP b; HGDIOBJ old=SelectObject(mem,g);
        GetObject(g,sizeof(b),&b);
        BitBlt(dc,0,0,(int)b.bmWidth,(int)b.bmHeight,mem,0,0,SRCCOPY);
        SelectObject(mem,old); DeleteDC(mem);
    } else if((g=GetClipboardData(CF_METAFILEPICT))!=NULL) {
        const METAFILEPICT *p=(const METAFILEPICT *)GlobalLock(g);
        if(p) {show_picture(dc,&r,p); GlobalUnlock(g);}
    } else if(CountClipboardFormats()) {
        DrawText(dc,"The clipboard has data that Clipboard cannot show.",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    CloseClipboard();
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: next_viewer=SetClipboardViewer(h); SetScrollRange(h,SB_VERT,0,100,FALSE); return 0;
    case WM_DRAWCLIPBOARD:
        top_line=0; SetScrollPos(h,SB_VERT,0,TRUE);
        InvalidateRect(h,NULL,TRUE);
        if(next_viewer) SendMessage(next_viewer,msg,wp,lp);
        return 0;
    case WM_CHANGECBCHAIN:
        if((HWND)wp==next_viewer) next_viewer=(HWND)lp;
        else if(next_viewer) SendMessage(next_viewer,msg,wp,lp);
        return 0;
    case WM_VSCROLL:
        switch(LOWORD(wp)) {
        case SB_LINEUP: top_line--; break;
        case SB_LINEDOWN: top_line++; break;
        case SB_PAGEUP: top_line-=10; break;
        case SB_PAGEDOWN: top_line+=10; break;
        case SB_THUMBPOSITION: top_line=(short)HIWORD(wp); break;
        default: return 0;
        }
        if(top_line<0) top_line=0;
        if(top_line>100) top_line=100;
        SetScrollPos(h,SB_VERT,top_line,TRUE);
        InvalidateRect(h,NULL,TRUE);
        return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(h,dc); EndPaint(h,&ps); return 0;}
    case WM_COMMAND:
        if(HelpCommand(h,LOWORD(wp),"CLIPBRD.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return 0;
        case IDM_DELETE:
            if(CountClipboardFormats() && MessageBox(h,"Delete the contents of the clipboard?","Clipboard",MB_YESNO|MB_ICONQUESTION)==IDYES &&
               OpenClipboard(h)) {EmptyClipboard(); CloseClipboard();}
            return 0;
        case IDM_ABOUT: MessageBox(h,"Clipboard\nShows what was last cut or copied.","About Clipboard",MB_OK|MB_ICONINFORMATION); return 0;
        }
        return 0;
    case WM_DESTROY: WinHelp(h,"CLIPBRD.HLP",HELP_QUIT,0); ChangeClipboardChain(h,next_viewer); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; HWND h; MSG m; HACCEL accel;
    (void)command;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"CLIPBRD"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="CLIPBRD"; wc.lpszClassName="Clipboard";
        RegisterClass(&wc);
    }
    h=CreateWindow("Clipboard","Clipboard",WS_OVERLAPPEDWINDOW|WS_VSCROLL,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    ShowWindow(h,show); UpdateWindow(h);
    accel=LoadAccelerators(inst,"CLIPBRD");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(h,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
