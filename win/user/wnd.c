/* SPDX-License-Identifier: GPL-2.0-or-later
 * Window classes, lifetime, z-order, placement and painting.
 * Top-level windows are desktop children, topmost first; owned windows stay
 * above their owners. Child creation order determines tab order.
 * Visibility clips against ancestor client areas and subtracts higher
 * siblings (including ancestors' siblings); client visibility also excludes
 * children. Update areas are bounding rectangles in client coordinates.
 * Minimized windows have a 32x32 icon client area with a title below.
 */
#include "user.h"

static Class classes[CLASSES];
static Wnd windows[WINDOWS];
Wnd *desktop,*active,*focus,*capture;
int screen_width,screen_height;
static RECT desktop_update;
static int cascade;

void *ualloc(DWORD bytes) {return GlobalAlloc(GPTR|GMEM_SHARE,bytes);}

/* --- rectangles ------------------------------------------------------------- */
void WINAPI SetRect(LPRECT r,int l,int t,int rt,int b) {if(r) {r->left=l; r->top=t; r->right=rt; r->bottom=b;}}
void WINAPI SetRectEmpty(LPRECT r) {SetRect(r,0,0,0,0);}
void WINAPI OffsetRect(LPRECT r,int dx,int dy) {if(r) {r->left+=dx; r->right+=dx; r->top+=dy; r->bottom+=dy;}}
void WINAPI InflateRect(LPRECT r,int dx,int dy) {if(r) {r->left-=dx; r->right+=dx; r->top-=dy; r->bottom+=dy;}}
BOOL WINAPI IsRectEmpty(LPCRECT r) {return !r || r->left>=r->right || r->top>=r->bottom;}
BOOL WINAPI IntersectRect(LPRECT d,LPCRECT a,LPCRECT b) {
    RECT r; SetRect(&r,max(a->left,b->left),max(a->top,b->top),min(a->right,b->right),min(a->bottom,b->bottom));
    if(IsRectEmpty(&r)) {SetRectEmpty(d); return FALSE;}
    *d=r; return TRUE;
}
BOOL WINAPI UnionRect(LPRECT d,LPCRECT a,LPCRECT b) {
    if(IsRectEmpty(a)) *d=*b;
    else if(IsRectEmpty(b)) *d=*a;
    else SetRect(d,min(a->left,b->left),min(a->top,b->top),max(a->right,b->right),max(a->bottom,b->bottom));
    return !IsRectEmpty(d);
}
/* The smallest rectangle covering a minus b (a when b cuts no whole side). */
BOOL WINAPI SubtractRect(LPRECT d,LPCRECT a,LPCRECT b) {
    RECT r=*a,c;
    if(IntersectRect(&c,a,b)) {
        if(c.top<=r.top && c.bottom>=r.bottom) {if(c.left<=r.left) r.left=max(r.left,c.right); else if(c.right>=r.right) r.right=min(r.right,c.left);}
        else if(c.left<=r.left && c.right>=r.right) {if(c.top<=r.top) r.top=max(r.top,c.bottom); else if(c.bottom>=r.bottom) r.bottom=min(r.bottom,c.top);}
    }
    if(IsRectEmpty(&r)) {SetRectEmpty(d); return FALSE;}
    *d=r; return TRUE;
}
BOOL WINAPI PtInRect(LPCRECT r,POINT p) {return r && p.x>=r->left && p.x<r->right && p.y>=r->top && p.y<r->bottom;}
BOOL WINAPI EqualRect(LPCRECT a,LPCRECT b) {return !memcmp(a,b,sizeof(RECT));}
void WINAPI CopyRect(LPRECT d,LPCRECT s) {*d=*s;}

/* Remove r from a list of rectangles; at most GDI_VIS_MAX remain. */
static int subtract(RECT *list,int count,const RECT *r) {
    static RECT out[GDI_VIS_MAX]; int n=0,i;
    for(i=0;i<count;i++) {
        RECT a=list[i],c;
        if(!IntersectRect(&c,&a,r)) {if(n<GDI_VIS_MAX) out[n++]=a; continue;}
        if(a.top<c.top && n<GDI_VIS_MAX) SetRect(&out[n++],a.left,a.top,a.right,c.top);
        if(c.bottom<a.bottom && n<GDI_VIS_MAX) SetRect(&out[n++],a.left,c.bottom,a.right,a.bottom);
        if(a.left<c.left && n<GDI_VIS_MAX) SetRect(&out[n++],a.left,c.top,c.left,c.bottom);
        if(c.right<a.right && n<GDI_VIS_MAX) SetRect(&out[n++],c.right,c.top,a.right,c.bottom);
    }
    memcpy(list,out,sizeof(RECT)*(unsigned)n);
    return n;
}
static int intersect(RECT *list,int count,const RECT *r) {
    int n=0,i;
    for(i=0;i<count;i++) if(IntersectRect(&list[n],&list[i],r)) n++;
    return n;
}

/* --- handles and classes ---------------------------------------------------- */
Wnd *WndFromHandle(HWND h) {
    ULONG_PTR v=(ULONG_PTR)h;
    if(v<HWND_BASE || (v-HWND_BASE)%4 || (v-HWND_BASE)/4>=WINDOWS) return NULL;
    return windows[(v-HWND_BASE)/4].used?&windows[(v-HWND_BASE)/4]:NULL;
}
static Wnd *new_wnd(void) {
    int i;
    for(i=1;i<WINDOWS;i++) if(!windows[i].used) {
        memset(&windows[i],0,sizeof(windows[i]));
        windows[i].used=TRUE; windows[i].handle=(HWND)(ULONG_PTR)(HWND_BASE+i*4);
        return &windows[i];
    }
    return NULL;
}
static BOOL same_name(LPCSTR name,const Class *c) {
    if(IS_INTRESOURCE(name)) return c->atom==(ATOM)(ULONG_PTR)name;
    return !lstrcmpi(name,c->name);
}
/* The instance's classes, then global classes, then USER's. */
Class *FindClass(LPCSTR name,HINSTANCE instance) {
    int i;
    if(!name) return NULL;
    for(i=0;i<CLASSES;i++) if(classes[i].used && !classes[i].system && classes[i].wc.hInstance==instance && same_name(name,&classes[i])) return &classes[i];
    for(i=0;i<CLASSES;i++) if(classes[i].used && !classes[i].system && (classes[i].wc.style&CS_GLOBALCLASS) && same_name(name,&classes[i])) return &classes[i];
    for(i=0;i<CLASSES;i++) if(classes[i].used && classes[i].system && same_name(name,&classes[i])) return &classes[i];
    return NULL;
}
static Class *add_class(const WNDCLASS *wc) {
    int i; Class *c=NULL;
    for(i=0;i<CLASSES && !c;i++) if(!classes[i].used) c=&classes[i];
    if(!c) return NULL;
    memset(c,0,sizeof(*c));
    if(wc->cbClsExtra>0 && !(c->extra=(BYTE *)ualloc((DWORD)wc->cbClsExtra))) return NULL;
    c->used=TRUE; c->wc=*wc; lstrcpyn(c->name,wc->lpszClassName,sizeof(c->name));
    c->wc.lpszClassName=c->name;
    if(wc->lpszMenuName && IS_INTRESOURCE(wc->lpszMenuName)) c->wc.lpszMenuName=wc->lpszMenuName;
    else if(wc->lpszMenuName) {lstrcpyn(c->menu_name,wc->lpszMenuName,sizeof(c->menu_name)); c->wc.lpszMenuName=c->menu_name;}
    c->atom=(ATOM)(0xc000+(c-classes));
    return c;
}
ATOM WINAPI RegisterClass(const WNDCLASS FAR *wc) {
    Class *c;
    if(!wc || !wc->lpszClassName || !wc->lpfnWndProc || IS_INTRESOURCE(wc->lpszClassName) ||
       lstrlen(wc->lpszClassName)>=(int)sizeof(c->name)) return 0;
    if((c=FindClass(wc->lpszClassName,wc->hInstance))!=NULL && !c->system) return 0;
    c=add_class(wc);
    return c?c->atom:0;
}
ATOM RegisterSystemClass(LPCSTR name,WNDPROC proc,UINT style,int extra,HCURSOR cursor,int background) {
    WNDCLASS wc; Class *c;
    memset(&wc,0,sizeof(wc));
    wc.style=style|CS_GLOBALCLASS; wc.lpfnWndProc=proc; wc.cbWndExtra=extra; wc.hInstance=user_instance;
    wc.hCursor=cursor; wc.hbrBackground=background?(HBRUSH)(ULONG_PTR)background:NULL; wc.lpszClassName=name;
    c=add_class(&wc);
    if(!c) return 0;
    c->system=TRUE; return c->atom;
}
BOOL WINAPI UnregisterClass(LPCSTR name,HINSTANCE instance) {
    Class *c=FindClass(name,instance); int i;
    if(!c || c->system) return FALSE;
    for(i=0;i<WINDOWS;i++) if(windows[i].used && windows[i].cls==c) return FALSE;
    if(c->extra) GlobalFree(c->extra);
    c->used=FALSE; return TRUE;
}
void UnregisterTaskClasses(HINSTANCE instance) {
    int i;
    for(i=0;i<CLASSES;i++) if(classes[i].used && !classes[i].system && classes[i].wc.hInstance==instance) {
        int k; BOOL in_use=FALSE;
        for(k=0;k<WINDOWS;k++) if(windows[k].used && windows[k].cls==&classes[i]) in_use=TRUE;
        if(in_use) continue;
        if(classes[i].extra) GlobalFree(classes[i].extra);
        classes[i].used=FALSE;
    }
}
BOOL WINAPI GetClassInfo(HINSTANCE instance,LPCSTR name,LPWNDCLASS out) {
    Class *c=FindClass(name,instance);
    if(!c || !out) return FALSE;
    *out=c->wc; return TRUE;
}
int WINAPI GetClassName(HWND h,LPSTR out,int size) {
    Wnd *w=WndFromHandle(h);
    if(!w || !out || size<=0) return 0;
    lstrcpyn(out,w->cls->name,size); return lstrlen(out);
}
static int class_index_ok(Class *c,int index,int bytes) {return c && index>=0 && index+bytes<=c->wc.cbClsExtra;}
ULONG_PTR WINAPI GetClassLongPtr(HWND h,int index) {
    Wnd *w=WndFromHandle(h); Class *c=w?w->cls:NULL; ULONG_PTR v=0;
    if(!c) return 0;
    switch(index) {
    case GCL_MENUNAME: return (ULONG_PTR)c->wc.lpszMenuName;
    case GCL_HBRBACKGROUND: return (ULONG_PTR)c->wc.hbrBackground;
    case GCL_HCURSOR: return (ULONG_PTR)c->wc.hCursor;
    case GCL_HICON: return (ULONG_PTR)c->wc.hIcon;
    case GCL_HMODULE: return (ULONG_PTR)c->wc.hInstance;
    case GCL_CBWNDEXTRA: return (ULONG_PTR)c->wc.cbWndExtra;
    case GCL_CBCLSEXTRA: return (ULONG_PTR)c->wc.cbClsExtra;
    case GCL_WNDPROC: return (ULONG_PTR)c->wc.lpfnWndProc;
    case GCL_STYLE: return c->wc.style;
    }
    if(class_index_ok(c,index,sizeof(v))) memcpy(&v,c->extra+index,sizeof(v));
    return v;
}
ULONG_PTR WINAPI SetClassLongPtr(HWND h,int index,LONG_PTR value) {
    Wnd *w=WndFromHandle(h); Class *c=w?w->cls:NULL; ULONG_PTR old=GetClassLongPtr(h,index);
    if(!c) return 0;
    switch(index) {
    case GCL_HBRBACKGROUND: c->wc.hbrBackground=(HBRUSH)value; return old;
    case GCL_HCURSOR: c->wc.hCursor=(HCURSOR)value; return old;
    case GCL_HICON: c->wc.hIcon=(HICON)value; return old;
    case GCL_WNDPROC: c->wc.lpfnWndProc=(WNDPROC)value; return old;
    case GCL_STYLE: c->wc.style=(UINT)value; return old;
    }
    if(class_index_ok(c,index,sizeof(value))) memcpy(c->extra+index,&value,sizeof(value));
    return old;
}
DWORD WINAPI GetClassLong(HWND h,int index) {
    Wnd *w=WndFromHandle(h); DWORD v=0;
    if(w && index>=0) {if(class_index_ok(w->cls,index,sizeof(v))) memcpy(&v,w->cls->extra+index,sizeof(v)); return v;}
    return (DWORD)GetClassLongPtr(h,index);
}
DWORD WINAPI SetClassLong(HWND h,int index,LONG value) {
    Wnd *w=WndFromHandle(h); DWORD old=GetClassLong(h,index);
    if(w && index>=0) {if(class_index_ok(w->cls,index,sizeof(value))) memcpy(w->cls->extra+index,&value,sizeof(value)); return old;}
    SetClassLongPtr(h,index,value); return old;
}
WORD WINAPI GetClassWord(HWND h,int index) {
    Wnd *w=WndFromHandle(h); WORD v=0;
    if(w && index>=0) {if(class_index_ok(w->cls,index,sizeof(v))) memcpy(&v,w->cls->extra+index,sizeof(v)); return v;}
    return (WORD)GetClassLongPtr(h,index);
}
WORD WINAPI SetClassWord(HWND h,int index,WORD value) {
    Wnd *w=WndFromHandle(h); WORD old=GetClassWord(h,index);
    if(w && index>=0 && class_index_ok(w->cls,index,sizeof(value))) memcpy(w->cls->extra+index,&value,sizeof(value));
    return old;
}

