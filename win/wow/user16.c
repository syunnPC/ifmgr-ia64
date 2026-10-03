/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: USER functions for Win16 programs (their ordinals and argument
 * kinds, Windows 3.0's USER.EXE's, are in api16.txt).
 *
 * A 16-bit window class is a native class whose procedure is WowWndProc;
 * the table here keeps its 16-bit procedure and menu. A program's own
 * resources are read from its NE file and converted: menus and dialogs to
 * the native (Win32) templates, accelerators to native tables, icons and
 * cursors through CreateIconFromResource.
 */
#include "api.h"
#include <dde.h>

#define RT_BITMAP16 MAKEINTRESOURCE(2)
#define RT_ICON16 MAKEINTRESOURCE(3)
#define RT_MENU16 MAKEINTRESOURCE(4)
#define RT_DIALOG16 MAKEINTRESOURCE(5)
#define RT_ACCELERATOR16 MAKEINTRESOURCE(9)
#define RT_GROUP_CURSOR16 MAKEINTRESOURCE(12)
#define RT_CURSOR16 MAKEINTRESOURCE(1)
#define RT_GROUP_ICON16 MAKEINTRESOURCE(14)

static Module16 *module_of(WORD h) {Module16 *m=NeFromHandle(h); return m?m:CurrentTask16()->module;}
static HINSTANCE instance_of(WORD h) {return INSTANCE32(module_of(h)->handle);}

/* --- classes ---------------------------------------------------------------- */
#define CLASSES 64
typedef struct {BOOL used; char name[64]; DWORD proc; LPCSTR menu; char menu_name[64]; Module16 *module;} Class16;
static Class16 classes[CLASSES];
static Class16 *class_named(LPCSTR name) {
    unsigned i;
    if(!name || !HIWORD((ULONG_PTR)name)) return NULL;
    for(i=0;i<CLASSES;i++) if(classes[i].used && !lstrcmpi(classes[i].name,name)) return &classes[i];
    return NULL;
}
DWORD ClassProc16(HWND h) {
    char name[64]; Class16 *c;
    if(!GetClassName(h,name,sizeof(name))) return 0;
    c=class_named(name);
    return c?c->proc:0;
}
static Class16 *class_of(HWND h) {
    char name[64];
    return GetClassName(h,name,sizeof(name))?class_named(name):NULL;
}

/* --- window procedures and long values -------------------------------------- */
/* A window whose procedure a Win16 program set (subclassing) keeps it here,
 * and gets WowWndProc as its native procedure. A native procedure reaches
 * 16-bit code as an entry of the procedure thunk segment (NativeProc16). */
#define SUBCLASSES 128
static struct {HWND h; DWORD proc;} subclasses[SUBCLASSES];
DWORD WindowProc16(HWND h) {
    unsigned i;
    for(i=0;i<SUBCLASSES;i++) if(subclasses[i].h==h) return subclasses[i].proc;
    return ClassProc16(h);
}
void WindowDestroyed16(HWND h) {
    unsigned i;
    for(i=0;i<SUBCLASSES;i++) if(subclasses[i].h==h) subclasses[i].h=NULL;
}
static DWORD window_proc16(HWND h) {
    WNDPROC p=(WNDPROC)GetWindowLongPtr(h,GWL_WNDPROC);
    return p==WowWndProc?WindowProc16(h):NativeProc16(p);
}
static DWORD set_window_proc16(HWND h,DWORD proc) {
    DWORD old=window_proc16(h); WNDPROC native=NativeProcOf(proc); unsigned i,free_slot=SUBCLASSES;
    if(!IsWindow(h) || !proc) return 0;
    for(i=0;i<SUBCLASSES;i++) {if(subclasses[i].h==h) break; if(!subclasses[i].h && free_slot==SUBCLASSES) free_slot=i;}
    if(native) {
        if(i<SUBCLASSES) subclasses[i].h=NULL;
        SetWindowLongPtr(h,GWL_WNDPROC,(LONG_PTR)native);
        return old;
    }
    if(i==SUBCLASSES) i=free_slot;
    if(i==SUBCLASSES) return 0;
    subclasses[i].h=h; subclasses[i].proc=proc;
    SetWindowLongPtr(h,GWL_WNDPROC,(LONG_PTR)WowWndProc);
    return old;
}
/* Windows 3.0 indexes: GWL_WNDPROC -4, GWW_HINSTANCE -6, GWW_HWNDPARENT -8,
 * GWW_ID -12, GWL_STYLE -16, GWL_EXSTYLE -20; a dialog's DWL_MSGRESULT 0,
 * DWL_DLGPROC 4 and DWL_USER 8 (the native ones are pointer-sized). */
#define GWW_HINSTANCE16 (-6)
#define GWW_HWNDPARENT16 (-8)
static BOOL dialog_index(HWND h,int i,int *native) {
    if(i<0 || i>8 || i%4 || !DialogProc16(h)) return FALSE;
    *native=i==0?DWLP_MSGRESULT:i==4?DWLP_DLGPROC:DWLP_USER;
    return TRUE;
}
DWORD W16_GetWindowLong(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1],native;
    if(i==GWL_WNDPROC) return window_proc16(h);
    if(dialog_index(h,i,&native)) return native==DWLP_DLGPROC?DialogProc16(h):(DWORD)GetWindowLongPtr(h,native);
    return (DWORD)GetWindowLong(h,i);
}
DWORD W16_SetWindowLong(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1],native; DWORD v=a->a[2],old;
    if(i==GWL_WNDPROC) return set_window_proc16(h,v);
    if(dialog_index(h,i,&native)) {
        if(native!=DWLP_DLGPROC) return (DWORD)SetWindowLongPtr(h,native,(LONG_PTR)(LONG)v);
        old=DialogProc16(h); SetDialogProc16(h,v); return old;
    }
    return (DWORD)SetWindowLong(h,i,(LONG)v);
}
static WORD instance16(HINSTANCE h) {
    Module16 *m=(ULONG_PTR)h>>24==0x7f?NeFromHandle((WORD)(ULONG_PTR)h):NULL;
    return m?m->instance:0;
}
DWORD W16_GetWindowWord(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1];
    if(i==GWW_HINSTANCE16) return instance16((HINSTANCE)GetWindowLongPtr(h,GWL_HINSTANCE));
    if(i==GWW_HWNDPARENT16) return HWND16(GetParent(h));
    return GetWindowWord(h,i);
}
DWORD W16_SetWindowWord(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1];
    if(i==GWW_HINSTANCE16 || i==GWW_HWNDPARENT16) return W16_GetWindowWord(a);
    return SetWindowWord(h,i,(WORD)a->a[2]);
}
/* Class indexes: GCL_MENUNAME -8, GCW_HBRBACKGROUND -10, GCW_HCURSOR -12,
 * GCW_HICON -14, GCW_HMODULE -16, GCW_CBWNDEXTRA -18, GCW_CBCLSEXTRA -20,
 * GCL_WNDPROC -24, GCW_STYLE -26. */
static WORD brush16(HBRUSH b) {return (ULONG_PTR)b<0x100?(WORD)(ULONG_PTR)b:HGDI16(b);}
DWORD W16_GetClassWord(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1]; ULONG_PTR v;
    if(i>=0) return GetClassWord(h,i);
    v=GetClassLongPtr(h,i);
    switch(i) {
    case GCL_HBRBACKGROUND: return brush16((HBRUSH)v);
    case GCL_HCURSOR: case GCL_HICON: return HICON16(v);
    case GCL_HMODULE: {Module16 *m=v>>24==0x7f?NeFromHandle((WORD)v):NULL; return m?m->handle:0;}
    default: return (WORD)v;
    }
}
DWORD W16_SetClassWord(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1]; WORD v=(WORD)a->a[2]; DWORD old=W16_GetClassWord(a);
    switch(i) {
    case GCL_HBRBACKGROUND: SetClassLongPtr(h,i,(LONG_PTR)Brush32(v)); break;
    case GCL_HCURSOR: case GCL_HICON: SetClassLongPtr(h,i,(LONG_PTR)HICON32(v)); break;
    case GCL_STYLE: SetClassLongPtr(h,i,v); break;
    default: if(i>=0) return SetClassWord(h,i,v);
    }
    return old;
}
DWORD W16_GetClassLong(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1]; Class16 *c;
    if(i==GCL_WNDPROC) {c=class_of(h); return c?c->proc:NativeProc16((WNDPROC)GetClassLongPtr(h,i));}
    if(i==GCL_MENUNAME) return 0;
    return GetClassLong(h,i);
}
DWORD W16_SetClassLong(Args16 *a) {
    HWND h=HWND32(a->a[0]); int i=(int)a->a[1]; Class16 *c; DWORD old;
    if(i==GCL_WNDPROC) {
        /* A native class keeps its procedure: other programs share it. */
        if(!(c=class_of(h)) || NativeProcOf(a->a[2])) return 0;
        old=c->proc; c->proc=a->a[2]; return old;
    }
    if(i==GCL_MENUNAME) return 0;
    return SetClassLong(h,i,(LONG)a->a[2]);
}
static DWORD call_proc16(Task16 *t,DWORD proc,WORD h,WORD msg,WORD wp,DWORD lp) {
    static const BYTE sizes[4]={2,2,2,4}; DWORD args[4]; WNDPROC native=NativeProcOf(proc);
    if(native) return CallNative16(t,native,h,msg,wp,lp);
    if(!proc) return 0;
    args[0]=h; args[1]=msg; args[2]=wp; args[3]=lp;
    return Call16(t,proc,4,args,sizes);
}
DWORD W16_CallWindowProc(Args16 *a) {return call_proc16(a->task,a->a[0],(WORD)a->a[1],(WORD)a->a[2],(WORD)a->a[3],a->a[4]);}

/* --- timers ------------------------------------------------------------------ */
/* A 16-bit timer procedure gets timer_proc natively. WM_TIMER's lParam
 * carries the 16-bit procedure to the program, and DispatchMessage calls
 * it; other native procedures go through procedure thunks. */
#define TIMERS 64
static struct {HWND h; UINT_PTR id; DWORD proc; Module16 *module;} timers[TIMERS];
static unsigned timer_slot(HWND h,UINT_PTR id) {
    unsigned i;
    for(i=0;i<TIMERS;i++) if(timers[i].proc && timers[i].h==h && timers[i].id==id) return i;
    return TIMERS;
}
static void CALLBACK timer_proc(HWND h,UINT msg,UINT_PTR id,DWORD time) {
    static const BYTE sizes[4]={2,2,2,4}; DWORD args[4]; unsigned i=timer_slot(h,id); Task16 *t=CurrentTask16();
    if(i==TIMERS || !t) return;
    args[0]=HWND16(h); args[1]=msg; args[2]=(WORD)id; args[3]=time;
    Call16(t,timers[i].proc,4,args,sizes);
}
DWORD W16_SetTimer(Args16 *a) {
    HWND h=HWND32(a->a[0]); DWORD proc=a->a[3]; UINT_PTR id; unsigned i;
    WNDPROC native=NativeProcOf(proc);
    if(!proc || native) return (WORD)SetTimer(h,a->a[1],(UINT)a->a[2],(TIMERPROC)native);
    id=SetTimer(h,a->a[1],(UINT)a->a[2],timer_proc);
    if(!id) return 0;
    if(h) id=a->a[1]; /* a window's timer keeps its id; the result is nonzero */
    if((i=timer_slot(h,id))==TIMERS) for(i=0;i<TIMERS && timers[i].proc;i++);
    if(i==TIMERS) {KillTimer(h,id); return 0;}
    timers[i].h=h; timers[i].id=id; timers[i].proc=proc; timers[i].module=a->task->module;
    return h?(id?(WORD)id:1):(WORD)id;
}
DWORD W16_KillTimer(Args16 *a) {
    HWND h=HWND32(a->a[0]); unsigned i=timer_slot(h,a->a[1]);
    if(i<TIMERS) timers[i].proc=0;
    return KillTimer(h,a->a[1]);
}

