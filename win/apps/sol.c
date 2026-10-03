/* SPDX-License-Identifier: GPL-2.0-or-later
 * GDI Klondike Solitaire. Game rules and controls are in SOL.RTF.
 * /seed:n produces repeatable deals; /trace reports the table after each
 * change through OutputDebugString for QEMU tests.
 */
#include <windows.h>
#include "sol.h"
#include "winapp.h"
#define CARD_W 71
#define CARD_H 96
#define COL_W 81
#define LEFT 8
#define TOP 8
#define TAB_Y (TOP+CARD_H+12)
#define DOWN_STEP 5
#define UP_STEP 18
#define FAN 14
#define UP 0x40
#define PILES 13
#define DECK 0
#define WASTE 1
#define FOUND 2      /* 2..5 */
#define TABLEAU 6    /* 6..12 */
#define TABLE_COLOR RGB(0,128,0)

typedef struct {int n; int cards[52];} Pile;
static HINSTANCE instance;
static HWND main_wnd;
static Pile piles[PILES],undo_piles[PILES];
static BOOL can_undo,draw_three,red_back,trace,fixed_seed;
static DWORD seed;
static HDC scene_dc; static HBITMAP scene_bitmap,scene_old; static int scene_w,scene_h;
static HFONT big_font;
/* Dragging: cards from pile drag_pile at drag_index up, offset from the cursor. */
static BOOL dragging,moved; static int drag_pile,drag_index; static POINT drag_offset,drag_at,drag_start;

