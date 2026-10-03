/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows Setup saves resolution in SYSTEM.INI [display] for the next
 * WIN.COM startup; keyboard/mouse/network have fixed choices. Application
 * search scans selected drives outside WINDOWS, classifies NE/PE as Windows
 * programs and others as DOS, then adds selected entries through Program
 * Manager DDE to Applications/DOS Applications groups.
 */
#include <windows.h>
#include <dde.h>
#include "winapp.h"
#include "setup.h"
#define APPS 256

static HINSTANCE instance;
static HWND main_wnd;
static const char *const resolutions[]={"640 x 480","800 x 600","1024 x 768","1280 x 1024"};
#define RESOLUTIONS (int)(sizeof(resolutions)/sizeof(resolutions[0]))
static const char keyboard[]="Enhanced 101 or 102 key US and Non US keyboards";
static const char mouse[]="PS/2 or firmware pointing device";
static const char network[]="No Network Installed";

/* The resolution as SYSTEM.INI has it ("1024x768"), shown as "1024 x 768". */
static int resolution(void) {
    char value[32],shown[32]; int i,k=0;
    GetPrivateProfileString("display","resolution","800x600",value,sizeof(value),"SYSTEM.INI");
    for(i=0;value[i] && k<28;i++) {
        if(value[i]=='x' || value[i]=='X') {shown[k++]=' '; shown[k++]='x'; shown[k++]=' ';}
        else if(value[i]!=' ') shown[k++]=value[i];
    }
    shown[k]=0;
    for(i=0;i<RESOLUTIONS;i++) if(!lstrcmp(shown,resolutions[i])) return i;
    return 1;
}

/* --- Change System Settings ---------------------------------------------------------- */
static void one_choice(HWND dlg,int id,LPCSTR text) {SendDlgItemMessage(dlg,id,CB_ADDSTRING,0,(LPARAM)text); SendDlgItemMessage(dlg,id,CB_SETCURSEL,0,0);}
static INT_PTR CALLBACK SettingsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int i;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        for(i=0;i<RESOLUTIONS;i++) SendDlgItemMessage(dlg,IDC_DISPLAY,CB_ADDSTRING,0,(LPARAM)resolutions[i]);
        SendDlgItemMessage(dlg,IDC_DISPLAY,CB_SETCURSEL,(WPARAM)resolution(),0);
        one_choice(dlg,IDC_KEYBOARD,keyboard); one_choice(dlg,IDC_MOUSE,mouse); one_choice(dlg,IDC_NETWORK,network);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        i=(int)SendDlgItemMessage(dlg,IDC_DISPLAY,CB_GETCURSEL,0,0);
        if(i>=0 && i!=resolution()) {
            char value[16],*p; lstrcpy(value,resolutions[i]);
            /* "1024 x 768" as "1024x768" */
            for(p=value;*p;p++) if(*p==' ') {lstrcpy(p,p+1); p--;}
            WritePrivateProfileString("display","resolution",value,"SYSTEM.INI");
            MessageBox(dlg,"The new display settings take effect the next time you start Interface Manager.","Setup",MB_OK|MB_ICONASTERISK);
            InvalidateRect(main_wnd,NULL,TRUE);
        }
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}

/* --- finding applications ------------------------------------------------------------------ */
typedef struct {char path[MAX_PATH],name[40]; BOOL windows;} App;
static App *apps; static int app_count;
static char drives[26]; static int drive_count;
static HWND searching; /* the dialog that shows where the search is */
/* A Windows program's description (an NE module's first nonresident name), or its file's name.
 * A PE file is a Windows program for the Windows GUI subsystem (2); DOS programs
 * here are EFI applications and drivers, PE files too. */