static void hooks16_ended(Module16 *m);
void User16TaskEnded(Module16 *m) {
    unsigned i;
    hooks16_ended(m);
    for(i=0;i<CLASSES;i++) if(classes[i].used && classes[i].module==m) {
        UnregisterClass(classes[i].name,INSTANCE32(m->handle)); classes[i].used=FALSE;
    }
    for(i=0;i<TIMERS;i++) if(timers[i].proc && timers[i].module==m) {KillTimer(timers[i].h,timers[i].id); timers[i].proc=0;}
    for(i=0;i<SUBCLASSES;i++) if(subclasses[i].h && !IsWindow(subclasses[i].h)) subclasses[i].h=NULL;
}

/* --- enumeration ------------------------------------------------------------- */
typedef struct {Task16 *task; DWORD proc,lp;} Enum16;
static BOOL CALLBACK enum_proc(HWND h,LPARAM lp) {
    static const BYTE sizes[2]={2,4}; Enum16 *e=(Enum16 *)lp; DWORD args[2];
    args[0]=HWND16(h); args[1]=e->lp;
    return LOWORD(Call16(e->task,e->proc,2,args,sizes))!=0;
}
static Enum16 *enum16(Args16 *a,Enum16 *e,int first) {e->task=a->task; e->proc=a->a[first]; e->lp=a->a[first+1]; return e;}
DWORD W16_EnumWindows(Args16 *a) {Enum16 e; return EnumWindows(enum_proc,(LPARAM)enum16(a,&e,0));}
DWORD W16_EnumChildWindows(Args16 *a) {Enum16 e; return EnumChildWindows(HWND32(a->a[0]),enum_proc,(LPARAM)enum16(a,&e,1));}
DWORD W16_EnumTaskWindows(Args16 *a) {
    Enum16 e; Task16 *t=Task16Of((WORD)a->a[0]);
    return t?EnumTaskWindows(t->native,enum_proc,(LPARAM)enum16(a,&e,1)):0;
}
DWORD W16_GetWindowTask(Args16 *a) {return Task16Handle(GetWindowTask(HWND32(a->a[0])));}

/* --- resources ---------------------------------------------------------------- */
static BYTE template_buffer[16384];
/* Copies an ANSI string to UTF-16 (code page 1252's first 256 are the same
 * code points); returns the next input byte. */
static const BYTE *widen(const BYTE *s,WORD **o,WORD *end) {
    while(*s && *o<end-1) *(*o)++=*s++;
    *(*o)++=0;
    return s+1;
}
static void menu_items(const BYTE **p,const BYTE *end,WORD **o,WORD *oend) {
    while(*p+2<=end && *o+3<oend) {
        WORD flags=get16(*p); *p+=2; *(*o)++=flags;
        if(!(flags&MF_POPUP)) {*(*o)++=get16(*p); *p+=2;}
        *p=widen(*p,o,oend);
        if(flags&MF_POPUP) menu_items(p,end,o,oend);
        if(flags&MF_END) return;
    }
}
static HMENU menu_from(const BYTE *r,DWORD size) {
    const BYTE *p; WORD *o=(WORD *)template_buffer;
    if(!r || size<4) return NULL;
    p=r+4+get16(r+2);
    *o++=0; *o++=0;
    menu_items(&p,r+size,&o,(WORD *)(template_buffer+sizeof(template_buffer)));
    return LoadMenuIndirect(template_buffer);
}
static HMENU load_menu(Module16 *m,LPCSTR name) {
    DWORD size; const BYTE *r=NeResource(m,RT_MENU16,name,&size);
    return menu_from(r,size);
}
/* Dialog icons: a static icon's resource names an icon of the program,
 * which the native static control cannot load; WM_INITDIALOG sets them. */
#define DIALOG_ICONS 16
static struct {Module16 *module; WORD id,icon; char name[16];} dialog_icons[DIALOG_ICONS]; /* icon 0: by name */
static unsigned dialog_icon_count;
static const BYTE *sz_or_ord(const BYTE *p,WORD **o,WORD *end,WORD *ordinal) {
    if(*p==0xff) {*(*o)++=0xffff; *(*o)++=get16(p+1); if(ordinal) *ordinal=get16(p+1); return p+3;}
    if(!*p) {*(*o)++=0; return p+1;}
    return widen(p,o,end);
}
static const void *convert_dialog(Module16 *m,const BYTE *r,DWORD size) {
    const BYTE *p; WORD *o=(WORD *)template_buffer,*end;
    DWORD style; unsigned count,i;
    if(!r || size<13) return NULL;
    end=(WORD *)(template_buffer+sizeof(template_buffer))-16;
    style=get32(r); count=r[4];
    put32((BYTE *)o,style); o+=2; *o++=0; *o++=0; *o++=(WORD)count;
    for(i=0;i<4;i++) *o++=get16(r+5+i*2);
    p=r+13;
    p=sz_or_ord(p,&o,end,NULL); /* menu */
    if(*p) p=widen(p,&o,end); else {*o++=0; p++;} /* class */
    p=widen(p,&o,end); /* caption */
    if(style&DS_SETFONT) {*o++=get16(p); p=widen(p+2,&o,end);}
    dialog_icon_count=0;
    for(i=0;i<count && o<end;i++) {
        WORD x=get16(p),y=get16(p+2),cx=get16(p+4),cy=get16(p+6),id=get16(p+8),icon=0; DWORD istyle=get32(p+10);
        BYTE cls; unsigned extra; const BYTE *text;
        p+=14;
        while(((BYTE *)o-template_buffer)&3) *o++=0;
        put32((BYTE *)o,istyle); o+=2; *o++=0; *o++=0;
        *o++=x; *o++=y; *o++=cx; *o++=cy; *o++=id;
        cls=*p;
        if(cls&0x80) {*o++=0xffff; *o++=cls; p++;} else p=widen(p,&o,end);
        text=p; p=sz_or_ord(p,&o,end,&icon);
        /* An icon's, by number or by name: from the module once the dialog is made. */
        if(cls==0x82 && (istyle&0x0f)==SS_ICON && (icon || *text) && dialog_icon_count<DIALOG_ICONS) {
            dialog_icons[dialog_icon_count].module=m; dialog_icons[dialog_icon_count].id=id;
            dialog_icons[dialog_icon_count].icon=icon;
            if(!icon) lstrcpyn(dialog_icons[dialog_icon_count].name,(LPCSTR)text,sizeof(dialog_icons[0].name));
            dialog_icon_count++;
        }
        extra=*p++;
        *o++=(WORD)extra; memcpy(o,p,extra); o=(WORD *)((BYTE *)o+extra); p+=extra;
    }
    return template_buffer;
}
#define ICON_CACHE 64
static struct {Module16 *module; WORD id; BOOL cursor; HICON h;} icon_cache[ICON_CACHE];
static HICON load_icon(Module16 *m,LPCSTR name,BOOL cursor) {
    DWORD size; const BYTE *dir,*bits; int id; HICON h; unsigned i;
    if(!HIWORD((ULONG_PTR)name))
        for(i=0;i<ICON_CACHE;i++) if(icon_cache[i].h && icon_cache[i].module==m && icon_cache[i].cursor==cursor && icon_cache[i].id==LOWORD((ULONG_PTR)name)) return icon_cache[i].h;
    dir=NeResource(m,cursor?RT_GROUP_CURSOR16:RT_GROUP_ICON16,name,&size);
    if(!dir || (id=LookupIconIdFromDirectory(dir,!cursor))<=0) return NULL;
    bits=NeResource(m,cursor?RT_CURSOR16:RT_ICON16,MAKEINTRESOURCE(id),&size);
    h=bits?CreateIconFromResource(bits,size,!cursor,0x00030000):NULL;
    if(h && !HIWORD((ULONG_PTR)name))
        for(i=0;i<ICON_CACHE;i++) if(!icon_cache[i].h) {icon_cache[i].module=m; icon_cache[i].cursor=cursor; icon_cache[i].id=LOWORD((ULONG_PTR)name); icon_cache[i].h=h; break;}
    return h;
}
void DialogBound16(HWND dlg) {
    unsigned i;
    for(i=0;i<dialog_icon_count;i++) {
        HICON h=load_icon(dialog_icons[i].module,dialog_icons[i].icon?MAKEINTRESOURCE(dialog_icons[i].icon):dialog_icons[i].name,FALSE);
        if(h) SendDlgItemMessage(dlg,dialog_icons[i].id,STM_SETICON,(WPARAM)h,0);
    }
    dialog_icon_count=0;
}

/* --- DDE ---------------------------------------------------------------------- */
/* Between a Win16 program and a native one, a DDE message's shared memory
 * becomes a copy on the other side (for WM_DDE_EXECUTE kept until its
 * acknowledgement brings the handle back, which then becomes the
 * original again; with fRelease the original is freed at once), and the
 * two values native code packs in a block (WM_DDE_ACK, ADVISE, DATA, POKE)
 * are packed and unpacked. Atoms are KERNEL's on both sides. Between two
 * Win16 programs nothing changes but the window in wParam. */
#define DDE_COPIES 32
static struct {HGLOBAL native; WORD sel; BOOL to_native;} dde_copies[DDE_COPIES];
static BOOL is_dde(UINT msg) {return msg>=WM_DDE_FIRST && msg<=WM_DDE_LAST;}
static BOOL win16_window(HWND h) {return ClassProc16(h)!=0;}
static void remember(HGLOBAL native,WORD sel,BOOL to_native) {
    unsigned i;
    for(i=0;i<DDE_COPIES;i++) if(!dde_copies[i].native) {dde_copies[i].native=native; dde_copies[i].sel=sel; dde_copies[i].to_native=to_native; return;}
}
static BOOL released(const BYTE *p) {return p && (p[1]&0x20);} /* DDEDATA/DDEPOKE's fRelease */
static HGLOBAL native_copy(WORD sel) {
    DWORD size=GlobalSize16(sel); const BYTE *p=(const BYTE *)Lin16(MAKELONG(0,sel)); HGLOBAL h; BYTE *d;
    if(!p || !size || !(h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,size))) return NULL;
    if((d=(BYTE *)GlobalLock(h))!=NULL) {memcpy(d,p,size); GlobalUnlock(h);}
    return h;
}
static WORD copy16(Task16 *t,HGLOBAL h) {
    SIZE_T size=h?GlobalSize(h):0; const BYTE *p; BYTE *d; WORD sel;
    if(!size || size>0xffff || !(sel=GlobalAlloc16(t,GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)size))) return 0;
    if((p=(const BYTE *)GlobalLock(h))!=NULL) {
        if((d=(BYTE *)Lin16(MAKELONG(0,sel)))!=NULL) memcpy(d,p,size);
        GlobalUnlock(h);
    }
    return sel;
}
/* Posted by a Win16 program to a native window: lParam for native code. */
static LPARAM dde_to_native(UINT msg,DWORD lp16) {
    WORD lo=LOWORD(lp16),hi=HIWORD(lp16); HGLOBAL h; unsigned i;
    switch(msg) {
    case WM_DDE_EXECUTE:
        if((h=native_copy(hi))!=NULL) remember(h,hi,TRUE);
        return (LPARAM)h;
    case WM_DDE_ADVISE: case WM_DDE_DATA: case WM_DDE_POKE: {
        BOOL release=msg!=WM_DDE_ADVISE && released((const BYTE *)Lin16(MAKELONG(0,lo)));
        h=native_copy(lo);
        if(h && release) GlobalFree16(lo);
        return PackDDElParam(msg,(UINT_PTR)h,hi);
    }
    case WM_DDE_ACK:
        /* A native program's commands acknowledged: its own handle again. */
        for(i=0;i<DDE_COPIES;i++) if(dde_copies[i].native && !dde_copies[i].to_native && dde_copies[i].sel==hi) {
            h=dde_copies[i].native; dde_copies[i].native=NULL; GlobalFree16(hi);
            return PackDDElParam(msg,lo,(UINT_PTR)h);
        }
        return PackDDElParam(msg,lo,hi);
    }
    return (LPARAM)lp16;
}
/* Posted by native code, retrieved by a Win16 program. */
static DWORD dde_to16(Task16 *t,UINT msg,LPARAM lp) {
    UINT_PTR lo,hi; WORD sel; unsigned i;
    switch(msg) {
    case WM_DDE_EXECUTE:
        if((sel=copy16(t,(HGLOBAL)lp))!=0) remember((HGLOBAL)lp,sel,FALSE);
        return MAKELONG(0,sel);
    case WM_DDE_ADVISE: case WM_DDE_DATA: case WM_DDE_POKE: {
        BOOL release; const BYTE *p;
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        sel=copy16(t,(HGLOBAL)lo);
        p=(const BYTE *)Lin16(MAKELONG(0,sel)); release=msg!=WM_DDE_ADVISE && released(p);
        if(lo && (release || msg==WM_DDE_ADVISE)) GlobalFree((HGLOBAL)lo);
        return MAKELONG(sel,(WORD)hi);
    }
    case WM_DDE_ACK:
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        /* The program's own commands acknowledged: its selector again. */
        for(i=0;i<DDE_COPIES;i++) if(dde_copies[i].native && dde_copies[i].to_native && (UINT_PTR)dde_copies[i].native==hi) {
            sel=dde_copies[i].sel; GlobalFree(dde_copies[i].native); dde_copies[i].native=NULL;
            return MAKELONG((WORD)lo,sel);
        }
        return MAKELONG((WORD)lo,(WORD)hi);
    }
    return (DWORD)lp;
}

