/* SPDX-License-Identifier: GPL-2.0-or-later
 * Open Watcom Win16 test application. Cover KERNEL/CRT, windows and
 * controls, subclassing, enumeration, dialogs, timers, GDI structures and
 * resources, W16DLL imports/dynamic loading, atoms/properties, clipboard,
 * MDI, hooks, LZEXPAND, printing and common dialogs. Report checks through
 * OutputDebugString. The "dde" command runs only the Program Manager DDE
 * setup-client test.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dos.h>
#include <commdlg.h>
#include <drivinit.h>
#include <lzexpand.h>
#include <dde.h>
#include <cderr.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <toolhelp.h>
#include "w16app.h"
#ifndef FINDMSGSTRING
#define FINDMSGSTRING "commdlg_FindReplace"
#endif
static HINSTANCE instance;
static BOOL painted;

static void trace(LPCSTR text) {OutputDebugString(text);}
static FARPROC old_edit,old_button;
static int edit_chars,edit_setsel,button_keys,enumerated,timer_calls,modeless_init;
static HWND main_window;
static int base=100; /* in the program's data segment */
/* W16DLL.DLL */
int FAR PASCAL DllAdd(int a,int b);
int FAR PASCAL DllCalls(void);
LPSTR FAR PASCAL DllText(void);
int FAR PASCAL DllHeap(void);
int FAR PASCAL DllCallBack(FARPROC proc,int value);
BOOL FAR PASCAL DllRegister(void);

LRESULT FAR PASCAL _export EditSub(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_CHAR) {edit_chars++; if(wp=='q') return 0;}
    if(msg==EM_SETSEL && LOWORD(lp)==1 && HIWORD(lp)==3) edit_setsel++;
    return CallWindowProc(old_edit,h,msg,wp,lp);
}
LRESULT FAR PASCAL _export ButtonSub(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_KEYDOWN && wp=='Z') button_keys++;
    return CallWindowProc(old_button,h,msg,wp,lp);
}
BOOL FAR PASCAL _export EnumProc(HWND h,LPARAM lp) {
    if(lp==0x1234567L && (h==main_window || GetParent(h)==main_window)) enumerated++;
    return TRUE;
}
void FAR PASCAL _export TimerProc(HWND h,UINT msg,UINT id,DWORD time) {
    (void)time;
    if(msg==WM_TIMER && !timer_calls++) {KillTimer(h,id); trace("W16APP: timer proc");}
}
BOOL FAR PASCAL _export ModelessProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)dlg; (void)wp;
    if(msg==WM_INITDIALOG && lp==77) {modeless_init++; return TRUE;}
    return FALSE;
}

BOOL FAR PASCAL _export AboutProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_TEXT,"Win16 on IA-64");
        trace("W16APP: about");
        return TRUE;
    case WM_COMMAND:
        if(wp==IDOK || wp==IDCANCEL) {EndDialog(dlg,wp); return TRUE;}
        break;
    }
    return FALSE;
}

/* The common dialogs, from the File menu. */
static char find_what[32]="hello";
static FINDREPLACE find;
static UINT find_message;
static HWND find_dialog;
static void common_dialog(HWND h,UINT id) {
    char line[160],file[128],title[32];
    switch(id) {
    case IDM_OPEN: {
        OPENFILENAME o;
        memset(&o,0,sizeof(o)); file[0]=0;
        o.lStructSize=sizeof(o); o.hwndOwner=h; o.lpstrFilter="Text Files (*.TXT)\0*.TXT\0Settings (*.INI)\0*.INI\0";
        o.nFilterIndex=2; o.lpstrFile=file; o.nMaxFile=sizeof(file); o.lpstrFileTitle=title; o.nMaxFileTitle=sizeof(title);
        o.lpstrInitialDir="C:\\WINDOWS"; o.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY;
        if(GetOpenFileName(&o)) wsprintf(line,"W16APP: open %s %s %d",(LPSTR)file,(LPSTR)title,o.nFileOffset);
        else wsprintf(line,"W16APP: open cancelled %lx",CommDlgExtendedError());
        break;
    }
    case IDM_FONT: {
        CHOOSEFONT c; LOGFONT lf;
        memset(&c,0,sizeof(c)); memset(&lf,0,sizeof(lf)); lf.lfHeight=-13; lstrcpy(lf.lfFaceName,"Helv");
        c.lStructSize=sizeof(c); c.hwndOwner=h; c.lpLogFont=&lf; c.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT|CF_EFFECTS;
        if(ChooseFont(&c)) wsprintf(line,"W16APP: font %s %d %d",(LPSTR)lf.lfFaceName,c.iPointSize,lf.lfHeight);
        else lstrcpy(line,"W16APP: font cancelled");
        break;
    }
    case IDM_COLOR: {
        CHOOSECOLOR c; static COLORREF custom[16];
        memset(&c,0,sizeof(c));
        c.lStructSize=sizeof(c); c.hwndOwner=h; c.rgbResult=RGB(0,0,255); c.lpCustColors=custom; c.Flags=CC_RGBINIT;
        if(ChooseColor(&c)) wsprintf(line,"W16APP: color %06lx",c.rgbResult);
        else lstrcpy(line,"W16APP: color cancelled");
        break;
    }
    case IDM_FIND:
        if(!find_message) find_message=RegisterWindowMessage(FINDMSGSTRING);
        memset(&find,0,sizeof(find));
        find.lStructSize=sizeof(find); find.hwndOwner=h; find.Flags=FR_DOWN; find.lpstrFindWhat=find_what; find.wFindWhatLen=sizeof(find_what);
        find_dialog=FindText(&find);
        lstrcpy(line,find_dialog?"W16APP: find dialog":"W16APP: find failed");
        break;
    case IDM_PRINT: {
        PRINTDLG pd; LPDEVNAMES n; LPDEVMODE m;
        memset(&pd,0,sizeof(pd));
        pd.lStructSize=sizeof(pd); pd.hwndOwner=h; pd.Flags=PD_RETURNDC|PD_NOSELECTION|PD_PAGENUMS;
        pd.nFromPage=2; pd.nToPage=3; pd.nMinPage=1; pd.nMaxPage=9; pd.nCopies=1;
        if(PrintDlg(&pd)) {
            n=(LPDEVNAMES)GlobalLock(pd.hDevNames); m=(LPDEVMODE)GlobalLock(pd.hDevMode);
            wsprintf(line,"W16APP: print %s, %s, %s %d-%d x%d %s paper %d %s",(LPSTR)n+n->wDriverOffset,(LPSTR)n+n->wDeviceOffset,
                     (LPSTR)n+n->wOutputOffset,pd.nFromPage,pd.nToPage,pd.nCopies,(LPSTR)(pd.Flags&PD_PRINTTOFILE?"to file":"to port"),
                     m->dmPaperSize,(LPSTR)(pd.hDC && GetDeviceCaps(pd.hDC,LOGPIXELSX)==150?"dc":"no dc"));
            GlobalUnlock(pd.hDevNames); GlobalUnlock(pd.hDevMode); GlobalFree(pd.hDevNames); GlobalFree(pd.hDevMode);
            if(pd.hDC) DeleteDC(pd.hDC);
        } else wsprintf(line,"W16APP: print cancelled %lx",CommDlgExtendedError());
        break;
    }
    default: return;
    }
    trace(line);
}

/* --- DDE ------------------------------------------------------------------------- */
static HWND dde_server;
static BOOL dde_acked,dde_ok,dde_data,dde_ended;
static char dde_text[256];
static void dde_message(UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_DDE_ACK:
        if(!dde_server) {dde_server=(HWND)wp; GlobalDeleteAtom(LOWORD(lp)); GlobalDeleteAtom(HIWORD(lp)); break;}
        dde_ok=(LOWORD(lp)&0x8000)!=0; dde_acked=TRUE;
        GlobalFree((HGLOBAL)HIWORD(lp)); /* the commands */
        break;
    case WM_DDE_DATA: {
        DDEDATA FAR *d=(DDEDATA FAR *)GlobalLock((HGLOBAL)LOWORD(lp)); BOOL release=d && d->fRelease;
        if(d) {lstrcpyn(dde_text,(LPCSTR)d->Value,sizeof(dde_text)); GlobalUnlock((HGLOBAL)LOWORD(lp));}
        if(release) GlobalFree((HGLOBAL)LOWORD(lp));
        GlobalDeleteAtom(HIWORD(lp)); dde_data=TRUE;
        break;
    }
    case WM_DDE_TERMINATE: dde_ended=TRUE; break;
    }
}
static void pump_dde(BOOL *done) {
    DWORD start=GetTickCount(); MSG m;
    while(!*done && GetTickCount()-start<5000)
        if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);}
}
/* A group and an item made through Program Manager's commands (a title
 * in quotes, with brackets in it), then its items asked for. */