/* --- geometry --------------------------------------------------------------- */
void CalcClient(Wnd *w) {
    int b;
    w->client=w->window;
    if(w->style&WS_MINIMIZE) {
        w->client.left=w->window.left+(w->window.right-w->window.left-ICON)/2; w->client.right=w->client.left+ICON;
        w->client.bottom=w->client.top+ICON;
        return;
    }
    b=FrameWidth(w);
    InflateRect(&w->client,-b,-b);
    if((w->style&WS_CAPTION)==WS_CAPTION) w->client.top+=CAPTION-1;
    if(!(w->style&WS_CHILD) && w->menu) w->client.top+=MenuBarHeight(w);
    if((w->style&WS_VSCROLL) && !w->scroll[SB_VERT].hidden) w->client.right-=SCROLL-1;
    if((w->style&WS_HSCROLL) && !w->scroll[SB_HORZ].hidden) w->client.bottom-=SCROLL-1;
    if(w->client.bottom<w->client.top) w->client.bottom=w->client.top;
    if(w->client.right<w->client.left) w->client.right=w->client.left;
}
BOOL WINAPI AdjustWindowRectEx(LPRECT r,DWORD style,BOOL menu,DWORD exstyle) {
    int b=0;
    if(!r) return FALSE;
    if(!(style&(WS_POPUP|WS_CHILD))) style|=WS_CAPTION;
    if((style&WS_THICKFRAME) || ((style&WS_DLGFRAME) && !(style&WS_BORDER)) || (exstyle&WS_EX_DLGMODALFRAME)) b=FRAME;
    else if(style&(WS_BORDER|WS_DLGFRAME)) b=1;
    InflateRect(r,b,b);
    if((style&WS_CAPTION)==WS_CAPTION) r->top-=CAPTION-1;
    if(menu) r->top-=MENUBAR;
    return TRUE;
}
BOOL WINAPI AdjustWindowRect(LPRECT r,DWORD style,BOOL menu) {return AdjustWindowRectEx(r,style,menu,0);}
/* Visible: it and its ancestors are visible, and no ancestor is minimized. */
BOOL Shown(Wnd *w) {
    Wnd *a;
    for(a=w;a && a!=desktop;a=a->parent) {
        if(!(a->style&WS_VISIBLE)) return FALSE;
        if(a!=w && (a->style&WS_MINIMIZE)) return FALSE;
    }
    return a==desktop;
}
BOOL Enabled(Wnd *w) {return w && !(w->style&WS_DISABLED);}
Wnd *TopLevel(Wnd *w) {while(w && w->parent && w->parent!=desktop) w=w->parent; return w;}
/* Group boxes (and WS_EX_TRANSPARENT windows) hide nothing under them: the
 * parent paints their inside and the controls they surround show through. */
static BOOL see_through(Wnd *w) {
    return (w->exstyle&WS_EX_TRANSPARENT) || ((w->style&WS_CHILD) && (w->style&0x0f)==BS_GROUPBOX && !lstrcmpi(w->cls->name,"BUTTON"));
}
/* The window's visible area in screen coordinates. A DC's (by_style) is
 * not clipped by the siblings of a child window without WS_CLIPSIBLINGS, as
 * in Windows: where such siblings overlap, the one painted last shows. */
static int visible(Wnd *w,BOOL client,RECT *rects,BOOL by_style) {
    Wnd *a,*s; int n=1; RECT screen;
    if(!Shown(w)) return 0;
    rects[0]=client?w->client:w->window;
    SetRect(&screen,0,0,screen_width,screen_height);
    n=intersect(rects,n,&screen);
    for(a=w;a && a!=desktop && n;a=a->parent) {
        if(a->parent && a->parent!=desktop) n=intersect(rects,n,&a->parent->client);
        if(by_style && a->parent!=desktop && !(a->style&WS_CLIPSIBLINGS)) continue;
        for(s=a->parent?a->parent->child:NULL;s && s!=a && n;s=s->next) if((s->style&WS_VISIBLE) && !see_through(s)) n=subtract(rects,n,&s->window);
    }
    if((client || w==desktop) && !(w->style&WS_MINIMIZE)) for(s=w->child;s && n;s=s->next) if((s->style&WS_VISIBLE) && !see_through(s)) n=subtract(rects,n,&s->window);
    if(!client && w!=desktop && n) n=subtract(rects,n,&w->client);
    return n;
}
static int VisibleRects(Wnd *w,BOOL client,RECT *rects) {return visible(w,client,rects,FALSE);}
/* The deepest visible, enabled-or-not window under p (minimized windows are leaves). */
Wnd *WndFromPoint(POINT p) {
    Wnd *w=desktop,*c;
    for(;;) {
        for(c=w->child;c;c=c->next) if((c->style&WS_VISIBLE) && !see_through(c) && PtInRect(&c->window,p)) break;
        if(!c) return w;
        if((c->style&WS_MINIMIZE) || !PtInRect(&c->client,p)) return c;
        w=c;
    }
}
HWND WINAPI WindowFromPoint(POINT p) {Wnd *w=WndFromPoint(p); return w==desktop?NULL:w->handle;}
HWND WINAPI ChildWindowFromPoint(HWND h,POINT p) {
    Wnd *w=WndFromHandle(h),*c;
    if(!w) return NULL;
    p.x+=w->client.left; p.y+=w->client.top;
    if(!PtInRect(&w->window,p)) return NULL;
    for(c=w->child;c;c=c->next) if((c->style&WS_VISIBLE) && PtInRect(&c->window,p)) return c->handle;
    return h;
}
void WINAPI ClientToScreen(HWND h,LPPOINT p) {Wnd *w=WndFromHandle(h); if(w && p) {p->x+=w->client.left; p->y+=w->client.top;}}
void WINAPI ScreenToClient(HWND h,LPPOINT p) {Wnd *w=WndFromHandle(h); if(w && p) {p->x-=w->client.left; p->y-=w->client.top;}}
int WINAPI MapWindowPoints(HWND from,HWND to,LPPOINT p,UINT count) {
    Wnd *a=from?WndFromHandle(from):desktop,*b=to?WndFromHandle(to):desktop; int dx,dy; UINT i;
    if(!a || !b) return 0;
    dx=a->client.left-b->client.left; dy=a->client.top-b->client.top;
    for(i=0;i<count;i++) {p[i].x+=dx; p[i].y+=dy;}
    return (int)MAKELONG(dx,dy);
}