/* --- messages ---------------------------------------------------------------- */
/* MSG16: hwnd, message, wParam (WORDs), lParam, time, pt (two shorts). */
static void msg_out16(BYTE *p,const MSG *m) {
    WORD m16,w16; DWORD l16;
    Msg32To16(m->message,m->wParam,m->lParam,&m16,&w16,&l16);
    put16(p,HWND16(m->hwnd)); put16(p+2,m16); put16(p+4,w16); put32(p+6,l16); put32(p+10,m->time);
    put16(p+14,(WORD)m->pt.x); put16(p+16,(WORD)m->pt.y);
}
static void msg_in16(const BYTE *p,MSG *m) {
    m->hwnd=HWND32(get16(p)); Msg16To32(get16(p+2),get16(p+4),get32(p+6),&m->message,&m->wParam,&m->lParam);
    m->time=get32(p+10); m->pt.x=(short)get16(p+14); m->pt.y=(short)get16(p+16);
}
/* The MSGs a program gets and gives back: WM_TIMER's lParam is a 16-bit procedure. */
static void msg_to16(BYTE *p,const MSG *m) {
    msg_out16(p,m);
    if(m->message==WM_TIMER && m->lParam) {
        unsigned i=timer_slot(m->hwnd,m->wParam);
        put32(p+6,(TIMERPROC)m->lParam==timer_proc?(i<TIMERS?timers[i].proc:0):NativeProc16((WNDPROC)m->lParam));
    }
}
static void msg_from16(MSG *m,const BYTE *p) {
    msg_in16(p,m);
    if(m->message==WM_TIMER) m->lParam=(LPARAM)NativeProcOf((DWORD)m->lParam);
}
/* A DDE message a native window posted is converted as it is removed. */
static void retrieved16(Task16 *t,BYTE *p,const MSG *m,BOOL removed) {
    msg_to16(p,m);
    if(removed && is_dde(m->message) && m->message!=WM_DDE_INITIATE && !win16_window((HWND)m->wParam)) put32(p+6,dde_to16(t,m->message,m->lParam));
}
DWORD W16_GetMessage(Args16 *a) {
    MSG m; BOOL r=GetMessage(&m,HWND32(a->a[1]),(UINT)a->a[2],(UINT)a->a[3]);
    if(a->a[0]) retrieved16(a->task,(BYTE *)PTR(a->a[0]),&m,TRUE);
    return (DWORD)r;
}
DWORD W16_PeekMessage(Args16 *a) {
    MSG m; BOOL r=PeekMessage(&m,HWND32(a->a[1]),(UINT)a->a[2],(UINT)a->a[3],(UINT)a->a[4]);
    if(r && a->a[0]) retrieved16(a->task,(BYTE *)PTR(a->a[0]),&m,(a->a[4]&PM_REMOVE)!=0);
    return (DWORD)r;
}
DWORD W16_TranslateMessage(Args16 *a) {MSG m; msg_from16(&m,(const BYTE *)PTR(a->a[0])); return TranslateMessage(&m);}
DWORD W16_DispatchMessage(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); MSG m;
    if(!p) return 0;
    if(get16(p+2)==WM_TIMER && get32(p+6) && !NativeProcOf(get32(p+6))) {
        static const BYTE sizes[4]={2,2,2,4}; DWORD args[4];
        args[0]=get16(p); args[1]=WM_TIMER; args[2]=get16(p+4); args[3]=get32(p+10);
        Call16(a->task,get32(p+6),4,args,sizes);
        return 0;
    }
    msg_from16(&m,p);
    return (DWORD)DispatchMessage(&m);
}
DWORD W16_TranslateAccelerator(Args16 *a) {
    MSG m; msg_from16(&m,(const BYTE *)PTR(a->a[2]));
    return (DWORD)TranslateAccelerator(HWND32(a->a[0]),HACCEL32(a->a[1]),&m);
}
DWORD W16_DefWindowProc(Args16 *a) {
    return CallNative16(a->task,DefWindowProc,(WORD)a->a[0],(WORD)a->a[1],(WORD)a->a[2],a->a[3]);
}
DWORD W16_DefDlgProc(Args16 *a) {
    return CallNative16(a->task,DefDlgProc,(WORD)a->a[0],(WORD)a->a[1],(WORD)a->a[2],a->a[3]);
}
static DWORD send16(Task16 *t,WORD h16,WORD m16,WORD w16,DWORD l16) {
    UINT msg; WPARAM wp; LPARAM lp; HWND h=HWND32(h16);
    if(h16==0xffff) { /* to every top-level window */
        Msg16To32(m16,w16,l16,&msg,&wp,&lp);
        return (DWORD)SendMessage((HWND)0xffff,msg,wp,lp);
    }
    if(ClassProc16(h)) {
        Msg16To32(m16,w16,l16,&msg,&wp,&lp);
        return (DWORD)SendMessage(h,msg,wp,lp);
    }
    return CallNative16(t,(WNDPROC)SendMessage,h16,m16,w16,l16);
}
DWORD W16_SendMessage(Args16 *a) {return send16(a->task,(WORD)a->a[0],(WORD)a->a[1],(WORD)a->a[2],a->a[3]);}
DWORD W16_SendDlgItemMessage(Args16 *a) {
    HWND h=GetDlgItem(HWND32(a->a[0]),(int)a->a[1]);
    return h?send16(a->task,HWND16(h),(WORD)a->a[2],(WORD)a->a[3],a->a[4]):0;
}
DWORD W16_IsDialogMessage(Args16 *a) {
    MSG m; const BYTE *p=(const BYTE *)PTR(a->a[1]);
    if(!p) return 0;
    msg_from16(&m,p);
    return IsDialogMessage(HWND32(a->a[0]),&m);
}
DWORD W16_GetDlgItemInt(Args16 *a) {
    BOOL ok=FALSE,sign=(BOOL)a->a[3]; int v=(int)GetDlgItemInt(HWND32(a->a[0]),(int)a->a[1],&ok,sign);
    BYTE *out=(BYTE *)PTR(a->a[2]);
    if(ok && (sign?v<-32768 || v>32767:(UINT)v>0xffff)) {ok=FALSE; v=0;}
    if(out) put16(out,(WORD)ok);
    return (WORD)v;
}
DWORD W16_PostMessage(Args16 *a) {
    UINT msg; WPARAM wp; LPARAM lp; HWND h=(WORD)a->a[0]==0xffff?(HWND)0xffff:HWND32(a->a[0]); BOOL r;
    Msg16To32((WORD)a->a[1],(WORD)a->a[2],a->a[3],&msg,&wp,&lp);
    if(is_dde(msg) && msg!=WM_DDE_INITIATE && h!=(HWND)0xffff && !win16_window(h)) lp=dde_to_native(msg,a->a[3]);
    r=PostMessage(h,msg,wp,lp);
    return (DWORD)r;
}

/* --- windows ------------------------------------------------------------------ */
DWORD W16_RegisterClass(Args16 *a) {
    const BYTE *w=(const BYTE *)PTR(a->a[0]); WNDCLASS wc; Class16 *c=NULL; LPCSTR name; ATOM atom; unsigned i;
    if(!w || !(name=(LPCSTR)Lin16(get32(w+22)))) return 0;
    for(i=0;i<CLASSES && !c;i++) if(!classes[i].used) c=&classes[i];
    if(!c || class_named(name)) return 0;
    memset(&wc,0,sizeof(wc));
    wc.style=get16(w); wc.lpfnWndProc=WowWndProc; wc.cbClsExtra=(short)get16(w+6); wc.cbWndExtra=(short)get16(w+8);
    wc.hInstance=instance_of(get16(w+10)); wc.hIcon=HICON32(get16(w+12)); wc.hCursor=HICON32(get16(w+14));
    wc.hbrBackground=Brush32(get16(w+16));
    wc.lpszClassName=name;
    atom=RegisterClass(&wc);
    if(!atom) return 0;
    memset(c,0,sizeof(*c)); c->used=TRUE; lstrcpyn(c->name,name,sizeof(c->name));
    c->proc=get32(w+2); c->module=module_of(get16(w+10));
    {
        DWORD menu=get32(w+18);
        if(HIWORD(menu)) {lstrcpyn(c->menu_name,(LPCSTR)Lin16(menu),sizeof(c->menu_name)); c->menu=c->menu_name;}
        else c->menu=MAKEINTRESOURCE(LOWORD(menu));
    }
    return atom;
}
static int coordinate(DWORD v) {return (short)v==(short)0x8000?CW_USEDEFAULT:(int)(short)v;}
/* CreateWindow's parameter for an MDI client: CLIENTCREATESTRUCT, the
 * window menu and the first child's identifier. */
