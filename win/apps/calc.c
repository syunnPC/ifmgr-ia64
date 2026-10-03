/* SPDX-License-Identifier: GPL-2.0-or-later
 * CALC: the Windows 3.0 standard Calculator. Numbers are doubles shown
 * with up to 13 significant digits (integers end with a point, as in
 * Windows); keys: digits . + - * / = (or Enter), Esc (C), Del (CE),
 * Backspace, @ (sqrt), % , r (1/x), F9 (+/-), Ctrl+L/R/M/P (MC, MR, MS,
 * M+). Pasted text is typed. "CALC /trace" reports the display through
 * OutputDebugString after each key, for tests.
 */
#include <windows.h>
#include "calc.h"
#include "winapp.h"

enum {K_C=256,K_CE,K_BACK,K_MC,K_MR,K_MS,K_MPLUS,K_SIGN,K_SQRT,K_RECIP,K_PERCENT};
typedef struct {LPCSTR label; int key,col,row;} Key;
static const Key keys[]={
    {"Back",K_BACK,2,0},{"CE",K_CE,3,0},{"C",K_C,4,0},
    {"MC",K_MC,-1,1},{"7",'7',0,1},{"8",'8',1,1},{"9",'9',2,1},{"/",'/',3,1},{"sqrt",K_SQRT,4,1},
    {"MR",K_MR,-1,2},{"4",'4',0,2},{"5",'5',1,2},{"6",'6',2,2},{"*",'*',3,2},{"%",K_PERCENT,4,2},
    {"MS",K_MS,-1,3},{"1",'1',0,3},{"2",'2',1,3},{"3",'3',2,3},{"-",'-',3,3},{"1/x",K_RECIP,4,3},
    {"M+",K_MPLUS,-1,4},{"0",'0',0,4},{"+/-",K_SIGN,1,4},{".",'.',2,4},{"+",'+',3,4},{"=",'=',4,4},
};
#define NKEYS ((int)(sizeof(keys)/sizeof(keys[0])))
#define BW 40
#define BH 24
#define GAP 6
#define LEFT 10
#define DIGITS_X (LEFT+BW+12)
#define TOP 40
#define WIDTH (DIGITS_X+5*BW+4*GAP+LEFT)
#define HEIGHT (TOP+BH+10+4*BH+3*GAP+10)

static HINSTANCE instance;
static HWND main_wnd,buttons[NKEYS];
static BOOL trace,skip_char;
/* The calculator: the displayed number x (typed as entry while typing),
 * the accumulator and pending operator, the last operation for repeated
 * '=', and memory. */
static double x,acc,last_operand,memory;
static int op,last_op;
static BOOL typing,fresh_op,error;
static char entry[32],display[64];

/* --- numbers ---------------------------------------------------------------------- */
static void format(double v,char *out) {
    char digits[16]; int e=0,n,i; double a=v<0?-v:v; __int64 m; char *p=out;
    if(v!=v || a>1.7e308) {lstrcpy(out,"Overflow"); return;}
    if(a==0) {lstrcpy(out,"0."); return;}
    while(a>=1e10) {a/=1e10; e+=10;}
    while(a>=10) {a/=10; e++;}
    while(a<1e-10) {a*=1e10; e-=10;}
    while(a<1) {a*=10; e--;}
    m=(__int64)(a*1e12+0.5);
    if(m>=10000000000000) {m/=10; e++;}
    for(i=12;i>=0;i--) {digits[i]=(char)('0'+(int)(m%10)); m/=10;}
    for(n=13;n>1 && digits[n-1]=='0';n--) {}
    if(v<0) *p++='-';
    if(e>=13 || e<-5) {
        *p++=digits[0]; *p++='.';
        for(i=1;i<n;i++) *p++=digits[i];
        wsprintf(p,"e%c%d",e<0?'-':'+',e<0?-e:e);
        return;
    }
    if(e>=0) {
        for(i=0;i<=e;i++) *p++=i<n?digits[i]:'0';
        *p++='.';
        for(i=e+1;i<n;i++) *p++=digits[i];
    } else {
        *p++='0'; *p++='.';
        for(i=0;i<-e-1;i++) *p++='0';
        for(i=0;i<n;i++) *p++=digits[i];
    }
    *p=0;
}
static double parse(const char *s) {
    double v=0,scale=1; BOOL neg=FALSE,point=FALSE;
    if(*s=='-') {neg=TRUE; s++;}
    for(;*s;s++) {
        if(*s=='.') {point=TRUE; continue;}
        v=v*10+(*s-'0');
        if(point) scale*=10;
    }
    v/=scale;
    return neg?-v:v;
}
static double square_root(double v) {
    double g=v,prev=0; int i;
    if(v==0) return 0;
    while(g>1e20) g/=1e10;
    while(g<1e-20) g*=1e10;
    for(i=0;i<200 && g!=prev;i++) {prev=g; g=(g+v/g)/2;}
    return g;
}

