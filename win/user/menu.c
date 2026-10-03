/* SPDX-License-Identifier: GPL-2.0-or-later
 * Menus: menu bars, popup menus and system menus, their resources, layout
 * and drawing, and the modal tracking loop for the mouse and the keyboard.
 * Popups are top-level windows of the "#32768" class that never take the
 * focus; while a menu is tracked its owner has the capture.
 */
#include "user.h"
#define ITEM_HEIGHT (CharHeight()+4)
#define SEPARATOR_HEIGHT 8
#define CHECK_WIDTH 16
#define BAR_PAD 8

/* A bitmap item shows its bitmap (the program's); an owner-drawn one is
 * measured and drawn by the window the menu belongs to (wnd). */
typedef struct {UINT flags; UINT_PTR id; HMENU popup; char *text; HBITMAP bitmap; ULONG_PTR data; RECT rect;} MenuItem;
typedef struct {BOOL used; int count,capacity; MenuItem *items; int width,height; Queue *owner; HWND wnd;} Menu;
static Menu menus[MENUS];

static Menu *menu_of(HMENU h) {
    ULONG_PTR v=(ULONG_PTR)h;
    if(v<HMENU_BASE || (v-HMENU_BASE)%4 || (v-HMENU_BASE)/4>=MENUS) return NULL;
    return menus[(v-HMENU_BASE)/4].used?&menus[(v-HMENU_BASE)/4]:NULL;
}
static HMENU new_menu(void) {
    int i;
    for(i=0;i<MENUS;i++) if(!menus[i].used) {memset(&menus[i],0,sizeof(menus[i])); menus[i].used=TRUE; menus[i].owner=CurrentQueue(); return (HMENU)(ULONG_PTR)(HMENU_BASE+i*4);}
    return NULL;
}
HMENU WINAPI CreateMenu(void) {return new_menu();}
HMENU WINAPI CreatePopupMenu(void) {return new_menu();}
BOOL WINAPI IsMenu(HMENU h) {return menu_of(h)!=NULL;}
static void free_item(MenuItem *it) {
    if(it->text) GlobalFree(it->text);
    it->text=NULL;
}
BOOL WINAPI DestroyMenu(HMENU h) {
    Menu *m=menu_of(h); int i;
    if(!m) return FALSE;
    for(i=0;i<m->count;i++) {if(m->items[i].flags&MF_POPUP) DestroyMenu(m->items[i].popup); free_item(&m->items[i]);}
    if(m->items) GlobalFree(m->items);
    m->used=FALSE;
    return TRUE;
}
/* The item by command (searching popups too) or by position. */
static MenuItem *find_item(HMENU h,UINT which,UINT flags,Menu **owner) {
    Menu *m=menu_of(h); int i;
    if(!m) return NULL;
    if(flags&MF_BYPOSITION) {if(which<(UINT)m->count) {if(owner) *owner=m; return &m->items[which];} return NULL;}
    for(i=0;i<m->count;i++) {
        MenuItem *it=&m->items[i];
        if(!(it->flags&MF_POPUP) && it->id==which) {if(owner) *owner=m; return it;}
        if(it->flags&MF_POPUP) {MenuItem *sub=find_item(it->popup,which,flags,owner); if(sub) return sub;}
    }
    return NULL;
}
static BOOL set_item(MenuItem *it,UINT flags,UINT_PTR id,LPCSTR text) {
    free_item(it);
    /* ChangeMenu's MF_APPEND and the like are gone by now: they share
     * values with MF_OWNERDRAW, MF_HILITE and MF_USECHECKBITMAPS. */
    it->flags=flags&~MF_BYPOSITION;
    if(flags&MF_POPUP) {it->popup=(HMENU)id; it->id=(UINT_PTR)-1;} else {it->popup=NULL; it->id=id;}
    it->bitmap=flags&MF_BITMAP?(HBITMAP)text:NULL; it->data=flags&MF_OWNERDRAW?(ULONG_PTR)text:0;
    if(!(flags&(MF_SEPARATOR|MF_BITMAP|MF_OWNERDRAW)) && text) {
        int n=lstrlen(text);
        if(!(it->text=(char *)ualloc((DWORD)n+1))) return FALSE;
        memcpy(it->text,text,(size_t)n+1);
    }
    if(!(flags&(MF_SEPARATOR|MF_POPUP|MF_BITMAP|MF_OWNERDRAW)) && !text) it->flags|=MF_SEPARATOR;
    return TRUE;
}
static BOOL insert_at(Menu *m,int pos,UINT flags,UINT_PTR id,LPCSTR text) {
    if(m->count==m->capacity) {
        int n=m->capacity?m->capacity*2:8; MenuItem *items=(MenuItem *)ualloc((DWORD)(n*sizeof(MenuItem)));
        if(!items) return FALSE;
        if(m->count) memcpy(items,m->items,sizeof(MenuItem)*(unsigned)m->count);
        if(m->items) GlobalFree(m->items);
        m->items=items; m->capacity=n;
    }
    if(pos<0 || pos>m->count) pos=m->count;
    memmove(&m->items[pos+1],&m->items[pos],sizeof(MenuItem)*(unsigned)(m->count-pos));
    memset(&m->items[pos],0,sizeof(MenuItem)); m->count++;
    return set_item(&m->items[pos],flags,id,text);
}
BOOL WINAPI AppendMenu(HMENU h,UINT flags,UINT_PTR id,LPCSTR text) {Menu *m=menu_of(h); return m && insert_at(m,-1,flags,id,text);}
BOOL WINAPI InsertMenu(HMENU h,UINT pos,UINT flags,UINT_PTR id,LPCSTR text) {
    Menu *m=menu_of(h),*owner=m; MenuItem *it;
    if(!m) return FALSE;
    if(pos==(UINT)-1) return insert_at(m,-1,flags,id,text);
    if(flags&MF_BYPOSITION) return insert_at(m,(int)pos,flags,id,text);
    if(!(it=find_item(h,pos,flags,&owner))) return insert_at(m,-1,flags,id,text);
    return insert_at(owner,(int)(it-owner->items),flags,id,text);
}
BOOL WINAPI ModifyMenu(HMENU h,UINT pos,UINT flags,UINT_PTR id,LPCSTR text) {
    Menu *owner; MenuItem *it=find_item(h,pos,flags,&owner);
    if(!it) return FALSE;
    return set_item(it,flags,id,text);
}
static BOOL remove_item(HMENU h,UINT pos,UINT flags,BOOL destroy) {
    Menu *owner; MenuItem *it=find_item(h,pos,flags,&owner); int i;
    if(!it) return FALSE;
    if(destroy && (it->flags&MF_POPUP)) DestroyMenu(it->popup);
    free_item(it);
    i=(int)(it-owner->items);
    memmove(&owner->items[i],&owner->items[i+1],sizeof(MenuItem)*(unsigned)(owner->count-i-1));
    owner->count--;
    return TRUE;
}
BOOL WINAPI DeleteMenu(HMENU h,UINT pos,UINT flags) {return remove_item(h,pos,flags,TRUE);}
BOOL WINAPI RemoveMenu(HMENU h,UINT pos,UINT flags) {return remove_item(h,pos,flags,FALSE);}
/* The Windows 2 interface: one call for append, insert, change and delete. */
BOOL WINAPI ChangeMenu(HMENU h,UINT pos,LPCSTR text,UINT id,UINT flags) {
    if(flags&MF_APPEND) return AppendMenu(h,flags&~MF_APPEND,id,text);
    if(flags&MF_DELETE) return DeleteMenu(h,pos,flags&~MF_DELETE);
    if(flags&MF_REMOVE) return RemoveMenu(h,pos,flags&~MF_REMOVE);
    if(flags&MF_CHANGE) return ModifyMenu(h,pos,flags&~MF_CHANGE,id,text);
    return InsertMenu(h,pos,flags,id,text);
}
DWORD WINAPI CheckMenuItem(HMENU h,UINT which,UINT flags) {
    MenuItem *it=find_item(h,which,flags,NULL); DWORD old;
    if(!it) return (DWORD)-1;
    old=it->flags&MF_CHECKED;
    it->flags=(it->flags&~MF_CHECKED)|(flags&MF_CHECKED);
    return old;
}
BOOL WINAPI EnableMenuItem(HMENU h,UINT which,UINT flags) {
    MenuItem *it=find_item(h,which,flags,NULL); UINT old;
    if(!it) return (BOOL)-1;
    old=it->flags&(MF_GRAYED|MF_DISABLED);
    it->flags=(it->flags&~(MF_GRAYED|MF_DISABLED))|(flags&(MF_GRAYED|MF_DISABLED));
    return (BOOL)old;
}
UINT WINAPI GetMenuState(HMENU h,UINT which,UINT flags) {
    MenuItem *it=find_item(h,which,flags,NULL);
    if(!it) return (UINT)-1;
    if(it->flags&MF_POPUP) {Menu *sub=menu_of(it->popup); return (it->flags&0xff)|((UINT)(sub?sub->count:0)<<8);}
    return it->flags&0xffff;
}
int WINAPI GetMenuString(HMENU h,UINT which,LPSTR out,int size,UINT flags) {
    MenuItem *it=find_item(h,which,flags,NULL);
    if(!it || !out || size<=0) return 0;
    lstrcpyn(out,it->text?it->text:"",size);
    return lstrlen(out);
}
int WINAPI GetMenuItemCount(HMENU h) {Menu *m=menu_of(h); return m?m->count:-1;}
UINT WINAPI GetMenuItemID(HMENU h,int pos) {
    Menu *m=menu_of(h);
    if(!m || pos<0 || pos>=m->count) return (UINT)-1;
    return m->items[pos].flags&MF_POPUP?(UINT)-1:(UINT)m->items[pos].id;
}
HMENU WINAPI GetSubMenu(HMENU h,int pos) {
    Menu *m=menu_of(h);
    if(!m || pos<0 || pos>=m->count || !(m->items[pos].flags&MF_POPUP)) return NULL;
    return m->items[pos].popup;
}
DWORD WINAPI GetMenuCheckMarkDimensions(void) {return MAKELONG(CHECK_WIDTH,CharHeight());}
HMENU WINAPI GetMenu(HWND h) {Wnd *w=WndFromHandle(h); return w && !(w->style&WS_CHILD)?w->menu:NULL;}
BOOL WINAPI SetMenu(HWND h,HMENU menu) {
    Wnd *w=WndFromHandle(h); RECT r;
    if(!w || (w->style&WS_CHILD)) return FALSE;
    w->menu=menu;
    r=w->window; OffsetRect(&r,-w->parent->client.left,-w->parent->client.top);
    PlaceWindow(w,&r,TRUE);
    return TRUE;
}
void FreeWindowMenus(Wnd *w) {
    if(!(w->style&WS_CHILD) && w->menu) DestroyMenu(w->menu);
    if(w->system_menu) DestroyMenu(w->system_menu);
    w->menu=NULL; w->system_menu=NULL;
}

