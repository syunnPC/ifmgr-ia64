/* SPDX-License-Identifier: GPL-2.0-or-later
 * The non-client area in the Windows 3.0 look: frames, captions with the
 * system menu, minimize and maximize boxes, menu bars, scroll bars and icon
 * titles; hit testing; moving and sizing with an outline (mouse or
 * keyboard); the system commands; and scroll bars, shared with the
 * SCROLLBAR control.
 */
#include "user.h"

static void fill(HDC dc,int l,int t,int r,int b,int color) {
    RECT rc; SetRect(&rc,l,t,r,b); FillRect(dc,&rc,SysBrush(color));
}
static BOOL thick(Wnd *w) {return (w->style&WS_THICKFRAME) || ((w->style&WS_DLGFRAME) && !(w->style&WS_BORDER)) || (w->exstyle&WS_EX_DLGMODALFRAME);}
int FrameWidth(Wnd *w) {return thick(w)?FRAME:(w->style&(WS_BORDER|WS_DLGFRAME))?1:0;}
static BOOL has_caption(Wnd *w) {return (w->style&WS_CAPTION)==WS_CAPTION;}
static void DrawBorder3D(HDC dc,const RECT *r,BOOL sunken) {
    fill(dc,r->left,r->top,r->right-1,r->top+1,sunken?COLOR_BTNSHADOW:COLOR_BTNHIGHLIGHT);
    fill(dc,r->left,r->top,r->left+1,r->bottom-1,sunken?COLOR_BTNSHADOW:COLOR_BTNHIGHLIGHT);
    fill(dc,r->left,r->bottom-1,r->right,r->bottom,sunken?COLOR_BTNHIGHLIGHT:COLOR_BTNSHADOW);
    fill(dc,r->right-1,r->top,r->right,r->bottom,sunken?COLOR_BTNHIGHLIGHT:COLOR_BTNSHADOW);
}

/* --- scroll bars ------------------------------------------------------------- */
static void ScrollBarRects(Wnd *w,int bar,RECT *r) {
    if(bar==SB_VERT) SetRect(r,w->client.right,w->client.top-1,w->client.right+SCROLL,w->client.bottom+1);
    else SetRect(r,w->client.left-1,w->client.bottom,w->client.right+1,w->client.bottom+SCROLL);
    IntersectRect(r,r,&w->window);
}
static void arrow(HDC dc,int cx,int cy,int dir,BOOL gray) {
    int i;
    for(i=0;i<4;i++) {
        RECT r;
        switch(dir) {
        case 0: SetRect(&r,cx-i,cy-2+i,cx+i+1,cy-1+i); break;      /* up */
        case 1: SetRect(&r,cx-i,cy+1-i,cx+i+1,cy+2-i); break;      /* down */
        case 2: SetRect(&r,cx-2+i,cy-i,cx-1+i,cy+i+1); break;      /* left */
        default: SetRect(&r,cx+1-i,cy-i,cx+2-i,cy+i+1); break;     /* right */
        }
        FillRect(dc,&r,SysBrush(gray?COLOR_GRAYTEXT:COLOR_BTNTEXT));
    }
}
static void scroll_box(HDC dc,const RECT *r,int dir,BOOL pressed,BOOL enabled) {
    RECT in=*r;
    FrameRect(dc,r,SysBrush(COLOR_WINDOWFRAME));
    InflateRect(&in,-1,-1);
    FillRect(dc,&in,SysBrush(COLOR_BTNFACE));
    if(!pressed) {DrawBorder3D(dc,&in,FALSE);}
    else {RECT s=in; s.right=s.left+1; FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW)); s=in; s.bottom=s.top+1; FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW));}
    if(dir>=0) arrow(dc,(r->left+r->right)/2+(pressed?1:0),(r->top+r->bottom)/2+(pressed?1:0),dir,!enabled);
}
static int thumb_at(const RECT *r,BOOL vertical,const ScrollInfo *s) {
    int len=(vertical?r->bottom-r->top:r->right-r->left)-3*SCROLL+2,range=s->max-s->min;
    if(len<0 || range<=0) return -1;
    return SCROLL-1+(int)((LONGLONG)len*(min(max(s->pos,s->min),s->max)-s->min)/range);
}
void DrawScrollBar(HDC dc,const RECT *r,BOOL vertical,const ScrollInfo *s,int pressed,BOOL enabled) {
    RECT a,b,shaft; int t;
    if(vertical) {SetRect(&a,r->left,r->top,r->right,r->top+SCROLL); SetRect(&b,r->left,r->bottom-SCROLL,r->right,r->bottom);}
    else {SetRect(&a,r->left,r->top,r->left+SCROLL,r->bottom); SetRect(&b,r->right-SCROLL,r->top,r->right,r->bottom);}
    shaft=*r;
    if(vertical) {shaft.top=a.bottom-1; shaft.bottom=b.top+1;} else {shaft.left=a.right-1; shaft.right=b.left+1;}
    if(!IsRectEmpty(&shaft)) {
        FrameRect(dc,&shaft,SysBrush(COLOR_WINDOWFRAME)); InflateRect(&shaft,-1,-1);
        FillRect(dc,&shaft,SysBrush(COLOR_SCROLLBAR));
        if(pressed==SB_PAGEUP || pressed==SB_PAGEDOWN) {
            RECT p=shaft; int at=thumb_at(r,vertical,s);
            if(at>=0) {
                if(vertical) {if(pressed==SB_PAGEUP) p.bottom=r->top+at; else p.top=r->top+at+SCROLL;}
                else {if(pressed==SB_PAGEUP) p.right=r->left+at; else p.left=r->left+at+SCROLL;}
                if(!IsRectEmpty(&p)) InvertRect(dc,&p);
            }
        }
    }
    scroll_box(dc,&a,vertical?0:2,pressed==SB_LINEUP,enabled && s->pos>s->min);
    scroll_box(dc,&b,vertical?1:3,pressed==SB_LINEDOWN,enabled && s->pos<s->max);
    if(enabled && (t=thumb_at(r,vertical,s))>=0) {
        RECT th;
        if(vertical) SetRect(&th,r->left,r->top+t,r->right,r->top+t+SCROLL); else SetRect(&th,r->left+t,r->top,r->left+t+SCROLL,r->bottom);
        scroll_box(dc,&th,-1,FALSE,TRUE);
    }
}
static int ScrollHit(const RECT *r,BOOL vertical,const ScrollInfo *s,POINT p) {
    int at,off;
    if(!PtInRect(r,p)) return -1;
    off=vertical?p.y-r->top:p.x-r->left;
    if(off<SCROLL) return SB_LINEUP;
    if(off>=(vertical?r->bottom-r->top:r->right-r->left)-SCROLL) return SB_LINEDOWN;
    at=thumb_at(r,vertical,s);
    if(at<0) return -1;
    if(off<at) return SB_PAGEUP;
    if(off>=at+SCROLL) return SB_PAGEDOWN;
    return SB_THUMBTRACK;
}
static void draw_window_bar(Wnd *w,int bar,int pressed) {
    HDC dc; RECT r;
    if(!(w->style&(bar==SB_VERT?WS_VSCROLL:WS_HSCROLL)) || w->scroll[bar].hidden || !Shown(w)) return;
    dc=GetWindowDC(w->handle);
    ScrollBarRects(w,bar,&r); OffsetRect(&r,-w->window.left,-w->window.top);
    DrawScrollBar(dc,&r,bar==SB_VERT,&w->scroll[bar],pressed,Enabled(w));
    ReleaseDC(w->handle,dc);
}
/* Follow a press on a scroll bar until the button is released: arrows and
 * the shaft repeat, the thumb is dragged. */
