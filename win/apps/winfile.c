/* SPDX-License-Identifier: GPL-2.0-or-later
 * File Manager directory tree/windows. Documents use WIN.INI [Extensions]
 * associations; txt/ini default to Notepad. /trace reports window state and
 * actions through OutputDebugString for QEMU tests.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "winfile.h"
#define MAXP 128
#define NODES 512
#define ENTRIES 1024
#define ROW 16
#define DRIVEBAR 26
#define LEAF 1
#define CLOSED 2
#define OPEN 3

typedef struct {char path[MAXP]; char name[14]; BYTE level,state;} Node;
typedef struct {char name[14]; DWORD attr,size; WORD date,time;} Entry;
typedef struct {char dir[MAXP]; Entry *entries; int count; HWND list;} DirWin;

static HINSTANCE instance;
static HWND frame,client,tree,tree_list;
static WNDPROC list_proc;
static Node *nodes;
static int node_count,status_height;
static char drive='C';
static BOOL details,trace;

/* --- helpers ------------------------------------------------------------------- */
static void report(LPCSTR text) {if(trace) OutputDebugString(text);}
static void join(char *out,LPCSTR dir,LPCSTR name) {
    int n;
    lstrcpyn(out,dir,MAXP); n=lstrlen(out);
    if(n && out[n-1]!='\\' && n+1<MAXP) {out[n++]='\\'; out[n]=0;}
    lstrcpyn(out+n,name,MAXP-n);
}
/* The parent of a directory; a root stays itself. */
static void parent(char *path) {
    int i,cut=-1;
    for(i=0;path[i];i++) if(path[i]=='\\') cut=i;
    if(cut<=2) path[3]=0; else path[cut]=0;
}
static LPCSTR extension(LPCSTR name) {
    LPCSTR dot=NULL;
    for(;*name;name++) if(*name=='.') dot=name+1;
    return dot?dot:"";
}
static BOOL program(LPCSTR name) {return !lstrcmpi(extension(name),"EXE");}
static BOOL dos_program(LPCSTR name) {LPCSTR e=extension(name); return !lstrcmpi(e,"COM") || !lstrcmpi(e,"BAT") || !lstrcmpi(e,"PIF");}
/* The program for a document: WIN.INI [Extensions] "txt=notepad.exe ^.txt". */
static BOOL association(LPCSTR name,char *out,int size) {
    char ext[8],text[MAXP]; int i;
    lstrcpyn(ext,extension(name),sizeof(ext)); AnsiLower(ext);
    if(!ext[0]) return FALSE;
    GetProfileString("Extensions",ext,"",text,sizeof(text));
    if(!text[0] && (!lstrcmp(ext,"txt") || !lstrcmp(ext,"ini"))) lstrcpy(text,"notepad.exe ^.txt");
    for(i=0;text[i] && text[i]!='^';i++) {}
    while(i>0 && text[i-1]==' ') i--;
    text[i]=0;
    if(!text[0]) return FALSE;
    lstrcpyn(out,text,size);
    return TRUE;
}
static void number(DWORD v,char *out) {
    char digits[16]; int n=0,k=0;
    do {if(n && n%3==0) digits[k++]=','; digits[k++]=(char)('0'+v%10); v/=10; n++;} while(v);
    while(k) *out++=digits[--k];
    *out=0;
}
static LPCSTR error_text(DWORD e) {
    switch(e) {
    case 2: return "File not found";
    case 3: return "Path not found";
    case 5: return "Access denied";
    case 15: return "Invalid drive";
    case 16: return "It is the current directory";
    case 17: return "Not the same drive";
    case 19: return "Disk is write-protected";
    case 21: return "Drive not ready";
    case 39: return "Disk full";
    case 80: return "File exists";
    case 82: return "Cannot make the directory entry";
    }
    return "Error";
}
static void fail(LPCSTR what,LPCSTR path,DWORD e) {
    char text[300],line[320];
    wsprintf(text,"Cannot %s %s: %s.",what,path,error_text(e));
    wsprintf(line,"WINFILE: error %s",text); report(line);
    MessageBox(frame,text,"File Manager",MB_OK|MB_ICONEXCLAMATION);
}
static BOOL is_dir(LPCSTR path) {DWORD a=GetFileAttributes(path); return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY);}
static BOOL real(const WIN32_FIND_DATA *f) {return lstrcmp(f->cFileName,".") && lstrcmp(f->cFileName,"..");}