static WORD create_window(DWORD exstyle,const DWORD *v) {
    LPCSTR cls=(LPCSTR)PTR(v[0]),title=(LPCSTR)PTR(v[1]); DWORD style=v[2]; HMENU menu; Class16 *c=class_named(cls); HWND h;
    CLIENTCREATESTRUCT client; LPVOID param=(LPVOID)(ULONG_PTR)v[10];
    if(style&WS_CHILD) menu=(HMENU)(ULONG_PTR)v[8];
    else if(v[8]) menu=HMENU32(v[8]);
    else menu=c && c->menu && (HIWORD((ULONG_PTR)c->menu) || LOWORD((ULONG_PTR)c->menu))?load_menu(c->module,c->menu):NULL;
    if(HIWORD((ULONG_PTR)cls) && !lstrcmpi(cls,"MDICLIENT") && v[10]) {
        const BYTE *p=(const BYTE *)Lin16(v[10]);
        client.hWindowMenu=HMENU32(get16(p)); client.idFirstChild=get16(p+2); param=&client;
    }
    h=CreateWindowEx(exstyle,cls,title,style,coordinate(v[3]),coordinate(v[4]),coordinate(v[5]),coordinate(v[6]),
                     HWND32(v[7]),menu,instance_of((WORD)v[9]),param);
    return HWND16(h);
}
DWORD W16_CreateWindow(Args16 *a) {return create_window(0,a->a);}
DWORD W16_CreateWindowEx(Args16 *a) {return create_window(a->a[0],a->a+1);}
/* HWND_TOP 0, HWND_BOTTOM 1, HWND_TOPMOST -1, HWND_NOTOPMOST -2, or a window. */
static HWND insert_after(WORD after) {return after<=1 || after>=0xfffe?(HWND)(LONG_PTR)(short)after:HWND32(after);}
DWORD W16_SetWindowPos(Args16 *a) {
    return SetWindowPos(HWND32(a->a[0]),insert_after((WORD)a->a[1]),
                        (short)a->a[2],(short)a->a[3],(short)a->a[4],(short)a->a[5],(UINT)a->a[6]);
}
/* DeferWindowPos collects the moves; EndDeferWindowPos makes them. */
#define DEFERS 4
#define DEFERRED 32
static struct {BOOL used; int count; struct {HWND h,after; int x,y,cx,cy; UINT flags;} pos[DEFERRED];} defers[DEFERS];
static int defer_index(WORD h) {return h%4 || !h || h/4>DEFERS || !defers[h/4-1].used?-1:h/4-1;}
DWORD W16_BeginDeferWindowPos(Args16 *a) {
    int i; (void)a;
    for(i=0;i<DEFERS;i++) if(!defers[i].used) {defers[i].used=TRUE; defers[i].count=0; return (DWORD)(i+1)*4;}
    return 0;
}
DWORD W16_DeferWindowPos(Args16 *a) {
    int i=defer_index((WORD)a->a[0]),k;
    if(i<0) return 0;
    if(defers[i].count==DEFERRED) {defers[i].used=FALSE; return 0;}
    k=defers[i].count++;
    defers[i].pos[k].h=HWND32(a->a[1]); defers[i].pos[k].after=insert_after((WORD)a->a[2]);
    defers[i].pos[k].x=(int)a->a[3]; defers[i].pos[k].y=(int)a->a[4]; defers[i].pos[k].cx=(int)a->a[5]; defers[i].pos[k].cy=(int)a->a[6];
    defers[i].pos[k].flags=(UINT)a->a[7];
    return a->a[0];
}
DWORD W16_EndDeferWindowPos(Args16 *a) {
    int i=defer_index((WORD)a->a[0]),k;
    if(i<0) return 0;
    for(k=0;k<defers[i].count;k++)
        SetWindowPos(defers[i].pos[k].h,defers[i].pos[k].after,defers[i].pos[k].x,defers[i].pos[k].y,defers[i].pos[k].cx,defers[i].pos[k].cy,defers[i].pos[k].flags);
    defers[i].used=FALSE;
    return 1;
}
/* Points passed by value: x in the low word, y in the high word. */
static POINT point_of(DWORD v) {POINT p; p.x=(short)LOWORD(v); p.y=(short)HIWORD(v); return p;}
DWORD W16_WindowFromPoint(Args16 *a) {return HWND16(WindowFromPoint(point_of(a->a[0])));}
DWORD W16_ChildWindowFromPoint(Args16 *a) {return HWND16(ChildWindowFromPoint(HWND32(a->a[0]),point_of(a->a[1])));}
DWORD W16_PtInRect(Args16 *a) {RECT r; return RectIn16(a->a[0],&r) && PtInRect(&r,point_of(a->a[1]));}
DWORD W16_GetScrollRange(Args16 *a) {
    int lo=0,hi=0; BYTE *pl=(BYTE *)PTR(a->a[2]),*ph=(BYTE *)PTR(a->a[3]);
    GetScrollRange(HWND32(a->a[0]),(int)a->a[1],&lo,&hi);
    if(pl) put16(pl,(WORD)lo);
    if(ph) put16(ph,(WORD)hi);
    return 0;
}
DWORD W16_MapWindowPoints(Args16 *a) {
    POINT pt[64]; BYTE *p=(BYTE *)PTR(a->a[2]); UINT n=(UINT)a->a[3],i,done;
    for(done=0;p && done<n;done+=i) {
        for(i=0;i<64 && done+i<n;i++) {pt[i].x=(short)get16(p+(done+i)*4); pt[i].y=(short)get16(p+(done+i)*4+2);}
        MapWindowPoints(HWND32(a->a[0]),HWND32(a->a[1]),pt,i);
        for(i=0;i<64 && done+i<n;i++) {put16(p+(done+i)*4,(WORD)pt[i].x); put16(p+(done+i)*4+2,(WORD)pt[i].y);}
    }
    return 0;
}
/* PAINTSTRUCT16: hdc, fErase, rcPaint (4 shorts), fRestore, fIncUpdate, 16 reserved. */
DWORD W16_BeginPaint(Args16 *a) {
    PAINTSTRUCT ps; HDC dc=BeginPaint(HWND32(a->a[0]),&ps); BYTE *o=(BYTE *)PTR(a->a[1]);
    if(o) {memset(o,0,32); put16(o,HGDI16(dc)); put16(o+2,(WORD)ps.fErase); RectOut16(a->a[1]+4,&ps.rcPaint);}
    return HGDI16(dc);
}
DWORD W16_EndPaint(Args16 *a) {
    PAINTSTRUCT ps; const BYTE *i=(const BYTE *)PTR(a->a[1]);
    memset(&ps,0,sizeof(ps));
    if(i) {ps.hdc=(HDC)HGDI32(get16(i)); ps.fErase=get16(i+2); RectIn16(a->a[1]+4,&ps.rcPaint);}
    EndPaint(HWND32(a->a[0]),&ps);
    return 1;
}
DWORD W16_FillRect(Args16 *a) {
    RECT r;
    if(!RectIn16(a->a[1],&r)) return 0;
    return (DWORD)FillRect((HDC)HGDI32(a->a[0]),&r,Brush32((WORD)a->a[2]));
}

/* --- menus ------------------------------------------------------------------- */
DWORD W16_LoadMenu(Args16 *a) {return HMENU16(load_menu(module_of((WORD)a->a[0]),(LPCSTR)PTR(a->a[1])));}
DWORD W16_LoadMenuIndirect(Args16 *a) {return HMENU16(menu_from((const BYTE *)PTR(a->a[0]),0x10000-LOWORD(a->raw[0])));}
static LPCSTR menu_item(UINT flags,DWORD item) {
    if(flags&(MF_BITMAP|MF_OWNERDRAW)) return (LPCSTR)(ULONG_PTR)(flags&MF_BITMAP?(ULONG_PTR)HGDI32(LOWORD(item)):item);
    if(flags&MF_SEPARATOR) return NULL;
    return (LPCSTR)Lin16(item);
}
DWORD W16_InsertMenu(Args16 *a) {
    UINT flags=(UINT)a->a[2]; UINT_PTR id=flags&MF_POPUP?(UINT_PTR)HMENU32(a->a[3]):(UINT_PTR)a->a[3];
    return InsertMenu(HMENU32(a->a[0]),(UINT)a->a[1],flags,id,menu_item(flags,a->a[4]));
}
DWORD W16_ModifyMenu(Args16 *a) {
    UINT flags=(UINT)a->a[2]; UINT_PTR id=flags&MF_POPUP?(UINT_PTR)HMENU32(a->a[3]):(UINT_PTR)a->a[3];
    return ModifyMenu(HMENU32(a->a[0]),(UINT)a->a[1],flags,id,menu_item(flags,a->a[4]));
}
DWORD W16_AppendMenu(Args16 *a) {
    UINT flags=(UINT)a->a[1]; UINT_PTR id=flags&MF_POPUP?(UINT_PTR)HMENU32(a->a[2]):(UINT_PTR)a->a[2];
    return AppendMenu(HMENU32(a->a[0]),flags,id,menu_item(flags,a->a[3]));
}

/* --- resources ---------------------------------------------------------------- */
DWORD W16_LoadIcon(Args16 *a) {
    if(!a->a[0]) return HICON16(LoadIcon(NULL,(LPCSTR)PTR(a->a[1])));
    return HICON16(load_icon(module_of((WORD)a->a[0]),(LPCSTR)PTR(a->a[1]),FALSE));
}
/* A bitmap resource is a DIB without the file header. */
DWORD W16_LoadBitmap(Args16 *a) {
    DWORD size,colors,header; const BITMAPINFOHEADER *h; HDC dc; HBITMAP b;
    if(!a->a[0]) return HGDI16(LoadBitmap(NULL,(LPCSTR)PTR(a->a[1])));
    h=(const BITMAPINFOHEADER *)NeResource(module_of((WORD)a->a[0]),RT_BITMAP16,(LPCSTR)PTR(a->a[1]),&size);
    if(!h || size<sizeof(BITMAPCOREHEADER)) return 0;
    if(h->biSize==sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREHEADER *c=(const BITMAPCOREHEADER *)h;
        colors=c->bcBitCount<=8?1u<<c->bcBitCount:0; header=c->bcSize+colors*3;
    } else {
        colors=h->biBitCount<=8?(h->biClrUsed?h->biClrUsed:1u<<h->biBitCount):0; header=h->biSize+colors*4;
    }
    if(header>=size) return 0;
    dc=GetDC(NULL);
    b=CreateDIBitmap(dc,h,CBM_INIT,(const BYTE *)h+header,(const BITMAPINFO *)h,DIB_RGB_COLORS);
    ReleaseDC(NULL,dc);
    return HGDI16(b);
}
DWORD W16_LoadCursor(Args16 *a) {
    if(!a->a[0]) return HICON16(LoadCursor(NULL,(LPCSTR)PTR(a->a[1])));
    return HICON16(load_icon(module_of((WORD)a->a[0]),(LPCSTR)PTR(a->a[1]),TRUE));
}
/* Accelerators: fFlags (0x80 ends the table), key, command. */
DWORD W16_LoadAccelerators(Args16 *a) {
    DWORD size; const BYTE *r=NeResource(module_of((WORD)a->a[0]),RT_ACCELERATOR16,(LPCSTR)PTR(a->a[1]),&size);
    ACCEL table[64]; int n=0;
    if(!r) return 0;
    while(n<64 && (DWORD)(n+1)*5<=size) {
        const BYTE *e=r+n*5;
        table[n].fVirt=(BYTE)(e[0]&0x7f); table[n].key=get16(e+1); table[n].cmd=get16(e+3); n++;
        if(e[0]&0x80) break;
    }
    return HACCEL16(CreateAcceleratorTable(table,n));
}
DWORD W16_LoadString(Args16 *a) {
    DWORD size; const BYTE *r=NeResource(module_of((WORD)a->a[0]),MAKEINTRESOURCE(6),MAKEINTRESOURCE((a->a[1]>>4)+1),&size);
    char *out=(char *)PTR(a->a[2]); int capacity=(int)(short)a->a[3],n; unsigned i;
    if(!out || capacity<=0) return 0;
    out[0]=0;
    if(!r) return 0;
    for(i=0;i<(a->a[1]&15);i++) r+=1+r[0];
    n=r[0]<capacity-1?r[0]:capacity-1;
    memcpy(out,r+1,(DWORD)n); out[n]=0;
    return (DWORD)n;
}

/* --- dialogs ------------------------------------------------------------------ */
/* Templates: a resource name, a far pointer, or (DialogBoxIndirect) a
 * global handle. Arguments: instance, template, owner, procedure. */