static int rank(int c) {return (c&0x3f)%13;}
static int suit(int c) {return (c&0x3f)/13;}         /* clubs, diamonds, hearts, spades */
static BOOL red(int c) {return suit(c)==1 || suit(c)==2;}
static void card_name(int c,char *out) {
    static const char *const ranks[13]={"A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    wsprintf(out,"%s%c",ranks[rank(c)],"CDHS"[suit(c)]);
}
static int top(int p) {return piles[p].n?piles[p].cards[piles[p].n-1]:-1;}

/* --- the game -------------------------------------------------------------------- */
static DWORD next_random(void) {seed=seed*1103515245u+12345u; return (seed>>16)&0x7fff;}
static void report(void) {
    char line[200]; int i,n=0; char name[8];
    if(!trace) return;
    n=wsprintf(line,"SOL: T");
    for(i=0;i<7;i++) {int c=top(TABLEAU+i); if(c>=0 && (c&UP)) {card_name(c,name); n+=wsprintf(line+n," %s",name);} else n+=wsprintf(line+n," -");}
    n+=wsprintf(line+n," | W");
    if(piles[WASTE].n) {card_name(top(WASTE),name); n+=wsprintf(line+n," %s",name);} else n+=wsprintf(line+n," -");
    n+=wsprintf(line+n," | F");
    for(i=0;i<4;i++) {if(piles[FOUND+i].n) {card_name(top(FOUND+i),name); n+=wsprintf(line+n," %s",name);} else n+=wsprintf(line+n," -");}
    wsprintf(line+n," | D %d",piles[DECK].n);
    OutputDebugString(line);
}
static void deal(void) {
    int cards[52],i,k,p;
    if(!fixed_seed) seed=GetTickCount();
    for(i=0;i<52;i++) cards[i]=i;
    for(i=51;i>0;i--) {int j=(int)(next_random()%(DWORD)(i+1)),t=cards[i]; cards[i]=cards[j]; cards[j]=t;}
    memset(piles,0,sizeof(piles));
    k=0;
    for(p=0;p<7;p++) for(i=0;i<=p;i++) piles[TABLEAU+p].cards[piles[TABLEAU+p].n++]=cards[k++]|(i==p?UP:0);
    while(k<52) piles[DECK].cards[piles[DECK].n++]=cards[k++];
    can_undo=FALSE;
}
static void remember(void) {memcpy(undo_piles,piles,sizeof(piles)); can_undo=TRUE;}
static BOOL fits_tableau(int c,int p) {
    int t=top(p);
    if(t<0) return rank(c)==12;
    return (t&UP) && rank(t)==rank(c)+1 && red(t)!=red(c);
}
static BOOL fits_foundation(int c,int f) {
    int t=top(FOUND+f);
    if(t<0) return rank(c)==0;
    return suit(t)==suit(c) && rank(t)+1==rank(c);
}
static BOOL won(void) {int f; for(f=0;f<4;f++) if(piles[FOUND+f].n!=13) return FALSE; return TRUE;}
/* Move cards from index on of pile 'from' to the top of 'to'. */
static void move_cards(int from,int index,int to) {
    int i;
    remember();
    for(i=index;i<piles[from].n;i++) piles[to].cards[piles[to].n++]=piles[from].cards[i]|UP;
    piles[from].n=index;
}
static BOOL legal(int from,int index,int to) {
    int c=piles[from].cards[index],count=piles[from].n-index;
    if(to==from || to==DECK || to==WASTE) return FALSE;
    if(to>=FOUND && to<FOUND+4) return count==1 && fits_foundation(c,to-FOUND);
    return fits_tableau(c,to);
}
static void deal_from_deck(void) {
    int i,count=draw_three?3:1;
    remember();
    if(!piles[DECK].n) {
        /* Turn the waste over. */
        for(i=piles[WASTE].n-1;i>=0;i--) piles[DECK].cards[piles[DECK].n++]=piles[WASTE].cards[i]&~UP;
        piles[WASTE].n=0;
        return;
    }
    for(i=0;i<count && piles[DECK].n;i++) piles[WASTE].cards[piles[WASTE].n++]=piles[DECK].cards[--piles[DECK].n]|UP;
}

/* --- drawing ------------------------------------------------------------------------- */
static int pile_x(int p) {
    if(p==DECK) return LEFT;
    if(p==WASTE) return LEFT+COL_W;
    if(p<TABLEAU) return LEFT+(p-FOUND+3)*COL_W;
    return LEFT+(p-TABLEAU)*COL_W;
}
/* Where card i of pile p lies. */
static void card_at(int p,int i,int *x,int *y) {
    *x=pile_x(p); *y=TOP;
    if(p==WASTE && draw_three) {int shown=min(3,piles[WASTE].n),first=piles[WASTE].n-shown; if(i>first) *x+=(i-first)*FAN;}
    if(p>=TABLEAU) {
        int k; *y=TAB_Y;
        for(k=0;k<i;k++) *y+=piles[p].cards[k]&UP?UP_STEP:DOWN_STEP;
    }
}
static void suit_shape(HDC dc,int s,int cx,int cy,int size) {
    HBRUSH b=CreateSolidBrush(s==1 || s==2?RGB(255,0,0):RGB(0,0,0)); HPEN pen=CreatePen(PS_SOLID,1,s==1 || s==2?RGB(255,0,0):RGB(0,0,0));
    HGDIOBJ ob=SelectObject(dc,b),op=SelectObject(dc,pen); int r=max(2,size/4); POINT p[4];
    switch(s) {
    case 1:
        p[0].x=cx; p[0].y=cy-size*6/10; p[1].x=cx+size/2; p[1].y=cy; p[2].x=cx; p[2].y=cy+size*6/10; p[3].x=cx-size/2; p[3].y=cy;
        Polygon(dc,p,4); break;
    case 2:
        Ellipse(dc,cx-2*r,cy-2*r,cx+1,cy+1); Ellipse(dc,cx,cy-2*r,cx+2*r+1,cy+1);
        p[0].x=cx-2*r; p[0].y=cy-r+1; p[1].x=cx+2*r; p[1].y=cy-r+1; p[2].x=cx; p[2].y=cy+2*r;
        Polygon(dc,p,3); break;
    case 3:
        p[0].x=cx; p[0].y=cy-2*r; p[1].x=cx+2*r; p[1].y=cy+r/2; p[2].x=cx-2*r; p[2].y=cy+r/2;
        Polygon(dc,p,3);
        Ellipse(dc,cx-2*r,cy-r/2,cx+1,cy+r+r/2+1); Ellipse(dc,cx,cy-r/2,cx+2*r+1,cy+r+r/2+1);
        p[0].x=cx; p[0].y=cy+r/2; p[1].x=cx+r; p[1].y=cy+2*r+1; p[2].x=cx-r; p[2].y=cy+2*r+1;
        Polygon(dc,p,3); break;
    default:
        Ellipse(dc,cx-r,cy-2*r,cx+r+1,cy+1); Ellipse(dc,cx-2*r,cy-r/2,cx+1,cy+r+r/2+1); Ellipse(dc,cx,cy-r/2,cx+2*r+1,cy+r+r/2+1);
        p[0].x=cx; p[0].y=cy; p[1].x=cx+r; p[1].y=cy+2*r+1; p[2].x=cx-r; p[2].y=cy+2*r+1;
        Polygon(dc,p,3); break;
    }
    SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(pen);
}
/* Pip positions of 2 to 10, in card coordinates. */
static const BYTE pips[9][10][2]={
    {{35,20},{35,76}},
    {{35,20},{35,48},{35,76}},
    {{20,20},{51,20},{20,76},{51,76}},
    {{20,20},{51,20},{20,76},{51,76},{35,48}},
    {{20,20},{51,20},{20,48},{51,48},{20,76},{51,76}},
    {{20,20},{51,20},{20,48},{51,48},{20,76},{51,76},{35,34}},
    {{20,20},{51,20},{20,48},{51,48},{20,76},{51,76},{35,34},{35,62}},
    {{20,20},{51,20},{20,39},{51,39},{20,57},{51,57},{20,76},{51,76},{35,48}},
    {{20,20},{51,20},{20,39},{51,39},{20,57},{51,57},{20,76},{51,76},{35,30},{35,66}},
};
static void draw_card(HDC dc,int c,int x,int y) {
    HGDIOBJ ob,op=SelectObject(dc,GetStockObject(BLACK_PEN));
    if(!(c&UP)) {
        HBRUSH hatch=CreateHatchBrush(HS_DIAGCROSS,red_back?RGB(255,128,128):RGB(0,128,255));
        ob=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        RoundRect(dc,x,y,x+CARD_W,y+CARD_H,8,8);
        SelectObject(dc,hatch); SetBkColor(dc,red_back?RGB(128,0,0):RGB(0,0,128)); SetBkMode(dc,OPAQUE);
        Rectangle(dc,x+5,y+5,x+CARD_W-5,y+CARD_H-5);
        SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(hatch);
        return;
    }
    {
        char name[8]; int r=rank(c),s=suit(c),i; SIZE sz; HGDIOBJ of;
        ob=SelectObject(dc,GetStockObject(WHITE_BRUSH));
        RoundRect(dc,x,y,x+CARD_W,y+CARD_H,8,8);
        card_name(c,name); name[lstrlen(name)-1]=0;
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,red(c)?RGB(255,0,0):RGB(0,0,0));
        TextOut(dc,x+4,y+3,name,lstrlen(name));
        GetTextExtentPoint(dc,name,lstrlen(name),&sz);
        TextOut(dc,x+CARD_W-4-(int)sz.cx,y+CARD_H-3-(int)sz.cy,name,lstrlen(name));
        suit_shape(dc,s,x+9,y+23,9);
        suit_shape(dc,s,x+CARD_W-10,y+CARD_H-27,9);
        if(r==0) suit_shape(dc,s,x+CARD_W/2,y+CARD_H/2,26);
        else if(r<10) for(i=0;i<r+1;i++) suit_shape(dc,s,x+pips[r-1][i][0],y+pips[r-1][i][1],11);
        else {
            HPEN frame=CreatePen(PS_SOLID,1,red(c)?RGB(255,0,0):RGB(0,0,0)); RECT t;
            SelectObject(dc,frame); SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,x+16,y+16,x+CARD_W-16,y+CARD_H-16);
            of=SelectObject(dc,big_font);
            SetRect(&t,x+16,y+20,x+CARD_W-16,y+40); DrawText(dc,name,-1,&t,DT_CENTER|DT_SINGLELINE);
            SelectObject(dc,of);
            suit_shape(dc,s,x+CARD_W/2,y+58,16);
            SelectObject(dc,GetStockObject(BLACK_PEN)); DeleteObject(frame);
        }
        SelectObject(dc,ob); SelectObject(dc,op);
    }
}
static void draw_empty(HDC dc,int x,int y,BOOL circle) {
    HPEN pen=CreatePen(PS_SOLID,2,RGB(0,64,0)); HGDIOBJ op=SelectObject(dc,pen),ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,x+1,y+1,x+CARD_W-1,y+CARD_H-1,8,8);
    if(circle) Ellipse(dc,x+16,y+28,x+CARD_W-16,y+CARD_H-28);
    SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen);
}
/* The table into the scene bitmap, without the cards being dragged. */
static void render(void) {
    RECT r; int p,i; HBRUSH table;
    if(!scene_dc) return;
    SetRect(&r,0,0,scene_w,scene_h);
    table=CreateSolidBrush(TABLE_COLOR); FillRect(scene_dc,&r,table); DeleteObject(table);
    for(p=0;p<PILES;p++) {
        int x,y,n=piles[p].n,first=0;
        if(dragging && moved && p==drag_pile) n=drag_index;
        if(!n) {card_at(p,0,&x,&y); draw_empty(scene_dc,x,y,p==DECK); continue;}
        if(p==DECK || (p>=FOUND && p<TABLEAU)) first=n-1;
        if(p==WASTE) first=max(0,n-(draw_three?3:1));
        for(i=first;i<n;i++) {card_at(p,i,&x,&y); draw_card(scene_dc,piles[p].cards[i],x,y);}
    }
}
static void refresh(void) {render(); InvalidateRect(main_wnd,NULL,FALSE); report();}
static void make_scene(void) {
    RECT r; HDC dc;
    GetClientRect(main_wnd,&r);
    if(scene_dc && r.right==scene_w && r.bottom==scene_h) return;
    if(scene_dc) {SelectObject(scene_dc,scene_old); DeleteObject(scene_bitmap); DeleteDC(scene_dc);}
    scene_w=max(1,(int)r.right); scene_h=max(1,(int)r.bottom);
    dc=GetDC(main_wnd);
    scene_dc=CreateCompatibleDC(dc); scene_bitmap=CreateCompatibleBitmap(dc,scene_w,scene_h);
    ReleaseDC(main_wnd,dc);
    scene_old=SelectObject(scene_dc,scene_bitmap);
    render();
}
/* The dragged cards' rectangle at drag_at. */
static void drag_rect(RECT *r) {
    int x0,y0,x1,y1;
    card_at(drag_pile,drag_index,&x0,&y0); card_at(drag_pile,piles[drag_pile].n-1,&x1,&y1);
    SetRect(r,drag_at.x,drag_at.y,drag_at.x+CARD_W+(x1-x0),drag_at.y+CARD_H+(y1-y0));
}
static void draw_dragged(HDC dc) {
    int i,x0,y0;
    card_at(drag_pile,drag_index,&x0,&y0);
    for(i=drag_index;i<piles[drag_pile].n;i++) {int x,y; card_at(drag_pile,i,&x,&y); draw_card(dc,piles[drag_pile].cards[i],drag_at.x+x-x0,drag_at.y+y-y0);}
}