void TrackScroll(Wnd *owner,HWND notify,Wnd *control,int bar,const RECT *r,POINT start) {
    BOOL vertical=control?(control->style&SBS_VERT)!=0:bar==SB_VERT;
    ScrollInfo *s=control?&control->scroll[0]:&owner->scroll[bar];
    UINT msg=vertical?WM_VSCROLL:WM_HSCROLL; HWND h=owner->handle,from=control?control->handle:NULL;
    int hit=ScrollHit(r,vertical,s,start),pos=s->pos,offset=0,len; MSG m; DWORD next; BOOL inside=TRUE;
    ScrollInfo shown=*s;
    if(hit<0) return;
    len=(vertical?r->bottom-r->top:r->right-r->left)-3*SCROLL+2;
    if(hit==SB_THUMBTRACK) offset=(vertical?start.y-r->top:start.x-r->left)-thumb_at(r,vertical,s);
    SetCapture(h);
    SetTimer(h,0x7ffe,50,NULL);
#define REDRAW(p) do {HDC dc_=control?GetDC(h):GetWindowDC(h); RECT rr=*r; \
        if(control) OffsetRect(&rr,-owner->client.left,-owner->client.top); else OffsetRect(&rr,-owner->window.left,-owner->window.top); \
        DrawScrollBar(dc_,&rr,vertical,&shown,p,TRUE); ReleaseDC(h,dc_);} while(0)
    if(hit!=SB_THUMBTRACK) {SendMessage(notify,msg,MAKEWPARAM(hit,0),(LPARAM)from); shown=*s; REDRAW(hit);}
    next=GetTickCount()+400;
    while(KeyDown(VK_LBUTTON)) {
        if(!GetMessage(&m,NULL,0,0)) {PostQuitMessage((int)m.wParam); break;}
        if(CallMsgFilter(&m,MSGF_SCROLLBAR)) continue;
        if(m.message==WM_LBUTTONUP || m.message==WM_NCLBUTTONUP) break;
        if(m.message==WM_MOUSEMOVE || (m.message==WM_TIMER && m.hwnd==h && m.wParam==0x7ffe)) {
            POINT p; GetCursorPos(&p);
            if(hit==SB_THUMBTRACK) {
                int off=(vertical?p.y-r->top:p.x-r->left)-offset-(SCROLL-1),range=s->max-s->min,np;
                np=len>0?s->min+(int)((LONGLONG)range*max(0,min(off,len))/len):s->min;
                if(np!=pos) {pos=np; shown.pos=pos; REDRAW(-1); SendMessage(notify,msg,MAKEWPARAM(SB_THUMBTRACK,pos),(LPARAM)from);}
            } else {
                inside=ScrollHit(r,vertical,s,p)==hit;
                if(m.message==WM_TIMER && inside && (LONG)(GetTickCount()-next)>=0) {
                    SendMessage(notify,msg,MAKEWPARAM(hit,0),(LPARAM)from); shown=*s; next=GetTickCount()+50;
                }
                REDRAW(inside?hit:-1);
            }
            continue;
        }
        if(m.message>=WM_MOUSEFIRST && m.message<=WM_MOUSELAST) continue;
        if(m.message>=WM_KEYFIRST && m.message<=WM_KEYLAST) continue;
        TranslateMessage(&m); DispatchMessage(&m);
    }
    KillTimer(h,0x7ffe);
    ReleaseCapture();
    if(hit==SB_THUMBTRACK) SendMessage(notify,msg,MAKEWPARAM(SB_THUMBPOSITION,pos),(LPARAM)from);
    SendMessage(notify,msg,MAKEWPARAM(SB_ENDSCROLL,0),(LPARAM)from);
    if(IsWindow(h)) {shown=*s; REDRAW(-1);}
