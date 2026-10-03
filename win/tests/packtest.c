/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native WIN16_MESSAGES tests with WORD wParam/LONG lParam procedures.
 * Cover USER notifications, activation/creation/dialogs, forwarded edit and
 * WM_CTLCOLOR messages, windowsx.h crackers, WORD-sized memory handles,
 * DDE with native peers, hooks chained to native hooks, and packed MSG loops.
 * OutputDebugString reports each check; exit status counts failures.
 */
#define WIN16_MESSAGES
#include <windows.h>
#include <windowsx.h>
#include <dde.h>
#include <string.h>
#include <stdio.h>
#define ID_EDIT 10
#define ID_SCROLL 11
#define ID_LATE 12
#define ID_HOOKED 13
#define ID_CHANGED 14
#define ID_LOOP 15

static int failures;
static HWND main_wnd,edit,scroll;
static HANDLE instance;
static FARPROC old_edit;
/* What the window procedure saw. */
static struct {WORD wp; LONG lp; BOOL seen;} command,colour,hscroll,activate,notify;
static WORD subclassed_chars;
static struct {int id; HWND ctl; UINT code;} cracked;
/* DDE as this program sees it, and as the native windows do. */
static struct {BOOL initiating,advised,terminated; HWND server,client; WORD status,value,item; char data[16];} dde;
static struct {HWND server,client,partner; BOOL initiating,executed,terminated; WORD status,item; char poked[16],data[16];} native;
static struct {HHOOK getmsg; WPARAM wp; LPARAM lp;} native_hook;
static void native_windows(HANDLE instance);
static BOOL native_advise(void);
static void native_hooks(BOOL set);
/* Windows 3.0's hooks: what SetWindowsHook gave, for DefHookProc. */
static HOOKPROC next_getmsg,next_callwnd;
static struct {BOOL seen; WORD wp; LONG lp;} hooked,called;
static void pump(void) {MSG msg; while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)) DispatchMessage(&msg);}
static BOOL atom_is(WORD atom,const char *name) {char text[32]; return GlobalGetAtomName(atom,text,sizeof(text)) && !lstrcmpi(text,name);}
static void OnCommand(HWND hwnd,int id,HWND ctl,UINT code) {(void)hwnd; cracked.id=id; cracked.ctl=ctl; cracked.code=code;}

static void check(const char *name,BOOL ok,const char *got) {
    char line[160];
    if(ok) sprintf(line,"PACKTEST: %s ok",name);
    else {sprintf(line,"PACKTEST: %s FAILED %s",name,got?got:""); failures++;}
    OutputDebugString(line);
}