/* --- the mouse ---------------------------------------------------------------------------- */
/* The card under (x,y): its pile and index, or FALSE. */
static BOOL hit(int x,int y,int *pile,int *index) {
    int p,i;
    for(p=0;p<PILES;p++) {
        int n=piles[p].n,first=0,cx,cy;
        if(p==DECK || p==WASTE || (p>=FOUND && p<TABLEAU)) first=max(0,n-1);
        if(!n) {card_at(p,0,&cx,&cy); if(x>=cx && x<cx+CARD_W && y>=cy && y<cy+CARD_H) {*pile=p; *index=-1; return TRUE;} continue;}
        for(i=n-1;i>=first;i--) {
            card_at(p,i,&cx,&cy);
            if(x>=cx && x<cx+CARD_W && y>=cy && y<cy+CARD_H) {*pile=p; *index=i; return TRUE;}
        }
    }
    return FALSE;
}
/* The pile under the dragged cards' top left corner area. */
static int drop_target(void) {
    int p,best=-1; long best_area=0; RECT d,c,o;
    drag_rect(&d); d.bottom=d.top+CARD_H;
    for(p=FOUND;p<PILES;p++) {
        int x,y;
        if(p==drag_pile) continue;
        card_at(p,max(0,piles[p].n-1),&x,&y);
        SetRect(&c,x,y,x+CARD_W,y+CARD_H);
        if(IntersectRect(&o,&c,&d)) {long a=(long)(o.right-o.left)*(o.bottom-o.top); if(a>best_area) {best_area=a; best=p;}}
    }
    return best;
}
static void to_foundation(int p) {
    int f,c=top(p);
    if(c<0 || !(c&UP) || p==DECK || (p>=FOUND && p<TABLEAU)) return;
    for(f=0;f<4;f++) if(fits_foundation(c,f)) {move_cards(p,piles[p].n-1,FOUND+f); refresh(); break;}
}
static void mouse_down(int x,int y,BOOL dbl) {
    int p,i;
    if(!hit(x,y,&p,&i)) return;
    if(dbl) {to_foundation(p); return;}
    if(p==DECK) {deal_from_deck(); refresh(); return;}
    if(i<0) return;
    if(!(piles[p].cards[i]&UP)) {
        if(i==piles[p].n-1) {remember(); piles[p].cards[i]|=UP; refresh();}
        return;
    }
    if(p==WASTE && i!=piles[p].n-1) return;
    {int cx,cy; card_at(p,i,&cx,&cy); drag_offset.x=x-cx; drag_offset.y=y-cy; drag_at.x=cx; drag_at.y=cy;}
    /* The drag starts when the mouse moves, so a click redraws nothing. */
    dragging=TRUE; moved=FALSE; drag_pile=p; drag_index=i; drag_start.x=x; drag_start.y=y;
    SetCapture(main_wnd);
}
static void mouse_move(int x,int y) {
    HDC dc; RECT old,now,u;
    if(!dragging) return;
    if(!moved) {
        if(x-drag_start.x<3 && drag_start.x-x<3 && y-drag_start.y<3 && drag_start.y-y<3) return;
        moved=TRUE; render();
    }
    drag_rect(&old);
    drag_at.x=x-drag_offset.x; drag_at.y=y-drag_offset.y;
    drag_rect(&now); UnionRect(&u,&old,&now);
    dc=GetDC(main_wnd);
    BitBlt(dc,u.left,u.top,u.right-u.left,u.bottom-u.top,scene_dc,u.left,u.top,SRCCOPY);
    draw_dragged(dc);
    ReleaseDC(main_wnd,dc);
}
static void mouse_up(void) {
    int to;
    if(!dragging) return;
    ReleaseCapture();
    to=moved?drop_target():-1;
    dragging=FALSE;
    if(!moved) return;
    if(to>=0 && legal(drag_pile,drag_index,to)) move_cards(drag_pile,drag_index,to);
    refresh();
    if(won()) {
        MessageBox(main_wnd,"Congratulations! You won.","Solitaire",MB_OK|MB_ICONINFORMATION);
        if(trace) OutputDebugString("SOL: won");
    }
}

