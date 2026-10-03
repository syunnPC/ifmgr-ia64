/* SPDX-License-Identifier: GPL-2.0-or-later
 * The EDIT control: single-line and multiline (with word wrap when it has
 * no ES_AUTOHSCROLL), selection with the mouse and the keyboard, the
 * clipboard, one level of undo, scroll bars, tabs and password characters.
 * Line breaks are CR LF. Positions are in pixels, from the font's
 * character widths; tab stops are every eight average characters (or as
 * EM_SETTABSTOPS sets them, in dialog units).
 */
#include "user.h"
#define MARGIN 2

typedef struct {
    char *text; int len,cap,limit;
    int anchor,caret;              /* the selection runs between them */
    int *lines,nlines,lines_cap;   /* start of each displayed line */
    int top,xoff;                  /* first visible line, horizontal scroll in pixels */
    BOOL modified,focused,captured,typing;
    char *undo; int undo_len,undo_anchor,undo_caret; BOOL can_undo;
    char password; int tab;        /* tab stop in pixels */
    int cw,lh;                     /* average character width and line height */
    int widths[256];               /* the font's character widths */
    HLOCAL handle;
} Edit;

static Edit *edit_of(HWND h) {return (Edit *)GetWindowLongPtr(h,0);}
static BOOL multiline(Wnd *w) {return (w->style&ES_MULTILINE)!=0;}
static BOOL wraps(Wnd *w) {return multiline(w) && !(w->style&ES_AUTOHSCROLL) && !(w->style&WS_HSCROLL);}
static int sel_min(Edit *e) {return min(e->anchor,e->caret);}
static int sel_max(Edit *e) {return max(e->anchor,e->caret);}
static void format_rect(Wnd *w,RECT *r) {
    GetClientRect(w->handle,r);
    InflateRect(r,-MARGIN,0);
    if(multiline(w)) r->top+=1;
    if(r->right<r->left) r->right=r->left;
}
static int visible_lines(Wnd *w,Edit *e) {RECT r; format_rect(w,&r); return max(1,(r.bottom-r.top)/e->lh);}
static void metrics(Wnd *w,Edit *e) {
    HDC dc=CreateCompatibleDC(NULL); TEXTMETRIC tm;
    SelectObject(dc,w->font?w->font:SystemFont()); GetTextMetrics(dc,&tm); GetCharWidth(dc,0,255,e->widths); DeleteDC(dc);
    e->cw=max(1,(int)tm.tmAveCharWidth); e->lh=(int)tm.tmHeight;
    e->tab=8*e->cw;
}
static BOOL reserve(Edit *e,int n) {
    char *t;
    if(n+1<=e->cap) return TRUE;
    n=max(n+1,e->cap*2);
    if(!(t=(char *)GlobalAlloc(GPTR|GMEM_SHARE,(DWORD)n))) return FALSE;
    memcpy(t,e->text,(size_t)e->len+1);
    GlobalFree(e->text); e->text=t; e->cap=n;
    return TRUE;
}