/* --- resources ------------------------------------------------------------------- */
static const WORD *load_items(HMENU h,const WORD *p,const WORD *end) {
    for(;p<end;) {
        WORD flags=*p++,id=0; char text[256]; HMENU popup=NULL;
        if(!(flags&MF_POPUP)) id=*p++;
        WideToAnsi(p,text,sizeof(text)); p=SkipWide(p);
        if(flags&MF_POPUP) {popup=CreatePopupMenu(); p=load_items(popup,p,end); AppendMenu(h,(flags&~MF_END)|MF_POPUP,(UINT_PTR)popup,text);}
        else if(!id && !text[0] && !(flags&~MF_END)) AppendMenu(h,MF_SEPARATOR,0,NULL);
        else AppendMenu(h,flags&~MF_END,id,text);
        if(flags&MF_END) break;
    }
    return p;
}
HMENU WINAPI LoadMenuIndirect(const void FAR *data) {
    const WORD *p=(const WORD *)data; HMENU h;
    if(!p || p[0]!=0) return NULL;
    h=CreateMenu();
    if(h) load_items(h,p+2+p[1]/2,p+0x100000);
    return h;
}
HMENU WINAPI LoadMenu(HINSTANCE instance,LPCSTR name) {
    DWORD size; const void *data=Resource(instance,RT_MENU,name,&size);
    return data?LoadMenuIndirect(data):NULL;
}
static HMENU DefaultSystemMenu(Wnd *w) {
    HMENU m=CreatePopupMenu();
    if(!m) return NULL;
    AppendMenu(m,MF_STRING,SC_RESTORE,"&Restore");
    AppendMenu(m,MF_STRING,SC_MOVE,"&Move");
    AppendMenu(m,MF_STRING,SC_SIZE,"&Size");
    AppendMenu(m,MF_STRING,SC_MINIMIZE,"Mi&nimize");
    AppendMenu(m,MF_STRING,SC_MAXIMIZE,"Ma&ximize");
    AppendMenu(m,MF_SEPARATOR,0,NULL);
    AppendMenu(m,MF_STRING,SC_CLOSE,w->style&WS_CHILD?"&Close\tCtrl+F4":"&Close\tAlt+F4");
    if(!(w->style&WS_CHILD)) {
        AppendMenu(m,MF_SEPARATOR,0,NULL);
        AppendMenu(m,MF_STRING,SC_TASKLIST,"S&witch To...\tCtrl+Esc");
    } else {
        AppendMenu(m,MF_SEPARATOR,0,NULL);
        AppendMenu(m,MF_STRING,SC_NEXTWINDOW,"Nex&t\tCtrl+F6");
    }
    return m;
}
HMENU WINAPI GetSystemMenu(HWND h,BOOL revert) {
    Wnd *w=WndFromHandle(h);
    if(!w) return NULL;
    if(revert) {if(w->system_menu) DestroyMenu(w->system_menu); w->system_menu=NULL; return NULL;}
    if(!w->system_menu) w->system_menu=DefaultSystemMenu(w);
    return w->system_menu;
}
/* The system menu's items follow the window's state. */
static void update_system_menu(Wnd *w,HMENU m) {
    BOOL min=(w->style&WS_MINIMIZE)!=0,max=(w->style&WS_MAXIMIZE)!=0;
    EnableMenuItem(m,SC_RESTORE,min || max?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,SC_MOVE,max?MF_GRAYED:MF_ENABLED);
    EnableMenuItem(m,SC_SIZE,!(w->style&WS_THICKFRAME) || min || max?MF_GRAYED:MF_ENABLED);
    EnableMenuItem(m,SC_MINIMIZE,!(w->style&WS_MINIMIZEBOX) || min?MF_GRAYED:MF_ENABLED);
    EnableMenuItem(m,SC_MAXIMIZE,!(w->style&WS_MAXIMIZEBOX) || max?MF_GRAYED:MF_ENABLED);
    EnableMenuItem(m,SC_CLOSE,w->cls->wc.style&CS_NOCLOSE?MF_GRAYED:MF_ENABLED);
}

