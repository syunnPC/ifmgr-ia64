/* SPDX-License-Identifier: GPL-2.0-or-later
 * The multiple-document interface: the MDICLIENT control, DefFrameProc,
 * DefMDIChildProc and TranslateMDISysAccel. Children are numbered from the
 * client's first child id and listed in the frame's Window menu. A
 * maximized child fills the client area with its frame outside it, its
 * system menu box and restore button join the frame's menu bar, and the
 * frame's title shows the child's.
 */
#include "user.h"
#define MDI_MAX 64

typedef struct {
    HMENU window_menu; UINT first; int count; HWND children[MDI_MAX];
    HWND active; BOOL maximized; char frame_title[128]; HMENU merged; int cascade;
} Mdi;
static Mdi *mdi_of(HWND client) {return (Mdi *)GetWindowLongPtr(client,0);}
static HWND client_of(HWND child) {return GetParent(child);}

/* --- the Window menu --------------------------------------------------------------- */
static void update_window_menu(Mdi *m) {
    int i,n;
    if(!m->window_menu) return;
    n=GetMenuItemCount(m->window_menu);
    /* Remove the old list (and its separator). */
    for(i=n-1;i>=0;i--) {
        UINT id=GetMenuItemID(m->window_menu,i);
        if(id>=m->first && id<m->first+MDI_MAX) DeleteMenu(m->window_menu,(UINT)i,MF_BYPOSITION);
    }
    n=GetMenuItemCount(m->window_menu);
    if(n>0 && (GetMenuState(m->window_menu,(UINT)(n-1),MF_BYPOSITION)&MF_SEPARATOR)) DeleteMenu(m->window_menu,(UINT)(n-1),MF_BYPOSITION);
    if(!m->count) return;
    AppendMenu(m->window_menu,MF_SEPARATOR,0,NULL);
    for(i=0;i<m->count && i<9;i++) {
        char title[96],text[110];
        GetWindowText(m->children[i],title,sizeof(title));
        wsprintf(text,"&%d %s",i+1,title);
        AppendMenu(m->window_menu,MF_STRING|(m->children[i]==m->active?MF_CHECKED:0),m->first+i,text);
    }
}
static void renumber(Mdi *m) {
    int i;
    for(i=0;i<m->count;i++) SetWindowLongPtr(m->children[i],GWL_ID,(LONG_PTR)(m->first+i));
}

/* --- maximized children: the frame's title and menu bar ------------------------------ */
static HWND frame_of(HWND client) {return GetParent(client);}
static void merge(HWND client,Mdi *m,HWND child) {
    HWND frame=frame_of(client); HMENU bar=GetMenu(frame); char title[128],text[260];
    if(!m->frame_title[0]) GetWindowText(frame,m->frame_title,sizeof(m->frame_title));
    GetWindowText(child,title,sizeof(title));
    wsprintf(text,"%s - [%s]",m->frame_title,title);
    DefWindowProc(frame,WM_SETTEXT,0,(LPARAM)text);
    InvalidateWnd(WndFromHandle(frame),NULL,FALSE,TRUE);
    if(bar && !m->merged) {
        HMENU sys=GetSystemMenu(child,FALSE);
        InsertMenu(bar,0,MF_BYPOSITION|MF_POPUP,(UINT_PTR)sys,"-");
        AppendMenu(bar,MF_STRING|MF_HELP,SC_RESTORE,"\a\x12");
        m->merged=sys;
        DrawMenuBar(frame);
    }
}
static void unmerge(HWND client,Mdi *m) {
    HWND frame=frame_of(client); HMENU bar=GetMenu(frame);
    if(m->frame_title[0]) {DefWindowProc(frame,WM_SETTEXT,0,(LPARAM)m->frame_title); InvalidateWnd(WndFromHandle(frame),NULL,FALSE,TRUE); m->frame_title[0]=0;}
    if(bar && m->merged) {
        int n=GetMenuItemCount(bar);
        if(n>0 && GetMenuItemID(bar,n-1)==SC_RESTORE) RemoveMenu(bar,(UINT)(n-1),MF_BYPOSITION);
        if(GetSubMenu(bar,0)==m->merged) RemoveMenu(bar,0,MF_BYPOSITION);
        m->merged=NULL;
        DrawMenuBar(frame);
    }
}

