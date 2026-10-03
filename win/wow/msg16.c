/* SPDX-License-Identifier: GPL-2.0-or-later
 * Convert messages between native and 16-bit procedures.
 *
 * - Packing: Windows 3.0 puts handles in lParam for WM_COMMAND, WM_ACTIVATE,
 *   WM_HSCROLL/VSCROLL, WM_MENUSELECT, WM_MENUCHAR, WM_PARENTNOTIFY and
 *   WM_CHARTOITEM/VKEYTOITEM; native packing uses wParam's high WORD.
 *   WM_CTLCOLORxxx becomes WM_CTLCOLOR with the kind in lParam's high WORD.
 * - Handles already share 16-bit values. Copy 16-bit MINMAXINFO,
 *   CREATESTRUCT and owner-draw structures through scratch memory.
 * - Native strings get alias selectors; guest far pointers map to native
 *   addresses without copying strings.
 * - Windows 3.0 EM_/LB_/CB_/BM_/STM_ numbers overlap at WM_USER; the window
 *   class disambiguates them. Convert EM_SETSEL/EM_LINESCROLL parameters,
 *   RECTs and WORD arrays as needed.
 * - MDI converts MDICREATESTRUCT (including child creation data), child
 *   WM_MDIACTIVATE (activation flag and both handles in lParam), WM_MDISETMENU
 *   (both menus in lParam), and WM_MDIGETACTIVE's high-WORD maximized flag.
 *
 * DDE retains the sender in wParam; user16.c converts posted/retrieved data.
 * Unknown messages pass through with lParam zero-extended.
 */
#include "api.h"
#include <dde.h>
#define WM_CTLCOLOR16 0x0019
DWORD pending_dialog_proc;

/* --- scratch memory and aliases ------------------------------------------- */
void *Scratch16(Task16 *t,WORD bytes,DWORD *segptr) {
    BYTE *p;
    bytes=(WORD)((bytes+3)&~3U);
    if(!t->scratch || bytes>t->scratch_top) {*segptr=0; return NULL;}
    t->scratch_top=(WORD)(t->scratch_top-bytes);
    p=t->scratch+t->scratch_top; memset(p,0,bytes);
    *segptr=(DWORD)t->scratch_sel<<16|t->scratch_top;
    return p;
}
DWORD AliasPointer(const void *p) {
    WORD sel;
    if(!p) return 0;
    sel=SelAlloc(SelLinear((void *)p),0xffff,SEL_DATA);
    return sel?(DWORD)sel<<16:0;
}
void AliasFree(DWORD segptr) {if(segptr) SelFree(HIWORD(segptr));}

