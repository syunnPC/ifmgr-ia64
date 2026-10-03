/* SPDX-License-Identifier: GPL-2.0-or-later
 * The dialog manager: dialogs from DIALOG and DIALOGEX templates (rc.exe's
 * Win32 format), modal and modeless; DefDlgProc; keyboard navigation in
 * IsDialogMessage; the GetDlgItem family; MessageBox; GDI's Print To File
 * prompt and spooler notice; DlgDirList.
 * Dialog units are a quarter of the font's average width and an eighth of
 * its height. The dialog window's extra bytes: DWLP_MSGRESULT, DWLP_DLGPROC,
 * DWLP_USER, then the result, state and the control that last had focus.
 */
#include "user.h"
#define DWLP_RESULT 24
#define DWLP_STATE 32    /* bit 0 ended, bit 1 in a modal loop, bit 2 focus given; high word: default button id */
#define DWLP_FOCUS 40
#define ENDED 1
#define MODAL 2
#define FOCUSED 4 /* the first activation gave a control the focus */

static LONG_PTR dlg_get(HWND h,int index) {return GetWindowLongPtr(h,index);}
static void dlg_set(HWND h,int index,LONG_PTR v) {SetWindowLongPtr(h,index,v);}
static WORD def_id(HWND h) {WORD id=(WORD)(dlg_get(h,DWLP_STATE)>>16); return id?id:IDOK;}

/* --- templates ------------------------------------------------------------------ */
typedef struct {
    DWORD style,exstyle; int count; int x,y,cx,cy;
    LPCSTR menu,cls; char menu_name[64],class_name[64],title[256];
    int point; char face[LF_FACESIZE]; WORD weight; BYTE italic; BOOL ex;
} Header;
typedef struct {DWORD style,exstyle; int x,y,cx,cy; WORD id; LPCSTR cls; char class_name[64]; LPCSTR text; char title[256]; const void *data;} Item;
/* A string or an ordinal (0xffff, id); empty is NULL. */
static const WORD *sz_or_ord(const WORD *p,LPCSTR *out,char *buf,int size) {
    if(*p==0) {*out=NULL; return p+1;}
    if(*p==0xffff) {*out=MAKEINTRESOURCE(p[1]); return p+2;}
    WideToAnsi(p,buf,size); *out=buf;
    return SkipWide(p);
}
static const WORD *parse_header(const void *t,Header *h) {
    const WORD *p=(const WORD *)t; LPCSTR title;
    memset(h,0,sizeof(*h));
    if(p[0]==1 && p[1]==0xffff) {
        h->ex=TRUE;
        h->exstyle=*(const DWORD *)(p+4); h->style=*(const DWORD *)(p+6);
        h->count=p[8]; h->x=(short)p[9]; h->y=(short)p[10]; h->cx=(short)p[11]; h->cy=(short)p[12];
        p+=13;
    } else {
        h->style=*(const DWORD *)p; h->exstyle=*(const DWORD *)(p+2);
        h->count=p[4]; h->x=(short)p[5]; h->y=(short)p[6]; h->cx=(short)p[7]; h->cy=(short)p[8];
        p+=9;
    }
    p=sz_or_ord(p,&h->menu,h->menu_name,sizeof(h->menu_name));
    p=sz_or_ord(p,&h->cls,h->class_name,sizeof(h->class_name));
    p=sz_or_ord(p,&title,h->title,sizeof(h->title));
    if(!title) h->title[0]=0;
    if(h->style&DS_SETFONT) {
        h->point=*p++;
        if(h->ex) {h->weight=*p++; h->italic=(BYTE)*p; p++;}
        WideToAnsi(p,h->face,sizeof(h->face)); p=SkipWide(p);
    }
    return p;
}
static const WORD *align4(const WORD *p,const void *base) {
    ULONG_PTR off=(ULONG_PTR)((const BYTE *)p-(const BYTE *)base);
    return (const WORD *)((const BYTE *)base+((off+3)&~(ULONG_PTR)3));
}
static const WORD *parse_item(const WORD *p,const void *base,BOOL ex,Item *it) {
    static const char *const atoms[]={"BUTTON","EDIT","STATIC","LISTBOX","SCROLLBAR","COMBOBOX"};
    WORD extra;
    memset(it,0,sizeof(*it));
    p=align4(p,base);
    if(ex) {
        it->exstyle=*(const DWORD *)(p+2); it->style=*(const DWORD *)(p+4);
        it->x=(short)p[6]; it->y=(short)p[7]; it->cx=(short)p[8]; it->cy=(short)p[9]; it->id=p[10]; p+=12;
    } else {
        it->style=*(const DWORD *)p; it->exstyle=*(const DWORD *)(p+2);
        it->x=(short)p[4]; it->y=(short)p[5]; it->cx=(short)p[6]; it->cy=(short)p[7]; it->id=p[8]; p+=9;
    }
    p=sz_or_ord(p,&it->cls,it->class_name,sizeof(it->class_name));
    if(it->cls && IS_INTRESOURCE(it->cls) && (ULONG_PTR)it->cls>=0x80 && (ULONG_PTR)it->cls<=0x85) it->cls=atoms[(ULONG_PTR)it->cls-0x80];
    p=sz_or_ord(p,&it->text,it->title,sizeof(it->title));
    extra=*p++;
    if(extra) it->data=p;
    return (const WORD *)((const BYTE *)p+extra);
}