#undef REDRAW
}
static void scroll_changed(Wnd *w,int bar,BOOL redraw) {
    RECT old=w->client;
    w->scroll[bar].hidden=w->scroll[bar].min==w->scroll[bar].max;
    CalcClient(w);
    if(!EqualRect(&old,&w->client)) {
        RECT r=w->window; OffsetRect(&r,-w->parent->client.left,-w->parent->client.top);
        w->client=old; PlaceWindow(w,&r,TRUE);
        SendMessage(w->handle,WM_SIZE,SIZE_RESTORED,MAKELPARAM(w->client.right-w->client.left,w->client.bottom-w->client.top));
    } else if(redraw) draw_window_bar(w,bar,-1);
}
int WINAPI SetScrollPos(HWND h,int bar,int pos,BOOL redraw) {
    Wnd *w=WndFromHandle(h); int old;
    if(!w) return 0;
    if(bar==SB_CTL) return (int)SendMessage(h,SBM_SETPOS,(WPARAM)pos,redraw);
    if(bar!=SB_HORZ && bar!=SB_VERT) return 0;
    old=w->scroll[bar].pos;
    w->scroll[bar].pos=max(w->scroll[bar].min,min(pos,w->scroll[bar].max));
    if(redraw && old!=w->scroll[bar].pos) draw_window_bar(w,bar,-1);
    return old;
}
int WINAPI GetScrollPos(HWND h,int bar) {
    Wnd *w=WndFromHandle(h);
    if(!w) return 0;
    if(bar==SB_CTL) return (int)SendMessage(h,SBM_GETPOS,0,0);
    return bar==SB_HORZ || bar==SB_VERT?w->scroll[bar].pos:0;
}
void WINAPI SetScrollRange(HWND h,int bar,int low,int high,BOOL redraw) {
    Wnd *w=WndFromHandle(h);
    if(!w) return;
    if(bar==SB_CTL) {SendMessage(h,redraw?SBM_SETRANGEREDRAW:SBM_SETRANGE,(WPARAM)low,(LPARAM)high); return;}
    if(bar!=SB_HORZ && bar!=SB_VERT) return;
    w->scroll[bar].min=low; w->scroll[bar].max=max(low,high);
    w->scroll[bar].pos=max(low,min(w->scroll[bar].pos,w->scroll[bar].max));
    if(w->style&(bar==SB_VERT?WS_VSCROLL:WS_HSCROLL)) scroll_changed(w,bar,redraw);
}
void WINAPI GetScrollRange(HWND h,int bar,LPINT low,LPINT high) {
    Wnd *w=WndFromHandle(h);
    if(!w) return;
    if(bar==SB_CTL) {SendMessage(h,SBM_GETRANGE,(WPARAM)low,(LPARAM)high); return;}
    if(bar!=SB_HORZ && bar!=SB_VERT) return;
    if(low) *low=w->scroll[bar].min;
    if(high) *high=w->scroll[bar].max;
}
void WINAPI ShowScrollBar(HWND h,int bar,BOOL show) {
    Wnd *w=WndFromHandle(h); int i;
    if(!w) return;
    if(bar==SB_CTL) {ShowWindow(h,show?SW_SHOW:SW_HIDE); return;}
    for(i=0;i<2;i++) if(bar==SB_BOTH || bar==i) {
        DWORD flag=i==SB_VERT?WS_VSCROLL:WS_HSCROLL; RECT old=w->client,r;
        if(show) w->style|=flag; else w->style&=~flag;
        w->scroll[i].hidden=FALSE;
        CalcClient(w);
        if(!EqualRect(&old,&w->client)) {
            r=w->window; OffsetRect(&r,-w->parent->client.left,-w->parent->client.top);
            w->client=old; PlaceWindow(w,&r,TRUE);
        }
    }
}