/* --- layout and drawing ---------------------------------------------------------------- */
static HDC measure_dc(void) {HDC dc=CreateCompatibleDC(NULL); SelectObject(dc,SystemFont()); return dc;}
/* Text before a tab; the rest is the accelerator, right-aligned. */
static int split(const char *text,int *right) {
    int i;
    for(i=0;text[i];i++) if(text[i]=='\t') {*right=i+1; return i;}
    *right=-1; return i;
}
/* A bitmap's or an owner-drawn item's size (WM_MEASUREITEM, from the item
 * height); FALSE for a text item or a separator. */
static BOOL item_size(Menu *m,MenuItem *it,int *width,int *height) {
    if(it->flags&MF_OWNERDRAW) {
        MEASUREITEMSTRUCT mi;
        mi.CtlType=ODT_MENU; mi.CtlID=0; mi.itemID=(UINT)it->id; mi.itemWidth=0; mi.itemHeight=(UINT)ITEM_HEIGHT; mi.itemData=it->data;
        if(m->wnd && IsWindow(m->wnd)) SendMessage(m->wnd,WM_MEASUREITEM,0,(LPARAM)&mi);
        *width=(int)mi.itemWidth; *height=(int)mi.itemHeight;
        return TRUE;
    }
    if(it->flags&MF_BITMAP) {
        BITMAP b;
        if(!it->bitmap || !GetObject(it->bitmap,sizeof(b),&b)) b.bmWidth=b.bmHeight=0;
        *width=b.bmWidth; *height=b.bmHeight;
        return TRUE;
    }
    return FALSE;
}
/* Items in columns: a new one at an item with MF_MENUBREAK or MF_MENUBARBREAK
 * (a line before it), or where the next item would pass the screen's bottom. */