/* --- packing ------------------------------------------------------------------ */
static BOOL handle_in_wparam(UINT msg) {
    if(msg>=WM_DDE_FIRST && msg<=WM_DDE_LAST) return TRUE;
    switch(msg) {
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SETCURSOR: case WM_MOUSEACTIVATE: case WM_INITDIALOG:
    case WM_ERASEBKGND: case WM_ICONERASEBKGND: case WM_SETFONT: case WM_INITMENU: case WM_INITMENUPOPUP:
    case WM_MDIDESTROY: case WM_MDIRESTORE: case WM_MDIMAXIMIZE: case WM_MDINEXT:
        return TRUE;
    default: return FALSE;
    }
}
static WORD handle16(UINT msg,WPARAM wp) {
    switch(msg) {
    case WM_ERASEBKGND: case WM_ICONERASEBKGND: case WM_SETFONT: return HGDI16(wp);
    case WM_INITMENU: case WM_INITMENUPOPUP: return HMENU16(wp);
    default: return HWND16(wp);
    }
}
static WPARAM handle32(UINT msg,WORD h) {
    switch(msg) {
    case WM_ERASEBKGND: case WM_ICONERASEBKGND: case WM_SETFONT: return (WPARAM)HGDI32(h);
    case WM_INITMENU: case WM_INITMENUPOPUP: return (WPARAM)HMENU32(h);
    default: return (WPARAM)HWND32(h);
    }
}
void Msg32To16(UINT msg,WPARAM wp,LPARAM lp,WORD *msg16,WORD *wp16,DWORD *lp16) {
    *msg16=(WORD)msg; *wp16=(WORD)wp; *lp16=(DWORD)lp;
    switch(msg) {
    case WM_COMMAND: case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM:
        *lp16=MAKELONG(HWND16(lp),HIWORD(wp)); break;
    case WM_HSCROLL: case WM_VSCROLL: *lp16=MAKELONG(HIWORD(wp),HWND16(lp)); break;
    case WM_MENUSELECT: case WM_MENUCHAR: *lp16=MAKELONG(HIWORD(wp),HMENU16(lp)); break;
    case WM_PARENTNOTIFY:
        if(LOWORD(wp)==WM_CREATE || LOWORD(wp)==WM_DESTROY) *lp16=MAKELONG(HWND16(lp),HIWORD(wp));
        break;
    case WM_ACTIVATEAPP: *lp16=0; break;
    default:
        if(msg>=WM_CTLCOLORMSGBOX && msg<=WM_CTLCOLORSTATIC) {
            *msg16=WM_CTLCOLOR16; *wp16=HGDI16(wp); *lp16=MAKELONG(HWND16(lp),msg-WM_CTLCOLORMSGBOX);
        } else if(handle_in_wparam(msg)) *wp16=handle16(msg,wp);
    }
}
void Msg16To32(WORD msg16,WORD wp16,DWORD lp16,UINT *msg,WPARAM *wp,LPARAM *lp) {
    *msg=msg16; *wp=wp16; *lp=(LPARAM)lp16;
    switch(msg16) {
    case WM_COMMAND: case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM:
        *wp=MAKELONG(wp16,HIWORD(lp16)); *lp=(LPARAM)HWND32(LOWORD(lp16)); break;
    case WM_HSCROLL: case WM_VSCROLL: *wp=MAKELONG(wp16,LOWORD(lp16)); *lp=(LPARAM)HWND32(HIWORD(lp16)); break;
    case WM_MENUSELECT: case WM_MENUCHAR: *wp=MAKELONG(wp16,LOWORD(lp16)); *lp=(LPARAM)HMENU32(HIWORD(lp16)); break;
    case WM_PARENTNOTIFY:
        if(wp16==WM_CREATE || wp16==WM_DESTROY) {*wp=MAKELONG(wp16,HIWORD(lp16)); *lp=(LPARAM)HWND32(LOWORD(lp16));}
        break;
    case WM_CTLCOLOR16:
        *msg=WM_CTLCOLORMSGBOX+HIWORD(lp16); *wp=(WPARAM)HGDI32(wp16); *lp=(LPARAM)HWND32(LOWORD(lp16)); break;
    default:
        if(handle_in_wparam(msg16)) *wp=handle32(msg16,wp16);
    }
}
static BOOL brush_result(UINT msg) {return msg>=WM_CTLCOLORMSGBOX && msg<=WM_CTLCOLORSTATIC;}

/* --- MDI ----------------------------------------------------------------------- */
static BOOL class_is(HWND h,LPCSTR name) {char n[16]; return h && GetClassName(h,n,sizeof(n)) && !lstrcmpi(n,name);}
static BOOL mdi_child(HWND h) {return class_is(GetParent(h),"MDICLIENT");}
/* MDICREATESTRUCT: class and title (far pointers), owner, x, y, cx, cy,
 * style, lParam. */
static void mdicreate_in(const BYTE *s,MDICREATESTRUCT *c) {
    DWORD cls=get32(s); short v[4]; int i;
    c->szClass=HIWORD(cls)?(LPCSTR)Lin16(cls):MAKEINTRESOURCE(LOWORD(cls)); c->szTitle=(LPCSTR)Lin16(get32(s+4));
    c->hOwner=get16(s+8)?INSTANCE32(NeFromHandle(get16(s+8))?NeFromHandle(get16(s+8))->handle:get16(s+8)):NULL;
    for(i=0;i<4;i++) v[i]=(short)get16(s+10+i*2);
    c->x=v[0]==(short)0x8000?CW_USEDEFAULT:v[0]; c->y=v[1]==(short)0x8000?CW_USEDEFAULT:v[1];
    c->cx=v[2]==(short)0x8000?CW_USEDEFAULT:v[2]; c->cy=v[3]==(short)0x8000?CW_USEDEFAULT:v[3];
    c->style=get32(s+18); c->lParam=(LPARAM)get32(s+22);
}
static DWORD mdicreate_out(Task16 *t,const MDICREATESTRUCT *c) {
    DWORD seg=0,cls=0,title=0; BYTE *s,*text;
    if(c->szClass && HIWORD((ULONG_PTR)c->szClass) && (text=(BYTE *)Scratch16(t,(WORD)(lstrlen(c->szClass)+1),&cls))!=NULL) lstrcpy((char *)text,c->szClass);
    else cls=LOWORD((ULONG_PTR)c->szClass);
    if(c->szTitle && (text=(BYTE *)Scratch16(t,(WORD)(lstrlen(c->szTitle)+1),&title))!=NULL) lstrcpy((char *)text,c->szTitle);
    if((s=(BYTE *)Scratch16(t,26,&seg))!=NULL) {
        put32(s,cls); put32(s+4,title); put16(s+8,(WORD)((ULONG_PTR)c->hOwner>>24==0x7f?(ULONG_PTR)c->hOwner:0));
        put16(s+10,(WORD)c->x); put16(s+12,(WORD)c->y); put16(s+14,(WORD)c->cx); put16(s+16,(WORD)c->cy);
        put32(s+18,c->style); put32(s+22,(DWORD)c->lParam);
    }
    return seg;
}

