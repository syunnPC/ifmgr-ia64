/* SPDX-License-Identifier: GPL-2.0-or-later
 * COM terminal over USER communications and PORTDRV.SYS, polled by timer.
 * Render 80-column TTY/VT-100 text with cursor/erase/index sequences;
 * accept but do not render attributes. Arrow keys use VT-100 sequences.
 * .TRM settings use this program's own format.
 * Dial/Hangup sends configured modem commands; text files can be sent/captured.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "terminal.h"
#define COLS 80
#define ROWS 24
#define MAX_LINES 400
#define TIMER 1
#define HANGUP_TIMER 2
#define POLL_MS 50

typedef struct {
    char magic[16];
    int port,baud,data_bits,stop_bits,parity,flow; /* port 0: none; stop: 0 one, 1 one and a half, 2 two */
    BOOL vt100,wrap,echo,sound,inbound,outbound; int lines;
    char number[32]; int timeout;
    char dial_prefix[16],dial_suffix[16],hang_prefix[16],hang_suffix[16],originate[40];
} Settings;
static const char magic[16]="IM TERMINAL 1\r\n";
static const int bauds[8]={110,300,600,1200,2400,4800,9600,19200};
static Settings settings;
static HINSTANCE instance;
static HWND main_wnd;
static char file[260];
static BOOL modified;
static int comm=-1;                       /* the open port */
/* The buffer: lines of text, the last ROWS the screen; the cursor on it. */
static char (*lines)[COLS]; static int line_count,cursor_row,cursor_col;
static int char_w,char_h; static HFONT font;
/* The VT-100's escape sequences. */
static enum {PLAIN,ESCAPE,CSI} state; static int params[8],param_count;
/* Transfers: a file sent a piece each tick, or what arrives captured. */
static HFILE sending=HFILE_ERROR,capturing=HFILE_ERROR;

static void defaults(void) {
    memset(&settings,0,sizeof(settings));
    memcpy(settings.magic,magic,sizeof(magic));
    settings.port=1; settings.baud=9600; settings.data_bits=8; settings.stop_bits=0; settings.parity=NOPARITY;
    settings.flow=0; settings.vt100=TRUE; settings.wrap=TRUE; settings.sound=TRUE; settings.lines=100;
    settings.timeout=30;
    lstrcpy(settings.dial_prefix,"ATDT"); lstrcpy(settings.hang_prefix,"+++"); lstrcpy(settings.hang_suffix,"ATH");
    lstrcpy(settings.originate,"ATQ0V1E1S0=0");
}

/* --- the port ------------------------------------------------------------------------ */
static void close_port(void) {
    if(comm>=0) {KillTimer(main_wnd,TIMER); CloseComm(comm); comm=-1;}
}
/* The port as the settings have it; FALSE (and said) when it cannot be opened. */
static BOOL open_port(BOOL quiet) {
    char name[8]; DCB d;
    close_port();
    if(!settings.port) return TRUE;
    wsprintf(name,"COM%d",settings.port);
    if((comm=OpenComm(name,1024,128))<0) {
        comm=-1;
        if(!quiet) {char text[96]; wsprintf(text,"%s: is not available. Use Communications to choose another port.",(LPCSTR)name);
                    MessageBox(main_wnd,text,"Terminal",MB_OK|MB_ICONEXCLAMATION);}
        return FALSE;
    }
    GetCommState(comm,&d);
    d.BaudRate=(UINT)settings.baud; d.ByteSize=(BYTE)settings.data_bits; d.Parity=(BYTE)settings.parity;
    d.StopBits=(BYTE)(settings.stop_bits==2?TWOSTOPBITS:settings.stop_bits==1?ONE5STOPBITS:ONESTOPBIT);
    d.fOutxCtsFlow=settings.flow==1; d.fRtsDisable=0; d.fDtrDisable=0; d.fOutX=d.fInX=settings.flow==0;
    if(SetCommState(&d)!=0 && !quiet) MessageBox(main_wnd,"The port does not take these settings.","Terminal",MB_OK|MB_ICONEXCLAMATION);
    SetTimer(main_wnd,TIMER,POLL_MS,NULL);
    return TRUE;
}
static void send(const char *s,int n) {if(comm>=0 && n>0) WriteComm(comm,s,n);}