static void layout_popup(Menu *m) {
    HDC dc=measure_dc(); int i,start=0,x=1,y=1,w=0,accel=0,bottom=1;
    for(i=0;i<=m->count;i++) {
        MenuItem *it=i<m->count?&m->items[i]:NULL; int h=0,iw=0,ia=0;
        if(it && !item_size(m,it,&iw,&h)) {
            h=it->flags&MF_SEPARATOR?SEPARATOR_HEIGHT:ITEM_HEIGHT;
            if(it->text) {
                int r,n=split(it->text,&r);
                iw=PrefixTextWidth(dc,it->text,n);
                if(r>=0) {SIZE sz; GetTextExtentPoint(dc,it->text+r,lstrlen(it->text+r),&sz); ia=(int)sz.cx;}
            }
        }
        if(!it || (i>start && ((it->flags&(MF_MENUBREAK|MF_MENUBARBREAK)) || y+h+1>screen_height))) {
            /* The column ends: its items take its width. */
            int k,width=CHECK_WIDTH+w+(accel?accel+3*CharWidth():CharWidth())+CHECK_WIDTH/2;
            for(k=start;k<i;k++) {m->items[k].rect.left=x; m->items[k].rect.right=x+width;}
            x+=width; bottom=max(bottom,y);
            if(!it) break;
            if(it->flags&MF_MENUBARBREAK) x+=2;
            start=i; y=1; w=0; accel=0;
        }
        w=max(w,iw); accel=max(accel,ia);
        SetRect(&it->rect,0,y,0,y+h); y+=h;
    }
    m->width=x+1; m->height=bottom+1;
    DeleteDC(dc);
}
static void layout_bar(Wnd *w,Menu *m) {
    HDC dc=measure_dc(); int i,x=0,y=0,width=w->window.right-w->window.left-2*FrameWidth(w),h=CharHeight()+5;
    if(width<=0) width=1;
    m->wnd=w->handle;
    for(i=0;i<m->count;i++) {
        MenuItem *it=&m->items[i]; int iw=it->text?PrefixTextWidth(dc,it->text,lstrlen(it->text))+2*BAR_PAD:2*BAR_PAD,ih;
        if(item_size(m,it,&iw,&ih)) iw+=2*BAR_PAD;
        if(x>0 && (x+iw>width || (it->flags&(MF_MENUBREAK|MF_MENUBARBREAK)))) {x=0; y+=h;}
        SetRect(&it->rect,x,y,x+iw,y+h); x+=iw;
    }
    /* A help item (text starting with \a) goes to the right. */
    for(i=0;i<m->count;i++) if(m->items[i].text && m->items[i].text[0]=='\a') {
        int dx=width-m->items[i].rect.right; if(dx>0) {int k; for(k=i;k<m->count;k++) OffsetRect(&m->items[k].rect,dx,0);} break;
    }
    m->width=width; m->height=y+h+1;
    DeleteDC(dc);
}
int MenuBarHeight(Wnd *w) {
    Menu *m=menu_of(w->menu);
    if(!m) return 0;
    layout_bar(w,m);
    return max(MENUBAR,m->height);
}
void MenuBarRect(Wnd *w,RECT *r) {
    int b=FrameWidth(w);
    SetRect(r,w->window.left+b,w->client.top-MenuBarHeight(w),w->window.right-b,w->client.top);
}
static void draw_check(HDC dc,int x,int cy,COLORREF c) {
    HBRUSH b=CreateSolidBrush(c); int i; RECT r;
    for(i=0;i<3;i++) {SetRect(&r,x+i,cy+i,x+i+2,cy+i+1); FillRect(dc,&r,b);}
    for(i=0;i<6;i++) {SetRect(&r,x+3+i,cy+1-i,x+5+i,cy+2-i); FillRect(dc,&r,b);}
    DeleteObject(b);
}
static void draw_item(HDC dc,HMENU menu,MenuItem *it,BOOL bar,BOOL selected,int offx,int offy) {
    RECT r=it->rect; BOOL gray=(it->flags&(MF_GRAYED|MF_DISABLED))!=0; COLORREF fg,bg; Menu *m=menu_of(menu);
    OffsetRect(&r,offx,offy);
    if(!bar && (it->flags&MF_MENUBARBREAK) && it!=m->items) {RECT l; SetRect(&l,r.left-2,1,r.left-1,m->height-1); FillRect(dc,&l,SysBrush(COLOR_MENUTEXT));}
    if(it->flags&MF_OWNERDRAW) {
        DRAWITEMSTRUCT d;
        d.CtlType=ODT_MENU; d.CtlID=0; d.itemID=(UINT)it->id; d.itemAction=ODA_DRAWENTIRE;
        d.itemState=(selected?ODS_SELECTED:0)|(it->flags&MF_CHECKED?ODS_CHECKED:0)|(it->flags&MF_GRAYED?ODS_GRAYED:0)|(it->flags&MF_DISABLED?ODS_DISABLED:0);
        d.hwndItem=(HWND)menu; d.hDC=dc; d.rcItem=r; d.itemData=it->data;
        FillRect(dc,&r,SysBrush(COLOR_MENU));
        if(m->wnd && IsWindow(m->wnd)) SendMessage(m->wnd,WM_DRAWITEM,0,(LPARAM)&d);
        return;
    }
    if(it->flags&MF_SEPARATOR) {
        RECT l; FillRect(dc,&r,SysBrush(COLOR_MENU));
        SetRect(&l,r.left,(r.top+r.bottom)/2,r.right,(r.top+r.bottom)/2+1); FillRect(dc,&l,SysBrush(COLOR_MENUTEXT));
        return;
    }
    bg=GetSysColor(selected?COLOR_HIGHLIGHT:COLOR_MENU);
    fg=GetSysColor(gray?COLOR_GRAYTEXT:selected?COLOR_HIGHLIGHTTEXT:COLOR_MENUTEXT);
    if(it->flags&MF_BITMAP) {
        /* The bitmap, inverted when selected. */
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old; BITMAP b; int x=bar?r.left+BAR_PAD:r.left+CHECK_WIDTH;
        FillRect(dc,&r,SysBrush(COLOR_MENU));
        if(it->bitmap && GetObject(it->bitmap,sizeof(b),&b)) {old=SelectObject(mem,it->bitmap); BitBlt(dc,x,r.top,b.bmWidth,b.bmHeight,mem,0,0,SRCCOPY); SelectObject(mem,old);}
        DeleteDC(mem);
        if(!bar && (it->flags&MF_CHECKED)) draw_check(dc,r.left+3,(r.top+r.bottom)/2-1,GetSysColor(COLOR_MENUTEXT));
        if(selected) InvertRect(dc,&r);
        return;
    }
    {HBRUSH b=CreateSolidBrush(bg); FillRect(dc,&r,b); DeleteObject(b);}
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,fg);
    if(!it->text) return;
    if(bar) DrawPrefixText(dc,r.left+BAR_PAD,r.top+2,it->text[0]=='\a'?it->text+1:it->text,-1,FALSE);
    else {
        int right,n=split(it->text,&right),cy=(r.top+r.bottom)/2;
        if(it->flags&MF_CHECKED) draw_check(dc,r.left+3,cy-1,fg);
        DrawPrefixText(dc,r.left+CHECK_WIDTH,r.top+2,it->text,n,FALSE);
        if(right>=0) {
            SIZE s; GetTextExtentPoint(dc,it->text+right,lstrlen(it->text+right),&s);
            TextOut(dc,r.right-CHECK_WIDTH/2-s.cx-(it->flags&MF_POPUP?CHECK_WIDTH/2:0),r.top+2,it->text+right,lstrlen(it->text+right));
        }
        if(it->flags&MF_POPUP) {
            int i,x=r.right-8; HBRUSH b=CreateSolidBrush(fg); RECT t;
            for(i=0;i<4;i++) {SetRect(&t,x+i,cy-3+i,x+i+1,cy+4-i); FillRect(dc,&t,b);}
            DeleteObject(b);
        }
    }
}
void PaintMenuBar(Wnd *w,HDC dc) {
    Menu *m=menu_of(w->menu); RECT r; int i,offx,offy; HGDIOBJ old;
    if(!m) return;
    MenuBarRect(w,&r); OffsetRect(&r,-w->window.left,-w->window.top);
    offx=r.left; offy=r.top;
    old=SelectObject(dc,SystemFont());
    {RECT b=r; b.bottom--; FillRect(dc,&b,SysBrush(COLOR_MENU)); b.top=b.bottom; b.bottom++; FillRect(dc,&b,SysBrush(COLOR_WINDOWFRAME));}
    for(i=0;i<m->count;i++) draw_item(dc,w->menu,&m->items[i],TRUE,(m->items[i].flags&MF_HILITE)!=0,offx,offy);
    SelectObject(dc,old);
}
static void paint_popup(HWND h,HMENU menu,int sel) {
    Menu *m=menu_of(menu); HDC dc; RECT r; int i; HGDIOBJ old;
    if(!m) return;
    dc=GetDC(h); old=SelectObject(dc,SystemFont());
    SetRect(&r,0,0,m->width,m->height);
    FrameRect(dc,&r,SysBrush(COLOR_WINDOWFRAME));
    /* Between columns and below a shorter one. */
    InflateRect(&r,-1,-1); FillRect(dc,&r,SysBrush(COLOR_MENU));
    for(i=0;i<m->count;i++) draw_item(dc,menu,&m->items[i],FALSE,i==sel,0,0);
    SelectObject(dc,old); ReleaseDC(h,dc);
}
static LRESULT CALLBACK MenuWindowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h,&ps); EndPaint(h,&ps);
        paint_popup(h,(HMENU)GetWindowLongPtr(h,0),(int)GetWindowLongPtr(h,8));
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* --- tracking ------------------------------------------------------------------------------ */
#define LEVELS 8
typedef struct {
    Wnd *owner; HMENU bar; BOOL system; int depth;
    struct {HMENU menu; HWND wnd; int sel; RECT rect;} level[LEVELS]; /* level 0 is the bar when there is one */
    BOOL done; UINT_PTR command; BOOL has_command;
    DWORD opened; BOOL restore; /* an icon's menu: when it opened; a second click on the icon */
} Tracker;
static void bar_select(Tracker *t,int sel) {
    Menu *m=menu_of(t->bar); int i; HDC dc;
    if(!m) return;
    for(i=0;i<m->count;i++) {if(i==sel) m->items[i].flags|=MF_HILITE; else m->items[i].flags&=~MF_HILITE;}
    t->level[0].sel=sel;
    dc=GetWindowDC(t->owner->handle); PaintMenuBar(t->owner,dc); ReleaseDC(t->owner->handle,dc);
}
static void notify_select(Tracker *t,HMENU menu,int sel) {
    Menu *m=menu_of(menu); UINT flags,item;
    if(!m || sel<0 || sel>=m->count) {SendMessage(t->owner->handle,WM_MENUSELECT,MAKEWPARAM(0,0xffff),0); return;}
    flags=m->items[sel].flags&(MF_POPUP|MF_GRAYED|MF_DISABLED|MF_CHECKED|MF_SEPARATOR);
    if(t->system) flags|=MF_SYSMENU;
    item=m->items[sel].flags&MF_POPUP?(UINT)sel:(UINT)m->items[sel].id;
    SendMessage(t->owner->handle,WM_MENUSELECT,MAKEWPARAM(item,flags),(LPARAM)menu);
}
static void popup_select(Tracker *t,int level,int sel) {
    if(t->level[level].sel==sel) return;
    t->level[level].sel=sel;
    if(t->level[level].wnd) {SetWindowLongPtr(t->level[level].wnd,8,sel); paint_popup(t->level[level].wnd,t->level[level].menu,sel);}
    notify_select(t,t->level[level].menu,sel);
}
static void close_from(Tracker *t,int level) {
    while(t->depth>level) {
        t->depth--;
        if(t->level[t->depth].wnd) DestroyWindow(t->level[t->depth].wnd);
        t->level[t->depth].wnd=NULL;
    }
}
/* Open a popup at screen (x,y) as the next level. */
static void open_popup(Tracker *t,HMENU menu,int x,int y,int index) {
    Menu *m=menu_of(menu); HWND wnd; int d=t->depth;
    if(!m || d>=LEVELS) return;
    SendMessage(t->owner->handle,WM_INITMENUPOPUP,(WPARAM)menu,MAKELPARAM(index,t->system));
    m->wnd=t->owner->handle; layout_popup(m);
    if(x+m->width>screen_width) x=max(0,screen_width-m->width);
    if(y+m->height>screen_height) y=max(0,screen_height-m->height);
    wnd=CreateWindowEx(WS_EX_TOPMOST,"#32768",NULL,WS_POPUP,x,y,m->width,m->height,t->owner->handle,NULL,user_instance,NULL);
    if(!wnd) return;
    SetWindowLongPtr(wnd,0,(LONG_PTR)menu); SetWindowLongPtr(wnd,8,-1);
    t->level[d].menu=menu; t->level[d].wnd=wnd; t->level[d].sel=-1; SetRect(&t->level[d].rect,x,y,x+m->width,y+m->height);
    t->depth=d+1;
    ShowWindow(wnd,SW_SHOWNA);
    UpdateWindow(wnd);
}
static int first_enabled(Menu *m,int from,int dir) {
    int i,n=m->count;
    if(!n) return -1;
    for(i=0;i<n;i++) {
        int k=((from+dir*i)%n+n)%n;
        if(!(m->items[k].flags&MF_SEPARATOR)) return k;
    }
    return -1;
}
/* Open the bar item's popup below it. */
static void open_bar_item(Tracker *t,int sel) {
    Menu *m=menu_of(t->bar); RECT bar,r;
    close_from(t,1);
    bar_select(t,sel);
    notify_select(t,t->bar,sel);
    if(!m || sel<0 || !(m->items[sel].flags&MF_POPUP) || (m->items[sel].flags&(MF_GRAYED|MF_DISABLED))) return;
    MenuBarRect(t->owner,&bar);
    r=m->items[sel].rect; OffsetRect(&r,bar.left,bar.top);
    open_popup(t,m->items[sel].popup,r.left,r.bottom,sel);
}
/* Where a screen point falls: level and item, or -1. */
static int locate(Tracker *t,POINT p,int *item) {
    int l;
    for(l=t->depth-1;l>=0;l--) {
        Menu *m=menu_of(t->level[l].menu); int i;
        if(l==0 && t->bar) {
            RECT bar; MenuBarRect(t->owner,&bar);
            if(!PtInRect(&bar,p) || !m) continue;
            for(i=0;i<m->count;i++) {RECT r=m->items[i].rect; OffsetRect(&r,bar.left,bar.top); if(PtInRect(&r,p)) {*item=i; return 0;}}
            *item=-1; return 0;
        }
        if(!m || !PtInRect(&t->level[l].rect,p)) continue;
        for(i=0;i<m->count;i++) {
            RECT r=m->items[i].rect; OffsetRect(&r,t->level[l].rect.left,t->level[l].rect.top);
            if(PtInRect(&r,p)) {*item=m->items[i].flags&MF_SEPARATOR?-1:i; return l;}
        }
        *item=-1; return l;
    }
    return -1;
}
static void choose(Tracker *t,int level,int sel) {
    Menu *m=menu_of(t->level[level].menu); MenuItem *it;
    if(!m || sel<0 || sel>=m->count) return;
    it=&m->items[sel];
    if(it->flags&(MF_GRAYED|MF_DISABLED|MF_SEPARATOR)) return;
    if(it->flags&MF_POPUP) {
        if(level==0 && t->bar) {open_bar_item(t,sel); return;}
        close_from(t,level+1);
        {RECT r=it->rect; OffsetRect(&r,t->level[level].rect.left,t->level[level].rect.top);
         open_popup(t,it->popup,r.right-2,r.top-1,sel);}
        {Menu *sub=menu_of(t->level[t->depth-1].menu); if(sub && t->depth>level+1) popup_select(t,t->depth-1,first_enabled(sub,0,1));}
        return;
    }
    t->command=it->id; t->has_command=TRUE; t->done=TRUE;
}
static int mnemonic(Menu *m,WPARAM ch) {
    int i;
    for(i=0;m && i<m->count;i++) if(m->items[i].text) {
        char c=PrefixChar(m->items[i].text);
        if(c && (BYTE)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)c)==(BYTE)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)ch)) return i;
    }
    return -1;
}
/* The first item of the column after (dir 1) or before (-1) the one item i is
 * in, or -1. */
