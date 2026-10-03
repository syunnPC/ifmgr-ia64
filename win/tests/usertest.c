/* SPDX-License-Identifier: GPL-2.0-or-later
 * USERTEST: checks USER and KERNEL without input and reports through
 * OutputDebugString ("USERTEST: n checks passed" or "USERTEST: FAIL line
 * n"): resources, menus, dialogs and controls, timers, the clipboard,
 * profiles, files, messages sent between tasks, MDI, hooks, painting and
 * window functions.
 * Then it keeps a window open for the QEMU test, which drives its menus,
 * dialog and message box with the keyboard and the mouse; it reports what
 * it was told ("USERTEST: command n", "USERTEST: dialog ...",
 * "USERTEST: message box n").
 * "USERTEST child" is the second task of the SendMessage check.
 */
#include <windows.h>
#include "usertest.h"

static HINSTANCE instance;
static HWND main_wnd;
static HACCEL accel;
static int passed,failed;
static void check(BOOL ok,int line) {
    char text[64];
    if(ok) {passed++; return;}
    failed++; wsprintf(text,"USERTEST: FAIL line %d",line); OutputDebugString(text);
}
#define CHECK(x) check((x)!=0,__LINE__)
static void report(LPCSTR format,LPCSTR a,int b) {char text[160]; wsprintf(text,format,a,b); OutputDebugString(text);}

/* Run the message loop until a condition or a timeout. */
static BOOL pump_until(BOOL *flag,DWORD ms) {
    DWORD start=GetTickCount(); MSG m;
    while(!*flag && GetTickCount()-start<ms) {
        if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);}
    }
    return *flag;
}

/* --- the second task ----------------------------------------------------------------- */
LRESULT CALLBACK ChildProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_USER+1) return (LRESULT)(wp+lp);
    if(msg==WM_DESTROY) {PostQuitMessage(0); return 0;}
    return DefWindowProc(h,msg,wp,lp);
}
static int child_main(void) {
    WNDCLASS wc; MSG m; HWND h;
    memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=ChildProc; wc.hInstance=instance; wc.lpszClassName="UserTestChild";
    RegisterClass(&wc);
    h=CreateWindow("UserTestChild","child",WS_OVERLAPPED,0,0,100,60,NULL,NULL,instance,NULL);
    while(GetMessage(&m,NULL,0,0)) {TranslateMessage(&m); DispatchMessage(&m);}
    (void)h;
    return 0;
}