/* --- invalidation and painting ---------------------------------------------- */
static void wake(Wnd *w) {if(w->queue) wh_wake(w->queue->task);}
void InvalidateWnd(Wnd *w,const RECT *area,BOOL erase,BOOL frame) {
    RECT r,full;
    if(w==desktop) {
        if(area) {r=*area; UnionRect(&desktop_update,&desktop_update,&r);}
        else SetRect(&desktop_update,0,0,screen_width,screen_height);
        return;
    }
    SetRect(&full,0,0,w->client.right-w->client.left,w->client.bottom-w->client.top);
    if(!area) r=full;
    else if(!IntersectRect(&r,area,&full)) SetRectEmpty(&r);
    if(!IsRectEmpty(&r)) {UnionRect(&w->update,&w->update,&r); if(erase) w->erase=TRUE;}
    if(frame) w->ncpaint=TRUE;
    if(!IsRectEmpty(&r) || frame) wake(w);
    /* A see-through child painted over by its parent paints again. */
    if(!IsRectEmpty(&r)) {
        Wnd *c; RECT s=r,cr;
        OffsetRect(&s,w->client.left,w->client.top);
        for(c=w->child;c;c=c->next) if((c->style&WS_VISIBLE) && see_through(c) && IntersectRect(&cr,&s,&c->window)) InvalidateWnd(c,NULL,FALSE,FALSE);
    }
}
/* A screen area became uncovered: whatever shows there must repaint it. */
static void expose_in(Wnd *parent,const RECT *area) {
    Wnd *w; RECT c,r;
    for(w=parent->child;w;w=w->next) {
        if(!(w->style&WS_VISIBLE) || !IntersectRect(&r,area,&w->window)) continue;
        if(!IntersectRect(&c,&r,&w->client) || !EqualRect(&c,&r)) InvalidateWnd(w,NULL,FALSE,TRUE);
        if(IntersectRect(&c,&r,&w->client)) {
            OffsetRect(&c,-w->client.left,-w->client.top);
            InvalidateWnd(w,&c,TRUE,FALSE);
            if(!(w->style&WS_MINIMIZE)) expose_in(w,&r);
        }
    }
}
static void Expose(const RECT *area) {
    UnionRect(&desktop_update,&desktop_update,area);
    expose_in(desktop,area);
}
/* A window that moved, came to the top or appeared repaints whole, and so
 * does everything in it, frames included: what it now shows of itself was
 * the screen of others. */
static void invalidate_tree(Wnd *w) {
    Wnd *c;
    InvalidateWnd(w,NULL,TRUE,TRUE);
    if(w->style&WS_MINIMIZE) return;
    for(c=w->child;c;c=c->next) if(c->style&WS_VISIBLE) invalidate_tree(c);
}
static void expose_window(Wnd *w) {
    RECT r=w->window;
    if(w->parent==desktop) Expose(&r);
    else {
        RECT c;
        if(IntersectRect(&c,&r,&w->parent->client)) {
            RECT rc=c; OffsetRect(&rc,-w->parent->client.left,-w->parent->client.top);
            InvalidateWnd(w->parent,&rc,TRUE,FALSE);
            expose_in(w->parent,&c);
        }
    }
}
void PaintDesktop(void) {
    static RECT rects[GDI_VIS_MAX]; int n; HDC dc;
    if(IsRectEmpty(&desktop_update)) return;
    n=VisibleRects(desktop,TRUE,rects); n=intersect(rects,n,&desktop_update);
    SetRectEmpty(&desktop_update);
    if(!n || !(dc=GdiCreateScreenDC())) return;
    GdiSetVisRects(dc,rects,n);
    SelectObject(dc,SysBrush(COLOR_BACKGROUND));
    PatBlt(dc,0,0,screen_width,screen_height,PATCOPY);
    DeleteDC(dc);
}
static Wnd *paint_in(Wnd *parent,Queue *q,HWND filter) {
    Wnd *w,*found;
    for(w=parent->child;w;w=w->next) {
        if(!(w->style&WS_VISIBLE)) continue;
        if(w->queue==q && (!filter || w->handle==filter) && (w->ncpaint || !IsRectEmpty(&w->update)) && !w->redraw_off) return w;
        if(!(w->style&WS_MINIMIZE) && (found=paint_in(w,q,filter))!=NULL) return found;
    }
    return NULL;
}
Wnd *PaintWindow(Queue *q,HWND filter) {return paint_in(desktop,q,filter);}
static HDC window_dc(Wnd *w,BOOL client,const RECT *clip) {
    static RECT rects[GDI_VIS_MAX]; int n=visible(w,client,rects,TRUE); HDC dc;
    if(client && w->own_dc) {dc=w->own_dc;}
    else dc=GdiCreateScreenDC();
    if(!dc) return NULL;
    if(clip) n=intersect(rects,n,clip);
    GdiSetVisRects(dc,rects,n);
    GdiSetDCOrigin(dc,client?w->client.left:w->window.left,client?w->client.top:w->window.top);
    return dc;
}
HDC WINAPI GetDC(HWND h) {
    Wnd *w=h?WndFromHandle(h):desktop;
    return w?window_dc(w,TRUE,NULL):NULL;
}
HDC WINAPI GetWindowDC(HWND h) {
    Wnd *w=h?WndFromHandle(h):desktop;
    return w?window_dc(w,w==desktop,NULL):NULL;
}
int WINAPI ReleaseDC(HWND h,HDC dc) {
    Wnd *w=WndFromHandle(h);
    if(w && w->own_dc==dc) return 1;
    return DeleteDC(dc);
}
HDC WINAPI BeginPaint(HWND h,LPPAINTSTRUCT ps) {
    Wnd *w=WndFromHandle(h); RECT clip;
    if(!w || !ps) return NULL;
    memset(ps,0,sizeof(*ps));
    HideCaret(h);
    if(w->ncpaint) {w->ncpaint=FALSE; SendMessage(h,WM_NCPAINT,1,0);}
    w->validated=TRUE;
    ps->rcPaint=w->update; clip=w->update; OffsetRect(&clip,w->client.left,w->client.top);
    SetRectEmpty(&w->update);
    ps->hdc=window_dc(w,TRUE,&clip);
    ps->fErase=TRUE;
    if(w->erase) {
        w->erase=FALSE;
        ps->fErase=!SendMessage(h,(w->style&WS_MINIMIZE) && w->cls->wc.hIcon?WM_ICONERASEBKGND:WM_ERASEBKGND,(WPARAM)ps->hdc,0);
    }
    return ps->hdc;
}
void WINAPI EndPaint(HWND h,const PAINTSTRUCT FAR *ps) {
    Wnd *w=WndFromHandle(h);
    if(ps && ps->hdc && (!w || w->own_dc!=ps->hdc)) DeleteDC(ps->hdc);
    ShowCaret(h);
}
void WINAPI InvalidateRect(HWND h,LPCRECT r,BOOL erase) {
    Wnd *w=WndFromHandle(h); int i;
    if(w) {InvalidateWnd(w,r,erase,FALSE); return;}
    if(h) return;
    for(i=1;i<WINDOWS;i++) if(windows[i].used) InvalidateWnd(&windows[i],NULL,TRUE,TRUE);
    SetRect(&desktop_update,0,0,screen_width,screen_height);
}
void WINAPI InvalidateRgn(HWND h,HRGN rgn,BOOL erase) {
    RECT r;
    if(!rgn) {InvalidateRect(h,NULL,erase); return;}
    if(GetRgnBox(rgn,&r)!=NULLREGION) InvalidateRect(h,&r,erase);
}
void WINAPI ValidateRect(HWND h,LPCRECT r) {
    Wnd *w=WndFromHandle(h); RECT c;
    if(!w) return;
    w->validated=TRUE;
    if(!r) {SetRectEmpty(&w->update); w->erase=FALSE; return;}
    if(IntersectRect(&c,r,&w->update) && EqualRect(&c,&w->update)) SetRectEmpty(&w->update);
    else if(SubtractRect(&c,&w->update,r)) w->update=c;
}
void WINAPI ValidateRgn(HWND h,HRGN rgn) {
    RECT r;
    if(!rgn) {ValidateRect(h,NULL); return;}
    if(GetRgnBox(rgn,&r)!=NULLREGION) ValidateRect(h,&r);
}
BOOL WINAPI GetUpdateRect(HWND h,LPRECT r,BOOL erase) {
    Wnd *w=WndFromHandle(h);
    if(!w) return FALSE;
    if(r) *r=w->update;
    if(erase && w->erase && !IsRectEmpty(&w->update)) {
        HDC dc=GetDC(h); RECT clip=w->update;
        IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
        w->erase=FALSE; SendMessage(h,WM_ERASEBKGND,(WPARAM)dc,0); ReleaseDC(h,dc);
    }
    return !IsRectEmpty(&w->update);
}
void WINAPI UpdateWindow(HWND h) {
    Wnd *w=WndFromHandle(h),*c;
    if(!w) return;
    if(Shown(w) && (w->ncpaint || !IsRectEmpty(&w->update))) {
        if(w->queue && w->queue!=CurrentQueue()) {wake(w); return;}
        SendMessage(h,(w->style&WS_MINIMIZE) && w->cls->wc.hIcon?WM_PAINTICON:WM_PAINT,0,0);
    }
    for(c=w->child;c;c=c->next) UpdateWindow(c->handle);
}
/* Scroll the client area's pixels where the window is fully visible;
 * whatever the move uncovers (or could not be copied) is invalidated. */
