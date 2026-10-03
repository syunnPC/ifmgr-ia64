/* SPDX-License-Identifier: GPL-2.0-or-later
 * Print Manager drains GDI's TEMP spool files to device ports in chunks
 * per tick, sized by priority. File/port handles belong to its task; serial
 * configuration comes from WIN.INI [ports]. Port failures pause the printer
 * and notify according to the selected alert mode. Printers can pause/resume;
 * documents can be deleted. /spool exits when the queue is empty; /trace
 * reports each document queued and sent through OutputDebugString, for tests.
 */
#include <windows.h>
#include "winapp.h"
#include "printman.h"
#define TIMER 1
#define TICK_MS 100
#define JOBS 64
#define PAUSED_PORTS 8
#define LINES 96

static HINSTANCE instance;
static HWND main_wnd,queue_list,status;
static SPOOLJOB queue[JOBS]; static int queued;
static DWORD sending;                          /* the job being sent, 0 for none */
static HFILE in_file=HFILE_ERROR,out_port=HFILE_ERROR; static DWORD sent;
static char paused[PAUSED_PORTS][64]; static int paused_count;
static BOOL for_spooler,trace,show_time=TRUE,show_size=TRUE;
static int priority=IDM_MEDIUM,alerts=IDM_FLASH;
/* What each line of the list is: a printer (its port) or a job. */
static struct {DWORD job; char port[64];} lines[LINES]; static int line_count;

static BOOL is_paused(LPCSTR port) {int i; for(i=0;i<paused_count;i++) if(!lstrcmpi(paused[i],port)) return TRUE; return FALSE;}
static void set_paused(LPCSTR port,BOOL on) {
    int i;
    for(i=0;i<paused_count;i++) if(!lstrcmpi(paused[i],port)) {if(!on) {lstrcpy(paused[i],paused[--paused_count]);} return;}
    if(on && paused_count<PAUSED_PORTS) lstrcpyn(paused[paused_count++],port,64);
}
static const SPOOLJOB *job_of(DWORD id) {int i; for(i=0;i<queued;i++) if(queue[i].id==id) return &queue[i]; return NULL;}
static void port_name(LPCSTR port,char *out) {
    int n; lstrcpyn(out,port,64); n=lstrlen(out);
    if(n && out[n-1]==':') out[n-1]=0;
}

