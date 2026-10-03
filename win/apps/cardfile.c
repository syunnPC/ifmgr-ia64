/* SPDX-License-Identifier: GPL-2.0-or-later
 * Cardfile keeps cards sorted by index line in a stack or list view.
 * .CRD text format: "CARDFILE", then per card "I:" plus index, one "B:"
 * line per body line, and "E". /trace reports cards after changes.
 */
#include <windows.h>
#include "winapp.h"
#include "cardfile.h"
#define CARDS 512
#define INDEX_LEN 40
#define BODY_LINES 11

typedef struct {char index[INDEX_LEN+1]; char *body;} Card;
static HINSTANCE instance;
static HWND main_wnd,body,list;
static Card cards[CARDS];
static int count,front;
static BOOL list_view,modified,trace;
static char file[260],find_text[80];
static int cw=8,ch=13;
#define IH (ch+4)
#define CARD_W (INDEX_LEN*cw+6)
#define CARD_H (IH+3+BODY_LINES*ch+6)
#define DX 12
#define DY (IH+1)
#define STATUS_H (ch+6)

/* --- cards ------------------------------------------------------------------------ */
static char *copy_text(LPCSTR s) {
    int n=s?lstrlen(s):0; char *t=(char *)GlobalAlloc(GPTR,(DWORD)n+1);
    if(t && n) memcpy(t,s,(size_t)n);
    return t;
}
static void free_cards(void) {int i; for(i=0;i<count;i++) if(cards[i].body) GlobalFree(cards[i].body); count=front=0;}
static void report(void) {
    char text[600]; int i,n;
    if(!trace) return;
    n=wsprintf(text,"CARDFILE: cards ");
    for(i=0;i<count && n<500;i++) n+=wsprintf(text+n,"%s%s",i?",":"",cards[i].index);
    wsprintf(text+n," front %s",count?cards[front].index:"");
    OutputDebugString(text);
}
/* Keep the cards in index order; returns where card i went. */
static int sort_card(int i) {
    Card c=cards[i];
    while(i>0 && lstrcmpi(cards[i-1].index,c.index)>0) {cards[i]=cards[i-1]; i--;}
    while(i<count-1 && lstrcmpi(cards[i+1].index,c.index)<0) {cards[i]=cards[i+1]; i++;}
    cards[i]=c;
    return i;
}
static int add_card(LPCSTR index,LPCSTR text) {
    if(count==CARDS) return -1;
    lstrcpyn(cards[count].index,index,sizeof(cards[0].index));
    cards[count].body=copy_text(text);
    count++;
    return sort_card(count-1);
}
static void remove_card(int i) {
    if(cards[i].body) GlobalFree(cards[i].body);
    for(;i+1<count;i++) cards[i]=cards[i+1];
    count--;
}