/* --- dialogs and the window ---------------------------------------------------------------- */
static INT_PTR CALLBACK ChoiceProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    BOOL *value=(BOOL *)GetProp(h,"Value"); int first=GetDlgItem(h,IDC_DRAWONE)?IDC_DRAWONE:IDC_BLUE;
    if(msg==WM_INITDIALOG) {value=(BOOL *)lp; SetProp(h,"Value",(HANDLE)value); CheckRadioButton(h,first,first+1,*value?first+1:first); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {*value=IsDlgButtonChecked(h,first+1)!=0; EndDialog(h,IDOK); return TRUE;}
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(h,IDCANCEL); return TRUE;}
    if(msg==WM_DESTROY) RemoveProp(h,"Value");
    return FALSE;
}
LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_SIZE: if(wp!=SIZE_MINIMIZED) {make_scene(); InvalidateRect(h,NULL,FALSE);} return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        if(scene_dc) BitBlt(dc,0,0,scene_w,scene_h,scene_dc,0,0,SRCCOPY);
        if(dragging && moved) draw_dragged(dc);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: mouse_down(GET_X_LPARAM(lp),GET_Y_LPARAM(lp),FALSE); return 0;
    case WM_LBUTTONDBLCLK: mouse_down(GET_X_LPARAM(lp),GET_Y_LPARAM(lp),TRUE); return 0;
    case WM_MOUSEMOVE: mouse_move(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: mouse_up(); return 0;
    case WM_CANCELMODE: if(dragging) {ReleaseCapture(); dragging=FALSE; refresh();} return 0;
    case WM_INITMENUPOPUP: EnableMenuItem((HMENU)wp,IDM_UNDO,MF_BYCOMMAND|(can_undo?MF_ENABLED:MF_GRAYED)); return 0;
    case WM_COMMAND:
        if(HelpCommand(h,LOWORD(wp),"SOL.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_DEAL: if(fixed_seed) seed++; deal(); refresh(); return 0;
        case IDM_UNDO: if(can_undo) {memcpy(piles,undo_piles,sizeof(piles)); can_undo=FALSE; if(trace) OutputDebugString("SOL: undo"); refresh();} return 0;
        case IDM_OPTIONS:
            if(DialogBoxParam(instance,"OPTIONSDLG",h,ChoiceProc,(LPARAM)&draw_three)==IDOK) {WriteProfileString("Solitaire","DrawThree",draw_three?"1":"0"); refresh();}
            return 0;
        case IDM_DECK:
            if(DialogBoxParam(instance,"DECKDLG",h,ChoiceProc,(LPARAM)&red_back)==IDOK) {WriteProfileString("Solitaire","Back",red_back?"2":"1"); refresh();}
            return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        case IDM_ABOUT: MessageBox(h,"Solitaire\nInterface Manager 3.0 for IA-64","About Solitaire",MB_OK|MB_ICONINFORMATION); return 0;
        }
        return 0;
    case WM_DESTROY: WinHelp(h,"SOL.HLP",HELP_QUIT,0);
        if(scene_dc) {SelectObject(scene_dc,scene_old); DeleteObject(scene_bitmap); DeleteDC(scene_dc); scene_dc=NULL;}
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    WNDCLASS wc; MSG m; HACCEL accel; char args[128],*p=args,*word;
    instance=inst;
    lstrcpyn(args,command_line?command_line:"",sizeof(args));
    while(*p) {
        while(*p==' ') p++;
        word=p;
        while(*p && *p!=' ') p++;
        if(*p) *p++=0;
        if(!lstrcmpi(word,"/trace")) trace=TRUE;
        else if((word[0]=='/' || word[0]=='-') && (word[1]|0x20)=='s' && (word[2]|0x20)=='e' && (word[3]|0x20)=='e' && (word[4]|0x20)=='d' && word[5]==':') {
            const char *d=word+6; seed=0; fixed_seed=TRUE;
            while(*d>='0' && *d<='9') seed=seed*10+(DWORD)(*d++-'0');
        }
    }
    draw_three=GetProfileInt("Solitaire","DrawThree",0)!=0;
    red_back=GetProfileInt("Solitaire","Back",1)==2;
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"SOL"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.lpszMenuName="SOL"; wc.lpszClassName="Solitaire";
        RegisterClass(&wc);
    }
    big_font=CreateFont(15,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,0,0,0,FIXED_PITCH|FF_MODERN,"Courier");
    deal();
    main_wnd=CreateWindow("Solitaire","Solitaire",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,LEFT*2+7*COL_W+8,TAB_Y+6*DOWN_STEP+12*UP_STEP+CARD_H+60,NULL,NULL,inst,NULL);
    ShowWindow(main_wnd,show);
    make_scene();
    UpdateWindow(main_wnd);
    if(trace) {POINT o; char line[64]; o.x=o.y=0; ClientToScreen(main_wnd,&o); wsprintf(line,"SOL: client %d %d",o.x,o.y); OutputDebugString(line);}
    report();
    accel=LoadAccelerators(inst,"SOL");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    DeleteObject(big_font);
    return (int)m.wParam;
}
