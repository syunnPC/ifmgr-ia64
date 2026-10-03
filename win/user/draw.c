/* SPDX-License-Identifier: GPL-2.0-or-later
 * System colors and metrics, painting helpers, text with mnemonics and tabs,
 * wsprintf, the ANSI character functions, ExitWindows and USER's start-up.
 * Colors follow the Windows 3.0 default scheme.
 */
#include "user.h"

HINSTANCE user_instance;
static COLORREF colors[COLOR_BTNHIGHLIGHT+1]={
    RGB(192,192,192), /* COLOR_SCROLLBAR */
    RGB(192,192,192), /* COLOR_BACKGROUND */
    RGB(0,0,128),     /* COLOR_ACTIVECAPTION */
    RGB(255,255,255), /* COLOR_INACTIVECAPTION */
    RGB(255,255,255), /* COLOR_MENU */
    RGB(255,255,255), /* COLOR_WINDOW */
    RGB(0,0,0),       /* COLOR_WINDOWFRAME */
    RGB(0,0,0),       /* COLOR_MENUTEXT */
    RGB(0,0,0),       /* COLOR_WINDOWTEXT */
    RGB(255,255,255), /* COLOR_CAPTIONTEXT */
    RGB(192,192,192), /* COLOR_ACTIVEBORDER */
    RGB(192,192,192), /* COLOR_INACTIVEBORDER */
    RGB(255,255,232), /* COLOR_APPWORKSPACE */
    RGB(0,0,128),     /* COLOR_HIGHLIGHT */
    RGB(255,255,255), /* COLOR_HIGHLIGHTTEXT */
    RGB(192,192,192), /* COLOR_BTNFACE */
    RGB(128,128,128), /* COLOR_BTNSHADOW */
    RGB(128,128,128), /* COLOR_GRAYTEXT */
    RGB(0,0,0),       /* COLOR_BTNTEXT */
    RGB(0,0,0),       /* COLOR_INACTIVECAPTIONTEXT */
    RGB(255,255,255), /* COLOR_BTNHIGHLIGHT */
};
static HBRUSH brushes[COLOR_BTNHIGHLIGHT+1];
static int char_height=13,char_width=8;

DWORD WINAPI GetSysColor(int index) {return index>=0 && index<=COLOR_BTNHIGHLIGHT?colors[index]:0;}
HBRUSH SysBrush(int index) {return index>=0 && index<=COLOR_BTNHIGHLIGHT?brushes[index]:NULL;}
void WINAPI SetSysColors(int count,const int FAR *which,const COLORREF FAR *values) {
    int i;
    for(i=0;i<count;i++) if(which[i]>=0 && which[i]<=COLOR_BTNHIGHLIGHT) {
        colors[which[i]]=values[i]&0xffffff;
        DeleteObject(brushes[which[i]]); brushes[which[i]]=CreateSolidBrush(colors[which[i]]);
        GdiSetOwner(brushes[which[i]],NULL); /* the system's, not the calling task's */
    }
    SendMessage((HWND)0xffff,WM_SYSCOLORCHANGE,0,0);
    InvalidateRect(NULL,NULL,TRUE);
}
HFONT SystemFont(void) {return (HFONT)GetStockObject(SYSTEM_FONT);}
int CharHeight(void) {return char_height;}
int CharWidth(void) {return char_width;}
int WINAPI GetSystemMetrics(int index) {
    switch(index) {
    case SM_CXSCREEN: case SM_CXFULLSCREEN: return screen_width;
    case SM_CYSCREEN: return screen_height;
    case SM_CYFULLSCREEN: return screen_height-CAPTION;
    case SM_CXVSCROLL: case SM_CYHSCROLL: case SM_CYVSCROLL: case SM_CXHSCROLL: return SCROLL;
    case SM_CYVTHUMB: case SM_CXHTHUMB: return SCROLL;
    case SM_CYCAPTION: return CAPTION;
    case SM_CXBORDER: case SM_CYBORDER: return 1;
    case SM_CXDLGFRAME: case SM_CYDLGFRAME: return FRAME;
    case SM_CXFRAME: case SM_CYFRAME: return FRAME;
    case SM_CXICON: case SM_CYICON: case SM_CXCURSOR: case SM_CYCURSOR: return ICON;
    case SM_CYMENU: return MENUBAR;
    case SM_MOUSEPRESENT: return 1;
    case SM_CXSIZE: case SM_CYSIZE: return BOXSIZE;
    case SM_CXMIN: case SM_CXMINTRACK: return 100;
    case SM_CYMIN: case SM_CYMINTRACK: return CAPTION+2*FRAME;
    case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
    case SM_CXICONSPACING: return ICON_SPACING;
    case SM_CYICONSPACING: return ICON_ROW;
    }
    return 0;
}
/* The warning beep (WIN.INI [windows] Beep) and the key repeat rate
 * (KeyboardSpeed, 0 to 31), kept for programs: there is no sound device
 * here, and the keyboard's own repeating is the firmware's. */