/* --- dialogs ------------------------------------------------------------------------ */
static int base_x(HWND h) {
    HFONT f=(HFONT)GetProp(h,"#font"); HDC dc; TEXTMETRIC tm;
    if(!f) return CharWidth();
    dc=CreateCompatibleDC(NULL); SelectObject(dc,f); GetTextMetrics(dc,&tm); tm.tmAveCharWidth=LetterWidth(dc); DeleteDC(dc);
    return (int)tm.tmAveCharWidth;
}
static int base_y(HWND h) {
    HFONT f=(HFONT)GetProp(h,"#font"); HDC dc; TEXTMETRIC tm;
    if(!f) return CharHeight();
    dc=CreateCompatibleDC(NULL); SelectObject(dc,f); GetTextMetrics(dc,&tm); DeleteDC(dc);
    return (int)tm.tmHeight;
}
void WINAPI MapDialogRect(HWND h,LPRECT r) {
    int bx=base_x(h),by=base_y(h);
    if(!r) return;
    r->left=r->left*bx/4; r->right=r->right*bx/4; r->top=r->top*by/8; r->bottom=r->bottom*by/8;
}
LONG WINAPI GetDialogBaseUnits(void) {return MAKELONG(CharWidth(),CharHeight());}
static BOOL tab_stop(Wnd *c) {return (c->style&(WS_TABSTOP|WS_VISIBLE))==(WS_TABSTOP|WS_VISIBLE) && Enabled(c);}
HWND WINAPI GetNextDlgTabItem(HWND dlg,HWND from,BOOL previous) {
    Wnd *d=WndFromHandle(dlg),*c,*list[256]; int n=0,i,at=-1;
    if(!d) return NULL;
    for(c=d->child;c && n<256;c=c->next) {if(c->handle==from) at=n; list[n++]=c;}
    if(!n) return NULL;
    for(i=1;i<=n;i++) {
        int k=at<0?(previous?n-i:i-1):((at+(previous?-i:i))%n+n)%n;
        if(tab_stop(list[k])) return list[k]->handle;
    }
    return from;
}
HWND WINAPI GetNextDlgGroupItem(HWND dlg,HWND from,BOOL previous) {
    Wnd *d=WndFromHandle(dlg),*c,*list[256]; int n=0,i,at=-1,start,end;
    if(!d) return NULL;
    for(c=d->child;c && n<256;c=c->next) {if(c->handle==from) at=n; list[n++]=c;}
    if(at<0) return from;
    for(start=at;start>0 && !(list[start]->style&WS_GROUP);start--);
    for(end=at+1;end<n && !(list[end]->style&WS_GROUP);end++);
    for(i=1;i<end-start;i++) {
        int k=start+((at-start+(previous?-i:i))%(end-start)+(end-start))%(end-start);
        if((list[k]->style&WS_VISIBLE) && Enabled(list[k])) return list[k]->handle;
    }
    return from;
}
HWND WINAPI GetDlgItem(HWND dlg,int id) {
    Wnd *d=WndFromHandle(dlg),*c;
    if(!d) return NULL;
    for(c=d->child;c;c=c->next) if((int)(WORD)c->id==(int)(WORD)id) return c->handle;
    return NULL;
}
LRESULT WINAPI SendDlgItemMessage(HWND dlg,int id,UINT msg,WPARAM wp,LPARAM lp) {
    HWND h=GetDlgItem(dlg,id);
    return h?SendMessage(h,msg,wp,lp):0;
}
void WINAPI SetDlgItemText(HWND dlg,int id,LPCSTR text) {SendDlgItemMessage(dlg,id,WM_SETTEXT,0,(LPARAM)text);}
int WINAPI GetDlgItemText(HWND dlg,int id,LPSTR out,int size) {
    if(!out || size<=0) return 0;
    out[0]=0;
    return (int)SendDlgItemMessage(dlg,id,WM_GETTEXT,(WPARAM)size,(LPARAM)out);
}
void WINAPI SetDlgItemInt(HWND dlg,int id,UINT value,BOOL is_signed) {
    char text[16]; wsprintf(text,is_signed?"%d":"%u",value); SetDlgItemText(dlg,id,text);
}
UINT WINAPI GetDlgItemInt(HWND dlg,int id,BOOL FAR *ok,BOOL is_signed) {
    char text[32]; const char *p=text; BOOL neg=FALSE; DWORD v=0; BOOL digits=FALSE;
    if(ok) *ok=FALSE;
    GetDlgItemText(dlg,id,text,sizeof(text));
    while(*p==' ') p++;
    if(is_signed && *p=='-') {neg=TRUE; p++;}
    while(*p>='0' && *p<='9') {v=v*10+(DWORD)(*p++-'0'); digits=TRUE; if(v>0x7fffffffU && is_signed) return 0;}
    while(*p==' ') p++;
    if(!digits || *p) return 0;
    if(ok) *ok=TRUE;
    return neg?(UINT)(0-(int)v):v;
}
void WINAPI CheckDlgButton(HWND dlg,int id,UINT check) {SendDlgItemMessage(dlg,id,BM_SETCHECK,check,0);}
UINT WINAPI IsDlgButtonChecked(HWND dlg,int id) {return (UINT)SendDlgItemMessage(dlg,id,BM_GETCHECK,0,0);}
void WINAPI CheckRadioButton(HWND dlg,int first,int last,int check) {
    int id;
    for(id=first;id<=last;id++) CheckDlgButton(dlg,id,id==check?BST_CHECKED:BST_UNCHECKED);
}
/* The default push button follows the focus among push buttons. */
static void set_default(HWND dlg,HWND to) {
    Wnd *d=WndFromHandle(dlg),*c; HWND target=to;
    if(!d) return;
    if(target) {
        LRESULT code=SendMessage(target,WM_GETDLGCODE,0,0);
        if(!(code&(DLGC_DEFPUSHBUTTON|DLGC_UNDEFPUSHBUTTON))) target=NULL;
    }
    if(!target) target=GetDlgItem(dlg,def_id(dlg));
    for(c=d->child;c;c=c->next) {
        LRESULT code=SendMessage(c->handle,WM_GETDLGCODE,0,0);
        if(c->handle==target && (code&DLGC_UNDEFPUSHBUTTON)) SendMessage(c->handle,BM_SETSTYLE,BS_DEFPUSHBUTTON,TRUE);
        else if(c->handle!=target && (code&DLGC_DEFPUSHBUTTON)) SendMessage(c->handle,BM_SETSTYLE,BS_PUSHBUTTON,TRUE);
    }
}
static void focus_control(HWND dlg,HWND to) {
    if(!to) return;
    SetFocus(to);
    if(SendMessage(to,WM_GETDLGCODE,0,0)&DLGC_HASSETSEL) SendMessage(to,EM_SETSEL,0,-1);
    set_default(dlg,to);
}
static HWND create_dialog(HINSTANCE instance,const void *t,HWND owner,DLGPROC proc,LPARAM param) {
    Header h; Item it; const WORD *p=parse_header(t,&h); HWND dlg,first=NULL; RECT r; HFONT font=NULL;
    Wnd *o=WndFromHandle(owner); int i,bx=CharWidth(),by=CharHeight(); LPCSTR cls=h.cls?h.cls:"#32770";
    DWORD style=h.style&~WS_VISIBLE,exstyle=h.exstyle;
    if(h.style&DS_SETFONT) {
        HDC dc; TEXTMETRIC tm;
        font=CreateFont(-(h.point*96+36)/72,0,0,0,h.weight?h.weight:FW_BOLD,h.italic,0,0,ANSI_CHARSET,0,0,0,0,h.face);
        dc=CreateCompatibleDC(NULL); SelectObject(dc,font); GetTextMetrics(dc,&tm); bx=LetterWidth(dc); DeleteDC(dc);
        by=(int)tm.tmHeight;
    }
    if(style&DS_MODALFRAME) exstyle|=WS_EX_DLGMODALFRAME;
    SetRect(&r,0,0,h.cx*bx/4,h.cy*by/8);
    AdjustWindowRectEx(&r,style,h.menu!=NULL,exstyle);
    {
        int x=h.x*bx/4+r.left,y=h.y*by/8+r.top;
        if(!(style&WS_CHILD) && o && !(style&DS_ABSALIGN)) {x+=o->client.left; y+=o->client.top;}
        if(!(style&WS_CHILD)) {
            if(x+r.right-r.left>screen_width) x=screen_width-(r.right-r.left);
            if(y+r.bottom-r.top>screen_height) y=screen_height-(r.bottom-r.top);
            if(x<0) x=0;
            if(y<0) y=0;
        }
        dlg=CreateWindowEx(exstyle,cls,h.title,style,x,y,r.right-r.left,r.bottom-r.top,owner,
                           h.menu?LoadMenu(instance,h.menu):NULL,instance,NULL);
    }
    if(!dlg) {if(font) DeleteObject(font); return NULL;}
    if(font) SetProp(dlg,"#font",font);
    dlg_set(dlg,DWLP_DLGPROC,(LONG_PTR)proc);
    for(i=0;i<h.count;i++) {
        HWND c;
        p=parse_item(p,t,h.ex,&it);
        c=CreateWindowEx(it.exstyle|WS_EX_NOPARENTNOTIFY,it.cls,it.text,it.style|WS_CHILD,it.x*bx/4,it.y*by/8,it.cx*bx/4,it.cy*by/8,
                         dlg,(HMENU)(ULONG_PTR)it.id,instance,(void *)it.data);
        if(!c) continue;
        if(font) SendMessage(c,WM_SETFONT,(WPARAM)font,FALSE);
        if((it.style&0x0f)==BS_DEFPUSHBUTTON && it.cls && !IS_INTRESOURCE(it.cls) && !lstrcmpi(it.cls,"BUTTON"))
            dlg_set(dlg,DWLP_STATE,(dlg_get(dlg,DWLP_STATE)&0xffff)|((LONG_PTR)it.id<<16));
    }
    first=GetNextDlgTabItem(dlg,NULL,FALSE);
    if(SendMessage(dlg,WM_INITDIALOG,(WPARAM)first,param) && first) {dlg_set(dlg,DWLP_FOCUS,(LONG_PTR)first); }
    else if(GetFocus() && IsChild(dlg,GetFocus())) dlg_set(dlg,DWLP_FOCUS,(LONG_PTR)GetFocus());
    if(!IsWindow(dlg)) return NULL;
    if(h.style&WS_VISIBLE) {ShowWindow(dlg,SW_SHOWNORMAL); UpdateWindow(dlg);}
    return dlg;
}
HWND WINAPI CreateDialogIndirectParam(HINSTANCE instance,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc,LPARAM param) {
    return t?create_dialog(instance,t,owner,proc,param):NULL;
}
HWND WINAPI CreateDialogParam(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC proc,LPARAM param) {
    DWORD size; const void *t=Resource(instance,RT_DIALOG,name,&size);
    return t?create_dialog(instance,t,owner,proc,param):NULL;
}
HWND WINAPI CreateDialog(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC proc) {return CreateDialogParam(instance,name,owner,proc,0);}
HWND WINAPI CreateDialogIndirect(HINSTANCE instance,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc) {return CreateDialogIndirectParam(instance,t,owner,proc,0);}
/* Run a dialog modally: its owner is disabled until EndDialog. The message
 * filter hooks see each message first (filter: MSGF_DIALOGBOX or
 * MSGF_MESSAGEBOX). */