/* --- checks ------------------------------------------------------------------------ */
static BOOL timer_fired;
static LRESULT CALLBACK CheckProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_TIMER && wp==5) {timer_fired=TRUE; return 0;}
    if(msg==WM_USER+7) return 77;
    return DefWindowProc(h,msg,wp,lp);
}
static INT_PTR CALLBACK NoProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {(void)h; (void)wp; (void)lp; return msg==WM_INITDIALOG;}
static BOOL CALLBACK count_children(HWND h,LPARAM lp) {(void)h; (*(int *)lp)++; return TRUE;}
static void check_resources(void) {
    char text[64]; HMENU m,file;
    CHECK(LoadString(instance,IDS_TITLE,text,sizeof(text))==9 && !lstrcmp(text,"User test"));
    CHECK(LoadString(instance,IDS_SECOND,text,4)==3 && !lstrcmp(text,"Sec"));
    CHECK(LoadString(instance,999,text,sizeof(text))==0);
    m=LoadMenu(instance,"MAINMENU");
    CHECK(m && GetMenuItemCount(m)==2);
    file=GetSubMenu(m,0);
    CHECK(file && GetMenuItemCount(file)==7 && GetMenuItemID(file,0)==IDM_DIALOG);
    CHECK(GetMenuString(m,0,text,sizeof(text),MF_BYPOSITION) && !lstrcmp(text,"&File"));
    CHECK(GetMenuString(file,IDM_DIALOG,text,sizeof(text),MF_BYCOMMAND) && !lstrcmp(text,"&Dialog...\tCtrl+D"));
    CHECK(GetMenuState(file,IDM_CHECKED,MF_BYCOMMAND)&MF_CHECKED);
    CHECK(GetMenuState(file,IDM_GRAYED,MF_BYCOMMAND)&MF_GRAYED);
    CHECK(GetMenuState(file,2,MF_BYPOSITION)&MF_SEPARATOR);
    CheckMenuItem(m,IDM_CHECKED,MF_UNCHECKED); CHECK(!(GetMenuState(m,IDM_CHECKED,MF_BYCOMMAND)&MF_CHECKED));
    CHECK(AppendMenu(file,MF_STRING,999,"Extra") && GetMenuItemCount(file)==8);
    CHECK(DeleteMenu(file,999,MF_BYCOMMAND) && GetMenuItemCount(file)==7);
    CHECK(InsertMenu(file,0,MF_BYPOSITION|MF_STRING,998,"First") && GetMenuItemID(file,0)==998);
    CHECK(ModifyMenu(file,998,MF_BYCOMMAND|MF_STRING,997,"Changed") && GetMenuItemID(file,0)==997);
    CHECK(RemoveMenu(file,0,MF_BYPOSITION) && GetMenuItemCount(file)==7);
    CHECK(DestroyMenu(m) && !IsMenu(m));
    CHECK(LoadAccelerators(instance,"MAINACCEL")!=NULL);
    CHECK(LoadIcon(NULL,IDI_HAND)!=NULL && LoadCursor(NULL,IDC_IBEAM)!=NULL);
}
static void check_dialog(void) {
    HWND d=CreateDialog(instance,"TESTDLG",NULL,NoProc),list,combo,edit,multi; char text[64]; BOOL ok; MSG m;
    CHECK(d!=NULL);
    if(!d) return;
    list=GetDlgItem(d,IDC_LIST); combo=GetDlgItem(d,IDC_COMBO); edit=GetDlgItem(d,IDC_EDIT); multi=GetDlgItem(d,IDC_MULTI);
    CHECK(list && combo && edit && multi && GetDlgCtrlID(edit)==IDC_EDIT && GetParent(edit)==d);
    SetDlgItemText(d,IDC_EDIT,"hello");
    CHECK(GetDlgItemText(d,IDC_EDIT,text,sizeof(text))==5 && !lstrcmp(text,"hello"));
    SetDlgItemInt(d,IDC_EDIT,1234,FALSE); CHECK(GetDlgItemInt(d,IDC_EDIT,&ok,FALSE)==1234 && ok);
    SetDlgItemText(d,IDC_EDIT,"12x"); GetDlgItemInt(d,IDC_EDIT,&ok,FALSE); CHECK(!ok);
    /* Editing, selection and undo. */
    SetDlgItemText(d,IDC_EDIT,"hello");
    SendMessage(edit,EM_SETSEL,0,2); SendMessage(edit,EM_REPLACESEL,0,(LPARAM)"J");
    GetWindowText(edit,text,sizeof(text)); CHECK(!lstrcmp(text,"Jllo"));
    CHECK(SendMessage(edit,EM_CANUNDO,0,0)); SendMessage(edit,EM_UNDO,0,0);
    GetWindowText(edit,text,sizeof(text)); CHECK(!lstrcmp(text,"hello"));
    SendMessage(edit,EM_SETSEL,1,3);
    {DWORD a,b; SendMessage(edit,EM_GETSEL,(WPARAM)&a,(LPARAM)&b); CHECK(a==1 && b==3);}
    SetWindowText(multi,"a\r\nbb\r\nccc");
    CHECK(SendMessage(multi,EM_GETLINECOUNT,0,0)==3 && SendMessage(multi,EM_LINEINDEX,2,0)==7);
    CHECK(SendMessage(multi,EM_LINELENGTH,7,0)==3 && SendMessage(multi,EM_LINEFROMCHAR,4,0)==1);
    {char line[16]; *(WORD *)line=sizeof(line); CHECK(SendMessage(multi,EM_GETLINE,1,(LPARAM)line)==2 && line[0]=='b');}
    CHECK(GetWindowTextLength(multi)==10);
    /* Buttons. */
    CheckDlgButton(d,IDC_CHECK,BST_CHECKED); CHECK(IsDlgButtonChecked(d,IDC_CHECK)==BST_CHECKED);
    CheckRadioButton(d,IDC_RED,IDC_BLUE,IDC_BLUE); CHECK(!IsDlgButtonChecked(d,IDC_RED) && IsDlgButtonChecked(d,IDC_BLUE));
    /* A sorted list box. */
    SendMessage(list,LB_ADDSTRING,0,(LPARAM)"pear"); SendMessage(list,LB_ADDSTRING,0,(LPARAM)"apple"); SendMessage(list,LB_ADDSTRING,0,(LPARAM)"fig");
    CHECK(SendMessage(list,LB_GETCOUNT,0,0)==3);
    SendMessage(list,LB_GETTEXT,0,(LPARAM)text); CHECK(!lstrcmp(text,"apple"));
    CHECK(SendMessage(list,LB_FINDSTRING,(WPARAM)-1,(LPARAM)"f")==1 && SendMessage(list,LB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)"PEAR")==2);
    SendMessage(list,LB_SETCURSEL,2,0); CHECK(SendMessage(list,LB_GETCURSEL,0,0)==2);
    SendMessage(list,LB_SETITEMDATA,1,1234); CHECK(SendMessage(list,LB_GETITEMDATA,1,0)==1234);
    CHECK(SendMessage(list,LB_DELETESTRING,0,0)==2 && SendMessage(list,LB_GETTEXTLEN,0,0)==3);
    /* A drop-down list. */
    SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"one"); SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"two");
    SendMessage(combo,CB_SETCURSEL,1,0);
    CHECK(SendMessage(combo,CB_GETCURSEL,0,0)==1 && GetWindowText(combo,text,sizeof(text))==3 && !lstrcmp(text,"two"));
    CHECK(SendMessage(combo,CB_GETCOUNT,0,0)==2 && SendMessage(combo,CB_FINDSTRING,(WPARAM)-1,(LPARAM)"on")==0);
    /* Tab moves along the tab stops; arrows within a group. */
    ShowWindow(d,SW_SHOW);
    SetFocus(edit); CHECK(GetFocus()==edit);
    memset(&m,0,sizeof(m)); m.hwnd=edit; m.message=WM_KEYDOWN; m.wParam=VK_TAB;
    IsDialogMessage(d,&m); CHECK(GetFocus()==GetDlgItem(d,IDC_CHECK));
    /* The checked auto radio button is its group's tab stop. */
    CHECK(GetNextDlgTabItem(d,GetDlgItem(d,IDC_CHECK),FALSE)==GetDlgItem(d,IDC_BLUE));
    CHECK(GetNextDlgGroupItem(d,GetDlgItem(d,IDC_RED),FALSE)==GetDlgItem(d,IDC_BLUE));
    CHECK(SendMessage(d,DM_GETDEFID,0,0)==MAKELONG(IDOK,DC_HASDEFID));
    {RECT r; SetRect(&r,0,0,4,8); MapDialogRect(d,&r); CHECK(r.right==8 && r.bottom==16);}
    DestroyWindow(d); CHECK(!IsWindow(d) && !IsWindow(edit));
}
static void check_windows(HWND h) {
    HWND c,c2; RECT r; POINT p; int n=0;
    c=CreateWindow("BUTTON","Push",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,10,10,60,20,h,(HMENU)7,instance,NULL);
    c2=CreateWindow("STATIC","Label",WS_CHILD|WS_VISIBLE,80,10,60,20,h,(HMENU)8,instance,NULL);
    CHECK(c && c2 && GetParent(c)==h && IsChild(h,c) && GetDlgItem(h,8)==c2 && GetWindow(h,GW_CHILD)==c && GetWindow(c,GW_HWNDNEXT)==c2);
    EnumChildWindows(h,(WNDENUMPROC)count_children,(LPARAM)&n); CHECK(n==2);
    p.x=0; p.y=0; ClientToScreen(c,&p); GetWindowRect(c,&r); CHECK(p.x==r.left && p.y==r.top);
    SetWindowPos(c,NULL,20,30,0,0,SWP_NOSIZE|SWP_NOZORDER); GetWindowRect(c,&r); p.x=r.left; p.y=r.top; ScreenToClient(h,&p); CHECK(p.x==20 && p.y==30 && r.right-r.left==60);
    MoveWindow(c,5,5,40,15,TRUE); GetClientRect(c,&r); CHECK(r.right==40 && r.bottom==15);
    CHECK(SetProp(h,"Answer",(HANDLE)42) && GetProp(h,"Answer")==(HANDLE)42 && RemoveProp(h,"Answer")==(HANDLE)42 && !GetProp(h,"Answer"));
    EnableWindow(c,FALSE); CHECK(!IsWindowEnabled(c)); EnableWindow(c,TRUE); CHECK(IsWindowEnabled(c));
    CHECK(SendMessage(h,WM_USER+7,0,0)==77);
    SetWindowText(c,"Other"); {char t[16]; CHECK(GetWindowText(c,t,sizeof(t))==5 && !lstrcmp(t,"Other"));}
    {char t[16]; CHECK(GetClassName(c,t,sizeof(t)) && !lstrcmpi(t,"BUTTON"));}
    DestroyWindow(c); DestroyWindow(c2); CHECK(!IsWindow(c));
    SetScrollRange(h,SB_VERT,0,50,FALSE); SetScrollPos(h,SB_VERT,20,FALSE);
    {int lo,hi; GetScrollRange(h,SB_VERT,&lo,&hi); CHECK(lo==0 && hi==50 && GetScrollPos(h,SB_VERT)==20);}
    ShowWindow(h,SW_MINIMIZE); CHECK(IsIconic(h));
    ShowWindow(h,SW_SHOWMAXIMIZED); CHECK(IsZoomed(h) && !IsIconic(h));
    GetWindowRect(h,&r); CHECK(r.left<0 && r.right>=GetSystemMetrics(SM_CXSCREEN));
    ShowWindow(h,SW_RESTORE); CHECK(!IsZoomed(h));
    GetWindowRect(h,&r); CHECK(r.left==300 && r.top==200);
}
static void check_timer(HWND h) {
    timer_fired=FALSE;
    CHECK(SetTimer(h,5,50,NULL)==5);
    CHECK(pump_until(&timer_fired,3000));
    CHECK(KillTimer(h,5) && !KillTimer(h,5));
}
static void check_clipboard(HWND h) {
    HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,5); char *p=(char *)GlobalLock(g);
    lstrcpy(p,"clip"); GlobalUnlock(g);
    CHECK(OpenClipboard(h) && EmptyClipboard() && SetClipboardData(CF_TEXT,g)==g && CloseClipboard());
    CHECK(IsClipboardFormatAvailable(CF_TEXT) && !IsClipboardFormatAvailable(CF_BITMAP) && CountClipboardFormats()==1);
    CHECK(OpenClipboard(h));
    g=GetClipboardData(CF_TEXT); CHECK(g && !lstrcmp((char *)GlobalLock(g),"clip")); GlobalUnlock(g);
    CHECK(EnumClipboardFormats(0)==CF_TEXT && GetClipboardOwner()==h);
    CloseClipboard();
    CHECK(RegisterClipboardFormat("Test Format")>=0xc000);
}
static void check_kernel(void) {
    char text[260]; WIN32_FIND_DATA f; HANDLE h; BOOL found=FALSE; OFSTRUCT of; SYSTEMTIME t;
    CHECK(GetWindowsDirectory(text,sizeof(text))==10 && !lstrcmpi(text,"C:\\WINDOWS"));
    CHECK(GetSystemDirectory(text,sizeof(text)) && !lstrcmpi(text,"C:\\WINDOWS\\SYSTEM"));
    CHECK(WritePrivateProfileString("Test","Key","Value","C:\\TEST.INI"));
    CHECK(WritePrivateProfileString("Test","Number","42","C:\\TEST.INI"));
    CHECK(WritePrivateProfileString("Other","Key","x","C:\\TEST.INI"));
    CHECK(GetPrivateProfileString("test","key","none",text,sizeof(text),"C:\\TEST.INI")==5 && !lstrcmp(text,"Value"));
    CHECK(GetPrivateProfileInt("Test","Number",0,"C:\\TEST.INI")==42 && GetPrivateProfileInt("Test","Missing",7,"C:\\TEST.INI")==7);
    CHECK(WritePrivateProfileString("Test","Key","Changed","C:\\TEST.INI"));
    CHECK(GetPrivateProfileString("Test","Key","",text,sizeof(text),"C:\\TEST.INI") && !lstrcmp(text,"Changed"));
    CHECK(WritePrivateProfileString("Test","Key",NULL,"C:\\TEST.INI"));
    CHECK(GetPrivateProfileString("Test","Key","gone",text,sizeof(text),"C:\\TEST.INI") && !lstrcmp(text,"gone"));
    CHECK(GetPrivateProfileString("Other","Key","",text,sizeof(text),"C:\\TEST.INI") && !lstrcmp(text,"x"));
    CHECK(WritePrivateProfileString("Test",NULL,NULL,"C:\\TEST.INI"));
    CHECK(GetPrivateProfileInt("Test","Number",5,"C:\\TEST.INI")==5);
    h=FindFirstFile("C:\\*.*",&f);
    CHECK(h!=INVALID_HANDLE_VALUE);
    if(h!=INVALID_HANDLE_VALUE) {do if(!lstrcmpi(f.cFileName,"COMMAND.COM")) found=TRUE; while(FindNextFile(h,&f)); FindClose(h);}
    CHECK(found);
    CHECK(GetFileAttributes("C:\\WINDOWS")&FILE_ATTRIBUTE_DIRECTORY);
    CHECK(GetFileAttributes("C:\\NOSUCH.FIL")==INVALID_FILE_ATTRIBUTES);
    CHECK(GetDriveType(2)==DRIVE_FIXED && (GetLogicalDrives()&4));
    CHECK(OpenFile("WIN.COM",&of,OF_EXIST)!=HFILE_ERROR && !lstrcmpi(of.szPathName,"C:\\WINDOWS\\WIN.COM"));
    CHECK(CopyFile("C:\\TEST.INI","C:\\TEST2.INI",FALSE) && DeleteFile("C:\\TEST2.INI") && DeleteFile("C:\\TEST.INI"));
    CHECK(CreateDirectory("C:\\TESTDIR",NULL) && SetCurrentDirectory("C:\\TESTDIR") && GetCurrentDirectory(sizeof(text),text) && !lstrcmpi(text,"C:\\TESTDIR"));
    CHECK(SetCurrentDirectory("C:\\") && RemoveDirectory("C:\\TESTDIR"));
    GetLocalTime(&t); CHECK(t.wYear>=1980 && t.wMonth>=1 && t.wMonth<=12);
    CHECK(lstrcpyn(text,"abcdef",4)==text && !lstrcmp(text,"abc"));
}
static void check_tasks(void) {
    HWND child=NULL; DWORD start=GetTickCount(); MSG m;
    CHECK(WinExec("USERTEST child",SW_SHOWNOACTIVATE)>32);
    while(!(child=FindWindow("UserTestChild",NULL)) && GetTickCount()-start<10000)
        if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) DispatchMessage(&m);
    CHECK(child!=NULL);
    if(!child) return;
    CHECK(SendMessage(child,WM_USER+1,20,22)==42);
    CHECK(GetWindowTask(child)!=GetCurrentTask());
    PostMessage(child,WM_CLOSE,0,0);
    start=GetTickCount();
    while(IsWindow(child) && GetTickCount()-start<10000) if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) DispatchMessage(&m);
    CHECK(!IsWindow(child));
}
static LRESULT CALLBACK MdiChildProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefMDIChildProc(h,msg,wp,lp);}
static HWND mdi_client;
static LRESULT CALLBACK MdiFrameProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {return DefFrameProc(h,mdi_client,msg,wp,lp);}
static void check_mdi(void) {
    WNDCLASS wc; HWND frame,a,b,c; HMENU menu=CreateMenu(),window=CreatePopupMenu(); CLIENTCREATESTRUCT ccs; MDICREATESTRUCT mcs; char text[64]; BOOL max;
    memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=MdiFrameProc; wc.hInstance=instance; wc.lpszClassName="TestFrame"; wc.hbrBackground=(HBRUSH)(COLOR_APPWORKSPACE+1);
    RegisterClass(&wc);
    wc.lpfnWndProc=MdiChildProc; wc.lpszClassName="TestChild"; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);
    AppendMenu(window,MF_STRING,50,"&Cascade"); AppendMenu(menu,MF_POPUP,(UINT_PTR)window,"&Window");
    frame=CreateWindow("TestFrame","MDI",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,500,400,NULL,menu,instance,NULL);
    ccs.hWindowMenu=window; ccs.idFirstChild=1000;
    mdi_client=CreateWindow("MDICLIENT",NULL,WS_CHILD|WS_CLIPCHILDREN|WS_VISIBLE,0,0,0,0,frame,(HMENU)1,instance,&ccs);
    CHECK(frame && mdi_client);
    ShowWindow(frame,SW_SHOW);
    SendMessage(frame,WM_SIZE,0,0);
    {RECT r,f; GetClientRect(mdi_client,&r); GetClientRect(frame,&f); CHECK(r.right==f.right && r.bottom==f.bottom);}
    memset(&mcs,0,sizeof(mcs)); mcs.szClass="TestChild"; mcs.hOwner=instance; mcs.x=mcs.y=mcs.cx=mcs.cy=CW_USEDEFAULT;
    mcs.szTitle="One"; a=(HWND)SendMessage(mdi_client,WM_MDICREATE,0,(LPARAM)&mcs);
    mcs.szTitle="Two"; b=(HWND)SendMessage(mdi_client,WM_MDICREATE,0,(LPARAM)&mcs);
    mcs.szTitle="Three"; c=(HWND)SendMessage(mdi_client,WM_MDICREATE,0,(LPARAM)&mcs);
    CHECK(a && b && c && GetParent(a)==mdi_client && GetDlgCtrlID(a)==1000 && GetDlgCtrlID(c)==1002);
    CHECK((HWND)SendMessage(mdi_client,WM_MDIGETACTIVE,0,(LPARAM)&max)==c && !max);
    CHECK(GetMenuItemCount(window)==5 && GetMenuItemID(window,4)==1002 && (GetMenuState(window,1002,MF_BYCOMMAND)&MF_CHECKED));
    SendMessage(mdi_client,WM_MDIACTIVATE,(WPARAM)a,0); CHECK((HWND)SendMessage(mdi_client,WM_MDIGETACTIVE,0,0)==a);
    SendMessage(mdi_client,WM_MDINEXT,0,0); CHECK((HWND)SendMessage(mdi_client,WM_MDIGETACTIVE,0,0)!=a);
    SendMessage(mdi_client,WM_MDICASCADE,0,0);
    SendMessage(mdi_client,WM_MDITILE,0,0);
    {RECT ra,rb; GetWindowRect(a,&ra); GetWindowRect(b,&rb); CHECK(!(ra.left==rb.left && ra.top==rb.top));}
    SendMessage(mdi_client,WM_MDIMAXIMIZE,(WPARAM)b,0);
    CHECK(IsZoomed(b) && (HWND)SendMessage(mdi_client,WM_MDIGETACTIVE,0,(LPARAM)&max)==b && max);
    GetWindowText(frame,text,sizeof(text)); CHECK(!lstrcmp(text,"MDI - [Two]"));
    CHECK(GetMenuItemCount(menu)==3);
    /* Another child activated is maximized in its place, and the one before
     * gets its own size back. */
    {
        RECT before,after;
        GetWindowRect(a,&before);
        SendMessage(mdi_client,WM_MDIACTIVATE,(WPARAM)a,0); CHECK(IsZoomed(a) && !IsZoomed(b));
        SendMessage(mdi_client,WM_MDIACTIVATE,(WPARAM)b,0); GetWindowRect(a,&after);
        CHECK(IsZoomed(b) && !IsZoomed(a) && EqualRect(&before,&after));
    }
    SendMessage(mdi_client,WM_MDIRESTORE,(WPARAM)b,0);
    CHECK(!IsZoomed(b)); GetWindowText(frame,text,sizeof(text)); CHECK(!lstrcmp(text,"MDI") && GetMenuItemCount(menu)==1);
    SendMessage(mdi_client,WM_MDIDESTROY,(WPARAM)b,0);
    CHECK(!IsWindow(b) && GetDlgCtrlID(c)==1001 && GetMenuItemCount(window)==4);
    DestroyWindow(frame); CHECK(!IsWindow(a) && !IsWindow(mdi_client));
}