/* --- the directory tree ----------------------------------------------------------- */
static BOOL has_subdirs(LPCSTR path) {
    char pattern[MAXP]; WIN32_FIND_DATA f; HANDLE h; BOOL found=FALSE;
    join(pattern,path,"*.*");
    if((h=FindFirstFile(pattern,&f))==INVALID_HANDLE_VALUE) return FALSE;
    do if((f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && real(&f)) found=TRUE; while(!found && FindNextFile(h,&f));
    FindClose(h);
    return found;
}
static int subdirs(LPCSTR path,char (*names)[14],int max) {
    char pattern[MAXP]; WIN32_FIND_DATA f; HANDLE h; int n=0,i,k;
    join(pattern,path,"*.*");
    if((h=FindFirstFile(pattern,&f))==INVALID_HANDLE_VALUE) return 0;
    do if((f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && real(&f) && n<max) lstrcpyn(names[n++],f.cFileName,14);
    while(FindNextFile(h,&f));
    FindClose(h);
    for(i=1;i<n;i++) for(k=i;k>0 && lstrcmpi(names[k-1],names[k])>0;k--) {char t[14]; lstrcpy(t,names[k]); lstrcpy(names[k],names[k-1]); lstrcpy(names[k-1],t);}
    return n;
}
static void expand(int at) {
    static char names[256][14]; int n=subdirs(nodes[at].path,names,256),i;
    if(node_count+n>NODES) n=NODES-node_count;
    memmove(&nodes[at+1+n],&nodes[at+1],sizeof(Node)*(unsigned)(node_count-at-1));
    for(i=0;i<n;i++) {
        Node *c=&nodes[at+1+i];
        join(c->path,nodes[at].path,names[i]); lstrcpy(c->name,names[i]);
        c->level=(BYTE)(nodes[at].level+1); c->state=(BYTE)(has_subdirs(c->path)?CLOSED:LEAF);
    }
    node_count+=n;
    nodes[at].state=(BYTE)(n?OPEN:LEAF);
}
static void collapse(int at) {
    int end=at+1;
    while(end<node_count && nodes[end].level>nodes[at].level) end++;
    memmove(&nodes[at+1],&nodes[end],sizeof(Node)*(unsigned)(node_count-end));
    node_count-=end-at-1;
    nodes[at].state=CLOSED;
}
static void tree_fill(int select) {
    char line[256]; int i,n;
    SendMessage(tree_list,WM_SETREDRAW,FALSE,0);
    SendMessage(tree_list,LB_RESETCONTENT,0,0);
    for(i=0;i<node_count;i++) SendMessage(tree_list,LB_ADDSTRING,0,(LPARAM)nodes[i].name);
    SendMessage(tree_list,WM_SETREDRAW,TRUE,0);
    SendMessage(tree_list,LB_SETCURSEL,(WPARAM)max(0,min(select,node_count-1)),0);
    InvalidateRect(tree_list,NULL,TRUE);
    lstrcpy(line,"WINFILE: tree"); n=lstrlen(line);
    for(i=0;i<node_count && n+16<(int)sizeof(line);i++) {line[n++]=' '; lstrcpy(line+n,nodes[i].name); n+=lstrlen(nodes[i].name);}
    report(line);
}
static void tree_load(char letter) {
    drive=letter;
    node_count=1;
    nodes[0].path[0]=letter; nodes[0].path[1]=':'; nodes[0].path[2]='\\'; nodes[0].path[3]=0;
    lstrcpy(nodes[0].name,nodes[0].path); nodes[0].level=0; nodes[0].state=CLOSED;
    expand(0);
    tree_fill(0);
    if(tree) InvalidateRect(tree,NULL,TRUE);
}
/* Read the tree again, keeping open what was open. */
static void tree_refresh(void) {
    static char open_paths[64][MAXP]; char selected[MAXP]; int n=0,i,sel=0;
    LRESULT cur=SendMessage(tree_list,LB_GETCURSEL,0,0);
    selected[0]=0;
    if(cur>=0 && cur<node_count) lstrcpy(selected,nodes[cur].path);
    for(i=1;i<node_count && n<64;i++) if(nodes[i].state==OPEN) lstrcpy(open_paths[n++],nodes[i].path);
    node_count=1; nodes[0].state=CLOSED;
    expand(0);
    for(i=1;i<node_count;i++) {
        int k;
        for(k=0;k<n;k++) if(!lstrcmpi(nodes[i].path,open_paths[k]) && nodes[i].state==CLOSED) {expand(i); break;}
    }
    for(i=0;i<node_count;i++) if(!lstrcmpi(nodes[i].path,selected)) sel=i;
    tree_fill(sel);
}
static int tree_selected(void) {
    LRESULT i=SendMessage(tree_list,LB_GETCURSEL,0,0);
    return i>=0 && i<node_count?(int)i:-1;
}
static void tree_toggle(int i) {
    if(i<0) return;
    if(nodes[i].state==OPEN) collapse(i);
    else if(nodes[i].state==CLOSED) expand(i);
    else return;
    tree_fill(i);
}

/* --- drawing ------------------------------------------------------------------------ */
static void box(HDC dc,int l,int t,int r,int b,COLORREF c) {
    RECT rc; HBRUSH br=CreateSolidBrush(c);
    SetRect(&rc,l,t,r,b); FillRect(dc,&rc,br); DeleteObject(br);
}
static void frame_box(HDC dc,int l,int t,int r,int b) {
    RECT rc; SetRect(&rc,l,t,r,b); FrameRect(dc,&rc,GetStockObject(BLACK_BRUSH));
}
static void folder(HDC dc,int x,int y,int mark) {
    box(dc,x,y+1,x+6,y+3,RGB(255,255,0)); frame_box(dc,x,y,x+7,y+3);
    box(dc,x,y+2,x+14,y+11,RGB(255,255,0)); frame_box(dc,x,y+2,x+15,y+12);
    if(mark) {
        box(dc,x+4,y+6,x+11,y+7,RGB(0,0,0));
        if(mark=='+') box(dc,x+7,y+3,x+8,y+10,RGB(0,0,0));
    }
}
static void page(HDC dc,int x,int y,BOOL lines) {
    int i;
    box(dc,x+2,y,x+12,y+12,RGB(255,255,255)); frame_box(dc,x+2,y,x+13,y+13);
    if(lines) for(i=0;i<4;i++) box(dc,x+4,y+3+i*2,x+11,y+4+i*2,RGB(0,0,128));
}
static void program_glyph(HDC dc,int x,int y) {
    box(dc,x,y+1,x+14,y+12,RGB(255,255,255)); frame_box(dc,x,y+1,x+15,y+13);
    box(dc,x+1,y+2,x+14,y+4,RGB(0,0,128));
}
static void drive_glyph(HDC dc,int x,int y,UINT type) {
    if(type==DRIVE_REMOVABLE) {
        box(dc,x,y+2,x+16,y+10,RGB(192,192,192)); frame_box(dc,x,y+2,x+16,y+10);
        box(dc,x+3,y+5,x+13,y+6,RGB(0,0,0));
    } else if(type==DRIVE_REMOTE || type==DRIVE_CDROM) {
        HGDIOBJ old=SelectObject(dc,GetStockObject(LTGRAY_BRUSH));
        Ellipse(dc,x+1,y,x+15,y+13); SelectObject(dc,GetStockObject(WHITE_BRUSH)); Ellipse(dc,x+6,y+5,x+10,y+9);
        SelectObject(dc,old);
    } else {
        box(dc,x,y+3,x+16,y+11,RGB(128,128,128)); frame_box(dc,x,y+3,x+16,y+11);
        box(dc,x+11,y+6,x+14,y+8,RGB(0,255,0));
    }
}
static void item_colors(HDC dc,const DRAWITEMSTRUCT *d) {
    if(d->itemState&ODS_SELECTED) {box(dc,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,GetSysColor(COLOR_HIGHLIGHT)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
    else {box(dc,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,GetSysColor(COLOR_WINDOW)); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));}
    SetBkMode(dc,TRANSPARENT);
}
static void draw_tree_item(const DRAWITEMSTRUCT *d) {
    HDC dc=d->hDC; int i=(int)d->itemID,x,y=d->rcItem.top;
    if(i<0 || i>=node_count) return;
    x=d->rcItem.left+4+nodes[i].level*16;
    box(dc,d->rcItem.left,y,d->rcItem.right,y+ROW,GetSysColor(COLOR_WINDOW));
    if(nodes[i].level) {box(dc,x-10,y+ROW/2,x-1,y+ROW/2+1,RGB(128,128,128)); box(dc,x-10,y,x-9,y+ROW/2,RGB(128,128,128));}
    if(!nodes[i].level) drive_glyph(dc,x,y+1,GetDriveType(nodes[i].path[0]-'A'));
    else folder(dc,x,y+2,nodes[i].state==CLOSED?'+':nodes[i].state==OPEN?'-':0);
    {
        RECT t; SIZE s; GetTextExtentPoint(dc,nodes[i].name,lstrlen(nodes[i].name),&s);
        SetRect(&t,x+18,y,x+22+s.cx,y+ROW);
        SetBkMode(dc,TRANSPARENT);
        if(d->itemState&ODS_SELECTED) {box(dc,t.left,t.top,t.right,t.bottom,GetSysColor(COLOR_HIGHLIGHT)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
        else SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        TextOut(dc,x+20,y+(ROW-13)/2,nodes[i].name,lstrlen(nodes[i].name));
        if(d->itemState&ODS_FOCUS) DrawFocusRect(dc,&t);
    }
}
static void draw_dir_item(DirWin *w,const DRAWITEMSTRUCT *d) {
    HDC dc=d->hDC; int i=(int)d->itemID,x=d->rcItem.left+2,y=d->rcItem.top; Entry *e; char text[48],cmd[8];
    if(i<0 || i>=w->count) return;
    e=&w->entries[i];
    item_colors(dc,d);
    if(!lstrcmp(e->name,"..")) {folder(dc,x,y+2,0); box(dc,x+5,y+5,x+10,y+6,RGB(0,0,0)); box(dc,x+7,y+4,x+8,y+10,RGB(0,0,0));}
    else if(e->attr&FILE_ATTRIBUTE_DIRECTORY) folder(dc,x,y+2,0);
    else if(program(e->name) || dos_program(e->name)) program_glyph(dc,x,y+1);
    else page(dc,x,y+1,association(e->name,cmd,sizeof(cmd)));
    TextOut(dc,x+20,y+(ROW-13)/2,e->name,lstrlen(e->name));
    if(details && lstrcmp(e->name,"..")) {
        SIZE s;
        if(!(e->attr&FILE_ATTRIBUTE_DIRECTORY)) {number(e->size,text); GetTextExtentPoint(dc,text,lstrlen(text),&s); TextOut(dc,x+210-s.cx,y+(ROW-13)/2,text,lstrlen(text));}
        wsprintf(text,"%02d-%02d-%02d  %2d:%02d%c  %c%c%c%c",(e->date>>5)&15,e->date&31,((e->date>>9)+80)%100,
                 ((e->time>>11)%12)?(e->time>>11)%12:12,(e->time>>5)&63,(e->time>>11)>=12?'p':'a',
                 e->attr&FILE_ATTRIBUTE_READONLY?'r':'-',e->attr&FILE_ATTRIBUTE_HIDDEN?'h':'-',
                 e->attr&FILE_ATTRIBUTE_SYSTEM?'s':'-',e->attr&FILE_ATTRIBUTE_ARCHIVE?'a':'-');
        TextOut(dc,x+224,y+(ROW-13)/2,text,lstrlen(text));
    }
    if(d->itemState&ODS_FOCUS) DrawFocusRect(dc,&d->rcItem);
}
/* The drive bar of the tree window. */
static int drive_list(char *letters) {
    DWORD mask=GetLogicalDrives(); int n=0,d;
    for(d=0;d<26;d++) if(mask&(1u<<d)) letters[n++]=(char)('A'+d);
    return n;
}
static void paint_drives(HDC dc) {
    char letters[26]; int n=drive_list(letters),i; RECT c;
    GetClientRect(tree,&c);
    box(dc,0,0,c.right,DRIVEBAR,GetSysColor(COLOR_WINDOW));
    box(dc,0,DRIVEBAR-1,c.right,DRIVEBAR,RGB(0,0,0));
    SetBkMode(dc,TRANSPARENT);
    for(i=0;i<n;i++) {
        int x=4+i*44; char label[3]; label[0]=(char)(letters[i]|0x20); label[1]=0;
        if(letters[i]==drive) {box(dc,x-2,2,x+40,DRIVEBAR-3,GetSysColor(COLOR_HIGHLIGHT)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT));}
        else SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        drive_glyph(dc,x,5,GetDriveType(letters[i]-'A'));
        TextOut(dc,x+20,6,label,1);
    }
}

/* --- directory windows ------------------------------------------------------------------ */
static int compare(const Entry *a,const Entry *b) {
    BOOL da=(a->attr&FILE_ATTRIBUTE_DIRECTORY)!=0,db=(b->attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
    if(!lstrcmp(a->name,"..")) return -1;
    if(!lstrcmp(b->name,"..")) return 1;
    if(da!=db) return da?-1:1;
    return lstrcmpi(a->name,b->name);
}
static void dir_read(HWND h,DirWin *w,LPCSTR select) {
    char pattern[MAXP],title[MAXP+8],line[256]; WIN32_FIND_DATA f; HANDLE fh; int i,k,n,sel=0;
    w->count=0;
    join(pattern,w->dir,"*.*");
    if((fh=FindFirstFile(pattern,&f))!=INVALID_HANDLE_VALUE) {
        do {
            Entry *e;
            if(!lstrcmp(f.cFileName,".") || (!lstrcmp(f.cFileName,"..") && lstrlen(w->dir)<=3) || w->count==ENTRIES) continue;
            e=&w->entries[w->count++];
            lstrcpyn(e->name,f.cFileName,sizeof(e->name)); e->attr=f.dwFileAttributes; e->size=f.nFileSizeLow;
            FileTimeToDosDateTime(&f.ftLastWriteTime,&e->date,&e->time);
        } while(FindNextFile(fh,&f));
        FindClose(fh);
    } else if(GetLastError()!=18 && GetLastError()!=2) fail("read",w->dir,GetLastError());
    for(i=1;i<w->count;i++) for(k=i;k>0 && compare(&w->entries[k-1],&w->entries[k])>0;k--) {Entry t=w->entries[k]; w->entries[k]=w->entries[k-1]; w->entries[k-1]=t;}
    SendMessage(w->list,WM_SETREDRAW,FALSE,0);
    SendMessage(w->list,LB_RESETCONTENT,0,0);
    for(i=0;i<w->count;i++) {
        SendMessage(w->list,LB_ADDSTRING,0,(LPARAM)w->entries[i].name);
        if(select && !lstrcmpi(select,w->entries[i].name)) sel=i;
    }
    SendMessage(w->list,WM_SETREDRAW,TRUE,0);
    if(w->count) SendMessage(w->list,LB_SETCURSEL,(WPARAM)sel,0);
    InvalidateRect(w->list,NULL,TRUE);
    join(title,w->dir,"*.*"); SetWindowText(h,title);
    wsprintf(line,"WINFILE: dir %s",title); n=lstrlen(line);
    for(i=0;i<w->count && n+16<(int)sizeof(line);i++) {line[n++]=' '; lstrcpy(line+n,w->entries[i].name); n+=lstrlen(w->entries[i].name);}
    report(line);
}
static DirWin *dir_of(HWND h) {
    char name[16];
    if(!h || !GetClassName(h,name,sizeof(name)) || lstrcmpi(name,"WFDir")) return NULL;
    return (DirWin *)GetWindowLongPtr(h,0);
}
static HWND active_child(void) {return (HWND)SendMessage(client,WM_MDIGETACTIVE,0,0);}
static void open_dir_window(LPCSTR path) {
    MDICREATESTRUCT mcs; RECT r; HWND h; int n=0;
    for(h=GetWindow(client,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT)) {
        DirWin *w=dir_of(h);
        if(w && !lstrcmpi(w->dir,path)) {SendMessage(client,WM_MDIACTIVATE,(WPARAM)h,0); if(IsIconic(h)) ShowWindow(h,SW_RESTORE); return;}
        if(w) n++;
    }
    GetClientRect(client,&r);
    mcs.szClass="WFDir"; mcs.szTitle=path; mcs.hOwner=instance;
    mcs.x=r.right*2/5+n%6*20; mcs.y=n%6*20; mcs.cx=r.right*3/5; mcs.cy=r.bottom*4/5;
    mcs.style=0; mcs.lParam=(LPARAM)path;
    SendMessage(client,WM_MDICREATE,0,(LPARAM)&mcs);
}
static void run(LPCSTR command) {
    char line[200];
    wsprintf(line,"WINFILE: run %s",command); report(line);
    if(WinExec(command,SW_SHOWNORMAL)<32) {
        char text[200]; wsprintf(text,"Cannot run %s.",command);
        MessageBox(frame,text,"File Manager",MB_OK|MB_ICONEXCLAMATION);
    }
}
/* Open a directory window's selection: go into directories, run programs,
 * open documents with their programs. */
static void dir_open(HWND h,DirWin *w) {
    LRESULT i=SendMessage(w->list,LB_GETCURSEL,0,0); Entry *e; char path[MAXP],cmd[MAXP*2],up[16];
    if(i<0 || i>=w->count) return;
    e=&w->entries[i];
    if(!lstrcmp(e->name,"..")) {
        LPCSTR base=FileTitle(w->dir); lstrcpyn(up,base,sizeof(up));
        parent(w->dir); dir_read(h,w,up); return;
    }
    join(path,w->dir,e->name);
    if(e->attr&FILE_ATTRIBUTE_DIRECTORY) {lstrcpy(w->dir,path); dir_read(h,w,NULL); return;}
    if(program(e->name) || dos_program(e->name)) {run(path); return;}
    if(association(e->name,cmd,MAXP)) {lstrcat(cmd," "); lstrcat(cmd,path); run(cmd); return;}
    MessageBox(frame,"No application is associated with this file.","File Manager",MB_OK|MB_ICONEXCLAMATION);
}
static void refresh_all(void) {
    HWND h;
    tree_refresh();
    for(h=GetWindow(client,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT)) {
        DirWin *w=dir_of(h);
        if(w) {
            char sel[16]; LRESULT i=SendMessage(w->list,LB_GETCURSEL,0,0);
            sel[0]=0;
            if(i>=0 && i<w->count) lstrcpy(sel,w->entries[i].name);
            if(!is_dir(w->dir)) parent(w->dir);
            dir_read(h,w,sel);
        }
    }
    InvalidateRect(frame,NULL,FALSE);
}

/* --- the list boxes: Enter opens, + and - open and close tree branches --------------------- */
static void open_selection(HWND child);
LRESULT CALLBACK ListProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    HWND child=GetParent(h);
    switch(msg) {
    case WM_SETFOCUS:
        if(active_child()!=child) SendMessage(client,WM_MDIACTIVATE,(WPARAM)child,0);
        break;
    case WM_KEYDOWN:
        if(wp==VK_RETURN) {open_selection(child); return 0;}
        if(wp==VK_BACK && dir_of(child)) {
            DirWin *w=dir_of(child);
            if(w->count && !lstrcmp(w->entries[0].name,"..")) {SendMessage(h,LB_SETCURSEL,0,0); dir_open(child,w);}
            return 0;
        }
        break;
    case WM_CHAR:
        if(h==tree_list && (wp=='+' || wp=='-')) {
            int i=tree_selected();
            if(i>=0 && ((wp=='+' && nodes[i].state==CLOSED) || (wp=='-' && nodes[i].state==OPEN))) tree_toggle(i);
            return 0;
        }
        if(wp=='\r') return 0;
        break;
    case WM_LBUTTONDOWN:
        if(h==tree_list) {
            /* A click on a folder's + or - opens or closes the branch. */
            int i=(int)SendMessage(h,LB_GETTOPINDEX,0,0)+GET_Y_LPARAM(lp)/ROW,x=GET_X_LPARAM(lp);
            if(i>=0 && i<node_count && nodes[i].level && x>=4+nodes[i].level*16 && x<4+nodes[i].level*16+15) {
                LRESULT r=CallWindowProc(list_proc,h,msg,wp,lp);
                tree_toggle(i);
                return r;
            }
        }
        break;
    }
    return CallWindowProc(list_proc,h,msg,wp,lp);
}
static HWND make_list(HWND parent_wnd) {
    HWND l=CreateWindow("LISTBOX",NULL,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,
                        0,0,0,0,parent_wnd,(HMENU)IDC_LIST,instance,NULL);
    WNDPROC old=(WNDPROC)SetWindowLongPtr(l,GWL_WNDPROC,(LONG_PTR)ListProc);
    if(!list_proc) list_proc=old;
    return l;
}
static void open_selection(HWND child) {
    DirWin *w=dir_of(child);
    if(w) {dir_open(child,w); return;}
    if(child==tree) {
        int i=tree_selected();
        if(i<0) return;
        if(nodes[i].state==CLOSED) tree_toggle(i);
        open_dir_window(nodes[i].path);
    }
}

/* --- child windows -------------------------------------------------------------------------- */
LRESULT CALLBACK TreeProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: tree=h; tree_list=make_list(h); tree_load(drive); return 0;
    case WM_SIZE: {RECT r; GetClientRect(h,&r); MoveWindow(tree_list,0,DRIVEBAR,r.right,max(0,(int)r.bottom-DRIVEBAR),TRUE); break;}
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint_drives(dc); EndPaint(h,&ps); return 0;}
    case WM_LBUTTONDOWN:
        if(GET_Y_LPARAM(lp)<DRIVEBAR) {
            char letters[26]; int n=drive_list(letters),i=(GET_X_LPARAM(lp)-2)/44;
            if(i>=0 && i<n) {tree_load(letters[i]); InvalidateRect(frame,NULL,FALSE);}
            SetFocus(tree_list);
        }
        return 0;
    case WM_SETFOCUS: SetFocus(tree_list); break;
    case WM_MEASUREITEM: ((LPMEASUREITEMSTRUCT)lp)->itemHeight=ROW; return TRUE;
    case WM_DRAWITEM: draw_tree_item((LPDRAWITEMSTRUCT)lp); return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_LIST && HIWORD(wp)==LBN_DBLCLK) open_selection(h);
        return 0;
    case WM_MDIACTIVATE: InvalidateRect(frame,NULL,FALSE); break;
    case WM_CLOSE: ShowWindow(h,SW_MINIMIZE); return 0;
    }
    return DefMDIChildProc(h,msg,wp,lp);
}
LRESULT CALLBACK DirProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    DirWin *w=(DirWin *)GetWindowLongPtr(h,0);
    switch(msg) {
    case WM_CREATE: {
        LPMDICREATESTRUCT mcs=(LPMDICREATESTRUCT)((LPCREATESTRUCT)lp)->lpCreateParams;
        w=(DirWin *)GlobalAlloc(GPTR,sizeof(DirWin));
        if(!w || !(w->entries=(Entry *)GlobalAlloc(GPTR,sizeof(Entry)*ENTRIES))) return -1;
        lstrcpyn(w->dir,(LPCSTR)mcs->lParam,MAXP);
        SetWindowLongPtr(h,0,(LONG_PTR)w);
        w->list=make_list(h);
        dir_read(h,w,NULL);
        return 0;
    }
    case WM_SIZE: if(w) {RECT r; GetClientRect(h,&r); MoveWindow(w->list,0,0,r.right,r.bottom,TRUE);} break;
    case WM_SETFOCUS: if(w) SetFocus(w->list); break;
    case WM_MEASUREITEM: ((LPMEASUREITEMSTRUCT)lp)->itemHeight=ROW; return TRUE;
    case WM_DRAWITEM: if(w) draw_dir_item(w,(LPDRAWITEMSTRUCT)lp); return TRUE;
    case WM_COMMAND:
        if(w && LOWORD(wp)==IDC_LIST && HIWORD(wp)==LBN_DBLCLK) dir_open(h,w);
        return 0;
    case WM_MDIACTIVATE: InvalidateRect(frame,NULL,FALSE); break;
    case WM_DESTROY:
        if(w) {GlobalFree(w->entries); GlobalFree(w); SetWindowLongPtr(h,0,0);}
        break;
    }
    return DefMDIChildProc(h,msg,wp,lp);
}