enum {FROM_RESOURCE,FROM_POINTER,FROM_HANDLE};
static const void *dialog_template(Args16 *a,int from,Module16 **m) {
    DWORD size=0; const BYTE *r;
    *m=module_of((WORD)a->a[0]);
    if(from==FROM_RESOURCE) r=NeResource(*m,RT_DIALOG16,(LPCSTR)PTR(a->a[1]),&size);
    else if(from==FROM_POINTER) {r=(const BYTE *)PTR(a->a[1]); size=r?0x10000-(a->raw[1]&0xffff):0;}
    else {r=SelValid((WORD)a->a[1])?(const BYTE *)SelPointer((WORD)(a->a[1]|7),0):NULL; size=r?SelLimit((WORD)(a->a[1]|7))+1:0;}
    return r?convert_dialog(*m,r,size):NULL;
}
static DWORD dialog(Args16 *a,int from,DWORD param) {
    Module16 *m; const void *t=dialog_template(a,from,&m); INT_PTR r;
    if(!t) return (DWORD)-1;
    pending_dialog_proc=a->a[3];
    r=DialogBoxIndirectParam(INSTANCE32(m->handle),(LPCDLGTEMPLATE)t,HWND32(a->a[2]),WowDlgProc,(LPARAM)param);
    pending_dialog_proc=0;
    return (WORD)r;
}
static DWORD modeless(Args16 *a,int from,DWORD param) {
    Module16 *m; const void *t=dialog_template(a,from,&m); HWND h;
    if(!t) return 0;
    pending_dialog_proc=a->a[3];
    h=CreateDialogIndirectParam(INSTANCE32(m->handle),(LPCDLGTEMPLATE)t,HWND32(a->a[2]),WowDlgProc,(LPARAM)param);
    pending_dialog_proc=0;
    return HWND16(h);
}
DWORD W16_DialogBox(Args16 *a) {return dialog(a,FROM_RESOURCE,0);}
DWORD W16_DialogBoxParam(Args16 *a) {return dialog(a,FROM_RESOURCE,a->a[4]);}
DWORD W16_DialogBoxIndirect(Args16 *a) {return dialog(a,FROM_HANDLE,0);}
DWORD W16_DialogBoxIndirectParam(Args16 *a) {return dialog(a,FROM_HANDLE,a->a[4]);}
DWORD W16_CreateDialog(Args16 *a) {return modeless(a,FROM_RESOURCE,0);}
DWORD W16_CreateDialogParam(Args16 *a) {return modeless(a,FROM_RESOURCE,a->a[4]);}
DWORD W16_CreateDialogIndirect(Args16 *a) {return modeless(a,FROM_POINTER,0);}
DWORD W16_CreateDialogIndirectParam(Args16 *a) {return modeless(a,FROM_POINTER,a->a[4]);}

/* --- start-up and the rest ------------------------------------------------- */
DWORD W16_InitApp(Args16 *a) {return InitApp(INSTANCE32(a->task->module->handle))?1:0;}

/* wsprintf: %[-][#][0][width][.precision][l]{d,i,u,x,X,c,s}; without l the
 * argument is a WORD, %s takes a far pointer. */
int Format16(char *out,unsigned size,LPCSTR f,WORD sel,WORD args) {
    unsigned n=0;
    if(!size) return 0;
    while(*f && n+1<size) {
        BOOL left=FALSE,zero=FALSE,alt=FALSE,lng=FALSE; unsigned width=0,precision=0,i; BOOL has_precision=FALSE;
        char digits[16],*p=digits; const char *text=NULL; unsigned len=0; DWORD v;
        if(*f!='%') {out[n++]=*f++; continue;}
        f++;
        for(;;f++) {if(*f=='-') left=TRUE; else if(*f=='0') zero=TRUE; else if(*f=='#') alt=TRUE; else break;}
        while(*f>='0' && *f<='9') width=width*10+(unsigned)(*f++-'0');
        if(*f=='.') {has_precision=TRUE; f++; while(*f>='0' && *f<='9') precision=precision*10+(unsigned)(*f++-'0');}
        if(*f=='l' || *f=='L') {lng=TRUE; f++;}
        switch(*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            BOOL neg=FALSE; unsigned base=*f=='x' || *f=='X'?16:10; const char *hex=*f=='X'?"0123456789ABCDEF":"0123456789abcdef";
            char rev[16]; unsigned r=0;
            if(lng) {v=Peek16(sel,args)|(DWORD)Peek16(sel,(WORD)(args+2))<<16; args=(WORD)(args+4);}
            else {v=Peek16(sel,args); args=(WORD)(args+2); if(*f=='d' || *f=='i') v=(DWORD)(LONG)(short)v;}
            if((*f=='d' || *f=='i') && (LONG)v<0) {neg=TRUE; v=(DWORD)-(LONG)v;}
            do {rev[r++]=hex[v%base]; v/=base;} while(v);
            if(neg) *p++='-';
            if(alt && base==16) {*p++='0'; *p++=*f;}
            while(r) *p++=rev[--r];
            text=digits; len=(unsigned)(p-digits); break;
        }
        case 'c': digits[0]=(char)Peek16(sel,args); args=(WORD)(args+2); text=digits; len=1; break;
        case 's': {
            DWORD sp=Peek16(sel,args)|(DWORD)Peek16(sel,(WORD)(args+2))<<16;
            args=(WORD)(args+4); text=(const char *)Lin16(sp); if(!text) text="(null)";
            len=(unsigned)lstrlen(text); if(has_precision && precision<len) len=precision; break;
        }
        case 0: continue;
        default: out[n++]=*f++; continue;
        }
        f++;
        if(!left) for(i=len;i<width && n+1<size;i++) out[n++]=zero && *text!='-'?'0':' ';
        for(i=0;i<len && n+1<size;i++) out[n++]=text[i];
        if(left) for(i=len;i<width && n+1<size;i++) out[n++]=' ';
    }
    out[n]=0;
    return (int)n;
}
DWORD W16_wsprintf(Args16 *a) {
    char *out=(char *)PTR(a->a[0]);
    if(!out || !a->a[1]) return 0;
    return (DWORD)Format16(out,1024,(LPCSTR)PTR(a->a[1]),Seg16(a->task,2),(WORD)(a->stack+8));
}
DWORD W16_wvsprintf(Args16 *a) {
    char *out=(char *)PTR(a->a[0]);
    if(!out || !a->a[1]) return 0;
    return (DWORD)Format16(out,1024,(LPCSTR)PTR(a->a[1]),HIWORD(a->raw[2]),LOWORD(a->raw[2]));
}

/* --- strings ------------------------------------------------------------------- */
/* AnsiUpper and AnsiLower take a string or, with a zero selector, a character. */
static char upper(char c) {return c>='a' && c<='z'?(char)(c-32):(BYTE)c>=0xe0 && (BYTE)c!=0xf7?(char)(c-32):c;}
static char lower(char c) {return c>='A' && c<='Z'?(char)(c+32):(BYTE)c>=0xc0 && (BYTE)c<=0xde && (BYTE)c!=0xd7?(char)(c+32):c;}
static DWORD ansi_case(Args16 *a,char (*fn)(char)) {
    DWORD raw=a->raw[0]; char *s;
    if(!HIWORD(raw)) return (BYTE)fn((char)LOWORD(raw));
    for(s=(char *)Lin16(raw);s && *s;s++) *s=fn(*s);
    return raw;
}
DWORD W16_AnsiUpper(Args16 *a) {return ansi_case(a,upper);}
DWORD W16_AnsiLower(Args16 *a) {return ansi_case(a,lower);}
DWORD W16_AnsiNext(Args16 *a) {const char *s=(const char *)Lin16(a->raw[0]); return s && *s?a->raw[0]+1:a->raw[0];}
DWORD W16_AnsiPrev(Args16 *a) {return LOWORD(a->raw[1])>LOWORD(a->raw[0])?a->raw[1]-1:a->raw[0];}

/* --- classes --------------------------------------------------------------------- */
/* WNDCLASS: style, procedure (far), class and window extra bytes, instance,
 * icon, cursor, brush, menu name and class name (far pointers). A native
 * class's procedure comes back as a procedure thunk, so that a program can
 * make a superclass of it. */
DWORD W16_GetClassInfo(Args16 *a) {
    WNDCLASS wc; LPCSTR name=HIWORD(a->raw[1])?(LPCSTR)Lin16(a->raw[1]):MAKEINTRESOURCE(LOWORD(a->raw[1]));
    BYTE *o=(BYTE *)PTR(a->a[2]); Class16 *c=class_named(name); HINSTANCE inst;
    if(!o || !name) return 0;
    inst=a->a[0]?instance_of((WORD)a->a[0]):NULL;
    if(!GetClassInfo(c?INSTANCE32(c->module->handle):inst,name,&wc) && !GetClassInfo(NULL,name,&wc)) return 0;
    put16(o,(WORD)wc.style); put32(o+2,c?c->proc:NativeProc16(wc.lpfnWndProc));
    put16(o+6,(WORD)wc.cbClsExtra); put16(o+8,(WORD)wc.cbWndExtra);
    put16(o+10,c?c->module->instance:0); put16(o+12,HICON16(wc.hIcon)); put16(o+14,HICON16(wc.hCursor));
    put16(o+16,brush16(wc.hbrBackground));
    put32(o+18,c && !HIWORD((ULONG_PTR)c->menu)?LOWORD((ULONG_PTR)c->menu):0); put32(o+22,a->raw[1]);
    return 1;
}
DWORD W16_UnregisterClass(Args16 *a) {
    LPCSTR name=(LPCSTR)PTR(a->a[0]); Class16 *c=class_named(name);
    if(!c || !UnregisterClass(name,INSTANCE32(c->module->handle))) return 0;
    c->used=FALSE;
    return 1;
}

/* --- clipboard ------------------------------------------------------------------ */
/* Bitmaps and palettes are GDI handles; other data is a global block,
 * copied into native memory (the clipboard then owns it, so the 16-bit
 * block goes) and out again into a block of the task's. A picture's
 * METAFILEPICT has 16-bit fields and its metafile is a block of its own. */