/* --- the screen ------------------------------------------------------------------------ */
/* The view: the screen's lines at the bottom, scrolled back by the bar. */
static int view_top(void) {return GetScrollPos(main_wnd,SB_VERT);}
static void invalidate_line(int line) {
    RECT r; GetClientRect(main_wnd,&r);
    r.top=(line-view_top())*char_h; r.bottom=r.top+char_h;
    if(r.bottom>0) InvalidateRect(main_wnd,&r,FALSE);
}
static void place_caret(void) {
    if(GetFocus()==main_wnd) SetCaretPos(cursor_col*char_w,(cursor_row-view_top()+1)*char_h-2); /* an underline */
}
static void update_bar(void) {
    RECT r; int visible,first;
    GetClientRect(main_wnd,&r); visible=r.bottom/max(char_h,1);
    first=max(0,line_count-visible);
    SetScrollRange(main_wnd,SB_VERT,0,first,FALSE); SetScrollPos(main_wnd,SB_VERT,first,TRUE);
}
static void clear_buffer(void) {
    int i;
    line_count=ROWS;
    for(i=0;i<settings.lines;i++) memset(lines[i],' ',COLS);
    cursor_row=0; cursor_col=0;
    update_bar(); InvalidateRect(main_wnd,NULL,TRUE); place_caret();
}
/* A new line at the bottom; the oldest goes when the buffer is full. */
static void new_line(void) {
    if(cursor_row<line_count-1) {cursor_row++; return;}
    if(line_count<settings.lines) line_count++;
    else {memmove(lines[0],lines[1],(size_t)(settings.lines-1)*COLS); cursor_row--;}
    memset(lines[line_count-1],' ',COLS); cursor_row=line_count-1;
    update_bar(); InvalidateRect(main_wnd,NULL,FALSE);
}
static void put_char(char c) {
    if(cursor_col>=COLS) {
        if(!settings.wrap) {cursor_col=COLS-1;}
        else {cursor_col=0; new_line();}
    }
    lines[cursor_row][cursor_col++]=c;
    invalidate_line(cursor_row);
}
static void erase(int row,int from,int to) {
    if(row<0 || row>=line_count) return;
    if(from<0) from=0;
    if(to>COLS) to=COLS;
    if(from<to) {memset(lines[row]+from,' ',(size_t)(to-from)); invalidate_line(row);}
}
static void move_to(int row,int col) {
    int screen_top=line_count-ROWS<0?0:line_count-ROWS;
    cursor_row=screen_top+max(0,min(ROWS-1,row)); cursor_col=max(0,min(COLS-1,col));
    if(cursor_row>=line_count) cursor_row=line_count-1;
}
static int param(int i,int def) {return i<param_count && params[i]?params[i]:def;}
static void csi(char final) {
    int screen_top=line_count-ROWS<0?0:line_count-ROWS,row=cursor_row-screen_top,i;
    switch(final) {
    case 'A': move_to(row-param(0,1),cursor_col); break;
    case 'B': move_to(row+param(0,1),cursor_col); break;
    case 'C': move_to(row,cursor_col+param(0,1)); break;
    case 'D': move_to(row,cursor_col-param(0,1)); break;
    case 'H': case 'f': move_to(param(0,1)-1,param(1,1)-1); break;
    case 'J':
        if(param(0,0)==0) {erase(cursor_row,cursor_col,COLS); for(i=cursor_row+1;i<line_count;i++) erase(i,0,COLS);}
        else if(param(0,0)==1) {erase(cursor_row,0,cursor_col+1); for(i=screen_top;i<cursor_row;i++) erase(i,0,COLS);}
        else for(i=screen_top;i<line_count;i++) erase(i,0,COLS);
        break;
    case 'K':
        if(param(0,0)==0) erase(cursor_row,cursor_col,COLS);
        else if(param(0,0)==1) erase(cursor_row,0,cursor_col+1);
        else erase(cursor_row,0,COLS);
        break;
    }
}
/* One character from the port, through the emulation. */
static void receive(char c) {
    if(state==ESCAPE) {
        state=PLAIN;
        switch(c) {
        case '[': state=CSI; param_count=0; memset(params,0,sizeof(params)); return;
        case 'D': new_line(); return;
        case 'E': cursor_col=0; new_line(); return;
        case 'M': if(cursor_row>0) cursor_row--; return;
        case 'c': clear_buffer(); return;
        }
        return;
    }
    if(state==CSI) {
        if(c>='0' && c<='9') {if(!param_count) param_count=1; params[param_count-1]=params[param_count-1]*10+(c-'0'); return;}
        if(c==';') {if(!param_count) param_count=1; if(param_count<8) param_count++; return;}
        if(c=='?') return;
        state=PLAIN; csi(c); return;
    }
    switch(c) {
    case 0x1b: if(settings.vt100) state=ESCAPE; return;
    case '\r': cursor_col=0; if(settings.inbound) new_line(); return;
    case '\n': new_line(); return;
    case '\b': if(cursor_col) cursor_col--; return;
    case '\t': cursor_col=min(COLS-1,(cursor_col/8+1)*8); return;
    case 7: if(settings.sound) MessageBeep(0); return;
    case 0: case 0x11: case 0x13: return;
    }
    if((BYTE)c>=' ') put_char(c);
}
static void received(const char *s,int n) {
    int i;
    if(capturing!=HFILE_ERROR && n) _lwrite(capturing,s,(UINT)n);
    for(i=0;i<n;i++) receive(s[i]);
    place_caret();
}
static void paint(HDC dc,const RECT *area) {
    int first=view_top(),y,line; HFONT old=(HFONT)SelectObject(dc,font);
    SetBkColor(dc,GetSysColor(COLOR_WINDOW)); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
    for(y=area->top/char_h;y*char_h<area->bottom;y++) {
        line=first+y;
        if(line<line_count) TextOut(dc,0,y*char_h,lines[line],COLS);
        else {RECT r; SetRect(&r,0,y*char_h,area->right,(y+1)*char_h); ExtTextOut(dc,0,0,ETO_OPAQUE,&r,NULL,0,NULL);}
    }
    SelectObject(dc,old);
}