/* --- activation -------------------------------------------------------------------------- */
static void activate(HWND client,Mdi *m,HWND child) {
    HWND old=m->active;
    if(child==old) {if(child && GetFocus()!=child && !IsChild(child,GetFocus())) SetFocus(child); return;}
    /* The frame painting asks which child is active: switch before repainting. */
    m->active=child;
    if(old && IsWindow(old)) {
        SendMessage(old,WM_NCACTIVATE,FALSE,0);
        SendMessage(old,WM_MDIACTIVATE,(WPARAM)old,(LPARAM)child);
    }
    if(child) {
        if(m->maximized && old && IsWindow(old) && old!=child) {
            /* The maximized state moves to the new child. */
            Wnd *o=WndFromHandle(old); if(o) ShowState(o,SW_RESTORE);
            unmerge(client,m);
            ShowWindow(child,SW_SHOWMAXIMIZED);
            merge(client,m,child);
        }
        SetWindowPos(child,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SendMessage(child,WM_NCACTIVATE,TRUE,0);
        SendMessage(child,WM_MDIACTIVATE,(WPARAM)old,(LPARAM)child);
        if(GetActiveWindow()==GetParent(client) || GetActiveWindow()==TopLevel(WndFromHandle(client))->handle) SetFocus(child);
    }
    update_window_menu(m);
}
static int index_of(Mdi *m,HWND child) {int i; for(i=0;i<m->count;i++) if(m->children[i]==child) return i; return -1;}
static HWND next_child(Mdi *m,HWND from,BOOL previous) {
    int i=index_of(m,from),k;
    for(k=1;k<=m->count;k++) {
        HWND c=m->children[((i<0?0:i)+(previous?-k:k)%m->count+m->count)%m->count];
        if(c!=from && IsWindowVisible(c)) return c;
    }
    return NULL;
}

/* --- arranging ---------------------------------------------------------------------------- */
static int arrangeable(Mdi *m,HWND *list) {
    int i,n=0;
    for(i=0;i<m->count;i++) if(IsWindowVisible(m->children[i]) && !IsIconic(m->children[i])) list[n++]=m->children[i];
    return n;
}
static int icon_space(HWND client) {
    Wnd *c=WndFromHandle(client),*s;
    for(s=c->child;s;s=s->next) if(s->style&WS_MINIMIZE) return ICON_ROW;
    return 0;
}
static void restore_max(HWND client,Mdi *m) {
    if(m->maximized && m->active) {m->maximized=FALSE; unmerge(client,m); ShowWindow(m->active,SW_RESTORE);}
}
static void cascade(HWND client,Mdi *m) {
    HWND list[MDI_MAX]; int n,i,step=CAPTION+FRAME-1; RECT r;
    restore_max(client,m);
    n=arrangeable(m,list);
    GetClientRect(client,&r); r.bottom-=icon_space(client);
    for(i=0;i<n;i++) {
        int k=n-1-i,x=k*step,y=k*step,w=max(r.right-(n-1)*step,r.right*2/3),h=max(r.bottom-(n-1)*step,r.bottom*2/3);
        SetWindowPos(list[i],NULL,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
}
static void tile(HWND client,Mdi *m,UINT how) {
    HWND list[MDI_MAX]; int n,i,cols,rows; RECT r;
    restore_max(client,m);
    n=arrangeable(m,list);
    if(!n) return;
    GetClientRect(client,&r); r.bottom-=icon_space(client);
    if(how&MDITILE_HORIZONTAL) {cols=1; rows=n;}
    else {for(cols=1;cols*cols<n;cols++); rows=(n+cols-1)/cols;}
    for(i=0;i<n;i++) {
        int c=i/rows,row=i%rows,in_col=c==cols-1?n-c*rows:rows,w=r.right/cols,h=r.bottom/max(1,in_col);
        if(how&MDITILE_HORIZONTAL) {w=r.right; h=r.bottom/n; c=0; row=i;}
        SetWindowPos(list[i],NULL,c*w,row*h,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
}

/* --- the client window ------------------------------------------------------------------------ */
static HWND create_child(HWND client,Mdi *m,MDICREATESTRUCT *cs) {
    HWND child; RECT r; int x=cs->x,y=cs->y,w=cs->cx,h=cs->cy; DWORD style;
    if(m->count>=MDI_MAX) return NULL;
    GetClientRect(client,&r);
    if(x==CW_USEDEFAULT) {int step=CAPTION+FRAME-1; x=y=(m->cascade%8)*step; m->cascade++;}
    if(w==CW_USEDEFAULT) {w=r.right*3/4; h=r.bottom*3/4;}
    style=cs->style|WS_CHILD|WS_CLIPSIBLINGS|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
    if(m->maximized) style&=~WS_MAXIMIZE;
    child=CreateWindowEx(0,cs->szClass,cs->szTitle,style&~(WS_VISIBLE|WS_MAXIMIZE|WS_MINIMIZE),x,y,w,h,client,(HMENU)(ULONG_PTR)(m->first+m->count),cs->hOwner,cs);
    if(!child) return NULL;
    m->children[m->count++]=child;
    ShowWindow(child,(cs->style&WS_MINIMIZE)?SW_SHOWMINNOACTIVE:SW_SHOWNA);
    if((cs->style&WS_MAXIMIZE) || m->maximized) {
        HWND was=m->active;
        if(m->maximized && was) {m->maximized=FALSE; unmerge(client,m); ShowWindow(was,SW_RESTORE);}
        m->active=child;
        ShowWindow(child,SW_SHOWMAXIMIZED);
        m->maximized=TRUE; merge(client,m,child);
        m->active=was;
    }
    activate(client,m,child);
    return child;
}
static void destroy_child(HWND client,Mdi *m,HWND child) {
    int i=index_of(m,child);
    if(i<0) return;
    if(m->active==child) {
        HWND next=next_child(m,child,FALSE);
        if(m->maximized) {unmerge(client,m); if(!next) m->maximized=FALSE;}
        if(next) {
            if(m->maximized) {m->active=NULL; ShowWindow(next,SW_SHOWMAXIMIZED); merge(client,m,next);}
            activate(client,m,next);
        } else m->active=NULL;
    }
    memmove(&m->children[i],&m->children[i+1],sizeof(HWND)*(unsigned)(m->count-i-1));
    m->count--;
    renumber(m);
    DestroyWindow(child);
    update_window_menu(m);
    if(!m->active && GetActiveWindow()==GetParent(client)) SetFocus(GetParent(client));
}
LRESULT CALLBACK MDIClientProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Mdi *m=mdi_of(h);
    if(!m && msg!=WM_NCCREATE) return DefWindowProc(h,msg,wp,lp);
    switch(msg) {
    case WM_NCCREATE: {
        LPCREATESTRUCT cs=(LPCREATESTRUCT)lp; LPCLIENTCREATESTRUCT ccs=(LPCLIENTCREATESTRUCT)cs->lpCreateParams;
        m=(Mdi *)GlobalAlloc(GPTR|GMEM_SHARE,sizeof(Mdi));
        if(!m) return FALSE;
        if(ccs) {m->window_menu=ccs->hWindowMenu; m->first=ccs->idFirstChild;}
        if(!m->first) m->first=0x7f00;
        SetWindowLongPtr(h,0,(LONG_PTR)m);
        return DefWindowProc(h,msg,wp,lp);
    }
    case WM_NCDESTROY: GlobalFree(m); SetWindowLongPtr(h,0,0); return 0;
    case WM_MDICREATE: return (LRESULT)create_child(h,m,(MDICREATESTRUCT *)lp);
    case WM_MDIDESTROY: destroy_child(h,m,(HWND)wp); return 0;
    case WM_MDIACTIVATE: if(index_of(m,(HWND)wp)>=0) activate(h,m,(HWND)wp); return 0;
    case WM_MDIGETACTIVE: if(lp) *(BOOL *)lp=m->maximized; return (LRESULT)m->active;
    case WM_MDINEXT: {
        HWND from=wp?(HWND)wp:m->active,to=next_child(m,from,lp!=0);
        if(to) {
            activate(h,m,to);
            if(!lp && from && !m->maximized) SetWindowPos(from,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_MDIMAXIMIZE: {
        HWND c=(HWND)wp;
        if(index_of(m,c)<0) return 0;
        if(m->maximized && m->active && m->active!=c) restore_max(h,m);
        activate(h,m,c);
        ShowWindow(c,SW_SHOWMAXIMIZED); m->maximized=TRUE; merge(h,m,c);
        return 0;
    }
    case WM_MDIRESTORE: {
        HWND c=(HWND)wp;
        if(m->maximized && c==m->active) {m->maximized=FALSE; unmerge(h,m);}
        ShowWindow(c,SW_RESTORE);
        return 0;
    }
    case WM_MDICASCADE: cascade(h,m); return TRUE;
    case WM_MDITILE: tile(h,m,(UINT)wp); return TRUE;
    case WM_MDIICONARRANGE: ArrangeIconicWindows(h); return 0;
    case WM_MDISETMENU: {
        HWND frame=frame_of(h); HMENU old=GetMenu(frame);
        if(wp) {if(m->merged) unmerge(h,m); SetMenu(frame,(HMENU)wp); if(m->maximized && m->active) merge(h,m,m->active);}
        if(lp) {m->window_menu=(HMENU)lp; update_window_menu(m);}
        return (LRESULT)old;
    }
    case WM_SIZE:
        if(m->maximized && m->active) {RECT r; MaximizedRect(WndFromHandle(m->active),&r); SetWindowPos(m->active,NULL,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);}
        ArrangeIconicWindows(h);
        return 0;
    case WM_SETFOCUS: if(m->active && IsWindow(m->active)) SetFocus(m->active); return 0;
    case WM_CHILDACTIVATE: return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* --- frames and children -------------------------------------------------------------------- */
LRESULT WINAPI DefFrameProc(HWND h,HWND client,UINT msg,WPARAM wp,LPARAM lp) {
    Mdi *m=client?mdi_of(client):NULL;
    if(m) switch(msg) {
    case WM_COMMAND: {
        UINT id=LOWORD(wp);
        if(id>=m->first && id<m->first+(UINT)m->count) {
            HWND c=m->children[id-m->first];
            if(IsIconic(c)) ShowWindow(c,SW_RESTORE);
            activate(client,m,c);
            return 0;
        }
        if(id>=0xf000 && m->active) return SendMessage(m->active,WM_SYSCOMMAND,id,lp);
        break;
    }
    case WM_SYSCOMMAND:
        if((wp&0xfff0)==SC_RESTORE && m->maximized && m->active && !IsIconic(h) && lp==0) {SendMessage(client,WM_MDIRESTORE,(WPARAM)m->active,0); return 0;}
        break;
    case WM_SETFOCUS: SetFocus(client); return 0;
    case WM_SIZE: {RECT r; GetClientRect(h,&r); MoveWindow(client,0,0,r.right,r.bottom,TRUE); return 0;}
    case WM_NCACTIVATE: if(m->active) SendMessage(m->active,WM_NCACTIVATE,wp,lp); break;
    case WM_MENUCHAR:
        if(LOWORD(wp)=='-' && m->active) {SendMessage(m->active,WM_SYSCOMMAND,SC_KEYMENU,'-'); return MAKELONG(0,1);}
        break;
    case WM_SETTEXT:
        if(m->maximized && m->active) {
            char text[260],title[128];
            lstrcpyn(m->frame_title,lp?(LPCSTR)lp:"",sizeof(m->frame_title));
            GetWindowText(m->active,title,sizeof(title));
            wsprintf(text,"%s - [%s]",m->frame_title,title);
            return DefWindowProc(h,msg,wp,(LPARAM)text);
        }
        break;
    }
    return DefWindowProc(h,msg,wp,lp);
}
LRESULT WINAPI DefMDIChildProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    HWND client=client_of(h); Mdi *m=client?mdi_of(client):NULL;
    if(!m) return DefWindowProc(h,msg,wp,lp);
    switch(msg) {
    case WM_SETFOCUS: if(m->active!=h) activate(client,m,h); break;
    case WM_CHILDACTIVATE: case WM_NCLBUTTONDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if(m->active!=h) activate(client,m,h);
        break;
    case WM_MOUSEACTIVATE: if(m->active!=h) activate(client,m,h); return MA_ACTIVATE;
    case WM_CLOSE: SendMessage(client,WM_MDIDESTROY,(WPARAM)h,0); return 0;
    case WM_SETTEXT: {
        LRESULT r=DefWindowProc(h,msg,wp,lp);
        update_window_menu(m);
        if(m->maximized && m->active==h) merge(client,m,h);
        return r;
    }
    case WM_SYSCOMMAND:
        switch(wp&0xfff0) {
        case SC_MAXIMIZE: SendMessage(client,WM_MDIMAXIMIZE,(WPARAM)h,0); return 0;
        case SC_RESTORE:
            if(m->maximized && m->active==h) {SendMessage(client,WM_MDIRESTORE,(WPARAM)h,0); return 0;}
            break;
        case SC_MINIMIZE:
            if(m->maximized && m->active==h) {m->maximized=FALSE; unmerge(client,m);}
            ShowWindow(h,SW_MINIMIZE);
            {HWND next=next_child(m,h,FALSE); if(next) activate(client,m,next);}
            return 0;
        case SC_NEXTWINDOW: SendMessage(client,WM_MDINEXT,(WPARAM)h,0); return 0;
        case SC_PREVWINDOW: SendMessage(client,WM_MDINEXT,(WPARAM)h,1); return 0;
        case SC_KEYMENU:
            if(lp!='-') {HWND frame=GetParent(client); return SendMessage(frame,WM_SYSCOMMAND,wp,lp);}
            break;
        }
        break;
    case WM_SYSCHAR:
        if(wp=='-') {SendMessage(h,WM_SYSCOMMAND,SC_KEYMENU,'-'); return 0;}
        return SendMessage(GetParent(client),msg,wp,lp);
    case WM_GETMINMAXINFO: {
        /* Maximized, the frame lies just outside the client area. */
        LPMINMAXINFO mm=(LPMINMAXINFO)lp; RECT r; GetClientRect(client,&r);
        mm->ptMaxSize.x=r.right+2*FRAME; mm->ptMaxSize.y=r.bottom+2*FRAME+CAPTION-1;
        mm->ptMaxPosition.x=-FRAME; mm->ptMaxPosition.y=-FRAME-(CAPTION-1);
        return 0;
    }
    case WM_SIZE:
        if(wp==SIZE_MAXIMIZED && !m->maximized && m->active==h) {m->maximized=TRUE; merge(client,m,h);}
        else if(wp!=SIZE_MAXIMIZED && m->maximized && m->active==h) {m->maximized=FALSE; unmerge(client,m);}
        break;
    }
    return DefWindowProc(h,msg,wp,lp);
}
BOOL WINAPI TranslateMDISysAccel(HWND client,LPMSG msg) {
    Mdi *m=client?mdi_of(client):NULL; UINT cmd=0;
    if(!m || !m->active || (msg->message!=WM_KEYDOWN && msg->message!=WM_SYSKEYDOWN) || !KeyDown(VK_CONTROL)) return FALSE;
    if(msg->wParam==VK_F4) cmd=SC_CLOSE;
    else if(msg->wParam==VK_F6) cmd=KeyDown(VK_SHIFT)?SC_PREVWINDOW:SC_NEXTWINDOW;
    if(!cmd) return FALSE;
    PostMessage(m->active,WM_SYSCOMMAND,cmd,0);
    return TRUE;
}