static BOOL gdi_format(UINT f) {return f==CF_BITMAP || f==CF_DSPBITMAP || f==CF_PALETTE;}
static BOOL picture_format(UINT f) {return f==CF_METAFILEPICT || f==CF_DSPMETAFILEPICT;}
static HANDLE picture32(WORD h) {
    const BYTE *p=GlobalSize16(h)>=8?(const BYTE *)SelPointer(h,0):NULL; HANDLE n; METAFILEPICT *m; HMETAFILE mf;
    if(!p || !(mf=Metafile32(get16(p+6)))) return NULL;
    if(!(n=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(METAFILEPICT))) || !(m=(METAFILEPICT *)GlobalLock(n))) {
        if(n) GlobalFree(n);
        DeleteMetaFile(mf); return NULL;
    }
    m->mm=(short)get16(p); m->xExt=(short)get16(p+2); m->yExt=(short)get16(p+4); m->hMF=mf;
    GlobalUnlock(n);
    GlobalFree16(get16(p+6));
    return n;
}
static WORD picture16(Task16 *t,HANDLE n) {
    const METAFILEPICT *m=(const METAFILEPICT *)GlobalLock(n); WORD sel=0,mf; BYTE *p;
    if(!m) return 0;
    if((mf=Metafile16(t,m->hMF,TRUE))!=0 && (sel=GlobalAlloc16(t,0,8))!=0) {
        p=(BYTE *)SelPointer(sel,0);
        put16(p,(WORD)m->mm); put16(p+2,(WORD)m->xExt); put16(p+4,(WORD)m->yExt); put16(p+6,mf);
    } else if(mf) GlobalFree16(mf);
    GlobalUnlock(n);
    return sel;
}
#define CLIP_COPIES 16
static struct {UINT format; HANDLE native; WORD sel; Task16 *task;} clip_copies[CLIP_COPIES];
static unsigned clip_next;
DWORD W16_SetClipboardData(Args16 *a) {
    UINT format=(UINT)a->a[0]; WORD h=(WORD)a->a[1]; DWORD size; HANDLE n; void *p;
    if(!h) {SetClipboardData(format,NULL); return 0;}
    if(gdi_format(format)) return SetClipboardData(format,HGDI32(h))?h:0;
    if(picture_format(format)) {
        if(!(n=picture32(h))) return 0;
        if(!SetClipboardData(format,n)) {const METAFILEPICT *m=(const METAFILEPICT *)GlobalLock(n); if(m) DeleteMetaFile(m->hMF); GlobalFree(n); return 0;}
        GlobalFree16(h);
        return h;
    }
    if(!(size=GlobalSize16(h)) || !(n=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,size))) return 0;
    if((p=GlobalLock(n))!=NULL) {memcpy(p,SelPointer(h,0),size); GlobalUnlock(n);}
    if(!p || !SetClipboardData(format,n)) {GlobalFree(n); return 0;}
    GlobalFree16(h);
    return h;
}
DWORD W16_GetClipboardData(Args16 *a) {
    UINT format=(UINT)a->a[0]; HANDLE n=GetClipboardData(format); DWORD size; const void *p; unsigned i; WORD sel;
    if(!n) return 0;
    if(gdi_format(format)) return HGDI16(n);
    for(i=0;i<CLIP_COPIES;i++)
        if(clip_copies[i].native==n && clip_copies[i].format==format && clip_copies[i].task==a->task && GlobalSize16(clip_copies[i].sel)) return clip_copies[i].sel;
    if(picture_format(format)) sel=picture16(a->task,n);
    else {
        if(!(size=GlobalSize(n)) || !(p=GlobalLock(n))) return 0;
        sel=GlobalAlloc16(a->task,0,size);
        if(sel) memcpy(SelPointer(sel,0),p,size);
        GlobalUnlock(n);
    }
    if(sel) {
        i=clip_next++%CLIP_COPIES;
        clip_copies[i].format=format; clip_copies[i].native=n; clip_copies[i].sel=sel; clip_copies[i].task=a->task;
    }
    return sel;
}

/* --- hooks ------------------------------------------------------------------------ */
/* One native hook per kind dispatches 16-bit hooks newest first.
 * Task hooks run only in their task; library SetWindowsHook, taskless
 * SetWindowsHookEx and journal hooks are global. Convert parameters through
 * task scratch structures and copy back. Use the current Win16 stack, or the
 * installing task's stack during native execution. CallNextHookEx/DefHookProc
 * continues 16-bit hooks then native hooks; SetWindowsHook returns the
 * DefHookProc chain value. Results, including journal waits, use AX. */
#define HOOKS16 32
#define KINDS (WH_SHELL-WH_MSGFILTER+1)
typedef struct {BOOL used; int id; DWORD proc; WORD task,owner; Module16 *owner_module; DWORD order;} Hook16;
static Hook16 hooks16[HOOKS16];
static HHOOK chains[KINDS];
static DWORD hook_order=1;
#define HOOK_COOKIE(h) MAKELONG((WORD)((h)-hooks16+1),0x4b48)
static Hook16 *hook16_of(DWORD cookie) {
    WORD i=LOWORD(cookie);
    return HIWORD(cookie)==0x4b48 && i && i<=HOOKS16 && hooks16[i-1].used?&hooks16[i-1]:NULL;
}
/* The task a hook runs in now: the current one, or the one that set it. */
static Task16 *hook_task(const Hook16 *h) {Task16 *t=CurrentTask16(); return t?t:Task16Of(h->owner);}
/* The next 16-bit hook of a kind, after the one set at 'below', that can run here. */
static Hook16 *next16(int id,DWORD below) {
    Hook16 *best=NULL; Task16 *cur=CurrentTask16(); WORD task=cur?cur->psp:0; unsigned i;
    for(i=0;i<HOOKS16;i++) {
        Hook16 *h=&hooks16[i];
        if(!h->used || h->id!=id || h->order>=below || (best && h->order<best->order)) continue;
        if(h->task && h->task!=task) continue;
        if(!cur && !Task16Of(h->owner)) continue;
        best=h;
    }
    return best;
}

/* Parameters by kind of hook and code. */
enum {P_PLAIN,P_MSG,P_CWP,P_CREATE,P_ACTIVATE,P_RECT,P_EVENT,P_MOUSE};
static int param_kind(int id,int code) {
    switch(id) {
    case WH_MSGFILTER: case WH_SYSMSGFILTER: case WH_GETMESSAGE: return code>=0?P_MSG:P_PLAIN;
    case WH_CALLWNDPROC: return code>=0?P_CWP:P_PLAIN;
    case WH_JOURNALRECORD: return code==HC_ACTION?P_EVENT:P_PLAIN;
    case WH_JOURNALPLAYBACK: return code==HC_GETNEXT?P_EVENT:P_PLAIN;
    case WH_MOUSE: return code>=0?P_MOUSE:P_PLAIN;
    case WH_CBT:
        switch(code) {
        case HCBT_CREATEWND: return P_CREATE;
        case HCBT_ACTIVATE: return P_ACTIVATE;
        case HCBT_MOVESIZE: return P_RECT;
        case HCBT_CLICKSKIPPED: return P_MOUSE;
        }
    }
    return P_PLAIN;
}
static BOOL window_wparam(int id,int code) {
    return id==WH_SHELL || (id==WH_CBT && code!=HCBT_SYSCOMMAND && code!=HCBT_CLICKSKIPPED && code!=HCBT_KEYSKIPPED && code!=HCBT_QS);
}
static BOOL window_lparam(int id,int code) {return id==WH_CBT && code==HCBT_SETFOCUS;}
typedef struct {
    MSG msg; CWPSTRUCT cwp; CBT_CREATEWND create; CREATESTRUCT cs; CBTACTIVATESTRUCT activate;
    RECT rect; EVENTMSG event; MOUSEHOOKSTRUCT mouse;
} HookParams;
/* MSG: MSG16 (msg_out16); CWP: lParam, wParam, message, window;
 * CBT_CREATEWND: CREATESTRUCT (far), insert after; CBTACTIVATESTRUCT:
 * mouse, active window; EVENTMSG: message, paramL, paramH, time;
 * MOUSEHOOKSTRUCT: position, window, hit test, extra information. */
static void mouse_out16(BYTE *p,const MOUSEHOOKSTRUCT *m) {
    put16(p,(WORD)m->pt.x); put16(p+2,(WORD)m->pt.y); put16(p+4,HWND16(m->hwnd)); put16(p+6,(WORD)m->wHitTestCode); put32(p+8,(DWORD)m->dwExtraInfo);
}
static void mouse_in16(const BYTE *p,MOUSEHOOKSTRUCT *m) {
    m->pt.x=(short)get16(p); m->pt.y=(short)get16(p+2); m->hwnd=HWND32(get16(p+4)); m->wHitTestCode=get16(p+6); m->dwExtraInfo=get32(p+8);
}
/* Native parameters to 16-bit ones in scratch memory; back after the call. */
static DWORD params_to16(Task16 *t,int kind,LPARAM lp,BYTE **out) {
    DWORD seg=0; BYTE *p=NULL;
    static const WORD sizes[]={0,18,10,6,4,8,10,12};
    if(kind==P_PLAIN || !lp || !(p=(BYTE *)Scratch16(t,sizes[kind],&seg))) {*out=NULL; return 0;}
    switch(kind) {
    case P_MSG: msg_out16(p,(const MSG *)lp); break;
    case P_CWP: {
        const CWPSTRUCT *c=(const CWPSTRUCT *)lp; WORD m16,w16; DWORD l16;
        Msg32To16(c->message,c->wParam,c->lParam,&m16,&w16,&l16);
        put32(p,l16); put16(p+4,w16); put16(p+6,m16); put16(p+8,HWND16(c->hwnd));
        break;
    }
    case P_CREATE: {
        const CBT_CREATEWND *c=(const CBT_CREATEWND *)lp; BYTE *cs;
        put32(p,CreateStructOut16(t,NULL,c->lpcs,&cs)); put16(p+4,HWND16(c->hwndInsertAfter));
        break;
    }
    case P_ACTIVATE: {const CBTACTIVATESTRUCT *a=(const CBTACTIVATESTRUCT *)lp; put16(p,(WORD)a->fMouse); put16(p+2,HWND16(a->hWndActive)); break;}
    case P_RECT: RectOut16(SelLinear(p),(const RECT *)lp); break;
    case P_EVENT: {
        const EVENTMSG *e=(const EVENTMSG *)lp;
        put16(p,(WORD)e->message); put16(p+2,(WORD)e->paramL); put16(p+4,(WORD)e->paramH); put32(p+6,e->time);
        break;
    }
    case P_MOUSE: mouse_out16(p,(const MOUSEHOOKSTRUCT *)lp); break;
    }
    *out=p; return seg;
}
static void params_back32(int kind,LPARAM lp,const BYTE *p) {
    if(!p) return;
    switch(kind) {
    case P_MSG: msg_in16(p,(MSG *)lp); break;
    case P_CREATE: {
        CREATESTRUCT *cs=((CBT_CREATEWND *)lp)->lpcs; const BYTE *c=(const BYTE *)Lin16(get32(p));
        if(c) {cs->cy=(short)get16(c+10); cs->cx=(short)get16(c+12); cs->y=(short)get16(c+14); cs->x=(short)get16(c+16);}
        break;
    }
    case P_RECT: RectIn16(SelLinear((void *)p),(RECT *)lp); break;
    case P_EVENT: {
        EVENTMSG *e=(EVENTMSG *)lp;
        e->message=get16(p); e->paramL=get16(p+2); e->paramH=get16(p+4); e->time=get32(p+6); e->hwnd=NULL;
        break;
    }
    }
}
/* 16-bit parameters to native ones; back after the native chain. */
static LPARAM params_to32(int kind,DWORD lp16,HookParams *n) {
    const BYTE *p=(const BYTE *)Lin16(lp16);
    if(kind==P_PLAIN || !p) return (LPARAM)lp16;
    switch(kind) {
    case P_MSG: msg_in16(p,&n->msg); return (LPARAM)&n->msg;
    case P_CWP: {
        UINT m; WPARAM w; LPARAM l;
        Msg16To32(get16(p+6),get16(p+4),get32(p),&m,&w,&l);
        n->cwp.message=m; n->cwp.wParam=w; n->cwp.lParam=l; n->cwp.hwnd=HWND32(get16(p+8));
        return (LPARAM)&n->cwp;
    }
    case P_CREATE: {
        const BYTE *c=(const BYTE *)Lin16(get32(p)); DWORD cls;
        memset(&n->cs,0,sizeof(n->cs));
        if(c) {
            cls=get32(c+26);
            n->cs.lpCreateParams=(LPVOID)(ULONG_PTR)get32(c); n->cs.hInstance=Instance32(get16(c+4));
            n->cs.hwndParent=HWND32(get16(c+8)); n->cs.style=(LONG)get32(c+18);
            n->cs.hMenu=n->cs.style&WS_CHILD?(HMENU)(ULONG_PTR)get16(c+6):HMENU32(get16(c+6));
            n->cs.cy=(short)get16(c+10); n->cs.cx=(short)get16(c+12); n->cs.y=(short)get16(c+14); n->cs.x=(short)get16(c+16);
            n->cs.lpszName=(LPCSTR)Lin16(get32(c+22)); n->cs.lpszClass=HIWORD(cls)?(LPCSTR)Lin16(cls):MAKEINTRESOURCE(LOWORD(cls));
            n->cs.dwExStyle=get32(c+30);
        }
        n->create.lpcs=&n->cs; n->create.hwndInsertAfter=HWND32(get16(p+4));
        return (LPARAM)&n->create;
    }
    case P_ACTIVATE: n->activate.fMouse=get16(p); n->activate.hWndActive=HWND32(get16(p+2)); return (LPARAM)&n->activate;
    case P_RECT: RectIn16(SelLinear((void *)p),&n->rect); return (LPARAM)&n->rect;
    case P_EVENT:
        n->event.message=get16(p); n->event.paramL=get16(p+2); n->event.paramH=get16(p+4); n->event.time=get32(p+6); n->event.hwnd=NULL;
        return (LPARAM)&n->event;
    case P_MOUSE: mouse_in16(p,&n->mouse); return (LPARAM)&n->mouse;
    }
    return (LPARAM)lp16;
}
static void params_back16(int kind,const HookParams *n,DWORD lp16) {
    BYTE *p=(BYTE *)Lin16(lp16);
    if(!p) return;
    switch(kind) {
    case P_MSG: msg_out16(p,&n->msg); break;
    case P_CREATE: {
        BYTE *c=(BYTE *)Lin16(get32(p));
        if(c) {put16(c+10,(WORD)n->cs.cy); put16(c+12,(WORD)n->cs.cx); put16(c+14,(WORD)n->cs.y); put16(c+16,(WORD)n->cs.x);}
        break;
    }
    case P_RECT: RectOut16(SelLinear(p),&n->rect); break;
    case P_EVENT: put16(p,(WORD)n->event.message); put16(p+2,(WORD)n->event.paramL); put16(p+4,(WORD)n->event.paramH); put32(p+6,n->event.time); break;
    }
}