static INT_PTR modal(HWND dlg,HWND owner,int filter) {
    MSG m; INT_PTR result; BOOL reenable=FALSE; Wnd *o=WndFromHandle(owner);
    if(!dlg) return -1;
    if(o) {o=TopLevel(o); owner=o->handle;}
    if(owner && IsWindowEnabled(owner)) {EnableWindow(owner,FALSE); reenable=TRUE;}
    dlg_set(dlg,DWLP_STATE,dlg_get(dlg,DWLP_STATE)|MODAL);
    if(!(dlg_get(dlg,DWLP_STATE)&ENDED)) {ShowWindow(dlg,SW_SHOWNORMAL); UpdateWindow(dlg);}
    while(IsWindow(dlg) && !(dlg_get(dlg,DWLP_STATE)&ENDED)) {
        if(!GetMessage(&m,NULL,0,0)) {PostQuitMessage((int)m.wParam); break;}
        if(CallMsgFilter(&m,filter)) continue;
        if(!IsDialogMessage(dlg,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    }
    result=IsWindow(dlg)?(INT_PTR)dlg_get(dlg,DWLP_RESULT):-1;
    if(reenable) EnableWindow(owner,TRUE);
    if(IsWindow(dlg)) {
        if(owner && GetActiveWindow()==dlg) SetActiveWindow(owner);
        DestroyWindow(dlg);
    }
    return result;
}
INT_PTR WINAPI DialogBoxIndirectParam(HINSTANCE instance,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc,LPARAM param) {
    return t?modal(create_dialog(instance,t,owner,proc,param),owner,MSGF_DIALOGBOX):-1;
}
INT_PTR WINAPI DialogBoxParam(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC proc,LPARAM param) {
    DWORD size; const void *t=Resource(instance,RT_DIALOG,name,&size);
    return t?modal(create_dialog(instance,t,owner,proc,param),owner,MSGF_DIALOGBOX):-1;
}
INT_PTR WINAPI DialogBox(HINSTANCE instance,LPCSTR name,HWND owner,DLGPROC proc) {return DialogBoxParam(instance,name,owner,proc,0);}
INT_PTR WINAPI DialogBoxIndirect(HINSTANCE instance,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc) {return DialogBoxIndirectParam(instance,t,owner,proc,0);}
/* A dialog EndDialog has ended, which goes when its loop next looks. */
BOOL DialogEnded(HWND dlg) {return WndFromHandle(dlg) && (dlg_get(dlg,DWLP_STATE)&ENDED);}
BOOL WINAPI EndDialog(HWND dlg,INT_PTR result) {
    if(!WndFromHandle(dlg)) return FALSE;
    dlg_set(dlg,DWLP_RESULT,result);
    dlg_set(dlg,DWLP_STATE,dlg_get(dlg,DWLP_STATE)|ENDED);
    if(!(dlg_get(dlg,DWLP_STATE)&MODAL)) ShowWindow(dlg,SW_HIDE);
    PostMessage(dlg,WM_NULL,0,0);
    return TRUE;
}
static BOOL result_message(UINT msg) {
    return (msg>=WM_CTLCOLORMSGBOX && msg<=WM_CTLCOLORSTATIC) || msg==WM_COMPAREITEM || msg==WM_VKEYTOITEM ||
           msg==WM_CHARTOITEM || msg==WM_QUERYDRAGICON || msg==WM_INITDIALOG;
}
LRESULT WINAPI DefDlgProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    DLGPROC proc=(DLGPROC)dlg_get(h,DWLP_DLGPROC);
    if(proc) {
        INT_PTR r;
        dlg_set(h,DWLP_MSGRESULT,0);
        r=(INT_PTR)CallProc((WNDPROC)proc,h,msg,wp,lp);
        if(!IsWindow(h)) return r;
        if(r) return result_message(msg)?r:dlg_get(h,DWLP_MSGRESULT);
    }
    switch(msg) {
    case WM_ERASEBKGND: {
        RECT r; HBRUSH b=(HBRUSH)SendMessage(h,WM_CTLCOLORDLG,wp,(LPARAM)h);
        GetClientRect(h,&r); FillRect((HDC)wp,&r,b?b:SysBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_ACTIVATE:
        if(LOWORD(wp)==WA_INACTIVE) {HWND f=GetFocus(); if(f && IsChild(h,f)) dlg_set(h,DWLP_FOCUS,(LONG_PTR)f);}
        else if(!HIWORD(wp)) {
            /* The first activation focuses a control as tabbing to it would (an edit
             * control's text selected); later ones give the focus back as it was. */
            HWND f=(HWND)dlg_get(h,DWLP_FOCUS); LONG_PTR state=dlg_get(h,DWLP_STATE);
            if(!f || !IsWindow(f) || !IsChild(h,f)) f=GetNextDlgTabItem(h,NULL,FALSE);
            if(!f) SetFocus(h);
            else if(state&FOCUSED) SetFocus(f);
            else {dlg_set(h,DWLP_STATE,state|FOCUSED); focus_control(h,f);}
        }
        return 0;
    case WM_SETFOCUS: {
        HWND f=(HWND)dlg_get(h,DWLP_FOCUS);
        if(!f || !IsWindow(f)) f=GetNextDlgTabItem(h,NULL,FALSE);
        if(f) focus_control(h,f);
        return 0;
    }
    case WM_CLOSE: {
        HWND c=GetDlgItem(h,IDCANCEL);
        if(!c || IsWindowEnabled(c)) PostMessage(h,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),(LPARAM)c);
        return 0;
    }
    case WM_NEXTDLGCTL: {
        HWND to=LOWORD(lp)?(HWND)wp:GetNextDlgTabItem(h,GetFocus(),wp!=0);
        focus_control(h,to);
        return 0;
    }
    case DM_GETDEFID: return MAKELONG(def_id(h),DC_HASDEFID);
    case DM_SETDEFID: dlg_set(h,DWLP_STATE,(dlg_get(h,DWLP_STATE)&0xffff)|((LONG_PTR)(WORD)wp<<16)); set_default(h,GetFocus()); return TRUE;
    case WM_GETFONT: return (LRESULT)GetProp(h,"#font");
    case WM_NCDESTROY: {HFONT f=(HFONT)RemoveProp(h,"#font"); if(f) DeleteObject(f); break;}
    case WM_SHOWWINDOW: case WM_PAINT: case WM_INITDIALOG: break;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return (LRESULT)ControlColor(WndFromHandle((HWND)lp),(HDC)wp,msg);
    }
    return DefWindowProc(h,msg,wp,lp);
}
/* The control whose mnemonic is ch: a static label selects the next control. */
static HWND mnemonic_target(HWND dlg,WPARAM ch,BOOL *click) {
    Wnd *d=WndFromHandle(dlg),*c; char text[256]; BYTE want=(BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)ch);
    *click=FALSE;
    if(!d) return NULL;
    for(c=d->child;c;c=c->next) {
        char m;
        if(!(c->style&WS_VISIBLE) || !Enabled(c)) continue;
        if(!lstrcmpi(c->cls->name,"EDIT") || !lstrcmpi(c->cls->name,"LISTBOX") || !lstrcmpi(c->cls->name,"COMBOBOX") || !lstrcmpi(c->cls->name,"SCROLLBAR")) continue;
        if(!lstrcmpi(c->cls->name,"STATIC") && (c->style&SS_NOPREFIX)) continue;
        GetWindowText(c->handle,text,sizeof(text));
        m=PrefixChar(text);
        if(!m || (BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)m)!=want) continue;
        if(!lstrcmpi(c->cls->name,"STATIC")) {
            Wnd *n; for(n=c->next;n;n=n->next) if((n->style&WS_VISIBLE) && Enabled(n) && lstrcmpi(n->cls->name,"STATIC")) return n->handle;
            return NULL;
        }
        if(!lstrcmpi(c->cls->name,"BUTTON") && (c->style&0x0f)!=BS_GROUPBOX) *click=TRUE;
        if(!lstrcmpi(c->cls->name,"BUTTON") && (c->style&0x0f)==BS_GROUPBOX) {
            Wnd *n; for(n=c->next;n;n=n->next) if(tab_stop(n)) return n->handle;
            return NULL;
        }
        return c->handle;
    }
    return NULL;
}
BOOL WINAPI IsDialogMessage(HWND dlg,LPMSG m) {
    HWND f; LRESULT code=0;
    if(!m || !dlg || !IsWindow(dlg) || (m->hwnd!=dlg && !IsChild(dlg,m->hwnd))) return FALSE;
    f=GetFocus();
    if(m->message==WM_KEYDOWN || m->message==WM_CHAR || m->message==WM_SYSCHAR)
        code=m->hwnd?SendMessage(m->hwnd,WM_GETDLGCODE,m->wParam,(LPARAM)m):0;
    switch(m->message) {
    case WM_KEYDOWN:
        if(code&DLGC_WANTALLKEYS) break;
        switch(m->wParam) {
        case VK_TAB:
            if(code&DLGC_WANTTAB) break;
            focus_control(dlg,GetNextDlgTabItem(dlg,f,KeyDown(VK_SHIFT)));
            return TRUE;
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: {
            HWND to;
            if(code&DLGC_WANTARROWS) break;
            to=GetNextDlgGroupItem(dlg,f,m->wParam==VK_LEFT || m->wParam==VK_UP);
            if(to && to!=f) {
                focus_control(dlg,to);
                if(SendMessage(to,WM_GETDLGCODE,0,0)&DLGC_RADIOBUTTON) SendMessage(to,WM_LBUTTONDOWN,0,0),SendMessage(to,WM_LBUTTONUP,0,0);
            }
            return TRUE;
        }
        case VK_RETURN: {
            WORD id=def_id(dlg); HWND b;
            if(f && (SendMessage(f,WM_GETDLGCODE,0,0)&DLGC_DEFPUSHBUTTON)) id=(WORD)GetDlgCtrlID(f);
            b=GetDlgItem(dlg,id);
            if(!b || IsWindowEnabled(b)) SendMessage(dlg,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),(LPARAM)b);
            return TRUE;
        }
        case VK_ESCAPE: {
            HWND b=GetDlgItem(dlg,IDCANCEL);
            if(!b || IsWindowEnabled(b)) SendMessage(dlg,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),(LPARAM)b);
            return TRUE;
        }
        }
        break;
    case WM_CHAR:
        if(code&(DLGC_WANTCHARS|DLGC_WANTALLKEYS)) break;
        if(m->wParam=='\t' || m->wParam=='\r' || m->wParam==0x1b) return TRUE;
        /* fall through: a plain letter is a mnemonic outside edit controls */
    case WM_SYSCHAR: {
        BOOL click; HWND to=mnemonic_target(dlg,m->wParam,&click);
        if(!to) {if(m->message==WM_SYSCHAR) break; if(m->wParam>' ') MessageBeep(0); return TRUE;}
        focus_control(dlg,to);
        if(click) {SendMessage(to,WM_LBUTTONDOWN,0,0); SendMessage(to,WM_LBUTTONUP,0,0);}
        return TRUE;
    }
    }
    TranslateMessage(m); DispatchMessage(m);
    return TRUE;
}
LRESULT SendNotify(Wnd *w,UINT code) {
    if(!w->parent || w->parent==desktop) return 0;
    return SendMessage(w->parent->handle,WM_COMMAND,MAKEWPARAM(w->id,code),(LPARAM)w->handle);
}
static LRESULT CALLBACK DialogClassProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefDlgProc(h,msg,wp,lp);}

