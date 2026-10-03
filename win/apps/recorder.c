/* SPDX-License-Identifier: GPL-2.0-or-later
 * Recorder uses journal hooks for macro recording/playback and a global
 * keyboard hook for shortcut keys. Ctrl+Break or an icon click ends recording
 * with save/resume/discard choices. Playback uses recorded speed or runs
 * immediately, once or repeatedly until Ctrl+Break. Mouse recording includes
 * clicks/drags, all moves or none, in window-relative or screen coordinates.
 * .REC files use this program's own format.
 */
#include <windows.h>
#include "winapp.h"
#include "recorder.h"
#define MACROS 32
#define EVENTS 4096
#define WM_RUNKEY (WM_USER+1)
#define WM_STOPPED (WM_USER+2)
#define WM_BACK (WM_USER+3)
/* Macro flags */
#define ANY_APP 1
#define RECORDED_SPEED 2
#define LOOP 4
#define SHORTCUT 8
#define RELATIVE_WINDOW 16
/* Shift keys */
#define CTRL 1
#define SHIFT 2
#define ALT 4
/* How the mouse is recorded */
#define MOUSE_DRAGS 0
#define MOUSE_ALL 1
#define MOUSE_NONE 2

typedef struct {DWORD message,paramL,paramH,time;} Event;
typedef struct {
    char name[40],description[128],app[32]; /* app: the class of the window it was recorded in */
    WORD key; BYTE shifts,flags,mouse; int count; Event *events;
} Macro;
static HINSTANCE instance;
static HWND main_wnd,list;
static Macro macros[MACROS]; static int macro_count;
static char file[260];
static BOOL modified,shortcuts=TRUE,minimize_on_use=TRUE;
static HHOOK key_hook;

/* --- keys ----------------------------------------------------------------------- */
static const struct {WORD vk; const char *name;} special[]={
    {VK_F1,"F1"},{VK_F2,"F2"},{VK_F3,"F3"},{VK_F4,"F4"},{VK_F5,"F5"},{VK_F6,"F6"},{VK_F7,"F7"},{VK_F8,"F8"},
    {VK_F9,"F9"},{VK_F10,"F10"},{VK_F11,"F11"},{VK_F12,"F12"},{VK_INSERT,"Insert"},{VK_DELETE,"Delete"},
    {VK_HOME,"Home"},{VK_END,"End"},{VK_PRIOR,"Page Up"},{VK_NEXT,"Page Down"},{VK_SPACE,"Space"}};
#define SPECIALS (int)(sizeof(special)/sizeof(special[0]))
/* The keys a shortcut can be: none, A to Z, 0 to 9, then the special ones. */
static WORD key_at(int i) {return (WORD)(i==0?0:i<=26?'A'+i-1:i<=36?'0'+i-27:special[i-37].vk);}
static int key_count(void) {return 37+SPECIALS;}
static void key_name(WORD vk,char *out) {
    int i;
    out[0]=0;
    if((vk>='A' && vk<='Z') || (vk>='0' && vk<='9')) {out[0]=(char)vk; out[1]=0; return;}
    for(i=0;i<SPECIALS;i++) if(special[i].vk==vk) lstrcpy(out,special[i].name);
}
static void shortcut_text(const Macro *m,char *out) {
    char key[16];
    out[0]=0;
    if(!m->key) return;
    if(m->shifts&CTRL) lstrcat(out,"Ctrl+");
    if(m->shifts&SHIFT) lstrcat(out,"Shift+");
    if(m->shifts&ALT) lstrcat(out,"Alt+");
    key_name(m->key,key); lstrcat(out,key);
}