static LRESULT run16(const Hook16 *h,int code,WORD wp16,DWORD lp16) {
    static const BYTE sizes[3]={2,2,4}; DWORD args[3]; Task16 *t=hook_task(h);
    if(!t) return 0;
    args[0]=(WORD)code; args[1]=wp16; args[2]=lp16;
    return (LRESULT)(short)LOWORD(Call16(t,h->proc,3,args,sizes));
}
/* A native hook's call: the first 16-bit hook, its parameters converted. */
static LRESULT dispatch(int id,int code,WPARAM wp,LPARAM lp) {
    Hook16 *h=next16(id,0xffffffffUL); Task16 *t; WORD mark,wp16; DWORD lp16; BYTE *p; int kind; LRESULT r;
    if(!h || !(t=hook_task(h))) return CallNextHookEx(chains[id-WH_MSGFILTER],code,wp,lp);
    kind=param_kind(id,code); mark=t->scratch_top;
    wp16=window_wparam(id,code)?HWND16((HWND)wp):(WORD)wp;
    lp16=window_lparam(id,code)?HWND16((HWND)lp):kind?params_to16(t,kind,lp,&p):(DWORD)lp;
    if(kind && !lp16 && lp) {t->scratch_top=mark; return CallNextHookEx(chains[id-WH_MSGFILTER],code,wp,lp);}
    r=run16(h,code,wp16,lp16);
    if(kind) params_back32(kind,lp,p);
    t->scratch_top=mark;
    return r;
}
#define DISPATCHER(name,id) static LRESULT CALLBACK name(int code,WPARAM wp,LPARAM lp) {return dispatch(id,code,wp,lp);}
DISPATCHER(hook_msgfilter,WH_MSGFILTER) DISPATCHER(hook_record,WH_JOURNALRECORD) DISPATCHER(hook_playback,WH_JOURNALPLAYBACK)
DISPATCHER(hook_keyboard,WH_KEYBOARD) DISPATCHER(hook_getmessage,WH_GETMESSAGE) DISPATCHER(hook_callwndproc,WH_CALLWNDPROC)
DISPATCHER(hook_cbt,WH_CBT) DISPATCHER(hook_sysmsgfilter,WH_SYSMSGFILTER) DISPATCHER(hook_mouse,WH_MOUSE)
DISPATCHER(hook_hardware,WH_HARDWARE) DISPATCHER(hook_debug,WH_DEBUG) DISPATCHER(hook_shell,WH_SHELL)
static const HOOKPROC dispatchers[KINDS]={hook_msgfilter,hook_record,hook_playback,hook_keyboard,hook_getmessage,
    hook_callwndproc,hook_cbt,hook_sysmsgfilter,hook_mouse,hook_hardware,hook_debug,hook_shell};

/* The hook after h: the next 16-bit one with the 16-bit parameters as they
 * are, or the native chain with them converted. */
static LRESULT call_next16(Hook16 *h,int code,WORD wp16,DWORD lp16) {
    Hook16 *n=next16(h->id,h->order); int kind; HookParams params; LPARAM lp; WPARAM wp; LRESULT r;
    if(n) return run16(n,code,wp16,lp16);
    kind=param_kind(h->id,code);
    wp=window_wparam(h->id,code)?(WPARAM)HWND32(wp16):wp16;
    lp=window_lparam(h->id,code)?(LPARAM)HWND32(LOWORD(lp16)):params_to32(kind,lp16,&params);
    r=CallNextHookEx(chains[h->id-WH_MSGFILTER],code,wp,lp);
    if(kind && Lin16(lp16)) params_back16(kind,&params,lp16);
    return r;
}
static DWORD add_hook16(Args16 *a,int id,DWORD proc,WORD task) {
    unsigned i; Hook16 *h=NULL; int k=id-WH_MSGFILTER;
    if(id<WH_MSGFILTER || id>WH_SHELL || !proc) return 0;
    if(id==WH_JOURNALRECORD || id==WH_JOURNALPLAYBACK) task=0;
    for(i=0;i<HOOKS16 && !h;i++) if(!hooks16[i].used) h=&hooks16[i];
    if(!h) return 0;
    if(!chains[k] && !(chains[k]=SetWindowsHookEx(id,dispatchers[k],wow_instance,0))) return 0;
    h->used=TRUE; h->id=id; h->proc=proc; h->task=task; h->owner=a->task->psp; h->owner_module=a->task->module; h->order=hook_order++;
    return HOOK_COOKIE(h);
}
static void remove_hook16(Hook16 *h) {
    unsigned i; int id=h->id;
    h->used=FALSE;
    for(i=0;i<HOOKS16;i++) if(hooks16[i].used && hooks16[i].id==id) return;
    if(chains[id-WH_MSGFILTER]) {UnhookWindowsHookEx(chains[id-WH_MSGFILTER]); chains[id-WH_MSGFILTER]=NULL;}
}
/* Windows 3.0's: a library's hook is the whole system's, a program's its own. */
DWORD W16_SetWindowsHook(Args16 *a) {
    Module16 *m=NeFromCode(HIWORD(a->a[1]));
    return add_hook16(a,(int)(LONG)a->a[0],a->a[1],m && m->library?0:a->task->psp);
}
DWORD W16_UnhookWindowsHook(Args16 *a) {
    unsigned i; int id=(int)(LONG)a->a[0];
    for(i=0;i<HOOKS16;i++) if(hooks16[i].used && hooks16[i].id==id && hooks16[i].proc==a->a[1]) {remove_hook16(&hooks16[i]); return 1;}
    return 0;
}
DWORD W16_DefHookProc(Args16 *a) {
    const BYTE *next=(const BYTE *)PTR(a->a[3]); Hook16 *h=next?hook16_of(get32(next)):NULL;
    return h?(DWORD)call_next16(h,(int)(LONG)a->a[0],(WORD)a->a[1],a->a[2]):0;
}
DWORD W16_SetWindowsHookEx(Args16 *a) {return add_hook16(a,(int)(LONG)a->a[0],a->a[1],(WORD)a->a[3]);}
DWORD W16_UnhookWindowsHookEx(Args16 *a) {Hook16 *h=hook16_of(a->a[0]); if(!h) return 0; remove_hook16(h); return 1;}
DWORD W16_CallNextHookEx(Args16 *a) {
    Hook16 *h=hook16_of(a->a[0]);
    return h?(DWORD)call_next16(h,(int)(LONG)a->a[1],(WORD)a->a[2],a->a[3]):0;
}
/* A task's hooks, and those in a library that goes, end with them. */
static void hooks16_ended(Module16 *m) {
    unsigned i;
    for(i=0;i<HOOKS16;i++) if(hooks16[i].used && (hooks16[i].owner_module==m || NeFromCode(HIWORD(hooks16[i].proc))==m)) remove_hook16(&hooks16[i]);
}

/* --- MDI -------------------------------------------------------------------------- */
/* DefFrameProc has the client window as a fifth argument. */
static HWND frame_client;
static LRESULT WINAPI frame_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefFrameProc(h,frame_client,msg,wp,lp);}
DWORD W16_DefFrameProc(Args16 *a) {
    HWND was=frame_client; DWORD r;
    frame_client=HWND32(a->a[1]);
    r=CallNative16(a->task,frame_proc,(WORD)a->a[0],(WORD)a->a[2],(WORD)a->a[3],a->a[4]);
    frame_client=was;
    return r;
}
DWORD W16_DefMDIChildProc(Args16 *a) {
    return CallNative16(a->task,DefMDIChildProc,(WORD)a->a[0],(WORD)a->a[1],(WORD)a->a[2],a->a[3]);
}
DWORD W16_TranslateMDISysAccel(Args16 *a) {
    MSG m; const BYTE *p=(const BYTE *)PTR(a->a[1]);
    if(!p) return 0;
    msg_from16(&m,p);
    return TranslateMDISysAccel(HWND32(a->a[0]),&m);
}

/* --- the rest of USER ------------------------------------------------------------- */
DWORD W16_PostAppMessage(Args16 *a) {
    Task16 *t=Task16Of((WORD)a->a[0]); UINT msg; WPARAM wp; LPARAM lp;
    if(!t) return 0;
    Msg16To32((WORD)a->a[1],(WORD)a->a[2],a->a[3],&msg,&wp,&lp);
    return PostAppMessage(t->native,msg,wp,lp);
}
DWORD W16_CallMsgFilter(Args16 *a) {
    MSG m; const BYTE *p=(const BYTE *)PTR(a->a[0]);
    if(!p) return 0;
    msg_from16(&m,p);
    return CallMsgFilter(&m,(int)a->a[1]);
}
DWORD W16_SetSysColors(Args16 *a) {
    int n=(int)a->a[0],i,which[32]; COLORREF colors[32]; const BYTE *w=(const BYTE *)PTR(a->a[1]),*c=(const BYTE *)PTR(a->a[2]);
    if(n<=0 || !w || !c) return 0;
    if(n>32) n=32;
    for(i=0;i<n;i++) {which[i]=(short)get16(w+i*2); colors[i]=get32(c+i*4);}
    SetSysColors(n,which,colors);
    return 0;
}
static int tab_stops(DWORD p,int n,int *tabs) {
    const BYTE *s=(const BYTE *)PTR(p); int i;
    if(!s || n<=0) return 0;
    if(n>32) n=32;
    for(i=0;i<n;i++) tabs[i]=(short)get16(s+i*2);
    return n;
}
/* hdc, x, y, string, count, tab count, tabs, origin */
DWORD W16_TabbedTextOut(Args16 *a) {
    int tabs[32],n=tab_stops(a->a[6],(int)a->a[5],tabs);
    return (DWORD)TabbedTextOut((HDC)HGDI32(a->a[0]),(int)a->a[1],(int)a->a[2],(LPCSTR)PTR(a->a[3]),(int)a->a[4],n,n?tabs:NULL,(int)a->a[7]);
}
DWORD W16_GetTabbedTextExtent(Args16 *a) {
    int tabs[32],n=tab_stops(a->a[4],(int)a->a[3],tabs);
    return GetTabbedTextExtent((HDC)HGDI32(a->a[0]),(LPCSTR)PTR(a->a[1]),(int)a->a[2],n,n?tabs:NULL);
}
/* GrayString: a 16-bit output procedure (hdc, data, count), or the text. */
typedef struct {Task16 *task; DWORD proc;} Gray16;
static Gray16 *gray_now;
static BOOL CALLBACK gray_proc(HDC dc,LPARAM data,int count) {
    static const BYTE sizes[3]={2,4,2}; DWORD args[3];
    args[0]=HGDI16(dc); args[1]=(DWORD)data; args[2]=(WORD)count;
    return LOWORD(Call16(gray_now->task,gray_now->proc,3,args,sizes))!=0;
}
DWORD W16_GrayString(Args16 *a) {
    HDC dc=(HDC)HGDI32(a->a[0]); Gray16 g,*was=gray_now; BOOL r;
    if(!a->a[2]) return GrayString(dc,Brush32((WORD)a->a[1]),NULL,(LPARAM)Lin16(a->a[3]),(int)a->a[4],(int)a->a[5],(int)a->a[6],(int)a->a[7],(int)a->a[8]);
    g.task=a->task; g.proc=a->a[2]; gray_now=&g;
    r=GrayString(dc,Brush32((WORD)a->a[1]),(FARPROC)gray_proc,(LPARAM)a->a[3],(int)a->a[4],(int)a->a[5],(int)a->a[6],(int)a->a[7],(int)a->a[8]);
    gray_now=was;
    return r;
}
/* SystemParametersInfo: values come back as 16-bit integers (three for
 * SPI_GETMOUSE), the icon title font as a LOGFONT of 16-bit fields. */