/* --- MessageBox ---------------------------------------------------------------------------- */
static INT_PTR CALLBACK message_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) return TRUE;
    if(msg==WM_COMMAND && HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=IDOK && LOWORD(wp)<=IDNO) {EndDialog(h,LOWORD(wp)); return TRUE;}
    return FALSE;
}
int WINAPI MessageBox(HWND owner,LPCSTR text,LPCSTR caption,UINT type) {
    static const char *const names[]={"OK","Cancel","&Abort","&Retry","&Ignore","&Yes","&No"};
    static const int sets[6][4]={{IDOK,0},{IDOK,IDCANCEL,0},{IDABORT,IDRETRY,IDIGNORE,0},{IDYES,IDNO,IDCANCEL,0},{IDYES,IDNO,0},{IDRETRY,IDCANCEL,0}};
    const int *buttons=sets[min(type&MB_TYPEMASK,5)]; int nb=0,i,bw,bh,tw,th,w,ht,x,icon_w,def;
    RECT r; HDC dc; HWND dlg,first=NULL; HICON icon=NULL; DWORD style;
    LPCSTR icon_id=NULL;
    switch(type&MB_ICONMASK) {
    case MB_ICONHAND: icon_id=IDI_HAND; break;
    case MB_ICONQUESTION: icon_id=IDI_QUESTION; break;
    case MB_ICONEXCLAMATION: icon_id=IDI_EXCLAMATION; break;
    case MB_ICONASTERISK: icon_id=IDI_ASTERISK; break;
    }
    if(icon_id) icon=LoadIcon(NULL,icon_id);
    if(!text) text="";
    while(buttons[nb]) nb++;
    dc=CreateCompatibleDC(NULL); SelectObject(dc,SystemFont());
    SetRect(&r,0,0,screen_width*2/3,0);
    DrawText(dc,text,-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EXPANDTABS);
    DeleteDC(dc);
    tw=r.right; th=r.bottom;
    icon_w=icon?ICON+CharWidth()*2:0;
    bw=CharWidth()*9; bh=CharHeight()*7/4;
    w=max(icon_w+tw+CharWidth()*4,nb*bw+(nb+1)*CharWidth()*2);
    w=max(w,lstrlen(caption?caption:"Error")*CharWidth()+BOXSIZE*2);
    ht=max(th,icon?ICON:0)+CharHeight()*2+bh+CharHeight();
    style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    SetRect(&r,0,0,w,ht); AdjustWindowRectEx(&r,style,FALSE,WS_EX_DLGMODALFRAME);
    dlg=CreateWindowEx(WS_EX_DLGMODALFRAME,"#32770",caption?caption:"Error",style,
                       (screen_width-(r.right-r.left))/2,(screen_height-(r.bottom-r.top))/3,r.right-r.left,r.bottom-r.top,owner,NULL,user_instance,NULL);
    if(!dlg) return 0;
    dlg_set(dlg,DWLP_DLGPROC,(LONG_PTR)message_proc);
    if(icon) {
        HWND s=CreateWindowEx(0,"STATIC",NULL,WS_CHILD|WS_VISIBLE|SS_ICON,CharWidth()*2,CharHeight(),ICON,ICON,dlg,(HMENU)(ULONG_PTR)0xffff,user_instance,NULL);
        SendMessage(s,STM_SETICON,(WPARAM)icon,0);
    }
    CreateWindowEx(0,"STATIC",text,WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,icon_w+CharWidth()*2,CharHeight()+(icon && th<ICON?(ICON-th)/2:0),tw+CharWidth(),th,dlg,(HMENU)(ULONG_PTR)0xffff,user_instance,NULL);
    def=(int)((type&MB_DEFMASK)>>8); if(def>=nb) def=0;
    x=(w-(nb*bw+(nb-1)*CharWidth()*2))/2;
    for(i=0;i<nb;i++) {
        HWND b=CreateWindowEx(0,"BUTTON",names[buttons[i]-1],WS_CHILD|WS_VISIBLE|WS_TABSTOP|(i==def?BS_DEFPUSHBUTTON:BS_PUSHBUTTON)|(i==0?WS_GROUP:0),
                              x+i*(bw+CharWidth()*2),ht-bh-CharHeight(),bw,bh,dlg,(HMENU)(ULONG_PTR)buttons[i],user_instance,NULL);
        if(i==def) {first=b; dlg_set(dlg,DWLP_STATE,(LONG_PTR)buttons[i]<<16);}
    }
    dlg_set(dlg,DWLP_FOCUS,(LONG_PTR)first);
    MessageBeep(type&MB_ICONMASK);
    return (int)modal(dlg,owner,MSGF_MESSAGEBOX);
}

