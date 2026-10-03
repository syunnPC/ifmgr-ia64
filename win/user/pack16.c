/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows 3.0 message packing for native WIN16_MESSAGES programs.
 * RegisterClass16, SetWindowsHook16 and related entry points mark procedures
 * for Win16-packed calls. SendMessage16, CallWindowProc16, DefWindowProc16,
 * CallNextHookEx16 and related functions unpack forwarded messages. The
 * message loop also uses a packed MSG (GetMessage16/DispatchMessage16).
 *
 * Conversions cover command/activation/scroll/menu/parent notifications,
 * WM_CHARTOITEM/VKEYTOITEM, WM_CTLCOLORxxx, MDI activation/menus/results,
 * EM_SETSEL/EM_LINESCROLL and DDE. DDE uses two lParam WORDs instead of a
 * native block; WM_DDE_EXECUTE puts commands in the high WORD. Handles
 * are already 16-bit and structures use these headers. Message numbers
 * remain Win32's.
 */
#include "user.h"
#include <dde.h>
#define WM_CTLCOLOR16 0x0019

/* --- marked procedures ------------------------------------------------------- */
#define MARKS 1024
static const void *marks[MARKS];
static unsigned slot_of(const void *p) {ULONG_PTR v=(ULONG_PTR)p; return (unsigned)((v>>4)^(v>>14))%MARKS;}
static void MarkWin16Proc(const void *proc) {
    unsigned i,k;
    if(!proc) return;
    for(k=0,i=slot_of(proc);k<MARKS;k++,i=(i+1)%MARKS) {
        if(marks[i]==proc) return;
        if(!marks[i]) {marks[i]=proc; return;}
    }
}
static BOOL marked(const void *proc) {
    unsigned i,k;
    if(!proc) return FALSE;
    for(k=0,i=slot_of(proc);k<MARKS && marks[i];k++,i=(i+1)%MARKS) if(marks[i]==proc) return TRUE;
    return FALSE;
}

/* --- packing ------------------------------------------------------------------ */
static BOOL mdi_client(HWND h) {Wnd *w=WndFromHandle(h); return w && !lstrcmpi(w->cls->name,"MDICLIENT");}
static WORD handle16(LPARAM lp) {return (WORD)(ULONG_PTR)lp;}
static LPARAM handle32(WORD h) {return (LPARAM)(ULONG_PTR)h;}
/* A message as Windows 3.0 packs it, for h's procedure. A popup's item in
 * WM_MENUSELECT is its position natively and its handle in Windows 3.0.
 * TRUE: lParam was a DDE block, for the receiver to free. */
static BOOL pack(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT *m,WPARAM *w,LPARAM *l) {
    *m=msg; *w=wp; *l=lp;
    switch(msg) {
    case WM_DDE_ACK: case WM_DDE_ADVISE: case WM_DDE_DATA: case WM_DDE_POKE: {
        UINT_PTR lo,hi;
        if(!DDEBlock(lp) || !UnpackDDElParam(msg,lp,&lo,&hi)) return FALSE;
        *l=MAKELONG((WORD)lo,(WORD)hi); return TRUE;
    }
    case WM_DDE_EXECUTE: *l=MAKELONG(0,handle16(lp)); return FALSE;
    case WM_COMMAND: case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM:
        *w=LOWORD(wp); *l=MAKELONG(handle16(lp),HIWORD(wp)); return FALSE;
    case WM_HSCROLL: case WM_VSCROLL: *w=LOWORD(wp); *l=MAKELONG(HIWORD(wp),handle16(lp)); return FALSE;
    case WM_MENUSELECT: {
        UINT item=LOWORD(wp),flags=HIWORD(wp); HMENU sub;
        if(flags!=0xffff && (flags&MF_POPUP) && lp && (sub=GetSubMenu((HMENU)lp,(int)item))!=NULL) item=(UINT)(ULONG_PTR)sub;
        *w=item; *l=MAKELONG(flags,handle16(lp)); return FALSE;
    }
    case WM_MENUCHAR: *w=LOWORD(wp); *l=MAKELONG(HIWORD(wp),handle16(lp)); return FALSE;
    case WM_PARENTNOTIFY:
        *w=LOWORD(wp);
        if(LOWORD(wp)==WM_CREATE || LOWORD(wp)==WM_DESTROY) *l=MAKELONG(handle16(lp),HIWORD(wp));
        return FALSE;
    case WM_MDIACTIVATE:
        /* A child hears whether it is the one activated, then both windows. */
        if(!mdi_client(h)) {*w=(WPARAM)((HWND)lp==h); *l=MAKELONG(handle16(lp),(WORD)wp);}
        return FALSE;
    case WM_MDISETMENU: *w=0; *l=MAKELONG((WORD)wp,handle16(lp)); return FALSE;
    case EM_SETSEL: *w=0; *l=MAKELONG((WORD)wp,(WORD)lp); return FALSE;
    case EM_LINESCROLL: *w=0; *l=MAKELONG((WORD)lp,(WORD)wp); return FALSE;
    }
    if(msg>=WM_CTLCOLORMSGBOX && msg<=WM_CTLCOLORSTATIC) {*m=WM_CTLCOLOR16; *l=MAKELONG(handle16(lp),msg-WM_CTLCOLORMSGBOX);}
    return FALSE;
}
/* A message packed as Windows 3.0 packs it, for a native procedure. With
 * blocks, DDE's two words become a block for the receiver to free (not for
 * the default procedures, which leave DDE alone); TRUE when one was made. */
