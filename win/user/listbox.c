/* SPDX-License-Identifier: GPL-2.0-or-later
 * The LISTBOX control (sorted or not, single, multiple or extended
 * selection, item data, owner-drawn fixed items, tab stops, directory
 * listings) and the COMBOBOX control (simple, drop-down and drop-down list),
 * whose list drops down in a "ComboLBox" popup.
 */
#include "user.h"

typedef struct {char *text; ULONG_PTR data; BOOL selected;} LbItem;
typedef struct {
    LbItem *items; int count,cap;
    int top,caret,anchor,cur;      /* first visible, focus item, extended-selection anchor, single selection */
    int item_height; BOOL focused,captured,hovered; int tabs[16],ntabs;
    HWND combo;                    /* the combo box this list belongs to */
} ListBox;

static ListBox *lb_of(HWND h) {return (ListBox *)GetWindowLongPtr(h,0);}
static BOOL has_strings(Wnd *w) {return !(w->style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE)) || (w->style&LBS_HASSTRINGS);}
static BOOL multi(Wnd *w) {return (w->style&(LBS_MULTIPLESEL|LBS_EXTENDEDSEL))!=0;}
static int page(Wnd *w,ListBox *l) {RECT r; GetClientRect(w->handle,&r); return max(1,(r.bottom-r.top)/l->item_height);}
static HWND notify_target(Wnd *w,ListBox *l) {return l->combo?l->combo:w->parent->handle;}
static void notify(Wnd *w,ListBox *l,UINT code) {
    if(!(w->style&LBS_NOTIFY) && !l->combo) return;
    SendMessage(notify_target(w,l),WM_COMMAND,MAKEWPARAM(l->combo?0xffff:w->id,code),(LPARAM)w->handle);
}
static void lb_bars(Wnd *w,ListBox *l) {
    if(!(w->style&WS_VSCROLL)) return;
    SetScrollRange(w->handle,SB_VERT,0,max(0,l->count-page(w,l)),FALSE);
    SetScrollPos(w->handle,SB_VERT,l->top,TRUE);
}
static void draw_item(Wnd *w,ListBox *l,HDC dc,int i,const RECT *r) {
    BOOL sel=i>=0 && i<l->count && (multi(w)?l->items[i].selected:i==l->cur);
    if(w->style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE)) {
        DRAWITEMSTRUCT d;
        d.CtlType=l->combo?ODT_COMBOBOX:ODT_LISTBOX; d.CtlID=(UINT)w->id; d.itemID=(UINT)i; d.itemAction=ODA_DRAWENTIRE;
        d.itemState=(sel?ODS_SELECTED:0)|(l->focused && i==l->caret?ODS_FOCUS:0)|(Enabled(w)?0:ODS_DISABLED);
        d.hwndItem=w->handle; d.hDC=dc; d.rcItem=*r; d.itemData=i>=0 && i<l->count?l->items[i].data:0;
        SendMessage(l->combo?GetParent(l->combo):w->parent->handle,WM_DRAWITEM,w->id,(LPARAM)&d);
        return;
    }
    if(sel) {FillRect(dc,r,SysBrush(COLOR_HIGHLIGHT)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
    else SetTextColor(dc,Enabled(w)?GetSysColor(COLOR_WINDOWTEXT):GetSysColor(COLOR_GRAYTEXT));
    SetBkMode(dc,TRANSPARENT);
    if(i>=0 && i<l->count && l->items[i].text) {
        if(w->style&LBS_USETABSTOPS) TabbedTextOut(dc,r->left+2,r->top,l->items[i].text,-1,l->ntabs,l->ntabs?l->tabs:NULL,r->left+2);
        else TextOut(dc,r->left+2,r->top,l->items[i].text,lstrlen(l->items[i].text));
    }
    if(l->focused && i==l->caret) DrawFocusRect(dc,r);
}
static void paint(Wnd *w,ListBox *l,HDC dc) {
    RECT c,r; int i; HBRUSH bg; HGDIOBJ old;
    GetClientRect(w->handle,&c);
    bg=(HBRUSH)SendMessage(l->combo?GetParent(l->combo):w->parent->handle,WM_CTLCOLORLISTBOX,(WPARAM)dc,(LPARAM)w->handle);
    if(!bg) bg=ControlColor(w,dc,WM_CTLCOLORLISTBOX);
    FillRect(dc,&c,bg);
    old=SelectObject(dc,w->font?w->font:SystemFont());
    for(i=l->top;i<l->count;i++) {
        SetRect(&r,c.left,c.top+(i-l->top)*l->item_height,c.right,c.top+(i-l->top+1)*l->item_height);
        if(r.top>=c.bottom) break;
        draw_item(w,l,dc,i,&r);
    }
    if(!l->count && l->focused) {SetRect(&r,c.left,c.top,c.right,c.top+l->item_height); DrawFocusRect(dc,&r);}
    SelectObject(dc,old);
}
static void redraw(Wnd *w,ListBox *l) {
    HDC dc;
    if(!Shown(w)) return;
    dc=GetDC(w->handle); paint(w,l,dc); ReleaseDC(w->handle,dc);
}
static void show_item(Wnd *w,ListBox *l,int i) {
    int p=page(w,l),old=l->top;
    if(i<0) return;
    if(i<l->top) l->top=i;
    else if(i>=l->top+p) l->top=i-p+1;
    l->top=max(0,min(l->top,max(0,l->count-p)));
    if(old!=l->top) lb_bars(w,l);
}
static int compare(const char *a,const char *b) {return lstrcmpi(a?a:"",b?b:"");}
static int insert(Wnd *w,ListBox *l,int at,LPCSTR text,ULONG_PTR data) {
    LbItem *it;
    if(l->count==l->cap) {
        int n=l->cap?l->cap*2:32; LbItem *items=(LbItem *)GlobalAlloc(GPTR|GMEM_SHARE,(DWORD)(n*sizeof(LbItem)));
        if(!items) return LB_ERRSPACE;
        if(l->count) memcpy(items,l->items,sizeof(LbItem)*(unsigned)l->count);
        if(l->items) GlobalFree(l->items);
        l->items=items; l->cap=n;
    }
    if(at<0 || at>l->count) at=l->count;
    memmove(&l->items[at+1],&l->items[at],sizeof(LbItem)*(unsigned)(l->count-at));
    it=&l->items[at]; memset(it,0,sizeof(*it));
    if(has_strings(w)) {
        int n=text?lstrlen(text):0;
        if(!(it->text=(char *)GlobalAlloc(GPTR|GMEM_SHARE,(DWORD)n+1))) {memmove(&l->items[at],&l->items[at+1],sizeof(LbItem)*(unsigned)(l->count-at)); return LB_ERRSPACE;}
        if(n) memcpy(it->text,text,(size_t)n);
    } else it->data=(ULONG_PTR)text;
    if(data) it->data=data;
    l->count++;
    if(l->cur>=at) l->cur++;
    if(l->caret>=at && l->count>1) l->caret++;
    return at;
}
static int sorted_position(Wnd *w,ListBox *l,LPCSTR text) {
    int i;
    if(!has_strings(w)) return l->count;
    for(i=0;i<l->count;i++) if(compare(text,l->items[i].text)<0) return i;
    return l->count;
}
static void delete_item(Wnd *w,ListBox *l,int i) {
    if(w->style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE)) {
        DELETEITEMSTRUCT d; d.CtlType=l->combo?ODT_COMBOBOX:ODT_LISTBOX; d.CtlID=(UINT)w->id; d.itemID=(UINT)i; d.hwndItem=w->handle; d.itemData=l->items[i].data;
        SendMessage(w->parent->handle,WM_DELETEITEM,w->id,(LPARAM)&d);
    }
    if(l->items[i].text) GlobalFree(l->items[i].text);
    memmove(&l->items[i],&l->items[i+1],sizeof(LbItem)*(unsigned)(l->count-i-1));
    l->count--;
    if(l->cur==i) l->cur=-1; else if(l->cur>i) l->cur--;
    if(l->caret>=l->count) l->caret=max(0,l->count-1);
}
static BOOL prefix(const char *t,const char *p) {
    for(;*p;p++,t++) if(!*t || (BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)*t)!=(BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)*p)) return FALSE;
    return TRUE;
}
static int find(Wnd *w,ListBox *l,int after,LPCSTR text,BOOL exact) {
    int i,n=l->count;
    if(after<0 || after>=n) after=-1;
    for(i=1;i<=n;i++) {
        int k=(after+i+n)%n; const char *t=l->items[k].text;
        if(!has_strings(w)) {if(l->items[k].data==(ULONG_PTR)text) return k; continue;}
        if(!t || !text) continue;
        if(exact?!lstrcmpi(t,text):prefix(t,text)) return k;
    }
    return LB_ERR;
}
static void select_single(Wnd *w,ListBox *l,int i,BOOL notify_change) {
    BOOL changed=l->cur!=i;
    l->cur=i; if(i>=0) l->caret=i;
    show_item(w,l,i); redraw(w,l);
    if(changed && notify_change) notify(w,l,LBN_SELCHANGE);
}
static void select_range(ListBox *l,int a,int b,BOOL on) {
    int i; if(a>b) {int t=a; a=b; b=t;}
    for(i=max(a,0);i<=b && i<l->count;i++) l->items[i].selected=on;
}
static int item_at(ListBox *l,int y) {
    int i=l->top+y/l->item_height;
    if(y<0) return l->top-1;
    return i;
}
/* Files, directories ([name]) and drives ([-x-]) matching spec. */
static void list_directory(Wnd *w,ListBox *l,UINT attrs,LPCSTR spec) {
    WIN32_FIND_DATA f; HANDLE h; char text[MAX_PATH+3];
    if(!(attrs&DDL_EXCLUSIVE) || !(attrs&(DDL_DIRECTORY|DDL_DRIVES))) {
        h=FindFirstFile(spec,&f);
        if(h!=INVALID_HANDLE_VALUE) {
            do {
                DWORD a=f.dwFileAttributes;
                if(a&FILE_ATTRIBUTE_DIRECTORY) continue;
                if((a&FILE_ATTRIBUTE_HIDDEN) && !(attrs&DDL_HIDDEN)) continue;
                if((a&FILE_ATTRIBUTE_SYSTEM) && !(attrs&DDL_SYSTEM)) continue;
                if((attrs&DDL_EXCLUSIVE) && !(a&attrs&(DDL_READONLY|DDL_ARCHIVE))) continue;
                lstrcpy(text,f.cFileName); AnsiLower(text);
                insert(w,l,w->style&LBS_SORT?sorted_position(w,l,text):-1,text,0);
            } while(FindNextFile(h,&f));
            FindClose(h);
        }
    }
    if(attrs&DDL_DIRECTORY) {
        h=FindFirstFile("*.*",&f);
        if(h!=INVALID_HANDLE_VALUE) {
            do {
                if(!(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || !lstrcmp(f.cFileName,".")) continue;
                wsprintf(text,"[%s]",f.cFileName); AnsiLower(text);
                insert(w,l,-1,text,0);
            } while(FindNextFile(h,&f));
            FindClose(h);
        }
    }
    if(attrs&DDL_DRIVES) {
        DWORD drives=GetLogicalDrives(); int d;
        for(d=0;d<26;d++) if(drives&(1u<<d)) {wsprintf(text,"[-%c-]",'a'+d); insert(w,l,-1,text,0);}
    }
}

/* --- the list box procedure ------------------------------------------------------------- */
static LRESULT list_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp,BOOL dropped) {
    Wnd *w=WndFromHandle(h); ListBox *l=lb_of(h);
    if(!w) return 0;
    if(!l && msg!=WM_NCCREATE) return DefWindowProc(h,msg,wp,lp);
    switch(msg) {
    case WM_NCCREATE: {
        l=(ListBox *)GlobalAlloc(GPTR|GMEM_SHARE,sizeof(ListBox));
        if(!l) return FALSE;
        l->cur=-1; l->item_height=CharHeight();
        if(w->style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE)) {
            MEASUREITEMSTRUCT m; memset(&m,0,sizeof(m)); m.CtlType=ODT_LISTBOX; m.CtlID=(UINT)w->id; m.itemHeight=(UINT)CharHeight();
            if(w->parent!=desktop) SendMessage(w->parent->handle,WM_MEASUREITEM,w->id,(LPARAM)&m);
            l->item_height=max(1,(int)m.itemHeight);
        }
        if(dropped) l->combo=((LPCREATESTRUCT)lp)->lpCreateParams?(HWND)((LPCREATESTRUCT)lp)->lpCreateParams:NULL;
        SetWindowLongPtr(h,0,(LONG_PTR)l);
        if(w->style&WS_VSCROLL) {w->scroll[SB_VERT].min=w->scroll[SB_VERT].max=0; w->scroll[SB_VERT].hidden=TRUE; CalcClient(w);}
        return DefWindowProc(h,msg,wp,lp);
    }
    case WM_NCDESTROY: {
        int i; for(i=0;i<l->count;i++) if(l->items[i].text) GlobalFree(l->items[i].text);
        if(l->items) GlobalFree(l->items);
        GlobalFree(l); SetWindowLongPtr(h,0,0);
        return 0;
    }
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(w,l,dc); EndPaint(h,&ps); return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: lb_bars(w,l); return 0;
    case WM_SETFOCUS: l->focused=TRUE; redraw(w,l); notify(w,l,LBN_SETFOCUS); return 0;
    case WM_KILLFOCUS: l->focused=FALSE; redraw(w,l); notify(w,l,LBN_KILLFOCUS); return 0;
    case WM_SETFONT: w->font=(HFONT)wp; if(lp) InvalidateRect(h,NULL,TRUE); return 0;
    case WM_ENABLE: InvalidateRect(h,NULL,TRUE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int i=item_at(l,GET_Y_LPARAM(lp));
        if(dropped) {
            RECT r; POINT p; GetClientRect(h,&r); p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp);
            if(!PtInRect(&r,p)) {if(l->combo) SendMessage(l->combo,WM_COMMAND,MAKEWPARAM(0xfffd,0),(LPARAM)h); return 0;}
        }
        if(!l->combo && GetFocus()!=h) SetFocus(h);
        if(i<0 || i>=l->count) return 0;
        if(msg==WM_LBUTTONDBLCLK && !(w->style&LBS_MULTIPLESEL)) {notify(w,l,LBN_DBLCLK); return 0;}
        if(w->style&LBS_MULTIPLESEL) {l->items[i].selected=!l->items[i].selected; l->caret=i; redraw(w,l); notify(w,l,LBN_SELCHANGE); return 0;}
        if(w->style&LBS_EXTENDEDSEL) {
            if(wp&MK_SHIFT) {int k; for(k=0;k<l->count;k++) l->items[k].selected=FALSE; select_range(l,l->anchor,i,TRUE);}
            else if(wp&MK_CONTROL) {l->items[i].selected=!l->items[i].selected; l->anchor=i;}
            else {int k; for(k=0;k<l->count;k++) l->items[k].selected=k==i; l->anchor=i;}
            l->caret=i; redraw(w,l); notify(w,l,LBN_SELCHANGE);
        } else select_single(w,l,i,TRUE);
        SetCapture(h); l->captured=TRUE;
        return 0;
    }
    case WM_MOUSEMOVE:
        if(l->captured || dropped) {
            int i=item_at(l,GET_Y_LPARAM(lp)); RECT r; GetClientRect(h,&r);
            if(!l->captured && (GET_X_LPARAM(lp)<0 || GET_X_LPARAM(lp)>=r.right || GET_Y_LPARAM(lp)<0 || GET_Y_LPARAM(lp)>=r.bottom)) return 0;
            if(dropped) l->hovered=TRUE;
            i=max(0,min(i,l->count-1));
            if(l->count && (w->style&LBS_EXTENDEDSEL) && l->captured) {int k; for(k=0;k<l->count;k++) l->items[k].selected=FALSE; select_range(l,l->anchor,i,TRUE); l->caret=i; show_item(w,l,i); redraw(w,l);}
            else if(l->count && !multi(w) && i!=l->cur) select_single(w,l,i,!dropped);
        }
        return 0;
    case WM_LBUTTONUP: {
        BOOL was=l->captured;
        if(l->captured) {ReleaseCapture(); l->captured=FALSE;}
        if(dropped && l->combo) {
            RECT r; POINT p; GetClientRect(h,&r); p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp);
            if(PtInRect(&r,p) && (was || l->hovered)) SendMessage(l->combo,WM_COMMAND,MAKEWPARAM(0xfffe,0),(LPARAM)h);
            else if(IsWindowVisible(h)) SetCapture(h);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int i=multi(w)?l->caret:l->cur,p=page(w,l);
        if(w->style&LBS_WANTKEYBOARDINPUT) {
            LRESULT r=SendMessage(w->parent->handle,WM_VKEYTOITEM,MAKEWPARAM(wp,l->caret),(LPARAM)h);
            if(r==-2) return 0;
            if(r>=0) {i=(int)r; goto go;}
        }
        switch(wp) {
        case VK_UP: case VK_LEFT: i=i<0?0:i-1; break;
        case VK_DOWN: case VK_RIGHT: i=i<0?0:i+1; break;
        case VK_PRIOR: i-=p-1; break;
        case VK_NEXT: i+=p-1; break;
        case VK_HOME: i=0; break;
        case VK_END: i=l->count-1; break;
        case VK_SPACE:
            if(w->style&LBS_MULTIPLESEL && l->caret<l->count) {l->items[l->caret].selected=!l->items[l->caret].selected; redraw(w,l); notify(w,l,LBN_SELCHANGE);}
            return 0;
        case VK_RETURN: if(dropped && l->combo) SendMessage(l->combo,WM_COMMAND,MAKEWPARAM(0xfffe,0),(LPARAM)h); return 0;
        case VK_ESCAPE: if(dropped && l->combo) SendMessage(l->combo,WM_COMMAND,MAKEWPARAM(0xfffd,0),(LPARAM)h); return 0;
        default: return 0;
        }
    go:
        if(!l->count) return 0;
        i=max(0,min(i,l->count-1));
        if(multi(w)) {
            l->caret=i;
            if(w->style&LBS_EXTENDEDSEL) {int k; if(!KeyDown(VK_SHIFT)) l->anchor=i; for(k=0;k<l->count;k++) l->items[k].selected=FALSE; select_range(l,l->anchor,i,TRUE); notify(w,l,LBN_SELCHANGE);}
            show_item(w,l,i); redraw(w,l);
        } else select_single(w,l,i,TRUE);
        return 0;
    }
    case WM_CHAR: {
        char text[2]; int i;
        if(wp<' ') return 0;
        if(w->style&LBS_WANTKEYBOARDINPUT) {
            LRESULT r=SendMessage(w->parent->handle,WM_CHARTOITEM,MAKEWPARAM(wp,l->caret),(LPARAM)h);
            if(r==-2) return 0;
            if(r>=0) {select_single(w,l,(int)r,TRUE); return 0;}
        }
        text[0]=(char)wp; text[1]=0;
        if((i=find(w,l,multi(w)?l->caret:l->cur,text,FALSE))>=0) {
            if(multi(w)) {l->caret=i; show_item(w,l,i); redraw(w,l);} else select_single(w,l,i,TRUE);
        }
        return 0;
    }
    case WM_VSCROLL: {
        int top=l->top,p=page(w,l);
        switch(LOWORD(wp)) {
        case SB_LINEUP: top--; break;
        case SB_LINEDOWN: top++; break;
        case SB_PAGEUP: top-=p; break;
        case SB_PAGEDOWN: top+=p; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: top=(short)HIWORD(wp); break;
        case SB_TOP: top=0; break;
        case SB_BOTTOM: top=l->count; break;
        default: return 0;
        }
        top=max(0,min(top,max(0,l->count-p)));
        if(top!=l->top) {l->top=top; lb_bars(w,l); redraw(w,l);}
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTCHARS;
    case LB_ADDSTRING: {
        int at=insert(w,l,w->style&LBS_SORT?sorted_position(w,l,(LPCSTR)lp):-1,(LPCSTR)lp,0);
        if(at>=0 && !(w->style&LBS_NOREDRAW)) {lb_bars(w,l); redraw(w,l);}
        return at;
    }
    case LB_INSERTSTRING: {
        int at=insert(w,l,(int)wp,(LPCSTR)lp,0);
        if(at>=0 && !(w->style&LBS_NOREDRAW)) {lb_bars(w,l); redraw(w,l);}
        return at;
    }
    case LB_DELETESTRING:
        if((int)wp<0 || (int)wp>=l->count) return LB_ERR;
        delete_item(w,l,(int)wp); l->top=max(0,min(l->top,l->count-1)); lb_bars(w,l); redraw(w,l);
        return l->count;
    case LB_RESETCONTENT:
        while(l->count) delete_item(w,l,l->count-1);
        l->top=0; l->cur=-1; l->caret=0; lb_bars(w,l); redraw(w,l);
        return 0;
    case LB_GETCOUNT: return l->count;
    case LB_GETTEXT:
        if((int)wp<0 || (int)wp>=l->count || !lp) return LB_ERR;
        if(!has_strings(w)) {*(ULONG_PTR *)lp=l->items[wp].data; return sizeof(ULONG_PTR);}
        lstrcpy((LPSTR)lp,l->items[wp].text); return lstrlen(l->items[wp].text);
    case LB_GETTEXTLEN:
        if((int)wp<0 || (int)wp>=l->count) return LB_ERR;
        return has_strings(w)?lstrlen(l->items[wp].text):sizeof(ULONG_PTR);
    case LB_GETITEMDATA: return (int)wp>=0 && (int)wp<l->count?(LRESULT)l->items[wp].data:LB_ERR;
    case LB_SETITEMDATA: if((int)wp<0 || (int)wp>=l->count) return LB_ERR; l->items[wp].data=(ULONG_PTR)lp; return 0;
    case LB_GETCURSEL: return multi(w)?l->caret:l->cur;
    case LB_SETCURSEL:
        if(multi(w)) return LB_ERR;
        if((int)wp>=l->count) return LB_ERR;
        select_single(w,l,(int)wp,FALSE);
        return (int)wp<0?LB_ERR:(LRESULT)wp;
    case LB_GETSEL:
        if((int)wp<0 || (int)wp>=l->count) return LB_ERR;
        return multi(w)?l->items[wp].selected:(int)wp==l->cur;
    case LB_SETSEL:
        if(!multi(w)) return LB_ERR;
        if((int)lp<0) select_range(l,0,l->count-1,(BOOL)wp);
        else if((int)lp<l->count) l->items[lp].selected=(BOOL)wp;
        redraw(w,l);
        return 0;
    case LB_SELITEMRANGE: if(!multi(w)) return LB_ERR; select_range(l,LOWORD(lp),HIWORD(lp),(BOOL)wp); redraw(w,l); return 0;
    case LB_GETSELCOUNT: {
        int i,n=0; if(!multi(w)) return LB_ERR;
        for(i=0;i<l->count;i++) n+=l->items[i].selected;
        return n;
    }
    case LB_GETSELITEMS: {
        int i,n=0; if(!multi(w)) return LB_ERR;
        for(i=0;i<l->count && n<(int)wp;i++) if(l->items[i].selected) ((LPINT)lp)[n++]=i;
        return n;
    }
    case LB_SELECTSTRING: {
        int i=find(w,l,(int)wp,(LPCSTR)lp,FALSE);
        if(i>=0 && !multi(w)) select_single(w,l,i,FALSE);
        return i;
    }
    case LB_FINDSTRING: return find(w,l,(int)wp,(LPCSTR)lp,FALSE);
    case LB_FINDSTRINGEXACT: return find(w,l,(int)wp,(LPCSTR)lp,TRUE);
    case LB_GETTOPINDEX: return l->top;
    case LB_SETTOPINDEX: l->top=max(0,min((int)wp,max(0,l->count-1))); lb_bars(w,l); redraw(w,l); return 0;
    case LB_GETCARETINDEX: return l->caret;
    case LB_SETCARETINDEX: l->caret=max(0,min((int)wp,l->count-1)); show_item(w,l,l->caret); redraw(w,l); return 0;
    case LB_GETITEMRECT: {
        RECT *r=(RECT *)lp; RECT c; GetClientRect(h,&c);
        if(!r || (int)wp<0 || (int)wp>=l->count) return LB_ERR;
        SetRect(r,0,((int)wp-l->top)*l->item_height,c.right,((int)wp-l->top+1)*l->item_height);
        return 0;
    }
    case LB_SETITEMHEIGHT: l->item_height=max(1,LOWORD(lp)); InvalidateRect(h,NULL,TRUE); return 0;
    case LB_GETITEMHEIGHT: return l->item_height;
    case LB_SETTABSTOPS: {
        int i; l->ntabs=min((int)wp,16);
        for(i=0;i<l->ntabs;i++) l->tabs[i]=((LPINT)lp)[i]*CharWidth()/4;
        return TRUE;
    }
    case LB_SETCOLUMNWIDTH: return 0;
    case LB_DIR: {
        int before=l->count;
        list_directory(w,l,(UINT)wp,(LPCSTR)lp);
        lb_bars(w,l); redraw(w,l);
        return l->count>before?l->count-1:LB_ERR;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}
LRESULT CALLBACK ListBoxProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return list_proc(h,msg,wp,lp,FALSE);}
LRESULT CALLBACK ComboListProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return list_proc(h,msg,wp,lp,TRUE);
}