static BOOL beep_on=TRUE; static UINT keyboard_speed=31;
static void keep_setting(UINT flags,LPCSTR key,LPCSTR value) {
    if(flags&SPIF_UPDATEINIFILE) WriteProfileString("windows",key,value);
    if(flags&SPIF_SENDWININICHANGE) SendMessage(HWND_BROADCAST,WM_WININICHANGE,0,(LPARAM)"windows");
}
BOOL WINAPI SystemParametersInfo(UINT action,UINT param,LPVOID out,UINT flags) {
    char text[8];
    switch(action) {
    case SPI_GETBEEP: if(out) *(BOOL *)out=beep_on; return TRUE;
    case SPI_SETBEEP: beep_on=param!=0; keep_setting(flags,"Beep",beep_on?"yes":"no"); return TRUE;
    case SPI_GETBORDER: if(out) *(int *)out=FRAME-1; return TRUE;
    case SPI_GETKEYBOARDSPEED: if(out) *(int *)out=(int)keyboard_speed; return TRUE;
    case SPI_SETKEYBOARDSPEED: keyboard_speed=min(param,31); wsprintf(text,"%u",keyboard_speed); keep_setting(flags,"KeyboardSpeed",text); return TRUE;
    case SPI_ICONHORIZONTALSPACING: if(out) *(int *)out=ICON_SPACING; return TRUE;
    }
    return FALSE;
}

/* --- rectangles ------------------------------------------------------------------ */
/* A brush handle up to COLOR_BTNHIGHLIGHT+1 means system color index-1. */
static HBRUSH brush_of(HBRUSH b) {
    ULONG_PTR v=(ULONG_PTR)b;
    return v && v<=COLOR_BTNHIGHLIGHT+1?brushes[v-1]:b;
}
int WINAPI FillRect(HDC dc,LPCRECT r,HBRUSH brush) {
    HGDIOBJ old;
    if(!r) return 0;
    old=SelectObject(dc,brush_of(brush));
    PatBlt(dc,r->left,r->top,r->right-r->left,r->bottom-r->top,PATCOPY);
    if(old) SelectObject(dc,old);
    return 1;
}
int WINAPI FrameRect(HDC dc,LPCRECT r,HBRUSH brush) {
    HGDIOBJ old;
    if(!r || r->right<=r->left || r->bottom<=r->top) return 0;
    old=SelectObject(dc,brush_of(brush));
    PatBlt(dc,r->left,r->top,r->right-r->left,1,PATCOPY);
    PatBlt(dc,r->left,r->bottom-1,r->right-r->left,1,PATCOPY);
    PatBlt(dc,r->left,r->top,1,r->bottom-r->top,PATCOPY);
    PatBlt(dc,r->right-1,r->top,1,r->bottom-r->top,PATCOPY);
    if(old) SelectObject(dc,old);
    return 1;
}
void WINAPI InvertRect(HDC dc,LPCRECT r) {if(r) PatBlt(dc,r->left,r->top,r->right-r->left,r->bottom-r->top,DSTINVERT);}
/* A dotted rectangle drawn with XOR: drawing it twice removes it. */
void WINAPI DrawFocusRect(HDC dc,LPCRECT r) {
    int x,y;
    if(!r) return;
    for(x=r->left;x<r->right;x+=2) {PatBlt(dc,x,r->top,1,1,DSTINVERT); if(r->bottom-1>r->top) PatBlt(dc,x,r->bottom-1,1,1,DSTINVERT);}
    for(y=r->top+2-((r->right-r->left)&1);y<r->bottom-1;y+=2) {
        if(y<=r->top) continue;
        PatBlt(dc,r->left,y,1,1,DSTINVERT); if(r->right-1>r->left) PatBlt(dc,r->right-1,y,1,1,DSTINVERT);
    }
}
void DrawButtonFace(HDC dc,const RECT *r,BOOL pressed) {
    RECT in=*r;
    FrameRect(dc,&in,SysBrush(COLOR_WINDOWFRAME));
    InflateRect(&in,-1,-1);
    FillRect(dc,&in,SysBrush(COLOR_BTNFACE));
    if(pressed) {
        RECT s; SetRect(&s,in.left,in.top,in.right,in.top+1); FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW));
        SetRect(&s,in.left,in.top,in.left+1,in.bottom); FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW));
        return;
    }
    {
        RECT s;
        SetRect(&s,in.left,in.top,in.right-1,in.top+2); FillRect(dc,&s,SysBrush(COLOR_BTNHIGHLIGHT));
        SetRect(&s,in.left,in.top,in.left+2,in.bottom-1); FillRect(dc,&s,SysBrush(COLOR_BTNHIGHLIGHT));
        SetRect(&s,in.left+1,in.bottom-2,in.right,in.bottom); FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW));
        SetRect(&s,in.right-2,in.top+1,in.right,in.bottom); FillRect(dc,&s,SysBrush(COLOR_BTNSHADOW));
    }
}
HBRUSH ControlColor(Wnd *w,HDC dc,UINT msg) {
    (void)w;
    if(msg==WM_CTLCOLORSCROLLBAR) {SetBkColor(dc,GetSysColor(COLOR_WINDOW)); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); return SysBrush(COLOR_SCROLLBAR);}
    SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(dc,GetSysColor(COLOR_WINDOW));
    return SysBrush(COLOR_WINDOW);
}