/* --- frames and captions --------------------------------------------------------- */
static void caption_box(HDC dc,int l,int t,int kind) {
    int cx=l+BOXSIZE/2,cy=t+(CAPTION-2)/2,i;
    fill(dc,l,t,l+BOXSIZE,t+CAPTION-2,COLOR_BTNFACE);
    if(kind==0) {
        fill(dc,l+3,cy-1,l+BOXSIZE-3,cy+2,COLOR_BTNHIGHLIGHT);
        fill(dc,l+4,cy+2,l+BOXSIZE-2,cy+3,COLOR_BTNSHADOW); fill(dc,l+BOXSIZE-3,cy,l+BOXSIZE-2,cy+3,COLOR_BTNSHADOW);
        return;
    }
    if(kind==4) { /* the system menu of a child window: a short bar */
        fill(dc,l+4,cy-1,l+BOXSIZE-4,cy+1,COLOR_BTNHIGHLIGHT); fill(dc,l+5,cy+1,l+BOXSIZE-3,cy+2,COLOR_BTNSHADOW);
        return;
    }
    {RECT r; SetRect(&r,l,t,l+BOXSIZE,t+CAPTION-2); DrawBorder3D(dc,&r,FALSE);}
    if(kind==3) { /* restore: up and down */
        for(i=0;i<4;i++) {fill(dc,cx-i,cy-4+i,cx+i+1,cy-3+i,COLOR_BTNTEXT); fill(dc,cx-i,cy+4-i,cx+i+1,cy+5-i,COLOR_BTNTEXT);}
        return;
    }
    for(i=0;i<5;i++) {
        int y=kind==1?cy-2+i:cy+2-i; /* maximize points up, minimize down */
        fill(dc,cx-i,y,cx+i+1,y+1,COLOR_BTNTEXT);
    }
}
static void paint_icon_title(Wnd *w,HDC dc) {
    RECT t; int W=w->window.right-w->window.left,H=w->window.bottom-w->window.top; HBRUSH bg;
    HFONT old; int len=w->text?lstrlen(w->text):0,x;
    bg=w->parent==desktop?SysBrush(COLOR_BACKGROUND):SysBrush(COLOR_APPWORKSPACE);
    {   /* Around the icon square, the parent shows. */
        RECT a; SetRect(&a,0,0,(W-ICON)/2,ICON); FillRect(dc,&a,bg);
        SetRect(&a,(W+ICON)/2,0,W,ICON); FillRect(dc,&a,bg);
        SetRect(&a,0,ICON,W,H); FillRect(dc,&a,bg);
    }
    old=SelectObject(dc,SystemFont());
    /* The title wraps at spaces into at most two lines. */
    SetRect(&t,1,ICON+3,W-1,ICON+3+2*CharHeight());
    DrawText(dc,w->text?w->text:"",len,&t,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX|DT_CALCRECT);
    if(t.bottom>ICON+3+2*CharHeight()) t.bottom=ICON+3+2*CharHeight();
    x=max(1,(W-(t.right-t.left))/2); OffsetRect(&t,x-t.left,0); t.right=min(t.right,W-1);
    if(w==active) {RECT h=t; InflateRect(&h,2,1); FillRect(dc,&h,SysBrush(COLOR_ACTIVECAPTION)); SetTextColor(dc,GetSysColor(COLOR_CAPTIONTEXT));}
    else SetTextColor(dc,w->parent==desktop?RGB(0,0,0):GetSysColor(COLOR_WINDOWTEXT));
    SetBkMode(dc,TRANSPARENT);
    {RECT c=t; c.left=1; c.right=W-1; DrawText(dc,w->text?w->text:"",len,&c,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);}
    SelectObject(dc,old);
}
void PaintFrame(Wnd *w) {
    HDC dc; int W=w->window.right-w->window.left,H=w->window.bottom-w->window.top,b,left,right,isactive;
    BOOL child_active=FALSE;
    if(w==desktop || !(dc=GetWindowDC(w->handle))) return;
    isactive=w==active;
    /* An MDI child is drawn active when it is its client's active child. */
    if((w->style&WS_CHILD) && w->parent && w->parent->cls && !lstrcmpi(w->parent->cls->name,"MDICLIENT")) {
        child_active=(HWND)SendMessage(w->parent->handle,WM_MDIGETACTIVE,0,0)==w->handle && TopLevel(w)==active;
        isactive=child_active;
    }
    if(w->style&WS_MINIMIZE) {paint_icon_title(w,dc); ReleaseDC(w->handle,dc); return;}
    b=FrameWidth(w);
    if(b==FRAME) {
        fill(dc,0,0,W,H,COLOR_WINDOWFRAME);
        fill(dc,1,1,W-1,H-1,isactive?COLOR_ACTIVEBORDER:COLOR_INACTIVEBORDER);
        fill(dc,b-1,b-1,W-b+1,H-b+1,COLOR_WINDOWFRAME);
        if(w->style&WS_THICKFRAME) {
            /* The corner marks of a sizable frame. */
            int m=CAPTION+FRAME-1;
            fill(dc,m,0,m+1,b,COLOR_WINDOWFRAME); fill(dc,W-m-1,0,W-m,b,COLOR_WINDOWFRAME);
            fill(dc,m,H-b,m+1,H,COLOR_WINDOWFRAME); fill(dc,W-m-1,H-b,W-m,H,COLOR_WINDOWFRAME);
            fill(dc,0,m,b,m+1,COLOR_WINDOWFRAME); fill(dc,0,H-m-1,b,H-m,COLOR_WINDOWFRAME);
            fill(dc,W-b,m,W,m+1,COLOR_WINDOWFRAME); fill(dc,W-b,H-m-1,W,H-m,COLOR_WINDOWFRAME);
        }
    } else if(b) fill(dc,0,0,W,H,COLOR_WINDOWFRAME);
    if(has_caption(w)) {
        int top=b,bottom=b+CAPTION-1; RECT t;
        left=b; right=W-b;
        fill(dc,left,bottom-1,right,bottom,COLOR_WINDOWFRAME);
        bottom--;
        if(w->style&WS_SYSMENU) {caption_box(dc,left,top,w->style&WS_CHILD?4:0); fill(dc,left+BOXSIZE,top,left+BOXSIZE+1,bottom,COLOR_WINDOWFRAME); left+=BOXSIZE+1;}
        if(w->style&WS_MAXIMIZEBOX) {right-=BOXSIZE; caption_box(dc,right,top,w->style&WS_MAXIMIZE?3:1); fill(dc,right-1,top,right,bottom,COLOR_WINDOWFRAME); right--;}
        if(w->style&WS_MINIMIZEBOX) {right-=BOXSIZE; caption_box(dc,right,top,2); fill(dc,right-1,top,right,bottom,COLOR_WINDOWFRAME); right--;}
        fill(dc,left,top,right,bottom,isactive?COLOR_ACTIVECAPTION:COLOR_INACTIVECAPTION);
        SelectObject(dc,SystemFont());
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,GetSysColor(isactive?COLOR_CAPTIONTEXT:COLOR_INACTIVECAPTIONTEXT));
        SetRect(&t,left,top,right,bottom);
        DrawText(dc,w->text?w->text:"",-1,&t,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    }
    if(!(w->style&WS_CHILD) && w->menu) PaintMenuBar(w,dc);
    {
        BOOL v=(w->style&WS_VSCROLL) && !w->scroll[SB_VERT].hidden,hz=(w->style&WS_HSCROLL) && !w->scroll[SB_HORZ].hidden; RECT r;
        if(v) {ScrollBarRects(w,SB_VERT,&r); OffsetRect(&r,-w->window.left,-w->window.top); DrawScrollBar(dc,&r,TRUE,&w->scroll[SB_VERT],-1,Enabled(w));}
        if(hz) {ScrollBarRects(w,SB_HORZ,&r); OffsetRect(&r,-w->window.left,-w->window.top); DrawScrollBar(dc,&r,FALSE,&w->scroll[SB_HORZ],-1,Enabled(w));}
        if(v && hz) {
            SetRect(&r,w->client.right,w->client.bottom,w->client.right+SCROLL-1,w->client.bottom+SCROLL-1);
            IntersectRect(&r,&r,&w->window); OffsetRect(&r,-w->window.left,-w->window.top);
            FillRect(dc,&r,SysBrush(COLOR_BTNFACE));
        }
    }
    ReleaseDC(w->handle,dc);
}