LONG FAR PASCAL MainProc(HWND hwnd,unsigned message,WORD wParam,LONG lParam) {
    switch(message) {
    case WM_COMMAND:
        if(wParam==ID_EDIT && HIWORD(lParam)==EN_CHANGE) {command.wp=wParam; command.lp=lParam; command.seen=TRUE;}
        return HANDLE_WM_COMMAND(hwnd,wParam,lParam,OnCommand);
    case WM_CTLCOLOR:
        if(HIWORD(lParam)==CTLCOLOR_EDIT) {colour.wp=wParam; colour.lp=lParam; colour.seen=TRUE;}
        return DefWindowProc(hwnd,message,wParam,lParam);
    case WM_HSCROLL: if(wParam==SB_LINEDOWN) {hscroll.wp=wParam; hscroll.lp=lParam; hscroll.seen=TRUE;} return 0;
    case WM_ACTIVATE:
        if(wParam==WA_ACTIVE) {activate.wp=wParam; activate.lp=lParam; activate.seen=TRUE;}
        return DefWindowProc(hwnd,message,wParam,lParam);
    case WM_PARENTNOTIFY:
        if(wParam==WM_CREATE && HIWORD(lParam)==ID_LATE) {notify.wp=wParam; notify.lp=lParam; notify.seen=TRUE;}
        return 0;
    case WM_USER: return -1L;
    /* As a DDE client: the server's acknowledgements, its data, its end. */
    case WM_DDE_ACK:
        if(dde.initiating) {
            if(!dde.server) dde.server=(HWND)wParam;
            GlobalDeleteAtom(LOWORD(lParam)); GlobalDeleteAtom(HIWORD(lParam));
        } else if((HWND)wParam==dde.server) {dde.status=LOWORD(lParam); dde.value=HIWORD(lParam);}
        return 0;
    case WM_DDE_DATA: {
        HANDLE data=(HANDLE)LOWORD(lParam); DDEDATA FAR *d=(DDEDATA FAR *)GlobalLock(data); BOOL release=FALSE;
        if(d) {lstrcpyn(dde.data,(LPSTR)d->Value,sizeof(dde.data)); release=d->fRelease; GlobalUnlock(data);}
        if(release) GlobalFree(data);
        dde.item=HIWORD(lParam);
        return 0;
    }
    /* As a DDE server: a native client's advise loop on "Clock", and its first data at once. */
    case WM_DDE_INITIATE:
        if((HWND)wParam!=hwnd && atom_is(LOWORD(lParam),"PackMain") && atom_is(HIWORD(lParam),"Mirror")) {
            dde.client=(HWND)wParam;
            SendMessage((HWND)wParam,WM_DDE_ACK,(WPARAM)hwnd,MAKELONG(GlobalAddAtom("PackMain"),GlobalAddAtom("Mirror")));
        }
        return 0;
    case WM_DDE_ADVISE: {
        HANDLE options=(HANDLE)LOWORD(lParam),data; DDEADVISE FAR *o=(DDEADVISE FAR *)GlobalLock(options); DDEDATA FAR *d;
        dde.advised=o && o->cfFormat==CF_TEXT && atom_is(HIWORD(lParam),"Clock");
        if(o) GlobalUnlock(options);
        if(dde.advised) GlobalFree(options);
        PostMessage((HWND)wParam,WM_DDE_ACK,(WPARAM)hwnd,MAKELONG(dde.advised?0x8000:0,HIWORD(lParam)));
        if(dde.advised && (data=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)sizeof(DDEDATA)+8))!=NULL) {
            d=(DDEDATA FAR *)GlobalLock(data);
            d->fResponse=0; d->fRelease=1; d->fAckReq=0; d->cfFormat=CF_TEXT; lstrcpy((LPSTR)d->Value,"tick");
            GlobalUnlock(data);
            PostMessage((HWND)wParam,WM_DDE_DATA,(WPARAM)hwnd,MAKELONG(data,GlobalAddAtom("Clock")));
        }
        return 0;
    }
    case WM_DDE_TERMINATE:
        if((HWND)wParam==dde.server) dde.terminated=TRUE;
        else if((HWND)wParam==dde.client) PostMessage((HWND)wParam,WM_DDE_TERMINATE,(WPARAM)hwnd,0L);
        return 0;
    }
    return DefWindowProc(hwnd,message,wParam,lParam);
}
/* A message hook: the command posted is seen as Windows 3.0 packs it, and changed. */
DWORD FAR PASCAL GetMsgHook(int code,WORD wParam,LONG lParam) {
    LPMSG m=(LPMSG)lParam;
    if(code>=0 && m->hwnd==main_wnd && m->message==WM_COMMAND && m->wParam==ID_HOOKED) {
        hooked.seen=TRUE; hooked.wp=(WORD)m->wParam; hooked.lp=m->lParam;
        m->wParam=ID_CHANGED;
    }
    return DefHookProc(code,wParam,lParam,&next_getmsg);
}
/* A sent message, as Windows 3.0 packs it. */
DWORD FAR PASCAL CallWndHook(int code,WORD wParam,LONG lParam) {
    CWPSTRUCT FAR *c=(CWPSTRUCT FAR *)lParam;
    if(code>=0 && c->hwnd==main_wnd && c->message==WM_HSCROLL && c->wParam==SB_LINEDOWN) {called.seen=TRUE; called.wp=(WORD)c->wParam; called.lp=c->lParam;}
    return DefHookProc(code,wParam,lParam,&next_callwnd);
}
/* The edit control subclassed: characters counted, then passed on. */
LONG FAR PASCAL EditProc(HWND hwnd,unsigned message,WORD wParam,LONG lParam) {
    if(message==WM_CHAR) subclassed_chars++;
    return CallWindowProc((WNDPROC)old_edit,hwnd,message,wParam,lParam);
}
BOOL FAR PASCAL DialogProc(HWND hdlg,unsigned message,WORD wParam,LONG lParam) {
    switch(message) {
    case WM_INITDIALOG:
        PostMessage(hdlg,WM_COMMAND,IDOK,MAKELONG(GetDlgItem(hdlg,IDOK),BN_CLICKED));
        return TRUE;
    case WM_COMMAND:
        if(wParam==IDOK && LOWORD(lParam)==(WORD)GetDlgItem(hdlg,IDOK) && HIWORD(lParam)==BN_CLICKED) EndDialog(hdlg,42);
        return TRUE;
    }
    return FALSE;
}
/* A dialog with one OK button, as a template in memory. */
static WORD *wide(WORD *p,const char *s) {while(*s) *p++=(BYTE)*s++; *p++=0; return p;}
static int dialog(void) {
    static DWORD buffer[64]; WORD *p=(WORD *)buffer;
    memset(buffer,0,sizeof(buffer));
    *(DWORD *)p=WS_POPUP|WS_CAPTION|DS_MODALFRAME; p+=2; *(DWORD *)p=0; p+=2;
    *p++=1; *p++=10; *p++=10; *p++=80; *p++=40;
    *p++=0; *p++=0; p=wide(p,"Packing");
    while((p-(WORD *)buffer)&1) *p++=0;
    *(DWORD *)p=WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON; p+=2; *(DWORD *)p=0; p+=2;
    *p++=20; *p++=20; *p++=40; *p++=14; *p++=IDOK;
    *p++=0xffff; *p++=0x80; p=wide(p,"OK"); *p++=0;
    return (int)DialogBoxIndirect(instance,(LPCDLGTEMPLATE)buffer,main_wnd,(DLGPROC)MakeProcInstance((FARPROC)DialogProc,instance));
}