static int column_step(Menu *m,int i,int dir) {
    int k=i;
    if(dir>0) {while(k<m->count && m->items[k].rect.left==m->items[i].rect.left) k++;}
    else {
        while(k>0 && m->items[k-1].rect.left==m->items[i].rect.left) k--;
        if(--k<0) return -1;
        while(k>0 && m->items[k-1].rect.left==m->items[k].rect.left) k--;
    }
    for(i=k;i<m->count && m->items[i].rect.left==m->items[k].rect.left;i++) if(!(m->items[i].flags&MF_SEPARATOR)) return i;
    return -1;
}
static void key(Tracker *t,WPARAM vk) {
    int top=t->depth-1; Menu *m=menu_of(t->level[top].menu);
    BOOL on_bar=t->bar && top==0;
    switch(vk) {
    case VK_ESCAPE: case VK_MENU: case VK_F10:
        if(t->depth>1 && vk==VK_ESCAPE) {close_from(t,t->depth-1); if(t->depth==1 && t->bar) notify_select(t,t->bar,t->level[0].sel);}
        else t->done=TRUE;
        return;
    case VK_LEFT: case VK_RIGHT:
        /* Across a popup's columns first. */
        if(!on_bar && m && t->level[top].sel>=0 && !(vk==VK_RIGHT && (m->items[t->level[top].sel].flags&MF_POPUP))) {
            int c=column_step(m,t->level[top].sel,vk==VK_RIGHT?1:-1);
            if(c>=0) {popup_select(t,top,c); return;}
        }
        if(on_bar || (t->bar && top==1 && (vk==VK_LEFT || !m || t->level[top].sel<0 || !(m->items[t->level[top].sel].flags&MF_POPUP)))) {
            Menu *b=menu_of(t->bar); int sel=first_enabled(b,t->level[0].sel+(vk==VK_LEFT?-1:1),vk==VK_LEFT?-1:1);
            BOOL was_open=t->depth>1;
            if(was_open) {open_bar_item(t,sel); if(t->depth>1) {Menu *sub=menu_of(t->level[1].menu); popup_select(t,1,first_enabled(sub,0,1));}}
            else {bar_select(t,sel); notify_select(t,t->bar,sel);}
            return;
        }
        if(vk==VK_RIGHT && m && t->level[top].sel>=0 && (m->items[t->level[top].sel].flags&MF_POPUP)) {choose(t,top,t->level[top].sel); return;}
        if(vk==VK_LEFT && top>0 && (!t->bar || top>1)) {close_from(t,top); return;}
        return;
    case VK_UP: case VK_DOWN:
        if(on_bar) {
            choose(t,0,t->level[0].sel);
            if(t->depth>1) {Menu *sub=menu_of(t->level[1].menu); popup_select(t,1,first_enabled(sub,0,1));}
            return;
        }
        if(m) popup_select(t,top,first_enabled(m,t->level[top].sel<0?(vk==VK_UP?m->count-1:0):t->level[top].sel+(vk==VK_UP?-1:1),vk==VK_UP?-1:1));
        return;
    case VK_RETURN:
        choose(t,top,t->level[top].sel);
        if(t->depth>top+1) {Menu *sub=menu_of(t->level[t->depth-1].menu); popup_select(t,t->depth-1,first_enabled(sub,0,1));}
        return;
    }
}
static void character(Tracker *t,WPARAM ch) {
    int top=t->depth-1; Menu *m=menu_of(t->level[top].menu); int i=mnemonic(m,ch);
    if(i<0) {
        LRESULT r=SendMessage(t->owner->handle,WM_MENUCHAR,MAKEWPARAM(ch,t->system?MF_SYSMENU:(top?MF_POPUP:0)),(LPARAM)t->level[top].menu);
        if(HIWORD(r)==2) i=LOWORD(r);
        else if(HIWORD(r)==1) {if(t->bar && top==0) bar_select(t,LOWORD(r)); else popup_select(t,top,LOWORD(r)); return;}
        else {MessageBeep(0); return;}
    }
    if(t->bar && top==0) {bar_select(t,i); choose(t,0,i); if(t->depth>1) {Menu *sub=menu_of(t->level[1].menu); popup_select(t,1,first_enabled(sub,0,1));} return;}
    popup_select(t,top,i); choose(t,top,i);
}
/* The modal loop: input goes to the menus until a command is chosen or the
 * menu is dismissed. */
