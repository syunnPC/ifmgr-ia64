/* SPDX-License-Identifier: GPL-2.0-or-later
 * Program Manager: MDI groups of icons, descriptions and command lines.
 * PROGMAN.INI stores groups/settings in text instead of .GRP format. First
 * startup creates Main, Accessories and Games from installed programs.
 * WIN.INI load=/run= starts programs; closing the shell ends the session.
 *
 * PROGMAN DDE application/topic supports CreateGroup, ShowGroup, AddItem,
 * DeleteGroup, DeleteItem, ReplaceItem, ExitProgman and Reload. Requests for
 * Groups or a group name return group names or items.
 */
#include <windows.h>
#include <dde.h>
#include "winapp.h"
#include "progman.h"
#define GROUPS 16
#define ITEMS 64
#define CELL_W 84
#define CELL_H 64
#define INI "PROGMAN.INI"

typedef struct {char title[64],command[160]; HICON icon;} Item;
typedef struct {BOOL used; char name[64]; HWND wnd; Item items[ITEMS]; int count,selected,top; RECT placement; int state;} Group;

static HINSTANCE instance;
static HWND frame,client;
static Group groups[GROUPS];
static BOOL auto_arrange=TRUE,min_on_run=FALSE,save_settings=TRUE;

/* --- items ---------------------------------------------------------------------- */
/* The program a command line runs, with its directory found as OpenFile does. */
static void program_path(LPCSTR command,char *out,int size) {
    char name[160]; OFSTRUCT of; int i=0; LPCSTR base;
    while(*command==' ') command++;
    while(command[i] && command[i]!=' ' && i<(int)sizeof(name)-5) {name[i]=command[i]; i++;}
    name[i]=0;
    base=FileTitle(name);
    {const char *p; BOOL dot=FALSE; for(p=base;*p;p++) if(*p=='.') dot=TRUE; if(!dot) lstrcat(name,".EXE");}
    if(OpenFile(name,&of,OF_EXIST)!=HFILE_ERROR) lstrcpyn(out,of.szPathName,size);
    else lstrcpyn(out,name,size);
}
/* Whether a file is a Windows program (PE subsystem 2); DOS programs here
 * are EFI applications, and batch files run under DOS too. */
static BOOL windows_program(LPCSTR path) {
    HFILE h=_lopen(path,OF_READ); BYTE head[64],pe[96]; BOOL yes=FALSE; int n=lstrlen(path);
    if(h==HFILE_ERROR) return !(n>4 && (!lstrcmpi(path+n-4,".COM") || !lstrcmpi(path+n-4,".BAT")));
    if(_lread(h,head,64)==64 && head[0]=='M' && head[1]=='Z') {
        DWORD at=*(const DWORD *)(head+60);
        if(_llseek(h,(LONG)at,0)==(LONG)at && _lread(h,pe,sizeof(pe))==sizeof(pe) && !memcmp(pe,"PE\0\0",4))
            yes=*(const WORD *)(pe+24+68)==2;
    }
    _lclose(h);
    return yes;
}
static void load_icon(Item *it) {
    char path[160];
    program_path(it->command,path,sizeof(path));
    it->icon=ExtractIcon(instance,path,0);
    if((ULONG_PTR)it->icon<=1) {BOOL win=windows_program(path); it->icon=LoadIcon(win?NULL:instance,win?IDI_APPLICATION:"DOS");}
}
static void free_icon(Item *it) {if(it->icon) DestroyIcon(it->icon); it->icon=NULL;}
static Item *add_item(Group *g,LPCSTR title,LPCSTR command) {
    Item *it;
    if(g->count==ITEMS) return NULL;
    it=&g->items[g->count++];
    lstrcpyn(it->title,title,sizeof(it->title)); lstrcpyn(it->command,command,sizeof(it->command));
    load_icon(it);
    return it;
}
static void remove_item(Group *g,int i) {
    free_icon(&g->items[i]);
    for(;i+1<g->count;i++) g->items[i]=g->items[i+1];
    g->count--;
    if(g->selected>=g->count) g->selected=g->count-1;
}
static int run(LPCSTR command,int show) {
    UINT r=WinExec(command,(UINT)show);
    if(r<32) {
        char text[256]; wsprintf(text,"Cannot run %s.\nCheck that the program exists and the command line is correct.",command);
        MessageBox(frame,text,"Program Manager",MB_OK|MB_ICONEXCLAMATION);
        return 0;
    }
    if(min_on_run) ShowWindow(frame,SW_MINIMIZE);
    return 1;
}