int PASCAL WinMain(HANDLE hInstance,HANDLE hPrevInstance,LPSTR lpCmdLine,int nCmdShow) {
    WNDCLASS wc; char text[64]; LONG sel; MSG msg; int i; HANDLE h; LPSTR p; ATOM app,topic,item; DDEPOKE FAR *poke;
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    instance=hInstance;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=(WNDPROC)MainProc; wc.hInstance=hInstance; wc.lpszClassName="PackTest";
    wc.hbrBackground=GetStockObject(WHITE_BRUSH); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClass(&wc)) return 1;
    main_wnd=CreateWindow("PackTest","Packing",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,hInstance,NULL);
    edit=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,10,10,200,24,main_wnd,(HMENU)ID_EDIT,hInstance,NULL);
    scroll=CreateWindow("SCROLLBAR","",WS_CHILD|WS_VISIBLE|SBS_HORZ,10,50,200,16,main_wnd,(HMENU)ID_SCROLL,hInstance,NULL);
    ShowWindow(main_wnd,SW_SHOWNORMAL); UpdateWindow(main_wnd);

    check("GetWindowWord",(HANDLE)GetWindowWord(main_wnd,GWW_HINSTANCE)==hInstance,NULL);
    sprintf(text,"wp %04x lp %08lx",activate.wp,activate.lp);
    check("WM_ACTIVATE",activate.seen && activate.wp==WA_ACTIVE && HIWORD(activate.lp)==0,text);

    /* A notification from the edit control: the code in lParam's high word. */
    SendMessage(edit,EM_REPLACESEL,0,(LONG)(LPSTR)"abc");
    sprintf(text,"wp %04x lp %08lx",command.wp,command.lp);
    check("WM_COMMAND",command.seen && LOWORD(command.lp)==(WORD)edit,text);
    check("HANDLE_WM_COMMAND",cracked.id==ID_EDIT && cracked.ctl==edit && cracked.code==EN_CHANGE,NULL);

    /* EM_SETSEL as Windows 3.0 sends it: both ends in lParam. */
    SendMessage(edit,EM_SETSEL,0,MAKELONG(1,2));
    sel=SendMessage(edit,EM_GETSEL,0,0L);
    sprintf(text,"%08lx",sel);
    check("EM_SETSEL",LOWORD(sel)==1 && HIWORD(sel)==2,text);

    /* The edit control's colour: one WM_CTLCOLOR with the kind in lParam. */
    InvalidateRect(edit,NULL,TRUE); UpdateWindow(edit);
    sprintf(text,"wp %04x lp %08lx",colour.wp,colour.lp);
    check("WM_CTLCOLOR",colour.seen && colour.wp!=0 && LOWORD(colour.lp)==(WORD)edit,text);

    /* The scroll bar's keyboard: the code in wParam, the bar in lParam's high word. */
    SendMessage(scroll,WM_KEYDOWN,VK_RIGHT,1L);
    sprintf(text,"wp %04x lp %08lx",hscroll.wp,hscroll.lp);
    check("WM_HSCROLL",hscroll.seen && hscroll.wp==SB_LINEDOWN && HIWORD(hscroll.lp)==(WORD)scroll,text);

    /* A child made now: its window and id in lParam. */
    CreateWindow("STATIC","",WS_CHILD,10,80,20,20,main_wnd,(HMENU)ID_LATE,hInstance,NULL);
    sprintf(text,"wp %04x lp %08lx",notify.wp,notify.lp);
    check("WM_PARENTNOTIFY",notify.seen && LOWORD(notify.lp)!=0,text);

    /* The edit control subclassed: its characters pass through. */
    old_edit=(FARPROC)SetWindowLong(edit,GWL_WNDPROC,(LONG)MakeProcInstance((FARPROC)EditProc,hInstance));
    SetWindowText(edit,"");
    SendMessage(edit,WM_CHAR,'x',1L); SendMessage(edit,WM_CHAR,'y',1L);
    GetWindowText(edit,text,sizeof(text));
    check("subclassing",subclassed_chars==2 && !strcmp(text,"xy"),text);

    /* A LONG result: -1 stays -1. */
    check("result",SendMessage(main_wnd,WM_USER,0,0L)==-1L,NULL);

    /* A dialog's command, posted as Windows 3.0 packs it. */
    i=dialog();
    sprintf(text,"%d",i);
    check("dialog",i==42,text);

    /* A moveable block's handle, kept in a WORD. */
    h=GlobalAlloc(GMEM_MOVEABLE,64);
    sprintf(text,"%p",(void *)h);
    check("memory handle",h && (p=GlobalLock((HANDLE)(WORD)h))!=NULL && GlobalUnlock((HANDLE)(WORD)h)==0 && GlobalFree((HANDLE)(WORD)h)==NULL,text);

    /* DDE with a native server, found by asking every window. */
    native_windows(hInstance);
    app=GlobalAddAtom("PackServer"); topic=GlobalAddAtom("Test");
    dde.initiating=TRUE;
    SendMessage((HWND)-1,WM_DDE_INITIATE,(WPARAM)main_wnd,MAKELONG(app,topic));
    dde.initiating=FALSE;
    GlobalDeleteAtom(app); GlobalDeleteAtom(topic);
    check("WM_DDE_INITIATE",dde.server!=NULL && dde.server==native.server,NULL);
    /* Commands: the acknowledgement brings their handle back. */
    h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,16);
    p=GlobalLock(h); lstrcpy(p,"[Go]"); GlobalUnlock(h);
    PostMessage(dde.server,WM_DDE_EXECUTE,(WPARAM)main_wnd,MAKELONG(0,h));
    pump();
    sprintf(text,"status %04x handle %04x",dde.status,dde.value);
    check("WM_DDE_EXECUTE",native.executed && dde.status==0x8000 && dde.value==(WORD)h,text);
    GlobalFree(h);
    /* A request: the data comes with the item. */
    item=GlobalAddAtom("Greeting");
    PostMessage(dde.server,WM_DDE_REQUEST,(WPARAM)main_wnd,MAKELONG(CF_TEXT,item));
    pump();
    check("WM_DDE_DATA",!strcmp(dde.data,"hello") && dde.item==item,dde.data);
    /* The data again, taken in the program's own loop as a Windows 3.0 client waits for it. */
    PostMessage(dde.server,WM_DDE_REQUEST,(WPARAM)main_wnd,MAKELONG(CF_TEXT,item));
    text[0]=0;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
        if(msg.hwnd==main_wnd && msg.message==WM_DDE_DATA) {
            HANDLE data=(HANDLE)LOWORD(msg.lParam); DDEDATA FAR *d=(DDEDATA FAR *)GlobalLock(data); BOOL release=FALSE;
            if(d) {lstrcpyn(text,(LPSTR)d->Value,sizeof(text)); release=d->fRelease; GlobalUnlock(data);}
            if(release) GlobalFree(data);
            if(HIWORD(msg.lParam)!=item) lstrcpy(text,"item");
        } else DispatchMessage(&msg);
    }
    check("DDE in the loop",!strcmp(text,"hello"),text);
    /* A poke, acknowledged with its item. */
    dde.status=dde.value=0;
    h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)sizeof(DDEPOKE)+8);
    poke=(DDEPOKE FAR *)GlobalLock(h); poke->fRelease=1; poke->cfFormat=CF_TEXT; lstrcpy((LPSTR)poke->Value,"poked"); GlobalUnlock(h);
    PostMessage(dde.server,WM_DDE_POKE,(WPARAM)main_wnd,MAKELONG(h,item));
    pump();
    sprintf(text,"%s status %04x item %04x",native.poked,dde.status,dde.value);
    check("WM_DDE_POKE",!strcmp(native.poked,"poked") && native.item==item && dde.status==0x8000 && dde.value==item,text);
    GlobalDeleteAtom(item);
    PostMessage(dde.server,WM_DDE_TERMINATE,(WPARAM)main_wnd,0L);
    pump();
    check("WM_DDE_TERMINATE",native.terminated && dde.terminated,NULL);
    /* DDE with a native client: its advise acknowledged, the data sent. */
    i=native_advise();
    sprintf(text,"%d status %04x data %s",i,native.status,native.data);
    check("WM_DDE_ADVISE",i && dde.advised && native.status==0x8000 && !strcmp(native.data,"tick"),text);

    /* Hooks: a message hook before a native one, and a hook on sent messages. */
    {
        HOOKPROC getmsg=(HOOKPROC)MakeProcInstance((FARPROC)GetMsgHook,hInstance),callwnd=(HOOKPROC)MakeProcInstance((FARPROC)CallWndHook,hInstance);
        native_hooks(TRUE);
        next_getmsg=SetWindowsHook(WH_GETMESSAGE,getmsg);
        next_callwnd=SetWindowsHook(WH_CALLWNDPROC,callwnd);
        memset(&cracked,0,sizeof(cracked));
        PostMessage(main_wnd,WM_COMMAND,ID_HOOKED,MAKELONG(edit,EN_CHANGE));
        pump();
        sprintf(text,"hook %04x %08lx native %08lx %08lx program %d",hooked.wp,hooked.lp,(DWORD)native_hook.wp,(DWORD)native_hook.lp,cracked.id);
        check("WH_GETMESSAGE",hooked.seen && LOWORD(hooked.lp)==(WORD)edit && HIWORD(hooked.lp)==EN_CHANGE &&
              native_hook.wp==MAKEWPARAM(ID_CHANGED,EN_CHANGE) && native_hook.lp==(LPARAM)edit &&
              cracked.id==ID_CHANGED && cracked.ctl==edit && cracked.code==EN_CHANGE,text);
        SendMessage(scroll,WM_KEYDOWN,VK_RIGHT,1L);
        sprintf(text,"%04x %08lx",called.wp,called.lp);
        check("WH_CALLWNDPROC",called.seen && HIWORD(called.lp)==(WORD)scroll,text);
        UnhookWindowsHook(WH_GETMESSAGE,getmsg); UnhookWindowsHook(WH_CALLWNDPROC,callwnd);
        native_hooks(FALSE);
    }

    /* A command in the program's own loop, then dispatched. */
    memset(&cracked,0,sizeof(cracked));
    PostMessage(main_wnd,WM_COMMAND,ID_LOOP,MAKELONG(edit,EN_CHANGE));
    i=PeekMessage(&msg,main_wnd,WM_COMMAND,WM_COMMAND,PM_REMOVE);
    sprintf(text,"%d wp %04x lp %08lx",i,(WORD)msg.wParam,(LONG)msg.lParam);
    if(i) DispatchMessage(&msg);
    check("GetMessage",i && msg.wParam==ID_LOOP && LOWORD(msg.lParam)==(WORD)edit && HIWORD(msg.lParam)==EN_CHANGE &&
          cracked.id==ID_LOOP && cracked.ctl==edit && cracked.code==EN_CHANGE,text);

    sprintf(text,"PACKTEST: %d failures",failures);
    OutputDebugString(text);
    DestroyWindow(main_wnd); DestroyWindow(native.server); DestroyWindow(native.client);
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)) DispatchMessage(&msg);
    return failures;
}