/* --- Print To File ------------------------------------------------------------------------ */
/* GDI asks for a file name when a document is printed to FILE:. */
#define PRINT_FILE_NAME 0x100
static LPSTR print_file; static int print_file_size;
static INT_PTR CALLBACK print_file_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_COMMAND && HIWORD(wp)==BN_CLICKED && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)) {
        if(LOWORD(wp)==IDOK) GetDlgItemText(h,PRINT_FILE_NAME,print_file,print_file_size);
        EndDialog(h,LOWORD(wp)); return TRUE;
    }
    return FALSE;
}
static BOOL CALLBACK print_to_file(LPSTR out,int size) {
    int cw=CharWidth(),ch=CharHeight(),w=cw*44,ht=ch*6,bw=cw*9,bh=ch*7/4; RECT r; HWND dlg,edit,owner=GetActiveWindow();
    DWORD style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    SetRect(&r,0,0,w,ht); AdjustWindowRectEx(&r,style,FALSE,WS_EX_DLGMODALFRAME);
    dlg=CreateWindowEx(WS_EX_DLGMODALFRAME,"#32770","Print To File",style,(screen_width-(r.right-r.left))/2,
                       (screen_height-(r.bottom-r.top))/3,r.right-r.left,r.bottom-r.top,owner,NULL,user_instance,NULL);
    if(!dlg) return FALSE;
    dlg_set(dlg,DWLP_DLGPROC,(LONG_PTR)print_file_proc);
    CreateWindowEx(0,"STATIC","&Output File Name:",WS_CHILD|WS_VISIBLE|SS_LEFT,cw*2,ch,cw*24,ch,dlg,(HMENU)(ULONG_PTR)0xffff,user_instance,NULL);
    edit=CreateWindowEx(0,"EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|WS_GROUP|ES_AUTOHSCROLL,cw*2,ch*5/2,cw*28,ch*3/2,
                        dlg,(HMENU)(ULONG_PTR)PRINT_FILE_NAME,user_instance,NULL);
    CreateWindowEx(0,"BUTTON","OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_GROUP|BS_DEFPUSHBUTTON,w-bw-cw*2,ch,bw,bh,dlg,(HMENU)IDOK,user_instance,NULL);
    CreateWindowEx(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,w-bw-cw*2,ch+bh+ch/2,bw,bh,dlg,(HMENU)IDCANCEL,user_instance,NULL);
    dlg_set(dlg,DWLP_FOCUS,(LONG_PTR)edit); dlg_set(dlg,DWLP_STATE,(LONG_PTR)IDOK<<16);
    print_file=out; print_file_size=size; out[0]=0;
    return modal(dlg,owner,MSGF_DIALOGBOX)==IDOK && out[0];
}

/* A spooled document waits: Print Manager is told, or started for the spooler
 * (it then ends when its queue is empty). */
static BOOL CALLBACK spool_notify(void) {
    HWND pm=FindWindow("PrintManager",NULL);
    if(pm) {PostMessage(pm,WM_SPOOLERSTATUS,0,0); return TRUE;}
    return WinExec("PRINTMAN.EXE /spool",SW_SHOWMINNOACTIVE)>=32;
}

/* --- directory lists ---------------------------------------------------------------------- */
/* Split "dir\spec" into a directory to change to and the spec left in path. */
static void split_spec(LPSTR path,char *dir,char *spec) {
    int i,cut=-1,n=lstrlen(path);
    for(i=0;i<n;i++) if(path[i]=='\\' || path[i]==':') cut=i;
    if(cut<0) {dir[0]=0; lstrcpy(spec,path);}
    else {
        lstrcpyn(dir,path,cut+2); if(path[cut]=='\\' && cut>0 && path[cut-1]!=':') dir[cut]=0;
        lstrcpy(spec,path+cut+1);
    }
    if(!spec[0]) lstrcpy(spec,"*.*");
    /* A bare directory name is a directory, not a spec. */
    if(!strchr(spec,'*') && !strchr(spec,'?')) {
        DWORD a; char full[MAX_PATH]; lstrcpy(full,dir); if(dir[0] && dir[lstrlen(dir)-1]!='\\' && dir[lstrlen(dir)-1]!=':') lstrcat(full,"\\"); lstrcat(full,spec);
        a=GetFileAttributes(full);
        if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY)) {lstrcpy(dir,full); lstrcpy(spec,"*.*");}
    }
}
static int dir_list(HWND dlg,LPSTR path,int list,int label,UINT attrs,BOOL combo) {
    char dir[MAX_PATH],spec[MAX_PATH],cwd[MAX_PATH];
    if(path && path[0]) {
        split_spec(path,dir,spec);
        if(dir[0] && !SetCurrentDirectory(dir)) return FALSE;
        lstrcpy(path,spec);
    } else lstrcpy(spec,"*.*");
    if(list) {
        HWND l=GetDlgItem(dlg,list);
        if(!l) return FALSE;
        SendMessage(l,combo?CB_RESETCONTENT:LB_RESETCONTENT,0,0);
        SendMessage(l,combo?CB_DIR:LB_DIR,attrs&~DDL_POSTMSGS,(LPARAM)spec);
    }
    if(label) {GetCurrentDirectory(sizeof(cwd),cwd); AnsiLower(cwd); SetDlgItemText(dlg,label,cwd);}
    return TRUE;
}
int WINAPI DlgDirList(HWND dlg,LPSTR path,int list,int label,UINT attrs) {return dir_list(dlg,path,list,label,attrs,FALSE);}
int WINAPI DlgDirListComboBox(HWND dlg,LPSTR path,int list,int label,UINT attrs) {return dir_list(dlg,path,list,label,attrs,TRUE);}
static BOOL dir_select(HWND dlg,LPSTR out,int list,BOOL combo) {
    char item[MAX_PATH]; HWND l=GetDlgItem(dlg,list); LRESULT sel; int n;
    if(!l || !out) return FALSE;
    sel=SendMessage(l,combo?CB_GETCURSEL:LB_GETCURSEL,0,0);
    out[0]=0;
    if(sel<0) return FALSE;
    SendMessage(l,combo?CB_GETLBTEXT:LB_GETTEXT,(WPARAM)sel,(LPARAM)item);
    n=lstrlen(item);
    if(n>4 && item[0]=='[' && item[1]=='-' && item[n-2]=='-' && item[n-1]==']') {out[0]=item[2]; out[1]=':'; out[2]=0; return TRUE;}
    if(n>2 && item[0]=='[' && item[n-1]==']') {lstrcpyn(out,item+1,n-1); lstrcat(out,"\\"); return TRUE;}
    lstrcpy(out,item);
    if(!strchr(out,'.')) lstrcat(out,".");
    return FALSE;
}
BOOL WINAPI DlgDirSelect(HWND dlg,LPSTR out,int list) {return dir_select(dlg,out,list,FALSE);}
BOOL WINAPI DlgDirSelectComboBox(HWND dlg,LPSTR out,int list) {return dir_select(dlg,out,list,TRUE);}

void DialogInit(void) {
    GdiSetFilePrompt(print_to_file); GdiSetSpoolNotify(spool_notify);
    RegisterSystemClass("#32770",DialogClassProc,CS_SAVEBITS,DLGWINDOWEXTRA,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("BUTTON",ButtonProc,CS_DBLCLKS|CS_PARENTDC,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("STATIC",StaticProc,CS_PARENTDC,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("EDIT",EditProc,CS_DBLCLKS|CS_PARENTDC,16,StockCursor(IDC_IBEAM),0);
    RegisterSystemClass("LISTBOX",ListBoxProc,CS_DBLCLKS|CS_PARENTDC,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("COMBOBOX",ComboBoxProc,CS_DBLCLKS|CS_PARENTDC,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("ComboLBox",ComboListProc,CS_DBLCLKS|CS_SAVEBITS,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("SCROLLBAR",ScrollBarProc,CS_DBLCLKS|CS_PARENTDC,16,StockCursor(IDC_ARROW),0);
    RegisterSystemClass("MDICLIENT",MDIClientProc,0,16,StockCursor(IDC_ARROW),COLOR_APPWORKSPACE+1);
}