DWORD W16_SystemParametersInfo(Args16 *a) {
    UINT action=(UINT)a->a[0],param=(UINT)a->a[1]; BYTE *o=(BYTE *)PTR(a->a[2]); int v[4]; LOGFONT lf; BOOL r;
    memset(v,0,sizeof(v));
    switch(action) {
    case 31: /* SPI_GETICONTITLELOGFONT */
        memset(&lf,0,sizeof(lf)); lf.lfHeight=-8; lf.lfWeight=FW_NORMAL; lstrcpy(lf.lfFaceName,"Helv");
        if(o) LogFontOut16(o,&lf);
        return 1;
    case 3: /* SPI_GETMOUSE */
        v[0]=6; v[1]=10; v[2]=1;
        if(o) {put16(o,(WORD)v[0]); put16(o+2,(WORD)v[1]); put16(o+4,(WORD)v[2]);}
        return 1;
    case 1: case 5: case 10: case 13: case 14: case 16: case 18: case 22: case 24: case 25: case 27: case 32: case 38:
        r=SystemParametersInfo(action,param,v,0);
        if(!r) switch(action) {
            case 10: v[0]=31; break;              /* SPI_GETKEYBOARDSPEED */
            case 14: v[0]=600; break;             /* SPI_GETSCREENSAVETIMEOUT */
            case 22: v[0]=1; break;               /* SPI_GETKEYBOARDDELAY */
            case 24: v[0]=75; break;              /* SPI_ICONVERTICALSPACING */
            case 25: case 32: v[0]=1; break;      /* SPI_GETICONTITLEWRAP, SPI_GETFASTTASKSWITCH */
            default: v[0]=0;
        }
        if(o) put16(o,(WORD)v[0]);
        return 1;
    case 2: case 11: /* SPI_SETBEEP, SPI_SETKEYBOARDSPEED */
        return SystemParametersInfo(action,param,NULL,(UINT)a->a[3]);
    default:
        return 1;
    }
}
/* RedrawWindow (Windows 3.1): invalidate or validate, then paint now. */
#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
DWORD W16_RedrawWindow(Args16 *a) {
    HWND h=HWND32(a->a[0]); RECT r; BOOL has=RectIn16(a->a[1],&r); HRGN rgn=(HRGN)HGDI32(a->a[2]); UINT flags=(UINT)a->a[3];
    if(flags&RDW_INVALIDATE) {if(rgn) InvalidateRgn(h,rgn,(flags&RDW_ERASE)!=0); else InvalidateRect(h,has?&r:NULL,(flags&RDW_ERASE)!=0);}
    if(flags&RDW_VALIDATE) {if(rgn) ValidateRgn(h,rgn); else ValidateRect(h,has?&r:NULL);}
    if(flags&(RDW_UPDATENOW|RDW_ERASENOW)) UpdateWindow(h);
    return 1;
}
/* WinHelp: a keyword or macro is a far pointer, as is a MULTIKEYHELP, whose
 * size is a word in Win16 (a doubleword natively). */
DWORD W16_WinHelp(Args16 *a) {
    UINT command=(UINT)a->a[2]; ULONG_PTR data=a->a[3]; static BYTE multikey[300];
    switch(command) {
    case HELP_KEY: case HELP_PARTIALKEY: case HELP_COMMAND: data=(ULONG_PTR)Lin16(a->a[3]); break;
    case HELP_MULTIKEY: {
        const BYTE *m=(const BYTE *)Lin16(a->a[3]); MULTIKEYHELP *k=(MULTIKEYHELP *)multikey; int n;
        if(!m || get16(m)<4) return 0;
        n=get16(m)-3; if(n>(int)sizeof(multikey)-6) n=(int)sizeof(multikey)-6;
        k->mkKeylist=(char)m[2]; memcpy(k->szKeyphrase,m+3,(size_t)n); k->szKeyphrase[n]=0;
        k->mkSize=(DWORD)(5+lstrlen(k->szKeyphrase)+1); data=(ULONG_PTR)k;
        break;
    }
    }
    return (DWORD)WinHelp(HWND32(a->a[0]),(LPCSTR)PTR(a->a[1]),command,data);
}
DWORD W16_GetFreeSystemResources(Args16 *a) {(void)a; return 90;}
DWORD W16_EnableScrollBar(Args16 *a) {(void)a; return 1;}

/* --- communications ------------------------------------------------------------- */
/* DCB: id, speed (Windows 3.1's CBR_ indexes taken too), data bits, parity, stop
 * bits, three timeouts, two bytes of flags, XON/XOFF characters and limits, the
 * parity, end and event characters, the transmit delay: 25 bytes. */
static UINT baud16(WORD v) {
    static const DWORD indexes[24]={110,300,600,1200,2400,4800,9600,14400,19200,0,0,38400,0,0,0,56000,0,0,0,128000,0,0,0,256000};
    return v>=0xff10 && v<0xff10+24?(UINT)indexes[v-0xff10]:v;
}
static void dcb_in16(const BYTE *p,DCB *d) {
    memset(d,0,sizeof(*d));
    d->Id=p[0]; d->BaudRate=baud16(get16(p+1)); d->ByteSize=p[3]; d->Parity=p[4]; d->StopBits=p[5];
    d->RlsTimeout=get16(p+6); d->CtsTimeout=get16(p+8); d->DsrTimeout=get16(p+10);
    d->fBinary=p[12]&1; d->fRtsDisable=p[12]>>1&1; d->fParity=p[12]>>2&1; d->fOutxCtsFlow=p[12]>>3&1;
    d->fOutxDsrFlow=p[12]>>4&1; d->fDtrDisable=p[12]>>7&1;
    d->fOutX=p[13]&1; d->fInX=p[13]>>1&1; d->fPeChar=p[13]>>2&1; d->fNull=p[13]>>3&1; d->fChEvt=p[13]>>4&1;
    d->fDtrflow=p[13]>>5&1; d->fRtsflow=p[13]>>6&1;
    d->XonChar=(char)p[14]; d->XoffChar=(char)p[15]; d->XonLim=get16(p+16); d->XoffLim=get16(p+18);
    d->PeChar=(char)p[20]; d->EofChar=(char)p[21]; d->EvtChar=(char)p[22]; d->TxDelay=get16(p+23);
}
static void dcb_out16(BYTE *p,const DCB *d) {
    p[0]=d->Id; put16(p+1,(WORD)d->BaudRate); p[3]=d->ByteSize; p[4]=d->Parity; p[5]=d->StopBits;
    put16(p+6,(WORD)d->RlsTimeout); put16(p+8,(WORD)d->CtsTimeout); put16(p+10,(WORD)d->DsrTimeout);
    p[12]=(BYTE)(d->fBinary|d->fRtsDisable<<1|d->fParity<<2|d->fOutxCtsFlow<<3|d->fOutxDsrFlow<<4|d->fDtrDisable<<7);
    p[13]=(BYTE)(d->fOutX|d->fInX<<1|d->fPeChar<<2|d->fNull<<3|d->fChEvt<<4|d->fDtrflow<<5|d->fRtsflow<<6);
    p[14]=(BYTE)d->XonChar; p[15]=(BYTE)d->XoffChar; put16(p+16,(WORD)d->XonLim); put16(p+18,(WORD)d->XoffLim);
    p[20]=(BYTE)d->PeChar; p[21]=(BYTE)d->EofChar; p[22]=(BYTE)d->EvtChar; put16(p+23,(WORD)d->TxDelay);
}
DWORD W16_GetCommState(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[1]); DCB d; int r;
    if(!p) return (DWORD)-1;
    if(!(r=GetCommState((int)(short)a->a[0],&d))) dcb_out16(p,&d);
    return (DWORD)r;
}
DWORD W16_SetCommState(Args16 *a) {
    const BYTE *p=(const BYTE *)PTR(a->a[0]); DCB d;
    if(!p) return (DWORD)IE_BADID;
    dcb_in16(p,&d);
    return (DWORD)SetCommState(&d);
}
DWORD W16_BuildCommDCB(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[1]); DCB d; int r;
    if(!p) return (DWORD)-1;
    if(!(r=BuildCommDCB((LPCSTR)PTR(a->a[0]),&d))) dcb_out16(p,&d);
    return (DWORD)r;
}
/* COMSTAT: the status byte and the queues' counts (words). */
DWORD W16_GetCommError(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[1]); COMSTAT s; int r=GetCommError((int)(short)a->a[0],&s);
    if(p) {p[0]=s.status; put16(p+1,(WORD)s.cbInQue); put16(p+3,(WORD)s.cbOutQue);}
    return (DWORD)r;
}
/* The event word a Win16 program reads through its far pointer is a word of
 * 16-bit memory for each port, brought up to date when it asks. */
#define COMM16 12
static struct {int id; WORD sel;} events16[COMM16];
static int event_slot16(int id,Task16 *t) {
    int i,unused=-1;
    for(i=0;i<COMM16;i++) {
        if(events16[i].sel && events16[i].id==id) return i;
        if(unused<0 && !events16[i].sel) unused=i;
    }
    if(unused<0 || !t || !(events16[unused].sel=GlobalAlloc16(t,GMEM_FIXED|GMEM_ZEROINIT,2))) return -1;
    events16[unused].id=id;
    return unused;
}
static void event_word16(int id,UINT events) {
    int i;
    for(i=0;i<COMM16;i++) if(events16[i].sel && events16[i].id==id) {
        BYTE *w=(BYTE *)Lin16(MAKELONG(0,events16[i].sel));
        if(w) put16(w,(WORD)events);
    }
}
DWORD W16_SetCommEventMask(Args16 *a) {
    int id=(int)(short)a->a[0],i;
    if(!SetCommEventMask(id,(UINT)a->a[1]) || (i=event_slot16(id,a->task))<0) return 0;
    event_word16(id,0);
    return MAKELONG(0,events16[i].sel);
}
DWORD W16_GetCommEventMask(Args16 *a) {
    int id=(int)(short)a->a[0]; UINT events=GetCommEventMask(id,(int)(short)a->a[1]);
    event_word16(id,events&~(UINT)(WORD)a->a[1]);
    return (WORD)events;
}
DWORD W16_CloseComm(Args16 *a) {
    int id=(int)(short)a->a[0],i;
    for(i=0;i<COMM16;i++) if(events16[i].sel && events16[i].id==id) {GlobalFree16(events16[i].sel); events16[i].sel=0;}
    return (DWORD)CloseComm(id);
}