void WINAPI ScrollWindow(HWND h,int dx,int dy,LPCRECT area,LPCRECT clip) {
    Wnd *w=WndFromHandle(h),*c; RECT r,full,rects[2]; int n; HDC dc;
    if(!w || (!dx && !dy)) return;
    SetRect(&full,0,0,w->client.right-w->client.left,w->client.bottom-w->client.top);
    r=area?*area:full;
    if(clip) IntersectRect(&r,&r,clip);
    if(!IntersectRect(&r,&r,&full)) return;
    HideCaret(h);
    n=VisibleRects(w,TRUE,rects);
    {
        RECT screen_r=r; OffsetRect(&screen_r,w->client.left,w->client.top);
        if(n==1 && IntersectRect(&rects[1],&rects[0],&screen_r) && EqualRect(&rects[1],&screen_r) && !w->child) {
            dc=GetDC(h);
            BitBlt(dc,r.left+max(dx,0),r.top+max(dy,0),r.right-r.left-(dx<0?-dx:dx),r.bottom-r.top-(dy<0?-dy:dy),
                   dc,r.left+max(-dx,0),r.top+max(-dy,0),SRCCOPY);
            ReleaseDC(h,dc);
            if(dx>0) {RECT e=r; e.right=r.left+dx; InvalidateWnd(w,&e,TRUE,FALSE);}
            if(dx<0) {RECT e=r; e.left=r.right+dx; InvalidateWnd(w,&e,TRUE,FALSE);}
            if(dy>0) {RECT e=r; e.bottom=r.top+dy; InvalidateWnd(w,&e,TRUE,FALSE);}
            if(dy<0) {RECT e=r; e.top=r.bottom+dy; InvalidateWnd(w,&e,TRUE,FALSE);}
            if(!IsRectEmpty(&w->update)) {RECT u=w->update; OffsetRect(&u,dx,dy); UnionRect(&w->update,&w->update,&u);}
        } else InvalidateWnd(w,&r,TRUE,FALSE);
    }
    /* Without a rectangle, the children move with the contents. */
    if(!area) for(c=w->child;c;c=c->next) {
        RECT m=c->window; OffsetRect(&m,dx-w->client.left,dy-w->client.top);
        PlaceWindow(c,&m,TRUE);
    }
    ShowCaret(h);
}
BOOL WINAPI ScrollDC(HDC dc,int dx,int dy,LPCRECT area,LPCRECT clip,HRGN update,LPRECT out) {
    RECT r,c; int w,h;
    if(!area) return FALSE;
    r=*area;
    if(clip) IntersectRect(&r,&r,clip);
    w=r.right-r.left-(dx<0?-dx:dx); h=r.bottom-r.top-(dy<0?-dy:dy);
    if(w>0 && h>0) BitBlt(dc,r.left+max(dx,0),r.top+max(dy,0),w,h,dc,r.left+max(-dx,0),r.top+max(-dy,0),SRCCOPY);
    SetRectEmpty(&c);
    if(dx>0) SetRect(&c,r.left,r.top,r.left+dx,r.bottom);
    else if(dx<0) SetRect(&c,r.right+dx,r.top,r.right,r.bottom);
    if(dy>0) {RECT e; SetRect(&e,r.left,r.top,r.right,r.top+dy); UnionRect(&c,&c,&e);}
    else if(dy<0) {RECT e; SetRect(&e,r.left,r.bottom+dy,r.right,r.bottom); UnionRect(&c,&c,&e);}
    if(update) SetRectRgn(update,c.left,c.top,c.right,c.bottom);
    if(out) *out=c;
    return TRUE;
}

/* --- z-order ------------------------------------------------------------------ */
static void unlink_wnd(Wnd *w) {
    Wnd **link;
    for(link=&w->parent->child;*link;link=&(*link)->next) if(*link==w) {*link=w->next; break;}
    w->next=NULL;
}
static BOOL owned_by(Wnd *w,Wnd *owner) {
    for(w=w->owner;w;w=w->owner) if(w==owner) return TRUE;
    return FALSE;
}
/* Insert below 'after' (NULL: at the top); topmost windows stay above the rest. */
static void link_after(Wnd *w,Wnd *after) {
    Wnd **link=&w->parent->child;
    if(!after) {
        if(!(w->exstyle&WS_EX_TOPMOST)) while(*link && ((*link)->exstyle&WS_EX_TOPMOST)) link=&(*link)->next;
    } else {
        while(*link && *link!=after) link=&(*link)->next;
        if(*link) link=&(*link)->next;
    }
    w->next=*link; *link=w;
}
static void link_bottom(Wnd *w) {
    Wnd **link=&w->parent->child;
    while(*link) link=&(*link)->next;
    w->next=NULL; *link=w;
}
/* Raise a top-level window, then the windows it owns above it. */
static void raise_owned(Wnd *w) {
    Wnd *owned[64],*o; int n=0,i;
    unlink_wnd(w); link_after(w,NULL);
    if(w->parent!=desktop) return;
    for(o=desktop->child;o && n<64;o=o->next) if(o!=w && o->owner==w) owned[n++]=o;
    for(i=n-1;i>=0;i--) raise_owned(owned[i]);
}
static void bring_to_top(Wnd *w) {
    Wnd *first;
    if(!w->parent) return;
    first=w->parent->child;
    raise_owned(w);
    if(first!=w && Shown(w)) invalidate_tree(w);
    if(w->parent==desktop) {
        /* Owned windows that were raised must repaint too. */
        Wnd *o; for(o=desktop->child;o && o!=w;o=o->next) if(owned_by(o,w) && Shown(o)) invalidate_tree(o);
    }
}
BOOL WINAPI BringWindowToTop(HWND h) {
    Wnd *w=WndFromHandle(h);
    if(!w) return FALSE;
    if(w->parent==desktop && !(w->style&WS_CHILD)) Activate(w,WA_ACTIVE);
    else bring_to_top(w);
    return TRUE;
}

/* --- activation and focus ------------------------------------------------------ */
static Wnd *first_visible_top(Wnd *skip) {
    Wnd *w;
    for(w=desktop->child;w;w=w->next) if(w!=skip && (w->style&WS_VISIBLE) && Enabled(w)) return w;
    return NULL;
}
/* Bring a top-level window forward and give it the keyboard: the old one
 * gets WM_NCACTIVATE and WM_ACTIVATE (inactive), the new one the same and
 * WM_ACTIVATEAPP when the task changes; DefWindowProc then sets the focus. */
void Activate(Wnd *w,int how) {
    Wnd *old=active;
    w=TopLevel(w);
    if(w==desktop) w=NULL;
    if(w && (!(w->style&WS_VISIBLE) || !w->used)) return;
    if(w && w!=old && HookActive(WH_CBT)) {
        CBTACTIVATESTRUCT a; a.fMouse=how==WA_CLICKACTIVE; a.hWndActive=old?old->handle:NULL;
        if(CallHook(WH_CBT,HCBT_ACTIVATE,(WPARAM)w->handle,(LPARAM)&a) || !w->used) return;
    }
    if(w) bring_to_top(w);
    if(w==old) return;
    active=w;
    if(old && old->used) {
        SendMessage(old->handle,WM_NCACTIVATE,FALSE,0);
        if(old->used) SendMessage(old->handle,WM_ACTIVATE,MAKEWPARAM(WA_INACTIVE,(old->style&WS_MINIMIZE)!=0),w?(LPARAM)w->handle:0);
        if(old->used && (!w || old->queue!=w->queue)) SendMessage(old->handle,WM_ACTIVATEAPP,FALSE,0);
        if(old->used) old->last_active=old;
    }
    if(active!=w) return;
    if(!w) {focus=NULL; return;}
    if(w->owner) w->owner->last_active=w;
    if(!old || !old->used || old->queue!=w->queue) SendMessage(w->handle,WM_ACTIVATEAPP,TRUE,0);
    if(active==w) SendMessage(w->handle,WM_NCACTIVATE,TRUE,0);
    if(active==w) SendMessage(w->handle,WM_ACTIVATE,MAKEWPARAM(how,(w->style&WS_MINIMIZE)!=0),old && old->used?(LPARAM)old->handle:0);
}
static void activate_next(Wnd *gone) {
    Wnd *next=NULL;
    if(gone && gone->owner && gone->owner->used && (gone->owner->style&WS_VISIBLE)) next=TopLevel(gone->owner);
    if(!next) next=first_visible_top(gone);
    if(next) Activate(next,WA_ACTIVE);
    else {active=NULL; focus=NULL;}
}
HWND WINAPI GetActiveWindow(void) {return active?active->handle:NULL;}
HWND WINAPI SetActiveWindow(HWND h) {
    Wnd *w=WndFromHandle(h); HWND old=GetActiveWindow();
    if(w) Activate(w,WA_ACTIVE);
    return old;
}
HWND WINAPI GetFocus(void) {return focus?focus->handle:NULL;}
HWND WINAPI SetFocus(HWND h) {
    Wnd *w=WndFromHandle(h),*old=focus;
    if(h && !w) return NULL;
    if(w && (w->style&WS_DISABLED)) return NULL;
    if(w && TopLevel(w)!=active) {
        Activate(w,WA_ACTIVE);
        if(focus==w) return old?old->handle:NULL;
    }
    if(old==w) return h;
    if(HookActive(WH_CBT) && CallHook(WH_CBT,HCBT_SETFOCUS,(WPARAM)h,(LPARAM)(old?old->handle:NULL))) return NULL;
    focus=w;
    if(old && old->used) SendMessage(old->handle,WM_KILLFOCUS,(WPARAM)h,0);
    if(w && focus==w) SendMessage(h,WM_SETFOCUS,old && old->used?(WPARAM)old->handle:0,0);
    return old && old->used?old->handle:NULL;
}
HWND WINAPI GetDesktopWindow(void) {return desktop->handle;}
HWND WINAPI GetLastActivePopup(HWND h) {
    Wnd *w=WndFromHandle(h);
    if(!w) return NULL;
    if(w->last_active && w->last_active->used && (w->last_active->style&WS_VISIBLE) && owned_by(w->last_active,w)) return w->last_active->handle;
    return h;
}
BOOL WINAPI AnyPopup(void) {
    Wnd *w;
    for(w=desktop->child;w;w=w->next) if((w->style&WS_VISIBLE) && (w->owner || (w->style&WS_POPUP))) return TRUE;
    return FALSE;
}
BOOL WINAPI FlashWindow(HWND h,BOOL invert) {
    Wnd *w=WndFromHandle(h);
    if(!w) return FALSE;
    SendMessage(h,WM_NCACTIVATE,invert?w!=active:w==active,0);
    return w==active;
}