static const char *dde_test(HWND h) {
    static const char commands[]="[CreateGroup(Win16 Group)][AddItem(notepad.exe,\"Notes (16-bit)\")][ShowGroup(Win16 Group,1)]";
    ATOM app=GlobalAddAtom("PROGMAN"),topic=GlobalAddAtom("PROGMAN"); HGLOBAL cmd; LPSTR p; BOOL got=FALSE;
    SendMessage((HWND)0xffff,WM_DDE_INITIATE,(WPARAM)h,MAKELONG(app,topic));
    GlobalDeleteAtom(app); GlobalDeleteAtom(topic);
    if(!dde_server) return "WM_DDE_INITIATE";
    cmd=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(commands)); p=(LPSTR)GlobalLock(cmd); lstrcpy(p,commands); GlobalUnlock(cmd);
    PostMessage(dde_server,WM_DDE_EXECUTE,(WPARAM)h,MAKELONG(0,cmd));
    pump_dde(&dde_acked);
    if(!dde_ok) return "WM_DDE_EXECUTE";
    PostMessage(dde_server,WM_DDE_REQUEST,(WPARAM)h,MAKELONG(CF_TEXT,GlobalAddAtom("Win16 Group")));
    pump_dde(&dde_data);
    got=strstr(dde_text,"\"Notes (16-bit)\",\"notepad.exe\"")!=NULL;
    PostMessage(dde_server,WM_DDE_TERMINATE,(WPARAM)h,0);
    pump_dde(&dde_ended);
    if(!got) return "WM_DDE_REQUEST";
    return dde_ended?NULL:"WM_DDE_TERMINATE";
}

LRESULT FAR PASCAL _export WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    char line[80];
    if(msg>=WM_DDE_FIRST && msg<=WM_DDE_LAST) {dde_message(msg,wp,lp); return 0;}
    if(msg==find_message && find_message) {
        LPFINDREPLACE fr=(LPFINDREPLACE)lp;
        if(fr->Flags&FR_DIALOGTERM) {trace("W16APP: find closed"); find_dialog=NULL;}
        else {wsprintf(line,"W16APP: find %s%s",(LPSTR)fr->lpstrFindWhat,(LPSTR)(fr->Flags&FR_DOWN?" down":" up")); trace(line);}
        return 0;
    }
    switch(msg) {
    case WM_CREATE:
        wsprintf(line,"W16APP: WM_CREATE \"%s\" %dx%d",((LPCREATESTRUCT)lp)->lpszName,((LPCREATESTRUCT)lp)->cx,((LPCREATESTRUCT)lp)->cy);
        trace(line);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.x=200;
        ((MINMAXINFO FAR *)lp)->ptMinTrackSize.y=120;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); HBRUSH red=CreateSolidBrush(RGB(255,0,0)),old;
        old=SelectObject(dc,red);
        Rectangle(dc,10,40,110,90);
        SelectObject(dc,old); DeleteObject(red);
        SetBkMode(dc,TRANSPARENT);
        TextOut(dc,10,10,"16-bit app on IA-64.",20);
        EndPaint(h,&ps);
        if(!painted) {painted=TRUE; trace("W16APP: painted");}
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT FAR *m=(MEASUREITEMSTRUCT FAR *)lp;
        if(m->CtlType==ODT_LISTBOX && m->CtlID==IDC_OWNER) m->itemHeight=12;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT FAR *d=(DRAWITEMSTRUCT FAR *)lp; static BOOL drawn;
        HBRUSH blue;
        if(d->CtlType!=ODT_LISTBOX || d->CtlID!=IDC_OWNER || d->itemID==(UINT)-1) return TRUE;
        blue=CreateSolidBrush(d->itemData==0x1234?RGB(0,0,255):RGB(0,255,0));
        FillRect(d->hDC,&d->rcItem,blue); DeleteObject(blue);
        if(!drawn && d->itemData==0x1234 && d->rcItem.bottom-d->rcItem.top==12) {drawn=TRUE; trace("W16APP: WM_DRAWITEM 1234");}
        return TRUE;
    }
    case WM_TIMER:
        if(wp==2) {KillTimer(h,2); trace("W16APP: WM_TIMER");}
        return 0;
    case WM_COMMAND:
        if(wp==IDM_ABOUT) {DialogBox(instance,MAKEINTRESOURCE(IDD_ABOUT),h,AboutProc); return 0;}
        if(wp==IDM_EXIT) {DestroyWindow(h); return 0;}
        if(wp>=IDM_OPEN && wp<=IDM_PRINT) {common_dialog(h,wp); return 0;}
        break;
    case WM_DESTROY:
        trace("W16APP: WM_DESTROY");
        PostQuitMessage(7);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

static CATCHBUF catch_buffer;
static void thrower(void) {Throw(catch_buffer,5);}
/* Returns what failed, or NULL. */
static const char *self_test(void) {
    char *p,line[80]; FILE *f; HGLOBAL g; char __far *block; HFILE h; int i; time_t now;
    p=malloc(200); if(!p) return "malloc";
    strcpy(p,"local heap"); if(strlen(p)!=10) return "strcpy";
    for(i=0;i<20;i++) {char *q=malloc(1000); if(!q) return "malloc 20"; memset(q,i,1000); free(q);}
    free(p);
    now=time(NULL); if(now<315532800L) return "time";
    sprintf(line,"%d-%s-%lx",42,"crt",0x12345678L); if(strcmp(line,"42-crt-12345678")) return "sprintf";
    f=fopen("C:\\W16APP.TMP","w"); if(!f) return "fopen w";
    fprintf(f,"first %d\nsecond\n",7); fclose(f);
    f=fopen("C:\\W16APP.TMP","r"); if(!f) return "fopen r";
    if(!fgets(line,sizeof(line),f) || strcmp(line,"first 7\n")) return "fgets";
    fseek(f,9,SEEK_SET); if(!fgets(line,sizeof(line),f) || strcmp(line,"second\n")) return "fseek";
    fclose(f);
    h=_lopen("C:\\W16APP.TMP",OF_READ); if(h==HFILE_ERROR) return "_lopen";
    if(_lread(h,line,5)!=5 || memcmp(line,"first",5)) return "_lread";
    _lclose(h); remove("C:\\W16APP.TMP");
    g=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,0x18000L); if(!g) return "GlobalAlloc";
    block=GlobalLock(g); if(!block || block[0x100]) return "GlobalLock";
    block[0]='A';
    ((char __far *)MK_FP(FP_SEG(block)+8,0x10))[0]='B';
    if(GlobalSize(g)<0x18000L) return "GlobalSize";
    GlobalUnlock(g);
    g=GlobalReAlloc(g,0x20000L,GMEM_MOVEABLE); if(!g) return "GlobalReAlloc";
    block=GlobalLock(g); if(block[0]!='A' || ((char __far *)MK_FP(FP_SEG(block)+8,0x10))[0]!='B') return "huge data";
    GlobalUnlock(g); GlobalFree(g);
    WriteProfileString("W16APP","Key","Value");
    GetProfileString("W16APP","Key","",line,sizeof(line)); if(strcmp(line,"Value")) return "profile";
    if(!getenv("COMSPEC")) return "environment";
    if(!GetProcAddress(GetModuleHandle("USER"),"MESSAGEBOX")) return "GetProcAddress";
    if((i=Catch(catch_buffer))==0) {thrower(); return "Throw";}
    if(i!=5) return "Catch";
    return NULL;
}