static void describe(App *a) {
    HFILE h=_lopen(a->path,OF_READ); BYTE header[64],ne[96]; LPCSTR base=FileTitle(a->path); int i; UINT got;
    a->windows=FALSE;
    for(i=0;base[i] && base[i]!='.' && i<39;i++) a->name[i]=(char)(i?(base[i]>='A' && base[i]<='Z'?base[i]+0x20:base[i]):base[i]);
    a->name[i]=0;
    if(h==HFILE_ERROR) return;
    if(_lread(h,header,sizeof(header))==sizeof(header) && header[0]=='M' && header[1]=='Z') {
        LONG at=(LONG)(header[0x3c]|header[0x3d]<<8|(DWORD)header[0x3e]<<16|(DWORD)header[0x3f]<<24);
        if(at>0 && _llseek(h,at,0)==at && (got=_lread(h,ne,sizeof(ne)))!=HFILE_ERROR && got>=64) {
            if(ne[0]=='P' && ne[1]=='E' && !ne[2] && !ne[3]) a->windows=got==sizeof(ne) && (ne[24+68]|ne[24+69]<<8)==2;
            else if(ne[0]=='N' && ne[1]=='E') {
                LONG table=(LONG)(ne[0x2c]|ne[0x2d]<<8|(DWORD)ne[0x2e]<<16|(DWORD)ne[0x2f]<<24); BYTE n; char text[64];
                a->windows=TRUE;
                if(table>0 && _llseek(h,table,0)==table && _lread(h,&n,1)==1 && n && n<40 && _lread(h,text,n)==n) {
                    text[n]=0; lstrcpy(a->name,text);
                }
            }
        }
    }
    _lclose(h);
}
static BOOL is_program(LPCSTR name) {
    int n=lstrlen(name);
    return n>4 && (!lstrcmpi(name+n-4,".EXE") || !lstrcmpi(name+n-4,".COM"));
}
/* A directory's programs and, below it, its subdirectories' (not Windows' own). */
static void search(LPCSTR dir,int depth) {
    char pattern[MAX_PATH],path[MAX_PATH],windir[MAX_PATH]; WIN32_FIND_DATA f; HANDLE h;
    if(depth>8 || app_count>=APPS) return;
    GetWindowsDirectory(windir,sizeof(windir));
    if(!lstrcmpi(dir,windir)) return;
    wsprintf(pattern,"%s\\*.*",dir);
    if(searching) SetDlgItemText(searching,IDC_SEARCHING,pattern);
    if((h=FindFirstFile(pattern,&f))==INVALID_HANDLE_VALUE) return;
    do {
        if(f.cFileName[0]=='.') continue;
        wsprintf(path,"%s\\%s",dir,(LPCSTR)f.cFileName);
        if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) search(path,depth+1);
        else if(is_program(f.cFileName) && app_count<APPS) {lstrcpy(apps[app_count].path,path); describe(&apps[app_count]); app_count++;}
    } while(FindNextFile(h,&f));
    FindClose(h);
}
static INT_PTR CALLBACK SearchProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int i; char name[8];
    (void)lp;
    if(msg==WM_INITDIALOG) {
        DWORD all=GetLogicalDrives();
        for(i=0;i<26;i++) if((all>>i&1) && GetDriveType(i)==DRIVE_FIXED) {
            wsprintf(name,"%c:",'A'+i); SendDlgItemMessage(dlg,IDC_DRIVES,LB_ADDSTRING,0,(LPARAM)name);
        }
        SendDlgItemMessage(dlg,IDC_DRIVES,LB_SETSEL,TRUE,-1);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        int n=(int)SendDlgItemMessage(dlg,IDC_DRIVES,LB_GETCOUNT,0,0);
        drive_count=0;
        for(i=0;i<n;i++) if(SendDlgItemMessage(dlg,IDC_DRIVES,LB_GETSEL,(WPARAM)i,0)>0) {
            SendDlgItemMessage(dlg,IDC_DRIVES,LB_GETTEXT,(WPARAM)i,(LPARAM)name); drives[drive_count++]=name[0];
        }
        EndDialog(dlg,drive_count?IDOK:IDCANCEL); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}

