/* SPDX-License-Identifier: GPL-2.0-or-later
 * CLOCK: the Windows 3.0 Clock. An analog face (60 marks, hour and minute
 * hands, a second hand) or a digital time drawn large, chosen from the
 * Settings menu with the second hand or seconds on or off. Minimized, it
 * draws its face as its icon (the class has no icon) and its title shows
 * the time. Settings and position live in WIN.INI [Clock].
 */
#include <windows.h>
#include "clock.h"

/* sin(i*6 degrees)*10000; cos(i) is sin(i+15). */
static const int sine[60]={
    0,1045,2079,3090,4067,5000,5878,6691,7431,8090,8660,9135,9511,9781,9945,10000,9945,9781,9511,9135,
    8660,8090,7431,6691,5878,5000,4067,3090,2079,1045,0,-1045,-2079,-3090,-4067,-5000,-5878,-6691,-7431,-8090,
    -8660,-9135,-9511,-9781,-9945,-10000,-9945,-9781,-9511,-9135,-8660,-8090,-7431,-6691,-5878,-5000,-4067,-3090,-2079,-1045};
static HWND main_wnd;
static BOOL digital,seconds=TRUE;
static SYSTEMTIME now;

static int sin60(int i) {return sine[((i%60)+60)%60];}
static int cos60(int i) {return sine[(((i+15)%60)+60)%60];}
static void time_text(char *out,BOOL with_seconds) {
    int h=now.wHour; BOOL h24=GetProfileInt("intl","iTime",0)!=0;
    if(!h24) {h%=12; if(!h) h=12;}
    if(with_seconds) wsprintf(out,"%d:%02d:%02d",h,now.wMinute,now.wSecond);
    else wsprintf(out,"%d:%02d",h,now.wMinute);
}

/* --- drawing ------------------------------------------------------------------------ */
static void hand(HDC dc,int cx,int cy,int at,int len,int width) {
    int s=sin60(at),c=cos60(at); POINT p[4];
    p[0].x=cx+s*len/10000; p[0].y=cy-c*len/10000;
    p[1].x=cx+(s*len/5+c*width)/10000; p[1].y=cy+(-c*len/5+s*width)/10000;
    p[2].x=cx-s*len/8/10000; p[2].y=cy+c*len/8/10000;
    p[3].x=cx+(s*len/5-c*width)/10000; p[3].y=cy+(-c*len/5-s*width)/10000;
    Polygon(dc,p,4);
}
static void analog(HDC dc,const RECT *r,BOOL icon) {
    int w=r->right-r->left,h=r->bottom-r->top,cx=r->left+w/2,cy=r->top+h/2,rad=min(w,h)/2-(icon?1:4),i,mark;
    HGDIOBJ old_pen,old_brush;
    if(rad<4) return;
    old_pen=SelectObject(dc,GetStockObject(BLACK_PEN));
    if(icon) {
        old_brush=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        Ellipse(dc,cx-rad,cy-rad,cx+rad+1,cy+rad+1);
    } else old_brush=SelectObject(dc,GetStockObject(BLACK_BRUSH));
    /* The marks: squares for hours, dots for minutes. */
    mark=max(1,rad/14);
    SelectObject(dc,GetStockObject(BLACK_BRUSH));
    for(i=0;i<60;i++) {
        int mr=rad*9/10,x=cx+sin60(i)*mr/10000,y=cy-cos60(i)*mr/10000;
        if(i%5==0) {if(icon) SetPixel(dc,x,y,RGB(0,0,0)); else Rectangle(dc,x-mark,y-mark,x+mark+1,y+mark+1);}
        else if(!icon && rad>=40) SetPixel(dc,x,y,RGB(0,0,0));
    }
    hand(dc,cx,cy,(now.wHour%12)*5+now.wMinute/12,rad*5/10,max(1,rad/14));
    hand(dc,cx,cy,now.wMinute,rad*8/10,max(1,rad/20));
    if(seconds && !icon) {
        int len=rad*85/100;
        MoveTo(dc,cx-sin60(now.wSecond)*len/8/10000,cy+cos60(now.wSecond)*len/8/10000);
        LineTo(dc,cx+sin60(now.wSecond)*len/10000,cy-cos60(now.wSecond)*len/10000);
    }
    SelectObject(dc,old_pen); SelectObject(dc,old_brush);
}
/* The time in Courier, drawn into a monochrome bitmap and stretched to fit. */
static void digital_face(HDC dc,const RECT *r) {
    char text[16]; HDC mem=CreateCompatibleDC(dc); HFONT font=CreateFont(15,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,0,0,0,FIXED_PITCH|FF_MODERN,"Courier");
    HBITMAP bm; SIZE s; int w=r->right-r->left,h=r->bottom-r->top,dw,dh;
    time_text(text,seconds);
    SelectObject(mem,font);
    GetTextExtentPoint(mem,text,lstrlen(text),&s);
    bm=CreateBitmap((int)s.cx,(int)s.cy,1,1,NULL);
    SelectObject(mem,bm);
    PatBlt(mem,0,0,(int)s.cx,(int)s.cy,WHITENESS);
    SetBkMode(mem,TRANSPARENT); SetTextColor(mem,RGB(0,0,0));
    TextOut(mem,0,0,text,lstrlen(text));
    /* As large as fits with a margin, keeping the characters' shape. */
    dw=w*9/10; dh=dw*(int)s.cy/(int)s.cx;
    if(dh>h*6/10) {dh=h*6/10; dw=dh*(int)s.cx/(int)s.cy;}
    SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); SetBkColor(dc,GetSysColor(COLOR_WINDOW));
    StretchBlt(dc,r->left+(w-dw)/2,r->top+(h-dh)/2,dw,dh,mem,0,0,(int)s.cx,(int)s.cy,SRCCOPY);
    DeleteDC(mem); DeleteObject(bm); DeleteObject(font);
}
static void paint(HWND h,HDC dc) {
    RECT r; HDC mem; HBITMAP bm; HGDIOBJ old; BOOL icon=IsIconic(h);
    GetClientRect(h,&r);
    if(r.right<=0 || r.bottom<=0) return;
    /* Drawn off screen, then copied, so the hands do not flicker. */
    mem=CreateCompatibleDC(dc); bm=CreateCompatibleBitmap(dc,r.right,r.bottom); old=SelectObject(mem,bm);
    FillRect(mem,&r,(HBRUSH)(ULONG_PTR)((icon?COLOR_BACKGROUND:COLOR_WINDOW)+1));
    if(icon || !digital) analog(mem,&r,icon); else digital_face(mem,&r);
    BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
    SelectObject(mem,old); DeleteObject(bm); DeleteDC(mem);
}