/* --- typing ---------------------------------------------------------------------------- */
static void typed(char c) {
    if(c=='\r' && settings.outbound) {send("\r\n",2); if(settings.echo) received("\r\n",2); return;}
    send(&c,1);
    if(settings.echo) received(&c,1);
}
static void arrow(WPARAM vk) {
    static const char keys[4]={'A','B','D','C'}; char s[3];
    s[0]=0x1b; s[1]='['; s[2]=keys[vk==VK_UP?0:vk==VK_DOWN?1:vk==VK_LEFT?2:3];
    send(s,3);
}

/* --- settings files --------------------------------------------------------------------- */
static void set_title(void) {
    char title[300];
    wsprintf(title,"Terminal - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(main_wnd,title);
}
static BOOL load(LPCSTR path) {
    DWORD size; LPSTR data=ReadWholeFile(path,&size);
    if(!data) return FALSE;
    if(size!=sizeof(Settings) || memcmp(data,magic,sizeof(magic))) {GlobalFree(data); return FALSE;}
    memcpy(&settings,data,sizeof(settings)); GlobalFree(data);
    if(settings.lines<ROWS || settings.lines>MAX_LINES) settings.lines=100;
    lstrcpy(file,path); AnsiUpper(file); modified=FALSE; set_title();
    clear_buffer(); open_port(FALSE);
    return TRUE;
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.TRM");
    if(!FileSaveDialog(main_wnd,"Save As","*.TRM",path,sizeof(path))) return FALSE;
    if(!WriteWholeFile(path,&settings,sizeof(settings))) {MessageBox(main_wnd,"Cannot write the file.","Terminal",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    lstrcpy(file,path); AnsiUpper(file); modified=FALSE; set_title();
    return TRUE;
}
static BOOL save(void) {
    if(!file[0]) return save_as();
    if(!WriteWholeFile(file,&settings,sizeof(settings))) {MessageBox(main_wnd,"Cannot write the file.","Terminal",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    modified=FALSE; return TRUE;
}
static BOOL query_save(void) {
    char text[320];
    if(!modified) return TRUE;
    wsprintf(text,"The settings in %s have changed.\n\nDo you want to save the changes?",file[0]?FileTitle(file):"(Untitled)");
    switch(MessageBox(main_wnd,text,"Terminal",MB_YESNOCANCEL|MB_ICONEXCLAMATION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}

/* --- dialogs ------------------------------------------------------------------------------ */
static INT_PTR CALLBACK CommunicationsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int i;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        for(i=0;i<8 && bauds[i]!=settings.baud;i++) {}
        CheckRadioButton(dlg,IDC_BAUD,IDC_BAUD+7,IDC_BAUD+(i<8?i:6));
        CheckRadioButton(dlg,IDC_DATA,IDC_DATA+3,IDC_DATA+settings.data_bits-5);
        CheckRadioButton(dlg,IDC_STOP,IDC_STOP+2,IDC_STOP+settings.stop_bits);
        CheckRadioButton(dlg,IDC_PARITY,IDC_PARITY+4,IDC_PARITY+settings.parity);
        CheckRadioButton(dlg,IDC_FLOW,IDC_FLOW+2,IDC_FLOW+settings.flow);
        CheckRadioButton(dlg,IDC_PORT,IDC_PORT+4,IDC_PORT+settings.port);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        for(i=0;i<8;i++) if(IsDlgButtonChecked(dlg,IDC_BAUD+i)) settings.baud=bauds[i];
        for(i=0;i<4;i++) if(IsDlgButtonChecked(dlg,IDC_DATA+i)) settings.data_bits=5+i;
        for(i=0;i<3;i++) if(IsDlgButtonChecked(dlg,IDC_STOP+i)) settings.stop_bits=i;
        for(i=0;i<5;i++) if(IsDlgButtonChecked(dlg,IDC_PARITY+i)) settings.parity=i;
        for(i=0;i<3;i++) if(IsDlgButtonChecked(dlg,IDC_FLOW+i)) settings.flow=i;
        for(i=0;i<5;i++) if(IsDlgButtonChecked(dlg,IDC_PORT+i)) settings.port=i;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK EmulationProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {CheckRadioButton(dlg,IDC_TTY,IDC_VT100,settings.vt100?IDC_VT100:IDC_TTY); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {settings.vt100=IsDlgButtonChecked(dlg,IDC_VT100)!=0; EndDialog(dlg,IDOK); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK PreferencesProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL ok; UINT n;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        CheckDlgButton(dlg,IDC_WRAP,settings.wrap); CheckDlgButton(dlg,IDC_ECHO,settings.echo); CheckDlgButton(dlg,IDC_SOUND,settings.sound);
        CheckDlgButton(dlg,IDC_INBOUND,settings.inbound); CheckDlgButton(dlg,IDC_OUTBOUND,settings.outbound);
        SetDlgItemInt(dlg,IDC_LINES,(UINT)settings.lines,FALSE);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        n=GetDlgItemInt(dlg,IDC_LINES,&ok,FALSE);
        if(!ok || n<ROWS || n>MAX_LINES) {
            char text[80]; wsprintf(text,"Buffer Lines must be between %d and %d.",ROWS,MAX_LINES);
            MessageBox(dlg,text,"Terminal",MB_OK|MB_ICONEXCLAMATION); return TRUE;
        }
        settings.wrap=IsDlgButtonChecked(dlg,IDC_WRAP)!=0; settings.echo=IsDlgButtonChecked(dlg,IDC_ECHO)!=0;
        settings.sound=IsDlgButtonChecked(dlg,IDC_SOUND)!=0; settings.inbound=IsDlgButtonChecked(dlg,IDC_INBOUND)!=0;
        settings.outbound=IsDlgButtonChecked(dlg,IDC_OUTBOUND)!=0; settings.lines=(int)n;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK PhoneProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL ok; UINT n;
    (void)lp;
    if(msg==WM_INITDIALOG) {SetDlgItemText(dlg,IDC_NUMBER,settings.number); SetDlgItemInt(dlg,IDC_TIMEOUT,(UINT)settings.timeout,FALSE); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        n=GetDlgItemInt(dlg,IDC_TIMEOUT,&ok,FALSE);
        GetDlgItemText(dlg,IDC_NUMBER,settings.number,sizeof(settings.number));
        if(ok) settings.timeout=(int)n;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static INT_PTR CALLBACK ModemProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {
        SetDlgItemText(dlg,IDC_DIALPREFIX,settings.dial_prefix); SetDlgItemText(dlg,IDC_DIALSUFFIX,settings.dial_suffix);
        SetDlgItemText(dlg,IDC_HANGPREFIX,settings.hang_prefix); SetDlgItemText(dlg,IDC_HANGSUFFIX,settings.hang_suffix);
        SetDlgItemText(dlg,IDC_ORIGINATE,settings.originate);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        GetDlgItemText(dlg,IDC_DIALPREFIX,settings.dial_prefix,sizeof(settings.dial_prefix));
        GetDlgItemText(dlg,IDC_DIALSUFFIX,settings.dial_suffix,sizeof(settings.dial_suffix));
        GetDlgItemText(dlg,IDC_HANGPREFIX,settings.hang_prefix,sizeof(settings.hang_prefix));
        GetDlgItemText(dlg,IDC_HANGSUFFIX,settings.hang_suffix,sizeof(settings.hang_suffix));
        GetDlgItemText(dlg,IDC_ORIGINATE,settings.originate,sizeof(settings.originate));
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}
static void settings_dialog(HWND h,LPCSTR name,DLGPROC proc,BOOL reopen) {
    Settings before=settings;
    if(DialogBox(instance,name,h,proc)!=IDOK) return;
    if(memcmp(&before,&settings,sizeof(settings))) modified=TRUE;
    if(reopen) open_port(FALSE);
    if(settings.lines!=before.lines) clear_buffer();
}

/* --- the phone and transfers ------------------------------------------------------------------ */
static void modem_command(LPCSTR prefix,LPCSTR middle,LPCSTR suffix) {
    char text[96];
    if(comm<0 && !open_port(FALSE)) return;
    wsprintf(text,"%s%s%s\r",prefix,middle,suffix);
    send(text,lstrlen(text));
}
static void dial(void) {
    if(!settings.number[0] && DialogBox(instance,"PHONE",main_wnd,PhoneProc)!=IDOK) return;
    if(settings.originate[0]) modem_command(settings.originate,"","");
    modem_command(settings.dial_prefix,settings.number,settings.dial_suffix);
}
/* The hangup prefix (the modem's escape), then a second's guard time, then the command. */
static void hangup(void) {
    if(comm<0) return;
    send(settings.hang_prefix,lstrlen(settings.hang_prefix));
    SetTimer(main_wnd,HANGUP_TIMER,1000,NULL);
}
static void stop_transfer(void) {
    if(sending!=HFILE_ERROR) {_lclose(sending); sending=HFILE_ERROR;}
    if(capturing!=HFILE_ERROR) {_lclose(capturing); capturing=HFILE_ERROR;}
}
static void send_text(HWND h) {
    char path[260];
    lstrcpy(path,"*.TXT");
    if(!FileOpenDialog(h,"Send Text File","*.TXT",path,sizeof(path))) return;
    stop_transfer();
    if((sending=_lopen(path,OF_READ))==HFILE_ERROR) MessageBox(h,"Cannot open the file.","Terminal",MB_OK|MB_ICONEXCLAMATION);
}
static void receive_text(HWND h) {
    char path[260];
    lstrcpy(path,"*.TXT");
    if(!FileSaveDialog(h,"Receive Text File","*.TXT",path,sizeof(path))) return;
    stop_transfer();
    if((capturing=_lcreat(path,0))==HFILE_ERROR) MessageBox(h,"Cannot create the file.","Terminal",MB_OK|MB_ICONEXCLAMATION);
}
/* Each tick: what has arrived, and the next piece of a file being sent. */
static void tick(void) {
    char buffer[256]; int n;
    if(comm<0) return;
    while((n=ReadComm(comm,buffer,sizeof(buffer)))>0) received(buffer,n);
    if(n<0) {COMSTAT s; GetCommError(comm,&s);}
    if(sending!=HFILE_ERROR) {
        UINT got=_lread(sending,buffer,64);
        if(got==(UINT)HFILE_ERROR || !got) {_lclose(sending); sending=HFILE_ERROR;}
        else send(buffer,(int)got);
    }
}
static void paste(void) {
    HANDLE h; LPCSTR text;
    if(!OpenClipboard(main_wnd)) return;
    if((h=GetClipboardData(CF_TEXT))!=NULL && (text=(LPCSTR)GlobalLock(h))!=NULL) {
        send(text,lstrlen(text)); GlobalUnlock(h);
    }
    CloseClipboard();
}

/* --- the main window ---------------------------------------------------------------------------- */
static void command(HWND h,UINT id) {
    char path[260];
    if(HelpCommand(h,id,"TERMINAL.HLP")) return;
    switch(id) {
    case IDM_NEW:
        if(!query_save()) return;
        defaults(); file[0]=0; modified=FALSE; set_title(); clear_buffer(); open_port(TRUE);
        return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.TRM");
        if(FileOpenDialog(h,"Open","*.TRM",path,sizeof(path)) && !load(path))
            MessageBox(h,"The file is not a Terminal settings file.","Terminal",MB_OK|MB_ICONEXCLAMATION);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_PASTE: paste(); return;
    case IDM_CLEAR: clear_buffer(); return;
    case IDM_PHONE: settings_dialog(h,"PHONE",PhoneProc,FALSE); return;
    case IDM_EMULATION: settings_dialog(h,"EMULATION",EmulationProc,FALSE); return;
    case IDM_PREFERENCES: settings_dialog(h,"PREFERENCES",PreferencesProc,FALSE); return;
    case IDM_COMMUNICATIONS: settings_dialog(h,"COMMUNICATIONS",CommunicationsProc,TRUE); return;
    case IDM_MODEM: settings_dialog(h,"MODEM",ModemProc,FALSE); return;
    case IDM_DIAL: dial(); return;
    case IDM_HANGUP: hangup(); return;
    case IDM_SENDTEXT: send_text(h); return;
    case IDM_RECEIVETEXT: receive_text(h); return;
    case IDM_STOP: stop_transfer(); return;
    case IDM_ABOUT:
        MessageBox(h,"Terminal\n\nA communications terminal for the COM ports.","About Terminal",MB_OK|MB_ICONASTERISK);
        return;
    }
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        HDC dc=GetDC(h); TEXTMETRIC tm;
        main_wnd=h; font=(HFONT)GetStockObject(OEM_FIXED_FONT);
        SelectObject(dc,font); GetTextMetrics(dc,&tm); ReleaseDC(h,dc);
        char_w=tm.tmAveCharWidth; char_h=tm.tmHeight+tm.tmExternalLeading;
        return 0;
    }
    case WM_SIZE: update_bar(); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_SETFOCUS: CreateCaret(h,NULL,char_w,2); ShowCaret(h); place_caret(); return 0;
    case WM_KILLFOCUS: DestroyCaret(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        HideCaret(h); paint(dc,&ps.rcPaint); ShowCaret(h);
        EndPaint(h,&ps); return 0;
    }
    case WM_VSCROLL: {
        int pos=view_top(),lo,hi;
        GetScrollRange(h,SB_VERT,&lo,&hi);
        switch(LOWORD(wp)) {
        case SB_LINEUP: pos--; break;
        case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos-=ROWS; break;
        case SB_PAGEDOWN: pos+=ROWS; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: pos=(short)HIWORD(wp); break;
        }
        SetScrollPos(h,SB_VERT,max(lo,min(hi,pos)),TRUE); InvalidateRect(h,NULL,FALSE);
        return 0;
    }
    case WM_CHAR: typed((char)wp); return 0;
    case WM_KEYDOWN:
        if(wp==VK_UP || wp==VK_DOWN || wp==VK_LEFT || wp==VK_RIGHT) {if(settings.vt100) arrow(wp); return 0;}
        break;
    case WM_TIMER:
        if(wp==TIMER) tick();
        else if(wp==HANGUP_TIMER) {KillTimer(h,HANGUP_TIMER); modem_command(settings.hang_suffix,"","");}
        return 0;
    case WM_COMMAND: command(h,LOWORD(wp)); return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"TERMINAL.HLP",HELP_QUIT,0); stop_transfer(); close_port(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    instance=inst;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"TERMINAL"); wc.hCursor=LoadCursor(NULL,IDC_IBEAM);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="TERMINAL"; wc.lpszClassName="Terminal";
        if(!RegisterClass(&wc)) return 0;
    }
    if(!(lines=(char (*)[COLS])GlobalAlloc(GPTR,(DWORD)MAX_LINES*COLS))) return 0;
    defaults();
    main_wnd=CreateWindow("Terminal","Terminal",WS_OVERLAPPEDWINDOW|WS_VSCROLL,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    if(!main_wnd) return 0;
    set_title(); clear_buffer();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    while(command_line && *command_line==' ') command_line++;
    if(command_line && *command_line) {
        if(!load(command_line)) MessageBox(main_wnd,"The file is not a Terminal settings file.","Terminal",MB_OK|MB_ICONEXCLAMATION);
    } else open_port(TRUE);
    accel=LoadAccelerators(inst,"TERMINAL");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    GlobalFree(lines);
    return (int)m.wParam;
}