/* --- hit testing --------------------------------------------------------------------- */
int HitTest(Wnd *w,POINT p) {
    RECT r=w->window,m; int b=FrameWidth(w);
    if(!PtInRect(&r,p)) return HTNOWHERE;
    if(w->style&WS_MINIMIZE) return HTCAPTION;
    if(PtInRect(&w->client,p)) return HTCLIENT;
    if((w->style&WS_THICKFRAME) && !(w->style&WS_MAXIMIZE) && (p.x<r.left+b || p.x>=r.right-b || p.y<r.top+b || p.y>=r.bottom-b)) {
        int c=CAPTION+FRAME-1;
        int left=p.x<r.left+c,right=p.x>=r.right-c,top=p.y<r.top+c,bottom=p.y>=r.bottom-c;
        if(top && left) return HTTOPLEFT;
        if(top && right) return HTTOPRIGHT;
        if(bottom && left) return HTBOTTOMLEFT;
        if(bottom && right) return HTBOTTOMRIGHT;
        if(p.x<r.left+b) return HTLEFT;
        if(p.x>=r.right-b) return HTRIGHT;
        return p.y<r.top+b?HTTOP:HTBOTTOM;
    }
    if(has_caption(w) && p.y<r.top+b+CAPTION-1) {
        if(w->style&WS_SYSMENU && p.x<r.left+b+BOXSIZE) return HTSYSMENU;
        if(w->style&WS_MAXIMIZEBOX && p.x>=r.right-b-BOXSIZE) return HTMAXBUTTON;
        if(w->style&WS_MINIMIZEBOX && p.x>=r.right-b-((w->style&WS_MAXIMIZEBOX)?2*BOXSIZE+1:BOXSIZE)) return HTMINBUTTON;
        return HTCAPTION;
    }
    if(!(w->style&WS_CHILD) && w->menu) {MenuBarRect(w,&m); if(PtInRect(&m,p)) return HTMENU;}
    if((w->style&WS_VSCROLL) && !w->scroll[SB_VERT].hidden) {ScrollBarRects(w,SB_VERT,&m); if(PtInRect(&m,p)) return HTVSCROLL;}
    if((w->style&WS_HSCROLL) && !w->scroll[SB_HORZ].hidden) {ScrollBarRects(w,SB_HORZ,&m); if(PtInRect(&m,p)) return HTHSCROLL;}
    if((w->style&WS_VSCROLL) && (w->style&WS_HSCROLL) && p.x>=w->client.right && p.y>=w->client.bottom) return (w->style&WS_THICKFRAME)?HTGROWBOX:HTBORDER;
    return HTBORDER;
}