/* --- the window ---------------------------------------------------------------------- */
static int back_cards(void) {
    RECT r; int by_h,by_w;
    GetClientRect(main_wnd,&r);
    by_h=((int)r.bottom-STATUS_H-8-CARD_H)/DY; by_w=((int)r.right-16-CARD_W)/DX;
    return max(0,min(count-1,min(by_h,by_w)));
}
static void front_origin(POINT *p) {p->x=8; p->y=STATUS_H+4+back_cards()*DY;}
/* The front card's text goes back into the card when it was edited. */
static void store_body(void) {
    int n; char *t;
    if(!count || !SendMessage(body,EM_GETMODIFY,0,0)) return;
    n=GetWindowTextLength(body);
    if(!(t=(char *)GlobalAlloc(GPTR,(DWORD)n+1))) return;
    GetWindowText(body,t,n+1);
    if(cards[front].body) GlobalFree(cards[front].body);
    cards[front].body=t;
    SendMessage(body,EM_SETMODIFY,FALSE,0);
    modified=TRUE;
}
static void layout(void) {
    RECT r; POINT p;
    GetClientRect(main_wnd,&r); front_origin(&p);
    MoveWindow(body,p.x+3,p.y+IH+3,CARD_W-6,CARD_H-IH-6,TRUE);
    MoveWindow(list,0,STATUS_H,r.right,max(0,(int)r.bottom-STATUS_H),TRUE);
    ShowWindow(body,list_view?SW_HIDE:SW_SHOW);
    ShowWindow(list,list_view?SW_SHOW:SW_HIDE);
}
static void fill_list(void) {
    int i;
    SendMessage(list,LB_RESETCONTENT,0,0);
    for(i=0;i<count;i++) SendMessage(list,LB_ADDSTRING,0,(LPARAM)cards[i].index);
    SendMessage(list,LB_SETCURSEL,(WPARAM)front,0);
}
/* Show card i at the front. */
static void bring(int i) {
    if(!count) return;
    store_body();
    front=(i%count+count)%count;
    SetWindowText(body,cards[front].body?cards[front].body:"");
    SendMessage(body,EM_SETMODIFY,FALSE,0);
    fill_list(); layout();
    InvalidateRect(main_wnd,NULL,TRUE);
    report();
}
static void set_title(void) {
    char title[300];
    wsprintf(title,"Cardfile - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(main_wnd,title);
}
static void paint(HDC dc) {
    RECT r,c; POINT p; int i,back=back_cards(); char text[40]; HGDIOBJ old=SelectObject(dc,GetStockObject(SYSTEM_FONT));
    GetClientRect(main_wnd,&c);
    SetBkMode(dc,TRANSPARENT);
    wsprintf(text,"%d Card%s",count,count==1?"":"s");
    SetRect(&r,8,2,c.right-8,STATUS_H);
    DrawText(dc,list_view?"List View":"Card View",-1,&r,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    DrawText(dc,text,-1,&r,DT_RIGHT|DT_SINGLELINE|DT_NOPREFIX);
    SetRect(&r,0,STATUS_H-1,c.right,STATUS_H); FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
    if(list_view || !count) {SelectObject(dc,old); return;}
    front_origin(&p);
    for(i=back;i>=0;i--) {
        Card *k=&cards[(front+i)%count];
        SetRect(&r,p.x+i*DX,p.y-i*DY,p.x+i*DX+CARD_W,p.y-i*DY+CARD_H);
        FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
        FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        TextOut(dc,r.left+4,r.top+2,k->index,lstrlen(k->index));
        if(!i) {
            RECT l; SetRect(&l,r.left,r.top+IH,r.right,r.top+IH+1); FillRect(dc,&l,(HBRUSH)GetStockObject(BLACK_BRUSH));
            OffsetRect(&l,0,2); FillRect(dc,&l,(HBRUSH)GetStockObject(BLACK_BRUSH));
        }
    }
    SelectObject(dc,old);
}
/* A click on a card's index line behind the front card brings it forward. */
static void click(int x,int y) {
    POINT p,pt; int i,back=back_cards();
    if(list_view) return;
    front_origin(&p); pt.x=x; pt.y=y;
    for(i=1;i<=back;i++) {
        RECT r; SetRect(&r,p.x+i*DX,p.y-i*DY,p.x+i*DX+CARD_W,p.y-i*DY+DY);
        if(PtInRect(&r,pt)) {bring(front+i); return;}
    }
}

/* --- files ---------------------------------------------------------------------------- */
static void message(LPCSTR format,LPCSTR name,UINT type) {
    char text[400]; wsprintf(text,format,name); MessageBox(main_wnd,text,"Cardfile",type);
}
static void blank(void) {free_cards(); add_card("",""); front=0;}
static BOOL save_to(LPCSTR path) {
    DWORD size=16,n=0; int i; char *out; BOOL ok;
    store_body();
    for(i=0;i<count;i++) size+=(DWORD)lstrlen(cards[i].index)+8+(cards[i].body?(DWORD)lstrlen(cards[i].body)*2+8:0)+8;
    if(!(out=(char *)GlobalAlloc(GPTR,size))) {message("Not enough memory to save %s.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    n+=(DWORD)wsprintf(out+n,"CARDFILE\r\n");
    for(i=0;i<count;i++) {
        const char *b=cards[i].body?cards[i].body:"";
        n+=(DWORD)wsprintf(out+n,"I:%s\r\n",cards[i].index);
        while(*b) {
            const char *e=b; while(*e && *e!='\r' && *e!='\n') e++;
            out[n++]='B'; out[n++]=':'; memcpy(out+n,b,(size_t)(e-b)); n+=(DWORD)(e-b); out[n++]='\r'; out[n++]='\n';
            if(*e=='\r') e++;
            if(*e=='\n') e++;
            b=e;
        }
        n+=(DWORD)wsprintf(out+n,"E\r\n");
    }
    ok=WriteWholeFile(path,out,n);
    GlobalFree(out);
    if(!ok) {message("Cannot write to the %s file.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    modified=FALSE;
    return TRUE;
}
static BOOL load(LPCSTR path) {
    DWORD size=0; char *text=ReadWholeFile(path,&size),*p,*b=NULL,index[INDEX_LEN+1]; int blen=0;
    if(!text) {message("Cannot open the %s file.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    if(memcmp(text,"CARDFILE",8)) {GlobalFree(text); message("%s is not a Cardfile file.",path,MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    free_cards();
    index[0]=0;
    for(p=text;*p;) {
        char *e=p,*next; while(*e && *e!='\r' && *e!='\n') e++;
        next=e; if(*next=='\r') next++; if(*next=='\n') next++;
        *e=0;
        if(p[0]=='I' && p[1]==':') {
            lstrcpyn(index,p+2,sizeof(index));
            if(b) GlobalFree(b);
            b=(char *)GlobalAlloc(GPTR,size+1); blen=0;
        }
        else if(p[0]=='B' && p[1]==':' && b) {int n=lstrlen(p+2); if(blen) {b[blen++]='\r'; b[blen++]='\n';} memcpy(b+blen,p+2,(size_t)n); blen+=n; b[blen]=0;}
        else if(p[0]=='E' && !p[1] && b) {add_card(index,b); GlobalFree(b); b=NULL;}
        p=next;
    }
    if(b) GlobalFree(b);
    GlobalFree(text);
    if(!count) add_card("","");
    front=0;
    lstrcpy(file,path); AnsiUpper(file); set_title();
    modified=FALSE;
    SendMessage(body,EM_SETMODIFY,FALSE,0);
    bring(0);
    return TRUE;
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.CRD");
    if(!FileSaveDialog(main_wnd,"Save As","*.CRD",path,sizeof(path)) || !save_to(path)) return FALSE;
    lstrcpy(file,path); set_title();
    return TRUE;
}
static BOOL save(void) {return file[0]?save_to(file):save_as();}
static BOOL query_save(void) {
    char text[400];
    store_body();
    if(!modified) return TRUE;
    wsprintf(text,"Save current changes to %s?",file[0]?FileTitle(file):"(Untitled)");
    switch(MessageBox(main_wnd,text,"Cardfile",MB_YESNOCANCEL|MB_ICONEXCLAMATION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}

/* --- dialogs ---------------------------------------------------------------------------- */
typedef struct {LPCSTR title,prompt; char *text; int size;} Ask;
static INT_PTR CALLBACK AskProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Ask *a=(Ask *)GetProp(h,"Ask");
    switch(msg) {
    case WM_INITDIALOG:
        a=(Ask *)lp; SetProp(h,"Ask",(HANDLE)a);
        SetWindowText(h,a->title); SetDlgItemText(h,IDC_PROMPT,a->prompt); SetDlgItemText(h,IDC_TEXT,a->text);
        SendDlgItemMessage(h,IDC_TEXT,EM_LIMITTEXT,(WPARAM)(a->size-1),0);
        SendDlgItemMessage(h,IDC_TEXT,EM_SETSEL,0,-1);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {GetDlgItemText(h,IDC_TEXT,a->text,a->size); EndDialog(h,IDOK); return TRUE;}
        if(LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
        return FALSE;
    case WM_DESTROY: RemoveProp(h,"Ask"); return FALSE;
    }
    return FALSE;
}
static BOOL ask(LPCSTR title,LPCSTR prompt,char *text,int size) {
    Ask a; a.title=title; a.prompt=prompt; a.text=text; a.size=size;
    return DialogBoxParam(instance,"TEXTDLG",main_wnd,AskProc,(LPARAM)&a)==IDOK;
}
static BOOL contains(LPCSTR text,LPCSTR word) {
    int n=lstrlen(word),i;
    if(!n) return FALSE;
    for(;text && *text;text++) {
        for(i=0;i<n && text[i] && (BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)text[i])==(BYTE)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)word[i]);i++) {}
        if(i==n) return TRUE;
    }
    return FALSE;
}
static void find_next(void) {
    int i;
    store_body();
    for(i=1;i<=count;i++) if(contains(cards[(front+i)%count].body,find_text)) {bring(front+i); return;}
    message("Cannot find \"%s\".",find_text,MB_OK|MB_ICONINFORMATION);
}
static void command(HWND h,UINT id) {
    char text[INDEX_LEN+1],path[260]; int i;
    if(HelpCommand(h,id,"CARDFILE.HLP")) return;
    switch(id) {
    case IDM_NEW: if(query_save()) {blank(); file[0]=0; modified=FALSE; set_title(); SendMessage(body,EM_SETMODIFY,FALSE,0); bring(0);} return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.CRD");
        if(FileOpenDialog(h,"Open","*.CRD",path,sizeof(path))) load(path);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_UNDO: SendMessage(body,WM_UNDO,0,0); return;
    case IDM_CUT: SendMessage(body,WM_CUT,0,0); return;
    case IDM_COPY: SendMessage(body,WM_COPY,0,0); return;
    case IDM_PASTE: SendMessage(body,WM_PASTE,0,0); return;
    case IDM_INDEX:
        lstrcpy(text,cards[front].index);
        if(!ask("Index","&Index Line:",text,sizeof(text))) return;
        store_body();
        lstrcpy(cards[front].index,text); front=sort_card(front); modified=TRUE;
        bring(front);
        return;
    case IDM_CARDS: case IDM_LIST:
        store_body();
        list_view=id==IDM_LIST;
        fill_list(); layout(); InvalidateRect(h,NULL,TRUE);
        SetFocus(list_view?list:body);
        return;
    case IDM_ADD:
        text[0]=0;
        if(!ask("Add","&Add:",text,sizeof(text))) return;
        store_body();
        if((i=add_card(text,""))<0) {MessageBox(h,"There are too many cards.","Cardfile",MB_OK|MB_ICONEXCLAMATION); return;}
        modified=TRUE; front=i; bring(i); SetFocus(body);
        return;
    case IDM_DELETE: {
        char q[120];
        wsprintf(q,"Delete \"%s\"?",cards[front].index);
        if(MessageBox(h,q,"Cardfile",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;
        SendMessage(body,EM_SETMODIFY,FALSE,0);
        remove_card(front);
        if(!count) add_card("","");
        modified=TRUE;
        if(front>=count) front=0;
        bring(front);
        return;
    }
    case IDM_DUPLICATE:
        store_body();
        if((i=add_card(cards[front].index,cards[front].body))>=0) {modified=TRUE; bring(i);}
        return;
    case IDM_GOTO:
        text[0]=0;
        if(!ask("Go To","&Go To:",text,sizeof(text)) || !text[0]) return;
        for(i=1;i<=count;i++) if(contains(cards[(front+i)%count].index,text)) {bring(front+i); return;}
        message("Cannot find \"%s\".",text,MB_OK|MB_ICONINFORMATION);
        return;
    case IDM_FIND:
        if(!ask("Find","&Find:",find_text,sizeof(find_text)) || !find_text[0]) return;
        find_next();
        return;
    case IDM_FINDNEXT: if(find_text[0]) find_next(); else command(h,IDM_FIND); return;
    case IDM_NEXT: bring(front+1); return;
    case IDM_PREVIOUS: bring(front-1); return;
    case IDM_ABOUT: MessageBox(h,"Cardfile\nKeeps notes on index cards.","About Cardfile",MB_OK|MB_ICONINFORMATION); return;
    }
}
LRESULT CALLBACK MainProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        HDC dc=GetDC(h); TEXTMETRIC tm; GetTextMetrics(dc,&tm); ReleaseDC(h,dc);
        cw=(int)tm.tmAveCharWidth; ch=(int)tm.tmHeight;
        main_wnd=h;
        body=CreateWindow("EDIT",NULL,WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL,0,0,10,10,h,(HMENU)IDC_BODY,instance,NULL);
        list=CreateWindow("LISTBOX",NULL,WS_CHILD|WS_VSCROLL|LBS_NOTIFY,0,0,10,10,h,(HMENU)IDC_LIST,instance,NULL);
        return 0;
    }
    case WM_SIZE: layout(); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(dc); EndPaint(h,&ps); return 0;}
    case WM_SETFOCUS: SetFocus(list_view?list:body); return 0;
    case WM_LBUTTONDOWN: click(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); return 0;
    case WM_INITMENUPOPUP:
        CheckMenuItem((HMENU)wp,IDM_CARDS,MF_BYCOMMAND|(list_view?MF_UNCHECKED:MF_CHECKED));
        CheckMenuItem((HMENU)wp,IDM_LIST,MF_BYCOMMAND|(list_view?MF_CHECKED:MF_UNCHECKED));
        return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_LIST) {
            if(HIWORD(wp)==LBN_SELCHANGE) {LRESULT s=SendMessage(list,LB_GETCURSEL,0,0); if(s>=0 && s!=front) {front=(int)s; report();}}
            if(HIWORD(wp)==LBN_DBLCLK) command(h,IDM_CARDS);
            return 0;
        }
        if(LOWORD(wp)==IDC_BODY) return 0;
        command(h,LOWORD(wp));
        return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"CARDFILE.HLP",HELP_QUIT,0); free_cards(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR cmdline,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; char arg[260]; int i=0;
    instance=inst;
    while(cmdline && *cmdline==' ') cmdline++;
    if(cmdline && (cmdline[0]=='/' || cmdline[0]=='-') && (cmdline[1]|0x20)=='t') {trace=TRUE; while(*cmdline && *cmdline!=' ') cmdline++; while(*cmdline==' ') cmdline++;}
    while(cmdline && cmdline[i] && cmdline[i]!=' ' && i<(int)sizeof(arg)-1) {arg[i]=cmdline[i]; i++;}
    arg[i]=0;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc=MainProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"CARDFILE"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_APPWORKSPACE+1); wc.lpszMenuName="CARDFILE"; wc.lpszClassName="Cardfile";
        RegisterClass(&wc);
    }
    blank();
    main_wnd=CreateWindow("Cardfile","Cardfile",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    set_title();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    if(trace) {
        char text[64]; POINT c; c.x=c.y=0; ClientToScreen(main_wnd,&c);
        wsprintf(text,"CARDFILE: client %d %d",c.x,c.y); OutputDebugString(text);
    }
    if(arg[0]) load(arg); else report();
    SetFocus(body);
    accel=LoadAccelerators(inst,"CARDFILE");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