/* --- lines ----------------------------------------------------------------------- */
static int advance(Edit *e,char c,int x) {return c=='\t'?(x/e->tab+1)*e->tab:x+e->widths[(BYTE)c];}
static void push_line(Edit *e,int start) {
    if(e->nlines==e->lines_cap) {
        int n=e->lines_cap?e->lines_cap*2:64; int *l=(int *)GlobalAlloc(GPTR|GMEM_SHARE,(DWORD)(n*sizeof(int)));
        if(!l) return;
        if(e->nlines) memcpy(l,e->lines,sizeof(int)*(unsigned)e->nlines);
        if(e->lines) GlobalFree(e->lines);
        e->lines=l; e->lines_cap=n;
    }
    e->lines[e->nlines++]=start;
}
static void build_lines(Wnd *w,Edit *e) {
    int pos=0,cols; RECT r;
    e->nlines=0;
    format_rect(w,&r); cols=max(1,r.right-r.left);
    push_line(e,0);
    if(!multiline(w)) return;
    while(pos<e->len) {
        int start=pos,col=0,brk=-1;
        while(pos<e->len && e->text[pos]!='\r' && e->text[pos]!='\n') {
            int next=advance(e,e->password?e->password:e->text[pos],col);
            if(wraps(w) && next>cols && pos>start) {
                if(brk>start) pos=brk;
                break;
            }
            col=next;
            if(e->text[pos]==' ' || e->text[pos]=='\t') brk=pos+1;
            pos++;
        }
        if(pos<e->len && (e->text[pos]=='\r' || e->text[pos]=='\n')) {
            if(e->text[pos]=='\r' && pos+1<e->len && e->text[pos+1]=='\n') pos++;
            pos++;
        } else if(pos>=e->len) break;
        push_line(e,pos);
    }
}
static int line_of(Edit *e,int pos) {
    int lo=0,hi=e->nlines-1;
    while(lo<hi) {int mid=(lo+hi+1)/2; if(e->lines[mid]<=pos) lo=mid; else hi=mid-1;}
    return lo;
}
/* The line's text end, before its line break. */
static int line_end(Edit *e,int line) {
    int end=line+1<e->nlines?e->lines[line+1]:e->len;
    if(end>e->lines[line] && e->text[end-1]=='\n') end--;
    if(end>e->lines[line] && e->text[end-1]=='\r') end--;
    return end;
}
static int col_of(Edit *e,int line,int pos) {
    int i,col=0;
    for(i=e->lines[line];i<pos && i<e->len;i++) col=advance(e,e->password?e->password:e->text[i],col);
    return col;
}
/* The position nearest x on a line. */
static int pos_of(Edit *e,int line,int x) {
    int i,col=0,end=line_end(e,line);
    for(i=e->lines[line];i<end;i++) {
        int next=advance(e,e->password?e->password:e->text[i],col);
        if(next>x) return (x-col)*2>=(next-col)?i+1:i;
        col=next;
    }
    return end;
}