/* --- hooks ------------------------------------------------------------------------------ */
static HHOOK hk_get,hk_key,hk_cwp,hk_cbt,hk_filter,hk_play;
static BOOL key_skipped,played_all; static HWND cwp_seen,cbt_destroyed; static int cbt_vetoed;
static HOOKPROC old_next;
static const EVENTMSG *play_events; static int play_count,play_at;
static LRESULT CALLBACK GetMsgHook(int code,WPARAM wp,LPARAM lp) {
    MSG *m=(MSG *)lp;
    if(code>=0 && m->message==WM_USER+20 && m->wParam==1) m->wParam=2;
    return CallNextHookEx(hk_get,code,wp,lp);
}
static LRESULT CALLBACK OldGetMsgHook(int code,WPARAM wp,LPARAM lp) {
    MSG *m=(MSG *)lp;
    if(code>=0 && m->message==WM_USER+20) m->wParam=5;
    return DefHookProc(code,wp,lp,&old_next);
}
static LRESULT CALLBACK KeyHook(int code,WPARAM wp,LPARAM lp) {
    if(code>=0 && wp==VK_F9) {key_skipped=TRUE; return 1;}
    return CallNextHookEx(hk_key,code,wp,lp);
}
static LRESULT CALLBACK CallWndHook(int code,WPARAM wp,LPARAM lp) {
    CWPSTRUCT *c=(CWPSTRUCT *)lp;
    if(code>=0 && c->message==WM_USER+7 && c->wParam==3 && c->lParam==4) cwp_seen=c->hwnd;
    return CallNextHookEx(hk_cwp,code,wp,lp);
}
static LRESULT CALLBACK CbtHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        CREATESTRUCT *cs=((CBT_CREATEWND *)lp)->lpcs;
        if(cs->lpszName && !lstrcmp(cs->lpszName,"veto")) {cbt_vetoed++; return 1;}
        if(cs->lpszName && !lstrcmp(cs->lpszName,"moved")) cs->x=33;
    }
    if(code==HCBT_DESTROYWND) cbt_destroyed=(HWND)wp;
    return CallNextHookEx(hk_cbt,code,wp,lp);
}
static LRESULT CALLBACK FilterHook(int code,WPARAM wp,LPARAM lp) {
    MSG *m=(MSG *)lp;
    if(code==MSGF_DIALOGBOX && m->message==WM_USER+22) {EndDialog(m->hwnd,42); return 1;}
    return CallNextHookEx(hk_filter,code,wp,lp);
}
static INT_PTR CALLBACK PostingProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)wp; (void)lp;
    if(msg==WM_INITDIALOG) {PostMessage(h,WM_USER+22,0,0); return TRUE;}
    return FALSE;
}
/* Journal playback of play_events, each after the time since the one before
 * (at most 100 ms); the hook goes when they are played. */