static BOOL unpack(HWND h,UINT m16,WPARAM w16,LPARAM l16,UINT *m,WPARAM *w,LPARAM *l,BOOL blocks) {
    WORD lo=LOWORD(l16),hi=HIWORD(l16),wlo=(WORD)w16;
    *m=m16; *w=w16; *l=l16;
    switch(m16) {
    case WM_DDE_ACK:
        /* WM_DDE_INITIATE's acknowledgement names an application and a
         * topic, string atoms from 0xC000; a status word is never as high
         * (fAck and fBusy together). */
        if(lo>=0xc000) return FALSE;
        /* fall through */
    case WM_DDE_ADVISE: case WM_DDE_DATA: case WM_DDE_POKE:
        if(!blocks) return FALSE;
        *l=PackDDElParam(m16,lo,hi); return *l!=0;
    case WM_DDE_EXECUTE: *l=handle32(hi); return FALSE;
    case WM_COMMAND: case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM:
        *w=MAKEWPARAM(wlo,hi); *l=handle32(lo); return FALSE;
    case WM_HSCROLL: case WM_VSCROLL: *w=MAKEWPARAM(wlo,lo); *l=handle32(hi); return FALSE;
    case WM_MENUSELECT: {
        UINT item=wlo; int i,n;
        if(lo!=0xffff && (lo&MF_POPUP) && hi)
            for(i=0,n=GetMenuItemCount((HMENU)handle32(hi));i<n;i++) if((ULONG_PTR)GetSubMenu((HMENU)handle32(hi),i)==item) {item=(UINT)i; break;}
        *w=MAKEWPARAM(item,lo); *l=handle32(hi); return FALSE;
    }
    case WM_MENUCHAR: *w=MAKEWPARAM(wlo,lo); *l=handle32(hi); return FALSE;
    case WM_PARENTNOTIFY:
        if(wlo==WM_CREATE || wlo==WM_DESTROY) {*w=MAKEWPARAM(wlo,hi); *l=handle32(lo);}
        return FALSE;
    case WM_MDIACTIVATE:
        if(!mdi_client(h)) {*w=(WPARAM)handle32(hi); *l=handle32(lo);}
        return FALSE;
    case WM_MDISETMENU: *w=(WPARAM)handle32(lo); *l=handle32(hi); return FALSE;
    case EM_SETSEL: *w=(WPARAM)(LONG_PTR)(short)lo; *l=(LPARAM)(short)hi; return FALSE;
    case EM_LINESCROLL: *w=(WPARAM)(LONG_PTR)(short)hi; *l=(LPARAM)(short)lo; return FALSE;
    case WM_CTLCOLOR16: *m=WM_CTLCOLORMSGBOX+hi; *l=handle32(lo); return FALSE;
    }
    return FALSE;
}
/* A native result as Windows 3.0 gives it: WM_MDIGETACTIVE's high word says
 * whether the window is maximized. */
