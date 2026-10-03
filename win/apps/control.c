/* SPDX-License-Identifier: GPL-2.0-or-later
 * CONTROL: the Control Panel, Windows 3.0's in its settings and their
 * places in WIN.INI. Its icons open them: Color (schemes applied with
 * SetSysColors and kept in [colors]), Fonts (the font files of [fonts],
 * added to GDI and taken from it, with a sample), Ports ([ports]), Mouse
 * (the double-click speed, [windows] DoubleClickSpeed), Desktop, Printers
 * ([devices], [PrinterPorts], the default printer and the spooler),
 * International ([intl]), Keyboard (the repeat rate) and Date/Time, and
 * Sound (the warning beep). USER and GDI read the settings when Interface
 * Manager starts; programs read them from WIN.INI.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "../gdi/fontfile.h"
#include "control.h"
#define ITEM_W 80
#define PER_ROW 5
#define COLORS 21

static HINSTANCE instance;
static HWND wnd;
static int selected;
static int char_height;
/* WIN.INI [colors] keys, by COLOR_ index. */
static const char *const color_keys[COLORS]={
    "Scrollbar","Background","ActiveTitle","InactiveTitle","Menu","Window","WindowFrame","MenuText","WindowText",
    "TitleText","ActiveBorder","InactiveBorder","AppWorkspace","Hilight","HilightText","ButtonFace","ButtonShadow",
    "GrayText","ButtonText","InactiveTitleText","ButtonHilight"};
#define W RGB(255,255,255)
#define K RGB(0,0,0)
#define L RGB(192,192,192)
#define D RGB(128,128,128)
static const struct {const char *name; COLORREF c[COLORS];} schemes[]={
    {"Default",{L,L,RGB(0,0,128),W,W,W,K,K,K,W,L,L,RGB(255,255,232),RGB(0,0,128),W,L,D,D,K,K,W}},
    {"Arizona",{L,RGB(0,128,128),RGB(128,0,0),W,W,RGB(255,255,232),K,K,K,W,L,L,RGB(255,251,240),RGB(128,0,0),W,L,D,D,K,K,W}},
    {"Ocean",{L,RGB(0,0,128),RGB(0,128,128),W,RGB(192,220,255),W,K,K,RGB(0,0,128),W,RGB(0,128,128),L,RGB(192,220,255),RGB(0,128,128),W,L,D,D,K,K,W}},
    {"Emerald City",{L,RGB(0,64,0),RGB(0,128,0),W,W,W,K,K,K,W,RGB(0,128,0),L,RGB(224,255,224),RGB(0,128,0),W,L,D,D,K,K,W}},
    {"Monochrome",{L,D,K,W,W,W,K,K,K,W,L,L,W,K,W,L,D,D,K,K,W}},
};
#define SCHEMES ((int)(sizeof(schemes)/sizeof(schemes[0])))
static const struct {const char *name; COLORREF c;} desk_colors[]={
    {"Gray",RGB(192,192,192)},{"Teal",RGB(0,128,128)},{"Navy",RGB(0,0,128)},{"Dark Green",RGB(0,128,0)},
    {"Maroon",RGB(128,0,0)},{"Purple",RGB(128,0,128)},{"Black",RGB(0,0,0)},{"White",RGB(255,255,255)},
};
#define DESK_COLORS ((int)(sizeof(desk_colors)/sizeof(desk_colors[0])))

/* --- color ------------------------------------------------------------------------- */
static void save_color(int index,COLORREF c) {
    char text[16]; wsprintf(text,"%d %d %d",GetRValue(c),GetGValue(c),GetBValue(c));
    WriteProfileString("colors",color_keys[index],text);
}
static void apply_colors(const COLORREF *c) {
    int which[COLORS],i;
    for(i=0;i<COLORS;i++) {which[i]=i; save_color(i,c[i]);}
    SetSysColors(COLORS,which,c);
}
static void fill(HDC dc,int l,int t,int r,int b,COLORREF c) {
    RECT rc; HBRUSH br=CreateSolidBrush(c);
    SetRect(&rc,l,t,r,b); FillRect(dc,&rc,br); DeleteObject(br);
}
/* A small desktop with an inactive and an active window in the scheme's colors. */
static void preview(HDC dc,const RECT *r,const COLORREF *c) {
    int x=r->left,y=r->top,w=r->right-r->left,h=r->bottom-r->top,ax,ay,aw,ah;
    fill(dc,x,y,x+w,y+h,c[COLOR_BACKGROUND]);
    fill(dc,x+8,y+6,x+w*3/5,y+h/2,c[COLOR_WINDOWFRAME]);
    fill(dc,x+9,y+7,x+w*3/5-1,y+h/2-1,c[COLOR_INACTIVEBORDER]);
    fill(dc,x+11,y+9,x+w*3/5-3,y+24,c[COLOR_INACTIVECAPTION]);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,c[COLOR_INACTIVECAPTIONTEXT]); TextOut(dc,x+16,y+10,"Inactive",8);
    ax=x+w/4; ay=y+h/4; aw=w*3/4-8; ah=h*3/4-6;
    fill(dc,ax,ay,ax+aw,ay+ah,c[COLOR_WINDOWFRAME]);
    fill(dc,ax+1,ay+1,ax+aw-1,ay+ah-1,c[COLOR_ACTIVEBORDER]);
    fill(dc,ax+3,ay+3,ax+aw-3,ay+18,c[COLOR_ACTIVECAPTION]);
    SetTextColor(dc,c[COLOR_CAPTIONTEXT]); TextOut(dc,ax+8,ay+4,"Active",6);
    fill(dc,ax+3,ay+18,ax+aw-3,ay+34,c[COLOR_MENU]);
    SetTextColor(dc,c[COLOR_MENUTEXT]); TextOut(dc,ax+8,ay+19,"File",4);
    fill(dc,ax+50,ay+18,ax+92,ay+34,c[COLOR_HIGHLIGHT]);
    SetTextColor(dc,c[COLOR_HIGHLIGHTTEXT]); TextOut(dc,ax+54,ay+19,"Edit",4);
    fill(dc,ax+3,ay+34,ax+aw-3,ay+ah-3,c[COLOR_WINDOW]);
    SetTextColor(dc,c[COLOR_WINDOWTEXT]); TextOut(dc,ax+8,ay+38,"Window Text",11);
    fill(dc,ax+aw-60,ay+ah-24,ax+aw-10,ay+ah-8,c[COLOR_WINDOWFRAME]);
    fill(dc,ax+aw-59,ay+ah-23,ax+aw-11,ay+ah-9,c[COLOR_BTNFACE]);
    SetTextColor(dc,c[COLOR_BTNTEXT]); TextOut(dc,ax+aw-46,ay+ah-23,"OK",2);
}
static void preview_rect(HWND dlg,RECT *r) {SetRect(r,6,36,190,124); MapDialogRect(dlg,r);}
static INT_PTR CALLBACK ColorProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        int i; char current[64];
        for(i=0;i<SCHEMES;i++) SendDlgItemMessage(dlg,IDC_SCHEME,CB_ADDSTRING,0,(LPARAM)schemes[i].name);
        GetProfileString("Control Panel","CurrentScheme",schemes[0].name,current,sizeof(current));
        i=(int)SendDlgItemMessage(dlg,IDC_SCHEME,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)current);
        SendDlgItemMessage(dlg,IDC_SCHEME,CB_SETCURSEL,i<0?0:i,0);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(dlg,&ps); RECT r; int i=(int)SendDlgItemMessage(dlg,IDC_SCHEME,CB_GETCURSEL,0,0);
        preview_rect(dlg,&r);
        preview(dc,&r,schemes[i<0?0:i].c);
        EndPaint(dlg,&ps);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_SCHEME:
            if(HIWORD(wp)==CBN_SELCHANGE) {RECT r; preview_rect(dlg,&r); InvalidateRect(dlg,&r,FALSE);}
            return TRUE;
        case IDOK: {
            int i=(int)SendDlgItemMessage(dlg,IDC_SCHEME,CB_GETCURSEL,0,0);
            if(i<0) i=0;
            WriteProfileString("Control Panel","CurrentScheme",schemes[i].name);
            EndDialog(dlg,IDOK);
            apply_colors(schemes[i].c);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* --- date and time ---------------------------------------------------------------- */
static INT_PTR CALLBACK DateProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        SYSTEMTIME t; GetLocalTime(&t);
        SetDlgItemInt(dlg,IDC_YEAR,t.wYear,FALSE); SetDlgItemInt(dlg,IDC_MONTH,t.wMonth,FALSE); SetDlgItemInt(dlg,IDC_DAY,t.wDay,FALSE);
        SetDlgItemInt(dlg,IDC_HOUR,t.wHour,FALSE); SetDlgItemInt(dlg,IDC_MINUTE,t.wMinute,FALSE); SetDlgItemInt(dlg,IDC_SECOND,t.wSecond,FALSE);
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            SYSTEMTIME t; BOOL ok[6];
            memset(&t,0,sizeof(t));
            t.wYear=(WORD)GetDlgItemInt(dlg,IDC_YEAR,&ok[0],FALSE); t.wMonth=(WORD)GetDlgItemInt(dlg,IDC_MONTH,&ok[1],FALSE);
            t.wDay=(WORD)GetDlgItemInt(dlg,IDC_DAY,&ok[2],FALSE); t.wHour=(WORD)GetDlgItemInt(dlg,IDC_HOUR,&ok[3],FALSE);
            t.wMinute=(WORD)GetDlgItemInt(dlg,IDC_MINUTE,&ok[4],FALSE); t.wSecond=(WORD)GetDlgItemInt(dlg,IDC_SECOND,&ok[5],FALSE);
            if(!ok[0] || !ok[1] || !ok[2] || !ok[3] || !ok[4] || !ok[5] || !SetLocalTime(&t)) {
                MessageBox(dlg,"The date or time is not valid.","Date & Time",MB_OK|MB_ICONEXCLAMATION);
                return TRUE;
            }
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- mouse ------------------------------------------------------------------------------ */
static UINT speed_of(int pos) {return (UINT)(900-pos*80);}
static int pos_of(UINT ms) {int p=(int)(900-(int)ms+40)/80; return p<0?0:p>10?10:p;}
static LRESULT CALLBACK TestBoxProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_LBUTTONDBLCLK: SetWindowLong(h,0,!GetWindowLong(h,0)); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; HBRUSH b;
        GetClientRect(h,&r);
        b=CreateSolidBrush(GetWindowLong(h,0)?RGB(0,0,128):RGB(255,255,255));
        FillRect(dc,&r,b); DeleteObject(b);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetWindowLong(h,0)?RGB(255,255,255):RGB(0,0,0));
        DrawText(dc,"TEST",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(h,&ps);
        return 0;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}