/* --- dialogs --------------------------------------------------------------------------------- */
typedef struct {LPCSTR title,label; char from[MAXP],to[MAXP];} Request;
static INT_PTR CALLBACK RequestProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Request *r=(Request *)GetProp(h,"Request");
    switch(msg) {
    case WM_INITDIALOG:
        r=(Request *)lp; SetProp(h,"Request",(HANDLE)r);
        SetWindowText(h,r->title);
        if(r->label) SetDlgItemText(h,IDC_LABEL,r->label);
        SetDlgItemText(h,IDC_FROM,r->from);
        if(GetDlgItem(h,IDC_TO)) SetDlgItemText(h,IDC_TO,r->to);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK) {
            GetDlgItemText(h,IDC_FROM,r->from,MAXP);
            if(GetDlgItem(h,IDC_TO)) GetDlgItemText(h,IDC_TO,r->to,MAXP);
            EndDialog(h,IDOK); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
        return FALSE;
    case WM_DESTROY: RemoveProp(h,"Request"); return FALSE;
    }
    return FALSE;
}
static BOOL ask(LPCSTR dialog,Request *r) {return DialogBoxParam(instance,dialog,frame,RequestProc,(LPARAM)r)==IDOK && r->from[0];}
static INT_PTR CALLBACK DriveProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: {
        char letters[26],text[16]; int n=drive_list(letters),i;
        for(i=0;i<n;i++) {
            UINT type=GetDriveType(letters[i]-'A');
            wsprintf(text,"%c:  %s",letters[i],type==DRIVE_REMOVABLE?"removable":type==DRIVE_REMOTE?"CD/remote":"fixed");
            SendDlgItemMessage(h,IDC_DRIVES,LB_ADDSTRING,0,(LPARAM)text);
            if(letters[i]==drive) SendDlgItemMessage(h,IDC_DRIVES,LB_SETCURSEL,(WPARAM)i,0);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK || (LOWORD(wp)==IDC_DRIVES && HIWORD(wp)==LBN_DBLCLK)) {
            char text[16]; LRESULT i=SendDlgItemMessage(h,IDC_DRIVES,LB_GETCURSEL,0,0);
            if(i<0) return TRUE;
            SendDlgItemMessage(h,IDC_DRIVES,LB_GETTEXT,(WPARAM)i,(LPARAM)text);
            EndDialog(h,text[0]); return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL) {EndDialog(h,0); return TRUE;}
        return FALSE;
    }
    return FALSE;
}