/* --- the list ----------------------------------------------------------------------- */
static void add_line(LPCSTR text,DWORD job,LPCSTR port) {
    if(line_count==LINES) return;
    SendMessage(queue_list,LB_ADDSTRING,0,(LPARAM)text);
    lines[line_count].job=job; lstrcpyn(lines[line_count].port,port,64); line_count++;
}
static void printer_line(LPCSTR device,LPCSTR port) {
    char line[160],name[64];
    port_name(port,name);
    wsprintf(line,"%s on %s [%s]",device,(LPCSTR)name,(LPCSTR)(is_paused(port)?"Paused":sending && job_of(sending) &&
             !lstrcmpi(job_of(sending)->port,port)?"Printing":"Idle"));
    add_line(line,0,port);
}
static void job_line(const SPOOLJOB *j,int number) {
    char line[200],part[64]; int done=j->id==sending && j->size?(int)((sent*100)/j->size):0;
    wsprintf(line,"    %d  %s",number,(LPCSTR)j->document);
    if(show_size) {
        if(j->id==sending) wsprintf(part,"    %d%% of %luK",done,(j->size+1023)/1024);
        else wsprintf(part,"    %luK",(j->size+1023)/1024);
        lstrcat(line,part);
    }
    if(show_time) {
        int hour=j->sent.wHour%12?j->sent.wHour%12:12;
        wsprintf(part,"    %d:%02d %s %d/%d/%d",hour,j->sent.wMinute,(LPCSTR)(j->sent.wHour<12?"AM":"PM"),
                 j->sent.wMonth,j->sent.wDay,j->sent.wYear%100);
        lstrcat(line,part);
    }
    add_line(line,j->id,j->port);
}
static void show_status(void) {
    char text[160],name[64]; const SPOOLJOB *j=job_of(sending);
    if(j) {port_name(j->port,name); wsprintf(text,"The %s printer is printing %s.",(LPCSTR)name,(LPCSTR)j->document);}
    else if(queued && is_paused(queue[0].port)) {port_name(queue[0].port,name); wsprintf(text,"The %s printer is paused.",(LPCSTR)name);}
    else lstrcpy(text,queued?"Waiting to print.":"No documents are waiting.");
    SetWindowText(status,text);
}
/* The queue from GDI, the list rebuilt with the selection kept. */
static void refresh(void) {
    LRESULT sel=SendMessage(queue_list,LB_GETCURSEL,0,0); DWORD keep=sel>=0 && sel<line_count?lines[sel].job:0;
    char keep_port[64]; int i,k,number;
    keep_port[0]=0;
    if(sel>=0 && sel<line_count) lstrcpy(keep_port,lines[sel].port);
    queued=GdiSpoolJobs(queue,JOBS);
    if(trace) {static DWORD seen; for(i=0;i<queued;i++) if(queue[i].id>seen) {
        char line[200]; wsprintf(line,"PRINTMAN: queued %s for %s",(LPCSTR)queue[i].document,(LPCSTR)queue[i].port); OutputDebugString(line); seen=queue[i].id;}}
    SendMessage(queue_list,WM_SETREDRAW,FALSE,0);
    SendMessage(queue_list,LB_RESETCONTENT,0,0); line_count=0;
    if(!queued) {
        char device[160],dev[CCHDEVICENAME],port[64]; LPCSTR s;
        GetProfileString("windows","device","",device,sizeof(device));
        for(i=0,s=device;*s && *s!=',' && i<CCHDEVICENAME-1;) dev[i++]=*s++;
        dev[i]=0;
        if(*s) {s++; while(*s && *s!=',') s++;}
        if(*s) s++;
        lstrcpyn(port,s,sizeof(port));
        if(dev[0]) printer_line(dev,port[0]?port:"LPT1:");
    }
    /* Each port's printer, then its documents in the order they came. */
    for(i=0;i<queued;i++) {
        BOOL first=TRUE;
        for(k=0;k<i;k++) if(!lstrcmpi(queue[k].port,queue[i].port)) first=FALSE;
        if(!first) continue;
        printer_line(queue[i].device,queue[i].port);
        for(k=i,number=1;k<queued;k++) if(!lstrcmpi(queue[k].port,queue[i].port)) job_line(&queue[k],number++);
    }
    for(i=0;i<line_count;i++) if((keep && lines[i].job==keep) || (!keep && keep_port[0] && !lines[i].job && !lstrcmpi(lines[i].port,keep_port))) break;
    SendMessage(queue_list,LB_SETCURSEL,(WPARAM)(i<line_count?i:0),0);
    SendMessage(queue_list,WM_SETREDRAW,TRUE,0); InvalidateRect(queue_list,NULL,TRUE);
    show_status();
}

/* --- sending --------------------------------------------------------------------------- */
static void close_job(void) {
    if(in_file!=HFILE_ERROR) _lclose(in_file);
    if(out_port!=HFILE_ERROR) _lclose(out_port);
    in_file=out_port=HFILE_ERROR; sending=0; sent=0;
}
/* The job is over: its file deleted and GDI told. */
static void finish_job(DWORD id) {
    const SPOOLJOB *j=job_of(id);
    if(id==sending) close_job();
    if(j) DeleteFile(j->path);
    GdiEndSpoolJob(id);
}
static void port_error(const SPOOLJOB *j) {
    char text[200],name[64];
    port_name(j->port,name);
    set_paused(j->port,TRUE);
    if(alerts==IDM_ALERT || (alerts!=IDM_IGNORE && GetActiveWindow()==main_wnd)) {
        wsprintf(text,"Cannot write to the %s port. The printer may be off line or not selected; its queue is paused.",(LPCSTR)name);
        MessageBox(main_wnd,text,"Print Manager",MB_OK|MB_ICONEXCLAMATION);
    } else if(alerts==IDM_FLASH) FlashWindow(main_wnd,TRUE);
}
/* A serial port is set as WIN.INI [ports] has it ("COM1:=9600,n,8,1,x")
 * before a document goes to it. */