/* --- text ------------------------------------------------------------------------------ */
/* The character after the first single '&', or 0. */
char PrefixChar(LPCSTR s) {
    for(;s && *s;s++) if(*s=='&') {if(s[1]=='&') s++; else return s[1];}
    return 0;
}
static int strip(LPCSTR s,int count,char *out,int *under) {
    int i,n=0;
    *under=-1;
    if(count<0) count=lstrlen(s);
    for(i=0;i<count && n<511;i++) {
        if(s[i]=='&' && i+1<count) {i++; if(s[i]!='&' && *under<0) *under=n;}
        out[n++]=s[i];
    }
    out[n]=0; return n;
}
int PrefixTextWidth(HDC dc,LPCSTR s,int count) {
    char text[512]; int under; SIZE size; int n=strip(s,count,text,&under);
    GetTextExtentPoint(dc,text,n,&size);
    return (int)size.cx;
}
/* Text with its mnemonic underlined; gray text is drawn in COLOR_GRAYTEXT. */
void DrawPrefixText(HDC dc,int x,int y,LPCSTR s,int count,BOOL gray) {
    char text[512]; int under,n=strip(s,count,text,&under); COLORREF old=0;
    TEXTMETRIC tm;
    if(gray) old=SetTextColor(dc,GetSysColor(COLOR_GRAYTEXT));
    TextOut(dc,x,y,text,n);
    if(under>=0) {
        SIZE a,b; HBRUSH br=CreateSolidBrush(GetTextColor(dc)); RECT r;
        GetTextMetrics(dc,&tm);
        GetTextExtentPoint(dc,text,under,&a); GetTextExtentPoint(dc,text,under+1,&b);
        SetRect(&r,x+a.cx,y+tm.tmAscent+1,x+b.cx,y+tm.tmAscent+2);
        FillRect(dc,&r,br); DeleteObject(br);
    }
    if(gray) SetTextColor(dc,old);
}
static int tab_width(HDC dc,int count,const int FAR *tabs,int pos,int origin) {
    TEXTMETRIC tm; int i;
    if(count==1 && tabs) return ((pos-origin)/tabs[0]+1)*tabs[0]+origin;
    for(i=0;tabs && i<count;i++) if(origin+tabs[i]>pos) return origin+tabs[i];
    GetTextMetrics(dc,&tm);
    return ((pos-origin)/(8*tm.tmAveCharWidth)+1)*(8*tm.tmAveCharWidth)+origin;
}
static LONG tabbed(HDC dc,int x,int y,LPCSTR s,int count,int ntabs,const int FAR *tabs,int origin,BOOL draw) {
    int i,start=0,pos=x; SIZE size; TEXTMETRIC tm;
    if(count<0) count=lstrlen(s);
    GetTextMetrics(dc,&tm);
    for(i=0;i<=count;i++) if(i==count || s[i]=='\t') {
        GetTextExtentPoint(dc,s+start,i-start,&size);
        if(draw) TextOut(dc,pos,y,s+start,i-start);
        pos+=size.cx;
        if(i<count) {
            int next=tab_width(dc,ntabs,tabs,pos,origin);
            if(draw && GetBkMode(dc)==OPAQUE) {RECT r; HBRUSH b=CreateSolidBrush(GetBkColor(dc)); SetRect(&r,pos,y,next,y+tm.tmHeight); FillRect(dc,&r,b); DeleteObject(b);}
            pos=next;
        }
        start=i+1;
    }
    return MAKELONG(pos-x,tm.tmHeight);
}
LONG WINAPI TabbedTextOut(HDC dc,int x,int y,LPCSTR s,int count,int ntabs,const int FAR *tabs,int origin) {
    return s?tabbed(dc,x,y,s,count,ntabs,tabs,origin,TRUE):0;
}
DWORD WINAPI GetTabbedTextExtent(HDC dc,LPCSTR s,int count,int ntabs,const int FAR *tabs) {
    return s?(DWORD)tabbed(dc,0,0,s,count,ntabs,tabs,0,FALSE):0;
}
static int line_width(HDC dc,const char *line,int len,UINT format) {
    SIZE size;
    if(format&DT_EXPANDTABS) return LOWORD(GetTabbedTextExtent(dc,line,len,0,NULL));
    if(!(format&DT_NOPREFIX)) return PrefixTextWidth(dc,line,len);
    GetTextExtentPoint(dc,line,len,&size); return (int)size.cx;
}
/* One line of DrawText: characters used and the visible text in out. */
static int take_line(LPCSTR s,int n,UINT format,char *out,int *out_len,int width,HDC dc) {
    int i=0,o=0,last_break=-1,last_out=0; SIZE size;
    while(i<n && s[i]!='\n' && s[i]!='\r') {
        out[o++]=s[i++];
        if(o>=511) break;
        if(format&DT_WORDBREAK && !(format&DT_SINGLELINE)) {
            if(out[o-1]==' ') {last_break=i; last_out=o-1;}
            size.cx=line_width(dc,out,o,format);
            if(size.cx>width && last_break>0) {*out_len=last_out; return last_break;}
        }
    }
    *out_len=o; out[o]=0;
    if(i<n && s[i]=='\r') i++;
    if(i<n && s[i]=='\n') i++;
    return i;
}
int WINAPI DrawText(HDC dc,LPCSTR text,int count,LPRECT rect,UINT format) {
    TEXTMETRIC tm; char line[512]; int used,len,y,total=0,widest=0,i,lh;
    if(!text || !rect) return 0;
    if(count<0) count=lstrlen(text);
    GetTextMetrics(dc,&tm);
    lh=tm.tmHeight+(format&DT_EXTERNALLEADING?tm.tmExternalLeading:0);
    for(i=0;i<count;) {
        used=take_line(text+i,count-i,format,line,&len,rect->right-rect->left,dc);
        widest=max(widest,line_width(dc,line,len,format));
        total+=lh;
        if(format&DT_SINGLELINE || !used) break;
        i+=used;
    }
    if(!count) total=lh;
    if(format&DT_CALCRECT) {rect->right=rect->left+widest; rect->bottom=rect->top+total; return total;}
    y=rect->top;
    if(format&DT_SINGLELINE) {
        if(format&DT_VCENTER) y=rect->top+(rect->bottom-rect->top-tm.tmHeight)/2;
        else if(format&DT_BOTTOM) y=rect->bottom-tm.tmHeight;
    }
    if(!(format&DT_NOCLIP)) {SaveDC(dc); IntersectClipRect(dc,rect->left,rect->top,rect->right,rect->bottom);}
    for(i=0;i<count;) {
        int x,w;
        used=take_line(text+i,count-i,format,line,&len,rect->right-rect->left,dc);
        w=line_width(dc,line,len,format);
        x=rect->left;
        if(format&DT_CENTER) x=rect->left+(rect->right-rect->left-w)/2;
        else if(format&DT_RIGHT) x=rect->right-w;
        if(format&DT_EXPANDTABS) TabbedTextOut(dc,x,y,line,len,0,NULL,x);
        else if(format&DT_NOPREFIX) TextOut(dc,x,y,line,len);
        else DrawPrefixText(dc,x,y,line,len,FALSE);
        y+=lh;
        if(format&DT_SINGLELINE || !used) break;
        i+=used;
    }
    if(!(format&DT_NOCLIP)) RestoreDC(dc,-1);
    return total;
}
/* Gray text: COLOR_GRAYTEXT on displays with a gray. */
BOOL WINAPI GrayString(HDC dc,HBRUSH brush,FARPROC proc,LPARAM data,int count,int x,int y,int w,int h) {
    COLORREF old=SetTextColor(dc,GetSysColor(COLOR_GRAYTEXT)); BOOL ok=TRUE;
    (void)brush; (void)w; (void)h;
    if(proc) ok=(BOOL)((BOOL (CALLBACK *)(HDC,LPARAM,int))proc)(dc,data,count);
    else TextOut(dc,x,y,(LPCSTR)data,count?count:lstrlen((LPCSTR)data));
    SetTextColor(dc,old);
    return ok;
}