static LRESULT CALLBACK PlayHook(int code,WPARAM wp,LPARAM lp) {
    (void)wp;
    if(code==HC_GETNEXT) {
        *(EVENTMSG *)lp=play_events[play_at];
        return play_at?min(100,(LRESULT)(play_events[play_at].time-play_events[play_at-1].time)):0;
    }
    if(code==HC_SKIP && ++play_at==play_count) {UnhookWindowsHookEx(hk_play); played_all=TRUE;}
    return 0;
}
static void play(const EVENTMSG *events,int count) {
    MSG m;
    play_events=events; play_count=count; play_at=0; played_all=FALSE;
    hk_play=SetWindowsHookEx(WH_JOURNALPLAYBACK,PlayHook,instance,0);
    pump_until(&played_all,5000);
    while(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);}
}
static void check_hooks(HWND h) {
    static const EVENTMSG keys[6]={{WM_KEYDOWN,VK_SHIFT,1,0},{WM_KEYDOWN,'H',1,0},{WM_KEYUP,'H',1,0},
                                   {WM_KEYUP,VK_SHIFT,1,0},{WM_KEYDOWN,'I',1,20},{WM_KEYUP,'I',1,20}};
    DWORD task=(DWORD)(ULONG_PTR)GetCurrentTask(); MSG m; HWND w; char text[16];
    hk_get=SetWindowsHookEx(WH_GETMESSAGE,GetMsgHook,instance,task);
    hk_key=SetWindowsHookEx(WH_KEYBOARD,KeyHook,instance,task);
    hk_cwp=SetWindowsHookEx(WH_CALLWNDPROC,CallWndHook,instance,task);
    hk_cbt=SetWindowsHookEx(WH_CBT,CbtHook,instance,task);
    hk_filter=SetWindowsHookEx(WH_MSGFILTER,FilterHook,instance,task);
    CHECK(hk_get && hk_key && hk_cwp && hk_cbt && hk_filter);
    PostMessage(h,WM_USER+20,1,0); CHECK(PeekMessage(&m,h,WM_USER+20,WM_USER+20,PM_REMOVE) && m.wParam==2);
    PostMessage(h,WM_KEYDOWN,VK_F9,0); CHECK(!PeekMessage(&m,h,WM_KEYDOWN,WM_KEYDOWN,PM_REMOVE) && key_skipped);
    CHECK(SendMessage(h,WM_USER+7,3,4)==77 && cwp_seen==h);
    CHECK(!CreateWindow("UserTestCheck","veto",WS_POPUP,0,0,50,50,NULL,NULL,instance,NULL) && cbt_vetoed==1);
    w=CreateWindow("UserTestCheck","moved",WS_POPUP,10,10,50,50,NULL,NULL,instance,NULL);
    {RECT r; GetWindowRect(w,&r); CHECK(w && r.left==33);}
    DestroyWindow(w); CHECK(cbt_destroyed==w);
    CHECK(DialogBox(instance,"TESTDLG",h,PostingProc)==42);
    CHECK(UnhookWindowsHookEx(hk_get) && UnhookWindowsHookEx(hk_key) && UnhookWindowsHookEx(hk_cwp) &&
          UnhookWindowsHookEx(hk_cbt) && UnhookWindowsHookEx(hk_filter) && !UnhookWindowsHookEx(hk_get));
    PostMessage(h,WM_USER+20,1,0); CHECK(PeekMessage(&m,h,WM_USER+20,WM_USER+20,PM_REMOVE) && m.wParam==1);
    /* Windows 3.0's interface. */
    old_next=SetWindowsHook(WH_GETMESSAGE,OldGetMsgHook); CHECK(old_next!=NULL);
    PostMessage(h,WM_USER+20,1,0); CHECK(PeekMessage(&m,h,WM_USER+20,WM_USER+20,PM_REMOVE) && m.wParam==5);
    CHECK(UnhookWindowsHook(WH_GETMESSAGE,OldGetMsgHook));
    /* Keys played into an edit control, Shift held for the first. */
    w=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,0,0,100,24,h,NULL,instance,NULL);
    SetFocus(w);
    play(keys,6);
    CHECK(played_all && GetWindowText(w,text,sizeof(text))==2 && !lstrcmp(text,"Hi"));
    DestroyWindow(w);
}
/* Ctrl+J records keys up to Enter, then plays them into an edit control. */
static EVENTMSG recorded[64]; static int recorded_count; static HHOOK hk_record;
static LRESULT CALLBACK RecordHook(int code,WPARAM wp,LPARAM lp) {
    EVENTMSG *e=(EVENTMSG *)lp;
    (void)wp;
    if(code!=HC_ACTION || !hk_record) return 0;
    if(e->message==WM_KEYDOWN && (e->paramL&0xff)==VK_RETURN) {
        UnhookWindowsHookEx(hk_record); hk_record=NULL;
        PostMessage(main_wnd,WM_COMMAND,IDM_PLAY,0);
    } else if(recorded_count<64) recorded[recorded_count++]=*e;
    return 0;
}
static void play_recorded(HWND h) {
    HWND w=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,10,10,200,24,h,NULL,instance,NULL); char text[64],line[96];
    SetFocus(w);
    if(recorded_count) play(recorded,recorded_count);
    GetWindowText(w,text,sizeof(text));
    wsprintf(line,"USERTEST: journal %d events, played %s",recorded_count,text);
    OutputDebugString(line);
    DestroyWindow(w);
}