/* --- control messages ------------------------------------------------------- */
enum {CTL_NONE,CTL_EDIT,CTL_LISTBOX,CTL_COMBOBOX,CTL_BUTTON,CTL_STATIC};
static int control_class(HWND h) {
    char n[16];
    if(!h || !GetClassName(h,n,sizeof(n))) return CTL_NONE;
    if(!lstrcmpi(n,"Edit")) return CTL_EDIT;
    if(!lstrcmpi(n,"ListBox")) return CTL_LISTBOX;
    if(!lstrcmpi(n,"ComboBox")) return CTL_COMBOBOX;
    if(!lstrcmpi(n,"Button")) return CTL_BUTTON;
    if(!lstrcmpi(n,"Static")) return CTL_STATIC;
    return CTL_NONE;
}
/* A superclass of a control (its procedure from GetClassInfo) has a class
 * of its own: the control's procedure that a message goes to tells. */
static int control_of_proc(WNDPROC proc) {
    static const char *const names[5]={"Edit","ListBox","ComboBox","Button","Static"};
    static WNDPROC procs[5]; static BOOL known; int i;
    if(!known) {
        WNDCLASS wc;
        for(i=0;i<5;i++) procs[i]=GetClassInfo(NULL,names[i],&wc)?wc.lpfnWndProc:NULL;
        known=TRUE;
    }
    for(i=0;i<5;i++) if(proc && procs[i]==proc) return CTL_EDIT+i;
    return CTL_NONE;
}
/* The first native message of a class that Windows 3.0 numbers WM_USER,
 * and how many there are. */
static UINT control_base(int c) {
    switch(c) {
    case CTL_EDIT: return EM_GETSEL;
    case CTL_LISTBOX: return LB_ADDSTRING-1;
    case CTL_COMBOBOX: return CB_GETEDITSEL;
    case CTL_BUTTON: return BM_GETCHECK;
    default: return STM_SETICON;
    }
}
static UINT control_count(int c) {
    switch(c) {
    case CTL_EDIT: return EM_GETPASSWORDCHAR-EM_GETSEL+1;
    case CTL_LISTBOX: return LB_FINDSTRINGEXACT-LB_ADDSTRING+2;
    case CTL_COMBOBOX: return CB_FINDSTRINGEXACT-CB_GETEDITSEL+1;
    case CTL_BUTTON: return BM_SETSTYLE-BM_GETCHECK+1;
    case CTL_STATIC: return 2;
    default: return 0;
    }
}
#define EM_SETWORDBREAKPROC 0x00D0
#define EM_GETWORDBREAKPROC 0x00D1
/* Messages whose lParam is a string or a buffer; owner-drawn list boxes
 * and combo boxes without strings take item data instead. */