static void set_port(LPCSTR port) {
    char setting[64],line[80]; int id; DCB now,dcb;
    if((port[0]|0x20)!='c' || (port[1]|0x20)!='o' || (port[2]|0x20)!='m') return;
    if(!GetProfileString("ports",port,"",setting,sizeof(setting))) return;
    wsprintf(line,"%s%s",port,(LPSTR)setting);
    if((id=OpenComm(port,256,256))<0) return;
    if(!GetCommState(id,&now) && !BuildCommDCB(line,&dcb)) {dcb.Id=now.Id; SetCommState(&dcb);}
    CloseComm(id);
}
static void tick(void) {
    static const UINT pieces[3]={256,1024,4096}; BYTE buffer[4096]; UINT n; const SPOOLJOB *j; int i;
    if(!sending) {
        for(i=0;i<queued && is_paused(queue[i].port);i++) {}
        if(i==queued) {if(!queued && for_spooler) DestroyWindow(main_wnd); return;}
        j=&queue[i];
        if((in_file=_lopen(j->path,OF_READ))==HFILE_ERROR) {finish_job(j->id); refresh(); return;}
        set_port(j->port);
        if((out_port=_lopen(j->port,OF_WRITE))==HFILE_ERROR) {_lclose(in_file); in_file=HFILE_ERROR; port_error(j); refresh(); return;}
        sending=j->id; sent=0; refresh();
    }
    if(!(j=job_of(sending))) {close_job(); return;}
    if(is_paused(j->port)) return;
    n=_lread(in_file,buffer,pieces[priority-IDM_LOW]);
    if(n==(UINT)HFILE_ERROR || !n) {
        if(!n && trace) {char line[200]; wsprintf(line,"PRINTMAN: sent %s to %s",(LPCSTR)j->document,(LPCSTR)j->port); OutputDebugString(line);}
        finish_job(sending); refresh(); return;
    }
    if(_lwrite(out_port,buffer,n)!=n) {
        /* Not taken: the rest waits, from where it stopped, until the printer is resumed. */
        _llseek(in_file,(LONG)sent,0);
        port_error(j); refresh(); return;
    }
    sent+=n;
    {static DWORD last; if(GetTickCount()-last>500) {last=GetTickCount(); refresh();}}
}