/* --- wsprintf -------------------------------------------------------------------------- */
int CDECL wvsprintf(LPSTR out,LPCSTR format,va_list ap) {
    int n=0;
    while(*format) {
        char digits[24],pad=' ',type; int width=0,left=0,len=0,neg=0,i,precision=-1; unsigned __int64 v; const char *s;
        if(*format!='%') {out[n++]=*format++; continue;}
        format++;
        if(*format=='-') {left=1; format++;}
        if(*format=='0') {pad='0'; format++;}
        while(*format>='0' && *format<='9') width=width*10+(*format++-'0');
        if(*format=='.') {precision=0; format++; while(*format>='0' && *format<='9') precision=precision*10+(*format++-'0');}
        if(*format=='l' || *format=='h') format++;
        type=*format++;
        switch(type) {
        case 'd': case 'i': {int x=va_arg(ap,int); neg=x<0; v=neg?(unsigned __int64)(0-(__int64)x):(unsigned __int64)x; break;}
        case 'u': case 'x': case 'X': v=va_arg(ap,unsigned); break;
        case 'c': digits[0]=(char)va_arg(ap,int); s=digits; len=1; v=0; goto emit;
        case 's': s=va_arg(ap,const char *); if(!s) s="(null)"; len=lstrlen(s); if(precision>=0 && len>precision) len=precision; v=0; goto emit;
        case 0: format--; continue;
        default: out[n++]=type; continue;
        }
        do {
            int d=(int)(v%(type=='x' || type=='X'?16:10));
            digits[sizeof(digits)-1-len++]=(char)(d<10?'0'+d:(type=='x'?'a':'A')+d-10);
            v/=type=='x' || type=='X'?16:10;
        } while(v);
        if(neg) digits[sizeof(digits)-1-len++]='-';
        s=digits+sizeof(digits)-len;
    emit:
        if(!left) for(i=len;i<width;i++) out[n++]=pad;
        for(i=0;i<len;i++) out[n++]=s[i];
        if(left) for(i=len;i<width;i++) out[n++]=' ';
    }
    out[n]=0;
    return n;
}
int CDECL wsprintf(LPSTR out,LPCSTR format,...) {
    va_list ap; int n;
    va_start(ap,format);
    n=wvsprintf(out,format,ap);
    va_end(ap);
    return n;
}

