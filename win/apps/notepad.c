/* SPDX-License-Identifier: GPL-2.0-or-later
 * NOTEPAD: the text editor. Its client area is a multiline EDIT control;
 * Word Wrap recreates the control with or without horizontal scrolling.
 * Files open and save through WINAPP.LIB's dialogs; a file named on the
 * command line opens at start. Print sends the text to the printer Print
 * Setup chose (or the default) with Page Setup's header, footer and margins.
 */
#include <windows.h>
#include "winapp.h"
#include "notepad.h"

static HINSTANCE instance;
static HWND main_wnd,edit;
static char file[260];             /* empty: untitled */
static BOOL wrap;
static char find_text[128];
static BOOL match_case,find_up;

static DWORD edit_style(void) {
    DWORD s=WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_NOHIDESEL;
    return wrap?s:s|WS_HSCROLL|ES_AUTOHSCROLL;
}
static void set_title(void) {
    char title[300];
    wsprintf(title,"Notepad - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(main_wnd,title);
}
static LPSTR text_of(void) {
    int n=GetWindowTextLength(edit); LPSTR t=(LPSTR)GlobalAlloc(GPTR,(DWORD)n+1);
    if(t) GetWindowText(edit,t,n+1);
    return t;
}
static void message(LPCSTR format,LPCSTR name,UINT type) {
    char text[400]; wsprintf(text,format,name); MessageBox(main_wnd,text,"Notepad",type);
}

/* --- files --------------------------------------------------------------------- */
static BOOL save_to(LPCSTR path) {
    LPSTR t=text_of(); BOOL ok;
    if(!t) {message("Not enough memory to save %s.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    ok=WriteWholeFile(path,t,(DWORD)lstrlen(t));
    GlobalFree(t);
    if(!ok) {message("Cannot write to the %s file.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    SendMessage(edit,EM_SETMODIFY,FALSE,0);
    return TRUE;
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.TXT");
    if(!FileSaveDialog(main_wnd,"Save As","*.TXT",path,sizeof(path)) || !save_to(path)) return FALSE;
    lstrcpy(file,path); set_title();
    return TRUE;
}
static BOOL save(void) {return file[0]?save_to(file):save_as();}
/* Before the text goes: Yes saves, No discards, Cancel keeps it. */
static BOOL query_save(void) {
    char text[400];
    if(!SendMessage(edit,EM_GETMODIFY,0,0)) return TRUE;
    wsprintf(text,"The text in the %s file has changed.\n\nDo you want to save the changes?",file[0]?file:"[Untitled]");
    switch(MessageBox(main_wnd,text,"Notepad",MB_YESNOCANCEL|MB_ICONEXCLAMATION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}
static void set_text(LPCSTR text) {
    SetWindowText(edit,text);
    SendMessage(edit,EM_SETSEL,0,0);
    SendMessage(edit,EM_SETMODIFY,FALSE,0);
}
static BOOL load(LPCSTR path) {
    LPSTR t=ReadWholeFile(path,NULL);
    if(!t) {message("Cannot open the %s file.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    set_text(t);
    GlobalFree(t);
    lstrcpy(file,path); AnsiUpper(file); set_title();
    return TRUE;
}
/* A file named on the command line: opened, or created when it is new. */
static void open_argument(LPCSTR arg) {
    char path[260]; int n;
    while(*arg==' ') arg++;
    lstrcpyn(path,arg,sizeof(path));
    for(n=lstrlen(path);n && path[n-1]==' ';n--) path[n-1]=0;
    if(!path[0]) return;
    if(GetFileAttributes(path)!=INVALID_FILE_ATTRIBUTES) {load(path); return;}
    {
        char text[400];
        wsprintf(text,"Cannot find the %s file.\n\nDo you want to create a new file?",path);
        if(MessageBox(main_wnd,text,"Notepad",MB_YESNO|MB_ICONQUESTION)==IDYES) {lstrcpy(file,path); AnsiUpper(file); set_title();}
    }
}

/* --- editing --------------------------------------------------------------------- */
static void create_edit(void) {
    RECT r; GetClientRect(main_wnd,&r);
    edit=CreateWindow("EDIT",NULL,edit_style(),0,0,r.right,r.bottom,main_wnd,(HMENU)1,instance,NULL);
    SendMessage(edit,EM_LIMITTEXT,0,0);
}
/* Word Wrap: a new control with the other style, the same text and selection. */
static void set_wrap(BOOL on) {
    LPSTR t=text_of(); DWORD a=0,b=0; BOOL modified=(BOOL)SendMessage(edit,EM_GETMODIFY,0,0); HWND old=edit;
    SendMessage(edit,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);
    wrap=on;
    create_edit();
    DestroyWindow(old);
    SetWindowText(edit,t?t:"");
    if(t) GlobalFree(t);
    SendMessage(edit,EM_SETSEL,a,b);
    SendMessage(edit,EM_SETMODIFY,modified,0);
    SendMessage(edit,EM_SCROLLCARET,0,0);
    SetFocus(edit);
}
static void update_menu(HMENU m) {
    DWORD a=0,b=0; UINT selected;
    SendMessage(edit,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);
    selected=a!=b?MF_ENABLED:MF_GRAYED;
    EnableMenuItem(m,IDM_UNDO,SendMessage(edit,EM_CANUNDO,0,0)?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,IDM_CUT,selected);
    EnableMenuItem(m,IDM_COPY,selected);
    EnableMenuItem(m,IDM_DELETE,selected);
    EnableMenuItem(m,IDM_PASTE,IsClipboardFormatAvailable(CF_TEXT)?MF_ENABLED:MF_GRAYED);
    EnableMenuItem(m,IDM_FINDNEXT,find_text[0]?MF_ENABLED:MF_GRAYED);
    CheckMenuItem(m,IDM_WORDWRAP,wrap?MF_CHECKED:MF_UNCHECKED);
}
static void time_date(void) {
    SYSTEMTIME t; char text[40]; int hour;
    GetLocalTime(&t);
    hour=t.wHour%12; if(!hour) hour=12;
    wsprintf(text,"%d:%02d %s %d/%d/%d",hour,t.wMinute,t.wHour<12?"AM":"PM",t.wMonth,t.wDay,t.wYear);
    SendMessage(edit,EM_REPLACESEL,0,(LPARAM)text);
}

/* --- searching --------------------------------------------------------------------- */
static BOOL same(char a,char b) {
    if(match_case) return a==b;
    return (BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)a)==(BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)b);
}
static BOOL at(LPCSTR t,int i,int n) {int k; for(k=0;k<n;k++) if(!same(t[i+k],find_text[k])) return FALSE; return TRUE;}
static void find_next(void) {
    LPSTR t=text_of(); DWORD a=0,b=0; int n=lstrlen(find_text),len,i,found=-1;
    if(!t) return;
    len=lstrlen(t);
    SendMessage(edit,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);
    if(find_up) {for(i=(int)a-1;i>=0;i--) if(i+n<=len && at(t,i,n)) {found=i; break;}}
    else for(i=(int)b;i+n<=len;i++) if(at(t,i,n)) {found=i; break;}
    GlobalFree(t);
    if(found<0) {char text[200]; wsprintf(text,"Cannot find \"%s\"",find_text); MessageBox(main_wnd,text,"Notepad",MB_OK|MB_ICONASTERISK); return;}
    SendMessage(edit,EM_SETSEL,(WPARAM)found,(LPARAM)(found+n));
    SendMessage(edit,EM_SCROLLCARET,0,0);
}
static INT_PTR CALLBACK FindProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_FINDWHAT,find_text);
        CheckDlgButton(dlg,IDC_MATCHCASE,match_case?BST_CHECKED:BST_UNCHECKED);
        CheckRadioButton(dlg,IDC_UP,IDC_DOWN,find_up?IDC_UP:IDC_DOWN);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            GetDlgItemText(dlg,IDC_FINDWHAT,find_text,sizeof(find_text));
            match_case=IsDlgButtonChecked(dlg,IDC_MATCHCASE)!=0;
            find_up=IsDlgButtonChecked(dlg,IDC_UP)!=0;
            EndDialog(dlg,find_text[0]?IDOK:IDCANCEL);
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- printing ------------------------------------------------------------------------ */
/* Page Setup: the header and footer (&f the file, &p the page, &d the date,
 * &t the time, && an &; &l, &c and &r put what follows left, centred or
 * right) and the margins, in hundredths of an inch. */
static char header[64]="&f",footer[64]="Page &p";
static int margins[4]={75,75,100,100}; /* left, right, top, bottom */
static int margin_ids[4]={IDC_LEFT,IDC_RIGHT,IDC_TOP,IDC_BOTTOM};

static void heading(HDC dc,LPCSTR format,int page,int y,int left,int right) {
    char part[3][160]; int n[3]={0,0,0},at=1,i; LPCSTR s; SYSTEMTIME t; char item[64];
    GetLocalTime(&t);
    for(s=format;*s;s++) {
        item[0]=*s; item[1]=0;
        if(*s=='&' && s[1]) {
            char c=(char)(*++s|0x20);
            if(c=='l' || c=='c' || c=='r') {at=c=='l'?0:c=='c'?1:2; continue;}
            if(c=='f') lstrcpyn(item,file[0]?FileTitle(file):"(Untitled)",sizeof(item));
            else if(c=='p') wsprintf(item,"%d",page);
            else if(c=='d') wsprintf(item,"%d/%d/%02d",t.wMonth,t.wDay,t.wYear%100);
            else if(c=='t') wsprintf(item,"%d:%02d %s",t.wHour%12?t.wHour%12:12,t.wMinute,t.wHour<12?"AM":"PM");
            else if(*s!='&') continue;
        }
        for(i=0;item[i] && n[at]<(int)sizeof(part[0])-1;i++) part[at][n[at]++]=item[i];
    }
    for(i=0;i<3;i++) if(n[i]) {
        SIZE size; int x;
        GetTextExtentPoint(dc,part[i],n[i],&size);
        x=i==0?left:i==1?(left+right-size.cx)/2:right-size.cx;
        TextOut(dc,x,y,part[i],n[i]);
    }
}
/* The next printed row: up to cols characters of a line, tabs expanded;
 * -1 at the end of the text. */
static int next_row(LPCSTR *text,char *row,int cols) {
    LPCSTR s=*text; int n=0;
    if(!*s) return -1;
    while(*s && *s!='\n' && n<cols) {
        if(*s=='\r') {s++; continue;}
        if(*s=='\t') {do row[n++]=' '; while(n%8 && n<cols); s++; continue;}
        row[n++]=*s++;
    }
    while(*s=='\r') s++;
    if(*s=='\n') s++;
    *text=s;
    return n;
}
/* The text in Courier at 10 points between the margins, with the header at
 * the top and the footer at the bottom of each page. */
static void print_text(HWND h) {
    HDC dc=PrinterDC(h); LPSTR text; LPCSTR s; TEXTMETRIC tm; HFONT font,old; POINT offset,paper; char row[512],name[260];
    int dpi_x,dpi_y,width,height,left,right,top,bottom,line,body,y=0,page=0,cols,n; BOOL ok=TRUE;
    if(!dc) return;
    if(!(text=text_of())) {DeleteDC(dc); MessageBox(h,"Not enough memory to print.","Notepad",MB_OK|MB_ICONEXCLAMATION); return;}
    lstrcpy(name,file[0]?FileTitle(file):"(Untitled)");
    if(!PrintStart(dc,h,"Notepad",name)) {GlobalFree(text); return;}
    dpi_x=GetDeviceCaps(dc,LOGPIXELSX); dpi_y=GetDeviceCaps(dc,LOGPIXELSY);
    width=GetDeviceCaps(dc,HORZRES); height=GetDeviceCaps(dc,VERTRES);
    if(Escape(dc,GETPRINTINGOFFSET,0,NULL,&offset)<=0) offset.x=offset.y=0;
    if(Escape(dc,GETPHYSPAGESIZE,0,NULL,&paper)<=0) {paper.x=width; paper.y=height;}
    left=max(0,margins[0]*dpi_x/100-offset.x); right=min(width,paper.x-offset.x-margins[1]*dpi_x/100);
    top=max(0,margins[2]*dpi_y/100-offset.y); bottom=min(height,paper.y-offset.y-margins[3]*dpi_y/100);
    if(right-left<dpi_x || bottom-top<dpi_y) {left=top=0; right=width; bottom=height;}
    font=CreateFont(-MulDiv(10,dpi_y,72),0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,0,0,0,FIXED_PITCH|FF_MODERN,"Courier");
    old=(HFONT)SelectObject(dc,font); GetTextMetrics(dc,&tm);
    line=tm.tmHeight+tm.tmExternalLeading;
    body=bottom-(footer[0]?2*line:0);
    cols=min((int)sizeof(row),max(1,(right-left)/max(1,tm.tmAveCharWidth)));
    for(s=text;ok && (n=next_row(&s,row,cols))>=0;) {
        if(!y) {
            page++; ok=StartPage(dc)>0;
            if(header[0]) heading(dc,header,page,top,left,right);
            y=top+(header[0]?2*line:0);
        }
        TextOut(dc,left,y,row,n); y+=line;
        if(y+line>body) {
            if(footer[0]) heading(dc,footer,page,bottom-line,left,right);
            ok=ok && EndPage(dc)>0 && !PrintCancelled(); y=0;
        }
    }
    if(y && ok) {
        if(footer[0]) heading(dc,footer,page,bottom-line,left,right);
        ok=EndPage(dc)>0 && !PrintCancelled();
    }
    GlobalFree(text);
    SelectObject(dc,old);
    PrintEnd(dc,ok);
    DeleteObject(font);
}
/* Margins as Page Setup shows them: ".75", "1", "1.5". */
static void format_margin(char *out,int v) {
    char *p=out;
    if(v>=100 || !v) p+=wsprintf(p,"%d",v/100);
    if(v%10) wsprintf(p,".%02d",v%100);
    else if(v%100) wsprintf(p,".%d",v%100/10);
}
static BOOL parse_margin(LPCSTR s,int *out) {
    int v=0,d=0,digits=0;
    while(*s==' ') s++;
    for(;*s>='0' && *s<='9';s++,digits++) v=v*10+(*s-'0');
    v*=100;
    if(*s=='.') for(s++;*s>='0' && *s<='9';s++,digits++) {if(d<2) v+=(*s-'0')*(d?1:10); d++;}
    while(*s==' ') s++;
    if(*s || !digits || v>2000) return FALSE;
    *out=v; return TRUE;
}
static INT_PTR CALLBACK PageSetupProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    char text[16]; int i,v[4];
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_HEADER,header); SetDlgItemText(dlg,IDC_FOOTER,footer);
        for(i=0;i<4;i++) {format_margin(text,margins[i]); SetDlgItemText(dlg,margin_ids[i],text);}
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            for(i=0;i<4;i++) {
                GetDlgItemText(dlg,margin_ids[i],text,sizeof(text));
                if(!parse_margin(text,&v[i])) {
                    MessageBox(dlg,"The margin must be a number of inches.","Notepad",MB_OK|MB_ICONEXCLAMATION);
                    SetFocus(GetDlgItem(dlg,margin_ids[i])); return TRUE;
                }
            }
            for(i=0;i<4;i++) margins[i]=v[i];
            GetDlgItemText(dlg,IDC_HEADER,header,sizeof(header)); GetDlgItemText(dlg,IDC_FOOTER,footer,sizeof(footer));
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- the main window ----------------------------------------------------------------- */
static void command(HWND h,UINT id) {
    char path[260];
    if(HelpCommand(h,id,"NOTEPAD.HLP")) return;
    switch(id) {
    case IDM_NEW:
        if(!query_save()) return;
        set_text(""); file[0]=0; set_title();
        return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.TXT");
        if(FileOpenDialog(h,"Open","*.TXT",path,sizeof(path))) load(path);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_PRINT: print_text(h); return;
    case IDM_PAGESETUP: DialogBox(instance,"PAGESETUP",h,PageSetupProc); return;
    case IDM_PRINTSETUP: PrinterSetup(h); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_UNDO: SendMessage(edit,EM_UNDO,0,0); return;
    case IDM_CUT: SendMessage(edit,WM_CUT,0,0); return;
    case IDM_COPY: SendMessage(edit,WM_COPY,0,0); return;
    case IDM_PASTE: SendMessage(edit,WM_PASTE,0,0); return;
    case IDM_DELETE: SendMessage(edit,WM_CLEAR,0,0); return;
    case IDM_SELECTALL: SendMessage(edit,EM_SETSEL,0,-1); return;
    case IDM_TIMEDATE: time_date(); return;
    case IDM_WORDWRAP: set_wrap(!wrap); return;
    case IDM_FIND: if(DialogBox(instance,"FIND",h,FindProc)==IDOK) find_next(); return;
    case IDM_FINDNEXT:
        if(!find_text[0]) {command(h,IDM_FIND); return;}
        find_next();
        return;
    case IDM_ABOUT:
        MessageBox(h,"Notepad\n\nA text editor for Interface Manager 3.0 on IA-64.","About Notepad",MB_OK|MB_ICONASTERISK);
        return;
    }
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: main_wnd=h; create_edit(); return 0;
    case WM_SIZE: if(edit) MoveWindow(edit,0,0,LOWORD(lp),HIWORD(lp),TRUE); return 0;
    case WM_SETFOCUS: if(edit) SetFocus(edit); return 0;
    case WM_INITMENU: update_menu((HMENU)wp); return 0;
    case WM_INITMENUPOPUP: if(!HIWORD(lp)) update_menu((HMENU)wp); return 0;
    case WM_COMMAND:
        if(lp && (HWND)lp==edit) {
            if(HIWORD(wp)==EN_ERRSPACE || HIWORD(wp)==EN_MAXTEXT) MessageBox(h,"Not enough memory.","Notepad",MB_OK|MB_ICONEXCLAMATION);
            return 0;
        }
        command(h,LOWORD(wp));
        return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"NOTEPAD.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"NOTEPAD"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="NOTEPAD"; wc.lpszClassName="Notepad";
        if(!RegisterClass(&wc)) return 0;
    }
    main_wnd=CreateWindow("Notepad","Notepad",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    set_title();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    if(command_line && command_line[0]) open_argument(command_line);
    SetFocus(edit);
    accel=LoadAccelerators(inst,"NOTEPAD");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