/* Controls, subclassing and the rest, once the main window exists. */
static const char *window_test(HWND h) {
    HWND edit,button,check,list,owner,dlg; char text[40]; BOOL ok; FARPROC proc; DWORD sel; int n;
    edit=CreateWindow("EDIT","hello world",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL,130,4,120,20,h,(HMENU)IDC_EDIT,instance,NULL);
    button=CreateWindow("BUTTON","Push",WS_CHILD|BS_PUSHBUTTON,130,30,60,20,h,(HMENU)IDC_BUTTON,instance,NULL);
    check=CreateWindow("BUTTON","Check",WS_CHILD|BS_AUTOCHECKBOX,200,30,60,20,h,(HMENU)IDC_CHECK,instance,NULL);
    list=CreateWindow("LISTBOX","",WS_CHILD|WS_BORDER|LBS_SORT,130,60,80,40,h,(HMENU)IDC_LIST,instance,NULL);
    owner=CreateWindow("LISTBOX","",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_OWNERDRAWFIXED,160,100,60,30,h,(HMENU)IDC_OWNER,instance,NULL);
    if(!edit || !button || !check || !list || !owner) return "controls";
    /* Control messages: Windows 3.0 numbers and packing. */
    SendMessage(edit,EM_SETSEL,0,MAKELONG(2,5));
    sel=SendMessage(edit,EM_GETSEL,0,0); if(LOWORD(sel)!=2 || HIWORD(sel)!=5) return "EM_SETSEL";
    SendMessage(edit,EM_REPLACESEL,0,(LPARAM)(LPSTR)"XYZ");
    GetWindowText(edit,text,sizeof(text)); if(strcmp(text,"heXYZ world")) return "EM_REPLACESEL";
    if(SendMessage(edit,EM_GETLINECOUNT,0,0)!=1) return "EM_GETLINECOUNT";
    SendMessage(list,LB_ADDSTRING,0,(LPARAM)(LPSTR)"beta");
    SendMessage(list,LB_ADDSTRING,0,(LPARAM)(LPSTR)"alpha");
    if(SendMessage(list,LB_GETCOUNT,0,0)!=2) return "LB_GETCOUNT";
    SendMessage(list,LB_GETTEXT,0,(LPARAM)(LPSTR)text); if(strcmp(text,"alpha")) return "LB_GETTEXT";
    if(SendMessage(list,LB_FINDSTRING,(WPARAM)-1,(LPARAM)(LPSTR)"be")!=1) return "LB_FINDSTRING";
    SendMessage(list,LB_SETCURSEL,1,0); if(SendMessage(list,LB_GETCURSEL,0,0)!=1) return "LB_GETCURSEL";
    SendMessage(owner,LB_ADDSTRING,0,0x1234L);
    SendMessage(owner,LB_ADDSTRING,0,0x42L);
    if(SendMessage(owner,LB_GETITEMDATA,1,0)!=0x42L) return "LB_GETITEMDATA";
    SendDlgItemMessage(h,IDC_CHECK,BM_SETCHECK,1,0);
    if(SendMessage(check,BM_GETCHECK,0,0)!=1 || !IsDlgButtonChecked(h,IDC_CHECK)) return "BM_SETCHECK";
    /* Subclassing a native control: messages reach the 16-bit procedure
     * in Windows 3.0 form and go on through CallWindowProc. */
    proc=MakeProcInstance((FARPROC)EditSub,instance);
    old_edit=(FARPROC)SetWindowLong(edit,GWL_WNDPROC,(LONG)proc);
    if(!old_edit || GetWindowLong(edit,GWL_WNDPROC)!=(LONG)proc) return "SetWindowLong GWL_WNDPROC";
    SetWindowText(edit,"");
    SendMessage(edit,WM_CHAR,'a',1); SendMessage(edit,WM_CHAR,'q',1); SendMessage(edit,WM_CHAR,'b',1);
    GetWindowText(edit,text,sizeof(text)); if(strcmp(text,"ab") || edit_chars!=3) return "subclassed WM_CHAR";
    SendDlgItemMessage(h,IDC_EDIT,EM_SETSEL,0,MAKELONG(1,3));
    sel=SendMessage(edit,EM_GETSEL,0,0); if(edit_setsel!=1 || LOWORD(sel)!=1 || HIWORD(sel)!=2) return "subclassed EM_SETSEL";
    if(SetWindowLong(edit,GWL_WNDPROC,(LONG)old_edit)!=(LONG)proc || GetWindowLong(edit,GWL_WNDPROC)!=(LONG)old_edit) return "restore GWL_WNDPROC";
    SendMessage(edit,WM_CHAR,'c',1); if(edit_chars!=3) return "restored procedure";
    old_button=(FARPROC)SetWindowLong(button,GWL_WNDPROC,(LONG)MakeProcInstance((FARPROC)ButtonSub,instance));
    SendMessage(button,WM_KEYDOWN,'Z',0); SendMessage(button,WM_KEYUP,'Z',0);
    if(button_keys!=1) return "subclassed WM_KEYDOWN";
    /* Values of windows and classes. */
    SetDlgItemInt(h,IDC_EDIT,1234,FALSE);
    if(GetDlgItemInt(h,IDC_EDIT,&ok,FALSE)!=1234 || !ok) return "GetDlgItemInt";
    SetWindowText(edit,"-42");
    if((int)GetDlgItemInt(h,IDC_EDIT,&ok,TRUE)!=-42 || !ok) return "GetDlgItemInt signed";
    SetWindowText(edit,"x"); GetDlgItemInt(h,IDC_EDIT,&ok,TRUE); if(ok) return "GetDlgItemInt error";
    if((HINSTANCE)GetWindowWord(h,GWW_HINSTANCE)!=instance) return "GWW_HINSTANCE";
    if((HWND)GetWindowWord(edit,GWW_HWNDPARENT)!=h || GetWindowWord(edit,GWW_ID)!=IDC_EDIT) return "GWW_HWNDPARENT";
    if(!(GetWindowLong(h,GWL_STYLE)&WS_CAPTION)) return "GWL_STYLE";
    if(GetClassLong(h,GCL_WNDPROC)!=(LONG)(FARPROC)WndProc) return "GCL_WNDPROC";
    if(GetClassWord(h,GCW_HBRBACKGROUND)!=COLOR_WINDOW+1 || !GetClassWord(h,GCW_HICON)) return "GetClassWord";
    if(GetClassWord(h,GCW_HMODULE)!=GetModuleHandle("W16APP")) return "GCW_HMODULE";
    /* Enumeration. */
    proc=MakeProcInstance((FARPROC)EnumProc,instance);
    EnumChildWindows(h,(WNDENUMPROC)proc,0x1234567L); if(enumerated!=5) return "EnumChildWindows";
    enumerated=0; EnumWindows((WNDENUMPROC)proc,0x1234567L); if(enumerated!=1) return "EnumWindows";
    enumerated=0; EnumTaskWindows(GetCurrentTask(),(WNDENUMPROC)proc,0x1234567L); if(enumerated!=1) return "EnumTaskWindows";
    if(GetWindowTask(h)!=GetCurrentTask()) return "GetWindowTask";
    /* A modeless dialog and its values. */
    proc=MakeProcInstance((FARPROC)ModelessProc,instance);
    dlg=CreateDialogParam(instance,MAKEINTRESOURCE(IDD_ABOUT),h,(DLGPROC)proc,77);
    if(!dlg || modeless_init!=1) return "CreateDialogParam";
    if(GetWindowLong(dlg,DWL_DLGPROC)!=(LONG)proc) return "DWL_DLGPROC";
    SetWindowLong(dlg,DWL_USER,0x12345678L); if(GetWindowLong(dlg,DWL_USER)!=0x12345678L) return "DWL_USER";
    SendDlgItemMessage(dlg,IDC_TEXT,WM_SETTEXT,0,(LPARAM)(LPSTR)"modeless");
    GetDlgItemText(dlg,IDC_TEXT,text,sizeof(text)); if(strcmp(text,"modeless")) return "SendDlgItemMessage";
    DestroyWindow(dlg); if(IsWindow(dlg)) return "DestroyWindow";
    DestroyWindow(list);
    /* Timers: a procedure, and WM_TIMER. */
    if(!SetTimer(h,1,50,(TIMERPROC)MakeProcInstance((FARPROC)TimerProc,instance))) return "SetTimer proc";
    if(!SetTimer(h,2,80,NULL)) return "SetTimer";
    n=GetDlgCtrlID(owner); if(n!=IDC_OWNER) return "GetDlgCtrlID";
    return NULL;
}