static void run(Tracker *t,BOOL button_down) {
    MSG msg; HWND h=t->owner->handle; BOOL pressed=button_down,moved=FALSE;
    SetCapture(h);
    SendMessage(h,WM_ENTERMENULOOP,0,0);
    while(!t->done) {
        if(!GetMessage(&msg,NULL,0,0)) {PostQuitMessage((int)msg.wParam); break;}
        if(!IsWindow(h)) return;
        if(CallMsgFilter(&msg,MSGF_MENU)) continue;
        if(msg.message==WM_MOUSEMOVE || msg.message==WM_LBUTTONDOWN || msg.message==WM_LBUTTONUP ||
           msg.message==WM_NCMOUSEMOVE || msg.message==WM_NCLBUTTONDOWN || msg.message==WM_NCLBUTTONUP ||
           msg.message==WM_RBUTTONDOWN || msg.message==WM_RBUTTONUP || msg.message==WM_LBUTTONDBLCLK) {
            POINT p; int item=-1,level; GetCursorPos(&p);
            level=locate(t,p,&item);
            if(msg.message==WM_MOUSEMOVE || msg.message==WM_NCMOUSEMOVE) {
                moved=TRUE;
                if(level<0) {if(t->depth>0 && t->level[t->depth-1].wnd) popup_select(t,t->depth-1,-1); continue;}
                if(level==0 && t->bar) {
                    if(item>=0 && item!=t->level[0].sel && (pressed || t->depth>1)) {open_bar_item(t,item); continue;}
                    if(item>=0 && item!=t->level[0].sel) {bar_select(t,item); notify_select(t,t->bar,item);}
                    continue;
                }
                if(level<t->depth-1 && item>=0 && t->level[level].sel!=item) close_from(t,level+1);
                popup_select(t,level,item);
                if(item>=0) {
                    Menu *m=menu_of(t->level[level].menu);
                    if(m && (m->items[item].flags&MF_POPUP) && t->depth==level+1) choose(t,level,item);
                }
                continue;
            }
            if(msg.message==WM_LBUTTONDOWN || msg.message==WM_NCLBUTTONDOWN || msg.message==WM_RBUTTONDOWN || msg.message==WM_LBUTTONDBLCLK) {
                pressed=TRUE;
                if(level<0) {
                    /* The icon clicked again soon after the click that opened its
                     * menu: a double click, which restores the window. */
                    if(t->system && (t->owner->style&WS_MINIMIZE) && msg.message!=WM_RBUTTONDOWN &&
                       PtInRect(&t->owner->window,p) && GetTickCount()-t->opened<=GetDoubleClickTime()) t->restore=TRUE;
                    t->done=TRUE; break;
                }
                if(level==0 && t->bar) {
                    if(item==t->level[0].sel && t->depth>1) {close_from(t,1); continue;}
                    if(item>=0) open_bar_item(t,item);
                    continue;
                }
                popup_select(t,level,item);
                continue;
            }
            /* Button up: choose what is under the cursor. */
            pressed=FALSE;
            if(level<0) {if(moved && !t->bar) t->done=TRUE; continue;}
            if(level==0 && t->bar) {
                Menu *m=menu_of(t->bar);
                if(item>=0 && m && !(m->items[item].flags&MF_POPUP)) choose(t,0,item);
                continue;
            }
            if(item>=0) {
                Menu *m=menu_of(t->level[level].menu);
                if(m && !(m->items[item].flags&MF_POPUP)) choose(t,level,item);
            }
            continue;
        }
        if(msg.message==WM_KEYDOWN || msg.message==WM_SYSKEYDOWN) {
            if(msg.wParam==VK_MENU || msg.wParam==VK_F10) {t->done=TRUE; continue;}
            key(t,msg.wParam);
            TranslateMessage(&msg);
            continue;
        }
        if(msg.message==WM_CHAR || msg.message==WM_SYSCHAR) {
            if(msg.wParam>' ' || msg.wParam==' ') character(t,msg.wParam);
            continue;
        }
        if(msg.message>=WM_KEYFIRST && msg.message<=WM_KEYLAST) continue;
        if(msg.message>=WM_MOUSEFIRST && msg.message<=WM_MOUSELAST) continue;
        DispatchMessage(&msg);
    }
    close_from(t,t->bar?1:0);
    if(t->bar) bar_select(t,-1);
    ReleaseCapture();
    SendMessage(h,WM_MENUSELECT,MAKEWPARAM(0,0xffff),0);
    SendMessage(h,WM_EXITMENULOOP,0,0);
    if(t->has_command && IsWindow(h)) PostMessage(h,t->system?WM_SYSCOMMAND:WM_COMMAND,t->system?t->command:MAKEWPARAM(t->command,0),0);
}
void TrackMenuBar(Wnd *w,int item,POINT p,BOOL keyboard) {
    Tracker t; Menu *m=menu_of(w->menu);
    if(!m) return;
    memset(&t,0,sizeof(t));
    t.owner=w; t.bar=w->menu; t.depth=1; t.level[0].menu=w->menu; t.level[0].sel=-1;
    MenuBarRect(w,&t.level[0].rect);
    SendMessage(w->handle,WM_INITMENU,(WPARAM)w->menu,0);
    if(!keyboard) {
        int l=locate(&t,p,&item);
        if(l!=0 || item<0) return;
        open_bar_item(&t,item);
        run(&t,TRUE);
        return;
    }
    if(item<0) item=0;
    item=first_enabled(m,item,1);
    if(item<0) return;
    bar_select(&t,item); notify_select(&t,t.bar,item);
    run(&t,FALSE);
}
/* Alt+letter: open the menu bar item with that mnemonic. */
BOOL MenuKey(Wnd *w,WPARAM ch) {
    Tracker t; Menu *m=menu_of(w->menu); int i;
    if(!m || (i=mnemonic(m,ch))<0) return FALSE;
    memset(&t,0,sizeof(t));
    t.owner=w; t.bar=w->menu; t.depth=1; t.level[0].menu=w->menu; t.level[0].sel=-1;
    SendMessage(w->handle,WM_INITMENU,(WPARAM)w->menu,0);
    open_bar_item(&t,i);
    if(t.depth>1) {Menu *sub=menu_of(t.level[1].menu); popup_select(&t,1,first_enabled(sub,0,1));}
    else {choose(&t,0,i); if(t.done) {t.done=FALSE; t.has_command=FALSE; PostMessage(w->handle,WM_COMMAND,MAKEWPARAM(m->items[i].id,0),0); bar_select(&t,-1); return TRUE;}}
    run(&t,FALSE);
    return TRUE;
}
void SystemMenuPopup(Wnd *w,BOOL keyboard) {
    Tracker t; HMENU m=GetSystemMenu(w->handle,FALSE); int x,y;
    if(!m) return;
    update_system_menu(w,m);
    memset(&t,0,sizeof(t));
    t.owner=w; t.system=TRUE;
    SendMessage(w->handle,WM_INITMENU,(WPARAM)m,0);
    if(w->style&WS_MINIMIZE) {x=w->window.left; y=w->window.top-1; {Menu *mm=menu_of(m); mm->wnd=w->handle; layout_popup(mm); y-=mm->height;} if(y<0) y=w->window.bottom;}
    else {int b=FrameWidth(w); x=w->window.left+b; y=w->window.top+b+CAPTION-1;}
    open_popup(&t,m,x,y,0);
    if(keyboard && t.depth) popup_select(&t,0,first_enabled(menu_of(m),0,1));
    /* From when it shows: input is polled, so a click made while the menu
     * was being drawn is read only now. */
    t.opened=GetTickCount();
    run(&t,!keyboard);
    if(t.restore && IsWindow(w->handle)) SendMessage(w->handle,WM_SYSCOMMAND,SC_RESTORE,0);
}
BOOL WINAPI TrackPopupMenu(HMENU menu,UINT flags,int x,int y,int reserved,HWND h,LPCRECT r) {
    Tracker t; Wnd *w=WndFromHandle(h); Menu *m=menu_of(menu);
    (void)reserved; (void)r;
    if(!w || !m) return FALSE;
    memset(&t,0,sizeof(t));
    t.owner=w;
    m->wnd=h; layout_popup(m);
    if(flags&TPM_CENTERALIGN) x-=m->width/2;
    else if(flags&TPM_RIGHTALIGN) x-=m->width;
    open_popup(&t,menu,x,y,0);
    run(&t,KeyDown(VK_LBUTTON) || KeyDown(VK_RBUTTON));
    return TRUE;
}
BOOL WINAPI HiliteMenuItem(HWND h,HMENU menu,UINT which,UINT flags) {
    Wnd *w=WndFromHandle(h); MenuItem *it=find_item(menu,which,flags,NULL);
    if(!w || !it) return FALSE;
    it->flags=(it->flags&~MF_HILITE)|(flags&MF_HILITE);
    if(w->menu==menu) {HDC dc=GetWindowDC(h); PaintMenuBar(w,dc); ReleaseDC(h,dc);}
    return TRUE;
}
void WINAPI DrawMenuBar(HWND h) {
    Wnd *w=WndFromHandle(h);
    if(!w || !w->menu) return;
    {RECT r=w->window; OffsetRect(&r,-w->parent->client.left,-w->parent->client.top); PlaceWindow(w,&r,TRUE);}
    InvalidateWnd(w,NULL,FALSE,TRUE);
}