static UINT original_speed;
static INT_PTR CALLBACK MouseProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_INITDIALOG:
        original_speed=GetDoubleClickTime();
        SetScrollRange(GetDlgItem(dlg,IDC_SPEED),SB_CTL,0,10,FALSE);
        SetScrollPos(GetDlgItem(dlg,IDC_SPEED),SB_CTL,pos_of(original_speed),TRUE);
        return TRUE;
    case WM_HSCROLL: {
        HWND bar=(HWND)lp; int pos=GetScrollPos(bar,SB_CTL);
        switch(LOWORD(wp)) {
        case SB_LINELEFT: pos--; break;
        case SB_LINERIGHT: pos++; break;
        case SB_PAGELEFT: pos-=3; break;
        case SB_PAGERIGHT: pos+=3; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos=(short)HIWORD(wp); break;
        case SB_LEFT: pos=0; break;
        case SB_RIGHT: pos=10; break;
        default: return TRUE;
        }
        pos=pos<0?0:pos>10?10:pos;
        SetScrollPos(bar,SB_CTL,pos,TRUE);
        SetDoubleClickTime(speed_of(pos));
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            char text[16]; wsprintf(text,"%u",GetDoubleClickTime());
            WriteProfileString("windows","DoubleClickSpeed",text);
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {SetDoubleClickTime(original_speed); EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- desktop ---------------------------------------------------------------------------- */
static INT_PTR CALLBACK DesktopProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        int i,sel=0; COLORREF now=GetSysColor(COLOR_BACKGROUND);
        for(i=0;i<DESK_COLORS;i++) {
            SendDlgItemMessage(dlg,IDC_DESKCOLOR,CB_ADDSTRING,0,(LPARAM)desk_colors[i].name);
            if(desk_colors[i].c==now) sel=i;
        }
        SendDlgItemMessage(dlg,IDC_DESKCOLOR,CB_SETCURSEL,sel,0);
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            int which=COLOR_BACKGROUND,i=(int)SendDlgItemMessage(dlg,IDC_DESKCOLOR,CB_GETCURSEL,0,0);
            EndDialog(dlg,IDOK);
            if(i>=0) {save_color(COLOR_BACKGROUND,desk_colors[i].c); SetSysColors(1,&which,&desk_colors[i].c);}
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- fonts ----------------------------------------------------------------------------- */
/* The fonts listed: WIN.INI [fonts]'s entries (a description and its file,
 * whose face shows the sample), then the faces built into GDI. */
#define FONT_ENTRIES 64
#define FILE_FONTS 32
typedef struct {char key[96],file[MAX_PATH],face[LF_FACESIZE]; int height; BOOL builtin;} FontEntry;
static FontEntry font_list[FONT_ENTRIES]; static int font_count;
static const char sample_text[]="AaBbCcXxYyZz 123";
static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
static void *font_bits(DWORD n) {return (void *)GlobalAlloc(GPTR,n);}
/* A font file's fonts (their bits freed); how many. */
static int file_fonts(const BYTE *data,DWORD size,FileFont *fonts) {
    int n=ReadFontFile(data,size,fonts,FILE_FONTS,font_bits),i;
    for(i=0;i<n;i++) {GlobalFree((HGLOBAL)fonts[i].bits); fonts[i].bits=NULL;}
    return n;
}
/* A .FON's description, as [fonts] keys it: the module's description
 * ("FONTRES 100,96,96 : Helv 8,10,12") after its colon; else the face and
 * its point sizes. */
static void font_description(const BYTE *d,DWORD n,const FileFont *fonts,int count,char *out,int size) {
    DWORD ne,table; int i,len;
    out[0]=0;
    if(n>0x40 && d[0]=='M' && d[1]=='Z' && (ne=get32(d+0x3c))<n-0x40 && d[ne]=='N' && d[ne+1]=='E' &&
       (table=get32(d+ne+0x2c))<n && (len=d[table])!=0 && table+1+len<=n) {
        char text[256]; const char *t;
        memcpy(text,d+table+1,(size_t)len); text[len]=0;
        t=strchr(text,':')?strchr(text,':')+1:text;
        while(*t==' ') t++;
        lstrcpyn(out,t,size);
    }
    if(!out[0] && count) {
        lstrcpyn(out,fonts[0].name,size);
        for(i=0;i<count && lstrlen(out)+8<size;i++) wsprintf(out+lstrlen(out),"%s%d",i?",":" ",fonts[i].points);
    }
}
/* The sample's size: the file's largest font that fits the sample's box. */
#define SAMPLE_MAX 44
static int sample_height(const FileFont *fonts,int count) {
    int i,best=0;
    for(i=0;i<count;i++) if(fonts[i].height<=SAMPLE_MAX && fonts[i].height>best) best=fonts[i].height;
    if(!best) for(i=0;i<count;i++) if(!best || fonts[i].height<best) best=fonts[i].height;
    return best;
}
static int CALLBACK builtin_face(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    int i; (void)tm; (void)type; (void)lp;
    for(i=0;i<font_count;i++) if(!lstrcmpi(font_list[i].face,lf->lfFaceName)) return 1;
    if(font_count<FONT_ENTRIES) {
        FontEntry *e=&font_list[font_count++];
        memset(e,0,sizeof(*e)); e->builtin=TRUE;
        lstrcpyn(e->face,lf->lfFaceName,sizeof(e->face)); wsprintf(e->key,"%s (built in)",lf->lfFaceName);
    }
    return 1;
}
static void read_fonts(HWND dlg) {
    char keys[2048]; const char *k; int n; HDC dc;
    font_count=0;
    n=GetProfileString("fonts",NULL,"",keys,sizeof(keys)-1); keys[n]=keys[n+1]=0;
    for(k=keys;*k && font_count<FONT_ENTRIES;k+=lstrlen(k)+1) {
        FontEntry *e=&font_list[font_count]; OFSTRUCT of; DWORD size; LPSTR data;
        memset(e,0,sizeof(*e)); lstrcpyn(e->key,k,sizeof(e->key));
        GetProfileString("fonts",k,"",e->file,sizeof(e->file));
        if(OpenFile(e->file,&of,OF_EXIST)!=HFILE_ERROR && (data=ReadWholeFile(of.szPathName,&size))!=NULL) {
            FileFont fonts[FILE_FONTS]; int count=file_fonts((const BYTE *)data,size,fonts);
            if(count) {lstrcpyn(e->face,fonts[0].name,sizeof(e->face)); e->height=sample_height(fonts,count);}
            GlobalFree((HGLOBAL)data);
        }
        font_count++;
    }
    dc=GetDC(dlg); EnumFonts(dc,NULL,(FONTENUMPROC)builtin_face,0); ReleaseDC(dlg,dc);
    SendDlgItemMessage(dlg,IDC_FONTS,LB_RESETCONTENT,0,0);
    for(n=0;n<font_count;n++) SendDlgItemMessage(dlg,IDC_FONTS,LB_ADDSTRING,0,(LPARAM)font_list[n].key);
}
static int font_selected(HWND dlg) {return (int)SendDlgItemMessage(dlg,IDC_FONTS,LB_GETCURSEL,0,0);}
static void sample_rect(HWND dlg,RECT *r) {SetRect(r,6,96,214,126); MapDialogRect(dlg,r);}
static void show_font(HWND dlg) {
    RECT r; int i=font_selected(dlg);
    EnableWindow(GetDlgItem(dlg,IDC_REMOVEFONT),i>=0 && i<font_count && !font_list[i].builtin);
    sample_rect(dlg,&r); InvalidateRect(dlg,&r,TRUE);
}
static void font_changed(void) {SendMessage(HWND_BROADCAST,WM_FONTCHANGE,0,0);}
/* A font file added: copied to the system directory (when it is not there),
 * given to GDI and entered in [fonts]. */
static void add_font(HWND dlg) {
    char path[MAX_PATH],target[MAX_PATH],description[96],text[MAX_PATH+64],existing[8]; DWORD size; LPSTR data;
    FileFont fonts[FILE_FONTS]; int count,i;
    lstrcpy(path,"*.FON");
    if(!FileOpenDialog(dlg,"Add Font Files","*.FON",path,sizeof(path))) return;
    if(!(data=ReadWholeFile(path,&size)) || !(count=file_fonts((const BYTE *)data,size,fonts))) {
        wsprintf(text,"%s is not a font file.",FileTitle(path));
        MessageBox(dlg,text,"Fonts",MB_OK|MB_ICONEXCLAMATION);
        if(data) GlobalFree((HGLOBAL)data);
        return;
    }
    font_description((const BYTE *)data,size,fonts,count,description,sizeof(description));
    if(GetProfileString("fonts",description,"",existing,sizeof(existing))) {
        MessageBox(dlg,"The font is already installed.","Fonts",MB_OK|MB_ICONINFORMATION);
        GlobalFree((HGLOBAL)data); return;
    }
    i=(int)GetSystemDirectory(target,sizeof(target));
    if(i && target[i-1]!='\\') lstrcat(target,"\\");
    lstrcat(target,FileTitle(path));
    if(lstrcmpi(target,path) && !WriteWholeFile(target,data,size)) {
        wsprintf(text,"%s could not be copied to the system directory.",FileTitle(path));
        MessageBox(dlg,text,"Fonts",MB_OK|MB_ICONEXCLAMATION);
        GlobalFree((HGLOBAL)data); return;
    }
    GlobalFree((HGLOBAL)data);
    if(!AddFontResource(target)) {MessageBox(dlg,"The font could not be added.","Fonts",MB_OK|MB_ICONEXCLAMATION); return;}
    WriteProfileString("fonts",description,FileTitle(target));
    font_changed();
    read_fonts(dlg);
    for(i=0;i<font_count && lstrcmpi(font_list[i].key,description);i++) {}
    SendDlgItemMessage(dlg,IDC_FONTS,LB_SETCURSEL,i<font_count?i:0,0);
    show_font(dlg);
}
/* An installed font removed from GDI and [fonts]; its file stays. */
static void remove_font(HWND dlg) {
    int i=font_selected(dlg); char text[160];
    if(i<0 || i>=font_count || font_list[i].builtin) return;
    wsprintf(text,"Remove the font %s?",(LPSTR)font_list[i].key);
    if(MessageBox(dlg,text,"Fonts",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
    RemoveFontResource(font_list[i].file);
    WriteProfileString("fonts",font_list[i].key,NULL);
    font_changed();
    read_fonts(dlg);
    SendDlgItemMessage(dlg,IDC_FONTS,LB_SETCURSEL,min(i,font_count-1),0);
    show_font(dlg);
}
static INT_PTR CALLBACK FontsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        read_fonts(dlg);
        SendDlgItemMessage(dlg,IDC_FONTS,LB_SETCURSEL,0,0);
        show_font(dlg);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(dlg,&ps); RECT r; int i=font_selected(dlg);
        sample_rect(dlg,&r);
        FillRect(dc,&r,(HBRUSH)(COLOR_WINDOW+1)); FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(i>=0 && i<font_count) {
            HFONT f=CreateFont(font_list[i].height,0,0,0,FW_NORMAL,0,0,0,0,0,0,0,0,font_list[i].face),old=(HFONT)SelectObject(dc,f);
            SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
            InflateRect(&r,-4,-2);
            DrawText(dc,sample_text,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_NOPREFIX);
            SelectObject(dc,old); DeleteObject(f);
        }
        EndPaint(dlg,&ps);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_FONTS: if(HIWORD(wp)==LBN_SELCHANGE) show_font(dlg); return TRUE;
        case IDC_ADDFONT: add_font(dlg); return TRUE;
        case IDC_REMOVEFONT: remove_font(dlg); return TRUE;
        case IDOK: case IDCANCEL: EndDialog(dlg,IDOK); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* --- ports ------------------------------------------------------------------------------- */
/* [ports]: "COM1:=9600,n,8,1,x", the rate, parity, data bits, stop bits
 * and the flow control (x Xon/Xoff, p hardware, none without). Print
 * Manager sets a serial port so before it prints to it. */
#define COM_PORTS 4
static const char *const rates[]={"110","300","600","1200","2400","4800","9600","19200"};
static const char *const data_bits[]={"4","5","6","7","8"};
static const char *const parities[]={"Even","Odd","None","Mark","Space"};
static const char parity_codes[]="eonms";
static const char *const stop_bits[]={"1","1.5","2"};
static const char *const flows[]={"Xon/Xoff","Hardware","None"};
#define COUNT(a) ((int)(sizeof(a)/sizeof(a[0])))
static int port_index;
static int number(LPCSTR s) {int v=0; while(*s>='0' && *s<='9') v=v*10+(*s++-'0'); return v;}
static void fill_combo(HWND dlg,int id,const char *const *names,int n,LPCSTR current) {
    int i,sel=0;
    for(i=0;i<n;i++) {SendDlgItemMessage(dlg,id,CB_ADDSTRING,0,(LPARAM)names[i]); if(!lstrcmpi(names[i],current)) sel=i;}
    SendDlgItemMessage(dlg,id,CB_SETCURSEL,sel,0);
}
static int combo(HWND dlg,int id) {int i=(int)SendDlgItemMessage(dlg,id,CB_GETCURSEL,0,0); return i<0?0:i;}
/* The fields of a [ports] value, by commas. */
static void field(LPCSTR s,int n,char *out,int size) {
    int k=0;
    while(n-- && *s) {while(*s && *s!=',') s++; if(*s) s++;}
    while(*s==' ') s++;
    while(*s && *s!=',' && k<size-1) out[k++]=*s++;
    while(k && out[k-1]==' ') k--;
    out[k]=0;
}
static INT_PTR CALLBACK PortSettingsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    char port[8],value[64],f[5][12],title[32]; int i;
    (void)lp;
    wsprintf(port,"COM%d:",port_index+1);
    switch(msg) {
    case WM_INITDIALOG:
        wsprintf(title,"Ports - %s",(LPSTR)port); SetWindowText(dlg,title);
        GetProfileString("ports",port,"9600,n,8,1",value,sizeof(value));
        for(i=0;i<5;i++) field(value,i,f[i],sizeof(f[i]));
        for(i=0;i<COUNT(parities) && (f[1][0]|0x20)!=parity_codes[i];i++) {}
        fill_combo(dlg,IDC_BAUD,rates,COUNT(rates),f[0][0]?f[0]:"9600");
        fill_combo(dlg,IDC_DATABITS,data_bits,COUNT(data_bits),f[2][0]?f[2]:"8");
        fill_combo(dlg,IDC_PARITY,parities,COUNT(parities),parities[i<COUNT(parities)?i:2]);
        fill_combo(dlg,IDC_STOPBITS,stop_bits,COUNT(stop_bits),f[3][0]?f[3]:"1");
        fill_combo(dlg,IDC_FLOW,flows,COUNT(flows),(f[4][0]|0x20)=='x'?flows[0]:(f[4][0]|0x20)=='p'?flows[1]:flows[2]);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            int flow=combo(dlg,IDC_FLOW);
            wsprintf(value,"%s,%c,%s,%s%s",(LPSTR)rates[combo(dlg,IDC_BAUD)],parity_codes[combo(dlg,IDC_PARITY)],
                     (LPSTR)data_bits[combo(dlg,IDC_DATABITS)],(LPSTR)stop_bits[combo(dlg,IDC_STOPBITS)],(LPSTR)(flow==0?",x":flow==1?",p":""));
            WriteProfileString("ports",port,value);
            SendMessage(HWND_BROADCAST,WM_WININICHANGE,0,(LPARAM)"ports");
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK PortsProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int i; char name[8];
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        for(i=0;i<COM_PORTS;i++) {wsprintf(name,"COM%d:",i+1); SendDlgItemMessage(dlg,IDC_PORTLIST,LB_ADDSTRING,0,(LPARAM)name);}
        SendDlgItemMessage(dlg,IDC_PORTLIST,LB_SETCURSEL,0,0);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_PORTLIST: if(HIWORD(wp)!=LBN_DBLCLK) return TRUE;
            /* fall through */
        case IDC_PORTSETTINGS: case IDOK:
            port_index=(int)SendDlgItemMessage(dlg,IDC_PORTLIST,LB_GETCURSEL,0,0);
            if(port_index>=0) DialogBox(instance,"PORTSET",dlg,PortSettingsProc);
            return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* --- printers ---------------------------------------------------------------------------- */
/* The printers: [devices] "PostScript Printer=PSCRIPT,LPT1:", [PrinterPorts]
 * with the timeouts ("PSCRIPT,LPT1:,15,45"), [windows] device= the default
 * one and spooler= whether Print Manager prints. Changed in the dialog,
 * written on OK. PostScript Printer is the driver there is. */
#define PRINTERS 16
typedef struct {char name[64],driver[16],port[16]; int not_selected,retry;} PrinterEntry;
static PrinterEntry printers[PRINTERS]; static int printer_count,default_printer,printer_index;
static BOOL use_spooler;
static void read_printers(void) {
    char keys[1024],value[128],device[160]; const char *k; int n;
    printer_count=0; default_printer=-1;
    n=GetProfileString("devices",NULL,"",keys,sizeof(keys)-1); keys[n]=keys[n+1]=0;
    GetProfileString("windows","device","",device,sizeof(device));
    for(k=keys;*k && printer_count<PRINTERS;k+=lstrlen(k)+1) {
        PrinterEntry *p=&printers[printer_count]; char t[16];
        memset(p,0,sizeof(*p)); lstrcpyn(p->name,k,sizeof(p->name));
        GetProfileString("devices",k,"",value,sizeof(value));
        field(value,0,p->driver,sizeof(p->driver)); field(value,1,p->port,sizeof(p->port));
        GetProfileString("PrinterPorts",k,"",value,sizeof(value));
        field(value,2,t,sizeof(t)); p->not_selected=t[0]?number(t):15;
        field(value,3,t,sizeof(t)); p->retry=t[0]?number(t):45;
        if(!p->driver[0]) lstrcpy(p->driver,"PSCRIPT");
        if(!p->port[0]) lstrcpy(p->port,"LPT1:");
        {int l=lstrlen(k); if(!memcmp(device,k,(size_t)l) && device[l]==',') default_printer=printer_count;}
        printer_count++;
    }
    GetProfileString("windows","spooler","yes",value,sizeof(value));
    use_spooler=lstrcmpi(value,"no")!=0;
}
static void write_printers(void) {
    char keys[1024],value[128]; const char *k; int n,i;
    n=GetProfileString("devices",NULL,"",keys,sizeof(keys)-1); keys[n]=keys[n+1]=0;
    for(k=keys;*k;k+=lstrlen(k)+1) {
        for(i=0;i<printer_count && lstrcmpi(printers[i].name,k);i++) {}
        if(i==printer_count) {WriteProfileString("devices",k,NULL); WriteProfileString("PrinterPorts",k,NULL);}
    }
    for(i=0;i<printer_count;i++) {
        const PrinterEntry *p=&printers[i];
        wsprintf(value,"%s,%s",(LPSTR)p->driver,(LPSTR)p->port); WriteProfileString("devices",p->name,value);
        wsprintf(value,"%s,%s,%d,%d",(LPSTR)p->driver,(LPSTR)p->port,p->not_selected,p->retry); WriteProfileString("PrinterPorts",p->name,value);
    }
    if(default_printer>=0 && default_printer<printer_count) {
        const PrinterEntry *p=&printers[default_printer];
        wsprintf(value,"%s,%s,%s",(LPSTR)p->name,(LPSTR)p->driver,(LPSTR)p->port);
        WriteProfileString("windows","device",value);
    } else WriteProfileString("windows","device",NULL);
    WriteProfileString("windows","spooler",use_spooler?"yes":"no");
    SendMessage(HWND_BROADCAST,WM_WININICHANGE,0,(LPARAM)"devices");
    SendMessage(HWND_BROADCAST,WM_WININICHANGE,0,(LPARAM)"windows");
}
static void show_printers(HWND dlg) {
    char text[100]; int i,sel=(int)SendDlgItemMessage(dlg,IDC_PRINTERS,LB_GETCURSEL,0,0);
    SendDlgItemMessage(dlg,IDC_PRINTERS,LB_RESETCONTENT,0,0);
    for(i=0;i<printer_count;i++) {
        wsprintf(text,"%s on %s",(LPSTR)printers[i].name,(LPSTR)printers[i].port);
        SendDlgItemMessage(dlg,IDC_PRINTERS,LB_ADDSTRING,0,(LPARAM)text);
    }
    if(sel<0 || sel>=printer_count) sel=printer_count?0:-1;
    SendDlgItemMessage(dlg,IDC_PRINTERS,LB_SETCURSEL,sel,0);
    if(default_printer>=0) wsprintf(text,"%s on %s",(LPSTR)printers[default_printer].name,(LPSTR)printers[default_printer].port);
    else lstrcpy(text,"(none)");
    SetDlgItemText(dlg,IDC_DEFAULTPRN,text);
    EnableWindow(GetDlgItem(dlg,IDC_CONFIGURE),sel>=0);
    EnableWindow(GetDlgItem(dlg,IDC_SETDEFAULT),sel>=0);
    EnableWindow(GetDlgItem(dlg,IDC_REMOVEPRN),sel>=0);
}
static INT_PTR CALLBACK ConfigureProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    PrinterEntry *p=&printers[printer_index]; char keys[512]; const char *k; int n,i;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_PRNNAME,p->name);
        n=GetProfileString("ports",NULL,"",keys,sizeof(keys)-1); keys[n]=keys[n+1]=0;
        for(k=keys;*k;k+=lstrlen(k)+1) {
            i=(int)SendDlgItemMessage(dlg,IDC_PRNPORTS,LB_ADDSTRING,0,(LPARAM)k);
            if(!lstrcmpi(k,p->port)) SendDlgItemMessage(dlg,IDC_PRNPORTS,LB_SETCURSEL,i,0);
        }
        SetDlgItemInt(dlg,IDC_NOTSELECTED,p->not_selected,FALSE); SetDlgItemInt(dlg,IDC_RETRY,p->retry,FALSE);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_PRNSETUP: {
            /* The driver's settings, through Print Setup with this printer chosen. */
            PRINTDLG pd; HGLOBAL names; DEVNAMES *dn; int at=(int)sizeof(DEVNAMES);
            memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=dlg; pd.Flags=PD_PRINTSETUP;
            if((names=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,sizeof(DEVNAMES)+200))!=NULL && (dn=(DEVNAMES *)GlobalLock(names))!=NULL) {
                char *b=(char *)dn;
                dn->wDriverOffset=(WORD)at; lstrcpy(b+at,p->driver); at+=lstrlen(p->driver)+1;
                dn->wDeviceOffset=(WORD)at; lstrcpy(b+at,p->name); at+=lstrlen(p->name)+1;
                dn->wOutputOffset=(WORD)at; lstrcpy(b+at,p->port);
                GlobalUnlock(names); pd.hDevNames=names;
            }
            PrintDlg(&pd);
            if(pd.hDevMode) GlobalFree(pd.hDevMode);
            if(pd.hDevNames) GlobalFree(pd.hDevNames);
            return TRUE;
        }
        case IDOK: {
            BOOL ok1,ok2; int a,b;
            i=(int)SendDlgItemMessage(dlg,IDC_PRNPORTS,LB_GETCURSEL,0,0);
            if(i>=0) SendDlgItemMessage(dlg,IDC_PRNPORTS,LB_GETTEXT,i,(LPARAM)p->port);
            a=(int)GetDlgItemInt(dlg,IDC_NOTSELECTED,&ok1,FALSE); b=(int)GetDlgItemInt(dlg,IDC_RETRY,&ok2,FALSE);
            if(ok1) p->not_selected=a;
            if(ok2) p->retry=b;
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK AddPrinterProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SendDlgItemMessage(dlg,IDC_DRIVERS,LB_ADDSTRING,0,(LPARAM)"PostScript Printer");
        SendDlgItemMessage(dlg,IDC_DRIVERS,LB_SETCURSEL,0,0);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_DRIVERS: if(HIWORD(wp)!=LBN_DBLCLK) return TRUE;
            /* fall through */
        case IDOK: {
            int i;
            for(i=0;i<printer_count && lstrcmpi(printers[i].name,"PostScript Printer");i++) {}
            if(i<printer_count) {MessageBox(dlg,"PostScript Printer is already installed.","Printers",MB_OK|MB_ICONINFORMATION); return TRUE;}
            if(printer_count==PRINTERS) return TRUE;
            memset(&printers[i],0,sizeof(printers[i]));
            lstrcpy(printers[i].name,"PostScript Printer"); lstrcpy(printers[i].driver,"PSCRIPT"); lstrcpy(printers[i].port,"LPT1:");
            printers[i].not_selected=15; printers[i].retry=45;
            printer_count++;
            if(default_printer<0) default_printer=i;
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK PrintersProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int sel;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        read_printers();
        show_printers(dlg);
        if(default_printer>=0) SendDlgItemMessage(dlg,IDC_PRINTERS,LB_SETCURSEL,default_printer,0);
        CheckDlgButton(dlg,IDC_SPOOLER,use_spooler);
        return TRUE;
    case WM_COMMAND:
        sel=(int)SendDlgItemMessage(dlg,IDC_PRINTERS,LB_GETCURSEL,0,0);
        switch(LOWORD(wp)) {
        case IDC_PRINTERS: if(HIWORD(wp)==LBN_SELCHANGE) show_printers(dlg); if(HIWORD(wp)!=LBN_DBLCLK) return TRUE;
            /* fall through: a double click configures */
        case IDC_CONFIGURE:
            if(sel>=0) {printer_index=sel; DialogBox(instance,"PRNCONFIG",dlg,ConfigureProc); show_printers(dlg);}
            return TRUE;
        case IDC_SETDEFAULT: if(sel>=0) {default_printer=sel; show_printers(dlg);} return TRUE;
        case IDC_REMOVEPRN:
            if(sel>=0) {
                char text[120]; wsprintf(text,"Remove %s?",(LPSTR)printers[sel].name);
                if(MessageBox(dlg,text,"Printers",MB_YESNO|MB_ICONQUESTION)!=IDYES) return TRUE;
                memmove(&printers[sel],&printers[sel+1],(size_t)(printer_count-sel-1)*sizeof(PrinterEntry)); printer_count--;
                if(default_printer==sel) default_printer=printer_count?0:-1;
                else if(default_printer>sel) default_printer--;
                show_printers(dlg);
            }
            return TRUE;
        case IDC_ADDPRN: if(DialogBox(instance,"PRNADD",dlg,AddPrinterProc)==IDOK) show_printers(dlg); return TRUE;
        case IDOK: use_spooler=IsDlgButtonChecked(dlg,IDC_SPOOLER)!=0; write_printers(); EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}
/* --- international ----------------------------------------------------------------------- */
/* [intl]: the country and its formats as Windows 3.0 keeps them (iCountry,
 * sCountry, sLanguage, iMeasure, sList; iDate, sDate, sShortDate, sLongDate;
 * iTime, sTime, s1159, s2359, iTLZero; sCurrency, iCurrency, iNegCurr,
 * iCurrDigits; sThousand, sDecimal, iDigits, iLzero). A country brings its
 * own; the Change buttons alter a format. Programs read them. */
typedef struct {
    char country[32]; int code,language,measure; char list[4];
    int date,day_zero,month_zero,century; char date_sep[4];
    int time24,time_zero; char time_sep[4],am[8],pm[8];
    char currency[8]; int currency_place,negative,currency_digits;
    char thousand[4],decimal[4]; int digits,zero;
} Intl;
static const struct {const char *name,*code;} languages[]={
    {"English (American)","enu"},{"English (International)","eng"},{"Danish","dan"},{"Dutch","nld"},{"Finnish","fin"},
    {"French","fra"},{"German","deu"},{"Italian","ita"},{"Japanese","jpn"},{"Norwegian","nor"},{"Portuguese","ptg"},
    {"Spanish","esp"},{"Swedish","sve"},
};
/* name, code, language, measure (0 metric), list, date order (0 MDY 1 DMY 2 YMD), day and month
 * leading zeros, century, separator, 24-hour, hour zero, time separator, AM, PM, currency,
 * its place, negative form, digits, thousands, decimal, digits, leading zero. */
static const Intl countries[]={
    {"Australia",61,1,0,",",1,1,1,1,"/",0,0,":","AM","PM","$",0,1,2,",",".",2,1},
    {"Canada",2,0,0,",",2,1,1,1,"-",1,1,":","","","$",0,1,2,",",".",2,1},
    {"Denmark",45,2,0,";",1,1,1,1,"-",1,1,".","","","kr",2,8,2,".",",",2,1},
    {"Finland",358,4,0,";",1,0,0,1,".",1,0,".","","","mk",3,8,2," ",",",2,1},
    {"France",33,5,0,";",1,1,1,1,"/",1,1,":","","","F",3,8,2," ",",",2,1},
    {"Germany",49,6,0,";",1,1,1,1,".",1,1,":","","","DM",3,8,2,".",",",2,1},
    {"Italy",39,7,0,";",1,1,1,1,"/",1,1,".","","","L.",2,9,0,".",",",2,1},
    {"Japan",81,8,0,",",2,1,1,1,"/",1,1,":","","","\\",0,1,0,",",".",2,1},
    {"Netherlands",31,3,0,";",1,1,1,1,"-",1,1,":","","","f",2,11,2,".",",",2,1},
    {"Norway",47,9,0,";",1,1,1,1,".",1,1,":","","","Kr",2,2,2,".",",",2,1},
    {"Portugal",351,10,0,";",1,1,1,1,"-",1,1,":","","","Esc.",3,8,2,".",",",2,1},
    {"Spain",34,11,0,";",1,1,1,1,"/",1,1,":","","","Pts",1,8,0,".",",",2,1},
    {"Sweden",46,12,0,";",2,1,1,1,"-",1,1,".","","","kr",3,8,2,".",",",2,1},
    {"Switzerland",41,6,0,";",1,1,1,1,".",1,1,".","","","Fr.",2,9,2,"'",".",2,1},
    {"United Kingdom",44,1,0,",",1,1,1,1,"/",1,1,":","","","\xa3",0,1,2,",",".",2,1},
    {"United States",1,0,1,",",0,0,0,0,"/",0,0,":","AM","PM","$",0,0,2,",",".",2,1},
};
static Intl intl;
static int intl_country(int code) {
    int i;
    for(i=0;i<COUNT(countries);i++) if(countries[i].code==code) return i;
    return COUNT(countries)-1;
}
/* A string of [intl], the country's own when there is none. */
static void intl_text(LPCSTR key,char *value,int size) {
    char def[32]; lstrcpyn(def,value,sizeof(def));
    GetProfileString("intl",key,def,value,size);
}
static void read_intl(void) {
    char lang[8]; int i;
    intl=countries[intl_country(GetProfileInt("intl","iCountry",1))];
    intl_text("sCountry",intl.country,sizeof(intl.country));
    GetProfileString("intl","sLanguage",languages[intl.language].code,lang,sizeof(lang));
    for(i=0;i<COUNT(languages);i++) if(!lstrcmpi(lang,languages[i].code)) intl.language=i;
    intl.measure=GetProfileInt("intl","iMeasure",intl.measure);
    intl_text("sList",intl.list,sizeof(intl.list));
    intl.date=GetProfileInt("intl","iDate",intl.date);
    intl_text("sDate",intl.date_sep,sizeof(intl.date_sep));
    {
        char shortdate[32]; const char *p;
        if(GetProfileString("intl","sShortDate","",shortdate,sizeof(shortdate))) {
            int d=0,m=0,y=0;
            for(p=shortdate;*p;p++) {if(*p=='d') d++; else if(*p=='M') m++; else if(*p=='y') y++;}
            intl.day_zero=d>=2; intl.month_zero=m>=2; intl.century=y>=4;
        }
    }
    intl.time24=GetProfileInt("intl","iTime",intl.time24);
    intl.time_zero=GetProfileInt("intl","iTLZero",intl.time_zero);
    intl_text("sTime",intl.time_sep,sizeof(intl.time_sep));
    intl_text("s1159",intl.am,sizeof(intl.am));
    intl_text("s2359",intl.pm,sizeof(intl.pm));
    intl_text("sCurrency",intl.currency,sizeof(intl.currency));
    intl.currency_place=GetProfileInt("intl","iCurrency",intl.currency_place);
    intl.negative=GetProfileInt("intl","iNegCurr",intl.negative);
    intl.currency_digits=GetProfileInt("intl","iCurrDigits",intl.currency_digits);
    intl_text("sThousand",intl.thousand,sizeof(intl.thousand));
    intl_text("sDecimal",intl.decimal,sizeof(intl.decimal));
    intl.digits=GetProfileInt("intl","iDigits",intl.digits);
    intl.zero=GetProfileInt("intl","iLzero",intl.zero);
}
/* The short date's picture ("M/d/yy"), the long one's ("dddd, MMMM dd, yyyy"). */
static void short_picture(const Intl *t,char *out) {
    char d[3],m[3],y[5]; LPCSTR s=t->date_sep;
    lstrcpy(d,t->day_zero?"dd":"d"); lstrcpy(m,t->month_zero?"MM":"M"); lstrcpy(y,t->century?"yyyy":"yy");
    if(t->date==0) wsprintf(out,"%s%s%s%s%s",(LPSTR)m,s,(LPSTR)d,s,(LPSTR)y);
    else if(t->date==1) wsprintf(out,"%s%s%s%s%s",(LPSTR)d,s,(LPSTR)m,s,(LPSTR)y);
    else wsprintf(out,"%s%s%s%s%s",(LPSTR)y,s,(LPSTR)m,s,(LPSTR)d);
}
static void long_picture(const Intl *t,char *out) {
    if(t->date==0) lstrcpy(out,"dddd, MMMM dd, yyyy");
    else if(t->date==1) lstrcpy(out,"dddd, dd MMMM yyyy");
    else lstrcpy(out,"yyyy MMMM dd, dddd");
}
static void write_intl(void) {
    char text[40];
#define PUT_INT(key,v) do {wsprintf(text,"%d",v); WriteProfileString("intl",key,text);} while(0)
    WriteProfileString("intl","sCountry",intl.country); PUT_INT("iCountry",intl.code);
    WriteProfileString("intl","sLanguage",languages[intl.language].code); PUT_INT("iMeasure",intl.measure);
    WriteProfileString("intl","sList",intl.list);
    PUT_INT("iDate",intl.date); WriteProfileString("intl","sDate",intl.date_sep);
    short_picture(&intl,text); WriteProfileString("intl","sShortDate",text);
    long_picture(&intl,text); WriteProfileString("intl","sLongDate",text);
    PUT_INT("iTime",intl.time24); WriteProfileString("intl","sTime",intl.time_sep);
    WriteProfileString("intl","s1159",intl.am); WriteProfileString("intl","s2359",intl.pm); PUT_INT("iTLZero",intl.time_zero);
    WriteProfileString("intl","sCurrency",intl.currency); PUT_INT("iCurrency",intl.currency_place);
    PUT_INT("iNegCurr",intl.negative); PUT_INT("iCurrDigits",intl.currency_digits);
    WriteProfileString("intl","sThousand",intl.thousand); WriteProfileString("intl","sDecimal",intl.decimal);
    PUT_INT("iDigits",intl.digits); PUT_INT("iLzero",intl.zero);
#undef PUT_INT
    SendMessage(HWND_BROADCAST,WM_WININICHANGE,0,(LPARAM)"intl");
}
/* --- samples ---- */
static const char *const month_names[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static const char *const day_names[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static void date_sample(const Intl *t,char *out) {
    SYSTEMTIME now; char picture[32],*o=out; const char *p;
    GetLocalTime(&now); short_picture(t,picture);
    for(p=picture;*p;) {
        int n=0; char c=*p;
        while(p[n]==c) n++;
        if(c=='d') o+=wsprintf(o,n>=2?"%02d":"%d",now.wDay);
        else if(c=='M') o+=wsprintf(o,n>=2?"%02d":"%d",now.wMonth);
        else if(c=='y') o+=wsprintf(o,n>=4?"%04d":"%02d",n>=4?now.wYear:now.wYear%100);
        else {int k; for(k=0;k<n;k++) *o++=c;}
        p+=n;
    }
    *o=0;
    if(t->date==0) wsprintf(o,"   %s, %s %d, %d",(LPSTR)day_names[now.wDayOfWeek],(LPSTR)month_names[now.wMonth-1],now.wDay,now.wYear);
    else if(t->date==1) wsprintf(o,"   %s, %d %s %d",(LPSTR)day_names[now.wDayOfWeek],now.wDay,(LPSTR)month_names[now.wMonth-1],now.wYear);
    else wsprintf(o,"   %d %s %d, %s",now.wYear,(LPSTR)month_names[now.wMonth-1],now.wDay,(LPSTR)day_names[now.wDayOfWeek]);
}
static void time_sample(const Intl *t,char *out) {
    SYSTEMTIME now; int h;
    GetLocalTime(&now);
    h=t->time24?now.wHour:(now.wHour%12?now.wHour%12:12);
    wsprintf(out,t->time_zero?"%02d%s%02d%s%02d %s":"%d%s%02d%s%02d %s",h,(LPSTR)t->time_sep,now.wMinute,(LPSTR)t->time_sep,now.wSecond,
             (LPSTR)(t->time24?"":now.wHour<12?t->am:t->pm));
}
/* 1,234.22 in the number format; with digits decimals. */
static void number_text(const Intl *t,int digits,char *out) {
    char frac[8]; int i;
    for(i=0;i<digits && i<6;i++) frac[i]=(char)("22"[i%2]); frac[i]=0;
    wsprintf(out,"1%s234%s%s",(LPSTR)t->thousand,(LPSTR)(digits?t->decimal:""),(LPSTR)frac);
}
/* $1,234.22 in the currency format. */
static void currency_sample(const Intl *t,char *out) {
    char n[32];
    number_text(t,t->currency_digits,n);
    switch(t->currency_place) {
    case 0: wsprintf(out,"%s%s",(LPSTR)t->currency,(LPSTR)n); break;
    case 1: wsprintf(out,"%s%s",(LPSTR)n,(LPSTR)t->currency); break;
    case 2: wsprintf(out,"%s %s",(LPSTR)t->currency,(LPSTR)n); break;
    default: wsprintf(out,"%s %s",(LPSTR)n,(LPSTR)t->currency); break;
    }
}
/* 1,234.22 and 0.7 (or .7). */
static void number_sample(const Intl *t,char *out) {
    char n[32]; number_text(t,t->digits,n);
    wsprintf(out,"%s   %s%s7",(LPSTR)n,(LPSTR)(t->zero?"0":""),(LPSTR)t->decimal);
}
static void show_intl(HWND dlg) {
    char text[96];
    date_sample(&intl,text); SetDlgItemText(dlg,IDC_DATESAMPLE,text);
    time_sample(&intl,text); SetDlgItemText(dlg,IDC_TIMESAMPLE,text);
    currency_sample(&intl,text); SetDlgItemText(dlg,IDC_CURRSAMPLE,text);
    number_sample(&intl,text); SetDlgItemText(dlg,IDC_NUMSAMPLE,text);
}
static INT_PTR CALLBACK DateFormatProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        CheckRadioButton(dlg,IDC_MDY,IDC_YMD,IDC_MDY+intl.date);
        SetDlgItemText(dlg,IDC_DATESEP,intl.date_sep); SendDlgItemMessage(dlg,IDC_DATESEP,EM_LIMITTEXT,1,0);
        CheckDlgButton(dlg,IDC_DAYZERO,intl.day_zero); CheckDlgButton(dlg,IDC_MONTHZERO,intl.month_zero); CheckDlgButton(dlg,IDC_CENTURY,intl.century);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            intl.date=IsDlgButtonChecked(dlg,IDC_DMY)?1:IsDlgButtonChecked(dlg,IDC_YMD)?2:0;
            GetDlgItemText(dlg,IDC_DATESEP,intl.date_sep,sizeof(intl.date_sep));
            intl.day_zero=IsDlgButtonChecked(dlg,IDC_DAYZERO)!=0; intl.month_zero=IsDlgButtonChecked(dlg,IDC_MONTHZERO)!=0;
            intl.century=IsDlgButtonChecked(dlg,IDC_CENTURY)!=0;
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK TimeFormatProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        CheckRadioButton(dlg,IDC_12HOUR,IDC_24HOUR,intl.time24?IDC_24HOUR:IDC_12HOUR);
        SetDlgItemText(dlg,IDC_AM,intl.am); SetDlgItemText(dlg,IDC_PM,intl.pm);
        SendDlgItemMessage(dlg,IDC_AM,EM_LIMITTEXT,sizeof(intl.am)-1,0); SendDlgItemMessage(dlg,IDC_PM,EM_LIMITTEXT,sizeof(intl.pm)-1,0);
        SetDlgItemText(dlg,IDC_TIMESEP,intl.time_sep); SendDlgItemMessage(dlg,IDC_TIMESEP,EM_LIMITTEXT,1,0);
        CheckDlgButton(dlg,IDC_TIMEZERO,intl.time_zero);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            intl.time24=IsDlgButtonChecked(dlg,IDC_24HOUR)!=0;
            GetDlgItemText(dlg,IDC_AM,intl.am,sizeof(intl.am)); GetDlgItemText(dlg,IDC_PM,intl.pm,sizeof(intl.pm));
            GetDlgItemText(dlg,IDC_TIMESEP,intl.time_sep,sizeof(intl.time_sep));
            intl.time_zero=IsDlgButtonChecked(dlg,IDC_TIMEZERO)!=0;
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK CurrencyFormatProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    static const char *const places[4]={"\x01" "1","1\x01","\x01 1","1 \x01"};
    static const char *const negatives[]={"(\x01" "1)","-\x01" "1","\x01-1","\x01" "1-","(1\x01)","-1\x01","1-\x01","1\x01-"};
    char text[32]; int i;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        for(i=0;i<4;i++) {
            const char *p=places[i]; char *o=text;
            for(;*p;p++) if(*p=='\x01') {lstrcpy(o,intl.currency); o+=lstrlen(o);} else *o++=*p;
            *o=0; SendDlgItemMessage(dlg,IDC_CURRPLACE,CB_ADDSTRING,0,(LPARAM)text);
        }
        for(i=0;i<COUNT(negatives);i++) {
            const char *p=negatives[i]; char *o=text;
            for(;*p;p++) if(*p=='\x01') {lstrcpy(o,intl.currency); o+=lstrlen(o);} else if(*p=='1') {lstrcpy(o,"123.22"); o+=6;} else *o++=*p;
            *o=0; SendDlgItemMessage(dlg,IDC_CURRNEG,CB_ADDSTRING,0,(LPARAM)text);
        }
        SendDlgItemMessage(dlg,IDC_CURRPLACE,CB_SETCURSEL,intl.currency_place&3,0);
        SendDlgItemMessage(dlg,IDC_CURRNEG,CB_SETCURSEL,intl.negative<COUNT(negatives)?intl.negative:0,0);
        SetDlgItemText(dlg,IDC_CURRSYMBOL,intl.currency); SendDlgItemMessage(dlg,IDC_CURRSYMBOL,EM_LIMITTEXT,sizeof(intl.currency)-1,0);
        SetDlgItemInt(dlg,IDC_CURRDIGITS,intl.currency_digits,FALSE);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            BOOL ok; int n=(int)GetDlgItemInt(dlg,IDC_CURRDIGITS,&ok,FALSE);
            intl.currency_place=combo(dlg,IDC_CURRPLACE); intl.negative=combo(dlg,IDC_CURRNEG);
            GetDlgItemText(dlg,IDC_CURRSYMBOL,intl.currency,sizeof(intl.currency));
            if(ok && n<=9) intl.currency_digits=n;
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK NumberFormatProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SetDlgItemText(dlg,IDC_THOUSAND,intl.thousand); SendDlgItemMessage(dlg,IDC_THOUSAND,EM_LIMITTEXT,1,0);
        SetDlgItemText(dlg,IDC_DECIMAL,intl.decimal); SendDlgItemMessage(dlg,IDC_DECIMAL,EM_LIMITTEXT,1,0);
        SetDlgItemInt(dlg,IDC_DIGITS,intl.digits,FALSE);
        CheckRadioButton(dlg,IDC_NOZERO,IDC_ZERO,intl.zero?IDC_ZERO:IDC_NOZERO);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            BOOL ok; int n=(int)GetDlgItemInt(dlg,IDC_DIGITS,&ok,FALSE);
            GetDlgItemText(dlg,IDC_THOUSAND,intl.thousand,sizeof(intl.thousand)); GetDlgItemText(dlg,IDC_DECIMAL,intl.decimal,sizeof(intl.decimal));
            if(ok && n<=9) intl.digits=n;
            intl.zero=IsDlgButtonChecked(dlg,IDC_ZERO)!=0;
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK IntlProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    int i;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        read_intl();
        for(i=0;i<COUNT(countries);i++) SendDlgItemMessage(dlg,IDC_COUNTRY,CB_ADDSTRING,0,(LPARAM)countries[i].country);
        SendDlgItemMessage(dlg,IDC_COUNTRY,CB_SETCURSEL,intl_country(intl.code),0);
        for(i=0;i<COUNT(languages);i++) SendDlgItemMessage(dlg,IDC_LANGUAGE,CB_ADDSTRING,0,(LPARAM)languages[i].name);
        SendDlgItemMessage(dlg,IDC_LANGUAGE,CB_SETCURSEL,intl.language,0);
        SendDlgItemMessage(dlg,IDC_MEASURE,CB_ADDSTRING,0,(LPARAM)"Metric"); SendDlgItemMessage(dlg,IDC_MEASURE,CB_ADDSTRING,0,(LPARAM)"English");
        SendDlgItemMessage(dlg,IDC_MEASURE,CB_SETCURSEL,intl.measure,0);
        SetDlgItemText(dlg,IDC_LISTSEP,intl.list); SendDlgItemMessage(dlg,IDC_LISTSEP,EM_LIMITTEXT,1,0);
        show_intl(dlg);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDC_COUNTRY:
            if(HIWORD(wp)==CBN_SELCHANGE) {
                intl=countries[combo(dlg,IDC_COUNTRY)];
                SendDlgItemMessage(dlg,IDC_LANGUAGE,CB_SETCURSEL,intl.language,0);
                SendDlgItemMessage(dlg,IDC_MEASURE,CB_SETCURSEL,intl.measure,0);
                SetDlgItemText(dlg,IDC_LISTSEP,intl.list);
                show_intl(dlg);
            }
            return TRUE;
        case IDC_DATECHG: if(DialogBox(instance,"DATEFMT",dlg,DateFormatProc)==IDOK) show_intl(dlg); return TRUE;
        case IDC_TIMECHG: if(DialogBox(instance,"TIMEFMT",dlg,TimeFormatProc)==IDOK) show_intl(dlg); return TRUE;
        case IDC_CURRCHG: if(DialogBox(instance,"CURRFMT",dlg,CurrencyFormatProc)==IDOK) show_intl(dlg); return TRUE;
        case IDC_NUMCHG: if(DialogBox(instance,"NUMFMT",dlg,NumberFormatProc)==IDOK) show_intl(dlg); return TRUE;
        case IDOK:
            intl.language=combo(dlg,IDC_LANGUAGE); intl.measure=combo(dlg,IDC_MEASURE);
            GetDlgItemText(dlg,IDC_LISTSEP,intl.list,sizeof(intl.list));
            write_intl();
            EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* --- keyboard and sound --------------------------------------------------------------------- */
/* [windows] KeyboardSpeed (0 slow to 31 fast) and Beep, through USER's
 * SystemParametersInfo, which keeps them. */
static UINT original_rate;
static INT_PTR CALLBACK KeyboardProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_INITDIALOG:
        SystemParametersInfo(SPI_GETKEYBOARDSPEED,0,&original_rate,0);
        SetScrollRange(GetDlgItem(dlg,IDC_RATE),SB_CTL,0,31,FALSE);
        SetScrollPos(GetDlgItem(dlg,IDC_RATE),SB_CTL,(int)original_rate,TRUE);
        return TRUE;
    case WM_HSCROLL: {
        HWND bar=(HWND)lp; int pos=GetScrollPos(bar,SB_CTL);
        switch(LOWORD(wp)) {
        case SB_LINELEFT: pos--; break;
        case SB_LINERIGHT: pos++; break;
        case SB_PAGELEFT: pos-=4; break;
        case SB_PAGERIGHT: pos+=4; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos=(short)HIWORD(wp); break;
        case SB_LEFT: pos=0; break;
        case SB_RIGHT: pos=31; break;
        default: return TRUE;
        }
        pos=pos<0?0:pos>31?31:pos;
        SetScrollPos(bar,SB_CTL,pos,TRUE);
        SystemParametersInfo(SPI_SETKEYBOARDSPEED,(UINT)pos,NULL,0);
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            UINT rate; SystemParametersInfo(SPI_GETKEYBOARDSPEED,0,&rate,0);
            SystemParametersInfo(SPI_SETKEYBOARDSPEED,rate,NULL,SPIF_UPDATEINIFILE|SPIF_SENDWININICHANGE);
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {SystemParametersInfo(SPI_SETKEYBOARDSPEED,original_rate,NULL,0); EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}
static INT_PTR CALLBACK SoundProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL beep;
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        SystemParametersInfo(SPI_GETBEEP,0,&beep,0);
        CheckDlgButton(dlg,IDC_BEEP,beep);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            SystemParametersInfo(SPI_SETBEEP,IsDlgButtonChecked(dlg,IDC_BEEP)!=0,NULL,SPIF_UPDATEINIFILE|SPIF_SENDWININICHANGE);
            EndDialog(dlg,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- the window -------------------------------------------------------------------------- */
/* The settings, in Windows 3.0's order, each an icon, a dialog and a menu command. */
static const struct {const char *title,*icon,*dialog; DLGPROC proc;} items[]={
    {"Color","COLOR","COLORDLG",ColorProc},{"Fonts","FONTS","FONTDLG",FontsProc},{"Ports","PORTS","PORTDLG",PortsProc},
    {"Mouse","MOUSE","MOUSEDLG",MouseProc},{"Desktop","DESKTOP","DESKDLG",DesktopProc},
    {"Printers","PRINTERS","PRINTERS",PrintersProc},{"International","INTL","INTLDLG",IntlProc},
    {"Keyboard","KEYBOARD","KEYBDLG",KeyboardProc},{"Date/Time","DATETIME","DATEDLG",DateProc},
    {"Sound","SOUND","SOUNDDLG",SoundProc},
};
#define ITEMS ((int)(sizeof(items)/sizeof(items[0])))
#define ROWS ((ITEMS+PER_ROW-1)/PER_ROW)
static HICON icons[ITEMS];
static void open_item(HWND h,int item) {
    if(item>=0 && item<ITEMS) DialogBox(instance,items[item].dialog,h,items[item].proc);
}
static int item_height(void) {return 32+4+char_height+8;}
static void item_rect(int i,RECT *r) {
    int x=8+(i%PER_ROW)*ITEM_W,y=6+(i/PER_ROW)*item_height();
    SetRect(r,x,y,x+ITEM_W,y+32+4+char_height+2);
}
static int item_at(int x,int y) {
    int i; POINT p; p.x=x; p.y=y;
    for(i=0;i<ITEMS;i++) {RECT r; item_rect(i,&r); if(PtInRect(&r,p)) return i;}
    return -1;
}
static void paint(HDC dc) {
    int i;
    SetBkMode(dc,TRANSPARENT);
    for(i=0;i<ITEMS;i++) {
        RECT r,t; SIZE s;
        item_rect(i,&r);
        DrawIcon(dc,r.left+(ITEM_W-32)/2,r.top,icons[i]);
        GetTextExtentPoint(dc,items[i].title,lstrlen(items[i].title),&s);
        SetRect(&t,r.left+(ITEM_W-s.cx)/2-2,r.top+36,r.left+(ITEM_W+s.cx)/2+2,r.top+36+s.cy);
        if(i==selected) {FillRect(dc,&t,(HBRUSH)(COLOR_HIGHLIGHT+1)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
        else SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        TextOut(dc,t.left+2,t.top,items[i].title,lstrlen(items[i].title));
    }
}
static void select_item(HWND h,int i) {
    if(i<0 || i>=ITEMS || i==selected) return;
    selected=i; InvalidateRect(h,NULL,TRUE);
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(dc); EndPaint(h,&ps); return 0;}
    case WM_LBUTTONDOWN: select_item(h,item_at(GET_X_LPARAM(lp),GET_Y_LPARAM(lp))); return 0;
    case WM_LBUTTONDBLCLK: {int i=item_at(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); if(i>=0) {select_item(h,i); open_item(h,i);} return 0;}
    case WM_KEYDOWN:
        switch(wp) {
        case VK_LEFT: select_item(h,(selected+ITEMS-1)%ITEMS); break;
        case VK_RIGHT: select_item(h,(selected+1)%ITEMS); break;
        case VK_UP: if(selected>=PER_ROW) select_item(h,selected-PER_ROW); break;
        case VK_DOWN: select_item(h,min(selected+PER_ROW,ITEMS-1)); break;
        case VK_HOME: select_item(h,0); break;
        case VK_END: select_item(h,ITEMS-1); break;
        case VK_RETURN: open_item(h,selected); break;
        }
        return 0;
    case WM_SYSCOLORCHANGE: InvalidateRect(h,NULL,TRUE); return 0;
    case WM_COMMAND:
        if(HelpCommand(h,LOWORD(wp),"CONTROL.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_EXIT: DestroyWindow(h); return 0;
        case IDM_ABOUT: MessageBox(h,"Control Panel\nThe system's settings.","About Control Panel",MB_OK|MB_ICONINFORMATION); return 0;
        }
        if(LOWORD(wp)>=IDM_ITEM && LOWORD(wp)<IDM_ITEM+ITEMS) {select_item(h,LOWORD(wp)-IDM_ITEM); open_item(h,LOWORD(wp)-IDM_ITEM);}
        return 0;
    case WM_DESTROY: WinHelp(h,"CONTROL.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; int i; RECT r; TEXTMETRIC tm; HDC dc; DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    (void)command;
    instance=inst;
    if(previous) return 0;
    memset(&wc,0,sizeof(wc));
    wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"CONTROL");
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="CONTROL"; wc.lpszClassName="ControlPanel";
    RegisterClass(&wc);
    wc.lpfnWndProc=TestBoxProc; wc.cbWndExtra=sizeof(LONG); wc.hIcon=NULL; wc.lpszMenuName=NULL; wc.lpszClassName="CtlTestBox";
    RegisterClass(&wc);
    for(i=0;i<ITEMS;i++) icons[i]=LoadIcon(inst,items[i].icon);
    dc=GetDC(NULL); GetTextMetrics(dc,&tm); ReleaseDC(NULL,dc); char_height=(int)tm.tmHeight;
    SetRect(&r,0,0,PER_ROW*ITEM_W+16,ROWS*item_height()+8);
    AdjustWindowRect(&r,style,TRUE);
    wnd=CreateWindow("ControlPanel","Control Panel",style,
                     CW_USEDEFAULT,0,r.right-r.left,r.bottom-r.top,NULL,NULL,inst,NULL);
    ShowWindow(wnd,show); UpdateWindow(wnd);
    accel=LoadAccelerators(inst,"CONTROL");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