/* --- groups ----------------------------------------------------------------------- */
static Group *group_of(HWND h) {
    LONG_PTR i=h?GetWindowLongPtr(h,0):-1;
    return i>=0 && i<GROUPS && groups[i].used && groups[i].wnd==h?&groups[i]:NULL;
}
static Group *active_group(void) {return group_of((HWND)SendMessage(client,WM_MDIGETACTIVE,0,0));}
static int columns(HWND h) {RECT r; GetClientRect(h,&r); return max(1,(int)r.right/CELL_W);}
static void update_scroll(HWND h,Group *g) {
    RECT r; int rows,visible;
    GetClientRect(h,&r);
    rows=(g->count+columns(h)-1)/columns(h); visible=max(1,(int)r.bottom/CELL_H);
    if(rows>visible) {SetScrollRange(h,SB_VERT,0,rows-visible,FALSE); g->top=min(g->top,rows-visible); SetScrollPos(h,SB_VERT,g->top,TRUE);}
    else {g->top=0; SetScrollRange(h,SB_VERT,0,0,TRUE);}
}
static void refresh(Group *g) {if(g->wnd) {update_scroll(g->wnd,g); InvalidateRect(g->wnd,NULL,TRUE);}}
static void cell(HWND h,Group *g,int i,RECT *r) {
    int cols=columns(h),x=(i%cols)*CELL_W,y=(i/cols-g->top)*CELL_H;
    SetRect(r,x,y,x+CELL_W,y+CELL_H);
}
static void paint_group(HWND h,Group *g,HDC dc) {
    int i; BOOL active=active_group()==g && GetActiveWindow()==frame;
    SetBkMode(dc,TRANSPARENT);
    for(i=0;i<g->count;i++) {
        RECT c,t; Item *it=&g->items[i];
        cell(h,g,i,&c);
        if(it->icon) DrawIcon(dc,c.left+(CELL_W-32)/2,c.top+4,it->icon);
        SetRect(&t,c.left+2,c.top+38,c.right-2,c.bottom);
        DrawText(dc,it->title,-1,&t,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX|DT_CALCRECT);
        OffsetRect(&t,((c.right-c.left-4)-(t.right-t.left))/2,0);
        if(i==g->selected && active) {
            RECT b=t; InflateRect(&b,1,0); FillRect(dc,&b,(HBRUSH)(COLOR_HIGHLIGHT+1));
            SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        DrawText(dc,it->title,-1,&t,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
    }
}
static int item_at(HWND h,Group *g,int x,int y) {
    int i;
    for(i=0;i<g->count;i++) {RECT c; POINT p; p.x=x; p.y=y; cell(h,g,i,&c); if(PtInRect(&c,p)) return i;}
    return -1;
}
static void select_item(HWND h,Group *g,int i) {
    RECT r; int rows,visible;
    if(i<0 || i>=g->count) return;
    g->selected=i;
    GetClientRect(h,&r); visible=max(1,(int)r.bottom/CELL_H); rows=i/columns(h);
    if(rows<g->top) g->top=rows;
    else if(rows>=g->top+visible) g->top=rows-visible+1;
    SetScrollPos(h,SB_VERT,g->top,TRUE);
    InvalidateRect(h,NULL,TRUE);
}
LRESULT CALLBACK GroupProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Group *g=group_of(h);
    switch(msg) {
    case WM_CREATE: {
        LPMDICREATESTRUCT mcs=(LPMDICREATESTRUCT)((LPCREATESTRUCT)lp)->lpCreateParams;
        SetWindowLongPtr(h,0,mcs->lParam);
        groups[mcs->lParam].wnd=h;
        return 0;
    }
    case WM_SIZE: if(g) update_scroll(h,g); InvalidateRect(h,NULL,TRUE); break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        if(g) paint_group(h,g,dc);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_MDIACTIVATE: InvalidateRect(h,NULL,TRUE); break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if(g) {
            int i=item_at(h,g,GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
            if(i>=0) {select_item(h,g,i); if(msg==WM_LBUTTONDBLCLK) run(g->items[i].command,SW_SHOWNORMAL);}
        }
        break;
    case WM_KEYDOWN:
        if(g && g->count) {
            int i=g->selected<0?0:g->selected,cols=columns(h);
            switch(wp) {
            case VK_LEFT: i--; break;
            case VK_RIGHT: i++; break;
            case VK_UP: i-=cols; break;
            case VK_DOWN: i+=cols; break;
            case VK_HOME: i=0; break;
            case VK_END: i=g->count-1; break;
            case VK_RETURN: if(g->selected>=0) run(g->items[g->selected].command,SW_SHOWNORMAL); return 0;
            default: return 0;
            }
            if(i>=0 && i<g->count) select_item(h,g,i);
        }
        return 0;
    case WM_VSCROLL:
        if(g) {
            int lo,hi,top=g->top;
            GetScrollRange(h,SB_VERT,&lo,&hi);
            switch(LOWORD(wp)) {
            case SB_LINEUP: case SB_PAGEUP: top--; break;
            case SB_LINEDOWN: case SB_PAGEDOWN: top++; break;
            case SB_THUMBPOSITION: top=(short)HIWORD(wp); break;
            }
            top=max(lo,min(top,hi));
            if(top!=g->top) {g->top=top; SetScrollPos(h,SB_VERT,top,TRUE); InvalidateRect(h,NULL,TRUE);}
        }
        return 0;
    case WM_SYSCOMMAND:
        /* Closing a group minimizes it; only Delete removes it. */
        if((wp&0xfff0)==SC_CLOSE) {ShowWindow(h,SW_MINIMIZE); return 0;}
        break;
    case WM_CLOSE: ShowWindow(h,SW_MINIMIZE); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    }
    return DefMDIChildProc(h,msg,wp,lp);
}
static Group *new_group(LPCSTR name) {
    int i;
    for(i=0;i<GROUPS;i++) if(!groups[i].used) {
        Group *g=&groups[i];
        memset(g,0,sizeof(*g)); g->used=TRUE; g->selected=0;
        lstrcpyn(g->name,name,sizeof(g->name));
        SetRect(&g->placement,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT);
        return g;
    }
    return NULL;
}
static void open_group(Group *g) {
    MDICREATESTRUCT mcs; RECT *p=&g->placement;
    mcs.szClass="PMGroup"; mcs.szTitle=g->name; mcs.hOwner=instance;
    mcs.x=p->left; mcs.y=p->top;
    mcs.cx=p->left==CW_USEDEFAULT?CW_USEDEFAULT:p->right-p->left; mcs.cy=p->left==CW_USEDEFAULT?CW_USEDEFAULT:p->bottom-p->top;
    mcs.style=WS_VSCROLL|(g->state==1?WS_MINIMIZE:0)|(g->state==2?WS_MAXIMIZE:0);
    mcs.lParam=g-groups;
    SendMessage(client,WM_MDICREATE,0,(LPARAM)&mcs);
}
static void delete_group(Group *g) {
    int i;
    for(i=0;i<g->count;i++) free_icon(&g->items[i]);
    if(g->wnd) SendMessage(client,WM_MDIDESTROY,(WPARAM)g->wnd,0);
    g->used=FALSE; g->wnd=NULL;
}

static Group *group_named(LPCSTR name) {
    int i;
    for(i=0;i<GROUPS;i++) if(groups[i].used && !lstrcmpi(groups[i].name,name)) return &groups[i];
    return NULL;
}

/* --- PROGMAN.INI --------------------------------------------------------------------- */
static int number(const char **p) {
    int v=0,neg=0;
    while(**p==' ') (*p)++;
    if(**p=='-') {neg=1; (*p)++;}
    while(**p>='0' && **p<='9') v=v*10+(*(*p)++-'0');
    return neg?-v:v;
}
static BOOL read_placement(LPCSTR section,RECT *r,int *state) {
    char text[64]; const char *p=text;
    if(!GetPrivateProfileString(section,"Window","",text,sizeof(text),INI) || !text[0]) return FALSE;
    r->left=number(&p); r->top=number(&p); r->right=number(&p); r->bottom=number(&p); *state=number(&p);
    return r->right>r->left && r->bottom>r->top;
}
static void write_placement(LPCSTR section,HWND h) {
    char text[64]; RECT r; int state=IsIconic(h)?1:IsZoomed(h)?2:0; POINT p;
    if(state) {
        /* The restored placement is what to remember. */
        ShowWindow(h,SW_RESTORE);
    }
    GetWindowRect(h,&r);
    p.x=r.left; p.y=r.top;
    if(GetParent(h)) ScreenToClient(GetParent(h),&p);
    wsprintf(text,"%d %d %d %d %d",p.x,p.y,p.x+r.right-r.left,p.y+r.bottom-r.top,state);
    WritePrivateProfileString(section,"Window",text,INI);
}
static void save(void) {
    int i,k,n=0; char key[16],section[16],value[240];
    WritePrivateProfileString("Settings","AutoArrange",auto_arrange?"1":"0",INI);
    WritePrivateProfileString("Settings","MinOnRun",min_on_run?"1":"0",INI);
    WritePrivateProfileString("Settings","SaveSettings",save_settings?"1":"0",INI);
    WritePrivateProfileString("Groups",NULL,NULL,INI);
    for(i=0;i<GROUPS;i++) {wsprintf(section,"Group%d",i+1); WritePrivateProfileString(section,NULL,NULL,INI);}
    for(i=0;i<GROUPS;i++) if(groups[i].used) {
        Group *g=&groups[i];
        n++; wsprintf(key,"Group%d",n); WritePrivateProfileString("Groups",key,g->name,INI);
        wsprintf(section,"Group%d",n);
        for(k=0;k<g->count;k++) {
            wsprintf(key,"Item%d",k+1); wsprintf(value,"%s,%s",g->items[k].title,g->items[k].command);
            WritePrivateProfileString(section,key,value,INI);
        }
        if(g->wnd) write_placement(section,g->wnd);
    }
    write_placement("Settings",frame);
}
static void load(void) {
    int i,k; char key[16],section[16],value[240];
    for(i=0;i<GROUPS;i++) {
        Group *g;
        wsprintf(key,"Group%d",i+1);
        if(!GetPrivateProfileString("Groups",key,"",value,sizeof(value),INI) || !value[0]) continue;
        if(!(g=new_group(value))) break;
        wsprintf(section,"Group%d",i+1);
        read_placement(section,&g->placement,&g->state);
        for(k=0;k<ITEMS;k++) {
            char *comma;
            wsprintf(key,"Item%d",k+1);
            if(!GetPrivateProfileString(section,key,"",value,sizeof(value),INI) || !value[0]) break;
            for(comma=value;*comma && *comma!=',';comma++) {}
            if(!*comma) continue;
            *comma=0;
            add_item(g,value,comma+1);
        }
    }
}
/* The first run: groups from the programs that are there. */
static void add_if_present(Group *g,LPCSTR title,LPCSTR program) {
    OFSTRUCT of;
    if(OpenFile(program,&of,OF_EXIST)!=HFILE_ERROR) add_item(g,title,program);
}
static void make_defaults(void) {
    Group *g;
    if((g=new_group("Main"))!=NULL) {
        add_if_present(g,"File Manager","WINFILE.EXE");
        add_if_present(g,"Control Panel","CONTROL.EXE");
        add_if_present(g,"Print Manager","PRINTMAN.EXE");
        add_if_present(g,"Clipboard","CLIPBRD.EXE");
        add_item(g,"DOS Prompt","COMMAND.COM");
        add_if_present(g,"Setup","SETUP.EXE");
        SetRect(&g->placement,8,8,440,200);
    }
    if((g=new_group("Accessories"))!=NULL) {
        add_if_present(g,"Notepad","NOTEPAD.EXE");
        add_if_present(g,"Calculator","CALC.EXE");
        add_if_present(g,"Clock","CLOCK.EXE");
        add_if_present(g,"Write","WRITE.EXE");
        add_if_present(g,"Paintbrush","PBRUSH.EXE");
        add_if_present(g,"Terminal","TERMINAL.EXE");
        add_if_present(g,"Recorder","RECORDER.EXE");
        add_if_present(g,"Cardfile","CARDFILE.EXE");
        add_if_present(g,"Calendar","CALENDAR.EXE");
        add_if_present(g,"PIF Editor","PIFEDIT.EXE");
        g->state=1;
    }
    if((g=new_group("Games"))!=NULL) {
        add_if_present(g,"Reversi","REVERSI.EXE");
        add_if_present(g,"Solitaire","SOL.EXE");
        g->state=1;
    }
}
/* WIN.INI's load= (minimized) and run= programs. */
static void startup_programs(LPCSTR key,int show) {
    char list[256],*p=list;
    GetProfileString("windows",key,"",list,sizeof(list));
    while(*p) {
        char *start;
        while(*p==' ' || *p==',') p++;
        start=p;
        while(*p && *p!=' ' && *p!=',') p++;
        if(*p) *p++=0;
        if(*start) WinExec(start,(UINT)show);
    }
}

/* --- dialogs ---------------------------------------------------------------------------- */
typedef struct {char title[64],command[160]; BOOL item; BOOL minimized; int target; Group *from;} DialogData;
static INT_PTR CALLBACK DialogProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    DialogData *d=(DialogData *)GetProp(h,"Data");
    switch(msg) {
    case WM_INITDIALOG:
        d=(DialogData *)lp; SetProp(h,"Data",(HANDLE)d);
        if(GetDlgItem(h,IDC_DESCRIPTION)) SetDlgItemText(h,IDC_DESCRIPTION,d->title);
        if(GetDlgItem(h,IDC_COMMAND)) SetDlgItemText(h,IDC_COMMAND,d->command);
        if(GetDlgItem(h,IDC_GROUP)) CheckRadioButton(h,IDC_GROUP,IDC_ITEM,d->item?IDC_ITEM:IDC_GROUP);
        if(GetDlgItem(h,IDC_SAVE)) CheckDlgButton(h,IDC_SAVE,save_settings);
        if(GetDlgItem(h,IDC_NAME)) SetDlgItemText(h,IDC_NAME,d->title);
        if(GetDlgItem(h,IDC_TOGROUP)) {
            int i;
            for(i=0;i<GROUPS;i++) if(groups[i].used && &groups[i]!=d->from) {
                LRESULT at=SendDlgItemMessage(h,IDC_TOGROUP,CB_ADDSTRING,0,(LPARAM)groups[i].name);
                SendDlgItemMessage(h,IDC_TOGROUP,CB_SETITEMDATA,(WPARAM)at,i);
            }
            SendDlgItemMessage(h,IDC_TOGROUP,CB_SETCURSEL,0,0);
        }
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            if(GetDlgItem(h,IDC_DESCRIPTION)) GetDlgItemText(h,IDC_DESCRIPTION,d->title,sizeof(d->title));
            if(GetDlgItem(h,IDC_COMMAND)) GetDlgItemText(h,IDC_COMMAND,d->command,sizeof(d->command));
            if(GetDlgItem(h,IDC_GROUP)) d->item=IsDlgButtonChecked(h,IDC_ITEM)!=0;
            if(GetDlgItem(h,IDC_MINIMIZED)) d->minimized=IsDlgButtonChecked(h,IDC_MINIMIZED)!=0;
            if(GetDlgItem(h,IDC_SAVE)) save_settings=IsDlgButtonChecked(h,IDC_SAVE)!=0;
            if(GetDlgItem(h,IDC_TOGROUP)) {
                LRESULT sel=SendDlgItemMessage(h,IDC_TOGROUP,CB_GETCURSEL,0,0);
                d->target=sel<0?-1:(int)SendDlgItemMessage(h,IDC_TOGROUP,CB_GETITEMDATA,(WPARAM)sel,0);
            }
            EndDialog(h,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
        return FALSE;
    case WM_DESTROY: RemoveProp(h,"Data"); return FALSE;
    }
    return FALSE;
}
static BOOL dialog(LPCSTR name,DialogData *d) {return DialogBoxParam(instance,name,frame,DialogProc,(LPARAM)d)==IDOK;}
/* The item a command applies to: the active group's selection. */
static Item *selected(Group **out) {
    Group *g=active_group();
    if(out) *out=g;
    if(!g || IsIconic(g->wnd) || g->selected<0 || g->selected>=g->count) return NULL;
    return &g->items[g->selected];
}
/* The groups are saved when the session ends (WM_ENDSESSION): another
 * program may keep it going. */
static void exit_windows(void) {
    DialogData d; memset(&d,0,sizeof(d));
    if(!dialog("EXITDLG",&d)) return;
    WritePrivateProfileString("Settings","SaveSettings",save_settings?"1":"0",INI);
    ExitWindows(0,0);
}
static void command(HWND h,UINT id) {
    Group *g; Item *it=selected(&g); DialogData d;
    memset(&d,0,sizeof(d));
    if(HelpCommand(h,id,"PROGMAN.HLP")) return;
    switch(id) {
    case IDM_NEW:
        d.item=g!=NULL;
        if(!dialog("NEWDLG",&d)) return;
        if(!d.item) {
            Group *n;
            d.title[0]=0;
            if(!dialog("GROUPDLG",&d) || !d.title[0]) return;
            if(!(n=new_group(d.title))) {MessageBox(h,"There are too many groups.","Program Manager",MB_OK|MB_ICONEXCLAMATION); return;}
            open_group(n);
        } else {
            if(!g) {MessageBox(h,"Open a group for the new item first.","Program Manager",MB_OK|MB_ICONEXCLAMATION); return;}
            if(IsIconic(g->wnd)) ShowWindow(g->wnd,SW_RESTORE);
            if(!dialog("ITEMDLG",&d) || !d.command[0]) return;
            if(!d.title[0]) {lstrcpyn(d.title,FileTitle(d.command),sizeof(d.title));}
            if(add_item(g,d.title,d.command)) {g->selected=g->count-1; refresh(g);}
        }
        return;
    case IDM_OPEN:
        if(g && IsIconic(g->wnd)) {ShowWindow(g->wnd,SW_RESTORE); return;}
        if(it) run(it->command,SW_SHOWNORMAL);
        return;
    case IDM_MOVE: case IDM_COPY: {
        Group *to;
        if(!it) return;
        lstrcpy(d.title,it->title); d.from=id==IDM_MOVE?g:NULL; d.target=-1;
        if(!dialog("MOVEDLG",&d) || d.target<0) return;
        to=&groups[d.target];
        if(!add_item(to,it->title,it->command)) return;
        if(id==IDM_MOVE) remove_item(g,g->selected);
        refresh(to); refresh(g);
        return;
    }
    case IDM_DELETE: {
        char text[160];
        if(it) {
            wsprintf(text,"Are you sure you want to delete the item '%s'?",it->title);
            if(MessageBox(h,text,"Delete",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
            remove_item(g,g->selected); refresh(g);
        } else if(g) {
            wsprintf(text,"Are you sure you want to delete the group '%s'?",g->name);
            if(MessageBox(h,text,"Delete",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
            delete_group(g);
        }
        return;
    }
    case IDM_PROPERTIES:
        if(it) {
            lstrcpy(d.title,it->title); lstrcpy(d.command,it->command);
            if(!dialog("ITEMDLG",&d)) return;
            lstrcpy(it->title,d.title);
            if(lstrcmp(it->command,d.command)) {lstrcpy(it->command,d.command); free_icon(it); load_icon(it);}
            InvalidateRect(g->wnd,NULL,TRUE);
        } else if(g) {
            lstrcpy(d.title,g->name);
            if(!dialog("GROUPDLG",&d) || !d.title[0]) return;
            lstrcpy(g->name,d.title); SetWindowText(g->wnd,g->name);
        }
        return;
    case IDM_RUN:
        if(dialog("RUNDLG",&d) && d.command[0]) run(d.command,d.minimized?SW_SHOWMINNOACTIVE:SW_SHOWNORMAL);
        return;
    case IDM_EXIT: exit_windows(); return;
    case IDM_AUTOARRANGE: auto_arrange=!auto_arrange; return;
    case IDM_MINONRUN: min_on_run=!min_on_run; return;
    case IDM_CASCADE: SendMessage(client,WM_MDICASCADE,0,0); return;
    case IDM_TILE: SendMessage(client,WM_MDITILE,0,0); return;
    case IDM_ARRANGE:
        if(g && !IsIconic(g->wnd)) InvalidateRect(g->wnd,NULL,TRUE);
        SendMessage(client,WM_MDIICONARRANGE,0,0);
        return;
    case IDM_ABOUT:
        MessageBox(h,"Program Manager\nInterface Manager 3.0 for IA-64\n\nGroups and items are kept in PROGMAN.INI.","About Program Manager",MB_OK|MB_ICONINFORMATION);
        return;
    }
}
/* --- DDE ------------------------------------------------------------------------------- */
#define DDE_ARGS 10
static Group *dde_group; /* AddItem's: the group created or shown last */
static int replace_at=-1; /* ReplaceItem's place for the next AddItem */
static int to_int(LPCSTR s) {const char *p=s; return number(&p);}
static int item_named(Group *g,LPCSTR title) {
    int i;
    for(i=0;i<g->count;i++) if(!lstrcmpi(g->items[i].title,title)) return i;
    return -1;
}
static BOOL dde_command(LPCSTR name,char args[][160],int n) {
    Group *g; int i;
    if(!lstrcmpi(name,"CreateGroup") && n>=1) {
        if(!(g=group_named(args[0]))) {if(!(g=new_group(args[0]))) return FALSE; open_group(g);}
        else if(g->wnd) SendMessage(client,WM_MDIACTIVATE,(WPARAM)g->wnd,0);
        dde_group=g; replace_at=-1; return TRUE;
    }
    if(!lstrcmpi(name,"ShowGroup") && n>=1) {
        if(!(g=group_named(args[0])) || !g->wnd) return FALSE;
        i=n>=2?to_int(args[1]):SW_SHOWNORMAL;
        ShowWindow(g->wnd,i);
        if(i!=SW_MINIMIZE && i!=SW_SHOWMINIMIZED && i!=SW_SHOWMINNOACTIVE && i!=SW_HIDE) SendMessage(client,WM_MDIACTIVATE,(WPARAM)g->wnd,0);
        dde_group=g; return TRUE;
    }
    if(!lstrcmpi(name,"AddItem") && n>=1) {
        char title[64]; LPCSTR base=args[0],p; Item *it;
        if(!(g=dde_group && dde_group->used?dde_group:active_group())) return FALSE;
        if(n>=2 && args[1][0]) lstrcpyn(title,args[1],sizeof(title));
        else {
            for(p=args[0];*p && *p!=' ';p++) if(*p=='\\' || *p==':') base=p+1;
            lstrcpyn(title,base,sizeof(title));
            for(i=0;title[i] && title[i]!=' ' && title[i]!='.';i++) {}
            title[i]=0;
        }
        if(!(it=add_item(g,title,args[0]))) return FALSE;
        if(replace_at>=0 && replace_at<g->count-1) {Item moved=*it; for(i=g->count-1;i>replace_at;i--) g->items[i]=g->items[i-1]; g->items[replace_at]=moved;}
        replace_at=-1; refresh(g); return TRUE;
    }
    if(!lstrcmpi(name,"DeleteGroup") && n>=1) {
        if(!(g=group_named(args[0]))) return FALSE;
        if(dde_group==g) dde_group=NULL;
        delete_group(g); return TRUE;
    }
    if((!lstrcmpi(name,"DeleteItem") || !lstrcmpi(name,"ReplaceItem")) && n>=1) {
        if(!(g=dde_group && dde_group->used?dde_group:active_group()) || (i=item_named(g,args[0]))<0) return FALSE;
        remove_item(g,i); replace_at=lstrcmpi(name,"ReplaceItem")?-1:i;
        refresh(g); return TRUE;
    }
    if(!lstrcmpi(name,"ExitProgman")) {if(n>=1 && to_int(args[0])) save(); return TRUE;} /* the shell stays */
    if(!lstrcmpi(name,"Reload")) return TRUE;
    return FALSE;
}
/* [name(argument,...)]..., an argument in quotes if it holds commas or
 * brackets ("" inside for a quote); stops at the first that fails. */
static BOOL dde_execute(LPCSTR p) {
    static char args[DDE_ARGS][160]; char name[32];
    for(;;) {
        int n=0,k=0;
        while(*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
        if(!*p) return TRUE;
        if(*p++!='[') return FALSE;
        while(*p==' ') p++;
        while(*p && *p!='(' && *p!=']' && *p!=' ' && k<(int)sizeof(name)-1) name[k++]=*p++;
        name[k]=0;
        while(*p==' ') p++;
        if(*p=='(') {
            p++;
            for(;;) {
                char *a=n<DDE_ARGS?args[n]:NULL; int len=0;
                while(*p==' ') p++;
                if(*p=='"') {
                    for(p++;*p && !(*p=='"' && p[1]!='"');p++) {if(*p=='"') p++; if(a && len<159) a[len++]=*p;}
                    if(*p=='"') p++;
                    while(*p==' ') p++;
                } else {
                    while(*p && *p!=',' && *p!=')') {if(a && len<159) a[len++]=*p; p++;}
                    while(len && a && a[len-1]==' ') len--;
                }
                if(a) a[len]=0;
                if(n<DDE_ARGS) n++;
                if(*p==',') {p++; continue;}
                if(*p==')') {p++; break;}
                return FALSE;
            }
            while(*p==' ') p++;
        }
        if(*p++!=']') return FALSE;
        if(!dde_command(name,args,n)) return FALSE;
    }
}
/* A request: the group names, or a group's items as quoted title and command lines. */
static HGLOBAL dde_data(LPCSTR item) {
    char text[2048]; int used=0,i; Group *g=NULL; HGLOBAL h; DDEDATA *d;
    text[0]=0;
    if(!lstrcmpi(item,"Groups")) {
        for(i=0;i<GROUPS;i++) if(groups[i].used && used+lstrlen(groups[i].name)+3<(int)sizeof(text)) used+=wsprintf(text+used,"%s\r\n",groups[i].name);
    } else if((g=group_named(item))!=NULL) {
        used=wsprintf(text,"\"%s\",%d\r\n",g->name,g->count);
        for(i=0;i<g->count && used+lstrlen(g->items[i].title)+lstrlen(g->items[i].command)+8<(int)sizeof(text);i++)
            used+=wsprintf(text+used,"\"%s\",\"%s\"\r\n",g->items[i].title,g->items[i].command);
    } else return NULL;
    if(!(h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(DDEDATA)+used+1)) || !(d=(DDEDATA *)GlobalLock(h))) return NULL;
    memset(d,0,sizeof(*d)); d->fResponse=1; d->fRelease=1; d->cfFormat=CF_TEXT; lstrcpy((LPSTR)d->Value,text);
    GlobalUnlock(h);
    return h;
}
static BOOL dde_name(ATOM a) {char n[16]; return !a || (GlobalGetAtomName(a,n,sizeof(n)) && !lstrcmpi(n,"PROGMAN"));}
static LRESULT dde(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    HWND other=(HWND)wp;
    switch(msg) {
    case WM_DDE_INITIATE:
        if(dde_name(LOWORD(lp)) && dde_name(HIWORD(lp)))
            SendMessage(other,WM_DDE_ACK,(WPARAM)h,MAKELPARAM(GlobalAddAtom("PROGMAN"),GlobalAddAtom("PROGMAN")));
        return 0;
    case WM_DDE_EXECUTE: {
        HGLOBAL commands=(HGLOBAL)lp; LPCSTR text=(LPCSTR)GlobalLock(commands); BOOL ok=text && dde_execute(text);
        LPARAM ack;
        if(text) GlobalUnlock(commands);
        ack=PackDDElParam(WM_DDE_ACK,ok?0x8000:0,(UINT_PTR)commands);
        if(!PostMessage(other,WM_DDE_ACK,(WPARAM)h,ack)) FreeDDElParam(WM_DDE_ACK,ack);
        return 0;
    }
    case WM_DDE_REQUEST: {
        char item[64]; HGLOBAL data=NULL; ATOM a=HIWORD(lp); LPARAM answer;
        if(LOWORD(lp)==CF_TEXT && GlobalGetAtomName(a,item,sizeof(item))) data=dde_data(item);
        answer=data?PackDDElParam(WM_DDE_DATA,(UINT_PTR)data,a):PackDDElParam(WM_DDE_ACK,0,a);
        if(!PostMessage(other,data?WM_DDE_DATA:WM_DDE_ACK,(WPARAM)h,answer)) {FreeDDElParam(data?WM_DDE_DATA:WM_DDE_ACK,answer); if(data) GlobalFree(data);}
        return 0;
    }
    case WM_DDE_ADVISE: case WM_DDE_POKE: {
        UINT_PTR lo,hi; LPARAM ack;
        UnpackDDElParam(msg,lp,&lo,&hi); FreeDDElParam(msg,lp);
        if(lo) GlobalFree((HGLOBAL)lo);
        ack=PackDDElParam(WM_DDE_ACK,0,hi);
        if(!PostMessage(other,WM_DDE_ACK,(WPARAM)h,ack)) FreeDDElParam(WM_DDE_ACK,ack);
        return 0;
    }
    case WM_DDE_UNADVISE: {
        LPARAM ack=PackDDElParam(WM_DDE_ACK,0,HIWORD(lp));
        if(!PostMessage(other,WM_DDE_ACK,(WPARAM)h,ack)) FreeDDElParam(WM_DDE_ACK,ack);
        return 0;
    }
    case WM_DDE_TERMINATE: PostMessage(other,WM_DDE_TERMINATE,(WPARAM)h,0); dde_group=NULL; return 0;
    }
    return 0;
}

LRESULT CALLBACK FrameProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg>=WM_DDE_FIRST && msg<=WM_DDE_LAST) return dde(h,msg,wp,lp);
    switch(msg) {
    case WM_CREATE: {
        CLIENTCREATESTRUCT ccs; int i;
        ccs.hWindowMenu=GetSubMenu(GetMenu(h),2); ccs.idFirstChild=IDM_FIRSTCHILD;
        frame=h;
        client=CreateWindow("MDICLIENT",NULL,WS_CHILD|WS_CLIPCHILDREN|WS_VISIBLE,0,0,0,0,h,(HMENU)1,instance,&ccs);
        auto_arrange=GetPrivateProfileInt("Settings","AutoArrange",1,INI)!=0;
        min_on_run=GetPrivateProfileInt("Settings","MinOnRun",0,INI)!=0;
        save_settings=GetPrivateProfileInt("Settings","SaveSettings",1,INI)!=0;
        load();
        for(i=0;i<GROUPS && !groups[i].used;i++) {}
        if(i==GROUPS) make_defaults();
        return 0;
    }
    case WM_INITMENUPOPUP: {
        HMENU m=(HMENU)wp; Group *g; Item *it=selected(&g); UINT on=MF_BYCOMMAND|MF_ENABLED,off=MF_BYCOMMAND|MF_GRAYED;
        if(HIWORD(lp)) break;
        EnableMenuItem(m,IDM_OPEN,it || (g && IsIconic(g->wnd))?on:off);
        EnableMenuItem(m,IDM_MOVE,it?on:off);
        EnableMenuItem(m,IDM_COPY,it?on:off);
        EnableMenuItem(m,IDM_DELETE,it || g?on:off);
        EnableMenuItem(m,IDM_PROPERTIES,it || g?on:off);
        CheckMenuItem(m,IDM_AUTOARRANGE,MF_BYCOMMAND|(auto_arrange?MF_CHECKED:MF_UNCHECKED));
        CheckMenuItem(m,IDM_MINONRUN,MF_BYCOMMAND|(min_on_run?MF_CHECKED:MF_UNCHECKED));
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)<IDM_FIRSTCHILD) {command(h,LOWORD(wp)); return 0;}
        break;
    case WM_CLOSE: exit_windows(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if(wp && save_settings) save(); return 0;
    case WM_DESTROY: WinHelp(h,"PROGMAN.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefFrameProc(h,client,msg,wp,lp);
}

int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR cmdline,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; RECT r; int state=0,i;
    (void)cmdline;
    instance=inst;
    if(previous) return 0;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=FrameProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"PROGMAN"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_APPWORKSPACE+1); wc.lpszMenuName="PROGMAN"; wc.lpszClassName="Progman";
    RegisterClass(&wc);
    wc.lpfnWndProc=GroupProc; wc.style=CS_DBLCLKS; wc.hIcon=LoadIcon(inst,"GROUP"); wc.lpszMenuName=NULL;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.cbWndExtra=sizeof(LONG_PTR); wc.lpszClassName="PMGroup";
    RegisterClass(&wc);
    if(!read_placement("Settings",&r,&state)) SetRect(&r,0,0,GetSystemMetrics(SM_CXSCREEN)*4/5,GetSystemMetrics(SM_CYSCREEN)*4/5);
    frame=CreateWindow("Progman","Program Manager",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,r.left,r.top,r.right-r.left,r.bottom-r.top,NULL,NULL,inst,NULL);
    ShowWindow(frame,state==2?SW_SHOWMAXIMIZED:state==1?SW_SHOWMINIMIZED:show);
    {RECT c; GetClientRect(frame,&c); MoveWindow(client,0,0,c.right,c.bottom,TRUE);}
    for(i=0;i<GROUPS;i++) if(groups[i].used) open_group(&groups[i]);
    {
        /* The first open group, not the last created, is active. */
        for(i=0;i<GROUPS;i++) if(groups[i].used && groups[i].wnd && !IsIconic(groups[i].wnd)) {SendMessage(client,WM_MDIACTIVATE,(WPARAM)groups[i].wnd,0); break;}
    }
    UpdateWindow(frame);
    accel=LoadAccelerators(inst,"PROGMAN");
    startup_programs("load",SW_SHOWMINNOACTIVE);
    startup_programs("run",SW_SHOWNORMAL);
    while(GetMessage(&m,NULL,0,0))
        if(!TranslateMDISysAccel(client,&m) && !TranslateAccelerator(frame,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