/* --- file operations ------------------------------------------------------------------------------ */
/* The full path of what the active window has selected, and its directory. */
static BOOL selection(char *path) {
    HWND h=active_child(); DirWin *w=dir_of(h);
    path[0]=0;
    if(w) {
        LRESULT i=SendMessage(w->list,LB_GETCURSEL,0,0);
        if(i<0 || i>=w->count || !lstrcmp(w->entries[i].name,"..")) return FALSE;
        join(path,w->dir,w->entries[i].name);
        return TRUE;
    }
    if(h==tree) {int i=tree_selected(); if(i>0) {lstrcpy(path,nodes[i].path); return TRUE;}}
    return FALSE;
}
static void current_dir(char *path) {
    HWND h=active_child(); DirWin *w=dir_of(h); int i;
    if(w) {lstrcpy(path,w->dir); return;}
    i=tree_selected();
    lstrcpy(path,i>=0?nodes[i].path:"C:\\");
}
/* A name without a directory is in the active window's directory. */
static void full(char *out,LPCSTR name) {
    char dir[MAXP];
    if(name[1]==':' || name[0]=='\\') {lstrcpyn(out,name,MAXP); return;}
    current_dir(dir); join(out,dir,name);
}
static BOOL delete_tree(LPCSTR path) {
    char pattern[MAXP],item[MAXP]; WIN32_FIND_DATA f; HANDLE h;
    join(pattern,path,"*.*");
    if((h=FindFirstFile(pattern,&f))!=INVALID_HANDLE_VALUE) {
        do {
            if(!real(&f)) continue;
            join(item,path,f.cFileName);
            if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {if(!delete_tree(item)) {FindClose(h); return FALSE;}}
            else if(!DeleteFile(item)) {fail("delete",item,GetLastError()); FindClose(h); return FALSE;}
        } while(FindNextFile(h,&f));
        FindClose(h);
    }
    if(!RemoveDirectory(path)) {fail("remove",path,GetLastError()); return FALSE;}
    return TRUE;
}
static void done(LPCSTR what,LPCSTR from,LPCSTR to) {
    char line[300];
    if(to) wsprintf(line,"WINFILE: %s %s to %s",what,from,to); else wsprintf(line,"WINFILE: %s %s",what,from);
    report(line);
}
static void copy_or_move(BOOL move) {
    Request r; char from[MAXP],to[MAXP];
    memset(&r,0,sizeof(r)); r.title=move?"Move":"Copy";
    selection(r.from);
    if(!ask("TWODLG",&r) || !r.to[0]) return;
    full(from,r.from); full(to,r.to);
    if(is_dir(from)) {
        if(!move) {MessageBox(frame,"Directories cannot be copied; copy the files in them.","File Manager",MB_OK|MB_ICONEXCLAMATION); return;}
    }
    if(is_dir(to)) {char t[MAXP]; join(t,to,FileTitle(from)); lstrcpy(to,t);}
    if(move) {
        if(!MoveFile(from,to)) {
            DWORD e=GetLastError();
            /* Across drives a file is copied, then deleted. */
            if(is_dir(from) || (from[0]|0x20)==(to[0]|0x20)) {fail("move",from,e); refresh_all(); return;}
            if(!CopyFile(from,to,TRUE)) {fail("move",from,GetLastError()); refresh_all(); return;}
            if(!DeleteFile(from)) {fail("delete",from,GetLastError()); refresh_all(); return;}
        }
        done("moved",from,to);
    } else {
        if(!CopyFile(from,to,FALSE)) {fail("copy",from,GetLastError()); refresh_all(); return;}
        done("copied",from,to);
    }
    refresh_all();
}
static void rename_file(void) {
    Request r; char from[MAXP],to[MAXP];
    memset(&r,0,sizeof(r)); r.title="Rename";
    selection(r.from);
    if(!ask("TWODLG",&r) || !r.to[0]) return;
    full(from,r.from);
    if(r.to[1]==':' || r.to[0]=='\\') lstrcpy(to,r.to);
    else {char dir[MAXP]; lstrcpy(dir,from); parent(dir); join(to,dir,r.to);}
    if(!MoveFile(from,to)) {fail("rename",from,GetLastError()); return;}
    done("renamed",from,to);
    refresh_all();
}
static void delete_file(void) {
    Request r; char path[MAXP],text[200];
    memset(&r,0,sizeof(r)); r.title="Delete"; r.label="&Delete:";
    selection(r.from);
    if(!ask("ONEDLG",&r)) return;
    full(path,r.from);
    if(lstrlen(path)<=3) {MessageBox(frame,"A root directory cannot be deleted.","File Manager",MB_OK|MB_ICONEXCLAMATION); return;}
    if(is_dir(path)) {
        char cwd[MAXP];
        wsprintf(text,"Delete the directory %s and everything in it?",path);
        if(MessageBox(frame,text,"Confirm Directory Delete",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        GetCurrentDirectory(sizeof(cwd),cwd);
        if(!lstrcmpi(cwd,path) || (lstrlen(cwd)>lstrlen(path) && !memcmp(cwd,path,(size_t)lstrlen(path)) && cwd[lstrlen(path)]=='\\')) {
            char root[4]; root[0]=path[0]; root[1]=':'; root[2]='\\'; root[3]=0; SetCurrentDirectory(root);
        }
        if(!delete_tree(path)) {refresh_all(); return;}
    } else {
        wsprintf(text,"Delete the file %s?",path);
        if(MessageBox(frame,text,"Confirm File Delete",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        if(!DeleteFile(path)) {fail("delete",path,GetLastError()); return;}
    }
    done("deleted",path,NULL);
    refresh_all();
}
static void make_dir(void) {
    Request r; char path[MAXP];
    memset(&r,0,sizeof(r)); r.title="Create Directory"; r.label="&Name:";
    if(!ask("ONEDLG",&r)) return;
    full(path,r.from);
    if(!CreateDirectory(path,NULL)) {fail("create",path,GetLastError()); return;}
    done("created",path,NULL);
    refresh_all();
}
static void run_dialog(void) {
    Request r;
    memset(&r,0,sizeof(r)); r.title="Run"; r.label="&Command:";
    if(ask("ONEDLG",&r)) run(r.from);
}
static void command(HWND h,UINT id) {
    HWND child=active_child();
    if(HelpCommand(h,id,"WINFILE.HLP")) return;
    switch(id) {
    case IDM_OPEN: if(child) open_selection(child); return;
    case IDM_MOVE: copy_or_move(TRUE); return;
    case IDM_COPY: copy_or_move(FALSE); return;
    case IDM_DELETE: delete_file(); return;
    case IDM_RENAME: rename_file(); return;
    case IDM_MKDIR: make_dir(); return;
    case IDM_RUN: run_dialog(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_DRIVE: {
        INT_PTR d=DialogBox(instance,"DRIVEDLG",h,DriveProc);
        if(d) {tree_load((char)d); InvalidateRect(h,NULL,FALSE); if(IsIconic(tree)) ShowWindow(tree,SW_RESTORE); SendMessage(client,WM_MDIACTIVATE,(WPARAM)tree,0);}
        return;
    }
    case IDM_NAME: case IDM_DETAILS: {
        HWND c;
        details=id==IDM_DETAILS;
        for(c=GetWindow(client,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT)) {DirWin *w=dir_of(c); if(w) InvalidateRect(w->list,NULL,TRUE);}
        return;
    }
    case IDM_NEWWINDOW: {char dir[MAXP]; current_dir(dir); open_dir_window(dir); return;}
    case IDM_CASCADE: SendMessage(client,WM_MDICASCADE,0,0); return;
    case IDM_TILE: SendMessage(client,WM_MDITILE,0,0); return;
    case IDM_ARRANGE: SendMessage(client,WM_MDIICONARRANGE,0,0); return;
    case IDM_REFRESH: refresh_all(); return;
    case IDM_ABOUT: MessageBox(h,"File Manager\nInterface Manager 3.0 for IA-64","About File Manager",MB_OK|MB_ICONINFORMATION); return;
    }
}

/* --- the frame ----------------------------------------------------------------------------------------- */
static void paint_status(HWND h,HDC dc) {
    RECT r; char dir[MAXP],text[160],free_text[16],total_text[16]; DWORD spc,bps,avail,total; HWND child=active_child(); DirWin *w=dir_of(child);
    GetClientRect(h,&r);
    r.top=r.bottom-status_height;
    FillRect(dc,&r,(HBRUSH)(COLOR_BTNFACE+1));
    {RECT l=r; l.bottom=l.top+1; FillRect(dc,&l,GetStockObject(BLACK_BRUSH));}
    current_dir(dir);
    dir[3]=0;
    text[0]=0;
    if(GetDiskFreeSpace(dir,&spc,&bps,&avail,&total)) {
        number((DWORD)(((ULONGLONG)avail*spc*bps)/1024),free_text); number((DWORD)(((ULONGLONG)total*spc*bps)/1024),total_text);
        wsprintf(text,"%c: %sKB free, %sKB total",dir[0],free_text,total_text);
    }
    if(w) {char more[48]; int files=w->count-(w->count && !lstrcmp(w->entries[0].name,"..")); wsprintf(more,"    %d item(s)",files); lstrcat(text,more);}
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_BTNTEXT));
    TextOut(dc,6,r.top+3,text,lstrlen(text));
}
LRESULT CALLBACK FrameProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        CLIENTCREATESTRUCT ccs; MDICREATESTRUCT mcs; TEXTMETRIC tm; HDC dc=GetDC(h);
        GetTextMetrics(dc,&tm); ReleaseDC(h,dc); status_height=(int)tm.tmHeight+6;
        ccs.hWindowMenu=GetSubMenu(GetMenu(h),3); ccs.idFirstChild=IDM_FIRSTCHILD;
        frame=h;
        client=CreateWindow("MDICLIENT",NULL,WS_CHILD|WS_CLIPCHILDREN|WS_VISIBLE,0,0,0,0,h,(HMENU)1,instance,&ccs);
        {RECT r; GetClientRect(h,&r); MoveWindow(client,0,0,r.right,r.bottom-status_height,TRUE);}
        {RECT r; GetClientRect(client,&r);
         mcs.szClass="WFTree"; mcs.szTitle="Directory Tree"; mcs.hOwner=instance;
         mcs.x=0; mcs.y=0; mcs.cx=r.right*2/5; mcs.cy=r.bottom; mcs.style=0; mcs.lParam=0;
         SendMessage(client,WM_MDICREATE,0,(LPARAM)&mcs);}
        return 0;
    }
    case WM_SIZE: {
        RECT r; GetClientRect(h,&r);
        MoveWindow(client,0,0,r.right,max(0,(int)r.bottom-status_height),TRUE);
        InvalidateRect(h,NULL,TRUE);
        return 0;
    }
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint_status(h,dc); EndPaint(h,&ps); return 0;}
    case WM_INITMENUPOPUP: {
        /* Move, Copy, Delete and Rename stay enabled: their dialogs take any path. */
        HMENU m=(HMENU)wp;
        if(HIWORD(lp)) break;
        CheckMenuItem(m,IDM_NAME,MF_BYCOMMAND|(details?MF_UNCHECKED:MF_CHECKED));
        CheckMenuItem(m,IDM_DETAILS,MF_BYCOMMAND|(details?MF_CHECKED:MF_UNCHECKED));
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)<IDM_FIRSTCHILD) {command(h,LOWORD(wp)); return 0;}
        break;
    case WM_DESTROY: WinHelp(h,"WINFILE.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefFrameProc(h,client,msg,wp,lp);
}