/* --- the native side, written for Win32: its messages packed natively ----------- */
#undef RegisterClass
#undef SendMessage
#undef PostMessage
#undef DefWindowProc
#undef SetWindowsHookEx
#undef CallNextHookEx
static LRESULT CALLBACK ServerProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    UINT_PTR lo,hi; HGLOBAL data; DDEDATA *d; const DDEPOKE *poke; LPCSTR text; BOOL ok;
    switch(msg) {
    case WM_DDE_INITIATE:
        if(atom_is(LOWORD(lp),"PackServer") && atom_is(HIWORD(lp),"Test"))
            SendMessage((HWND)wp,WM_DDE_ACK,(WPARAM)h,MAKELPARAM(GlobalAddAtom("PackServer"),GlobalAddAtom("Test")));
        return 0;
    case WM_DDE_EXECUTE:
        text=(LPCSTR)GlobalLock((HGLOBAL)lp); ok=text && !lstrcmp(text,"[Go]");
        if(text) GlobalUnlock((HGLOBAL)lp);
        native.executed=ok;
        PostMessage((HWND)wp,WM_DDE_ACK,(WPARAM)h,PackDDElParam(WM_DDE_ACK,ok?0x8000:0,(UINT_PTR)lp));
        return 0;
    case WM_DDE_REQUEST:
        if(LOWORD(lp)!=CF_TEXT || !(data=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(DDEDATA)+8))) return 0;
        d=(DDEDATA *)GlobalLock(data);
        d->fResponse=1; d->fRelease=1; d->fAckReq=0; d->cfFormat=CF_TEXT; lstrcpy((LPSTR)d->Value,"hello");
        GlobalUnlock(data);
        PostMessage((HWND)wp,WM_DDE_DATA,(WPARAM)h,PackDDElParam(WM_DDE_DATA,(UINT_PTR)data,HIWORD(lp)));
        return 0;
    case WM_DDE_POKE:
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        if((poke=(const DDEPOKE *)GlobalLock((HGLOBAL)lo))!=NULL) {
            lstrcpyn(native.poked,(LPCSTR)poke->Value,sizeof(native.poked)); ok=poke->fRelease;
            GlobalUnlock((HGLOBAL)lo);
            if(ok) GlobalFree((HGLOBAL)lo);
        }
        native.item=(WORD)hi;
        PostMessage((HWND)wp,WM_DDE_ACK,(WPARAM)h,PackDDElParam(WM_DDE_ACK,0x8000,hi));
        return 0;
    case WM_DDE_TERMINATE: native.terminated=TRUE; PostMessage((HWND)wp,WM_DDE_TERMINATE,(WPARAM)h,0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
static LRESULT CALLBACK ClientProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    UINT_PTR lo,hi; const DDEDATA *d; BOOL release=FALSE;
    switch(msg) {
    case WM_DDE_ACK:
        if(native.initiating) {
            if(!native.partner) native.partner=(HWND)wp;
            GlobalDeleteAtom(LOWORD(lp)); GlobalDeleteAtom(HIWORD(lp));
            return 0;
        }
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        native.status=(WORD)lo;
        return 0;
    case WM_DDE_DATA:
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        if((d=(const DDEDATA *)GlobalLock((HGLOBAL)lo))!=NULL) {lstrcpyn(native.data,(LPCSTR)d->Value,sizeof(native.data)); release=d->fRelease; GlobalUnlock((HGLOBAL)lo);}
        if(release) GlobalFree((HGLOBAL)lo);
        GlobalDeleteAtom((ATOM)hi);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
static LRESULT CALLBACK NativeGetMsg(int code,WPARAM wp,LPARAM lp) {
    const MSG *m=(const MSG *)lp;
    if(code>=0 && m->hwnd==main_wnd && m->message==WM_COMMAND && LOWORD(m->wParam)==ID_CHANGED) {native_hook.wp=m->wParam; native_hook.lp=m->lParam;}
    return CallNextHookEx(native_hook.getmsg,code,wp,lp);
}
static void native_hooks(BOOL set) {
    if(set) native_hook.getmsg=SetWindowsHookEx(WH_GETMESSAGE,NativeGetMsg,NULL,(DWORD)(ULONG_PTR)GetCurrentTask());
    else UnhookWindowsHookEx(native_hook.getmsg);
}
static void native_windows(HANDLE instance) {
    WNDCLASS wc;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=ServerProc; wc.hInstance=(HINSTANCE)instance; wc.lpszClassName="PackServer";
    RegisterClass(&wc);
    wc.lpfnWndProc=ClientProc; wc.lpszClassName="PackClient";
    RegisterClass(&wc);
    native.server=CreateWindow("PackServer","",WS_POPUP,0,0,10,10,NULL,NULL,(HINSTANCE)instance,NULL);
    native.client=CreateWindow("PackClient","",WS_POPUP,0,0,10,10,NULL,NULL,(HINSTANCE)instance,NULL);
}
static BOOL native_advise(void) {
    ATOM app=GlobalAddAtom("PackMain"),topic=GlobalAddAtom("Mirror"),item; HGLOBAL h; DDEADVISE *o; LPARAM lp;
    native.initiating=TRUE;
    SendMessage(HWND_BROADCAST,WM_DDE_INITIATE,(WPARAM)native.client,MAKELPARAM(app,topic));
    native.initiating=FALSE;
    GlobalDeleteAtom(app); GlobalDeleteAtom(topic);
    if(!native.partner || !(h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(DDEADVISE)))) return FALSE;
    o=(DDEADVISE *)GlobalLock(h); o->fAckReq=0; o->fDeferUpd=0; o->cfFormat=CF_TEXT; GlobalUnlock(h);
    item=GlobalAddAtom("Clock");
    lp=PackDDElParam(WM_DDE_ADVISE,(UINT_PTR)h,item);
    if(!PostMessage(native.partner,WM_DDE_ADVISE,(WPARAM)native.client,lp)) {FreeDDElParam(WM_DDE_ADVISE,lp); GlobalFree(h); return FALSE;}
    pump();
    GlobalDeleteAtom(item);
    PostMessage(native.partner,WM_DDE_TERMINATE,(WPARAM)native.client,0);
    pump();
    return TRUE;
}