/* --- the interactive window ---------------------------------------------------------- */
static INT_PTR CALLBACK DialogProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SendDlgItemMessage(h,IDC_LIST,LB_ADDSTRING,0,(LPARAM)"alpha");
        SendDlgItemMessage(h,IDC_COMBO,CB_ADDSTRING,0,(LPARAM)"first");
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL) {
            char text[64],line[128];
            GetDlgItemText(h,IDC_EDIT,text,sizeof(text));
            wsprintf(line,"USERTEST: dialog %d %s %s",LOWORD(wp),text,IsDlgButtonChecked(h,IDC_CHECK)?"checked":"unchecked");
            OutputDebugString(line);
            EndDialog(h,LOWORD(wp));
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
/* An owner-drawn popup: two items the window measures and draws (red and
 * blue, green when selected) and, in a column after a bar, a magenta bitmap. */
static HBITMAP menu_bitmap;
static void owner_menu(HWND h) {
    HMENU popup=CreatePopupMenu(); HDC dc=GetDC(h),mem=CreateCompatibleDC(dc); HGDIOBJ old; RECT r; HBRUSH b=CreateSolidBrush(RGB(255,0,255));
    menu_bitmap=CreateCompatibleBitmap(dc,24,16);
    old=SelectObject(mem,menu_bitmap); SetRect(&r,0,0,24,16); FillRect(mem,&r,b); SelectObject(mem,old);
    DeleteObject(b); DeleteDC(mem); ReleaseDC(h,dc);
    AppendMenu(popup,MF_OWNERDRAW,IDM_OWNER1,(LPCSTR)(ULONG_PTR)RGB(255,0,0));
    AppendMenu(popup,MF_OWNERDRAW,IDM_OWNER2,(LPCSTR)(ULONG_PTR)RGB(0,0,255));
    AppendMenu(popup,MF_BITMAP|MF_MENUBARBREAK,IDM_BITMAP,(LPCSTR)menu_bitmap);
    AppendMenu(GetMenu(h),MF_POPUP,(UINT_PTR)popup,"&Owner");
    DrawMenuBar(h);
}
static LRESULT CALLBACK MainProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_COMMAND:
        report("USERTEST: command %s%d","",LOWORD(wp));
        switch(LOWORD(wp)) {
        case IDM_DIALOG: DialogBox(instance,"TESTDLG",h,DialogProc); return 0;
        case IDM_MESSAGE: report("USERTEST: message box %s%d","",MessageBox(h,"Is this a question?","Question",MB_YESNO|MB_ICONQUESTION)); return 0;
        case IDM_ABOUT: MessageBox(h,"USER test program","About",MB_OK); return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        case IDM_JOURNAL:
            recorded_count=0; hk_record=SetWindowsHookEx(WH_JOURNALRECORD,RecordHook,instance,0);
            OutputDebugString(hk_record?"USERTEST: recording":"USERTEST: no journal");
            return 0;
        case IDM_PLAY: play_recorded(h); return 0;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
        DrawText(dc,"USER test",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mi=(LPMEASUREITEMSTRUCT)lp;
        if(mi->CtlType!=ODT_MENU) break;
        mi->itemWidth=60; mi->itemHeight=20;
        return TRUE;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT di=(LPDRAWITEMSTRUCT)lp; HBRUSH b;
        if(di->CtlType!=ODT_MENU) break;
        b=CreateSolidBrush(di->itemState&ODS_SELECTED?RGB(0,255,0):(COLORREF)di->itemData); FillRect(di->hDC,&di->rcItem,b); DeleteObject(b);
        return TRUE;
    }
    case WM_DESTROY: DeleteObject(menu_bitmap); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* Painting: a window invalidated while it paints paints once more, and a
 * new title leaves its client area alone; siblings that overlap are drawn
 * over by the one painted last unless it has WS_CLIPSIBLINGS. */
static int paints;
static LRESULT CALLBACK AgainProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps; BeginPaint(h,&ps);
        if(++paints==1) InvalidateRect(h,NULL,FALSE);
        EndPaint(h,&ps);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
static LRESULT CALLBACK BoxProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps; RECT r; BeginPaint(h,&ps);
        GetClientRect(h,&r); FillRect(ps.hdc,&r,(HBRUSH)GetStockObject(GetWindowLong(h,0)?WHITE_BRUSH:BLACK_BRUSH));
        EndPaint(h,&ps);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
static void settle(DWORD ms) {MSG m; DWORD start=GetTickCount(); while(GetTickCount()-start<ms) if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) DispatchMessage(&m);}
static HWND box(HWND parent,int x,int y,BOOL white,DWORD style) {
    HWND h=CreateWindow("UserTestBox","",WS_CHILD|WS_VISIBLE|style,x,y,40,20,parent,NULL,instance,NULL);
    SetWindowLong(h,0,white); return h;
}
static void check_painting(void) {
    WNDCLASS wc; HWND h,a,c; HDC dc;
    memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=AgainProc; wc.hInstance=instance; wc.lpszClassName="UserTestAgain"; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);
    wc.lpfnWndProc=BoxProc; wc.lpszClassName="UserTestBox"; wc.cbWndExtra=sizeof(LONG); wc.hbrBackground=NULL;
    RegisterClass(&wc);
    h=CreateWindow("UserTestAgain","Again",WS_POPUP|WS_CAPTION|WS_VISIBLE,20,240,120,80,NULL,NULL,instance,NULL);
    settle(500);
    CHECK(paints==2);
    SetWindowText(h,"Titled");
    CHECK(!GetUpdateRect(h,NULL,FALSE));
    settle(300);
    CHECK(paints==2);
    /* Black boxes made first, so above white ones that overlap their right halves. */
    a=box(h,0,0,FALSE,0); box(h,20,0,TRUE,0);
    c=box(h,0,24,FALSE,0); box(h,20,24,TRUE,WS_CLIPSIBLINGS);
    UpdateWindow(h);
    dc=GetDC(a); CHECK(GetPixel(dc,30,10)==RGB(255,255,255)); CHECK(GetPixel(dc,10,10)==RGB(0,0,0)); ReleaseDC(a,dc);
    dc=GetDC(c); CHECK(GetPixel(dc,30,10)==RGB(0,0,0)); ReleaseDC(c,dc);
    DestroyWindow(h);
}