/* --- creation ---------------------------------------------------------------------- */
static void child_rect_to_screen(Wnd *p,RECT *r) {OffsetRect(r,p->client.left,p->client.top);}
/* WM_SIZE and WM_MOVE for where the window is. */
static void send_size(Wnd *w) {
    w->need_size=FALSE;
    SendMessage(w->handle,WM_SIZE,w->style&WS_MINIMIZE?SIZE_MINIMIZED:w->style&WS_MAXIMIZE?SIZE_MAXIMIZED:SIZE_RESTORED,
                MAKELPARAM(w->client.right-w->client.left,w->client.bottom-w->client.top));
    if(w->used) SendMessage(w->handle,WM_MOVE,0,MAKELPARAM(w->client.left-w->parent->client.left,w->client.top-w->parent->client.top));
}
HWND WINAPI CreateWindowEx(DWORD exstyle,LPCSTR class_name,LPCSTR title,DWORD style,int x,int y,int width,int height,
                           HWND parent,HMENU menu,HINSTANCE instance,void FAR *param) {
    Class *c=FindClass(class_name,instance); Wnd *w,*p=parent?WndFromHandle(parent):NULL,*owner=NULL; CREATESTRUCT cs;
    BOOL show_max=FALSE,show_min=FALSE; RECT r; int show;
    if(!c) c=FindClass(class_name,NULL);
    if(!c || (style&WS_CHILD && !p)) return NULL;
    if(!(style&WS_CHILD)) {owner=p?TopLevel(p):NULL; if(owner==desktop) owner=NULL; p=desktop;}
    if(!p) p=desktop;
    if(!(w=new_wnd())) return NULL;
    if(!(style&(WS_CHILD|WS_POPUP))) style|=WS_CAPTION|WS_CLIPSIBLINGS;
    show_max=(style&WS_MAXIMIZE)!=0; show_min=(style&WS_MINIMIZE)!=0;
    show=style&WS_VISIBLE?SW_SHOW:-1;
    style&=~(WS_MAXIMIZE|WS_MINIMIZE|WS_VISIBLE);
    w->cls=c; w->proc=c->wc.lpfnWndProc; w->style=style; w->exstyle=exstyle; w->parent=p; w->owner=owner;
    w->instance=instance; w->queue=CurrentQueue();
    if(style&WS_CHILD) w->id=(UINT_PTR)menu;
    else {
        w->menu=menu;
        if(!menu && c->wc.lpszMenuName) w->menu=LoadMenu(instance?instance:c->wc.hInstance,c->wc.lpszMenuName);
    }
    w->scroll[0].max=w->scroll[1].max=100;
    SetWndText(w,title);
    if(c->wc.cbWndExtra>0) {
        w->extra_bytes=c->wc.cbWndExtra;
        if(!(w->extra=(BYTE *)ualloc((DWORD)w->extra_bytes))) {w->used=FALSE; return NULL;}
    }
    if(c->wc.style&(CS_OWNDC|CS_CLASSDC)) w->own_dc=GdiCreateScreenDC();
    if(p==desktop) {
        if(x==CW_USEDEFAULT) {x=y=24*(1+cascade%6); cascade++;}
        if(width==CW_USEDEFAULT) {width=screen_width*3/4; height=screen_height*3/4;}
        if(width+x>screen_width && x>0) x=max(0,screen_width-width);
        if(height+y>screen_height && y>0) y=max(0,screen_height-height);
    } else {
        if(x==CW_USEDEFAULT) x=y=0;
        if(width==CW_USEDEFAULT) width=height=0;
    }
    SetRect(&r,x,y,x+width,y+height);
    w->normal=r;
    if(p!=desktop) child_rect_to_screen(p,&r);
    w->window=r;
    CalcClient(w);
    if(style&WS_CHILD) link_bottom(w); else link_after(w,NULL);
    cs.lpCreateParams=param; cs.hInstance=instance; cs.hMenu=menu; cs.hwndParent=parent;
    cs.cx=width; cs.cy=height; cs.x=x; cs.y=y; cs.style=(LONG)w->style; cs.lpszName=title; cs.lpszClass=class_name; cs.dwExStyle=exstyle;
    /* A CBT hook may refuse the window, or change where it goes. */
    if(HookActive(WH_CBT)) {
        CBT_CREATEWND cbt; cbt.lpcs=&cs; cbt.hwndInsertAfter=HWND_TOP;
        if(CallHook(WH_CBT,HCBT_CREATEWND,(WPARAM)w->handle,(LPARAM)&cbt)) {if(w->used) DestroyWindow(w->handle); return NULL;}
        if(!w->used) return NULL;
        if(cs.x!=x || cs.y!=y || cs.cx!=width || cs.cy!=height) {
            x=cs.x; y=cs.y; width=cs.cx; height=cs.cy;
            SetRect(&r,x,y,x+width,y+height); w->normal=r;
            if(p!=desktop) child_rect_to_screen(p,&r);
            w->window=r; CalcClient(w);
        }
    }
    if(!SendMessage(w->handle,WM_NCCREATE,0,(LPARAM)&cs)) {DestroyWindow(w->handle); return NULL;}
    if(!w->used) return NULL;
    CalcClient(w);
    if(SendMessage(w->handle,WM_CREATE,0,(LPARAM)&cs)==-1 || !w->used) {if(w->used) DestroyWindow(w->handle); return NULL;}
    /* A child or popup gets WM_SIZE and WM_MOVE now, an overlapped window
     * when first shown: programs set up its data after CreateWindow. */
    if(style&(WS_CHILD|WS_POPUP)) send_size(w);
    else w->need_size=TRUE;
    if((style&WS_CHILD) && !(exstyle&WS_EX_NOPARENTNOTIFY) && w->used) SendMessage(p->handle,WM_PARENTNOTIFY,MAKEWPARAM(WM_CREATE,w->id),(LPARAM)w->handle);
    if(!w->used) return NULL;
    if(show_min) ShowState(w,SW_SHOWMINNOACTIVE);
    else if(show_max) ShowState(w,SW_SHOWMAXIMIZED);
    if(p==desktop && !owner) CallHook(WH_SHELL,HSHELL_WINDOWCREATED,(WPARAM)w->handle,0);
    if(!w->used) return NULL;
    if(show>=0) ShowWindow(w->handle,show_min?SW_SHOWMINNOACTIVE:show_max?SW_SHOWMAXIMIZED:SW_SHOW);
    return w->handle;
}
HWND WINAPI CreateWindow(LPCSTR class_name,LPCSTR title,DWORD style,int x,int y,int width,int height,
                         HWND parent,HMENU menu,HINSTANCE instance,void FAR *param) {
    return CreateWindowEx(0,class_name,title,style,x,y,width,height,parent,menu,instance,param);
}
void SetWndText(Wnd *w,LPCSTR text) {
    int n=text?lstrlen(text):0;
    if(w->text) {GlobalFree(w->text); w->text=NULL;}
    w->text=(char *)ualloc((DWORD)n+1);
    if(w->text && n) memcpy(w->text,text,(size_t)n);
}