static int fonts_seen,line_points;
int FAR PASCAL _export FontProc(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    if(lp==99 && type==RASTER_FONTTYPE && lf->lfFaceName[0] && tm->tmHeight>0 && tm->tmAveCharWidth>0) fonts_seen++;
    return 1;
}
void FAR PASCAL _export LineProc(int x,int y,LPARAM lp) {if(lp==5 && y==3 && x>=0) line_points++;}
/* Mapping modes, arcs, polygons, regions, palettes and callbacks. */
static const char *gdi_test2(HDC mem) {
    POINT p[8]; int counts[2]; HRGN rgn; HPALETTE pal,oldpal; HBRUSH red=CreateSolidBrush(RGB(255,0,0));
    struct {WORD version,count; PALETTEENTRY e[2];} lp; const char *failed=NULL;
    PatBlt(mem,0,0,32,32,WHITENESS);
    SelectObject(mem,red); SelectObject(mem,GetStockObject(NULL_PEN));
    SetMapMode(mem,MM_ANISOTROPIC); SetWindowExt(mem,100,100); SetViewportExt(mem,50,50);
    if(GetViewportExt(mem)!=MAKELONG(50,50) || GetMapMode(mem)!=MM_ANISOTROPIC) failed="SetViewportExt";
    Rectangle(mem,10,10,40,40);
    p[0].x=40; p[0].y=20; p[1].x=0; p[1].y=0; LPtoDP(mem,p,2);
    if(!failed && (p[0].x!=20 || p[0].y!=10 || p[1].x!=0)) failed="LPtoDP";
    SetMapMode(mem,MM_TEXT);
    if(!failed && (GetPixel(mem,10,10)!=RGB(255,0,0) || GetPixel(mem,25,25)!=RGB(255,255,255))) failed="mapped Rectangle";
    PatBlt(mem,0,0,32,32,WHITENESS);
    Pie(mem,0,0,32,32,32,16,16,0);
    if(!failed && (GetPixel(mem,22,9)!=RGB(255,0,0) || GetPixel(mem,9,22)!=RGB(255,255,255))) failed="Pie";
    PatBlt(mem,0,0,32,32,WHITENESS);
    p[0].x=0; p[0].y=0; p[1].x=8; p[1].y=0; p[2].x=8; p[2].y=8; p[3].x=0; p[3].y=8;
    p[4].x=16; p[4].y=16; p[5].x=24; p[5].y=16; p[6].x=24; p[6].y=24; p[7].x=16; p[7].y=24;
    counts[0]=counts[1]=4; PolyPolygon(mem,p,counts,2);
    if(!failed && (GetPixel(mem,4,4)!=RGB(255,0,0) || GetPixel(mem,20,20)!=RGB(255,0,0) || GetPixel(mem,12,12)!=RGB(255,255,255))) failed="PolyPolygon";
    rgn=CreatePolygonRgn(p,4,ALTERNATE);
    if(!failed && (!rgn || !PtInRegion(rgn,4,4) || PtInRegion(rgn,12,12))) failed="CreatePolygonRgn";
    DeleteObject(rgn);
    rgn=CreateEllipticRgn(0,0,20,10);
    if(!failed && (!rgn || !PtInRegion(rgn,10,5) || PtInRegion(rgn,0,0))) failed="CreateEllipticRgn";
    DeleteObject(rgn);
    lp.version=0x300; lp.count=2;
    lp.e[0].peRed=255; lp.e[0].peGreen=lp.e[0].peBlue=0; lp.e[0].peFlags=0;
    lp.e[1].peRed=lp.e[1].peGreen=0; lp.e[1].peBlue=255; lp.e[1].peFlags=0;
    pal=CreatePalette((LOGPALETTE FAR *)&lp); oldpal=SelectPalette(mem,pal,FALSE); RealizePalette(mem);
    SetPixel(mem,0,0,PALETTEINDEX(1));
    if(!failed && (!pal || !oldpal || GetPixel(mem,0,0)!=RGB(0,0,255) || GetNearestPaletteIndex(pal,RGB(200,0,0))!=0)) failed="palette";
    SelectPalette(mem,oldpal,FALSE); DeleteObject(pal);
    EnumFonts(mem,"Helv",(OLDFONTENUMPROC)MakeProcInstance((FARPROC)FontProc,instance),(LPSTR)99L);
    if(!failed && fonts_seen!=3) failed="EnumFonts";
    LineDDA(0,3,6,3,(LINEDDAPROC)MakeProcInstance((FARPROC)LineProc,instance),5);
    if(!failed && line_points!=6) failed="LineDDA";
    SelectObject(mem,GetStockObject(WHITE_BRUSH)); DeleteObject(red);
    return failed;
}
/* Metafiles: a Windows 3.0 metafile handle is a global block of its bits;
 * one is played, enumerated, and put on the clipboard as a picture. */