static BOOL control_string(HWND h,int c,UINT msg) {
    LONG style=GetWindowLong(h,GWL_STYLE);
    switch(msg) {
    case EM_REPLACESEL: case EM_GETLINE: case LB_GETTEXT: case LB_DIR: case CB_GETLBTEXT: case CB_DIR: return TRUE;
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING: case LB_SELECTSTRING: case LB_FINDSTRINGEXACT:
        return c==CTL_LISTBOX && (!(style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE)) || (style&LBS_HASSTRINGS));
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING: case CB_SELECTSTRING: case CB_FINDSTRINGEXACT:
        return c==CTL_COMBOBOX && (!(style&(CBS_OWNERDRAWFIXED|CBS_OWNERDRAWVARIABLE)) || (style&CBS_HASSTRINGS));
    default: return FALSE;
    }
}
static BOOL control_rect(UINT msg) {
    return msg==EM_GETRECT || msg==EM_SETRECT || msg==EM_SETRECTNP || msg==LB_GETITEMRECT || msg==CB_GETDROPPEDCONTROLRECT;
}
/* A 16-bit control message to a native procedure. */
static DWORD control_to_native(WNDPROC proc,HWND h,int c,WORD m16,WORD w16,DWORD l16) {
    UINT msg=control_base(c)+(m16-WM_USER); WPARAM wp=(WPARAM)(LONG_PTR)(short)w16; LPARAM lp=(LPARAM)l16;
    RECT r; int tabs[64]; unsigned i; LRESULT result; WORD *words=(WORD *)Lin16(l16);
    switch(msg) {
    case EM_SETSEL: wp=(WPARAM)(LONG_PTR)(short)LOWORD(l16); lp=(LPARAM)(short)HIWORD(l16); break;
    case EM_LINESCROLL: wp=(WPARAM)(LONG_PTR)(short)HIWORD(l16); lp=(LPARAM)(short)LOWORD(l16); break;
    case EM_SETTABSTOPS: case LB_SETTABSTOPS:
        for(i=0;i<w16 && i<64 && words;i++) tabs[i]=(short)words[i];
        lp=(LPARAM)(words?tabs:NULL); wp=w16<64?w16:64; break;
    case LB_GETSELITEMS: {
        int items[256]; unsigned n=w16<256?w16:256; LRESULT k;
        k=proc(h,msg,n,(LPARAM)items);
        for(i=0;i<(unsigned)k && i<n && words;i++) words[i]=(WORD)items[i];
        return (DWORD)(LONG)k;
    }
    case STM_SETICON: wp=(WPARAM)HICON32(w16); break;
    case EM_SETHANDLE: case EM_GETHANDLE: case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC: return 0;
    default:
        if(control_rect(msg)) {if(!RectIn16(SelLinear(Lin16(l16)),&r)) memset(&r,0,sizeof(r)); lp=(LPARAM)&r;}
        else if(control_string(h,c,msg)) lp=(LPARAM)Lin16(l16);
    }
    result=proc(h,msg,wp,lp);
    if(control_rect(msg)) RectOut16(SelLinear(Lin16(l16)),&r);
    if(msg==STM_SETICON || msg==STM_GETICON) return HICON16(result);
    return (DWORD)(LONG)result;
}
/* A native control message as Windows 3.0 numbers and packs it, for a
 * 16-bit procedure that subclasses the control. Strings get an alias,
 * RECTs a scratch copy. */
static BOOL control_to16(Task16 *t,HWND h,UINT msg,WPARAM wp,LPARAM lp,WORD *m16,WORD *w16,DWORD *l16,DWORD *alias,BYTE **rect16) {
    int c=control_class(h); UINT base=control_base(c);
    if(!c || msg<base || msg>=base+control_count(c)) return FALSE;
    *m16=(WORD)(WM_USER+(msg-base)); *w16=(WORD)wp; *l16=(DWORD)lp;
    if(msg==EM_SETSEL) *l16=MAKELONG((WORD)wp,(WORD)lp);
    else if(msg==EM_LINESCROLL) *l16=MAKELONG((WORD)lp,(WORD)wp);
    else if(msg==STM_SETICON) *w16=HICON16(wp);
    else if(control_rect(msg)) {
        if((*rect16=(BYTE *)Scratch16(t,8,l16))!=NULL && lp) RectOut16(SelLinear(*rect16),(const RECT *)lp);
    } else if(control_string(h,c,msg)) *l16=*alias=AliasPointer((const void *)lp);
    return TRUE;
}

/* --- native message, 16-bit procedure ------------------------------------- */
/* CREATESTRUCT: creation parameter, instance, menu, parent, cy, cx, y, x,
 * style, name and class (far pointers), extended style. */
