/* SPDX-License-Identifier: GPL-2.0-or-later
 * The BUTTON, STATIC and SCROLLBAR controls in the Windows 3.0 look.
 * A button keeps its state in its first extra word: check state (bits
 * 0-1), pressed (bit 2), focus (bit 3).
 */
#include "user.h"
#define PRESSED 4
#define FOCUSED 8

static HFONT font_of(Wnd *w) {return w->font?w->font:SystemFont();}
static HBRUSH parent_color(Wnd *w,HDC dc,UINT msg) {
    HBRUSH b=NULL;
    if(w->parent && w->parent!=desktop) b=(HBRUSH)SendMessage(w->parent->handle,msg,(WPARAM)dc,(LPARAM)w->handle);
    return b?b:ControlColor(w,dc,msg);
}

/* --- buttons ------------------------------------------------------------------ */
static int kind(Wnd *w) {return (int)(w->style&0x0f);}
static void button_paint(Wnd *w,HDC dc) {
    RECT r,text_r; LONG_PTR state=GetWindowLongPtr(w->handle,0); int k=kind(w); BOOL gray=!Enabled(w);
    char text[256]; HGDIOBJ old_font; HBRUSH bg;
    GetClientRect(w->handle,&r);
    GetWindowText(w->handle,text,sizeof(text));
    old_font=SelectObject(dc,font_of(w));
    if(k==BS_OWNERDRAW) {
        DRAWITEMSTRUCT d;
        d.CtlType=ODT_BUTTON; d.CtlID=(UINT)w->id; d.itemID=0; d.itemAction=ODA_DRAWENTIRE;
        d.itemState=(state&PRESSED?ODS_SELECTED:0)|(state&FOCUSED?ODS_FOCUS:0)|(gray?ODS_DISABLED:0);
        d.hwndItem=w->handle; d.hDC=dc; d.rcItem=r; d.itemData=0;
        SendMessage(w->parent->handle,WM_DRAWITEM,w->id,(LPARAM)&d);
        SelectObject(dc,old_font);
        return;
    }
    if(k==BS_PUSHBUTTON || k==BS_DEFPUSHBUTTON || k==BS_USERBUTTON) {
        RECT f=r; BOOL pressed=(state&PRESSED)!=0; SIZE s; int x,y;
        bg=parent_color(w,dc,WM_CTLCOLORBTN);
        FillRect(dc,&r,bg);
        if(k==BS_DEFPUSHBUTTON) {FrameRect(dc,&f,SysBrush(COLOR_WINDOWFRAME)); InflateRect(&f,-1,-1);}
        DrawButtonFace(dc,&f,pressed);
        /* Rounded corners: the parent shows in each corner pixel. */
        {RECT c; SetRect(&c,r.left,r.top,r.left+1,r.top+1); FillRect(dc,&c,bg); OffsetRect(&c,r.right-r.left-1,0); FillRect(dc,&c,bg);
         OffsetRect(&c,0,r.bottom-r.top-1); FillRect(dc,&c,bg); OffsetRect(&c,-(r.right-r.left-1),0); FillRect(dc,&c,bg);}
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_BTNTEXT));
        s.cx=PrefixTextWidth(dc,text,-1); s.cy=CharHeight();
        x=(r.left+r.right-s.cx)/2+(pressed?1:0); y=(r.top+r.bottom-s.cy)/2+(pressed?1:0);
        DrawPrefixText(dc,x,y,text,-1,gray);
        if(state&FOCUSED) {SetRect(&text_r,x-2,y-1,x+s.cx+2,y+s.cy+1); IntersectRect(&text_r,&text_r,&f); DrawFocusRect(dc,&text_r);}
        SelectObject(dc,old_font);
        return;
    }
    bg=parent_color(w,dc,WM_CTLCOLORBTN);
    if(k==BS_GROUPBOX) {
        RECT f=r; SIZE s;
        /* Only the frame and the caption: the parent paints the inside. */
        f.top+=CharHeight()/2;
        FrameRect(dc,&f,SysBrush(COLOR_WINDOWFRAME));
        if(text[0]) {
            RECT t; s.cx=PrefixTextWidth(dc,text,-1);
            SetRect(&t,r.left+CharWidth()-2,r.top,r.left+CharWidth()+s.cx+2,r.top+CharHeight()); FillRect(dc,&t,bg);
            SetBkMode(dc,TRANSPARENT);
            DrawPrefixText(dc,r.left+CharWidth(),r.top,text,-1,gray);
        }
        SelectObject(dc,old_font);
        return;
    }
    /* Check boxes and radio buttons. */
    {
        int box=CharHeight()-1,cy=(r.top+r.bottom)/2,bx=w->style&BS_LEFTTEXT?r.right-box:r.left,tx,check=(int)(state&3); RECT b; SIZE s;
        BOOL radio=k==BS_RADIOBUTTON || k==BS_AUTORADIOBUTTON;
        FillRect(dc,&r,bg);
        SetRect(&b,bx,cy-box/2,bx+box,cy-box/2+box);
        if(radio) {
            HGDIOBJ op=SelectObject(dc,GetStockObject(BLACK_PEN)),ob=SelectObject(dc,state&PRESSED?SysBrush(COLOR_BTNFACE):GetStockObject(WHITE_BRUSH));
            Ellipse(dc,b.left,b.top,b.right,b.bottom);
            if(check) {SelectObject(dc,GetStockObject(BLACK_BRUSH)); Ellipse(dc,b.left+3,b.top+3,b.right-3,b.bottom-3);}
            SelectObject(dc,op); SelectObject(dc,ob);
        } else {
            FrameRect(dc,&b,SysBrush(COLOR_WINDOWFRAME));
            {RECT in=b; InflateRect(&in,-1,-1); FillRect(dc,&in,state&PRESSED?SysBrush(COLOR_BTNFACE):check==BST_INDETERMINATE?SysBrush(COLOR_GRAYTEXT):SysBrush(COLOR_WINDOW));}
            if(check==BST_CHECKED) {
                HGDIOBJ op=SelectObject(dc,GetStockObject(BLACK_PEN));
                MoveTo(dc,b.left+1,b.top+1); LineTo(dc,b.right-1,b.bottom-1);
                MoveTo(dc,b.right-2,b.top+1); LineTo(dc,b.left,b.bottom-1);
                SelectObject(dc,op);
            }
        }
        tx=w->style&BS_LEFTTEXT?r.left:bx+box+CharWidth()/2+2;
        SetBkMode(dc,TRANSPARENT);
        DrawPrefixText(dc,tx,cy-CharHeight()/2,text,-1,gray);
        if(state&FOCUSED && text[0]) {s.cx=PrefixTextWidth(dc,text,-1); SetRect(&b,tx-1,cy-CharHeight()/2-1,tx+s.cx+1,cy-CharHeight()/2+CharHeight()+1); DrawFocusRect(dc,&b);}
    }
    SelectObject(dc,old_font);
}
static void button_redraw(Wnd *w) {
    HDC dc;
    if(!Shown(w)) return;
    dc=GetDC(w->handle); button_paint(w,dc); ReleaseDC(w->handle,dc);
}
static void set_state(Wnd *w,LONG_PTR mask,LONG_PTR value) {
    LONG_PTR s=GetWindowLongPtr(w->handle,0),n=(s&~mask)|(value&mask);
    if(n!=s) {SetWindowLongPtr(w->handle,0,n); button_redraw(w);}
}
static void click(Wnd *w) {
    int k=kind(w); LONG_PTR s=GetWindowLongPtr(w->handle,0);
    if(k==BS_AUTOCHECKBOX) set_state(w,3,(s&3)?0:BST_CHECKED);
    else if(k==BS_AUTO3STATE) set_state(w,3,((s&3)+1)%3);
    else if(k==BS_AUTORADIOBUTTON) {
        /* Check it and uncheck the others in its group. */
        Wnd *p=w->parent,*c,*start=NULL;
        for(c=p->child;c;c=c->next) {if(c->style&WS_GROUP) start=c; if(c==w) break;}
        for(c=start?start:p->child;c;c=c->next) {
            if(c!=start && (c->style&WS_GROUP) && c!=w) break;
            if(c!=w && c->cls==w->cls && kind(c)==BS_AUTORADIOBUTTON) SendMessage(c->handle,BM_SETCHECK,0,0);
        }
        set_state(w,3,BST_CHECKED);
    }
    SendNotify(w,BN_CLICKED);
}
LRESULT CALLBACK ButtonProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h); LONG_PTR s;
    if(!w) return 0;
    s=GetWindowLongPtr(h,0);
    switch(msg) {
    case WM_CREATE: SetWindowLongPtr(h,0,0); return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); button_paint(w,dc); EndPaint(h,&ps); return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDBLCLK:
        if(kind(w)==BS_RADIOBUTTON || kind(w)==BS_AUTORADIOBUTTON || kind(w)==BS_USERBUTTON || kind(w)==BS_OWNERDRAW) {SendNotify(w,BN_DOUBLECLICKED); return 0;}
        /* fall through */
    case WM_LBUTTONDOWN:
        if(kind(w)==BS_GROUPBOX) return 0;
        SetFocus(h); SetCapture(h); set_state(w,PRESSED,PRESSED);
        return 0;
    case WM_MOUSEMOVE:
        if(GetCapture()==h) {RECT r; POINT p; GetClientRect(h,&r); p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); set_state(w,PRESSED,PtInRect(&r,p)?PRESSED:0);}
        return 0;
    case WM_LBUTTONUP:
        if(GetCapture()==h) {
            BOOL in=(GetWindowLongPtr(h,0)&PRESSED)!=0;
            ReleaseCapture(); set_state(w,PRESSED,0);
            if(in) click(w);
        } else if(!lp && !wp) {set_state(w,PRESSED,0); click(w);}
        return 0;
    case WM_KEYDOWN: if(wp==VK_SPACE) set_state(w,PRESSED,PRESSED); return 0;
    case WM_KEYUP: if(wp==VK_SPACE && (s&PRESSED)) {set_state(w,PRESSED,0); click(w);} return 0;
    case WM_CHAR:
        if(wp=='+' || wp=='=') {if(kind(w)==BS_AUTOCHECKBOX || kind(w)==BS_CHECKBOX) {if(!(s&3)) click(w);} return 0;}
        if(wp=='-') {if(kind(w)==BS_AUTOCHECKBOX || kind(w)==BS_CHECKBOX) {if(s&3) click(w);} return 0;}
        return 0;
    case WM_SETFOCUS: set_state(w,FOCUSED,FOCUSED); return 0;
    case WM_KILLFOCUS: set_state(w,FOCUSED|PRESSED,0); if(GetCapture()==h) ReleaseCapture(); return 0;
    case WM_CANCELMODE: if(GetCapture()==h) ReleaseCapture(); set_state(w,PRESSED,0); return 0;
    case WM_ENABLE: button_redraw(w); return 0;
    case WM_SETTEXT: DefWindowProc(h,msg,wp,lp); button_redraw(w); return TRUE;
    case WM_SETFONT: w->font=(HFONT)wp; if(lp) button_redraw(w); return 0;
    case WM_GETDLGCODE:
        switch(kind(w)) {
        case BS_DEFPUSHBUTTON: return DLGC_BUTTON|DLGC_DEFPUSHBUTTON;
        case BS_PUSHBUTTON: case BS_USERBUTTON: case BS_OWNERDRAW: return DLGC_BUTTON|DLGC_UNDEFPUSHBUTTON;
        case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON: return DLGC_BUTTON|DLGC_RADIOBUTTON;
        case BS_GROUPBOX: return DLGC_STATIC;
        }
        return DLGC_BUTTON;
    case BM_GETCHECK: return s&3;
    case BM_SETCHECK: {
        int k=kind(w);
        if(k==BS_CHECKBOX || k==BS_AUTOCHECKBOX || k==BS_3STATE || k==BS_AUTO3STATE || k==BS_RADIOBUTTON || k==BS_AUTORADIOBUTTON) {
            set_state(w,3,(LONG_PTR)(wp&3));
            /* The checked radio button is its group's tab stop. */
            if(k==BS_AUTORADIOBUTTON) {if(wp) w->style|=WS_TABSTOP; else w->style&=~WS_TABSTOP;}
        }
        return 0;
    }
    case BM_GETSTATE: return (s&3)|(s&PRESSED?BST_PUSHED:0)|(s&FOCUSED?BST_FOCUS:0);
    case BM_SETSTATE: set_state(w,PRESSED,wp?PRESSED:0); return 0;
    case BM_SETSTYLE: w->style=(w->style&~0x0fL)|(DWORD)(wp&0x0f); if(lp) button_redraw(w); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* --- static controls ------------------------------------------------------------- */