/* --- the list and the title ------------------------------------------------------------ */
static void set_title(void) {
    char title[300];
    wsprintf(title,"Recorder - %s",file[0]?FileTitle(file):"(untitled)");
    SetWindowText(main_wnd,title);
}
static void fill_list(int select) {
    int i; char line[96],keys[32];
    SendMessage(list,LB_RESETCONTENT,0,0);
    for(i=0;i<macro_count;i++) {
        shortcut_text(&macros[i],keys);
        wsprintf(line,"%s\t%s",keys,macros[i].name);
        SendMessage(list,LB_ADDSTRING,0,(LPARAM)line);
    }
    if(macro_count) SendMessage(list,LB_SETCURSEL,(WPARAM)(select<macro_count?select:macro_count-1),0);
}
static int selected(void) {LRESULT i=SendMessage(list,LB_GETCURSEL,0,0); return i<0 || i>=macro_count?-1:(int)i;}
static void forget(int i) {
    if(macros[i].events) GlobalFree(macros[i].events);
    for(;i<macro_count-1;i++) macros[i]=macros[i+1];
    macro_count--;
}

/* --- files: a header, then each macro and its events ------------------------------------- */
static const char magic[16]="IM RECORDER 1\r\n";
typedef struct {char name[40],description[128],app[32]; WORD key; BYTE shifts,flags,mouse,reserved[3]; DWORD count;} StoredMacro;
static BOOL save_to(LPCSTR path) {
    DWORD size=sizeof(magic)+sizeof(WORD); int i; BYTE *data,*p; BOOL ok;
    for(i=0;i<macro_count;i++) size+=sizeof(StoredMacro)+(DWORD)macros[i].count*sizeof(Event);
    if(!(data=(BYTE *)GlobalAlloc(GPTR,size))) return FALSE;
    memcpy(data,magic,sizeof(magic)); p=data+sizeof(magic);
    *(WORD *)p=(WORD)macro_count; p+=sizeof(WORD);
    for(i=0;i<macro_count;i++) {
        StoredMacro s; const Macro *m=&macros[i];
        memset(&s,0,sizeof(s));
        lstrcpy(s.name,m->name); lstrcpy(s.description,m->description); lstrcpy(s.app,m->app);
        s.key=m->key; s.shifts=m->shifts; s.flags=m->flags; s.mouse=m->mouse; s.count=(DWORD)m->count;
        memcpy(p,&s,sizeof(s)); p+=sizeof(s);
        memcpy(p,m->events,(size_t)m->count*sizeof(Event)); p+=(size_t)m->count*sizeof(Event);
    }
    ok=WriteWholeFile(path,data,size);
    GlobalFree(data);
    return ok;
}
static BOOL load(LPCSTR path) {
    DWORD size; BYTE *data=(BYTE *)ReadWholeFile(path,&size),*p,*end; int n,i;
    if(!data) return FALSE;
    end=data+size;
    if(size<sizeof(magic)+sizeof(WORD) || memcmp(data,magic,sizeof(magic))) {GlobalFree(data); return FALSE;}
    while(macro_count) forget(macro_count-1);
    p=data+sizeof(magic); n=*(WORD *)p; p+=sizeof(WORD);
    for(i=0;i<n && macro_count<MACROS && p+sizeof(StoredMacro)<=end;i++) {
        StoredMacro s; Macro *m=&macros[macro_count];
        memcpy(&s,p,sizeof(s)); p+=sizeof(s);
        if(s.count>EVENTS || p+s.count*sizeof(Event)>end) break;
        memset(m,0,sizeof(*m));
        s.name[sizeof(s.name)-1]=0; s.description[sizeof(s.description)-1]=0; s.app[sizeof(s.app)-1]=0;
        lstrcpy(m->name,s.name); lstrcpy(m->description,s.description); lstrcpy(m->app,s.app);
        m->key=s.key; m->shifts=s.shifts; m->flags=s.flags; m->mouse=s.mouse; m->count=(int)s.count;
        if(!(m->events=(Event *)GlobalAlloc(GPTR,s.count*sizeof(Event)+1))) break;
        memcpy(m->events,p,s.count*sizeof(Event)); p+=s.count*sizeof(Event);
        macro_count++;
    }
    GlobalFree(data);
    lstrcpy(file,path); AnsiUpper(file); modified=FALSE; set_title(); fill_list(0);
    return TRUE;
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.REC");
    if(!FileSaveDialog(main_wnd,"Save As","*.REC",path,sizeof(path))) return FALSE;
    if(!save_to(path)) {MessageBox(main_wnd,"Cannot write the file.","Recorder",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    lstrcpy(file,path); AnsiUpper(file); modified=FALSE; set_title();
    return TRUE;
}
static BOOL save(void) {
    if(!file[0]) return save_as();
    if(!save_to(file)) {MessageBox(main_wnd,"Cannot write the file.","Recorder",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    modified=FALSE; return TRUE;
}
static BOOL query_save(void) {
    char text[320];
    if(!modified) return TRUE;
    wsprintf(text,"The macros in %s have changed.\n\nDo you want to save the changes?",file[0]?FileTitle(file):"(untitled)");
    switch(MessageBox(main_wnd,text,"Recorder",MB_YESNOCANCEL|MB_ICONEXCLAMATION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}

/* --- recording ------------------------------------------------------------------------------ */
static Macro draft;                     /* the macro being recorded */
static Event *draft_events; static int draft_count;
static HHOOK record_hook; static DWORD record_start; static POINT record_origin; static BOOL record_buttons;
static int record_click; /* where the last click began */
static BOOL stopping;     /* stopped by a click on the icon: the click is not for its menu */
static HWND target;                     /* the window recorded in, active when the Recorder stepped aside */
static BOOL is_mouse(DWORD msg) {return msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST;}
static LRESULT CALLBACK RecordHook(int code,WPARAM wp,LPARAM lp) {
    EVENTMSG *e=(EVENTMSG *)lp; Event *ev;
    if(code!=HC_ACTION || !record_hook) return CallNextHookEx(record_hook,code,wp,lp);
    /* Ctrl+Break ends the recording. */
    if((e->message==WM_KEYDOWN || e->message==WM_SYSKEYDOWN) && (e->paramL&0xff)==VK_CANCEL) {
        UnhookWindowsHookEx(record_hook); record_hook=NULL;
        PostMessage(main_wnd,WM_STOPPED,0,0);
        return 0;
    }
    if(e->message==WM_LBUTTONDOWN) record_click=draft_count;
    if(is_mouse(e->message)) {
        if(e->message==WM_LBUTTONDOWN || e->message==WM_RBUTTONDOWN) record_buttons=TRUE;
        if(e->message==WM_LBUTTONUP || e->message==WM_RBUTTONUP) record_buttons=FALSE;
        if(draft.mouse==MOUSE_NONE) return 0;
        if(draft.mouse==MOUSE_DRAGS && e->message==WM_MOUSEMOVE && !record_buttons) return 0;
    }
    if(draft_count>=EVENTS) return 0;
    if(!draft_count) record_start=e->time;
    ev=&draft_events[draft_count++];
    ev->message=e->message; ev->paramL=e->paramL; ev->paramH=e->paramH; ev->time=e->time-record_start;
    if(is_mouse(e->message) && (draft.flags&RELATIVE_WINDOW)) {ev->paramL-=(DWORD)record_origin.x; ev->paramH-=(DWORD)record_origin.y;}
    return 0;
}
static void start_recording(void) {
    RECT r; HWND top;
    draft_count=0; record_buttons=FALSE; record_click=EVENTS;
    /* Minimized, the Recorder hands over to the window under it. */
    if(minimize_on_use) ShowWindow(main_wnd,SW_MINIMIZE);
    top=GetActiveWindow(); target=top!=main_wnd?top:NULL;
    if(target) {GetWindowRect(top,&r); record_origin.x=r.left; record_origin.y=r.top; GetClassName(top,draft.app,sizeof(draft.app));}
    else {record_origin.x=record_origin.y=0; draft.app[0]=0;}
    record_hook=SetWindowsHookEx(WH_JOURNALRECORD,RecordHook,instance,0);
}
/* The shift keys pressed for Ctrl+Break come last; they go. */
static void drop_trailing_shifts(void) {
    while(draft_count) {
        const Event *e=&draft_events[draft_count-1]; WORD vk=(WORD)(e->paramL&0xff);
        if((e->message==WM_KEYDOWN || e->message==WM_SYSKEYDOWN) && (vk==VK_CONTROL || vk==VK_SHIFT || vk==VK_MENU)) draft_count--;
        else break;
    }
}
/* So does a click on the Recorder's icon, which is not part of the macro. */
static void stop_by_icon(void) {
    if(!record_hook) return;
    UnhookWindowsHookEx(record_hook); record_hook=NULL;
    if(record_click<draft_count) draft_count=record_click;
    stopping=TRUE;
    PostMessage(main_wnd,WM_STOPPED,0,0);
}
static INT_PTR CALLBACK StoppedProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {CheckRadioButton(dlg,IDC_SAVE,IDC_DISCARD,IDC_SAVE); return TRUE;}
    if(msg==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)) {
        int r=IDC_RESUME;
        if(LOWORD(wp)==IDOK) r=IsDlgButtonChecked(dlg,IDC_SAVE)?IDC_SAVE:IsDlgButtonChecked(dlg,IDC_DISCARD)?IDC_DISCARD:IDC_RESUME;
        EndDialog(dlg,r); return TRUE;
    }
    return FALSE;
}
static void stopped(void) {
    int r=(int)DialogBox(instance,"STOPPED",GetActiveWindow(),StoppedProc);
    stopping=FALSE; /* the click on the icon came while the dialog was up */
    if(r==IDC_RESUME) {
        record_click=EVENTS; record_hook=SetWindowsHookEx(WH_JOURNALRECORD,RecordHook,instance,0);
        PostMessage(main_wnd,WM_BACK,0,0);
        return;
    }
    if(r==IDC_SAVE && macro_count<MACROS) {
        drop_trailing_shifts();
        draft.count=draft_count;
        if((draft.events=(Event *)GlobalAlloc(GPTR,(DWORD)draft_count*sizeof(Event)+1))!=NULL) {
            memcpy(draft.events,draft_events,(size_t)draft_count*sizeof(Event));
            macros[macro_count++]=draft; modified=TRUE; fill_list(macro_count-1);
        }
    }
    if(IsIconic(main_wnd) && !minimize_on_use) ShowWindow(main_wnd,SW_RESTORE);
    /* Back to the application, once the dialog's own activations are done. */
    PostMessage(main_wnd,WM_BACK,0,0);
}

/* --- the Record Macro dialog (Properties uses it too) ----------------------------------------- */
static Macro *editing; static BOOL editing_new;
static void add_strings(HWND dlg,int id,const char *const *items,int n,int selected_item) {
    int i;
    for(i=0;i<n;i++) SendDlgItemMessage(dlg,id,CB_ADDSTRING,0,(LPARAM)items[i]);
    SendDlgItemMessage(dlg,id,CB_SETCURSEL,(WPARAM)selected_item,0);
}
static INT_PTR CALLBACK RecordProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    static const char *const to[]={"Same Application","Any Application"},*const speed[]={"Fast","Recorded Speed"};
    static const char *const mouse[]={"Clicks + Drags","Everything","Ignore Mouse"},*const relative[]={"Window","Screen"};
    Macro *m=editing; int i,at=0; char name[16];
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        if(!editing_new) {SetWindowText(dlg,"Macro Properties"); SetDlgItemText(dlg,IDOK,"OK");}
        SetDlgItemText(dlg,IDC_NAME,m->name); SetDlgItemText(dlg,IDC_DESCRIPTION,m->description);
        for(i=0;i<key_count();i++) {
            WORD vk=key_at(i);
            if(vk) key_name(vk,name); else lstrcpy(name,"(none)");
            SendDlgItemMessage(dlg,IDC_KEY,CB_ADDSTRING,0,(LPARAM)name);
            if(vk==m->key) at=i;
        }
        SendDlgItemMessage(dlg,IDC_KEY,CB_SETCURSEL,(WPARAM)at,0);
        CheckDlgButton(dlg,IDC_CTRL,(m->shifts&CTRL)!=0); CheckDlgButton(dlg,IDC_SHIFT,(m->shifts&SHIFT)!=0);
        CheckDlgButton(dlg,IDC_ALT,(m->shifts&ALT)!=0);
        add_strings(dlg,IDC_TO,to,2,(m->flags&ANY_APP)?1:0);
        add_strings(dlg,IDC_SPEED,speed,2,(m->flags&RECORDED_SPEED)?1:0);
        CheckDlgButton(dlg,IDC_LOOP,(m->flags&LOOP)!=0); CheckDlgButton(dlg,IDC_ENABLE,(m->flags&SHORTCUT)!=0);
        add_strings(dlg,IDC_MOUSE,mouse,3,m->mouse);
        add_strings(dlg,IDC_RELATIVE,relative,2,(m->flags&RELATIVE_WINDOW)?0:1);
        if(!editing_new) {EnableWindow(GetDlgItem(dlg,IDC_MOUSE),FALSE); EnableWindow(GetDlgItem(dlg,IDC_RELATIVE),FALSE);}
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            GetDlgItemText(dlg,IDC_NAME,m->name,sizeof(m->name));
            if(!m->name[0]) {MessageBox(dlg,"The macro needs a name or a shortcut key.","Recorder",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
            GetDlgItemText(dlg,IDC_DESCRIPTION,m->description,sizeof(m->description));
            m->key=key_at((int)SendDlgItemMessage(dlg,IDC_KEY,CB_GETCURSEL,0,0));
            m->shifts=(BYTE)((IsDlgButtonChecked(dlg,IDC_CTRL)?CTRL:0)|(IsDlgButtonChecked(dlg,IDC_SHIFT)?SHIFT:0)|(IsDlgButtonChecked(dlg,IDC_ALT)?ALT:0));
            m->flags=(BYTE)((SendDlgItemMessage(dlg,IDC_TO,CB_GETCURSEL,0,0)==1?ANY_APP:0)|
                            (SendDlgItemMessage(dlg,IDC_SPEED,CB_GETCURSEL,0,0)==1?RECORDED_SPEED:0)|
                            (IsDlgButtonChecked(dlg,IDC_LOOP)?LOOP:0)|(IsDlgButtonChecked(dlg,IDC_ENABLE)?SHORTCUT:0)|
                            (SendDlgItemMessage(dlg,IDC_RELATIVE,CB_GETCURSEL,0,0)==0?RELATIVE_WINDOW:0));
            if(editing_new) m->mouse=(BYTE)SendDlgItemMessage(dlg,IDC_MOUSE,CB_GETCURSEL,0,0);
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- playback ---------------------------------------------------------------------------------- */
static const Macro *playing; static int play_at; static POINT play_origin; static HHOOK play_hook;
static LRESULT CALLBACK PlayHook(int code,WPARAM wp,LPARAM lp) {
    const Event *e;
    if(code<0 || !playing) return CallNextHookEx(play_hook,code,wp,lp);
    e=&playing->events[play_at];
    if(code==HC_GETNEXT) {
        EVENTMSG *out=(EVENTMSG *)lp; LONG wait=0;
        out->message=e->message; out->paramL=e->paramL; out->paramH=e->paramH; out->time=GetTickCount(); out->hwnd=NULL;
        if(is_mouse(e->message) && (playing->flags&RELATIVE_WINDOW)) {out->paramL+=(DWORD)play_origin.x; out->paramH+=(DWORD)play_origin.y;}
        if((playing->flags&RECORDED_SPEED) && play_at) wait=(LONG)(e->time-playing->events[play_at-1].time);
        return wait<0?0:wait>10000?10000:wait;
    }
    if(code==HC_SKIP && ++play_at>=playing->count) {
        if(playing->flags&LOOP) play_at=0;
        else {HHOOK h=play_hook; playing=NULL; play_hook=NULL; UnhookWindowsHookEx(h);}
    }
    return 0;
}
/* Plays macro i into the active window (the Recorder steps aside first when
 * it is the active one). */
static void play(int i) {
    Macro *m=&macros[i]; HWND top; RECT r; char app[32];
    if(i<0 || i>=macro_count || !m->count || playing || record_hook) return;
    if(GetActiveWindow()==main_wnd && minimize_on_use) ShowWindow(main_wnd,SW_MINIMIZE);
    top=GetActiveWindow();
    if(!(m->flags&ANY_APP) && m->app[0]) {
        if(!top || !GetClassName(top,app,sizeof(app)) || lstrcmp(app,m->app)) {
            MessageBox(top,"The macro plays back only in the application it was recorded in.","Recorder",MB_OK|MB_ICONEXCLAMATION);
            return;
        }
    }
    play_origin.x=play_origin.y=0;
    if(top && top!=main_wnd) {GetWindowRect(top,&r); play_origin.x=r.left; play_origin.y=r.top;}
    playing=m; play_at=0;
    if(!(play_hook=SetWindowsHookEx(WH_JOURNALPLAYBACK,PlayHook,instance,0))) playing=NULL;
}
/* Shortcut keys, wherever they are typed. */
static LRESULT CALLBACK KeyHook(int code,WPARAM wp,LPARAM lp) {
    int i; BYTE shifts;
    if(code==HC_ACTION && !(lp&0x80000000L) && shortcuts && !playing && !record_hook) {
        shifts=(BYTE)((GetKeyState(VK_CONTROL)<0?CTRL:0)|(GetKeyState(VK_SHIFT)<0?SHIFT:0)|(GetKeyState(VK_MENU)<0?ALT:0));
        for(i=0;i<macro_count;i++)
            if(macros[i].key==(WORD)wp && macros[i].shifts==shifts && (macros[i].flags&SHORTCUT)) {
                PostMessage(main_wnd,WM_RUNKEY,(WPARAM)i,0);
                return 1;
            }
    }
    return CallNextHookEx(key_hook,code,wp,lp);
}

/* --- the main window ---------------------------------------------------------------------------- */
static void command(HWND h,UINT id) {
    char path[260]; int i;
    if(HelpCommand(h,id,"RECORDER.HLP")) return;
    switch(id) {
    case IDM_NEW:
        if(!query_save()) return;
        while(macro_count) forget(macro_count-1);
        file[0]=0; modified=FALSE; set_title(); fill_list(0);
        return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.REC");
        if(FileOpenDialog(h,"Open","*.REC",path,sizeof(path)) && !load(path))
            MessageBox(h,"The file is not a Recorder file.","Recorder",MB_OK|MB_ICONEXCLAMATION);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_RUN: play(selected()); return;
    case IDM_RECORD:
        if(macro_count>=MACROS) {MessageBox(h,"There is no room for another macro.","Recorder",MB_OK|MB_ICONEXCLAMATION); return;}
        memset(&draft,0,sizeof(draft)); draft.flags=SHORTCUT|RELATIVE_WINDOW; draft.mouse=MOUSE_DRAGS;
        editing=&draft; editing_new=TRUE;
        if(DialogBox(instance,"RECORD",h,RecordProc)==IDOK) start_recording();
        return;
    case IDM_DELETE: {
        char text[96];
        if((i=selected())<0) return;
        wsprintf(text,"Delete the macro %s?",(LPCSTR)macros[i].name);
        if(MessageBox(h,text,"Recorder",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;
        forget(i); modified=TRUE; fill_list(i);
        return;
    }
    case IDM_PROPERTIES:
        if((i=selected())<0) return;
        editing=&macros[i]; editing_new=FALSE;
        if(DialogBox(instance,"RECORD",h,RecordProc)==IDOK) {modified=TRUE; fill_list(i);}
        return;
    case IDM_SHORTCUTS: shortcuts=!shortcuts; return;
    case IDM_MINIMIZE: minimize_on_use=!minimize_on_use; return;
    case IDM_ABOUT:
        MessageBox(h,"Recorder\n\nRecords keys and mouse actions and plays them back.","About Recorder",MB_OK|MB_ICONASTERISK);
        return;
    }
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        static const int tabs[1]={60};
        main_wnd=h;
        list=CreateWindow("LISTBOX",NULL,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_USETABSTOPS|LBS_NOINTEGRALHEIGHT,
                          0,0,0,0,h,(HMENU)IDC_LIST,instance,NULL);
        SendMessage(list,LB_SETTABSTOPS,1,(LPARAM)tabs);
        key_hook=SetWindowsHookEx(WH_KEYBOARD,KeyHook,instance,0);
        return 0;
    }
    case WM_SIZE: MoveWindow(list,0,0,LOWORD(lp),HIWORD(lp),TRUE); return 0;
    case WM_SETFOCUS: SetFocus(list); return 0;
    case WM_ACTIVATE:
        if(LOWORD(wp)!=WA_INACTIVE && record_hook && IsIconic(h)) stop_by_icon();
        break;
    case WM_INITMENUPOPUP: {
        HMENU m=(HMENU)wp; UINT any=selected()>=0?MF_ENABLED:MF_GRAYED;
        EnableMenuItem(m,IDM_RUN,any); EnableMenuItem(m,IDM_DELETE,any); EnableMenuItem(m,IDM_PROPERTIES,any);
        CheckMenuItem(m,IDM_SHORTCUTS,shortcuts?MF_CHECKED:MF_UNCHECKED);
        CheckMenuItem(m,IDM_MINIMIZE,minimize_on_use?MF_CHECKED:MF_UNCHECKED);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_LIST) {if(HIWORD(wp)==LBN_DBLCLK) play(selected()); return 0;}
        command(h,LOWORD(wp));
        return 0;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: case WM_NCLBUTTONDBLCLK:
        if(IsIconic(h) && (record_hook || stopping)) {stop_by_icon(); return 0;}
        break;
    case WM_RUNKEY: play((int)wp); return 0;
    case WM_STOPPED: stopped(); return 0;
    case WM_BACK: if(target && IsWindow(target)) SetActiveWindow(target); return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"RECORDER.HLP",HELP_QUIT,0);
        if(key_hook) UnhookWindowsHookEx(key_hook);
        if(record_hook) UnhookWindowsHookEx(record_hook);
        if(play_hook) UnhookWindowsHookEx(play_hook);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"RECORDER"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="RECORDER"; wc.lpszClassName="Recorder";
        if(!RegisterClass(&wc)) return 0;
    }
    if(!(draft_events=(Event *)GlobalAlloc(GPTR,EVENTS*sizeof(Event)))) return 0;
    main_wnd=CreateWindow("Recorder","Recorder",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,360,220,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    set_title();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    while(command_line && *command_line==' ') command_line++;
    if(command_line && *command_line && !load(command_line))
        MessageBox(main_wnd,"The file is not a Recorder file.","Recorder",MB_OK|MB_ICONEXCLAMATION);
    accel=LoadAccelerators(inst,"RECORDER");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    GlobalFree(draft_events);
    return (int)m.wParam;
}