/* --- accelerators -------------------------------------------------------------------------- */
typedef struct {BOOL used; int count; ACCEL *table; Queue *owner;} AccelTable;
static AccelTable accels[ACCELS];
HACCEL WINAPI CreateAcceleratorTable(LPACCEL entries,int n) {
    int i;
    if(!entries || n<=0) return NULL;
    for(i=0;i<ACCELS;i++) if(!accels[i].used) break;
    if(i==ACCELS || !(accels[i].table=(ACCEL *)ualloc((DWORD)(n*sizeof(ACCEL))))) return NULL;
    memcpy(accels[i].table,entries,(DWORD)n*sizeof(ACCEL));
    accels[i].used=TRUE; accels[i].count=n; accels[i].owner=CurrentQueue();
    return (HACCEL)(ULONG_PTR)(HACCEL_BASE+i*4);
}
HACCEL WINAPI LoadAccelerators(HINSTANCE instance,LPCSTR name) {
    DWORD size; const WORD *p=(const WORD *)Resource(instance,RT_ACCELERATOR,name,&size); int i,n,k;
    if(!p) return NULL;
    n=(int)(size/8);
    for(i=0;i<ACCELS;i++) if(!accels[i].used) break;
    if(i==ACCELS || !(accels[i].table=(ACCEL *)ualloc((DWORD)(n*sizeof(ACCEL))))) return NULL;
    for(k=0;k<n;k++) {accels[i].table[k].fVirt=(BYTE)p[k*4]; accels[i].table[k].key=p[k*4+1]; accels[i].table[k].cmd=p[k*4+2]; if(p[k*4]&0x80) {n=k+1; break;}}
    accels[i].used=TRUE; accels[i].count=n; accels[i].owner=CurrentQueue();
    return (HACCEL)(ULONG_PTR)(HACCEL_BASE+i*4);
}
static AccelTable *accel_of(HACCEL h) {
    ULONG_PTR v=(ULONG_PTR)h;
    if(v<HACCEL_BASE || (v-HACCEL_BASE)%4 || (v-HACCEL_BASE)/4>=ACCELS) return NULL;
    return accels[(v-HACCEL_BASE)/4].used?&accels[(v-HACCEL_BASE)/4]:NULL;
}
BOOL WINAPI DestroyAcceleratorTable(HACCEL h) {
    AccelTable *a=accel_of(h);
    if(!a) return FALSE;
    GlobalFree(a->table); a->used=FALSE; return TRUE;
}
/* WM_INITMENUPOPUP for each popup on the way to a command's item, as when the
 * menu is opened; FALSE when the menu does not have it. */