/* --- min/max ------------------------------------------------------------------------- */
static void MinMaxInfo(Wnd *w,MINMAXINFO *mm) {
    Wnd *p=w->parent; int b=FrameWidth(w);
    memset(mm,0,sizeof(*mm));
    mm->ptMaxSize.x=p->client.right-p->client.left+2*b; mm->ptMaxSize.y=p->client.bottom-p->client.top+2*b;
    mm->ptMaxPosition.x=-b; mm->ptMaxPosition.y=-b;
    mm->ptMinTrackSize.x=GetSystemMetrics(SM_CXMINTRACK); mm->ptMinTrackSize.y=GetSystemMetrics(SM_CYMINTRACK);
    mm->ptMaxTrackSize.x=screen_width+2*b; mm->ptMaxTrackSize.y=screen_height+2*b;
    SendMessage(w->handle,WM_GETMINMAXINFO,0,(LPARAM)mm);
}
void MaximizedRect(Wnd *w,RECT *r) {
    MINMAXINFO mm; MinMaxInfo(w,&mm);
    SetRect(r,mm.ptMaxPosition.x,mm.ptMaxPosition.y,mm.ptMaxPosition.x+mm.ptMaxSize.x,mm.ptMaxPosition.y+mm.ptMaxSize.y);
}

/* --- moving and sizing ------------------------------------------------------------------ */
typedef struct {int hit; POINT start; RECT orig,cur; HDC dc; int t; BOOL drawn; MINMAXINFO mm;} Track;
static HBRUSH halftone;
static void outline(Track *k) {
    RECT r=k->cur; int t=k->t; HGDIOBJ old=SelectObject(k->dc,halftone);
    PatBlt(k->dc,r.left,r.top,r.right-r.left,t,PATINVERT);
    PatBlt(k->dc,r.left,r.bottom-t,r.right-r.left,t,PATINVERT);
    PatBlt(k->dc,r.left,r.top+t,t,r.bottom-r.top-2*t,PATINVERT);
    PatBlt(k->dc,r.right-t,r.top+t,t,r.bottom-r.top-2*t,PATINVERT);
    SelectObject(k->dc,old);
    k->drawn=!k->drawn;
}
static void track_to(Track *k,POINT p) {
    RECT r=k->orig; int dx=p.x-k->start.x,dy=p.y-k->start.y;
    switch(k->hit) {
    case HTCAPTION: OffsetRect(&r,dx,dy); break;
    case HTLEFT: case HTTOPLEFT: case HTBOTTOMLEFT: r.left+=dx; break;
    case HTRIGHT: case HTTOPRIGHT: case HTBOTTOMRIGHT: case HTGROWBOX: r.right+=dx; break;
    }
    switch(k->hit) {
    case HTTOP: case HTTOPLEFT: case HTTOPRIGHT: r.top+=dy; break;
    case HTBOTTOM: case HTBOTTOMLEFT: case HTBOTTOMRIGHT: case HTGROWBOX: r.bottom+=dy; break;
    }
    if(k->hit!=HTCAPTION) {
        int minw=k->mm.ptMinTrackSize.x,minh=k->mm.ptMinTrackSize.y;
        if(r.right-r.left<minw) {if(r.left!=k->orig.left) r.left=r.right-minw; else r.right=r.left+minw;}
        if(r.bottom-r.top<minh) {if(r.top!=k->orig.top) r.top=r.bottom-minh; else r.bottom=r.top+minh;}
    }
    if(EqualRect(&r,&k->cur) && k->drawn) return;
    if(k->drawn) outline(k);
    k->cur=r; outline(k);
}
/* Drag an outline with the mouse (or the arrow keys); Enter or the button's
 * release accepts, Escape cancels. */