static int records_seen,rectangles_seen;
int FAR PASCAL _export MetaProc(HDC dc,HANDLETABLE FAR *t,METARECORD FAR *r,int n,LPARAM lp) {
    records_seen++;
    if(r->rdFunction==META_RECTANGLE) rectangles_seen++;
    PlayMetaFileRecord(dc,t,r,n);
    return lp==7;
}
static const char *metafile_test(HWND h,HDC mem) {
    HDC mf=CreateMetaFile(NULL); HMETAFILE hmf; HBRUSH blue=CreateSolidBrush(RGB(0,0,255));
    METAHEADER FAR *head; HGLOBAL pict; METAFILEPICT FAR *mp; BOOL ok;
    if(!mf) return "CreateMetaFile";
    SelectObject(mf,blue); SelectObject(mf,GetStockObject(NULL_PEN)); Rectangle(mf,0,0,10,10);
    SetPixel(mf,20,20,RGB(0,255,0));
    hmf=CloseMetaFile(mf); DeleteObject(blue);
    if(!hmf || GlobalSize(hmf)<sizeof(METAHEADER)) return "CloseMetaFile";
    head=(METAHEADER FAR *)GlobalLock(hmf);
    ok=head->mtType==1 && head->mtHeaderSize==9 && head->mtSize*2<=GlobalSize(hmf);
    GlobalUnlock(hmf);
    if(!ok) return "METAHEADER";
    if(GetMetaFileBits(hmf)!=hmf || SetMetaFileBits(hmf)!=hmf) return "GetMetaFileBits";
    PatBlt(mem,0,0,32,32,WHITENESS);
    if(!PlayMetaFile(mem,hmf) || GetPixel(mem,5,5)!=RGB(0,0,255) || GetPixel(mem,20,20)!=RGB(0,255,0) || GetPixel(mem,15,15)!=RGB(255,255,255)) return "PlayMetaFile";
    PatBlt(mem,0,0,32,32,WHITENESS);
    if(!EnumMetaFile(mem,hmf,(MFENUMPROC)MakeProcInstance((FARPROC)MetaProc,instance),7) || rectangles_seen!=1 || records_seen<5 || GetPixel(mem,5,5)!=RGB(0,0,255)) return "EnumMetaFile";
    /* The clipboard takes the picture (and its metafile); a copy comes back. */
    pict=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(METAFILEPICT)); mp=(METAFILEPICT FAR *)GlobalLock(pict);
    mp->mm=MM_ANISOTROPIC; mp->xExt=1000; mp->yExt=500; mp->hMF=hmf; GlobalUnlock(pict);
    if(!OpenClipboard(h)) return "OpenClipboard";
    EmptyClipboard(); ok=SetClipboardData(CF_METAFILEPICT,pict)!=NULL; CloseClipboard();
    if(!ok || !OpenClipboard(h)) return "SetClipboardData picture";
    pict=GetClipboardData(CF_METAFILEPICT); ok=FALSE;
    if(pict && (mp=(METAFILEPICT FAR *)GlobalLock(pict))!=NULL) {
        PatBlt(mem,0,0,32,32,WHITENESS);
        ok=mp->mm==MM_ANISOTROPIC && mp->xExt==1000 && mp->yExt==500 && PlayMetaFile(mem,mp->hMF) && GetPixel(mem,5,5)==RGB(0,0,255);
        GlobalUnlock(pict);
    }
    EmptyClipboard(); CloseClipboard();
    return ok?NULL:"GetClipboardData picture";
}
/* GDI's 16-bit structures, drawing into a memory DC. */
static const char *gdi_test(HWND h) {
    static BYTE bits[8]={0x01,0x10,0,0,0x01,0x10,0,0};
    struct {BITMAPINFOHEADER h; RGBQUAD c[16];} bi;
    LOGFONT lf,lf2; LOGPEN lp,lp2; LOGBRUSH lb,lb2; BITMAP bm; TEXTMETRIC tm; int widths[3]; POINT pts[3];
    HFONT font; HPEN pen; HBRUSH brush; HBITMAP bitmap,picture,dib,old; HDC dc,mem; BYTE back[32*3]; const char *failed=NULL;
    memset(&lf,0,sizeof(lf)); lf.lfHeight=-12; lf.lfWeight=FW_BOLD; lf.lfItalic=1; lstrcpy(lf.lfFaceName,"Helv");
    font=CreateFontIndirect(&lf);
    if(!font || GetObject(font,sizeof(lf2),&lf2)!=sizeof(LOGFONT) || lf2.lfHeight!=-12 || lf2.lfWeight!=FW_BOLD || !lf2.lfItalic || lstrcmp(lf2.lfFaceName,"Helv")) return "GetObject font";
    lp.lopnStyle=PS_SOLID; lp.lopnWidth.x=3; lp.lopnWidth.y=0; lp.lopnColor=RGB(0,128,0);
    pen=CreatePenIndirect(&lp);
    if(!pen || GetObject(pen,sizeof(lp2),&lp2)!=sizeof(LOGPEN) || lp2.lopnWidth.x!=3 || lp2.lopnColor!=RGB(0,128,0)) return "GetObject pen";
    lb.lbStyle=BS_HATCHED; lb.lbColor=RGB(1,2,3); lb.lbHatch=HS_CROSS;
    brush=CreateBrushIndirect(&lb);
    if(!brush || GetObject(brush,sizeof(lb2),&lb2)!=sizeof(LOGBRUSH) || lb2.lbStyle!=BS_HATCHED || lb2.lbColor!=RGB(1,2,3) || lb2.lbHatch!=HS_CROSS) return "GetObject brush";
    DeleteObject(font); DeleteObject(pen); DeleteObject(brush);
    bitmap=CreateBitmap(16,8,1,1,NULL);
    if(GetObject(bitmap,sizeof(bm),&bm)!=sizeof(BITMAP) || bm.bmWidth!=16 || bm.bmHeight!=8 || bm.bmWidthBytes!=2 || bm.bmPlanes!=1 || bm.bmBitsPixel!=1) return "GetObject bitmap";
    DeleteObject(bitmap);
    if(MulDiv(100,3,4)!=75 || MulDiv(-7,1,2)!=-4 || MulDiv(1,1,0)!=-32768) return "MulDiv";
    dc=GetDC(h);
    if(!GetTextMetrics(dc,&tm) || tm.tmHeight<8 || tm.tmAveCharWidth<4 || tm.tmAscent>tm.tmHeight) failed="GetTextMetrics";
    else if(!GetCharWidth(dc,'A','C',widths) || widths[0]<4 || widths[2]<4) failed="GetCharWidth";
    mem=CreateCompatibleDC(dc); bitmap=CreateCompatibleBitmap(dc,32,32);
    ReleaseDC(h,dc);
    if(failed) return failed;
    old=SelectObject(mem,bitmap);
    PatBlt(mem,0,0,32,32,WHITENESS);
    brush=CreateSolidBrush(RGB(255,0,0)); SelectObject(mem,brush); SelectObject(mem,GetStockObject(NULL_PEN));
    pts[0].x=2; pts[0].y=2; pts[1].x=20; pts[1].y=2; pts[2].x=2; pts[2].y=20;
    Polygon(mem,pts,3);
    if(GetPixel(mem,4,4)!=RGB(255,0,0) || GetPixel(mem,25,25)!=RGB(255,255,255)) failed="Polygon";
    SelectObject(mem,GetStockObject(BLACK_PEN));
    pts[0].x=0; pts[0].y=30; pts[1].x=31; pts[1].y=30;
    Polyline(mem,pts,2);
    if(!failed && GetPixel(mem,10,30)!=RGB(0,0,0)) failed="Polyline";
    /* An 8x2 DIB of 4 bits per pixel: red, blue, blue, red, then red. */
    memset(&bi,0,sizeof(bi)); bi.h.biSize=sizeof(BITMAPINFOHEADER); bi.h.biWidth=8; bi.h.biHeight=2;
    bi.h.biPlanes=1; bi.h.biBitCount=4; bi.h.biClrUsed=2; bi.c[0].rgbRed=255; bi.c[1].rgbBlue=255;
    StretchDIBits(mem,0,0,8,2,0,0,8,2,bits,(BITMAPINFO FAR *)&bi,DIB_RGB_COLORS,SRCCOPY);
    if(!failed && (GetPixel(mem,0,0)!=RGB(255,0,0) || GetPixel(mem,1,0)!=RGB(0,0,255) || GetPixel(mem,3,1)!=RGB(255,0,0))) failed="StretchDIBits";
    SetDIBitsToDevice(mem,10,10,8,2,0,0,0,2,bits,(BITMAPINFO FAR *)&bi,DIB_RGB_COLORS);
    if(!failed && (GetPixel(mem,12,10)!=RGB(0,0,255) || GetPixel(mem,13,11)!=RGB(255,0,0))) failed="SetDIBitsToDevice";
    SetPixel(mem,0,31,RGB(255,0,0));
    SelectObject(mem,old); DeleteObject(brush);
    bi.h.biWidth=32; bi.h.biHeight=32; bi.h.biBitCount=24; bi.h.biClrUsed=0;
    if(!failed && (GetDIBits(mem,bitmap,0,1,back,(BITMAPINFO FAR *)&bi,DIB_RGB_COLORS)!=1 || back[0]!=0 || back[2]!=255 || back[3]!=255)) failed="GetDIBits";
    DeleteObject(bitmap);
    bi.h.biWidth=8; bi.h.biHeight=2; bi.h.biBitCount=4; bi.h.biClrUsed=2;
    dc=GetDC(h); dib=CreateDIBitmap(dc,&bi.h,CBM_INIT,bits,(BITMAPINFO FAR *)&bi,DIB_RGB_COLORS); ReleaseDC(h,dc);
    old=SelectObject(mem,dib);
    if(!failed && (!dib || GetPixel(mem,2,0)!=RGB(0,0,255) || GetPixel(mem,0,1)!=RGB(255,0,0))) failed="CreateDIBitmap";
    picture=LoadBitmap(instance,MAKEINTRESOURCE(IDB_TEST));
    if(!failed && (!picture || GetObject(picture,sizeof(bm),&bm)!=sizeof(BITMAP) || bm.bmWidth!=32 || bm.bmHeight!=32)) failed="LoadBitmap";
    SelectObject(mem,picture);
    if(!failed && (GetPixel(mem,6,8)!=RGB(255,0,0) || GetPixel(mem,20,8)!=RGB(255,255,255) || GetPixel(mem,1,1)!=RGB(0,0,0))) failed="bitmap resource";
    SelectObject(mem,old); DeleteObject(picture); DeleteObject(dib);
    if(!failed) {
        dc=GetDC(h); bitmap=CreateCompatibleBitmap(dc,32,32); ReleaseDC(h,dc);
        old=SelectObject(mem,bitmap); failed=gdi_test2(mem);
        if(!failed) failed=metafile_test(h,mem);
        SelectObject(mem,old); DeleteObject(bitmap);
    }
    DeleteDC(mem);
    return failed;
}

int FAR PASCAL _export Twice(int value) {return value*2+base;}
/* The library: imported entry points with its own data and heap, a call
 * back into this program, LoadLibrary and GetProcAddress, a window class
 * of the library's. */
static const char *dll_test(HWND h) {
    HINSTANCE lib; FARPROC calls; HWND w; char text[32];
    if(DllAdd(2,3)!=5 || DllCalls()!=1) return "DllAdd";
    lstrcpy(text,DllText()); if(lstrcmp(text,"from W16DLL")) return "DllText";
    if(!DllHeap()) return "DllHeap";
    if(DllCallBack(MakeProcInstance((FARPROC)Twice,instance),5)!=111) return "DllCallBack";
    lib=LoadLibrary("W16DLL.DLL"); if((UINT)lib<32) return "LoadLibrary";
    if(GetModuleHandle("W16DLL")==NULL || GetModuleUsage(lib)!=2) return "GetModuleUsage";
    calls=GetProcAddress(lib,"DllCalls");
    if(!calls || ((int (FAR PASCAL *)(void))calls)()!=1 || GetProcAddress(lib,MAKEINTRESOURCE(2))!=(FARPROC)DllAdd) return "GetProcAddress";
    if(!DllRegister()) return "DllRegister";
    w=CreateWindow("W16DllClass","",WS_CHILD,0,0,10,10,h,NULL,lib,NULL); if(!w) return "library class";
    if(SendMessage(w,WM_USER+5,7,0)!=1008) return "library window procedure";
    DestroyWindow(w);
    FreeLibrary(lib); if(GetModuleUsage(GetModuleHandle("W16DLL"))!=1) return "FreeLibrary";
    if((UINT)LoadLibrary("NOSUCH.DLL")>=32) return "LoadLibrary error";
    return NULL;
}