DWORD CreateStructOut16(Task16 *t,HWND h,const CREATESTRUCT *c,BYTE **out) {
    DWORD name=0,cls=0,params=(DWORD)(ULONG_PTR)c->lpCreateParams,seg=0; BYTE *text,*s;
    if(c->lpszName && (text=(BYTE *)Scratch16(t,(WORD)(lstrlen(c->lpszName)+1),&name))!=NULL) lstrcpy((char *)text,c->lpszName);
    if(c->lpszClass && HIWORD((ULONG_PTR)c->lpszClass) && (text=(BYTE *)Scratch16(t,(WORD)(lstrlen(c->lpszClass)+1),&cls))!=NULL) lstrcpy((char *)text,c->lpszClass);
    else if(c->lpszClass) cls=LOWORD((ULONG_PTR)c->lpszClass);
    if(c->lpCreateParams && h && mdi_child(h)) params=mdicreate_out(t,(const MDICREATESTRUCT *)c->lpCreateParams);
    if((s=(BYTE *)Scratch16(t,34,&seg))!=NULL) {
        put32(s,params);
        put16(s+4,(WORD)((ULONG_PTR)c->hInstance>>24==0x7f?(ULONG_PTR)c->hInstance:0));
        put16(s+6,c->style&WS_CHILD?(WORD)(ULONG_PTR)c->hMenu:HMENU16(c->hMenu)); put16(s+8,HWND16(c->hwndParent));
        put16(s+10,(WORD)c->cy); put16(s+12,(WORD)c->cx); put16(s+14,(WORD)c->y); put16(s+16,(WORD)c->x);
        put32(s+18,(DWORD)c->style); put32(s+22,name); put32(s+26,cls); put32(s+30,c->dwExStyle);
    }
    *out=s;
    return seg;
}
LRESULT CallProc16(Task16 *t,DWORD proc,HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    static const BYTE sizes[4]={2,2,2,4};
    WORD mark=t->scratch_top,m16,w16; DWORD l16,args[4],r,alias=0; BYTE *s=NULL,*rect16=NULL; LRESULT result;
    BOOL control=control_to16(t,h,msg,wp,lp,&m16,&w16,&l16,&alias,&rect16);
    if(!control) Msg32To16(msg,wp,lp,&m16,&w16,&l16);
    if(!control) switch(msg) {
    case WM_GETMINMAXINFO: {
        const POINT *pt=(const POINT *)lp; int i;
        if((s=(BYTE *)Scratch16(t,20,&l16))!=NULL) for(i=0;i<5;i++) {put16(s+i*4,(WORD)pt[i].x); put16(s+i*4+2,(WORD)pt[i].y);}
        break;
    }
    case WM_MDIACTIVATE:
        if(!class_is(h,"MDICLIENT")) {w16=(WORD)((HWND)lp==h); l16=MAKELONG(HWND16(lp),HWND16(wp));}
        else w16=HWND16(wp);
        break;
    case WM_CREATE: case WM_NCCREATE: l16=CreateStructOut16(t,h,(const CREATESTRUCT *)lp,&s); break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d=(const DRAWITEMSTRUCT *)lp;
        if((s=(BYTE *)Scratch16(t,26,&l16))!=NULL) {
            put16(s,(WORD)d->CtlType); put16(s+2,(WORD)d->CtlID); put16(s+4,(WORD)d->itemID);
            put16(s+6,(WORD)d->itemAction); put16(s+8,(WORD)d->itemState);
            put16(s+10,d->CtlType==ODT_MENU?HMENU16(d->hwndItem):HWND16(d->hwndItem)); put16(s+12,HGDI16(d->hDC));
            RectOut16(SelLinear(s+14),&d->rcItem); put32(s+22,(DWORD)d->itemData);
        }
        break;
    }
    case WM_MEASUREITEM: {
        const MEASUREITEMSTRUCT *m=(const MEASUREITEMSTRUCT *)lp;
        if((s=(BYTE *)Scratch16(t,14,&l16))!=NULL) {
            put16(s,(WORD)m->CtlType); put16(s+2,(WORD)m->CtlID); put16(s+4,(WORD)m->itemID);
            put16(s+6,(WORD)m->itemWidth); put16(s+8,(WORD)m->itemHeight); put32(s+10,(DWORD)m->itemData);
        }
        break;
    }
    case WM_DELETEITEM: {
        const DELETEITEMSTRUCT *d=(const DELETEITEMSTRUCT *)lp;
        if((s=(BYTE *)Scratch16(t,12,&l16))!=NULL) {
            put16(s,(WORD)d->CtlType); put16(s+2,(WORD)d->CtlID); put16(s+4,(WORD)d->itemID);
            put16(s+6,HWND16(d->hwndItem)); put32(s+8,(DWORD)d->itemData);
        }
        break;
    }
    case WM_COMPAREITEM: {
        const COMPAREITEMSTRUCT *c=(const COMPAREITEMSTRUCT *)lp;
        if((s=(BYTE *)Scratch16(t,18,&l16))!=NULL) {
            put16(s,(WORD)c->CtlType); put16(s+2,(WORD)c->CtlID); put16(s+4,HWND16(c->hwndItem));
            put16(s+6,(WORD)c->itemID1); put32(s+8,(DWORD)c->itemData1); put16(s+12,(WORD)c->itemID2); put32(s+14,(DWORD)c->itemData2);
        }
        break;
    }
    case WM_SETTEXT: case WM_GETTEXT: l16=alias=AliasPointer((const void *)lp); break;
    default:
        if(msg>=0xc000) {DWORD fr=FindReplace16(lp); if(fr) l16=fr;}
        break;
    }
    args[0]=HWND16(h); args[1]=m16; args[2]=w16; args[3]=l16;
    r=Call16(t,proc,4,args,sizes);
    result=(LRESULT)(LONG)r;
    if(control) {
        if(rect16 && lp) RectIn16(SelLinear(rect16),(RECT *)lp);
        if(msg==STM_SETICON || msg==STM_GETICON) result=(LRESULT)HICON32(LOWORD(r));
        AliasFree(alias); t->scratch_top=mark;
        return result;
    }
    switch(msg) {
    case WM_GETMINMAXINFO:
        if(s) {POINT *pt=(POINT *)lp; int i; for(i=0;i<5;i++) {pt[i].x=(short)get16(s+i*4); pt[i].y=(short)get16(s+i*4+2);}}
        break;
    case WM_MEASUREITEM:
        if(s) {MEASUREITEMSTRUCT *m=(MEASUREITEMSTRUCT *)lp; m->itemWidth=get16(s+6); m->itemHeight=get16(s+8);}
        break;
    default: break;
    }
    if(msg==WM_COMPAREITEM) result=(LRESULT)(short)LOWORD(r);
    else if(brush_result(msg)) result=(LRESULT)HGDI32(LOWORD(r));
    else if(msg==WM_GETTEXT || msg==WM_GETTEXTLENGTH) result=(LRESULT)(WORD)r;
    AliasFree(alias);
    t->scratch_top=mark;
    return result;
}