/* --- characters (code page 1252) -------------------------------------------------------- */
static BYTE to_upper(BYTE c) {
    if(c>='a' && c<='z') return (BYTE)(c-0x20);
    if(c>=0xe0 && c!=0xf7 && c!=0xff) return (BYTE)(c-0x20);
    if(c==0x9a || c==0x9c || c==0x9e) return (BYTE)(c-0x10);
    if(c==0xff) return 0x9f;
    return c;
}
static BYTE to_lower(BYTE c) {
    if(c>='A' && c<='Z') return (BYTE)(c+0x20);
    if(c>=0xc0 && c<=0xde && c!=0xd7) return (BYTE)(c+0x20);
    if(c==0x8a || c==0x8c || c==0x8e) return (BYTE)(c+0x10);
    if(c==0x9f) return 0xff;
    return c;
}
LPSTR WINAPI AnsiUpper(LPSTR s) {
    LPSTR p;
    if(IS_INTRESOURCE(s)) return (LPSTR)(ULONG_PTR)to_upper((BYTE)(ULONG_PTR)s);
    for(p=s;*p;p++) *p=(char)to_upper((BYTE)*p);
    return s;
}
LPSTR WINAPI AnsiLower(LPSTR s) {
    LPSTR p;
    if(IS_INTRESOURCE(s)) return (LPSTR)(ULONG_PTR)to_lower((BYTE)(ULONG_PTR)s);
    for(p=s;*p;p++) *p=(char)to_lower((BYTE)*p);
    return s;
}
UINT WINAPI AnsiUpperBuff(LPSTR s,UINT n) {UINT i; for(i=0;i<n;i++) s[i]=(char)to_upper((BYTE)s[i]); return n;}
UINT WINAPI AnsiLowerBuff(LPSTR s,UINT n) {UINT i; for(i=0;i<n;i++) s[i]=(char)to_lower((BYTE)s[i]); return n;}
LPSTR WINAPI AnsiNext(LPCSTR s) {return (LPSTR)(s && *s?s+1:s);}
LPSTR WINAPI AnsiPrev(LPCSTR start,LPCSTR s) {return (LPSTR)(s>start?s-1:start);}
void WINAPI AnsiToOem(LPCSTR from,LPSTR to) {if(from!=to) lstrcpy(to,from);}
void WINAPI OemToAnsi(LPCSTR from,LPSTR to) {if(from!=to) lstrcpy(to,from);}
BOOL WINAPI IsCharUpper(char c) {return to_lower((BYTE)c)!=(BYTE)c;}
BOOL WINAPI IsCharLower(char c) {return to_upper((BYTE)c)!=(BYTE)c;}
BOOL WINAPI IsCharAlpha(char c) {return IsCharUpper(c) || IsCharLower(c) || (BYTE)c==0xdf;}
BOOL WINAPI IsCharAlphaNumeric(char c) {return IsCharAlpha(c) || (c>='0' && c<='9');}