/* --- placement ------------------------------------------------------------------------ */
static void offset_children(Wnd *w,int dx,int dy) {
    Wnd *c;
    for(c=w->child;c;c=c->next) {OffsetRect(&c->window,dx,dy); OffsetRect(&c->client,dx,dy); offset_children(c,dx,dy);}
}
/* Move a window to 'r' (parent client coordinates); the old and new areas repaint. */
void PlaceWindow(Wnd *w,const RECT *r,BOOL notify) {
    RECT old=w->window,nr=*r,oldc=w->client; BOOL visible=Shown(w);
    if(w->parent!=desktop) child_rect_to_screen(w->parent,&nr);
    if(EqualRect(&nr,&old)) {
        CalcClient(w);
        if(!EqualRect(&oldc,&w->client) && visible) {InvalidateWnd(w,NULL,TRUE,TRUE); expose_window(w);}
        else if(visible) InvalidateWnd(w,NULL,FALSE,TRUE);
        return;
    }
    w->window=nr; CalcClient(w);
    offset_children(w,w->client.left-oldc.left,w->client.top-oldc.top);
    if(visible) {
        RECT keep=w->window;
        w->window=old; expose_window(w); w->window=keep;
        invalidate_tree(w);
    }
    if(notify) {
        if(oldc.right-oldc.left!=w->client.right-w->client.left || oldc.bottom-oldc.top!=w->client.bottom-w->client.top)
            SendMessage(w->handle,WM_SIZE,w->style&WS_MINIMIZE?SIZE_MINIMIZED:w->style&WS_MAXIMIZE?SIZE_MAXIMIZED:SIZE_RESTORED,
                        MAKELPARAM(w->client.right-w->client.left,w->client.bottom-w->client.top));
        if(w->used && (oldc.left!=w->client.left || oldc.top!=w->client.top))
            SendMessage(w->handle,WM_MOVE,0,MAKELPARAM(w->client.left-w->parent->client.left,w->client.top-w->parent->client.top));
    }
}
static void to_parent(Wnd *w,RECT *r) {*r=w->window; OffsetRect(r,-w->parent->client.left,-w->parent->client.top);}
BOOL WINAPI SetWindowPos(HWND h,HWND after,int x,int y,int cx,int cy,UINT flags) {
    Wnd *w=WndFromHandle(h); RECT r;
    if(!w || w==desktop) return FALSE;
    to_parent(w,&r);
    if(!(flags&SWP_NOMOVE)) OffsetRect(&r,x-r.left,y-r.top);
    if(!(flags&SWP_NOSIZE)) {r.right=r.left+max(cx,0); r.bottom=r.top+max(cy,0);}
    if(!(flags&SWP_NOZORDER)) {
        Wnd *a=after==HWND_TOP || after==HWND_TOPMOST || after==HWND_NOTOPMOST?NULL:WndFromHandle(after);
        if(after==HWND_TOPMOST) w->exstyle|=WS_EX_TOPMOST;
        if(after==HWND_NOTOPMOST) w->exstyle&=~WS_EX_TOPMOST;
        unlink_wnd(w);
        if(after==HWND_BOTTOM) link_bottom(w);
        else if(a && a->parent==w->parent && a!=w) link_after(w,a);
        else link_after(w,NULL);
        if(Shown(w)) {InvalidateWnd(w,NULL,TRUE,TRUE); expose_window(w);}
    }
    if((flags&(SWP_NOMOVE|SWP_NOSIZE))!=(SWP_NOMOVE|SWP_NOSIZE) || (flags&SWP_FRAMECHANGED)) {
        if(!(w->style&(WS_MINIMIZE|WS_MAXIMIZE))) w->normal=r;
        if(w->style&WS_MINIMIZE) {w->icon.x=r.left; w->icon.y=r.top-4; w->icon_placed=TRUE;}
        PlaceWindow(w,&r,TRUE);
    }
    if(flags&SWP_HIDEWINDOW) ShowWindow(h,SW_HIDE);
    else if(flags&SWP_SHOWWINDOW) ShowWindow(h,flags&SWP_NOACTIVATE?SW_SHOWNA:SW_SHOW);
    else if(!(flags&SWP_NOACTIVATE) && !(w->style&WS_CHILD) && (w->style&WS_VISIBLE) && !(flags&SWP_NOZORDER)) Activate(w,WA_ACTIVE);
    return TRUE;
}
BOOL WINAPI MoveWindow(HWND h,int x,int y,int width,int height,BOOL repaint) {
    Wnd *w=WndFromHandle(h); RECT r;
    if(!w) return FALSE;
    SetRect(&r,x,y,x+width,y+height);
    if(!(w->style&(WS_MINIMIZE|WS_MAXIMIZE))) w->normal=r;
    PlaceWindow(w,&r,TRUE);
    (void)repaint;
    return TRUE;
}
/* Icons fill rows from the bottom left of the parent's client area. */
static void icon_slot(Wnd *w,POINT *out) {
    Wnd *p=w->parent,*s; int i,cols=max(1,(p->client.right-p->client.left)/ICON_SPACING),bottom=p->client.bottom-p->client.top;
    for(i=0;;i++) {
        POINT pt; BOOL used=FALSE;
        pt.x=(i%cols)*ICON_SPACING; pt.y=bottom-ICON_ROW*(1+i/cols);
        for(s=p->child;s;s=s->next) if(s!=w && (s->style&WS_MINIMIZE) && s->icon_placed && s->icon.x==pt.x && s->icon.y==pt.y) used=TRUE;
        if(!used) {*out=pt; return;}
    }
}
static void icon_rect(Wnd *w,RECT *r) {
    SetRect(r,w->icon.x,w->icon.y+4,w->icon.x+ICON_SPACING,w->icon.y+4+ICON+3+2*CharHeight()+1);
}
UINT WINAPI ArrangeIconicWindows(HWND h) {
    Wnd *p=h?WndFromHandle(h):desktop,*s; UINT n=0;
    if(!p) return 0;
    for(s=p->child;s;s=s->next) if(s->style&WS_MINIMIZE) s->icon_placed=FALSE;
    for(s=p->child;s;s=s->next) if(s->style&WS_MINIMIZE) {
        RECT r; icon_slot(s,&s->icon); s->icon_placed=TRUE; icon_rect(s,&r); PlaceWindow(s,&r,FALSE); n++;
    }
    return n;
}
void ArrangeIcons(Wnd *parent) {ArrangeIconicWindows(parent->handle);}
/* Minimize, maximize or restore: the frame, the client area and WM_SIZE. */
void ShowState(Wnd *w,int command) {
    RECT r; BOOL was_min=(w->style&WS_MINIMIZE)!=0,was_max=(w->style&WS_MAXIMIZE)!=0,minimize,maximize;
    minimize=command==SW_MINIMIZE || command==SW_SHOWMINIMIZED || command==SW_SHOWMINNOACTIVE;
    maximize=command==SW_MAXIMIZE;
    if((minimize?!was_min:maximize?!was_max || was_min:was_min || was_max) &&
       HookActive(WH_CBT) && CallHook(WH_CBT,HCBT_MINMAX,(WPARAM)w->handle,command)) return;
    switch(command) {
    case SW_MINIMIZE: case SW_SHOWMINIMIZED: case SW_SHOWMINNOACTIVE:
        if(was_min) return;
        if(!was_max) to_parent(w,&w->normal);
        w->style=(w->style&~WS_MAXIMIZE)|WS_MINIMIZE;
        if(!w->icon_placed) {icon_slot(w,&w->icon); w->icon_placed=TRUE;}
        icon_rect(w,&r);
        if(focus && (focus==w || IsChild(w->handle,focus->handle))) SetFocus(NULL);
        PlaceWindow(w,&r,TRUE);
        if(Shown(w)) expose_window(w);
        return;
    case SW_MAXIMIZE:
        if(was_max && !was_min) return;
        if(!was_min) to_parent(w,&w->normal);
        w->style=(w->style&~WS_MINIMIZE)|WS_MAXIMIZE;
        MaximizedRect(w,&r);
        PlaceWindow(w,&r,TRUE);
        return;
    default:
        if(!was_min && !was_max) return;
        w->style&=~(WS_MINIMIZE|WS_MAXIMIZE);
        r=w->normal;
        PlaceWindow(w,&r,TRUE);
        return;
    }
}
BOOL WINAPI ShowWindow(HWND h,int command) {
    Wnd *w=WndFromHandle(h); BOOL was;
    if(!w) return FALSE;
    was=(w->style&WS_VISIBLE)!=0;
    if(command==SW_HIDE) {
        if(!was) return FALSE;
        SendMessage(h,WM_SHOWWINDOW,FALSE,0);
        w->style&=~WS_VISIBLE;
        if(Shown(w->parent) || w->parent==desktop) expose_window(w);
        if(capture && (capture==w || IsChild(h,capture->handle))) ReleaseCapture();
        if(focus && (focus==w || IsChild(h,focus->handle))) {
            if(w->style&WS_CHILD) SetFocus(w->parent->handle); else focus=NULL;
        }
        if(w==active) activate_next(w);
        return TRUE;
    }
    if(command==SW_SHOWMINIMIZED || command==SW_MINIMIZE || command==SW_SHOWMINNOACTIVE) ShowState(w,SW_MINIMIZE);
    else if(command==SW_SHOWMAXIMIZED) ShowState(w,SW_MAXIMIZE);
    else if(command==SW_SHOWNORMAL || command==SW_RESTORE) ShowState(w,SW_RESTORE);
    if(!was) {
        SendMessage(h,WM_SHOWWINDOW,TRUE,0);
        if(!w->used) return FALSE;
        w->style|=WS_VISIBLE;
        if(Shown(w)) invalidate_tree(w);
    }
    if(!(w->style&WS_CHILD) && command!=SW_SHOWNA && command!=SW_SHOWNOACTIVATE && command!=SW_SHOWMINNOACTIVE && command!=SW_MINIMIZE) Activate(w,WA_ACTIVE);
    /* The next window becomes active; alone, the icon stays active. */
    else if(command==SW_MINIMIZE && w==active) {Wnd *next=first_visible_top(w); if(next) Activate(next,WA_ACTIVE);}
    if(w->used && w->need_size) send_size(w);
    return was;
}
void WINAPI CloseWindow(HWND h) {ShowWindow(h,SW_MINIMIZE);}
BOOL WINAPI OpenIcon(HWND h) {return ShowWindow(h,SW_SHOWNORMAL);}
void WINAPI ShowOwnedPopups(HWND h,BOOL show) {
    Wnd *o,*w=WndFromHandle(h);
    if(!w) return;
    for(o=desktop->child;o;o=o->next) if(o->owner==w && (o->style&WS_POPUP)) ShowWindow(o->handle,show?SW_SHOWNA:SW_HIDE);
}
BOOL WINAPI IsWindow(HWND h) {return WndFromHandle(h)!=NULL;}
BOOL WINAPI IsWindowVisible(HWND h) {Wnd *w=WndFromHandle(h); return w && Shown(w);}
BOOL WINAPI IsIconic(HWND h) {Wnd *w=WndFromHandle(h); return w && (w->style&WS_MINIMIZE);}
BOOL WINAPI IsZoomed(HWND h) {Wnd *w=WndFromHandle(h); return w && (w->style&WS_MAXIMIZE);}
BOOL WINAPI IsWindowEnabled(HWND h) {Wnd *w=WndFromHandle(h); return w && Enabled(w);}
BOOL WINAPI EnableWindow(HWND h,BOOL enable) {
    Wnd *w=WndFromHandle(h); BOOL was_disabled;
    if(!w) return FALSE;
    was_disabled=(w->style&WS_DISABLED)!=0;
    if(enable && was_disabled) {w->style&=~WS_DISABLED; SendMessage(h,WM_ENABLE,TRUE,0);}
    else if(!enable && !was_disabled) {
        w->style|=WS_DISABLED;
        if(focus && (focus==w || IsChild(h,focus->handle))) SetFocus(NULL);
        if(capture==w) ReleaseCapture();
        SendMessage(h,WM_ENABLE,FALSE,0);
    }
    return was_disabled;
}