static BOOL track(Wnd *w,int hit,POINT start,BOOL keyboard) {
    Track k; MSG m; BOOL ok=TRUE,moved=FALSE; HWND h=w->handle;
    memset(&k,0,sizeof(k));
    k.hit=hit; k.start=start; k.orig=k.cur=w->window;
    k.t=w->style&WS_MINIMIZE?2:FRAME;
    MinMaxInfo(w,&k.mm);
    k.dc=GdiCreateScreenDC();
    if(!k.dc) return FALSE;
    if(w->parent!=desktop) IntersectClipRect(k.dc,w->parent->client.left,w->parent->client.top,w->parent->client.right,w->parent->client.bottom);
    if(!keyboard && !KeyDown(VK_LBUTTON)) {DeleteDC(k.dc); return FALSE;}
    SetCapture(h);
    if(keyboard) {track_to(&k,start); SetCursorPos(start.x,start.y);}
    for(;;) {
        if(!keyboard && !KeyDown(VK_LBUTTON)) {
            POINT p; GetCursorPos(&p);
            if(p.x!=start.x || p.y!=start.y) {moved=TRUE; track_to(&k,p);}
            break;
        }
        if(!GetMessage(&m,NULL,0,0)) {PostQuitMessage((int)m.wParam); ok=FALSE; break;}
        if(CallMsgFilter(&m,hit==HTCAPTION || (w->style&WS_MINIMIZE)?MSGF_MOVE:MSGF_SIZE)) continue;
        if(m.message==WM_MOUSEMOVE) {
            POINT p; GetCursorPos(&p);
            if(p.x!=start.x || p.y!=start.y) moved=TRUE;
            if(moved) track_to(&k,p);
            continue;
        }
        if(m.message==WM_LBUTTONUP || m.message==WM_NCLBUTTONUP) {if(!keyboard) break; continue;}
        if(m.message==WM_LBUTTONDOWN && keyboard) break;
        if(m.message==WM_KEYDOWN) {
            POINT p; GetCursorPos(&p);
            switch(m.wParam) {
            case VK_ESCAPE: ok=FALSE; goto done;
            case VK_RETURN: goto done;
            case VK_LEFT: p.x-=8; break;
            case VK_RIGHT: p.x+=8; break;
            case VK_UP: p.y-=8; break;
            case VK_DOWN: p.y+=8; break;
            }
            SetCursorPos(p.x,p.y); moved=TRUE; track_to(&k,p);
            continue;
        }
        if(m.message>=WM_MOUSEFIRST && m.message<=WM_MOUSELAST) continue;
        if(m.message>=WM_KEYFIRST && m.message<=WM_KEYLAST) continue;
        TranslateMessage(&m); DispatchMessage(&m);
        if(!IsWindow(h)) {ok=FALSE; break;}
    }
done:
    if(k.drawn) outline(&k);
    DeleteDC(k.dc);
    ReleaseCapture();
    if(!ok || !IsWindow(h) || !moved || EqualRect(&k.cur,&k.orig)) return moved && ok;
    {
        RECT r=k.cur;
        if(HookActive(WH_CBT) && (CallHook(WH_CBT,HCBT_MOVESIZE,(WPARAM)h,(LPARAM)&r) || !IsWindow(h))) return FALSE;
        OffsetRect(&r,-w->parent->client.left,-w->parent->client.top);
        if(w->style&WS_MINIMIZE) {w->icon.x=r.left; w->icon.y=r.top-4; w->icon_placed=TRUE; PlaceWindow(w,&r,FALSE);}
        else SetWindowPos(h,NULL,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    return TRUE;
}
/* A caption box pressed with the mouse; TRUE when released over it. */
static BOOL press_box(Wnd *w,int hit) {
    MSG m; BOOL in=TRUE; HWND h=w->handle; RECT box;
    int b=FrameWidth(w),right=w->window.right-b;
    if(hit==HTMAXBUTTON) SetRect(&box,right-BOXSIZE,w->window.top+b,right,w->window.top+b+CAPTION-2);
    else {int r2=(w->style&WS_MAXIMIZEBOX)?right-BOXSIZE-1:right; SetRect(&box,r2-BOXSIZE,w->window.top+b,r2,w->window.top+b+CAPTION-2);}
    SetCapture(h);
#define SHOW(pressed) do {HDC dc=GetWindowDC(h); RECT rr=box; OffsetRect(&rr,-w->window.left,-w->window.top); \
        if(pressed) InvertRect(dc,&rr); else PaintFrame(w); ReleaseDC(h,dc);} while(0)
    SHOW(TRUE);
    while(KeyDown(VK_LBUTTON)) {
        if(!GetMessage(&m,NULL,0,0)) {PostQuitMessage((int)m.wParam); break;}
        if(m.message==WM_LBUTTONUP || m.message==WM_NCLBUTTONUP) break;
        if(m.message==WM_MOUSEMOVE) {
            POINT p; BOOL now; GetCursorPos(&p); now=PtInRect(&box,p);
            if(now!=in) {in=now; if(in) SHOW(TRUE); else PaintFrame(w);}
            continue;
        }
        if(m.message>=WM_MOUSEFIRST && m.message<=WM_MOUSELAST) continue;
        TranslateMessage(&m); DispatchMessage(&m);
    }
    ReleaseCapture();
    {POINT p; GetCursorPos(&p); in=PtInRect(&box,p);}
    if(IsWindow(h)) PaintFrame(w);
#undef SHOW
    return in;
}
LRESULT NcButtonDown(Wnd *w,int hit,POINT p,BOOL dbl) {
    HWND h=w->handle;
    if(dbl) {
        if(hit==HTCAPTION && (w->style&WS_MINIMIZE)) SendMessage(h,WM_SYSCOMMAND,SC_RESTORE,MAKELPARAM(p.x,p.y));
        else if(hit==HTCAPTION && (w->style&WS_MAXIMIZEBOX)) SendMessage(h,WM_SYSCOMMAND,w->style&WS_MAXIMIZE?SC_RESTORE:SC_MAXIMIZE,MAKELPARAM(p.x,p.y));
        else if(hit==HTSYSMENU && !(w->cls->wc.style&CS_NOCLOSE)) SendMessage(h,WM_SYSCOMMAND,SC_CLOSE,MAKELPARAM(p.x,p.y));
        else if(hit!=HTCAPTION && hit!=HTSYSMENU) return NcButtonDown(w,hit,p,FALSE);
        return 0;
    }
    switch(hit) {
    case HTCAPTION:
        if((w->style&WS_CHILD) && (w->style&WS_CAPTION)) BringWindowToTop(h);
        if(w->style&WS_MAXIMIZE) return 0;
        if(!track(w,HTCAPTION,p,FALSE) && (w->style&WS_MINIMIZE) && IsWindow(h)) SystemMenuPopup(w,FALSE);
        return 0;
    case HTSYSMENU: SystemMenuPopup(w,FALSE); return 0;
    case HTMENU: TrackMenuBar(w,-1,p,FALSE); return 0;
    case HTMINBUTTON: if(press_box(w,hit)) SendMessage(h,WM_SYSCOMMAND,SC_MINIMIZE,MAKELPARAM(p.x,p.y)); return 0;
    case HTMAXBUTTON: if(press_box(w,hit)) SendMessage(h,WM_SYSCOMMAND,w->style&WS_MAXIMIZE?SC_RESTORE:SC_MAXIMIZE,MAKELPARAM(p.x,p.y)); return 0;
    case HTVSCROLL: case HTHSCROLL: {
        RECT r; int bar=hit==HTVSCROLL?SB_VERT:SB_HORZ;
        ScrollBarRects(w,bar,&r);
        TrackScroll(w,h,NULL,bar,&r,p);
        return 0;
    }
    case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM: case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT: case HTGROWBOX:
        track(w,hit,p,FALSE);
        return 0;
    }
    return 0;
}

/* --- system commands ------------------------------------------------------------------------ */
static void next_window(Wnd *w,BOOL previous) {
    Wnd *list[64],*t; int n=0,i;
    for(t=desktop->child;t && n<64;t=t->next) if((t->style&WS_VISIBLE) && Enabled(t) && !t->owner) list[n++]=t;
    if(n<2) return;
    if(previous) {Activate(list[n-1],WA_ACTIVE); return;}
    /* Alt+Esc: the active window goes to the bottom. */
    for(i=0;i<n;i++) if(list[i]==TopLevel(w)) break;
    Activate(list[(i+1)%n],WA_ACTIVE);
    if(i<n) SetWindowPos(list[i]->handle,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
/* Ctrl+Esc, a double click on the desktop and Switch To...: the Task List
 * (TASKMAN.EXE), or the next window without it. */
void TaskList(Wnd *w) {
    HWND t=FindWindow("#32770","Task List"); Wnd *tw=WndFromHandle(t);
    /* Not one that is going (Switch To has just ended it). */
    if(tw && (tw->style&WS_VISIBLE) && !DialogEnded(t)) {Activate(tw,WA_ACTIVE); return;}
    if(WinExec("TASKMAN.EXE",SW_SHOWNORMAL)<32 && w) next_window(w,FALSE);
}
void SysCommand(Wnd *w,WPARAM wp,LPARAM lp) {
    HWND h=w->handle; POINT p;
    switch(wp&0xfff0) {
    case SC_CLOSE: if(!(w->cls->wc.style&CS_NOCLOSE)) SendMessage(h,WM_CLOSE,0,0); return;
    case SC_MINIMIZE: ShowWindow(h,SW_MINIMIZE); return;
    case SC_MAXIMIZE: ShowWindow(h,SW_SHOWMAXIMIZED); return;
    case SC_RESTORE:
        if((w->style&WS_MINIMIZE) && !SendMessage(h,WM_QUERYOPEN,0,0)) return;
        ShowWindow(h,SW_RESTORE); return;
    case SC_MOVE: case SC_SIZE:
        if(w->style&WS_MAXIMIZE) return;
        if((wp&0xfff0)==SC_MOVE) {p.x=(w->window.left+w->window.right)/2; p.y=w->window.top+FRAME+CAPTION/2;}
        else {p.x=w->window.right-1; p.y=w->window.bottom-1;}
        track(w,(wp&0xfff0)==SC_MOVE?HTCAPTION:HTBOTTOMRIGHT,p,TRUE);
        return;
    case SC_KEYMENU:
        if(lp==' ' || ((w->style&WS_MINIMIZE) && !lp)) {SystemMenuPopup(w,TRUE); return;}
        if(lp=='-' && (w->style&WS_CHILD)) {SystemMenuPopup(w,TRUE); return;}
        if((w->style&WS_CHILD) || !w->menu) {
            if(!lp && (w->style&WS_SYSMENU)) SystemMenuPopup(w,TRUE);
            else if(lp) MessageBeep(0);
            return;
        }
        if(!lp) {p.x=p.y=0; TrackMenuBar(w,0,p,TRUE); return;}
        if(!MenuKey(w,(WPARAM)lp)) MessageBeep(0);
        return;
    case SC_MOUSEMENU: p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); TrackMenuBar(w,-1,p,FALSE); return;
    case SC_NEXTWINDOW: case SC_PREVWINDOW: next_window(w,(wp&0xfff0)==SC_PREVWINDOW); return;
    case SC_TASKLIST: TaskList(w); return;
    case SC_ARRANGE: ArrangeIcons(w->parent); return;
    case SC_VSCROLL: case SC_HSCROLL: {
        RECT r; int bar=(wp&0xfff0)==SC_VSCROLL?SB_VERT:SB_HORZ;
        p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp);
        ScrollBarRects(w,bar,&r); TrackScroll(w,h,NULL,bar,&r,p);
        return;
    }
    }
}
void NcInit(void) {
    static const WORD gray[8]={0x55,0xaa,0x55,0xaa,0x55,0xaa,0x55,0xaa};
    HBITMAP b=CreateBitmap(8,8,1,1,gray);
    halftone=CreatePatternBrush(b);
    DeleteObject(b);
}
void NcShutdown(void) {DeleteObject(halftone);}