int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; MSG m; HWND checker; char text[64];
    (void)previous;
    instance=inst;
    if(command && !lstrcmpi(command,"child")) return child_main();
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=CheckProc; wc.hInstance=inst; wc.lpszClassName="UserTestCheck"; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);
    checker=CreateWindow("UserTestCheck","Checks",WS_OVERLAPPEDWINDOW|WS_VSCROLL,300,200,240,160,NULL,NULL,inst,NULL);
    ShowWindow(checker,SW_SHOW);
    OutputDebugString("USERTEST: resources"); check_resources();
    OutputDebugString("USERTEST: dialog"); check_dialog();
    OutputDebugString("USERTEST: windows"); check_windows(checker);
    OutputDebugString("USERTEST: timer"); check_timer(checker);
    OutputDebugString("USERTEST: clipboard"); check_clipboard(checker);
    OutputDebugString("USERTEST: kernel"); check_kernel();
    OutputDebugString("USERTEST: tasks"); check_tasks();
    OutputDebugString("USERTEST: mdi"); check_mdi();
    OutputDebugString("USERTEST: hooks"); check_hooks(checker);
    OutputDebugString("USERTEST: painting"); check_painting();
    DestroyWindow(checker);
    wsprintf(text,failed?"USERTEST: %d failed, %d passed":"USERTEST: %d checks passed",failed?failed:passed,passed);
    OutputDebugString(text);

    wc.lpfnWndProc=MainProc; wc.lpszClassName="UserTest"; wc.lpszMenuName="MAINMENU"; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClass(&wc);
    accel=LoadAccelerators(inst,"MAINACCEL");
    main_wnd=CreateWindow("UserTest","User Test",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,inst,NULL);
    owner_menu(main_wnd);
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    OutputDebugString("USERTEST: ready");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