static BOOL init_popups(HWND h,HMENU menu,UINT cmd) {
    Menu *m=menu_of(menu); int i;
    if(!m) return FALSE;
    for(i=0;i<m->count;i++) {
        HMENU popup=m->items[i].popup;
        if(!(m->items[i].flags&MF_POPUP)) {if(m->items[i].id==cmd) return TRUE; continue;}
        if(find_item(popup,cmd,MF_BYCOMMAND,NULL)) {
            SendMessage(h,WM_INITMENUPOPUP,(WPARAM)popup,MAKELPARAM(i,FALSE));
            init_popups(h,popup,cmd);
            return TRUE;
        }
    }
    return FALSE;
}
int WINAPI TranslateAccelerator(HWND h,HACCEL table,LPMSG msg) {
    AccelTable *a=accel_of(table); int i; BOOL virt,shift,ctrl,alt;
    if(!a || !msg || !h) return 0;
    if(msg->message!=WM_KEYDOWN && msg->message!=WM_SYSKEYDOWN && msg->message!=WM_CHAR && msg->message!=WM_SYSCHAR) return 0;
    virt=msg->message==WM_KEYDOWN || msg->message==WM_SYSKEYDOWN;
    shift=KeyDown(VK_SHIFT); ctrl=KeyDown(VK_CONTROL); alt=msg->message==WM_SYSKEYDOWN || msg->message==WM_SYSCHAR || KeyDown(VK_MENU);
    for(i=0;i<a->count;i++) {
        ACCEL *e=&a->table[i];
        if(((e->fVirt&FVIRTKEY)!=0)!=virt) continue;
        if(virt) {
            if(e->key!=(WORD)msg->wParam) continue;
            if(((e->fVirt&FSHIFT)!=0)!=shift || ((e->fVirt&FCONTROL)!=0)!=ctrl || ((e->fVirt&FALT)!=0)!=alt) continue;
        } else {
            if(e->key!=(WORD)msg->wParam) continue;
            if((e->fVirt&FALT) && !alt) continue;
        }
        {
            /* As if its menu were opened: the program sets the item's state first. */
            Wnd *w=WndFromHandle(h); HMENU menu=w?w->menu:NULL; MenuItem *it;
            if(menu) {SendMessage(h,WM_INITMENU,(WPARAM)menu,0); init_popups(h,menu,e->cmd);}
            it=menu && IsWindow(h)?find_item(menu,e->cmd,MF_BYCOMMAND,NULL):NULL;
            if(it && (it->flags&(MF_GRAYED|MF_DISABLED))) return 1;
            if(IsIconic(h)) return 1;
            SendMessage(h,WM_COMMAND,MAKEWPARAM(e->cmd,1),0);
        }
        return 1;
    }
    return 0;
}
/* A task's menus and accelerator tables go when it ends (its windows' went with them). */
void MenuTaskEnded(Queue *q) {
    int i;
    for(i=0;i<MENUS;i++) if(menus[i].used && menus[i].owner==q) {
        int k; MenuItem *items=menus[i].items;
        for(k=0;k<menus[i].count;k++) if(items[k].text) GlobalFree(items[k].text);
        if(items) GlobalFree(items);
        menus[i].used=FALSE;
    }
    for(i=0;i<ACCELS;i++) if(accels[i].used && accels[i].owner==q) {GlobalFree(accels[i].table); accels[i].used=FALSE;}
}
void MenuInit(void) {
    memset(menus,0,sizeof(menus)); memset(accels,0,sizeof(accels));
    RegisterSystemClass("#32768",MenuWindowProc,CS_SAVEBITS,16,NULL,0);
}