static LRESULT result16(UINT msg,LRESULT r) {
    if(msg==WM_MDIGETACTIVE) return (LRESULT)MAKELONG((WORD)r,r && IsZoomed((HWND)r)?1:0);
    return r;
}

/* --- calls ------------------------------------------------------------------------ */
/* A window or dialog procedure called with a message: a marked one gets it
 * packed as Windows 3.0 packs it, and its result, a LONG, is widened. A
 * DDE block it got as two words is freed for it. */
LRESULT CallProc(WNDPROC proc,HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    UINT m; WPARAM w; LPARAM l; BOOL block; LRESULT r;
    if(!proc) return 0;
    if(!marked((const void *)proc)) return proc(h,msg,wp,lp);
    block=pack(h,msg,wp,lp,&m,&w,&l);
    r=(LRESULT)(LONG)proc(h,m,w,l);
    if(block) FreeDDElParam(msg,lp);
    return r;
}
ATOM WINAPI RegisterClass16(const WNDCLASS FAR *wc) {if(wc) MarkWin16Proc((const void *)wc->lpfnWndProc); return RegisterClass(wc);}
LONG_PTR WINAPI SetWindowLong16(HWND h,int index,LONG_PTR value) {
    if(index==GWLP_WNDPROC || index==DWLP_DLGPROC) MarkWin16Proc((const void *)value);
    return SetWindowLongPtr(h,index,value);
}
ULONG_PTR WINAPI SetClassLong16(HWND h,int index,LONG_PTR value) {
    if(index==GCLP_WNDPROC) MarkWin16Proc((const void *)value);
    return SetClassLongPtr(h,index,value);
}
/* GWW_HINSTANCE and GCW_HMODULE are module bases, wider than a WORD. */
ULONG_PTR WINAPI GetWindowWord16(HWND h,int index) {return index<0 && index!=GWW_ID?(ULONG_PTR)GetWindowLongPtr(h,index):GetWindowWord(h,index);}
ULONG_PTR WINAPI GetClassWord16(HWND h,int index) {return index<0?GetClassLongPtr(h,index):GetClassWord(h,index);}
INT_PTR WINAPI DialogBoxParam16(HINSTANCE i,LPCSTR name,HWND owner,DLGPROC proc,LPARAM param) {
    MarkWin16Proc((const void *)proc); return DialogBoxParam(i,name,owner,proc,param);
}
INT_PTR WINAPI DialogBoxIndirectParam16(HINSTANCE i,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc,LPARAM param) {
    MarkWin16Proc((const void *)proc); return DialogBoxIndirectParam(i,t,owner,proc,param);
}
HWND WINAPI CreateDialogParam16(HINSTANCE i,LPCSTR name,HWND owner,DLGPROC proc,LPARAM param) {
    MarkWin16Proc((const void *)proc); return CreateDialogParam(i,name,owner,proc,param);
}
HWND WINAPI CreateDialogIndirectParam16(HINSTANCE i,LPCDLGTEMPLATE t,HWND owner,DLGPROC proc,LPARAM param) {
    MarkWin16Proc((const void *)proc); return CreateDialogIndirectParam(i,t,owner,proc,param);
}
/* A window as a Win16 source gives it: -1 for every top-level window, as
 * (HWND)0xffff is. */