/* --- Program Manager's DDE commands ------------------------------------------------------------- */
static HWND server; static BOOL acked,accepted;
static BOOL execute(LPCSTR commands) {
    HWND pm=FindWindow("Progman",NULL); ATOM app,topic; HGLOBAL h; LPSTR p; DWORD start; MSG m;
    if(!pm) return FALSE;
    app=GlobalAddAtom("PROGMAN"); topic=GlobalAddAtom("PROGMAN"); server=NULL;
    SendMessage(pm,WM_DDE_INITIATE,(WPARAM)main_wnd,MAKELPARAM(app,topic));
    GlobalDeleteAtom(app); GlobalDeleteAtom(topic);
    if(!server) return FALSE;
    if(!(h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)lstrlen(commands)+1)) || !(p=(LPSTR)GlobalLock(h))) return FALSE;
    lstrcpy(p,commands); GlobalUnlock(h);
    acked=FALSE;
    if(!PostMessage(server,WM_DDE_EXECUTE,(WPARAM)main_wnd,(LPARAM)h)) {GlobalFree(h); return FALSE;}
    accepted=FALSE;
    /* Adding many items takes Program Manager a while on a slow machine. */
    for(start=GetTickCount();!acked && GetTickCount()-start<60000 && IsWindow(server);)
        if(PeekMessage(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessage(&m);} else WaitMessage();
    PostMessage(server,WM_DDE_TERMINATE,(WPARAM)main_wnd,0);
    return acked && accepted;
}
/* Each chosen program as an item of its group. */
static void set_up(const int *chosen,int n) {
    char *commands; int i,pass,used=0,size=n*(MAX_PATH+64)+256;
    if(!n || !(commands=(char *)GlobalAlloc(GPTR,(DWORD)size))) return;
    for(pass=0;pass<2;pass++) {
        BOOL any=FALSE;
        for(i=0;i<n;i++) if(apps[chosen[i]].windows==(pass==0)) any=TRUE;
        if(!any) continue;
        used+=wsprintf(commands+used,"[CreateGroup(%s)]",(LPCSTR)(pass==0?"Applications":"DOS Applications"));
        for(i=0;i<n;i++) if(apps[chosen[i]].windows==(pass==0))
            used+=wsprintf(commands+used,"[AddItem(%s,%s)]",(LPCSTR)apps[chosen[i]].path,(LPCSTR)apps[chosen[i]].name);
    }
    if(!execute(commands)) MessageBox(main_wnd,"Program Manager did not take the applications.","Setup",MB_OK|MB_ICONEXCLAMATION);
    GlobalFree(commands);
}
/* Each list line is "name (path)", with the program's index as its data. */
static void list_add(HWND list,int i) {
    char line[MAX_PATH+48]; LRESULT at;
    wsprintf(line,"%s (%s)",(LPCSTR)apps[i].name,(LPCSTR)apps[i].path);
    at=SendMessage(list,LB_ADDSTRING,0,(LPARAM)line); SendMessage(list,LB_SETITEMDATA,(WPARAM)at,(LPARAM)i);
}
/* The selected (or all) lines moved from one list to the other. */
static void move_lines(HWND from,HWND to,BOOL all) {
    int i,n=(int)SendMessage(from,LB_GETCOUNT,0,0);
    for(i=n-1;i>=0;i--) if(all || SendMessage(from,LB_GETSEL,(WPARAM)i,0)>0) {
        list_add(to,(int)SendMessage(from,LB_GETITEMDATA,(WPARAM)i,0)); SendMessage(from,LB_DELETESTRING,(WPARAM)i,0);
    }
}
/* Add and Add All with lines to add, Remove with lines to remove; the focus
 * goes on to OK from a button that is no longer of use. */