/* --- destruction ----------------------------------------------------------------------- */
static void free_wnd(Wnd *w,BOOL notify) {
    Prop *p,*next;
    while(w->child) free_wnd(w->child,notify);
    if(notify) SendMessage(w->handle,WM_NCDESTROY,0,0);
    unlink_wnd(w);
    KillWindowTimers(w); CaretOff(w); ClipboardWindowGone(w);
    if(w->extra) GlobalFree(w->extra);
    if(w->text) GlobalFree(w->text);
    for(p=w->props;p;p=next) {next=p->next; GlobalFree(p);}
    if(w->own_dc) DeleteDC(w->own_dc);
    FreeWindowMenus(w);
    if(focus==w) focus=NULL;
    if(capture==w) capture=NULL;
    if(active==w) active=NULL;
    {int i; for(i=1;i<WINDOWS;i++) if(windows[i].used && windows[i].last_active==w) windows[i].last_active=NULL;}
    w->used=FALSE;
}
static void send_destroy(Wnd *w) {
    Wnd *c,*next;
    if(!w->used) return;
    SendMessage(w->handle,WM_DESTROY,0,0);
    for(c=w->child;c;c=next) {next=c->next; send_destroy(c);}
}
static void destroy(Wnd *w,BOOL notify) {
    BOOL was_active=w==active,visible=Shown(w),child=(w->style&WS_CHILD)!=0; Wnd *parent=w->parent,*o;
    RECT area=w->window;
    int i;
    /* Owned windows go first. */
    for(i=1;i<WINDOWS;i++) if(windows[i].used && windows[i].owner==w) {
        o=&windows[i];
        if(notify) DestroyWindow(o->handle); else destroy(o,FALSE);
    }
    if(!w->used) return;
    if(child && notify && !(w->exstyle&WS_EX_NOPARENTNOTIFY) && parent->used)
        SendMessage(parent->handle,WM_PARENTNOTIFY,MAKEWPARAM(WM_DESTROY,w->id),(LPARAM)w->handle);
    if(!w->used) return;
    if(capture && (capture==w || IsChild(w->handle,capture->handle))) ReleaseCapture();
    if(notify) send_destroy(w);
    if(!w->used) return;
    if(focus && (focus==w || IsChild(w->handle,focus->handle))) {
        focus=NULL;
        if(child && parent->used && notify && parent->queue==CurrentQueue()) SetFocus(parent->handle);
    }
    free_wnd(w,notify);
    if(visible) {
        if(parent==desktop) Expose(&area);
        else if(parent->used) {
            RECT c; if(IntersectRect(&c,&area,&parent->client)) {OffsetRect(&c,-parent->client.left,-parent->client.top); InvalidateWnd(parent,&c,TRUE,FALSE); expose_in(parent,&area);}
        }
    }
    if(was_active) {active=NULL; activate_next(w);}
}
BOOL WINAPI DestroyWindow(HWND h) {
    Wnd *w=WndFromHandle(h);
    if(!w || w==desktop) return FALSE;
    if(HookActive(WH_CBT) && (CallHook(WH_CBT,HCBT_DESTROYWND,(WPARAM)h,0) || !w->used)) return FALSE;
    if(w->parent==desktop && !w->owner && !(w->style&WS_CHILD)) CallHook(WH_SHELL,HSHELL_WINDOWDESTROYED,(WPARAM)h,0);
    if(!w->used) return FALSE;
    destroy(w,TRUE);
    return TRUE;
}
/* A task ended: its windows go without messages. */
void DestroyTaskWindows(Queue *q) {
    int i;
    for(i=1;i<WINDOWS;i++) if(windows[i].used && windows[i].queue==q && windows[i].parent==desktop) destroy(&windows[i],FALSE);
    for(i=1;i<WINDOWS;i++) if(windows[i].used && windows[i].queue==q) destroy(&windows[i],FALSE);
}

/* --- relationships and enumeration ------------------------------------------------------- */
HWND WINAPI GetParent(HWND h) {
    Wnd *w=WndFromHandle(h);
    if(!w) return NULL;
    if(w->style&WS_CHILD) return w->parent==desktop?NULL:w->parent->handle;
    return w->owner?w->owner->handle:NULL;
}
HWND WINAPI SetParent(HWND h,HWND parent) {
    Wnd *w=WndFromHandle(h),*p=parent?WndFromHandle(parent):desktop,*old; RECT r;
    if(!w || !p || w==desktop) return NULL;
    old=w->parent;
    to_parent(w,&r);
    if(Shown(w)) expose_window(w);
    unlink_wnd(w); w->parent=p; link_bottom(w);
    PlaceWindow(w,&r,TRUE);
    return old==desktop?NULL:old->handle;
}
BOOL WINAPI IsChild(HWND parent,HWND h) {
    Wnd *w=WndFromHandle(h),*p=WndFromHandle(parent);
    if(!w || !p) return FALSE;
    for(w=w->parent;w && w!=desktop;w=w->parent) if(w==p) return TRUE;
    return FALSE;
}
HWND WINAPI GetWindow(HWND h,UINT cmd) {
    Wnd *w=WndFromHandle(h),*s;
    if(!w) return NULL;
    switch(cmd) {
    case GW_HWNDFIRST: return w->parent?w->parent->child->handle:NULL;
    case GW_HWNDLAST: if(!w->parent) return NULL; for(s=w->parent->child;s->next;s=s->next); return s->handle;
    case GW_HWNDNEXT: return w->next?w->next->handle:NULL;
    case GW_HWNDPREV:
        if(!w->parent || w->parent->child==w) return NULL;
        for(s=w->parent->child;s->next!=w;s=s->next);
        return s->handle;
    case GW_OWNER: return w->owner?w->owner->handle:NULL;
    case GW_CHILD: return w->child?w->child->handle:NULL;
    }
    return NULL;
}
HWND WINAPI GetTopWindow(HWND h) {Wnd *w=h?WndFromHandle(h):desktop; return w && w->child?w->child->handle:NULL;}
HWND WINAPI GetNextWindow(HWND h,UINT cmd) {return GetWindow(h,cmd);}
/* Snapshots, so the callbacks may destroy windows. */
static BOOL enum_list(Wnd **list,int n,WNDENUMPROC proc,LPARAM lp) {
    int i;
    for(i=0;i<n;i++) if(list[i]->used && !proc(list[i]->handle,lp)) return FALSE;
    return TRUE;
}
BOOL WINAPI EnumWindows(WNDENUMPROC proc,LPARAM lp) {
    Wnd *list[WINDOWS],*w; int n=0;
    for(w=desktop->child;w;w=w->next) list[n++]=w;
    return enum_list(list,n,proc,lp);
}
static void collect(Wnd *p,Wnd **list,int *n) {Wnd *c; for(c=p->child;c;c=c->next) {list[(*n)++]=c; collect(c,list,n);}}
BOOL WINAPI EnumChildWindows(HWND h,WNDENUMPROC proc,LPARAM lp) {
    static Wnd *list[WINDOWS]; Wnd *p=WndFromHandle(h); int n=0;
    if(!p) return FALSE;
    collect(p,list,&n);
    return enum_list(list,n,proc,lp);
}
BOOL WINAPI EnumTaskWindows(HTASK task,WNDENUMPROC proc,LPARAM lp) {
    Wnd *list[WINDOWS],*w; int n=0; Queue *q=QueueOfTask(task);
    for(w=desktop->child;w;w=w->next) if(w->queue==q) list[n++]=w;
    return enum_list(list,n,proc,lp);
}
HTASK WINAPI GetWindowTask(HWND h) {Wnd *w=WndFromHandle(h); return w && w->queue?w->queue->htask:NULL;}
HWND WINAPI FindWindow(LPCSTR class_name,LPCSTR title) {
    Wnd *w;
    for(w=desktop->child;w;w=w->next) {
        if(class_name && (IS_INTRESOURCE(class_name)?w->cls->atom!=(ATOM)(ULONG_PTR)class_name:lstrcmpi(class_name,w->cls->name))) continue;
        if(title && lstrcmp(title,w->text?w->text:"")) continue;
        return w->handle;
    }
    return NULL;
}
int WINAPI GetDlgCtrlID(HWND h) {Wnd *w=WndFromHandle(h); return w?(int)w->id:0;}

/* --- text, long values and properties -------------------------------------------------------- */
void WINAPI SetWindowText(HWND h,LPCSTR text) {SendMessage(h,WM_SETTEXT,0,(LPARAM)text);}
int WINAPI GetWindowText(HWND h,LPSTR text,int size) {
    Wnd *w=WndFromHandle(h);
    if(!w || !text || size<=0) return 0;
    text[0]=0;
    return (int)SendMessage(h,WM_GETTEXT,(WPARAM)size,(LPARAM)text);
}
int WINAPI GetWindowTextLength(HWND h) {return (int)SendMessage(h,WM_GETTEXTLENGTH,0,0);}
LONG_PTR WINAPI GetWindowLongPtr(HWND h,int index) {
    Wnd *w=WndFromHandle(h); LONG_PTR v=0;
    if(!w) return 0;
    switch(index) {
    case GWL_WNDPROC: return (LONG_PTR)w->proc;
    case GWL_HINSTANCE: return (LONG_PTR)w->instance;
    case GWL_HWNDPARENT: return (LONG_PTR)GetParent(h);
    case GWL_STYLE: return (LONG)w->style;
    case GWL_EXSTYLE: return (LONG)w->exstyle;
    case GWL_USERDATA: return w->user;
    case GWL_ID: return (LONG_PTR)w->id;
    }
    if(index>=0 && index+(int)sizeof(v)<=w->extra_bytes) memcpy(&v,w->extra+index,sizeof(v));
    return v;
}
LONG_PTR WINAPI SetWindowLongPtr(HWND h,int index,LONG_PTR value) {
    Wnd *w=WndFromHandle(h); LONG_PTR old=GetWindowLongPtr(h,index);
    if(!w) return 0;
    switch(index) {
    case GWL_WNDPROC: w->proc=(WNDPROC)value; return old;
    case GWL_STYLE: {
        DWORD before=w->style; w->style=(DWORD)value;
        if((before^w->style)&WS_VISIBLE) {w->style^=WS_VISIBLE; ShowWindow(h,value&WS_VISIBLE?SW_SHOWNA:SW_HIDE);}
        return old;
    }
    case GWL_EXSTYLE: w->exstyle=(DWORD)value; return old;
    case GWL_USERDATA: w->user=value; return old;
    case GWL_ID: w->id=(UINT_PTR)value; return old;
    case GWL_HWNDPARENT: {Wnd *o=WndFromHandle((HWND)value); if(!(w->style&WS_CHILD)) w->owner=o; return old;}
    }
    if(index>=0 && index+(int)sizeof(value)<=w->extra_bytes) memcpy(w->extra+index,&value,sizeof(value));
    return old;
}
LONG WINAPI GetWindowLong(HWND h,int index) {
    Wnd *w=WndFromHandle(h); LONG v=0;
    if(w && index>=0) {if(index+(int)sizeof(v)<=w->extra_bytes) memcpy(&v,w->extra+index,sizeof(v)); return v;}
    return (LONG)GetWindowLongPtr(h,index);
}
LONG WINAPI SetWindowLong(HWND h,int index,LONG value) {
    Wnd *w=WndFromHandle(h); LONG old=GetWindowLong(h,index);
    if(w && index>=0) {if(index+(int)sizeof(value)<=w->extra_bytes) memcpy(w->extra+index,&value,sizeof(value)); return old;}
    SetWindowLongPtr(h,index,(LONG_PTR)value);
    return old;
}
WORD WINAPI GetWindowWord(HWND h,int index) {
    Wnd *w=WndFromHandle(h); WORD v=0;
    if(!w) return 0;
    if(index==GWW_ID) return (WORD)w->id;
    if(index>=0 && index+(int)sizeof(v)<=w->extra_bytes) memcpy(&v,w->extra+index,sizeof(v));
    return v;
}
WORD WINAPI SetWindowWord(HWND h,int index,WORD value) {
    Wnd *w=WndFromHandle(h); WORD old=GetWindowWord(h,index);
    if(!w) return 0;
    if(index==GWW_ID) w->id=value;
    else if(index>=0 && index+(int)sizeof(value)<=w->extra_bytes) memcpy(w->extra+index,&value,sizeof(value));
    return old;
}
static Prop **find_prop(Wnd *w,LPCSTR name) {
    Prop **p;
    for(p=&w->props;*p;p=&(*p)->next)
        if(IS_INTRESOURCE(name)?(*p)->atom==(ATOM)(ULONG_PTR)name:(!(*p)->atom && !lstrcmpi((*p)->name,name))) return p;
    return NULL;
}
BOOL WINAPI SetProp(HWND h,LPCSTR name,HANDLE data) {
    Wnd *w=WndFromHandle(h); Prop **p,*n;
    if(!w || !name) return FALSE;
    if((p=find_prop(w,name))!=NULL) {(*p)->data=data; return TRUE;}
    if(!(n=(Prop *)ualloc(sizeof(Prop)))) return FALSE;
    if(IS_INTRESOURCE(name)) n->atom=(ATOM)(ULONG_PTR)name; else lstrcpyn(n->name,name,sizeof(n->name));
    n->data=data; n->next=w->props; w->props=n;
    return TRUE;
}
HANDLE WINAPI GetProp(HWND h,LPCSTR name) {
    Wnd *w=WndFromHandle(h); Prop **p;
    return w && name && (p=find_prop(w,name))!=NULL?(*p)->data:NULL;
}
HANDLE WINAPI RemoveProp(HWND h,LPCSTR name) {
    Wnd *w=WndFromHandle(h); Prop **p,*gone; HANDLE data;
    if(!w || !name || !(p=find_prop(w,name))) return NULL;
    gone=*p; data=gone->data; *p=gone->next; GlobalFree(gone);
    return data;
}
BOOL WINAPI IsWindowUnicode(HWND h) {(void)h; return FALSE;}