/* --- the display ---------------------------------------------------------------------- */
static void show(void) {
    RECT r;
    if(!error) {
        if(typing) {
            BOOL point=FALSE; const char *p;
            for(p=entry;*p;p++) if(*p=='.') point=TRUE;
            wsprintf(display,point?"%s":"%s.",entry[0] && lstrcmp(entry,"-")?entry:"0");
        } else format(x,display);
    }
    SetRect(&r,LEFT,8,WIDTH-LEFT,8+24);
    InvalidateRect(main_wnd,&r,TRUE);
    r.left=LEFT; r.right=LEFT+BW; r.top=TOP; r.bottom=TOP+BH;
    InvalidateRect(main_wnd,&r,TRUE);
    if(trace) {char line[80]; wsprintf(line,"CALC: %s",display); OutputDebugString(line);}
}
static void fail(LPCSTR message) {error=TRUE; lstrcpy(display,message); typing=FALSE; op=0;}
static double compute(double a,int o,double b) {
    double r;
    switch(o) {
    case '+': r=a+b; break;
    case '-': r=a-b; break;
    case '*': r=a*b; break;
    case '/': if(b==0) {fail("Cannot divide by zero"); return 0;} r=a/b; break;
    default: return b;
    }
    if(r!=r || r>1.7e308 || r<-1.7e308) {fail("Overflow"); return 0;}
    return r;
}
static void settle(void) {if(typing) {x=parse(entry); typing=FALSE;}}
static void clear_all(void) {x=acc=last_operand=0; op=last_op=0; typing=fresh_op=error=FALSE; entry[0]=0;}

/* One key of the calculator. */
static void press(int key) {
    if(error && key!=K_C && key!=K_CE && !(key>='0' && key<='9') && key!='.') {MessageBeep(0); return;}
    if(error) {clear_all();}
    if((key>='0' && key<='9') || key=='.') {
        int digits=0; const char *p; BOOL point=FALSE;
        if(!typing) {entry[0]=0; typing=TRUE; fresh_op=FALSE;}
        for(p=entry;*p;p++) {if(*p>='0' && *p<='9') digits++; if(*p=='.') point=TRUE;}
        if(key=='.') {if(point) {MessageBeep(0); return;} lstrcat(entry,entry[0] && lstrcmp(entry,"-")?".":"0.");}
        else if(digits>=13) {MessageBeep(0); return;}
        else if(!lstrcmp(entry,"0")) entry[0]=(char)key;
        else if(!lstrcmp(entry,"-0")) entry[1]=(char)key;
        else {int n=lstrlen(entry); entry[n]=(char)key; entry[n+1]=0;}
        show();
        return;
    }
    switch(key) {
    case '+': case '-': case '*': case '/':
        settle();
        if(op && !fresh_op) {x=compute(acc,op,x); if(error) break;}
        acc=x; op=key; fresh_op=TRUE;
        break;
    case '=':
        settle();
        if(op) {last_op=op; last_operand=x; x=compute(acc,op,x); op=0;}
        else if(last_op) x=compute(x,last_op,last_operand);
        acc=x; fresh_op=FALSE;
        break;
    case K_C: clear_all(); break;
    case K_CE: x=0; typing=FALSE; entry[0]=0; break;
    case K_BACK:
        if(!typing) {MessageBeep(0); return;}
        {int n=lstrlen(entry); if(n) entry[n-1]=0; if(!entry[0] || !lstrcmp(entry,"-")) lstrcpy(entry,"0");}
        break;
    case K_SIGN:
        if(typing) {
            if(entry[0]=='-') lstrcpy(entry,entry+1);
            else {char t[32]; t[0]='-'; lstrcpy(t+1,entry); lstrcpy(entry,t);}
        } else x=-x;
        break;
    case K_SQRT: settle(); if(x<0) fail("Invalid input for function"); else x=square_root(x); break;
    case K_RECIP: settle(); if(x==0) fail("Cannot divide by zero"); else x=1/x; break;
    case K_PERCENT: settle(); x=op?acc*x/100:0; break;
    case K_MC: memory=0; break;
    case K_MR: x=memory; typing=FALSE; break;
    case K_MS: settle(); memory=x; break;
    case K_MPLUS: settle(); memory+=x; break;
    default: return;
    }
    show();
}
/* Keys as typed or pasted. */
static int key_of_char(int c) {
    switch(c) {
    case '\r': case '=': return '=';
    case 0x1b: return K_C;
    case '\b': return K_BACK;
    case '@': return K_SQRT;
    case '%': return K_PERCENT;
    case 'r': case 'R': return K_RECIP;
    case ',': return '.';
    }
    if((c>='0' && c<='9') || c=='.' || c=='+' || c=='-' || c=='*' || c=='/') return c;
    return 0;
}
static void flash(int key) {
    int i;
    for(i=0;i<NKEYS;i++) if(keys[i].key==key && buttons[i]) {
        SendMessage(buttons[i],BM_SETSTATE,TRUE,0);
        SetTimer(main_wnd,(UINT_PTR)(100+i),120,NULL);
    }
}
static void typed(int key) {if(key) {flash(key); press(key);}}