/* --- combo boxes ---------------------------------------------------------------------------- */
typedef struct {HWND edit,list; int drop_height; BOOL dropped,focused; int item_height;} Combo;
#define CB_EDIT_ID 1001
#define CB_LIST_ID 1000
static Combo *combo_of(HWND h) {return (Combo *)GetWindowLongPtr(h,0);}
static int combo_type(Wnd *w) {return (int)(w->style&3);}
static void combo_rects(Wnd *w,Combo *c,RECT *field,RECT *button) {
    RECT r; GetClientRect(w->handle,&r);
    *field=r; field->bottom=c->item_height+4;
    SetRectEmpty(button);
    if(combo_type(w)!=CBS_SIMPLE) {*button=*field; button->left=button->right-SCROLL; field->right=button->left+1;}
}
static void combo_paint(Wnd *w,Combo *c,HDC dc) {
    RECT f,b; HBRUSH bg;
    combo_rects(w,c,&f,&b);
    bg=(HBRUSH)SendMessage(w->parent->handle,WM_CTLCOLORLISTBOX,(WPARAM)dc,(LPARAM)w->handle);
    if(!bg) bg=ControlColor(w,dc,WM_CTLCOLORLISTBOX);
    if(combo_type(w)==CBS_DROPDOWNLIST) {
        char text[256]; LRESULT sel=SendMessage(c->list,LB_GETCURSEL,0,0); RECT t=f; HGDIOBJ old;
        FrameRect(dc,&f,SysBrush(COLOR_WINDOWFRAME)); InflateRect(&t,-1,-1);
        FillRect(dc,&t,c->focused?SysBrush(COLOR_HIGHLIGHT):bg);
        text[0]=0;
        if(sel>=0) SendMessage(c->list,LB_GETTEXT,(WPARAM)sel,(LPARAM)text);
        old=SelectObject(dc,w->font?w->font:SystemFont());
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(c->focused?COLOR_HIGHLIGHTTEXT:COLOR_WINDOWTEXT));
        TextOut(dc,t.left+2,t.top+1,text,lstrlen(text));
        SelectObject(dc,old);
        if(c->focused) {InflateRect(&t,-1,-1); DrawFocusRect(dc,&t);}
    }
    if(!IsRectEmpty(&b)) {
        RECT in=b; int cx=(b.left+b.right)/2,cy=(b.top+b.bottom)/2,i;
        DrawButtonFace(dc,&in,FALSE);
        for(i=0;i<4;i++) {RECT a; SetRect(&a,cx-3+i,cy-1+i,cx+4-i,cy+i); FillRect(dc,&a,SysBrush(COLOR_BTNTEXT));}
        {RECT u; SetRect(&u,cx-3,cy+4,cx+4,cy+5); FillRect(dc,&u,SysBrush(COLOR_BTNTEXT));}
    }
}
static void combo_redraw(Wnd *w,Combo *c) {HDC dc; if(!Shown(w)) return; dc=GetDC(w->handle); combo_paint(w,c,dc); ReleaseDC(w->handle,dc);}
static void combo_sync_edit(Wnd *w,Combo *c) {
    LRESULT sel=SendMessage(c->list,LB_GETCURSEL,0,0); char text[256];
    if(!c->edit) {combo_redraw(w,c); return;}
    text[0]=0;
    if(sel>=0) SendMessage(c->list,LB_GETTEXT,(WPARAM)sel,(LPARAM)text);
    SetWindowText(c->edit,text);
    SendMessage(c->edit,EM_SETSEL,0,-1);
}
static void combo_notify(Wnd *w,UINT code) {SendNotify(w,code);}
static void drop(Wnd *w,Combo *c,BOOL show) {
    RECT f,b;
    if(combo_type(w)==CBS_SIMPLE || show==c->dropped) return;
    if(show) {
        int n=(int)SendMessage(c->list,LB_GETCOUNT,0,0),h;
        combo_notify(w,CBN_DROPDOWN);
        combo_rects(w,c,&f,&b);
        h=min(c->drop_height,max(1,n)*c->item_height+2);
        h=max(h,c->item_height+2);
        SetWindowPos(c->list,HWND_TOPMOST,w->client.left,w->client.top+f.bottom,w->client.right-w->client.left,h,SWP_NOACTIVATE);
        {ListBox *l=lb_of(c->list); if(l) {l->hovered=FALSE; l->captured=FALSE;}}
        ShowWindow(c->list,SW_SHOWNA);
        c->dropped=TRUE;
        SetCapture(c->list);
    } else {
        if(GetCapture()==c->list) ReleaseCapture();
        ShowWindow(c->list,SW_HIDE);
        c->dropped=FALSE;
        combo_notify(w,CBN_CLOSEUP);
    }
}
LRESULT CALLBACK ComboBoxProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h); Combo *c=combo_of(h);
    if(!w) return 0;
    if(!c && msg!=WM_NCCREATE) return DefWindowProc(h,msg,wp,lp);
    switch(msg) {
    case WM_NCCREATE: {
        LPCREATESTRUCT cs=(LPCREATESTRUCT)lp; RECT f,b; DWORD lstyle;
        if(!(c=(Combo *)GlobalAlloc(GPTR|GMEM_SHARE,sizeof(Combo)))) return FALSE;
        SetWindowLongPtr(h,0,(LONG_PTR)c);
        /* Scroll bar styles belong to the list. */
        w->style&=~(WS_VSCROLL|WS_HSCROLL); CalcClient(w);
        c->item_height=CharHeight();
        c->drop_height=max(cs->cy-(c->item_height+4),c->item_height*4);
        /* The control itself is as tall as its field (with the list when simple). */
        if(combo_type(w)!=CBS_SIMPLE) {RECT r=w->window; r.bottom=r.top+c->item_height+4; w->window=r; CalcClient(w);}
        combo_rects(w,c,&f,&b);
        lstyle=WS_BORDER|WS_VSCROLL|LBS_NOTIFY|(w->style&CBS_OWNERDRAWFIXED?LBS_OWNERDRAWFIXED:0)|(w->style&CBS_HASSTRINGS?LBS_HASSTRINGS:0);
        if(w->style&CBS_SORT) lstyle|=LBS_SORT;
        if(combo_type(w)==CBS_SIMPLE)
            c->list=CreateWindowEx(0,"LISTBOX",NULL,WS_CHILD|WS_VISIBLE|lstyle,0,f.bottom,cs->cx,max(c->item_height,cs->cy-f.bottom),h,(HMENU)CB_LIST_ID,cs->hInstance,NULL);
        else
            c->list=CreateWindowEx(WS_EX_TOPMOST,"ComboLBox",NULL,WS_POPUP|lstyle,0,0,cs->cx,c->drop_height,h,(HMENU)CB_LIST_ID,cs->hInstance,(void *)h);
        if(combo_type(w)!=CBS_DROPDOWNLIST)
            c->edit=CreateWindowEx(0,"EDIT",NULL,WS_CHILD|WS_VISIBLE|WS_BORDER|(w->style&CBS_AUTOHSCROLL?ES_AUTOHSCROLL:0),
                                   0,0,f.right-f.left,f.bottom-f.top,h,(HMENU)CB_EDIT_ID,cs->hInstance,NULL);
        if(combo_type(w)==CBS_SIMPLE && c->list) {ListBox *l=lb_of(c->list); if(l) l->combo=h;}
        return DefWindowProc(h,msg,wp,lp);
    }
    case WM_NCDESTROY:
        if(c->list && combo_type(w)!=CBS_SIMPLE) DestroyWindow(c->list);
        GlobalFree(c); SetWindowLongPtr(h,0,0);
        return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); combo_paint(w,c,dc); EndPaint(h,&ps); return 0;}
    case WM_ERASEBKGND: {RECT r; GetClientRect(h,&r); FillRect((HDC)wp,&r,ControlColor(w,(HDC)wp,WM_CTLCOLORDLG)); return 1;}
    case WM_SETFOCUS:
        c->focused=TRUE;
        if(c->edit) {SetFocus(c->edit); SendMessage(c->edit,EM_SETSEL,0,-1);}
        else combo_redraw(w,c);
        combo_notify(w,CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS: {
        HWND to=(HWND)wp;
        if(to && (to==c->edit || to==c->list)) return 0;
        c->focused=FALSE; drop(w,c,FALSE); combo_redraw(w,c); combo_notify(w,CBN_KILLFOCUS);
        return 0;
    }
    case WM_SETFONT:
        w->font=(HFONT)wp;
        if(c->edit) SendMessage(c->edit,WM_SETFONT,wp,lp);
        if(c->list) SendMessage(c->list,WM_SETFONT,wp,lp);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        RECT f,b; POINT p; p.x=GET_X_LPARAM(lp); p.y=GET_Y_LPARAM(lp);
        combo_rects(w,c,&f,&b);
        if(GetFocus()!=h && GetFocus()!=c->edit) SetFocus(h);
        if(PtInRect(&b,p) || (combo_type(w)==CBS_DROPDOWNLIST && PtInRect(&f,p))) drop(w,c,!c->dropped);
        return 0;
    }
    case WM_KEYDOWN:
        if((wp==VK_DOWN || wp==VK_UP) && KeyDown(VK_MENU)) {drop(w,c,!c->dropped); return 0;}
        if(wp==VK_F4) {drop(w,c,!c->dropped); return 0;}
        if(c->dropped) return SendMessage(c->list,WM_KEYDOWN,wp,lp);
        if(wp==VK_UP || wp==VK_DOWN || wp==VK_PRIOR || wp==VK_NEXT || wp==VK_HOME || wp==VK_END) {
            LRESULT before=SendMessage(c->list,LB_GETCURSEL,0,0);
            SendMessage(c->list,WM_KEYDOWN,wp,lp);
            if(SendMessage(c->list,LB_GETCURSEL,0,0)!=before) {combo_sync_edit(w,c); combo_notify(w,CBN_SELCHANGE);}
        }
        return 0;
    case WM_CHAR:
        if(combo_type(w)==CBS_DROPDOWNLIST) {
            LRESULT before=SendMessage(c->list,LB_GETCURSEL,0,0);
            SendMessage(c->list,WM_CHAR,wp,lp);
            if(SendMessage(c->list,LB_GETCURSEL,0,0)!=before) {combo_sync_edit(w,c); combo_notify(w,CBN_SELCHANGE);}
        }
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTCHARS;
    case WM_COMMAND:
        if(LOWORD(wp)==CB_EDIT_ID) {
            if(HIWORD(wp)==EN_CHANGE) combo_notify(w,CBN_EDITCHANGE);
            else if(HIWORD(wp)==EN_UPDATE) combo_notify(w,CBN_EDITUPDATE);
            else if(HIWORD(wp)==EN_SETFOCUS && !c->focused) {c->focused=TRUE; combo_notify(w,CBN_SETFOCUS);}
            else if(HIWORD(wp)==EN_KILLFOCUS && GetFocus()!=h) {c->focused=FALSE; drop(w,c,FALSE); combo_notify(w,CBN_KILLFOCUS);}
            return 0;
        }
        if(LOWORD(wp)==0xfffe) {drop(w,c,FALSE); combo_sync_edit(w,c); combo_notify(w,CBN_SELCHANGE); if(!c->edit) SetFocus(h); return 0;}
        if(LOWORD(wp)==0xfffd) {drop(w,c,FALSE); return 0;}
        if(LOWORD(wp)==0xffff || LOWORD(wp)==CB_LIST_ID) {
            if(HIWORD(wp)==LBN_SELCHANGE && (!c->dropped || combo_type(w)==CBS_SIMPLE)) {combo_sync_edit(w,c); combo_notify(w,CBN_SELCHANGE);}
            if(HIWORD(wp)==LBN_DBLCLK) combo_notify(w,CBN_DBLCLK);
        }
        return 0;
    case WM_SETTEXT: if(c->edit) return SendMessage(c->edit,WM_SETTEXT,wp,lp); return CB_ERR;
    case WM_GETTEXT:
        if(c->edit) return SendMessage(c->edit,WM_GETTEXT,wp,lp);
        {LRESULT sel=SendMessage(c->list,LB_GETCURSEL,0,0); char text[256];
         if(!lp || (int)wp<=0) return 0;
         ((char *)lp)[0]=0;
         if(sel<0) return 0;
         SendMessage(c->list,LB_GETTEXT,(WPARAM)sel,(LPARAM)text); lstrcpyn((char *)lp,text,(int)wp); return lstrlen((char *)lp);}
    case WM_GETTEXTLENGTH: {char t[256]; return SendMessage(h,WM_GETTEXT,sizeof(t),(LPARAM)t);}
    case CB_ADDSTRING: return SendMessage(c->list,LB_ADDSTRING,wp,lp);
    case CB_INSERTSTRING: return SendMessage(c->list,LB_INSERTSTRING,wp,lp);
    case CB_DELETESTRING: return SendMessage(c->list,LB_DELETESTRING,wp,lp);
    case CB_RESETCONTENT: SendMessage(c->list,LB_RESETCONTENT,0,0); if(c->edit) SetWindowText(c->edit,""); combo_redraw(w,c); return CB_OKAY;
    case CB_GETCOUNT: return SendMessage(c->list,LB_GETCOUNT,0,0);
    case CB_GETCURSEL: return SendMessage(c->list,LB_GETCURSEL,0,0);
    case CB_SETCURSEL: {LRESULT r=SendMessage(c->list,LB_SETCURSEL,wp,0); combo_sync_edit(w,c); return r;}
    case CB_GETLBTEXT: return SendMessage(c->list,LB_GETTEXT,wp,lp);
    case CB_GETLBTEXTLEN: return SendMessage(c->list,LB_GETTEXTLEN,wp,lp);
    case CB_FINDSTRING: return SendMessage(c->list,LB_FINDSTRING,wp,lp);
    case CB_FINDSTRINGEXACT: return SendMessage(c->list,LB_FINDSTRINGEXACT,wp,lp);
    case CB_SELECTSTRING: {LRESULT r=SendMessage(c->list,LB_SELECTSTRING,wp,lp); if(r>=0) combo_sync_edit(w,c); return r;}
    case CB_GETITEMDATA: return SendMessage(c->list,LB_GETITEMDATA,wp,lp);
    case CB_SETITEMDATA: return SendMessage(c->list,LB_SETITEMDATA,wp,lp);
    case CB_DIR: return SendMessage(c->list,LB_DIR,wp,lp);
    case CB_SHOWDROPDOWN: drop(w,c,(BOOL)wp); return TRUE;
    case CB_GETDROPPEDSTATE: return c->dropped;
    case CB_GETDROPPEDCONTROLRECT: if(lp) {GetWindowRect(h,(LPRECT)lp); ((LPRECT)lp)->bottom+=c->drop_height;} return CB_OKAY;
    case CB_LIMITTEXT: if(c->edit) SendMessage(c->edit,EM_LIMITTEXT,wp,0); return TRUE;
    case CB_GETEDITSEL: return c->edit?SendMessage(c->edit,EM_GETSEL,wp,lp):CB_ERR;
    case CB_SETEDITSEL: if(c->edit) SendMessage(c->edit,EM_SETSEL,LOWORD(lp),(short)HIWORD(lp)); return TRUE;
    }
    return DefWindowProc(h,msg,wp,lp);
}