/* --- geometry queries ---------------------------------------------------------------------------- */
void WINAPI GetClientRect(HWND h,LPRECT r) {
    Wnd *w=WndFromHandle(h);
    if(!w) w=desktop;
    SetRect(r,0,0,w->client.right-w->client.left,w->client.bottom-w->client.top);
}
void WINAPI GetWindowRect(HWND h,LPRECT r) {
    Wnd *w=WndFromHandle(h);
    *r=(w?w:desktop)->window;
}

/* --- DefWindowProc ---------------------------------------------------------------------------------- */
/* The background a window's icon or children show through. */
static HBRUSH parent_background(Wnd *w) {
    if(w->parent==desktop) return SysBrush(COLOR_BACKGROUND);
    if(w->parent->cls->wc.hbrBackground) return w->parent->cls->wc.hbrBackground;
    return SysBrush(COLOR_APPWORKSPACE);
}
static void fill_brush(HDC dc,const RECT *r,HBRUSH brush) {FillRect(dc,r,brush);}
LRESULT WINAPI DefWindowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h);
    if(!w) return 0;
    switch(msg) {
    case WM_NCCREATE: return TRUE;
    case WM_NCPAINT: PaintFrame(w); return 0;
    case WM_NCACTIVATE: if(Shown(w)) PaintFrame(w); return TRUE;
    case WM_ACTIVATE:
        if(LOWORD(wp)!=WA_INACTIVE && !(w->style&WS_MINIMIZE) && (!focus || TopLevel(focus)!=w)) SetFocus(h);
        else if(LOWORD(wp)!=WA_INACTIVE && (w->style&WS_MINIMIZE)) SetFocus(NULL);
        return 0;
    case WM_ERASEBKGND: {
        RECT r; HBRUSH brush=w->cls->wc.hbrBackground;
        if(!brush) return 0;
        GetClientRect(h,&r); fill_brush((HDC)wp,&r,brush);
        return 1;
    }
    case WM_ICONERASEBKGND: {
        RECT r; GetClientRect(h,&r); fill_brush((HDC)wp,&r,parent_background(w)); return 1;
    }
    case WM_PAINTICON: case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        if((w->style&WS_MINIMIZE) && w->cls->wc.hIcon) DrawIcon(dc,0,0,w->cls->wc.hIcon);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SETTEXT:
        /* The caption, or an icon's title, is drawn again at once; the
         * client area is left as it is. */
        SetWndText(w,(LPCSTR)lp);
        if(((w->style&WS_CAPTION)==WS_CAPTION || (w->style&WS_MINIMIZE)) && Shown(w)) PaintFrame(w);
        return TRUE;
    case WM_GETTEXT: {
        int n=0; char *d=(char *)lp;
        if(!d || !wp) return 0;
        while(w->text && w->text[n] && n<(int)wp-1) {d[n]=w->text[n]; n++;}
        d[n]=0; return n;
    }
    case WM_GETTEXTLENGTH: return w->text?lstrlen(w->text):0;
    case WM_SETREDRAW: w->redraw_off=!wp; if(wp) InvalidateWnd(w,NULL,TRUE,TRUE); return 0;
    case WM_NCHITTEST: {POINT p; p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp); return HitTest(w,p);}
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK: {
        POINT p; p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp);
        return NcButtonDown(w,(int)wp,p,msg==WM_NCLBUTTONDBLCLK);
    }
    case WM_LBUTTONDBLCLK: return 0;
    case WM_SETCURSOR: {
        int hit=(short)LOWORD(lp);
        if((w->style&WS_CHILD) && w->parent!=desktop && SendMessage(w->parent->handle,WM_SETCURSOR,wp,lp)) return TRUE;
        if(hit==HTERROR) {if(HIWORD(lp)==WM_LBUTTONDOWN) MessageBeep(0); return TRUE;}
        switch(hit) {
        case HTCLIENT: if(w->cls->wc.hCursor) SetCursorShape(w->cls->wc.hCursor); else return FALSE; break;
        case HTLEFT: case HTRIGHT: SetCursorShape(StockCursor(IDC_SIZEWE)); break;
        case HTTOP: case HTBOTTOM: SetCursorShape(StockCursor(IDC_SIZENS)); break;
        case HTTOPLEFT: case HTBOTTOMRIGHT: SetCursorShape(StockCursor(IDC_SIZENWSE)); break;
        case HTTOPRIGHT: case HTBOTTOMLEFT: SetCursorShape(StockCursor(IDC_SIZENESW)); break;
        default: SetCursorShape(StockCursor(IDC_ARROW));
        }
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        if((w->style&WS_CHILD) && w->parent!=desktop) {LRESULT r=SendMessage(w->parent->handle,WM_MOUSEACTIVATE,wp,lp); if(r) return r;}
        return MA_ACTIVATE;
    case WM_KEYDOWN:
        if(wp==VK_F10) SendMessage(h,WM_SYSCOMMAND,SC_KEYMENU,0);
        return 0;
    case WM_SYSKEYDOWN:
        if(wp==VK_F4 && !(w->style&WS_CHILD)) PostMessage(h,WM_SYSCOMMAND,SC_CLOSE,0);
        else if(wp==VK_F4 && (w->style&WS_CHILD)) PostMessage(TopLevel(w)->handle,WM_SYSCOMMAND,SC_CLOSE,0);
        else if(wp==VK_F10) SendMessage(h,WM_SYSCOMMAND,SC_KEYMENU,0);
        else if(wp==VK_TAB || wp==VK_ESCAPE) SendMessage(h,WM_SYSCOMMAND,SC_NEXTWINDOW,0);
        return 0;
    case WM_SYSCHAR:
        if(wp==VK_TAB || wp==VK_ESCAPE) return 0;
        if((w->style&WS_CHILD) && w->parent!=desktop) return SendMessage(TopLevel(w)->handle,msg,wp,lp);
        SendMessage(h,WM_SYSCOMMAND,SC_KEYMENU,(LPARAM)wp);
        return 0;
    case WM_SYSCOMMAND: if(!CallHook(WH_CBT,HCBT_SYSCOMMAND,wp,lp)) SysCommand(w,wp,lp); return 0;
    case WM_GETMINMAXINFO: return 0;
    case WM_QUERYENDSESSION: case WM_QUERYOPEN: return TRUE;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return (LRESULT)ControlColor(WndFromHandle((HWND)lp),(HDC)wp,msg);
    case WM_VKEYTOITEM: case WM_CHARTOITEM: return -1;
    case WM_SETFONT: w->font=(HFONT)wp; if(LOWORD(lp)) InvalidateWnd(w,NULL,TRUE,FALSE); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_QUERYDRAGICON: return (LRESULT)w->cls->wc.hIcon;
    case WM_CANCELMODE: if(capture==w) ReleaseCapture(); return 0;
    case WM_SHOWWINDOW: return 0;
    }
    return 0;
}

void WndInit(void) {
    wh_u32 w,h;
    memset(classes,0,sizeof(classes)); memset(windows,0,sizeof(windows));
    wh_display_size(&w,&h); screen_width=(int)w; screen_height=(int)h;
    desktop=&windows[0]; desktop->used=TRUE; desktop->handle=(HWND)(ULONG_PTR)HWND_BASE;
    desktop->style=WS_VISIBLE|WS_CLIPCHILDREN; SetRect(&desktop->window,0,0,screen_width,screen_height); desktop->client=desktop->window;
    active=focus=capture=NULL; cascade=0;
    desktop_update=desktop->window;
}