/* --- 16-bit message, native procedure --------------------------------------- */
DWORD CallNative16(Task16 *t,WNDPROC proc,WORD h16,WORD m16,WORD w16,DWORD l16) {
    UINT msg; WPARAM wp; LPARAM lp; LRESULT r; BYTE *s=(BYTE *)Lin16(l16);
    union {POINT pt[5]; CREATESTRUCT cs;} u; MDICREATESTRUCT mdi;
    (void)t;
    if(m16>=WM_USER && m16<WM_USER+0x40) {
        int c=control_class(HWND32(h16));
        if(!c) c=control_of_proc(proc);
        if(c && m16<WM_USER+control_count(c)) return control_to_native(proc,HWND32(h16),c,m16,w16,l16);
    }
    Msg16To32(m16,w16,l16,&msg,&wp,&lp);
    switch(msg) {
    case WM_MDICREATE: {
        MDICREATESTRUCT c;
        if(!s) return 0;
        mdicreate_in(s,&c);
        return HWND16(proc(HWND32(h16),msg,0,(LPARAM)&c));
    }
    case WM_MDIGETACTIVE: {
        BOOL maximized=FALSE; HWND active=(HWND)proc(HWND32(h16),msg,0,(LPARAM)&maximized);
        return MAKELONG(HWND16(active),maximized?1:0);
    }
    case WM_MDISETMENU: return HMENU16(proc(HWND32(h16),msg,(WPARAM)HMENU32(LOWORD(l16)),(LPARAM)HMENU32(HIWORD(l16))));
    case WM_MDIACTIVATE:
        if(class_is(HWND32(h16),"MDICLIENT")) wp=(WPARAM)HWND32(w16);
        else {wp=(WPARAM)HWND32(HIWORD(l16)); lp=(LPARAM)HWND32(LOWORD(l16));}
        break;
    case WM_GETMINMAXINFO:
        if(s) {int i; for(i=0;i<5;i++) {u.pt[i].x=(short)get16(s+i*4); u.pt[i].y=(short)get16(s+i*4+2);} lp=(LPARAM)u.pt;}
        break;
    case WM_CREATE: case WM_NCCREATE:
        if(s) {
            u.cs.lpCreateParams=(LPVOID)(ULONG_PTR)get32(s);
            if(u.cs.lpCreateParams && mdi_child(HWND32(h16)) && Lin16(get32(s))) {mdicreate_in((const BYTE *)Lin16(get32(s)),&mdi); u.cs.lpCreateParams=&mdi;}
            u.cs.hInstance=get16(s+4)?INSTANCE32(get16(s+4)):NULL;
            u.cs.style=(LONG)get32(s+18);
            u.cs.hMenu=u.cs.style&WS_CHILD?(HMENU)(ULONG_PTR)get16(s+6):HMENU32(get16(s+6));
            u.cs.hwndParent=HWND32(get16(s+8));
            u.cs.cy=(short)get16(s+10); u.cs.cx=(short)get16(s+12); u.cs.y=(short)get16(s+14); u.cs.x=(short)get16(s+16);
            u.cs.lpszName=(LPCSTR)Lin16(get32(s+22)); u.cs.lpszClass=(LPCSTR)Lin16(get32(s+26)); u.cs.dwExStyle=get32(s+30);
            lp=(LPARAM)&u.cs;
        }
        break;
    case WM_SETTEXT: case WM_GETTEXT: lp=(LPARAM)s; break;
    default: break;
    }
    r=proc(HWND32(h16),msg,wp,lp);
    switch(msg) {
    case WM_GETMINMAXINFO: if(s) {int i; for(i=0;i<5;i++) {put16(s+i*4,(WORD)u.pt[i].x); put16(s+i*4+2,(WORD)u.pt[i].y);}} break;
    default: break;
    }
    if(brush_result(msg) || msg==WM_GETFONT) return HGDI16(r);
    if(msg==WM_QUERYDRAGICON) return HICON16(r);
    return (DWORD)r;
}