static HWND window16(HWND h) {return (HWND)(ULONG_PTR)(WORD)(ULONG_PTR)h;}
LRESULT WINAPI SendMessage16(HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    h=window16(h);
    if(unpack(h,m16,w16,l16,&m,&w,&l,TRUE) && !IsWindow(h)) {FreeDDElParam(m,l); return 0;}
    return result16(m,SendMessage(h,m,w,l));
}
BOOL WINAPI PostMessage16(HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l; BOOL block;
    h=window16(h);
    block=unpack(h,m16,w16,l16,&m,&w,&l,TRUE);
    if(PostMessage(h,m,w,l)) return TRUE;
    if(block) FreeDDElParam(m,l);
    return FALSE;
}
LRESULT WINAPI SendDlgItemMessage16(HWND dlg,int id,UINT m16,WPARAM w16,LPARAM l16) {
    HWND h=GetDlgItem(dlg,id);
    return h?SendMessage16(h,m16,w16,l16):0;
}
LRESULT WINAPI CallWindowProc16(WNDPROC proc,HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    unpack(h,m16,w16,l16,&m,&w,&l,TRUE);
    return result16(m,CallWindowProc(proc,h,m,w,l));
}
LRESULT WINAPI DefWindowProc16(HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    unpack(h,m16,w16,l16,&m,&w,&l,FALSE);
    return DefWindowProc(h,m,w,l);
}
LRESULT WINAPI DefDlgProc16(HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    unpack(h,m16,w16,l16,&m,&w,&l,FALSE);
    return DefDlgProc(h,m,w,l);
}
LRESULT WINAPI DefFrameProc16(HWND h,HWND client,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    unpack(h,m16,w16,l16,&m,&w,&l,FALSE);
    return DefFrameProc(h,client,m,w,l);
}
LRESULT WINAPI DefMDIChildProc16(HWND h,UINT m16,WPARAM w16,LPARAM l16) {
    UINT m; WPARAM w; LPARAM l;
    unpack(h,m16,w16,l16,&m,&w,&l,FALSE);
    return DefMDIChildProc(h,m,w,l);
}

/* --- hooks -------------------------------------------------------------------------- */
/* A marked hook procedure gets messages in Windows 3.0's packing: in the MSG
 * of WH_GETMESSAGE, WH_MSGFILTER and WH_SYSMSGFILTER, which it may change,
 * and in WH_CALLWNDPROC's CWPSTRUCT. The other hooks' data (keys, the
 * mouse, journal events, CBT's structures) is the same in both. A DDE block
 * stays its receiver's: a hook only looks at the message. */
static BOOL msg_hook(int id) {return id==WH_GETMESSAGE || id==WH_MSGFILTER || id==WH_SYSMSGFILTER;}
LRESULT CallHookProc(HOOKPROC proc,int id,int code,WPARAM wp,LPARAM lp) {
    if(!proc) return 0;
    if(!marked((const void *)proc)) return proc(code,wp,lp);
    if(lp && msg_hook(id)) {
        MSG *msg=(MSG *)lp,m16,seen,was=*msg; LRESULT r; BOOL block;
        m16=*msg;
        block=pack(msg->hwnd,msg->message,msg->wParam,msg->lParam,&m16.message,&m16.wParam,&m16.lParam);
        seen=m16;
        r=(LRESULT)(LONG)proc(code,wp,(LPARAM)&m16);
        if(memcmp(&m16,&seen,sizeof(MSG))) {
            *msg=m16;
            unpack(m16.hwnd,m16.message,m16.wParam,m16.lParam,&msg->message,&msg->wParam,&msg->lParam,TRUE);
            if(block) FreeDDElParam(was.message,was.lParam);
        }
        return r;
    }
    if(lp && id==WH_CALLWNDPROC) {
        const CWPSTRUCT *c=(const CWPSTRUCT *)lp; CWPSTRUCT c16=*c;
        pack(c->hwnd,c->message,c->wParam,c->lParam,&c16.message,&c16.wParam,&c16.lParam);
        return (LRESULT)(LONG)proc(code,wp,(LPARAM)&c16);
    }
    return (LRESULT)(LONG)proc(code,wp,lp);
}
/* A packed MSG for native code, which may change it: native_msg unpacks it
 * (TRUE when it made a DDE block), msg_back packs what changed back and
 * frees the block, which no one else consumes. */
static BOOL native_msg(const MSG *m16,MSG *m) {
    *m=*m16;
    return unpack(m16->hwnd,m16->message,m16->wParam,m16->lParam,&m->message,&m->wParam,&m->lParam,TRUE);
}
static void msg_back(MSG *m16,const MSG *m,const MSG *seen,BOOL block) {
    if(memcmp(m,seen,sizeof(MSG))) {*m16=*m; pack(m->hwnd,m->message,m->wParam,m->lParam,&m16->message,&m16->wParam,&m16->lParam);}
    if(block) FreeDDElParam(seen->message,seen->lParam);
}
/* What a marked hook passes on: its message unpacked for the next hook,
 * and what that one changes packed again. */