/* --- drawing and scrolling ---------------------------------------------------------- */
static void update_bars(Wnd *w,Edit *e) {
    if(w->style&WS_VSCROLL) {SetScrollRange(w->handle,SB_VERT,0,max(0,e->nlines-1),FALSE); SetScrollPos(w->handle,SB_VERT,e->top,TRUE);}
    if(w->style&WS_HSCROLL) {SetScrollRange(w->handle,SB_HORZ,0,100,FALSE); SetScrollPos(w->handle,SB_HORZ,min(100,e->xoff/max(1,e->cw)),TRUE);}
}
static void place_caret(Wnd *w,Edit *e) {
    RECT r; int line=line_of(e,e->caret);
    if(!e->focused) return;
    format_rect(w,&r);
    SetCaretPos(r.left+col_of(e,line,e->caret)-e->xoff,r.top+(line-e->top)*e->lh+(multiline(w)?0:max(0,(r.bottom-r.top-e->lh)/2)));
}
static void paint(Wnd *w,Edit *e,HDC dc) {
    RECT r,c; int line,y,last; HBRUSH bg; COLORREF fg; HGDIOBJ old; char buf[512];
    BOOL show_sel=e->focused || (w->style&ES_NOHIDESEL);
    GetClientRect(w->handle,&c);
    bg=(HBRUSH)SendMessage(w->parent->handle,WM_CTLCOLOREDIT,(WPARAM)dc,(LPARAM)w->handle);
    if(!bg) bg=ControlColor(w,dc,WM_CTLCOLOREDIT);
    fg=Enabled(w)?GetTextColor(dc):GetSysColor(COLOR_GRAYTEXT);
    FillRect(dc,&c,bg);
    old=SelectObject(dc,w->font?w->font:SystemFont());
    format_rect(w,&r);
    IntersectClipRect(dc,r.left,c.top,r.right,c.bottom);
    y=r.top+(multiline(w)?0:max(0,(r.bottom-r.top-e->lh)/2));
    last=multiline(w)?min(e->nlines,e->top+visible_lines(w,e)+1):1;
    for(line=multiline(w)?e->top:0;line<last;line++,y+=e->lh) {
        int i,end=line_end(e,line),col=0,n;
        int smin=show_sel?sel_min(e):-1,smax=show_sel?sel_max(e):-1;
        /* Runs of selected and unselected text; a tab is the space to its stop. */
        i=e->lines[line];
        while(i<end) {
            BOOL sel=i>=smin && i<smax; int startcol=col,j;
            n=0;
            for(j=i;j<end && (j>=smin && j<smax)==sel && n<500;j++) {
                char ch=e->password?e->password:e->text[j];
                if(ch=='\t') break;
                buf[n++]=ch; col=advance(e,ch,col);
            }
            if(sel) {SetBkColor(dc,GetSysColor(COLOR_HIGHLIGHT)); SetTextColor(dc,GetSysColor(COLOR_HIGHLIGHTTEXT)); SetBkMode(dc,OPAQUE);}
            else {SetTextColor(dc,fg); SetBkMode(dc,TRANSPARENT);}
            if(n) TextOut(dc,r.left+startcol-e->xoff,y,buf,n);
            if(j<end && j==i+n && e->text[j]=='\t' && !e->password) {
                int next=advance(e,'\t',col);
                if(j>=smin && j<smax) {RECT t; SetRect(&t,r.left+col-e->xoff,y,r.left+next-e->xoff,y+e->lh); FillRect(dc,&t,SysBrush(COLOR_HIGHLIGHT));}
                col=next; j++;
            }
            i=j;
        }
        /* A selected line break shows as a cell of highlight. */
        if(show_sel && multiline(w) && end<e->len && end>=smin && end<smax) {
            RECT s; SetRect(&s,r.left+col-e->xoff,y,r.left+col-e->xoff+e->cw/2,y+e->lh);
            FillRect(dc,&s,SysBrush(COLOR_HIGHLIGHT));
        }
    }
    SetBkMode(dc,OPAQUE);
    SelectObject(dc,old);
}
static void redraw(Wnd *w,Edit *e) {
    HDC dc;
    if(!Shown(w)) return;
    HideCaret(w->handle);
    dc=GetDC(w->handle);
    paint(w,e,dc);
    ReleaseDC(w->handle,dc);
    place_caret(w,e);
    ShowCaret(w->handle);
}
/* Scroll so the caret shows; TRUE if it scrolled. */
static BOOL scroll_to_caret(Wnd *w,Edit *e) {
    RECT r; int line=line_of(e,e->caret),x,vis=visible_lines(w,e),oldtop=e->top,oldx=e->xoff;
    format_rect(w,&r);
    if(multiline(w)) {
        if(line<e->top) e->top=line;
        else if(line>=e->top+vis) e->top=line-vis+1;
    }
    x=col_of(e,line,e->caret);
    if(!wraps(w)) {
        int width=r.right-r.left;
        if(x<e->xoff) e->xoff=max(0,x-width/3);
        else if(x>=e->xoff+width) e->xoff=x-width+width/3;
    }
    if(oldtop!=e->top || oldx!=e->xoff) {
        if(oldtop!=e->top) SendNotify(w,EN_VSCROLL);
        if(oldx!=e->xoff) SendNotify(w,EN_HSCROLL);
        return TRUE;
    }
    return FALSE;
}
static void changed(Wnd *w,Edit *e,BOOL text) {
    if(text) {build_lines(w,e); e->modified=TRUE;}
    if(e->top>max(0,e->nlines-1)) e->top=max(0,e->nlines-1);
    scroll_to_caret(w,e);
    if(text) SendNotify(w,EN_UPDATE);
    redraw(w,e);
    update_bars(w,e);
    if(text) SendNotify(w,EN_CHANGE);
}