/* --- native procedures for 16-bit windows and dialogs ------------------------ */
LRESULT WINAPI WowWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Task16 *t=CurrentTask16(); DWORD proc=WindowProc16(h); LRESULT r;
    if(!t || !proc) r=DefWindowProc(h,msg,wp,lp);
    else r=CallProc16(t,proc,h,msg,wp,lp);
    if(msg==WM_NCDESTROY) WindowDestroyed16(h);
    return r;
}
#define DIALOGS 32
static struct {HWND h; DWORD proc;} dialogs[DIALOGS];
DWORD DialogProc16(HWND h) {
    unsigned i;
    if(h) for(i=0;i<DIALOGS;i++) if(dialogs[i].h==h) return dialogs[i].proc;
    return 0;
}
BOOL SetDialogProc16(HWND h,DWORD proc) {
    unsigned i;
    if(h) for(i=0;i<DIALOGS;i++) if(dialogs[i].h==h) {dialogs[i].proc=proc; return TRUE;}
    return FALSE;
}
INT_PTR WINAPI WowDlgProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Task16 *t=CurrentTask16(); DWORD proc=0; unsigned i,free_slot=DIALOGS; LRESULT r;
    for(i=0;i<DIALOGS && !proc;i++) {if(dialogs[i].h==h) proc=dialogs[i].proc; else if(!dialogs[i].h && free_slot==DIALOGS) free_slot=i;}
    if(!proc && pending_dialog_proc && free_slot<DIALOGS) {
        proc=pending_dialog_proc; pending_dialog_proc=0;
        dialogs[free_slot].h=h; dialogs[free_slot].proc=proc;
    }
    if(!t || !proc) return FALSE;
    if(msg==WM_INITDIALOG) DialogBound16(h);
    r=CallProc16(t,proc,h,msg,wp,lp);
    if(msg==WM_NCDESTROY) for(i=0;i<DIALOGS;i++) if(dialogs[i].h==h) dialogs[i].h=NULL;
    return brush_result(msg)?(INT_PTR)r:(INT_PTR)(WORD)r;
}