/* --- the window ------------------------------------------------------------------------- */
static int selected(void) {LRESULT i=SendMessage(queue_list,LB_GETCURSEL,0,0); return i>=0 && i<line_count?(int)i:-1;}
static void update_buttons(void) {
    int i=selected();
    EnableWindow(GetDlgItem(main_wnd,IDC_PAUSE),i>=0 && !is_paused(lines[i].port));
    EnableWindow(GetDlgItem(main_wnd,IDC_RESUME),i>=0 && is_paused(lines[i].port));
    EnableWindow(GetDlgItem(main_wnd,IDC_DELETE),i>=0 && lines[i].job!=0);
}
static BOOL query_exit(void) {
    int i;
    if(!queued) return TRUE;
    if(MessageBox(main_wnd,"If you quit Print Manager, the documents waiting to print are deleted. Quit anyway?",
                  "Print Manager",MB_OKCANCEL|MB_ICONEXCLAMATION)!=IDOK) return FALSE;
    for(i=queued-1;i>=0;i--) finish_job(queue[i].id);
    queued=0;
    return TRUE;
}
static void command(HWND h,UINT id) {
    int i=selected(); char text[160];
    if(HelpCommand(h,id,"PRINTMAN.HLP")) return;
    switch(id) {
    case IDC_PAUSE: if(i>=0) {set_paused(lines[i].port,TRUE); refresh();} break;
    case IDC_RESUME: if(i>=0) {set_paused(lines[i].port,FALSE); refresh();} break;
    case IDC_DELETE: {
        const SPOOLJOB *j=i>=0?job_of(lines[i].job):NULL;
        if(!j) break;
        wsprintf(text,"Do you want to cancel printing of %s?",(LPCSTR)j->document);
        if(MessageBox(h,text,"Print Manager",MB_OKCANCEL|MB_ICONQUESTION)==IDOK) {finish_job(j->id); refresh();}
        break;
    }
    case IDM_LOW: case IDM_MEDIUM: case IDM_HIGH: priority=(int)id; break;
    case IDM_ALERT: case IDM_FLASH: case IDM_IGNORE: alerts=(int)id; break;
    case IDM_TIME: show_time=!show_time; refresh(); break;
    case IDM_SIZE: show_size=!show_size; refresh(); break;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); break;
    case IDM_ABOUT: MessageBox(h,"Print Manager\n\nSends spooled documents to their printers.","About Print Manager",MB_OK|MB_ICONASTERISK); break;
    }
    update_buttons();
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        static const char *const names[3]={"&Pause","&Resume","&Delete"}; int i;
        main_wnd=h;
        for(i=0;i<3;i++) CreateWindow("BUTTON",names[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,8+i*80,8,72,24,h,(HMENU)(INT_PTR)(IDC_PAUSE+i),instance,NULL);
        status=CreateWindow("STATIC","",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,256,12,360,18,h,(HMENU)IDC_STATUS,instance,NULL);
        queue_list=CreateWindow("LISTBOX",NULL,WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
                                0,40,0,0,h,(HMENU)IDC_QUEUE,instance,NULL);
        refresh(); update_buttons();
        SetTimer(h,TIMER,TICK_MS,NULL);
        return 0;
    }
    case WM_SIZE: MoveWindow(queue_list,0,40,LOWORD(lp),HIWORD(lp)>40?HIWORD(lp)-40:0,TRUE); return 0;
    case WM_SETFOCUS: SetFocus(queue_list); return 0;
    case WM_SPOOLERSTATUS: refresh(); update_buttons(); return 0;
    case WM_SYSCHAR: {
        /* The buttons' mnemonics, as in a dialog. */
        int id=(wp|0x20)=='p'?IDC_PAUSE:(wp|0x20)=='r'?IDC_RESUME:(wp|0x20)=='d'?IDC_DELETE:0;
        if(id) {if(IsWindowEnabled(GetDlgItem(h,id))) command(h,(UINT)id); else MessageBeep(0); return 0;}
        break;
    }
    case WM_TIMER: tick(); return 0;
    case WM_INITMENUPOPUP: {
        HMENU m=(HMENU)wp; int i;
        for(i=IDM_LOW;i<=IDM_HIGH;i++) CheckMenuItem(m,(UINT)i,i==priority?MF_CHECKED:MF_UNCHECKED);
        for(i=IDM_ALERT;i<=IDM_IGNORE;i++) CheckMenuItem(m,(UINT)i,i==alerts?MF_CHECKED:MF_UNCHECKED);
        CheckMenuItem(m,IDM_TIME,show_time?MF_CHECKED:MF_UNCHECKED); CheckMenuItem(m,IDM_SIZE,show_size?MF_CHECKED:MF_UNCHECKED);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_QUEUE) {if(HIWORD(wp)==LBN_SELCHANGE) update_buttons(); return 0;}
        command(h,LOWORD(wp)); return 0;
    case WM_CLOSE: if(query_exit()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_exit();
    case WM_DESTROY: WinHelp(h,"PRINTMAN.HLP",HELP_QUIT,0); KillTimer(h,TIMER); close_job(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst;
    if(previous) return 0; /* one Print Manager */
    while(command_line && *command_line==' ') command_line++;
    for_spooler=command_line && !lstrcmpi(command_line,"/spool");
    trace=command_line && !lstrcmpi(command_line,"/trace");
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"PRINTMAN"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.lpszMenuName="PRINTMAN"; wc.lpszClassName="PrintManager";
    if(!RegisterClass(&wc)) return 0;
    main_wnd=CreateWindow("PrintManager","Print Manager",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,560,240,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    accel=LoadAccelerators(inst,"PRINTMAN");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