static void update_buttons(HWND dlg) {
    BOOL any_found=SendDlgItemMessage(dlg,IDC_FOUND,LB_GETCOUNT,0,0)>0,any_chosen=SendDlgItemMessage(dlg,IDC_CHOSEN,LB_GETCOUNT,0,0)>0;
    HWND focus=GetFocus();
    if((!any_found && (focus==GetDlgItem(dlg,IDC_ADD) || focus==GetDlgItem(dlg,IDC_ADDALL))) || (!any_chosen && focus==GetDlgItem(dlg,IDC_REMOVE)))
        SendMessage(dlg,WM_NEXTDLGCTL,(WPARAM)GetDlgItem(dlg,IDOK),TRUE);
    EnableWindow(GetDlgItem(dlg,IDC_ADD),any_found); EnableWindow(GetDlgItem(dlg,IDC_ADDALL),any_found);
    EnableWindow(GetDlgItem(dlg,IDC_REMOVE),any_chosen);
}
static INT_PTR CALLBACK ApplicationsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    HWND found=GetDlgItem(dlg,IDC_FOUND),chosen=GetDlgItem(dlg,IDC_CHOSEN); int i;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        char root[4]; HCURSOR old=SetCursor(LoadCursor(NULL,IDC_WAIT));
        app_count=0; searching=dlg;
        for(i=0;i<drive_count;i++) {wsprintf(root,"%c:",drives[i]); search(root,0);}
        for(i=0;i<app_count;i++) list_add(found,i);
        SetDlgItemText(dlg,IDC_SEARCHING,""); searching=NULL;
        SetCursor(old); update_buttons(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_ADD: move_lines(found,chosen,FALSE); update_buttons(dlg); return TRUE;
        case IDC_REMOVE: move_lines(chosen,found,FALSE); update_buttons(dlg); return TRUE;
        case IDC_ADDALL: move_lines(found,chosen,TRUE); update_buttons(dlg); return TRUE;
        case IDOK: {
            int n=(int)SendMessage(chosen,LB_GETCOUNT,0,0),*list=(int *)GlobalAlloc(GPTR,(DWORD)(n+1)*sizeof(int));
            if(list) {
                for(i=0;i<n;i++) list[i]=(int)SendMessage(chosen,LB_GETITEMDATA,(WPARAM)i,0);
                EndDialog(dlg,IDOK); set_up(list,n); GlobalFree(list);
            } else EndDialog(dlg,IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* --- the window ----------------------------------------------------------------------------------- */
static void paint(HDC dc) {
    static const char *const labels[4]={"Display:","Keyboard:","Mouse:","Network:"};
    const char *values[4]; TEXTMETRIC tm; int i,line;
    values[0]=resolutions[resolution()]; values[1]=keyboard; values[2]=mouse; values[3]=network;
    SelectObject(dc,GetStockObject(SYSTEM_FONT)); GetTextMetrics(dc,&tm); line=tm.tmHeight+4;
    SetBkMode(dc,TRANSPARENT);
    for(i=0;i<4;i++) {TextOut(dc,12,10+i*line,labels[i],lstrlen(labels[i])); TextOut(dc,100,10+i*line,values[i],lstrlen(values[i]));}
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(dc); EndPaint(h,&ps); return 0;}
    case WM_COMMAND:
        if(HelpCommand(h,LOWORD(wp),"SETUP.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_SETTINGS: DialogBox(instance,"SETTINGS",h,SettingsProc); return 0;
        case IDM_APPLICATIONS:
            if(DialogBox(instance,"SEARCH",h,SearchProc)==IDOK) DialogBox(instance,"APPLICATIONS",h,ApplicationsProc);
            return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        case IDM_ABOUT: MessageBox(h,"Setup\n\nSystem settings and applications.","About Setup",MB_OK|MB_ICONASTERISK); return 0;
        }
        return 0;
    /* The DDE conversation with Program Manager. */
    case WM_DDE_ACK:
        if(!server) {server=(HWND)wp; GlobalDeleteAtom(LOWORD(lp)); GlobalDeleteAtom(HIWORD(lp)); return 0;}
        {
            UINT_PTR status,handle;
            UnpackDDElParam(WM_DDE_ACK,lp,&status,&handle); FreeDDElParam(WM_DDE_ACK,lp);
            if(handle) GlobalFree((HGLOBAL)handle);
            acked=TRUE; accepted=(status&0x8000)!=0;
        }
        return 0;
    case WM_DDE_TERMINATE: return 0;
    case WM_DESTROY: WinHelp(h,"SETUP.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst; (void)command_line;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"SETUP"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="SETUP"; wc.lpszClassName="WindowsSetup";
        if(!RegisterClass(&wc)) return 0;
    }
    if(!(apps=(App *)GlobalAlloc(GPTR,APPS*sizeof(App)))) return 0;
    main_wnd=CreateWindow("WindowsSetup","Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                          CW_USEDEFAULT,0,480,150,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    accel=LoadAccelerators(inst,"SETUP");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    GlobalFree(apps);
    return (int)m.wParam;
}