LRESULT CALLBACK StaticProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h); int k;
    if(!w) return 0;
    k=(int)(w->style&0x0f);
    switch(msg) {
    case WM_CREATE:
        if(k==SS_ICON) {
            LPCREATESTRUCT cs=(LPCREATESTRUCT)lp; HICON icon=NULL;
            if(cs->lpszName) {
                icon=LoadIcon(cs->hInstance,cs->lpszName);
                if(!icon) icon=LoadIcon(NULL,cs->lpszName);
            }
            SetWindowLongPtr(h,0,(LONG_PTR)icon);
            if(icon) SetWindowPos(h,NULL,0,0,ICON,ICON,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; HBRUSH bg; char text[1024]; HGDIOBJ old;
        GetClientRect(h,&r);
        switch(k) {
        case SS_ICON:
            bg=parent_color(w,dc,WM_CTLCOLORSTATIC); FillRect(dc,&r,bg);
            if(GetWindowLongPtr(h,0)) DrawIcon(dc,0,0,(HICON)GetWindowLongPtr(h,0));
            break;
        case SS_BLACKRECT: FillRect(dc,&r,SysBrush(COLOR_WINDOWFRAME)); break;
        case SS_GRAYRECT: FillRect(dc,&r,SysBrush(COLOR_BACKGROUND)); break;
        case SS_WHITERECT: FillRect(dc,&r,SysBrush(COLOR_WINDOW)); break;
        case SS_BLACKFRAME: FrameRect(dc,&r,SysBrush(COLOR_WINDOWFRAME)); break;
        case SS_GRAYFRAME: FrameRect(dc,&r,SysBrush(COLOR_BACKGROUND)); break;
        case SS_WHITEFRAME: FrameRect(dc,&r,SysBrush(COLOR_WINDOW)); break;
        case SS_USERITEM: break;
        default: {
            UINT f=k==SS_CENTER?DT_CENTER|DT_WORDBREAK:k==SS_RIGHT?DT_RIGHT|DT_WORDBREAK:k==SS_LEFT?DT_LEFT|DT_WORDBREAK:DT_LEFT|DT_SINGLELINE;
            bg=parent_color(w,dc,WM_CTLCOLORSTATIC);
            FillRect(dc,&r,bg);
            old=SelectObject(dc,font_of(w));
            GetWindowText(h,text,sizeof(text));
            SetBkMode(dc,TRANSPARENT);
            if(!Enabled(w)) SetTextColor(dc,GetSysColor(COLOR_GRAYTEXT));
            DrawText(dc,text,-1,&r,f|DT_EXPANDTABS*(k==SS_LEFTNOWORDWRAP)|(w->style&SS_NOPREFIX?DT_NOPREFIX:0));
            SelectObject(dc,old);
        }
        }
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT: DefWindowProc(h,msg,wp,lp); InvalidateRect(h,NULL,TRUE); UpdateWindow(h); return TRUE;
    case WM_ENABLE: InvalidateRect(h,NULL,TRUE); return 0;
    case WM_SETFONT: w->font=(HFONT)wp; if(lp) InvalidateRect(h,NULL,TRUE); return 0;
    case WM_GETDLGCODE: return DLGC_STATIC;
    case STM_SETICON: {LONG_PTR old=GetWindowLongPtr(h,0); SetWindowLongPtr(h,0,(LONG_PTR)wp); InvalidateRect(h,NULL,TRUE); return old;}
    case STM_GETICON: return GetWindowLongPtr(h,0);
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* --- scroll bar controls -------------------------------------------------------------- */
static void bar_redraw(Wnd *w) {
    HDC dc; RECT r;
    if(!Shown(w)) return;
    dc=GetDC(w->handle); GetClientRect(w->handle,&r);
    DrawScrollBar(dc,&r,(w->style&SBS_VERT)!=0,&w->scroll[0],-1,Enabled(w));
    ReleaseDC(w->handle,dc);
}
LRESULT CALLBACK ScrollBarProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h);
    if(!w) return 0;
    switch(msg) {
    case WM_CREATE: w->scroll[0].min=0; w->scroll[0].max=100; w->scroll[0].pos=0; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
        if(w->style&SBS_SIZEBOX) FillRect(dc,&r,SysBrush(COLOR_BTNFACE));
        else DrawScrollBar(dc,&r,(w->style&SBS_VERT)!=0,&w->scroll[0],-1,Enabled(w));
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        RECT r=w->client; POINT p; p.x=GET_X_LPARAM(lp)+w->client.left; p.y=GET_Y_LPARAM(lp)+w->client.top;
        if(w->style&SBS_SIZEBOX) return 0;
        if(w->style&WS_TABSTOP) SetFocus(h);
        TrackScroll(w,w->parent->handle,w,SB_CTL,&r,p);
        return 0;
    }
    case WM_KEYDOWN: {
        UINT m=w->style&SBS_VERT?WM_VSCROLL:WM_HSCROLL; int code=-1;
        switch(wp) {
        case VK_UP: case VK_LEFT: code=SB_LINEUP; break;
        case VK_DOWN: case VK_RIGHT: code=SB_LINEDOWN; break;
        case VK_PRIOR: code=SB_PAGEUP; break;
        case VK_NEXT: code=SB_PAGEDOWN; break;
        case VK_HOME: code=SB_TOP; break;
        case VK_END: code=SB_BOTTOM; break;
        }
        if(code>=0) {SendMessage(w->parent->handle,m,MAKEWPARAM(code,0),(LPARAM)h); SendMessage(w->parent->handle,m,MAKEWPARAM(SB_ENDSCROLL,0),(LPARAM)h);}
        return 0;
    }
    case WM_ENABLE: bar_redraw(w); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case SBM_SETPOS: {
        int old=w->scroll[0].pos;
        w->scroll[0].pos=max(w->scroll[0].min,min((int)wp,w->scroll[0].max));
        if(lp && old!=w->scroll[0].pos) bar_redraw(w);
        return old;
    }
    case SBM_GETPOS: return w->scroll[0].pos;
    case SBM_SETRANGE: case SBM_SETRANGEREDRAW:
        w->scroll[0].min=(int)wp; w->scroll[0].max=max((int)wp,(int)lp);
        w->scroll[0].pos=max(w->scroll[0].min,min(w->scroll[0].pos,w->scroll[0].max));
        if(msg==SBM_SETRANGEREDRAW) bar_redraw(w);
        return 0;
    case SBM_GETRANGE:
        if(wp) *(LPINT)wp=w->scroll[0].min;
        if(lp) *(LPINT)lp=w->scroll[0].max;
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