/* --- the clipboard -------------------------------------------------------------------- */
static void copy(void) {
    HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)lstrlen(display)+1); char *p;
    if(!g) return;
    p=(char *)GlobalLock(g); lstrcpy(p,display); GlobalUnlock(g);
    if(OpenClipboard(main_wnd)) {EmptyClipboard(); SetClipboardData(CF_TEXT,g); CloseClipboard();}
    else GlobalFree(g);
}
static void paste(void) {
    HGLOBAL g; const char *p;
    if(!OpenClipboard(main_wnd)) return;
    if((g=GetClipboardData(CF_TEXT))!=NULL && (p=(const char *)GlobalLock(g))!=NULL) {
        char text[256]; int i;
        lstrcpyn(text,p,sizeof(text)); GlobalUnlock(g); CloseClipboard();
        for(i=0;text[i];i++) {
            /* An "e" between digits is not a key; Windows types the text as keys. */
            int k=key_of_char(text[i]);
            if(k) press(k);
        }
        return;
    }
    CloseClipboard();
}

/* --- the window ------------------------------------------------------------------------- */
static void paint(HDC dc) {
    RECT r; HGDIOBJ old;
    SetRect(&r,LEFT,8,WIDTH-LEFT,8+24);
    FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); InflateRect(&r,-1,-1);
    FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
    old=SelectObject(dc,GetStockObject(SYSTEM_FONT));
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(0,0,0));
    r.right-=6;
    DrawText(dc,display,-1,&r,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    /* The memory indicator. */
    SetRect(&r,LEFT,TOP,LEFT+BW,TOP+BH);
    FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); InflateRect(&r,-1,-1);
    FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
    if(memory!=0) DrawText(dc,"M",1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,old);
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        int i;
        main_wnd=h;
        for(i=0;i<NKEYS;i++) {
            int bx=keys[i].col<0?LEFT:DIGITS_X+keys[i].col*(BW+GAP);
            int by=keys[i].row==0?TOP:TOP+BH+10+(keys[i].row-1)*(BH+GAP);
            buttons[i]=CreateWindow("BUTTON",keys[i].label,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,bx,by,BW,BH,h,(HMENU)(ULONG_PTR)(1000+i),instance,NULL);
        }
        clear_all(); show();
        return 0;
    }
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(dc); EndPaint(h,&ps); return 0;}
    case WM_CTLCOLORBTN: SetBkColor((HDC)wp,GetSysColor(COLOR_BTNFACE)); return (LRESULT)GetStockObject(LTGRAY_BRUSH);
    case WM_COMMAND:
        if(LOWORD(wp)>=1000 && LOWORD(wp)<1000+NKEYS) {press(keys[LOWORD(wp)-1000].key); SetFocus(h); return 0;}
        if(HelpCommand(h,LOWORD(wp),"CALC.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_COPY: copy(); return 0;
        case IDM_PASTE: paste(); return 0;
        case IDM_ABOUT: MessageBox(h,"Calculator\nThe standard calculator.","About Calculator",MB_OK|MB_ICONINFORMATION); return 0;
        }
        return 0;
    case WM_TIMER:
        if(wp>=100 && wp<100+(WPARAM)NKEYS) {KillTimer(h,wp); SendMessage(buttons[wp-100],BM_SETSTATE,FALSE,0);}
        return 0;
    case WM_KEYDOWN:
        if(GetKeyState(VK_CONTROL)<0) {
            int k=wp=='L'?K_MC:wp=='R'?K_MR:wp=='M'?K_MS:wp=='P'?K_MPLUS:0;
            if(k) {skip_char=TRUE; typed(k);}
            return 0;
        }
        if(wp==VK_DELETE) typed(K_CE);
        else if(wp==VK_F9) typed(K_SIGN);
        return 0;
    case WM_CHAR:
        if(skip_char) {skip_char=FALSE; return 0;}
        typed(key_of_char((int)wp));
        return 0;
    case WM_DESTROY: WinHelp(h,"CALC.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show_cmd) {
    WNDCLASS wc; MSG m; HACCEL accel; RECT r; DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    instance=inst;
    trace=command && !lstrcmpi(command,"/trace");
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"CALC"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.lpszMenuName="CALC"; wc.lpszClassName="Calculator";
        RegisterClass(&wc);
    }
    SetRect(&r,0,0,WIDTH,HEIGHT); AdjustWindowRect(&r,style,TRUE);
    main_wnd=CreateWindow("Calculator","Calculator",style,CW_USEDEFAULT,0,r.right-r.left,r.bottom-r.top,NULL,NULL,inst,NULL);
    ShowWindow(main_wnd,show_cmd); UpdateWindow(main_wnd);
    if(trace) {POINT p; char line[64]; p.x=p.y=0; ClientToScreen(main_wnd,&p); wsprintf(line,"CALC: client %d %d",p.x,p.y); OutputDebugString(line);}
    accel=LoadAccelerators(inst,"CALC");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