int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR cmdline,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; char cwd[MAXP];
    instance=inst;
    if(previous) return 0;
    trace=cmdline && !lstrcmpi(cmdline,"/trace");
    if(!(nodes=(Node *)GlobalAlloc(GPTR,sizeof(Node)*NODES))) return 1;
    if(GetCurrentDirectory(sizeof(cwd),cwd) && cwd[1]==':') drive=(char)(cwd[0]&~0x20);
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=FrameProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"WINFILE"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_APPWORKSPACE+1); wc.lpszMenuName="WINFILE"; wc.lpszClassName="WFS_Frame";
    RegisterClass(&wc);
    wc.lpszMenuName=NULL; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.cbWndExtra=sizeof(LONG_PTR);
    wc.lpfnWndProc=TreeProc; wc.lpszClassName="WFTree"; RegisterClass(&wc);
    wc.lpfnWndProc=DirProc; wc.lpszClassName="WFDir"; RegisterClass(&wc);
    frame=CreateWindow("WFS_Frame","File Manager",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,
                       GetSystemMetrics(SM_CXSCREEN)*4/5,GetSystemMetrics(SM_CYSCREEN)*4/5,NULL,NULL,inst,NULL);
    ShowWindow(frame,show); UpdateWindow(frame);
    SendMessage(client,WM_MDIACTIVATE,(WPARAM)tree,0);
    report("WINFILE: ready");
    accel=LoadAccelerators(inst,"WINFILE");
    while(GetMessage(&m,NULL,0,0))
        if(!TranslateMDISysAccel(client,&m) && !TranslateAccelerator(frame,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    GlobalFree(nodes);
    return (int)m.wParam;
}