/* MDI: a frame, its client and a child made with WM_MDICREATE. */
static HWND mdi_client;
static int child_created,child_activated;
LRESULT FAR PASCAL _export FrameProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefFrameProc(h,mdi_client,msg,wp,lp);}
LRESULT FAR PASCAL _export ChildProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_CREATE) {
        MDICREATESTRUCT FAR *mc=(MDICREATESTRUCT FAR *)((LPCREATESTRUCT)lp)->lpCreateParams;
        if(mc && mc->lParam==0x5678L && !lstrcmp(mc->szTitle,"Child")) child_created++;
    }
    if(msg==WM_MDIACTIVATE && wp && (HWND)LOWORD(lp)==h) child_activated++;
    return DefMDIChildProc(h,msg,wp,lp);
}
static const char *mdi_test(void) {
    WNDCLASS wc; CLIENTCREATESTRUCT ccs; MDICREATESTRUCT mc; HWND frame,child; DWORD active;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=FrameProc; wc.hInstance=instance; wc.hbrBackground=(HBRUSH)(COLOR_APPWORKSPACE+1); wc.lpszClassName="W16Frame";
    if(!RegisterClass(&wc)) return "frame class";
    wc.lpfnWndProc=ChildProc; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName="W16Child";
    if(!RegisterClass(&wc)) return "child class";
    frame=CreateWindow("W16Frame","MDI",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,300,200,NULL,NULL,instance,NULL);
    ccs.hWindowMenu=NULL; ccs.idFirstChild=100;
    mdi_client=CreateWindow("MDICLIENT",NULL,WS_CHILD|WS_CLIPCHILDREN|WS_VISIBLE,0,0,280,150,frame,NULL,instance,(LPSTR)&ccs);
    if(!frame || !mdi_client) return "MDI client";
    mc.szClass="W16Child"; mc.szTitle="Child"; mc.hOwner=instance; mc.x=mc.y=mc.cx=mc.cy=CW_USEDEFAULT; mc.style=0; mc.lParam=0x5678L;
    child=(HWND)LOWORD(SendMessage(mdi_client,WM_MDICREATE,0,(LPARAM)(MDICREATESTRUCT FAR *)&mc));
    if(!child) return "WM_MDICREATE";
    if(child_created!=1) return "MDI child WM_CREATE";
    active=SendMessage(mdi_client,WM_MDIGETACTIVE,0,0);
    if((HWND)LOWORD(active)!=child || HIWORD(active)) return "WM_MDIGETACTIVE";
    if(!child_activated) return "WM_MDIACTIVATE";
    if(GetParent(child)!=mdi_client || GetDlgCtrlID(child)!=100) return "MDI child";
    SendMessage(mdi_client,WM_MDIDESTROY,(WPARAM)child,0);
    if(IsWindow(child)) return "WM_MDIDESTROY";
    DestroyWindow(frame);
    return NULL;
}
static FARPROC button_proc;
static int super_messages;
LRESULT FAR PASCAL _export SuperButton(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==BM_SETCHECK) super_messages++;
    return CallWindowProc(button_proc,h,msg,wp,lp);
}
/* Resources, atoms, properties, the clipboard, strings and the rest. */
static const char *misc_test(HWND h) {
    HRSRC r; HGLOBAL g; LPSTR p; ATOM atom; char text[64]; HANDLE clip; BOOL ok; WNDCLASS wc; HWND button; HDWP defer;
    RECT rc; POINT pt; int args[2],border;
    r=FindResource(instance,MAKEINTRESOURCE(IDR_DATA),RT_RCDATA);
    if(!r || SizeofResource(instance,r)<14 || FindResource(instance,"#7",RT_RCDATA)!=r) return "FindResource";
    g=LoadResource(instance,r); p=LockResource(g);
    if(!p || lstrcmp(p,"resource data")) return "LockResource";
    UnlockResource(g); FreeResource(g);
    r=FindResource(instance,"ART","TEXTFILE"); g=r?LoadResource(instance,r):NULL; p=g?LockResource(g):NULL;
    /* The drawing's text: its license line, then its own "# W16APP" line. */
    if(p) while(*p && *p!='\n') p++;
    if(!p || p[0]!='\n' || p[1]!='#' || p[3]!='W') return "custom resource";
    UnlockResource(g); FreeResource(g);
    atom=AddAtom("W16Atom");
    if(atom<0xc000 || FindAtom("w16atom")!=atom) return "AddAtom";
    GetAtomName(atom,text,sizeof(text)); if(lstrcmp(text,"W16Atom")) return "GetAtomName";
    DeleteAtom(atom); if(FindAtom("W16Atom")) return "DeleteAtom";
    if(GlobalAddAtom("#42")!=42) return "integer atom";
    atom=GlobalAddAtom("W16Global"); if(!atom || GlobalFindAtom("W16GLOBAL")!=atom) return "GlobalAddAtom";
    GlobalDeleteAtom(atom);
    SetProp(h,"W16Prop",(HANDLE)0x1234); if(GetProp(h,"W16Prop")!=(HANDLE)0x1234) return "GetProp";
    RemoveProp(h,"W16Prop"); if(GetProp(h,"W16Prop")) return "RemoveProp";
    g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,16); p=GlobalLock(g); lstrcpy(p,"clip text"); GlobalUnlock(g);
    if(!OpenClipboard(h)) return "OpenClipboard";
    EmptyClipboard(); SetClipboardData(CF_TEXT,g); CloseClipboard();
    OpenClipboard(h); clip=GetClipboardData(CF_TEXT); p=clip?GlobalLock(clip):NULL;
    ok=p && !lstrcmp(p,"clip text");
    if(p) GlobalUnlock(clip);
    CloseClipboard();
    if(!ok) return "GetClipboardData";
    lstrcpy(text,"Mixed Case"); AnsiUpper(text); if(lstrcmp(text,"MIXED CASE")) return "AnsiUpper";
    if(LOWORD((DWORD)AnsiLower((LPSTR)MAKELP(0,'Q')))!='q') return "AnsiLower";
    if(AnsiNext(text)!=text+1 || AnsiPrev(text,text+3)!=text+2) return "AnsiNext";
    args[0]=7; args[1]=-3; wvsprintf(text,"%d/%d",(LPSTR)args); if(lstrcmp(text,"7/-3")) return "wvsprintf";
    hmemcpy(text,"hmem",5); if(lstrcmp(text,"hmem")) return "hmemcpy";
    if(IsBadReadPtr(text,10) || !IsBadReadPtr(MAKELP(0,0x10),1) || IsBadStringPtr(text,100)) return "IsBadReadPtr";
    /* A superclass of BUTTON: its procedure comes from GetClassInfo. */
    if(!GetClassInfo(NULL,"BUTTON",&wc) || !wc.lpfnWndProc) return "GetClassInfo";
    button_proc=(FARPROC)wc.lpfnWndProc;
    wc.lpfnWndProc=SuperButton; wc.hInstance=instance; wc.lpszClassName="W16Button";
    if(!RegisterClass(&wc)) return "superclass";
    button=CreateWindow("W16Button","Super",WS_CHILD|BS_AUTOCHECKBOX,0,0,60,20,h,(HMENU)306,instance,NULL);
    SendMessage(button,BM_SETCHECK,1,0);
    if(!button || super_messages!=1 || SendMessage(button,BM_GETCHECK,0,0)!=1) return "superclass messages";
    defer=BeginDeferWindowPos(1); defer=DeferWindowPos(defer,button,NULL,5,6,40,20,SWP_NOZORDER|SWP_NOACTIVATE);
    if(!defer || !EndDeferWindowPos(defer)) return "DeferWindowPos";
    pt.x=0; pt.y=0; MapWindowPoints(button,h,&pt,1); if(pt.x!=5 || pt.y!=6) return "MapWindowPoints";
    SetRect(&rc,0,0,10,10); pt.x=5; pt.y=5; if(!PtInRect(&rc,pt)) return "PtInRect";
    pt.x=12; if(PtInRect(&rc,pt)) return "PtInRect outside";
    DestroyWindow(button);
    border=0; SystemParametersInfo(SPI_GETBORDER,0,&border,0); if(border<=0) return "SystemParametersInfo";
    if(CheckMenuItem(GetMenu(h),IDM_ABOUT,MF_CHECKED)!=MF_UNCHECKED || CheckMenuItem(GetMenu(h),IDM_ABOUT,MF_UNCHECKED)!=MF_CHECKED) return "CheckMenuItem";
    {
        PRINTDLG pd; LPDEVNAMES n; BOOL ok;
        if(GetFileTitle("C:\\DIR\\FILE.TXT",text,sizeof(text)) || lstrcmp(text,"FILE.TXT") || GetFileTitle("C:\\X\\LONGNAME.TXT",text,4)!=13) return "GetFileTitle";
        /* The default printer, WIN.INI's, without a dialog; not again into the handles it gave. */
        memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=h; pd.Flags=PD_RETURNDEFAULT;
        if(!PrintDlg(&pd) || !pd.hDevMode || !pd.hDevNames || pd.hDC) return "PrintDlg PD_RETURNDEFAULT";
        n=(LPDEVNAMES)GlobalLock(pd.hDevNames);
        ok=n && !lstrcmp((LPSTR)n+n->wDriverOffset,"PSCRIPT") && !lstrcmp((LPSTR)n+n->wDeviceOffset,"PostScript Printer") &&
           !lstrcmp((LPSTR)n+n->wOutputOffset,"LPT1:") && (n->wDefault&DN_DEFAULTPRN);
        GlobalUnlock(pd.hDevNames);
        if(!ok) return "DEVNAMES";
        if(PrintDlg(&pd) || CommDlgExtendedError()!=PDERR_RETDEFFAILURE) return "PD_RETURNDEFAULT again";
        GlobalFree(pd.hDevMode); GlobalFree(pd.hDevNames);
    }
    {
        /* Communications: a DCB from MODE's form; no port driver here, so no COM1. */
        DCB dcb;
        if(BuildCommDCB("COM1:2400,e,7,2",&dcb) || dcb.Id!=0 || dcb.BaudRate!=2400 || dcb.ByteSize!=7 ||
           dcb.Parity!=EVENPARITY || dcb.StopBits!=TWOSTOPBITS) return "BuildCommDCB";
        if(OpenComm("COM1",1024,128)>=0 || OpenComm("COM9X",1024,128)!=IE_BADID) return "OpenComm";
    }
    {
        /* The printer driver's own entry points, as a program's Printer Setup reaches them. */
        typedef int (FAR PASCAL *EXTDEVMODE)(HWND,HANDLE,LPDEVMODE,LPSTR,LPSTR,LPDEVMODE,LPSTR,WORD);
        typedef DWORD (FAR PASCAL *DEVCAPS)(LPSTR,LPSTR,WORD,LPSTR,LPDEVMODE);
        HINSTANCE lib=LoadLibrary("PSCRIPT.DRV"); EXTDEVMODE ext; DEVCAPS caps; DEVMODE dm; WORD papers[8];
        if((UINT)lib<32 || !(ext=(EXTDEVMODE)GetProcAddress(lib,"EXTDEVICEMODE")) ||
           !(caps=(DEVCAPS)GetProcAddress(lib,"DEVICECAPABILITIES"))) return "PSCRIPT.DRV";
        if(ext(h,lib,NULL,"PostScript Printer","LPT1:",NULL,NULL,0)!=sizeof(DEVMODE) ||
           ext(h,lib,&dm,"PostScript Printer","LPT1:",NULL,NULL,DM_COPY)!=IDOK || dm.dmPaperSize!=1 || dm.dmOrientation!=1) return "ExtDeviceMode";
        if(caps("PostScript Printer","LPT1:",2,(LPSTR)papers,NULL)!=5 || papers[0]!=1) return "DeviceCapabilities";
        FreeLibrary(lib);
    }
    {
        /* SHELL's registration database, MMSYSTEM, TOOLHELP, KEYBOARD. */
        HKEY key; LONG size=sizeof(text); DWORD t0; TIMERINFO ti;
        if(RegCreateKey(HKEY_CLASSES_ROOT,"W16App\\shell\\open",&key) || RegSetValue(key,"command",REG_SZ,"w16app.exe",10)) return "RegSetValue";
        RegCloseKey(key);
        if(RegQueryValue(HKEY_CLASSES_ROOT,"W16App\\shell\\open\\command",text,&size) || lstrcmp(text,"w16app.exe")) return "RegQueryValue";
        if(RegOpenKey(HKEY_CLASSES_ROOT,"W16App",&key) || RegEnumKey(key,0,text,sizeof(text)) || lstrcmp(text,"shell") ||
           !RegEnumKey(key,1,text,sizeof(text))) return "RegEnumKey";
        RegCloseKey(key);
        if(RegDeleteKey(HKEY_CLASSES_ROOT,"W16App") || !RegOpenKey(HKEY_CLASSES_ROOT,"W16App\\shell",&key)) return "RegDeleteKey";
        t0=timeGetTime(); if(!t0 || waveOutGetNumDevs() || midiOutGetNumDevs()) return "MMSYSTEM";
        ti.dwSize=sizeof(ti); if(!TimerCount(&ti) || ti.dwmsSinceStart<t0) return "TimerCount";
        if(GetKeyboardType(0)!=4) return "GetKeyboardType";
        /* A function WOW has no implementation of returns 0, and
         * GetProcAddress does not offer it. */
        GetSystemDebugState();
        if(GetProcAddress(GetModuleHandle("USER"),"GetSystemDebugState")) return "GetProcAddress of a stub";
    }
    return mdi_test();
}