/* --- editing --------------------------------------------------------------------------- */
static void save_undo(Edit *e) {
    char *u=(char *)GlobalAlloc(GPTR|GMEM_SHARE,(DWORD)e->len+1);
    if(!u) return;
    memcpy(u,e->text,(size_t)e->len+1);
    if(e->undo) GlobalFree(e->undo);
    e->undo=u; e->undo_len=e->len; e->undo_anchor=e->anchor; e->undo_caret=e->caret; e->can_undo=TRUE;
}
/* Replace the selection with s; FALSE when the limit or memory stops it. */
static BOOL replace(Wnd *w,Edit *e,const char *s,int n,BOOL typed) {
    int a=sel_min(e),b=sel_max(e),i,k=0; char *conv=NULL;
    if(!typed || !e->typing) save_undo(e);
    e->typing=typed;
    if(e->limit && e->len-(b-a)+n>e->limit) {
        n=max(0,e->limit-(e->len-(b-a)));
        if(!n && b==a) {SendNotify(w,EN_MAXTEXT); return FALSE;}
    }
    if(w->style&(ES_UPPERCASE|ES_LOWERCASE) || !multiline(w)) {
        if(!(conv=(char *)GlobalAlloc(GPTR,(DWORD)n+1))) return FALSE;
        for(i=0;i<n;i++) {
            char c=s[i];
            if(!multiline(w) && (c=='\r' || c=='\n')) continue;
            if(w->style&ES_UPPERCASE) c=(char)(ULONG_PTR)AnsiUpper((LPSTR)(ULONG_PTR)(BYTE)c);
            if(w->style&ES_LOWERCASE) c=(char)(ULONG_PTR)AnsiLower((LPSTR)(ULONG_PTR)(BYTE)c);
            conv[k++]=c;
        }
        s=conv; n=k;
    }
    if(!reserve(e,e->len-(b-a)+n)) {if(conv) GlobalFree(conv); SendNotify(w,EN_ERRSPACE); return FALSE;}
    memmove(e->text+a+n,e->text+b,(size_t)(e->len-b+1));
    memcpy(e->text+a,s,(size_t)n);
    e->len+=n-(b-a);
    e->anchor=e->caret=a+n;
    if(conv) GlobalFree(conv);
    changed(w,e,TRUE);
    return TRUE;
}
static void set_text(Wnd *w,Edit *e,LPCSTR s) {
    int n=s?lstrlen(s):0;
    if(!reserve(e,n)) {SendNotify(w,EN_ERRSPACE); return;}
    memcpy(e->text,s?s:"",(size_t)n+1); e->len=n;
    e->anchor=e->caret=0; e->top=0; e->xoff=0; e->can_undo=FALSE; e->typing=FALSE;
    changed(w,e,TRUE);
    e->modified=FALSE;
}
static void copy(Edit *e,HWND h) {
    int a=sel_min(e),b=sel_max(e); HGLOBAL g; char *p;
    if(a==b || e->password) return;
    if(!(g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,(DWORD)(b-a+1)))) return;
    p=(char *)GlobalLock(g); memcpy(p,e->text+a,(size_t)(b-a)); p[b-a]=0; GlobalUnlock(g);
    if(OpenClipboard(h)) {EmptyClipboard(); SetClipboardData(CF_TEXT,g); CloseClipboard();}
    else GlobalFree(g);
}
static void paste(Wnd *w,Edit *e) {
    HGLOBAL g; const char *p;
    if(!OpenClipboard(w->handle)) return;
    if((g=GetClipboardData(CF_TEXT))!=NULL && (p=(const char *)GlobalLock(g))!=NULL) {
        replace(w,e,p,lstrlen(p),FALSE); GlobalUnlock(g);
    }
    CloseClipboard();
}
static void undo(Wnd *w,Edit *e) {
    char *t; int len,anchor,caret;
    if(!e->can_undo || !e->undo) return;
    /* Undo swaps with the current text, so a second undo redoes. */
    t=e->undo; len=e->undo_len; anchor=e->undo_anchor; caret=e->undo_caret;
    e->undo=NULL; save_undo(e);
    if(reserve(e,len)) {memcpy(e->text,t,(size_t)len+1); e->len=len; e->anchor=min(anchor,len); e->caret=min(caret,len);}
    GlobalFree(t);
    e->typing=FALSE;
    changed(w,e,TRUE);
}
static void move_caret(Wnd *w,Edit *e,int pos,BOOL extend) {
    pos=max(0,min(pos,e->len));
    /* Never between CR and LF. */
    if(pos>0 && pos<e->len && e->text[pos-1]=='\r' && e->text[pos]=='\n') pos+=pos>e->caret?1:-1;
    e->caret=pos;
    if(!extend) e->anchor=pos;
    e->typing=FALSE;
    changed(w,e,FALSE);
}
static int word_left(Edit *e,int p) {
    while(p>0 && (e->text[p-1]==' ' || e->text[p-1]=='\t' || e->text[p-1]=='\r' || e->text[p-1]=='\n')) p--;
    while(p>0 && !(e->text[p-1]==' ' || e->text[p-1]=='\t' || e->text[p-1]=='\r' || e->text[p-1]=='\n')) p--;
    return p;
}
static int word_right(Edit *e,int p) {
    while(p<e->len && !(e->text[p]==' ' || e->text[p]=='\t' || e->text[p]=='\r' || e->text[p]=='\n')) p++;
    while(p<e->len && (e->text[p]==' ' || e->text[p]=='\t' || e->text[p]=='\r' || e->text[p]=='\n')) p++;
    return p;
}
static void key(Wnd *w,Edit *e,WPARAM vk) {
    BOOL shift=KeyDown(VK_SHIFT),ctrl=KeyDown(VK_CONTROL); int line=line_of(e,e->caret);
    switch(vk) {
    case VK_LEFT: move_caret(w,e,!shift && e->anchor!=e->caret?sel_min(e):ctrl?word_left(e,e->caret):e->caret-1,shift); break;
    case VK_RIGHT: move_caret(w,e,!shift && e->anchor!=e->caret?sel_max(e):ctrl?word_right(e,e->caret):e->caret+1,shift); break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        int delta=vk==VK_UP?-1:vk==VK_DOWN?1:vk==VK_PRIOR?-visible_lines(w,e):visible_lines(w,e),target,x;
        if(!multiline(w)) {if(vk==VK_UP) move_caret(w,e,e->caret-1,shift); else if(vk==VK_DOWN) move_caret(w,e,e->caret+1,shift); break;}
        target=max(0,min(line+delta,e->nlines-1));
        if(vk==VK_PRIOR || vk==VK_NEXT) e->top=max(0,min(e->top+delta,max(0,e->nlines-visible_lines(w,e))));
        x=col_of(e,line,e->caret);
        move_caret(w,e,pos_of(e,target,x),shift);
        break;
    }
    case VK_HOME: move_caret(w,e,ctrl?0:e->lines[line],shift); break;
    case VK_END: move_caret(w,e,ctrl?e->len:line_end(e,line),shift); break;
    case VK_DELETE:
        if(w->style&ES_READONLY) break;
        if(shift) {copy(e,w->handle); if(e->anchor!=e->caret) replace(w,e,"",0,FALSE); break;}
        if(e->anchor==e->caret) {
            if(e->caret>=e->len) break;
            e->caret+=e->text[e->caret]=='\r' && e->caret+1<e->len && e->text[e->caret+1]=='\n'?2:1;
        }
        replace(w,e,"",0,FALSE);
        break;
    case VK_INSERT:
        if(ctrl) copy(e,w->handle);
        else if(shift && !(w->style&ES_READONLY)) paste(w,e);
        break;
    }
}
static void character(Wnd *w,Edit *e,WPARAM ch) {
    char c=(char)ch;
    switch(ch) {
    case 3: copy(e,w->handle); return;
    case 22: if(!(w->style&ES_READONLY)) paste(w,e); return;
    case 24: if(!(w->style&ES_READONLY)) {copy(e,w->handle); if(e->anchor!=e->caret) replace(w,e,"",0,FALSE);} return;
    case 26: if(!(w->style&ES_READONLY)) undo(w,e); return;
    case 1: e->anchor=0; e->caret=e->len; changed(w,e,FALSE); return;
    }
    if(w->style&ES_READONLY) {MessageBeep(0); return;}
    if(ch=='\b') {
        if(e->anchor==e->caret) {
            if(!e->caret) return;
            e->anchor=e->caret-(e->caret>=2 && e->text[e->caret-1]=='\n' && e->text[e->caret-2]=='\r'?2:1);
        }
        replace(w,e,"",0,FALSE);
        return;
    }
    if(ch=='\r') {if(multiline(w)) replace(w,e,"\r\n",2,TRUE); return;}
    if(ch=='\t' && !multiline(w)) return;
    if((BYTE)c<' ' && c!='\t') return;
    replace(w,e,&c,1,TRUE);
}
static int hit(Wnd *w,Edit *e,int x,int y) {
    RECT r; int line;
    format_rect(w,&r);
    line=multiline(w)?e->top+(y-r.top)/e->lh:0;
    if(y<r.top && multiline(w)) line=e->top-1;
    line=max(0,min(line,e->nlines-1));
    return pos_of(e,line,max(0,x-r.left+e->xoff));
}