/* --- miscellaneous ------------------------------------------------------------------------- */
DWORD WINAPI GetTickCount(void) {return (DWORD)wh_ticks();}
DWORD WINAPI GetCurrentTime(void) {return (DWORD)wh_ticks();}
void WINAPI MessageBeep(UINT type) {(void)type;}
/* Windows 3.0's sound functions, without a sound device. */
int WINAPI OpenSound(void) {return S_SERDVNA;}
void WINAPI CloseSound(void) {}
int WINAPI SetVoiceQueueSize(int voice,int bytes) {(void)voice; (void)bytes; return S_SERDVNA;}
int WINAPI SetVoiceNote(int voice,int note,int length,int dots) {(void)voice; (void)note; (void)length; (void)dots; return S_SERDVNA;}
int WINAPI SetVoiceAccent(int voice,int tempo,int volume,int mode,int pitch) {(void)voice; (void)tempo; (void)volume; (void)mode; (void)pitch; return S_SERDVNA;}
int WINAPI SetVoiceEnvelope(int voice,int shape,int repeat) {(void)voice; (void)shape; (void)repeat; return S_SERDVNA;}
int WINAPI SetSoundNoise(int source,int duration) {(void)source; (void)duration; return S_SERDVNA;}
int WINAPI SetVoiceSound(int voice,DWORD frequency,int duration) {(void)voice; (void)frequency; (void)duration; return S_SERDVNA;}
int WINAPI StartSound(void) {return 0;}
int WINAPI StopSound(void) {return 0;}
int WINAPI WaitSoundState(int state) {(void)state; return 0;}
int WINAPI SyncAllVoices(void) {return 0;}
int WINAPI CountVoiceNotes(int voice) {(void)voice; return 0;}
LPINT WINAPI GetThresholdEvent(void) {static int none; return &none;}
int WINAPI GetThresholdStatus(void) {return 0;}
int WINAPI SetVoiceThreshold(int voice,int notes) {(void)voice; (void)notes; return 0;}
/* Every top-level window agrees, then hears the session end, and Windows ends. */
BOOL WINAPI ExitWindows(DWORD reserved,UINT code) {
    HWND list[64]; int n=0,i; HWND h;
    (void)reserved; (void)code;
    for(h=GetTopWindow(NULL);h && n<64;h=GetWindow(h,GW_HWNDNEXT)) list[n++]=h;
    for(i=0;i<n;i++) if(IsWindow(list[i]) && !SendMessage(list[i],WM_QUERYENDSESSION,0,0)) {
        int k; for(k=0;k<i;k++) if(IsWindow(list[k])) SendMessage(list[k],WM_ENDSESSION,FALSE,0);
        return FALSE;
    }
    for(i=0;i<n;i++) if(IsWindow(list[i])) SendMessage(list[i],WM_ENDSESSION,TRUE,0);
    ExitKernel(0);
    return TRUE;
}