/* --- hooks -------------------------------------------------------------------- */
typedef struct {LPARAM lParam; WPARAM wParam; UINT message; HWND hwnd;} CallWndParams;
static HOOKPROC old_hook;
static HHOOK key_hook,cwp_hook,cbt_hook,filter_hook,play_hook;
static int key_skipped,cbt_vetoed,played,play_at;
static HWND cwp_seen;
static const EVENTMSG play_keys[6]={{WM_KEYDOWN,VK_SHIFT,1,0},{WM_KEYDOWN,'H',1,0},{WM_KEYUP,'H',1,0},
                                    {WM_KEYUP,VK_SHIFT,1,0},{WM_KEYDOWN,'I',1,20},{WM_KEYUP,'I',1,20}};
LRESULT FAR PASCAL _export GetMsgHook(int code,WPARAM wp,LPARAM lp) {
    MSG FAR *m=(MSG FAR *)lp;
    if(code>=0 && m->message==WM_USER+20) m->wParam=5;
    return DefHookProc(code,wp,lp,&old_hook);
}
LRESULT FAR PASCAL _export KeyHook(int code,WPARAM wp,LPARAM lp) {
    if(code>=0 && wp==VK_F9) {key_skipped++; return 1;}
    return CallNextHookEx(key_hook,code,wp,lp);
}
LRESULT FAR PASCAL _export CallWndHook(int code,WPARAM wp,LPARAM lp) {
    CallWndParams FAR *c=(CallWndParams FAR *)lp;
    if(code>=0 && c->message==WM_USER+21 && c->wParam==3 && c->lParam==4) cwp_seen=c->hwnd;
    return CallNextHookEx(cwp_hook,code,wp,lp);
}
LRESULT FAR PASCAL _export CbtHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        CREATESTRUCT FAR *cs=((CBT_CREATEWND FAR *)lp)->lpcs;
        if(cs->lpszName && !lstrcmp(cs->lpszName,"veto")) {cbt_vetoed++; return 1;}
        if(cs->lpszName && !lstrcmp(cs->lpszName,"moved")) cs->x=33;
    }
    return CallNextHookEx(cbt_hook,code,wp,lp);
}
LRESULT FAR PASCAL _export FilterHook(int code,WPARAM wp,LPARAM lp) {
    MSG FAR *m=(MSG FAR *)lp;
    if(code==MSGF_DIALOGBOX && m->message==WM_USER+22) {EndDialog(m->hwnd,42); return 1;}
    return CallNextHookEx(filter_hook,code,wp,lp);
}
BOOL FAR PASCAL _export PostingProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)wp; (void)lp;
    if(msg==WM_INITDIALOG) {PostMessage(dlg,WM_USER+22,0,0); return TRUE;}
    return FALSE;
}
LRESULT FAR PASCAL _export PlayHook(int code,WPARAM wp,LPARAM lp) {
    (void)wp;
    if(code==HC_GETNEXT) {
        *(EVENTMSG FAR *)lp=play_keys[play_at];
        return play_at?(LRESULT)(play_keys[play_at].time-play_keys[play_at-1].time):0;
    }
    if(code==HC_SKIP && ++play_at==6) {UnhookWindowsHookEx(play_hook); played=1;}
    return 0;
}
static HOOKPROC hook_proc(FARPROC f) {return (HOOKPROC)MakeProcInstance(f,instance);}
/* LZEXPAND: a file COMPRESS made (LZTEST.TX_), its name, reading, seeking
 * and LZCopy to C:\WINDOWS\LZTEST.TXT, which the test compares with the
 * original. */
static const char *lz_test(void) {
    char name[16],text[24]; OFSTRUCT of; int h,to; LONG n;
    if(GetExpandedName("LZTEST.TX_",name)!=1 || lstrcmpi(name,"LZTEST.TXT")) return "GetExpandedName";
    if((h=LZOpenFile("LZTEST.TXT",&of,OF_READ))<0) return "LZOpenFile";
    if(LZRead(h,text,21)!=21 || memcmp(text,"Expanded by LZEXPAND.",21)) return "LZRead";
    if(LZSeek(h,0,2)!=365 || LZSeek(h,0,0)!=0) return "LZSeek";
    if((to=OpenFile("C:\\WINDOWS\\LZTEST.TXT",&of,OF_CREATE))<0) return "OpenFile";
    n=LZCopy(h,to);
    LZClose(h); _lclose(to);
    return n==365?NULL:"LZCopy";
}
/* Printing the Windows 3.0 way, PSCRIPT's escapes with an abort procedure
 * and bands, to C:\\WINDOWS\\W16APP.PS: two copies of one page with a green
 * rectangle at (100,100)-(400,250) and "Win16" at (100,300). */