/* --- settings ------------------------------------------------------------------------ */
static void set_menu(void) {
    HMENU m=GetMenu(main_wnd);
    CheckMenuItem(m,IDM_ANALOG,digital?MF_UNCHECKED:MF_CHECKED);
    CheckMenuItem(m,IDM_DIGITAL,digital?MF_CHECKED:MF_UNCHECKED);
    CheckMenuItem(m,IDM_SECONDS,seconds?MF_CHECKED:MF_UNCHECKED);
}
static void save(HWND h) {
    char text[48]; RECT r;
    WriteProfileString("Clock","Mode",digital?"1":"0");
    WriteProfileString("Clock","Seconds",seconds?"1":"0");
    if(!IsIconic(h) && !IsZoomed(h)) {
        GetWindowRect(h,&r);
        wsprintf(text,"%d,%d,%d,%d",r.left,r.top,r.right-r.left,r.bottom-r.top);
        WriteProfileString("Clock","Position",text);
    }
}
static BOOL read_position(int *v) {
    char text[48]; const char *p=text; int i;
    if(!GetProfileString("Clock","Position","",text,sizeof(text))) return FALSE;
    for(i=0;i<4;i++) {
        BOOL neg=*p=='-'; if(neg) p++;
        if(*p<'0' || *p>'9') return FALSE;
        v[i]=0; while(*p>='0' && *p<='9') v[i]=v[i]*10+(*p++-'0');
        if(neg) v[i]=-v[i];
        if(i<3 && *p++!=',') return FALSE;
    }
    return v[2]>=60 && v[3]>=60 && v[0]<GetSystemMetrics(SM_CXSCREEN) && v[1]<GetSystemMetrics(SM_CYSCREEN);
}
static void title(HWND h) {
    char text[16];
    if(IsIconic(h)) {time_text(text,FALSE); SetWindowText(h,text);}
    else SetWindowText(h,"Clock");
}

LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: main_wnd=h; GetLocalTime(&now); SetTimer(h,1,1000,NULL); return 0;
    case WM_TIMER: {
        SYSTEMTIME t; GetLocalTime(&t);
        if(t.wSecond==now.wSecond && t.wMinute==now.wMinute) return 0;
        if(t.wMinute!=now.wMinute || t.wHour!=now.wHour) {now=t; if(IsIconic(h)) title(h); InvalidateRect(h,NULL,FALSE); return 0;}
        now=t;
        if(seconds && !IsIconic(h)) InvalidateRect(h,NULL,FALSE);
        return 0;
    }
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(h,dc); EndPaint(h,&ps); return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: title(h); InvalidateRect(h,NULL,FALSE); return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDM_ANALOG: digital=FALSE; break;
        case IDM_DIGITAL: digital=TRUE; break;
        case IDM_SECONDS: seconds=!seconds; break;
        case IDM_ABOUT: MessageBox(h,"Clock\nShows the time of day.","About Clock",MB_OK|MB_ICONINFORMATION); return 0;
        default: return 0;
        }
        set_menu(); InvalidateRect(h,NULL,FALSE);
        return 0;
    case WM_DESTROY: KillTimer(h,1); save(h); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; MSG m; int pos[4];
    (void)command;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.lpszMenuName="CLOCK"; wc.lpszClassName="Clock";
        RegisterClass(&wc);
    }
    digital=GetProfileInt("Clock","Mode",0)!=0;
    seconds=GetProfileInt("Clock","Seconds",1)!=0;
    if(read_position(pos))
        main_wnd=CreateWindow("Clock","Clock",WS_OVERLAPPEDWINDOW,pos[0],pos[1],pos[2],pos[3],NULL,NULL,inst,NULL);
    else
        main_wnd=CreateWindow("Clock","Clock",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,200,220,NULL,NULL,inst,NULL);
    set_menu();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    while(GetMessage(&m,NULL,0,0)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