static LRESULT next_hook16(HHOOK hh,int code,WPARAM wp,LPARAM lp) {
    int id=HookKind(hh); LRESULT r; BOOL block;
    if(lp && msg_hook(id)) {
        MSG m,seen;
        block=native_msg((const MSG *)lp,&m); seen=m;
        r=CallNextHookEx(hh,code,wp,(LPARAM)&m);
        msg_back((MSG *)lp,&m,&seen,block);
        return r;
    }
    if(lp && id==WH_CALLWNDPROC) {
        const CWPSTRUCT *c16=(const CWPSTRUCT *)lp; CWPSTRUCT c=*c16;
        block=unpack(c16->hwnd,c16->message,c16->wParam,c16->lParam,&c.message,&c.wParam,&c.lParam,TRUE);
        r=CallNextHookEx(hh,code,wp,(LPARAM)&c);
        if(block) FreeDDElParam(c.message,c.lParam);
        return r;
    }
    return CallNextHookEx(hh,code,wp,lp);
}
HOOKPROC WINAPI SetWindowsHook16(int id,HOOKPROC proc) {MarkWin16Proc((const void *)proc); return SetWindowsHook(id,proc);}
HHOOK WINAPI SetWindowsHookEx16(int id,HOOKPROC proc,HINSTANCE module,DWORD task) {
    MarkWin16Proc((const void *)proc); return SetWindowsHookEx(id,proc,module,task);
}
LRESULT WINAPI CallNextHookEx16(HHOOK hh,int code,WPARAM wp,LPARAM lp) {return next_hook16(hh,code,wp,lp);}
LRESULT WINAPI DefHookProc16(int code,WPARAM wp,LPARAM lp,HOOKPROC FAR *next) {return next?next_hook16((HHOOK)*next,code,wp,lp):0;}

/* --- the message loop --------------------------------------------------------------- */
/* What GetMessage16 and PeekMessage16 give is packed as Windows 3.0 packs
 * it; a DDE block in a message taken from the queue is freed as its words
 * go out, and DispatchMessage16 makes one again for the receiver.
 * TranslateMessage, TranslateAccelerator and TranslateMDISysAccel read
 * keys only, which are the same in both. */
static void retrieved(MSG *m,BOOL removed) {
    MSG native=*m;
    if(pack(native.hwnd,native.message,native.wParam,native.lParam,&m->message,&m->wParam,&m->lParam) && removed) FreeDDElParam(native.message,native.lParam);
}
BOOL WINAPI GetMessage16(LPMSG m,HWND h,UINT low,UINT high) {
    BOOL r=GetMessage(m,h,low,high);
    if(m) retrieved(m,TRUE);
    return r;
}
BOOL WINAPI PeekMessage16(LPMSG m,HWND h,UINT low,UINT high,UINT flags) {
    BOOL r=PeekMessage(m,h,low,high,flags);
    if(r && m) retrieved(m,(flags&PM_REMOVE)!=0);
    return r;
}
LRESULT WINAPI DispatchMessage16(const MSG FAR *m16) {
    MSG m;
    if(!m16) return 0;
    if(native_msg(m16,&m) && !IsWindow(m.hwnd)) {FreeDDElParam(m.message,m.lParam); return 0;}
    return DispatchMessage(&m);
}
/* A dialog's message: one for another window is left alone, so that no
 * block is made that nothing frees. */
BOOL WINAPI IsDialogMessage16(HWND dlg,LPMSG m16) {
    MSG m; BOOL block,r;
    if(!m16 || !dlg || (m16->hwnd!=dlg && !IsChild(dlg,m16->hwnd))) return FALSE;
    block=native_msg(m16,&m);
    r=IsDialogMessage(dlg,&m);
    if(!r && block) FreeDDElParam(m.message,m.lParam);
    return r;
}
/* The filter hooks see the message natively (a marked one packed again);
 * what they change is packed back. */
BOOL WINAPI CallMsgFilter16(LPMSG m16,int code) {
    MSG m,seen; BOOL block,r;
    if(!m16) return FALSE;
    block=native_msg(m16,&m); seen=m;
    r=CallMsgFilter(&m,code);
    msg_back(m16,&m,&seen,block);
    return r;
}