static int abort_calls;
BOOL FAR PASCAL _export AbortProc(HDC dc,int code) {(void)dc; (void)code; abort_calls++; return TRUE;}
static const char *print_test(void) {
    HDC p=CreateDC("PSCRIPT","PostScript Printer","C:\\WINDOWS\\W16APP.PS",NULL);
    POINT pt; RECT band; int code=NEXTBAND,copies=2,actual=0,bands=0; HBRUSH green; char tech[16];
    if(!p) return "CreateDC PSCRIPT";
    if(GetDeviceCaps(p,LOGPIXELSX)!=150 || GetDeviceCaps(p,HORZRES)!=1200) return "printer caps";
    if(!Escape(p,QUERYESCSUPPORT,sizeof(code),(LPSTR)&code,NULL)) return "QUERYESCSUPPORT";
    if(Escape(p,GETPHYSPAGESIZE,0,NULL,&pt)<=0 || pt.x!=1275 || pt.y!=1650) return "GETPHYSPAGESIZE";
    if(Escape(p,SETCOPYCOUNT,sizeof(int),(LPSTR)&copies,&actual)<=0 || actual!=2) return "SETCOPYCOUNT";
    if(Escape(p,GETTECHNOLOGY,0,NULL,tech)<=0 || lstrcmp(tech,"PostScript")) return "GETTECHNOLOGY";
    if(Escape(p,SETABORTPROC,0,(LPSTR)MakeProcInstance((FARPROC)AbortProc,instance),NULL)<=0) return "SETABORTPROC";
    if(Escape(p,STARTDOC,6,"W16APP",NULL)<=0) return "STARTDOC";
    green=CreateSolidBrush(RGB(0,255,0)); SelectObject(p,green);
    while(Escape(p,NEXTBAND,0,NULL,&band)>0 && !IsRectEmpty(&band)) {
        if(band.right!=1200 || band.bottom!=1575) return "NEXTBAND";
        Rectangle(p,100,100,400,250); TextOut(p,100,300,"Win16",5); bands++;
    }
    if(bands!=1 || Escape(p,ENDDOC,0,NULL,NULL)<=0 || !abort_calls) return "ENDDOC";
    SelectObject(p,GetStockObject(WHITE_BRUSH)); DeleteObject(green);
    return DeleteDC(p)?NULL:"DeleteDC";
}
static const char *hook_test(HWND h) {
    HTASK task=GetCurrentTask(); MSG m; HWND w; char text[16]; DWORD start; int result;
    HOOKPROC get_msg=hook_proc((FARPROC)GetMsgHook);
    old_hook=SetWindowsHook(WH_GETMESSAGE,get_msg);
    PostMessage(h,WM_USER+20,1,0);
    if(!PeekMessage(&m,h,WM_USER+20,WM_USER+20,PM_REMOVE) || m.wParam!=5) return "WH_GETMESSAGE";
    if(!UnhookWindowsHook(WH_GETMESSAGE,get_msg)) return "UnhookWindowsHook";
    PostMessage(h,WM_USER+20,1,0);
    if(!PeekMessage(&m,h,WM_USER+20,WM_USER+20,PM_REMOVE) || m.wParam!=1) return "WH_GETMESSAGE removed";
    key_hook=SetWindowsHookEx(WH_KEYBOARD,hook_proc((FARPROC)KeyHook),instance,task);
    cwp_hook=SetWindowsHookEx(WH_CALLWNDPROC,hook_proc((FARPROC)CallWndHook),instance,task);
    cbt_hook=SetWindowsHookEx(WH_CBT,hook_proc((FARPROC)CbtHook),instance,task);
    filter_hook=SetWindowsHookEx(WH_MSGFILTER,hook_proc((FARPROC)FilterHook),instance,task);
    if(!key_hook || !cwp_hook || !cbt_hook || !filter_hook) return "SetWindowsHookEx";
    PostMessage(h,WM_KEYDOWN,VK_F9,0);
    if(PeekMessage(&m,h,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE) || key_skipped!=1) return "WH_KEYBOARD";
    SendMessage(h,WM_USER+21,3,4); if(cwp_seen!=h) return "WH_CALLWNDPROC";
    if(CreateWindow("W16App","veto",WS_POPUP,0,0,50,50,NULL,NULL,instance,NULL) || cbt_vetoed!=1) return "HCBT_CREATEWND veto";
    w=CreateWindow("W16App","moved",WS_POPUP,10,10,50,50,NULL,NULL,instance,NULL);
    {RECT r; GetWindowRect(w,&r); if(!w || r.left!=33) return "HCBT_CREATEWND position";}
    DestroyWindow(w);
    result=DialogBox(instance,MAKEINTRESOURCE(IDD_ABOUT),h,(DLGPROC)MakeProcInstance((FARPROC)PostingProc,instance));
    if(result!=42) return "WH_MSGFILTER";
    if(!UnhookWindowsHookEx(key_hook) || !UnhookWindowsHookEx(cwp_hook) || !UnhookWindowsHookEx(cbt_hook) ||
       !UnhookWindowsHookEx(filter_hook)) return "UnhookWindowsHookEx";
    PostMessage(h,WM_KEYDOWN,VK_F9,0);
    if(!PeekMessage(&m,h,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE) || key_skipped!=1) return "hook removed";
    /* Keys played into an edit control, Shift held for the first. */
    w=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,0,0,100,24,h,(HMENU)310,instance,NULL);
    SetFocus(w);
    play_at=0; played=0; play_hook=SetWindowsHookEx(WH_JOURNALPLAYBACK,hook_proc((FARPROC)PlayHook),instance,NULL);
    for(start=GetTickCount();!played && GetTickCount()-start<5000;) if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);}
    while(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);}
    GetWindowText(w,text,sizeof(text)); DestroyWindow(w);
    if(!played || lstrcmp(text,"Hi")) return "WH_JOURNALPLAYBACK";
    return NULL;
}

int PASCAL WinMain(HINSTANCE inst,HINSTANCE prev,LPSTR cmd,int show) {
    WNDCLASS wc; HWND h; HACCEL accel; MSG msg; char line[80];
    const char *failed;
    (void)cmd;
    instance=inst;
    failed=self_test();
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: run-time library ok");
    if(!prev) {
        wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WndProc; wc.cbClsExtra=0; wc.cbWndExtra=0;
        wc.hInstance=inst; wc.hIcon=LoadIcon(inst,MAKEINTRESOURCE(IDI_APP)); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName=MAKEINTRESOURCE(IDR_MENU); wc.lpszClassName="W16App";
        if(!RegisterClass(&wc)) {trace("W16APP: RegisterClass failed"); return 1;}
    }
    h=CreateWindow("W16App","Win16 Test",WS_OVERLAPPEDWINDOW,40,40,320,200,NULL,NULL,inst,NULL);
    if(!h) {trace("W16APP: CreateWindow failed"); return 1;}
    main_window=h;
    while(cmd && (*cmd==' ' || *cmd=='\t')) cmd++;
    if(cmd && !_fstrnicmp(cmd,"dde",3) && (cmd[3]==0 || cmd[3]==' ' || cmd[3]=='\r')) {
        failed=dde_test(h);
        if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: dde ok");
        DestroyWindow(h);
        return 0;
    }
    accel=LoadAccelerators(inst,MAKEINTRESOURCE(IDR_ACCEL));
    ShowWindow(h,show);
    UpdateWindow(h);
    failed=window_test(h);
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: windows ok");
    failed=gdi_test(h);
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: gdi ok");
    failed=dll_test(h);
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: library ok");
    failed=misc_test(h);
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: more ok");
    failed=hook_test(h);
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: hooks ok");
    failed=lz_test();
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: lzexpand ok");
    failed=print_test();
    if(failed) {wsprintf(line,"W16APP: %s FAILED",(LPSTR)failed); trace(line);} else trace("W16APP: printing ok");
    while(GetMessage(&msg,NULL,0,0)) {
        if(find_dialog && IsDialogMessage(find_dialog,&msg)) continue;
        if(!TranslateAccelerator(h,accel,&msg)) {TranslateMessage(&msg); DispatchMessage(&msg);}
    }
    wsprintf(line,"W16APP: exit %d",(int)msg.wParam);
    trace(line);
    return msg.wParam;
}