/* --- the window procedure --------------------------------------------------------------- */
LRESULT CALLBACK EditProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h); Edit *e=edit_of(h);
    if(!w) return 0;
    if(!e && msg!=WM_NCCREATE) return DefWindowProc(h,msg,wp,lp);
    switch(msg) {
    case WM_NCCREATE: {
        LPCREATESTRUCT cs=(LPCREATESTRUCT)lp;
        e=(Edit *)GlobalAlloc(GPTR|GMEM_SHARE,sizeof(Edit));
        if(!e) return FALSE;
        e->text=(char *)GlobalAlloc(GPTR|GMEM_SHARE,64); e->cap=64; e->tab=8;
        if(!e->text) {GlobalFree(e); return FALSE;}
        SetWindowLongPtr(h,0,(LONG_PTR)e);
        if(w->style&ES_PASSWORD) e->password='*';
        if(!(w->style&ES_MULTILINE)) w->style&=~(WS_HSCROLL|WS_VSCROLL);
        metrics(w,e);
        DefWindowProc(h,msg,wp,lp);
        if(cs->lpszName && reserve(e,lstrlen(cs->lpszName))) {e->len=lstrlen(cs->lpszName); memcpy(e->text,cs->lpszName,(size_t)e->len+1);}
        build_lines(w,e);
        return TRUE;
    }
    case WM_NCDESTROY:
        if(e->undo) GlobalFree(e->undo);
        if(e->lines) GlobalFree(e->lines);
        GlobalFree(e->text); GlobalFree(e);
        SetWindowLongPtr(h,0,0);
        return 0;
    case WM_SIZE: build_lines(w,e); scroll_to_caret(w,e); update_bars(w,e); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(w,e,dc); EndPaint(h,&ps); place_caret(w,e); return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS:
        e->focused=TRUE;
        CreateCaret(h,NULL,1,e->lh); place_caret(w,e); ShowCaret(h);
        redraw(w,e);
        SendNotify(w,EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        e->focused=FALSE; DestroyCaret();
        redraw(w,e);
        SendNotify(w,EN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int p=hit(w,e,GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
        if(!e->focused) SetFocus(h);
        if(msg==WM_LBUTTONDBLCLK) {e->anchor=word_left(e,min(p+1,e->len)); e->caret=p; while(e->caret<e->len && e->text[e->caret]!=' ' && e->text[e->caret]!='\r' && e->text[e->caret]!='\n' && e->text[e->caret]!='\t') e->caret++; changed(w,e,FALSE); return 0;}
        move_caret(w,e,p,(wp&MK_SHIFT)!=0);
        SetCapture(h); e->captured=TRUE;
        return 0;
    }
    case WM_MOUSEMOVE:
        if(e->captured && GetCapture()==h) {
            int p=hit(w,e,GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
            if(p!=e->caret) move_caret(w,e,p,TRUE);
        }
        return 0;
    case WM_LBUTTONUP: if(e->captured) {ReleaseCapture(); e->captured=FALSE;} return 0;
    case WM_KEYDOWN: key(w,e,wp); return 0;
    case WM_CHAR: character(w,e,wp); return 0;
    case WM_GETDLGCODE: {
        LRESULT code=DLGC_WANTCHARS|DLGC_HASSETSEL|DLGC_WANTARROWS;
        if(multiline(w)) code|=DLGC_WANTALLKEYS;
        if(lp && multiline(w)) {
            const MSG *m=(const MSG *)lp;
            if(m->message==WM_KEYDOWN && (m->wParam==VK_ESCAPE || m->wParam==VK_TAB)) code&=~DLGC_WANTALLKEYS;
            if(m->message==WM_KEYDOWN && m->wParam==VK_RETURN && !(w->style&ES_WANTRETURN) && w->parent!=desktop &&
               w->parent->cls->wc.cbWndExtra>=DLGWINDOWEXTRA && GetWindowLongPtr(w->parent->handle,DWLP_DLGPROC)) code&=~DLGC_WANTALLKEYS;
        }
        return code;
    }
    case WM_SETTEXT: set_text(w,e,(LPCSTR)lp); return TRUE;
    case WM_GETTEXT: {
        int n=min(e->len,(int)wp-1);
        if(!lp || (int)wp<=0) return 0;
        memcpy((char *)lp,e->text,(size_t)max(n,0)); ((char *)lp)[max(n,0)]=0;
        return max(n,0);
    }
    case WM_GETTEXTLENGTH: return e->len;
    case WM_SETFONT: w->font=(HFONT)wp; metrics(w,e); build_lines(w,e); if(e->focused) {CreateCaret(h,NULL,1,e->lh); place_caret(w,e); ShowCaret(h);} if(lp) InvalidateRect(h,NULL,TRUE); return 0;
    case WM_ENABLE: InvalidateRect(h,NULL,TRUE); return 0;
    case WM_CUT: copy(e,h); if(e->anchor!=e->caret && !(w->style&ES_READONLY)) replace(w,e,"",0,FALSE); return 0;
    case WM_COPY: copy(e,h); return 0;
    case WM_PASTE: if(!(w->style&ES_READONLY)) paste(w,e); return 0;
    case WM_CLEAR: if(e->anchor!=e->caret && !(w->style&ES_READONLY)) replace(w,e,"",0,FALSE); return 0;
    case WM_UNDO: case EM_UNDO: undo(w,e); return TRUE;
    case WM_VSCROLL: case WM_HSCROLL: {
        int code=LOWORD(wp),vis=visible_lines(w,e);
        if(msg==WM_VSCROLL) {
            int top=e->top;
            switch(code) {
            case SB_LINEUP: top--; break;
            case SB_LINEDOWN: top++; break;
            case SB_PAGEUP: top-=vis; break;
            case SB_PAGEDOWN: top+=vis; break;
            case SB_THUMBPOSITION: case SB_THUMBTRACK: top=(short)HIWORD(wp); break;
            case SB_TOP: top=0; break;
            case SB_BOTTOM: top=e->nlines-1; break;
            default: return 0;
            }
            top=max(0,min(top,max(0,e->nlines-1)));
            if(top!=e->top) {e->top=top; SendNotify(w,EN_VSCROLL); redraw(w,e); update_bars(w,e);}
        } else {
            int x=e->xoff; RECT r; format_rect(w,&r);
            switch(code) {
            case SB_LINELEFT: x-=e->cw; break;
            case SB_LINERIGHT: x+=e->cw; break;
            case SB_PAGELEFT: x-=r.right-r.left; break;
            case SB_PAGERIGHT: x+=r.right-r.left; break;
            case SB_THUMBPOSITION: case SB_THUMBTRACK: x=(short)HIWORD(wp)*e->cw; break;
            case SB_LEFT: x=0; break;
            default: return 0;
            }
            x=max(0,x);
            if(x!=e->xoff) {e->xoff=x; SendNotify(w,EN_HSCROLL); redraw(w,e); update_bars(w,e);}
        }
        return 0;
    }
    case EM_GETSEL:
        if(wp) *(LPDWORD)wp=(DWORD)sel_min(e);
        if(lp) *(LPDWORD)lp=(DWORD)sel_max(e);
        return MAKELONG(min(sel_min(e),0xffff),min(sel_max(e),0xffff));
    case EM_SETSEL: {
        int a=(int)wp,b=(int)lp;
        if(a<0) {e->anchor=e->caret; changed(w,e,FALSE); return 0;}
        if(b<0 || b>e->len) b=e->len;
        a=min(a,e->len);
        e->anchor=a; e->caret=b; e->typing=FALSE;
        changed(w,e,FALSE);
        return 0;
    }
    case EM_REPLACESEL: replace(w,e,lp?(LPCSTR)lp:"",lp?lstrlen((LPCSTR)lp):0,FALSE); return 0;
    case EM_GETLINECOUNT: return e->nlines;
    case EM_LINEINDEX: {
        int line=(int)wp<0?line_of(e,e->caret):(int)wp;
        return line>=0 && line<e->nlines?e->lines[line]:-1;
    }
    case EM_LINELENGTH: {
        int line;
        if((int)wp<0) {int a=line_of(e,sel_min(e)),b=line_of(e,sel_max(e)); return (sel_min(e)-e->lines[a])+(line_end(e,b)-sel_max(e));}
        line=line_of(e,min((int)wp,e->len));
        return line_end(e,line)-e->lines[line];
    }
    case EM_LINEFROMCHAR: return line_of(e,(int)wp<0?e->caret:min((int)wp,e->len));
    case EM_GETLINE: {
        int line=(int)wp,n,size;
        if(!lp || line<0 || line>=e->nlines) return 0;
        size=*(const WORD *)lp; n=min(line_end(e,line)-e->lines[line],size);
        memcpy((char *)lp,e->text+e->lines[line],(size_t)n);
        return n;
    }
    case EM_LIMITTEXT: e->limit=(int)wp; return 0;
    case EM_CANUNDO: return e->can_undo;
    case EM_EMPTYUNDOBUFFER: e->can_undo=FALSE; return 0;
    case EM_GETMODIFY: return e->modified;
    case EM_SETMODIFY: e->modified=(BOOL)wp; return 0;
    case EM_GETFIRSTVISIBLELINE: return multiline(w)?e->top:0;
    case EM_LINESCROLL:
        e->top=max(0,min(e->top+(int)lp,max(0,e->nlines-1)));
        e->xoff=max(0,e->xoff+(int)wp*e->cw);
        redraw(w,e); update_bars(w,e);
        return TRUE;
    case EM_SCROLL: SendMessage(h,WM_VSCROLL,wp,0); return 0;
    case EM_SCROLLCARET: if(scroll_to_caret(w,e)) {redraw(w,e); update_bars(w,e);} return 0;
    case EM_SETREADONLY: if(wp) w->style|=ES_READONLY; else w->style&=~ES_READONLY; return TRUE;
    case EM_SETPASSWORDCHAR: e->password=(char)wp; build_lines(w,e); redraw(w,e); return 0;
    case EM_GETPASSWORDCHAR: return (BYTE)e->password;
    case EM_SETTABSTOPS: {
        /* In dialog units, a quarter of the average character width. */
        int units=wp?*(const int *)lp:32;
        e->tab=max(1,units*e->cw/4); build_lines(w,e); redraw(w,e);
        return TRUE;
    }
    case EM_GETRECT: if(lp) format_rect(w,(LPRECT)lp); return 0;
    case EM_SETRECT: case EM_SETRECTNP: case EM_FMTLINES: return 0;
    case EM_GETHANDLE: {
        /* A copy of the text in a local handle; EM_SETHANDLE gives text back. */
        HLOCAL l=LocalAlloc(LMEM_FIXED,(UINT)e->len+1); char *p=(char *)LocalLock(l);
        if(p) {memcpy(p,e->text,(size_t)e->len+1); LocalUnlock(l);}
        if(e->handle) LocalFree(e->handle);
        e->handle=l; return (LRESULT)l;
    }
    case EM_SETHANDLE: {
        char *p=(char *)LocalLock((HLOCAL)wp);
        if(p) {set_text(w,e,p); LocalUnlock((HLOCAL)wp);}
        return 0;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}