/* WIN.INI [colors] holds "r g b" by these names (the Control Panel writes them). */
static const char *const color_keys[COLOR_BTNHIGHLIGHT+1]={
    "Scrollbar","Background","ActiveTitle","InactiveTitle","Menu","Window","WindowFrame","MenuText","WindowText",
    "TitleText","ActiveBorder","InactiveBorder","AppWorkspace","Hilight","HilightText","ButtonFace","ButtonShadow",
    "GrayText","ButtonText","InactiveTitleText","ButtonHilight"};
static void read_colors(void) {
    int i;
    for(i=0;i<=COLOR_BTNHIGHLIGHT;i++) {
        char text[32]; const char *p=text; int v[3],k;
        if(!GetProfileString("colors",color_keys[i],"",text,sizeof(text))) continue;
        for(k=0;k<3;k++) {
            while(*p==' ' || *p=='\t') p++;
            if(*p<'0' || *p>'9') break;
            for(v[k]=0;*p>='0' && *p<='9';p++) v[k]=v[k]*10+(*p-'0');
            if(v[k]>255) break;
        }
        if(k==3) colors[i]=RGB(v[0],v[1],v[2]);
    }
}
/* The average character width as USER computes it for dialog units: of the
 * 52 letters, rounded. */
int LetterWidth(HDC dc) {
    SIZE s;
    GetTextExtentPoint(dc,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",52,&s);
    return (s.cx/26+1)/2;
}
void DrawInit(void) {
    int i; HDC dc; TEXTMETRIC tm;
    read_colors();
    SetDoubleClickTime((UINT)GetProfileInt("windows","DoubleClickSpeed",500));
    {char beep[8]; GetProfileString("windows","Beep","yes",beep,sizeof(beep)); beep_on=lstrcmpi(beep,"no")!=0;}
    keyboard_speed=min((UINT)GetProfileInt("windows","KeyboardSpeed",31),31);
    for(i=0;i<=COLOR_BTNHIGHLIGHT;i++) brushes[i]=CreateSolidBrush(colors[i]);
    dc=CreateCompatibleDC(NULL); SelectObject(dc,SystemFont()); GetTextMetrics(dc,&tm);
    char_height=(int)tm.tmHeight; char_width=LetterWidth(dc);
    DeleteDC(dc);
}
void DrawShutdown(void) {
    int i;
    for(i=0;i<=COLOR_BTNHIGHLIGHT;i++) if(brushes[i]) DeleteObject(brushes[i]);
}
int WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if(reason==1) {
        user_instance=instance;
        WndInit(); DrawInit(); NcInit(); ResourceInit(); MenuInit(); DialogInit(); HookInit(); MsgInit();
        PaintDesktop(); GdiFlush();
    } else if(reason==0) {
        wh_set_idle(NULL); SetTaskSignalProc(NULL,NULL);
        ResourceShutdown(); NcShutdown(); DrawShutdown();
    }
    return TRUE;
}
