/* SPDX-License-Identifier: GPL-2.0-or-later
 * Calendar day/month views with daily notes.
 * .CAL text format: "CALENDAR interval start", then "YYYYMMDD HHMM text"
 * appointments and "YYYYMMDD NOTE text" notes; note line breaks use \n.
 * /trace reports the displayed date and appointments through OutputDebugString.
 */
#include <windows.h>
#include "winapp.h"
#include "calendar.h"
#define APPTS 512
#define NOTES 128
#define ID_APPT 10
#define ID_PAD 11
#define ID_SCROLL 12
#define ID_PREV 13
#define ID_NEXT 14

typedef struct {long date; int minute; char text[80];} Appt;
typedef struct {long date; char text[256];} Note;
static HINSTANCE instance;
static HWND main_wnd,appt_edit,pad,scroll,prev_button,next_button;
static Appt appts[APPTS]; static int nappts;
static Note notes[NOTES]; static int nnotes;
static int year,month,day;              /* the day shown */
static BOOL month_view,modified,trace;
static int interval=60,start_hour=7,sel_row,top_row;
static int cw,ch,row_h,status_h,pad_h,time_w;
static RECT area;                       /* the day list or the month */
static char file[MAX_PATH];
static const char *const month_names[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static const char *const day_names[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static void layout(void);

/* --- dates ----------------------------------------------------------------------- */
static BOOL leap(int y) {return (y%4==0 && y%100!=0) || y%400==0;}
static int month_days(int y,int m) {static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31}; return m==2 && leap(y)?29:d[m-1];}
/* Days since 1 March of year 0 (proleptic Gregorian), and back. */
static long serial(int y,int m,int d) {
    long era,yoe,doy;
    if(m<=2) y--;
    era=y/400; yoe=y-era*400;
    doy=(153*(m>2?m-3:m+9)+2)/5+d-1;
    return era*146097+yoe*365+yoe/4-yoe/100+doy;
}
static void civil(long n,int *y,int *m,int *d) {
    long era=n/146097,doe=n-era*146097,yoe=(doe-doe/1460+doe/36524-doe/146096)/365,doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153;
    *d=(int)(doy-(153*mp+2)/5+1); *m=(int)(mp<10?mp+3:mp-9); *y=(int)(yoe+era*400+(*m<=2));
}
static int weekday(int y,int m,int d) {return (int)((serial(y,m,d)+3)%7);}
static long key(int y,int m,int d) {return (long)y*10000+m*100+d;}
static void today(int *y,int *m,int *d) {SYSTEMTIME t; GetLocalTime(&t); *y=t.wYear; *m=t.wMonth; *d=t.wDay;}

/* --- appointments and notes --------------------------------------------------------- */
static const char *appointment(long date,int minute) {
    int i;
    for(i=0;i<nappts;i++) if(appts[i].date==date && appts[i].minute==minute) return appts[i].text;
    return "";
}
static void set_appointment(long date,int minute,const char *text) {
    int i;
    for(i=0;i<nappts;i++) if(appts[i].date==date && appts[i].minute==minute) break;
    if(i<nappts && !lstrcmp(appts[i].text,text)) return;
    if(!text[0]) {if(i<nappts) {appts[i]=appts[--nappts]; modified=TRUE;} return;}
    if(i==nappts) {if(nappts==APPTS) return; nappts++; appts[i].date=date; appts[i].minute=minute;}
    lstrcpyn(appts[i].text,text,sizeof(appts[i].text));
    modified=TRUE;
}
static const char *note(long date) {
    int i;
    for(i=0;i<nnotes;i++) if(notes[i].date==date) return notes[i].text;
    return "";
}
static void set_note(long date,const char *text) {
    int i;
    for(i=0;i<nnotes;i++) if(notes[i].date==date) break;
    if(i<nnotes && !lstrcmp(notes[i].text,text)) return;
    if(!text[0]) {if(i<nnotes) {notes[i]=notes[--nnotes]; modified=TRUE;} return;}
    if(i==nnotes) {if(nnotes==NOTES) return; nnotes++; notes[i].date=date;}
    lstrcpyn(notes[i].text,text,sizeof(notes[i].text));
    modified=TRUE;
}
static BOOL busy(long date) {
    int i;
    for(i=0;i<nappts;i++) if(appts[i].date==date) return TRUE;
    return note(date)[0]!=0;
}

/* --- the shown day ---------------------------------------------------------------------- */
static int rows(void) {return 1440/interval;}
static int visible_rows(void) {return max(1,(int)(area.bottom-area.top)/row_h);}
static void time_text(int minute,char *out) {
    int h=minute/60,m=minute%60;
    wsprintf(out,"%d:%02d %s",h%12?h%12:12,m,h<12?"AM":"PM");
}
static void report_day(void) {
    char line[160]; int r;
    if(!trace) return;
    wsprintf(line,"CALENDAR: day %d-%02d-%02d",year,month,day); OutputDebugString(line);
    for(r=0;r<rows();r++) {
        const char *a=appointment(key(year,month,day),r*interval);
        if(a[0]) {wsprintf(line,"CALENDAR: at %02d%02d %s",r*interval/60,r*interval%60,a); OutputDebugString(line);}
    }
}
/* Keep what was typed for the shown day. */
static void commit(void) {
    char text[256];
    if(!month_view) {GetWindowText(appt_edit,text,80); set_appointment(key(year,month,day),sel_row*interval,text);}
    GetWindowText(pad,text,sizeof(text));
    set_note(key(year,month,day),text);
}
static void place_editor(void) {
    int y=area.top+(sel_row-top_row)*row_h;
    if(month_view || sel_row<top_row || sel_row>=top_row+visible_rows()) {ShowWindow(appt_edit,SW_HIDE); return;}
    MoveWindow(appt_edit,area.left+time_w+4,y+1,area.right-area.left-time_w-6,row_h-2,TRUE);
    ShowWindow(appt_edit,SW_SHOW);
}
static void update_scroll(void) {
    SetScrollRange(scroll,SB_CTL,0,rows()-1,FALSE);
    SetScrollPos(scroll,SB_CTL,top_row,TRUE);
    ShowWindow(scroll,month_view?SW_HIDE:SW_SHOW);
}
/* Show the day's data in the editors. */
static void load_day(void) {
    SetWindowText(appt_edit,appointment(key(year,month,day),sel_row*interval));
    SetWindowText(pad,note(key(year,month,day)));
    place_editor(); update_scroll();
    InvalidateRect(main_wnd,NULL,TRUE);
    report_day();
}
static void select_row(int r) {
    commit();
    sel_row=max(0,min(r,rows()-1));
    if(sel_row<top_row) top_row=sel_row;
    if(sel_row>=top_row+visible_rows()) top_row=sel_row-visible_rows()+1;
    SetWindowText(appt_edit,appointment(key(year,month,day),sel_row*interval));
    SendMessage(appt_edit,EM_SETSEL,0,-1);
    place_editor(); update_scroll();
    InvalidateRect(main_wnd,&area,TRUE);
}
static void go(int y,int m,int d) {
    commit();
    year=y; month=m; day=d;
    load_day();
}
static void add_days(long n) {int y,m,d; civil(serial(year,month,day)+n,&y,&m,&d); go(y,m,d);}
static void add_months(int n) {
    int y=year,m=month+n;
    while(m>12) {m-=12; y++;}
    while(m<1) {m+=12; y--;}
    go(y,m,min(day,month_days(y,m)));
}
static void set_view(BOOL month) {
    commit();
    month_view=month;
    layout();
    if(trace) OutputDebugString(month?"CALENDAR: view month":"CALENDAR: view day");
    load_day();
    if(month) SetFocus(main_wnd); else SetFocus(appt_edit);
}

/* --- files ------------------------------------------------------------------------------ */
static void set_title(void) {
    char text[MAX_PATH+16];
    wsprintf(text,"Calendar - %s",file[0]?FileTitle(file):"(untitled)");
    SetWindowText(main_wnd,text);
}
static void clear(void) {nappts=nnotes=0; modified=FALSE;}
static int number(const char **p,int digits) {
    int v=0;
    while(digits-- && **p>='0' && **p<='9') v=v*10+(*(*p)++-'0');
    return v;
}
static BOOL load(LPCSTR path) {
    char *text=ReadWholeFile(path,NULL),*p,*line; char message[MAX_PATH+40];
    if(!text || memcmp(text,"CALENDAR",8)) {
        wsprintf(message,"%s is not a calendar file.",path);
        MessageBox(main_wnd,message,"Calendar",MB_OK|MB_ICONEXCLAMATION);
        if(text) GlobalFree(text);
        return FALSE;
    }
    clear();
    for(p=text;*p;) {
        const char *q; long date;
        line=p;
        while(*p && *p!='\n') p++;
        if(*p) *p++=0;
        if(p-line>1 && p[-2]=='\r') p[-2]=0;
        q=line;
        if(!memcmp(q,"CALENDAR",8)) {q+=8; while(*q==' ') q++; interval=number(&q,2); while(*q==' ') q++; start_hour=number(&q,2); continue;}
        date=number(&q,4)*10000L; date+=number(&q,2)*100; date+=number(&q,2);
        if(*q++!=' ' || date<10000) continue;
        if(!memcmp(q,"NOTE ",5) && nnotes<NOTES) {
            /* \n is a line break, \\ a backslash. */
            char *o=notes[nnotes].text; int n=0;
            for(q+=5;*q && n<(int)sizeof(notes[0].text)-2;q++) {
                if(*q=='\\' && q[1]=='n') {o[n++]='\r'; o[n++]='\n'; q++;}
                else if(*q=='\\' && q[1]=='\\') {o[n++]='\\'; q++;}
                else o[n++]=*q;
            }
            o[n]=0; notes[nnotes++].date=date;
        } else if(nappts<APPTS) {
            int minute=number(&q,2)*60; minute+=number(&q,2);
            if(*q==' ') q++;
            appts[nappts].date=date; appts[nappts].minute=minute; lstrcpyn(appts[nappts].text,q,sizeof(appts[0].text)); nappts++;
        }
    }
    GlobalFree(text);
    if(interval!=15 && interval!=30 && interval!=60) interval=60;
    if(start_hour<0 || start_hour>23) start_hour=7;
    lstrcpyn(file,path,sizeof(file)); modified=FALSE;
    set_title();
    if(trace) {wsprintf(message,"CALENDAR: opened %s",path); OutputDebugString(message);}
    return TRUE;
}
static BOOL save_to(LPCSTR path) {
    char *text; DWORD n=0; int i; BOOL ok; char message[MAX_PATH+40];
    commit();
    if(!(text=(char *)GlobalAlloc(GPTR,(DWORD)(nappts*100+nnotes*600+64)))) return FALSE;
    n=(DWORD)wsprintf(text,"CALENDAR %d %d\r\n",interval,start_hour);
    for(i=0;i<nappts;i++) n+=(DWORD)wsprintf(text+n,"%08ld %02d%02d %s\r\n",appts[i].date,appts[i].minute/60,appts[i].minute%60,appts[i].text);
    for(i=0;i<nnotes;i++) {
        const char *s=notes[i].text;
        n+=(DWORD)wsprintf(text+n,"%08ld NOTE ",notes[i].date);
        for(;*s;s++) {
            if(*s=='\r') continue;
            if(*s=='\n') {text[n++]='\\'; text[n++]='n';}
            else if(*s=='\\') {text[n++]='\\'; text[n++]='\\';}
            else text[n++]=*s;
        }
        text[n++]='\r'; text[n++]='\n';
    }
    ok=WriteWholeFile(path,text,n);
    GlobalFree(text);
    if(!ok) {wsprintf(message,"Cannot write %s.",path); MessageBox(main_wnd,message,"Calendar",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    lstrcpyn(file,path,sizeof(file)); modified=FALSE; set_title();
    if(trace) {wsprintf(message,"CALENDAR: saved %s",path); OutputDebugString(message);}
    return TRUE;
}
static BOOL save_as(void) {
    char path[MAX_PATH];
    lstrcpy(path,file[0]?file:"*.CAL");
    return FileSaveDialog(main_wnd,"Save As","*.CAL",path,sizeof(path)) && save_to(path);
}
static BOOL save(void) {return file[0]?save_to(file):save_as();}
/* TRUE when it is all right to drop what is in memory. */
static BOOL query_save(void) {
    char text[MAX_PATH+64]; int r;
    commit();
    if(!modified) return TRUE;
    wsprintf(text,"Do you want to save the changes to %s?",file[0]?FileTitle(file):"(untitled)");
    r=MessageBox(main_wnd,text,"Calendar",MB_YESNOCANCEL|MB_ICONQUESTION);
    if(r==IDCANCEL) return FALSE;
    return r==IDNO || save();
}

/* --- painting --------------------------------------------------------------------------- */
static void layout(void) {
    RECT r; int pad_top;
    GetClientRect(main_wnd,&r);
    pad_top=r.bottom-pad_h;
    SetRect(&area,0,status_h+1,r.right-(month_view?0:GetSystemMetrics(SM_CXVSCROLL)),pad_top-1);
    if(area.bottom<area.top) area.bottom=area.top;
    MoveWindow(prev_button,cw*10,3,cw*3,status_h-6,TRUE);
    MoveWindow(next_button,cw*13+2,3,cw*3,status_h-6,TRUE);
    MoveWindow(pad,0,pad_top,r.right,pad_h,TRUE);
    MoveWindow(scroll,r.right-GetSystemMetrics(SM_CXVSCROLL),area.top,GetSystemMetrics(SM_CXVSCROLL),area.bottom-area.top,TRUE);
}
static void paint_status(HDC dc,const RECT *client) {
    SYSTEMTIME t; char text[80]; RECT r;
    GetLocalTime(&t);
    time_text(t.wHour*60+t.wMinute,text);
    TextOut(dc,cw,(status_h-ch)/2,text,lstrlen(text));
    wsprintf(text,"%s, %s %d, %d",day_names[weekday(year,month,day)],month_names[month-1],day,year);
    TextOut(dc,cw*17,(status_h-ch)/2,text,lstrlen(text));
    SetRect(&r,0,status_h,client->right,status_h+1); FillRect(dc,&r,GetStockObject(BLACK_BRUSH));
}
static void paint_day(HDC dc) {
    int r; char text[16]; long date=key(year,month,day);
    for(r=top_row;r<top_row+visible_rows()+1 && r<rows();r++) {
        int y=area.top+(r-top_row)*row_h; RECT line; SIZE s;
        if(y>=area.bottom) break;
        time_text(r*interval,text);
        GetTextExtentPoint(dc,text,lstrlen(text),&s);
        TextOut(dc,area.left+time_w-(int)s.cx,y+2,text,lstrlen(text));
        if(r!=sel_row || month_view) {const char *a=appointment(date,r*interval); TextOut(dc,area.left+time_w+6,y+2,a,lstrlen(a));}
        SetRect(&line,area.left,y+row_h-1,area.right,y+row_h); FillRect(dc,&line,(HBRUSH)(COLOR_BTNSHADOW+1));
    }
}
static void month_cell(int cell,RECT *r) {
    int w=(area.right-area.left)/7,h=(area.bottom-area.top-2*row_h)/6;
    SetRect(r,area.left+(cell%7)*w,area.top+2*row_h+(cell/7)*h,area.left+(cell%7+1)*w,area.top+2*row_h+(cell/7+1)*h);
}
static void paint_month(HDC dc) {
    char text[40]; RECT r; int i,first=weekday(year,month,1),ty,tm,td;
    today(&ty,&tm,&td);
    wsprintf(text,"%s %d",month_names[month-1],year);
    SetRect(&r,area.left,area.top,area.right,area.top+row_h); DrawText(dc,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    for(i=0;i<7;i++) {
        RECT c; month_cell(i,&c); c.top=area.top+row_h; c.bottom=c.top+row_h;
        lstrcpyn(text,day_names[i],4); DrawText(dc,text,-1,&c,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    for(i=0;i<month_days(year,month);i++) {
        RECT c; int d=i+1;
        month_cell(first+i,&c);
        FrameRect(dc,&c,(HBRUSH)(COLOR_BTNSHADOW+1));
        if(d==day) {RECT s=c; InflateRect(&s,-2,-2); FillRect(dc,&s,(HBRUSH)(COLOR_HIGHLIGHT+1)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
        else SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        if(year==ty && month==tm && d==td) wsprintf(text,">%d<",d); else wsprintf(text,"%d",d);
        DrawText(dc,text,-1,&c,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(busy(key(year,month,d))) {RECT m; SetRect(&m,c.right-8,c.top+3,c.right-3,c.top+8); FillRect(dc,&m,d==day?GetStockObject(WHITE_BRUSH):GetStockObject(BLACK_BRUSH));}
    }
    SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
}
static int month_hit(int x,int y) {
    int i,first=weekday(year,month,1);
    for(i=0;i<month_days(year,month);i++) {RECT c; POINT p; p.x=x; p.y=y; month_cell(first+i,&c); if(PtInRect(&c,p)) return i+1;}
    return 0;
}

/* --- dialogs ---------------------------------------------------------------------------- */
static INT_PTR CALLBACK DateProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {char text[16]; wsprintf(text,"%d/%d/%d",month,day,year); SetDlgItemText(h,IDC_DATE,text); SendDlgItemMessage(h,IDC_DATE,EM_SETSEL,0,-1); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        char text[32]; const char *p=text; int m,d,y;
        GetDlgItemText(h,IDC_DATE,text,sizeof(text));
        m=number(&p,2); if(*p=='/' || *p=='-') p++;
        d=number(&p,2); if(*p=='/' || *p=='-') p++;
        y=number(&p,4);
        if(y<100) y+=y<80?2000:1900;
        if(*p || m<1 || m>12 || y<1980 || y>2099 || d<1 || d>month_days(y,m)) {
            MessageBox(h,"Enter a date between 1/1/1980 and 12/31/2099, as month/day/year.","Calendar",MB_OK|MB_ICONEXCLAMATION);
            return TRUE;
        }
        EndDialog(h,IDOK); go(y,m,d);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK DayProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {CheckRadioButton(h,IDC_15,IDC_60,interval==15?IDC_15:interval==30?IDC_30:IDC_60); SetDlgItemInt(h,IDC_START,(UINT)start_hour,FALSE); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        BOOL ok; UINT s=GetDlgItemInt(h,IDC_START,&ok,FALSE);
        if(!ok || s>23) {MessageBox(h,"The starting hour is from 0 to 23.","Calendar",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
        commit();
        interval=IsDlgButtonChecked(h,IDC_15)?15:IsDlgButtonChecked(h,IDC_30)?30:60;
        start_hour=(int)s; sel_row=top_row=start_hour*60/interval; modified=TRUE;
        EndDialog(h,IDOK); load_day();
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
    return FALSE;
}

/* --- the window --------------------------------------------------------------------------- */
static void command(HWND h,UINT id) {
    HWND f=GetFocus(); char path[MAX_PATH]; int y,m,d;
    if(HelpCommand(h,id,"CALENDAR.HLP")) return;
    switch(id) {
    case IDM_NEW: if(query_save()) {clear(); file[0]=0; set_title(); load_day();} return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.CAL");
        if(FileOpenDialog(h,"Open","*.CAL",path,sizeof(path)) && load(path)) {sel_row=top_row=start_hour*60/interval; load_day();}
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_CUT: if(f==appt_edit || f==pad) SendMessage(f,WM_CUT,0,0); return;
    case IDM_COPY: if(f==appt_edit || f==pad) SendMessage(f,WM_COPY,0,0); return;
    case IDM_PASTE: if(f==appt_edit || f==pad) SendMessage(f,WM_PASTE,0,0); return;
    case IDM_DAY: if(month_view) set_view(FALSE); return;
    case IDM_MONTH: if(!month_view) set_view(TRUE); return;
    case IDM_TODAY: today(&y,&m,&d); go(y,m,d); return;
    case IDM_PREVIOUS: if(month_view) add_months(-1); else add_days(-1); return;
    case IDM_NEXT: if(month_view) add_months(1); else add_days(1); return;
    case IDM_DATE: DialogBox(instance,"DATEDLG",h,DateProc); return;
    case IDM_DAYSETTINGS: DialogBox(instance,"DAYDLG",h,DayProc); return;
    case IDM_ABOUT: MessageBox(h,"Calendar\nInterface Manager 3.0 for IA-64","About Calendar",MB_OK|MB_ICONINFORMATION); return;
    }
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        HDC dc=GetDC(h); TEXTMETRIC tm;
        GetTextMetrics(dc,&tm); ReleaseDC(h,dc);
        cw=(int)tm.tmAveCharWidth; ch=(int)tm.tmHeight; row_h=ch+4; status_h=ch+10; pad_h=3*ch+8; time_w=cw*9;
        main_wnd=h;
        appt_edit=CreateWindow("EDIT",NULL,WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,h,(HMENU)ID_APPT,instance,NULL);
        SendMessage(appt_edit,EM_LIMITTEXT,79,0);
        pad=CreateWindow("EDIT",NULL,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL,0,0,0,0,h,(HMENU)ID_PAD,instance,NULL);
        SendMessage(pad,EM_LIMITTEXT,240,0);
        scroll=CreateWindow("SCROLLBAR",NULL,WS_CHILD|WS_VISIBLE|SBS_VERT,0,0,0,0,h,(HMENU)ID_SCROLL,instance,NULL);
        prev_button=CreateWindow("BUTTON","<",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,h,(HMENU)ID_PREV,instance,NULL);
        next_button=CreateWindow("BUTTON",">",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,h,(HMENU)ID_NEXT,instance,NULL);
        SetTimer(h,1,30000,NULL);
        return 0;
    }
    case WM_SIZE: layout(); place_editor(); update_scroll(); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_SETFOCUS: SetFocus(month_view?h:appt_edit); return 0;
    case WM_TIMER: {RECT r; GetClientRect(h,&r); r.bottom=status_h; InvalidateRect(h,&r,TRUE);} return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r;
        GetClientRect(h,&r);
        SetBkMode(dc,TRANSPARENT);
        paint_status(dc,&r);
        if(month_view) paint_month(dc); else paint_day(dc);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);
        if(y<area.top || y>=area.bottom) return 0;
        if(month_view) {
            int d=month_hit(x,y);
            if(d) {go(year,month,d); if(msg==WM_LBUTTONDBLCLK) set_view(FALSE); else SetFocus(h);}
        } else {select_row(top_row+(y-area.top)/row_h); SetFocus(appt_edit);}
        return 0;
    }
    case WM_KEYDOWN:
        if(!month_view) return 0;
        switch(wp) {
        case VK_LEFT: add_days(-1); break;
        case VK_RIGHT: add_days(1); break;
        case VK_UP: add_days(-7); break;
        case VK_DOWN: add_days(7); break;
        case VK_RETURN: set_view(FALSE); break;
        }
        return 0;
    case WM_VSCROLL: {
        int top=top_row,lo,hi;
        switch(LOWORD(wp)) {
        case SB_LINEUP: top--; break;
        case SB_LINEDOWN: top++; break;
        case SB_PAGEUP: top-=visible_rows(); break;
        case SB_PAGEDOWN: top+=visible_rows(); break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: top=(short)HIWORD(wp); break;
        default: return 0;
        }
        GetScrollRange(scroll,SB_CTL,&lo,&hi);
        top=max(lo,min(top,hi));
        if(top!=top_row) {top_row=top; SetScrollPos(scroll,SB_CTL,top,TRUE); place_editor(); InvalidateRect(h,&area,TRUE);}
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==ID_PREV && HIWORD(wp)==BN_CLICKED) {command(h,IDM_PREVIOUS); SetFocus(month_view?h:appt_edit); return 0;}
        if(LOWORD(wp)==ID_NEXT && HIWORD(wp)==BN_CLICKED) {command(h,IDM_NEXT); SetFocus(month_view?h:appt_edit); return 0;}
        if(LOWORD(wp)==ID_PAD || LOWORD(wp)==ID_APPT) return 0;
        command(h,LOWORD(wp));
        return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"CALENDAR.HLP",HELP_QUIT,0); KillTimer(h,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
/* Keys the editors leave to Calendar: moving between times and editors. */
static BOOL editor_key(const MSG *m) {
    if(m->message!=WM_KEYDOWN) return FALSE;
    if(m->wParam==VK_TAB && (m->hwnd==appt_edit || m->hwnd==pad)) {SetFocus(m->hwnd==pad?(month_view?main_wnd:appt_edit):pad); return TRUE;}
    if(m->wParam==VK_TAB && m->hwnd==main_wnd) {SetFocus(pad); return TRUE;}
    if(m->hwnd!=appt_edit || GetKeyState(VK_CONTROL)<0) return FALSE;
    switch(m->wParam) {
    case VK_UP: select_row(sel_row-1); return TRUE;
    case VK_DOWN: case VK_RETURN: select_row(sel_row+1); return TRUE;
    case VK_PRIOR: select_row(sel_row-visible_rows()); return TRUE;
    case VK_NEXT: select_row(sel_row+visible_rows()); return TRUE;
    }
    return FALSE;
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; char args[MAX_PATH],*p=args,*word; char open_path[MAX_PATH];
    instance=inst; open_path[0]=0;
    lstrcpyn(args,command_line?command_line:"",sizeof(args));
    while(*p) {
        while(*p==' ') p++;
        word=p;
        while(*p && *p!=' ') p++;
        if(*p) *p++=0;
        if(!lstrcmpi(word,"/trace")) trace=TRUE;
        else if(*word) lstrcpyn(open_path,word,sizeof(open_path));
    }
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"CALENDAR"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="CALENDAR"; wc.lpszClassName="Calendar";
        RegisterClass(&wc);
    }
    today(&year,&month,&day);
    sel_row=top_row=start_hour*60/interval;
    main_wnd=CreateWindow("Calendar","Calendar",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    if(open_path[0] && load(open_path)) sel_row=top_row=start_hour*60/interval;
    set_title();
    ShowWindow(main_wnd,show);
    layout(); load_day();
    SetFocus(appt_edit);
    UpdateWindow(main_wnd);
    accel=LoadAccelerators(inst,"CALENDAR");
    while(GetMessage(&m,NULL,0,0)) {
        if(TranslateAccelerator(main_wnd,accel,&m) || editor_key(&m)) continue;
        TranslateMessage(&m); DispatchMessage(&m);
    }
    return (int)m.wParam;
}
