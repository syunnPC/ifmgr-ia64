/* SPDX-License-Identifier: GPL-2.0-or-later
 * REVERSI: the Windows 3.0 game. You play black against the computer;
 * click a square (or move with the arrow keys and press Enter) to play.
 * The computer looks 1 to 4 moves ahead by skill, weighing corners and
 * edges and the moves each side has left.
 */
#include <windows.h>
#include "reversi.h"
#include "winapp.h"
#define EMPTY 0
#define BLACK 1   /* you */
#define WHITE 2   /* the computer */

static HWND wnd;
static BYTE board[64];
static int skill=1,sel=-1,ox,oy,sq=40;
static BOOL over,thinking;
static char status[96];
static HCURSOR cross,arrow,hourglass;
static const int dirs[8][2]={{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
static const int weight[64]={
    100,-20,10, 5, 5,10,-20,100,
    -20,-50,-2,-2,-2,-2,-50,-20,
     10, -2, 1, 1, 1, 1, -2, 10,
      5, -2, 1, 0, 0, 1, -2,  5,
      5, -2, 1, 0, 0, 1, -2,  5,
     10, -2, 1, 1, 1, 1, -2, 10,
    -20,-50,-2,-2,-2,-2,-50,-20,
    100,-20,10, 5, 5,10,-20,100};

/* --- the game ------------------------------------------------------------------- */
/* The pieces a move at pos would turn (and turns them when apply is set). */
static int flips(BYTE *b,int pos,int who,BOOL apply) {
    int d,n=0,other=3-who,r0=pos/8,c0=pos%8;
    if(b[pos]!=EMPTY) return 0;
    for(d=0;d<8;d++) {
        int r=r0+dirs[d][0],c=c0+dirs[d][1],run=0;
        while(r>=0 && r<8 && c>=0 && c<8 && b[r*8+c]==other) {r+=dirs[d][0]; c+=dirs[d][1]; run++;}
        if(!run || r<0 || r>=8 || c<0 || c>=8 || b[r*8+c]!=who) continue;
        n+=run;
        if(apply) {r=r0+dirs[d][0]; c=c0+dirs[d][1]; while(b[r*8+c]==other) {b[r*8+c]=(BYTE)who; r+=dirs[d][0]; c+=dirs[d][1];}}
    }
    if(apply && n) b[pos]=(BYTE)who;
    return n;
}
static int moves(BYTE *b,int who,int *list) {
    int pos,n=0;
    for(pos=0;pos<64;pos++) if(flips(b,pos,who,FALSE)) {if(list) list[n]=pos; n++;}
    return n;
}
static int count(const BYTE *b,int who) {int i,n=0; for(i=0;i<64;i++) n+=b[i]==who; return n;}
static int evaluate(BYTE *b,int who) {
    int other=3-who,mine=moves(b,who,NULL),theirs=moves(b,other,NULL),s=0,i;
    if(!mine && !theirs) return (count(b,who)-count(b,other))*1000;
    for(i=0;i<64;i++) s+=b[i]==who?weight[i]:b[i]==other?-weight[i]:0;
    return s+5*(mine-theirs);
}
static int search(BYTE *b,int who,int depth,int alpha,int beta) {
    int list[32],n,i,best=-100000;
    if(!depth) return evaluate(b,who);
    n=moves(b,who,list);
    if(!n) {
        if(!moves(b,3-who,NULL)) return evaluate(b,who);
        return -search(b,3-who,depth-1,-beta,-alpha);
    }
    for(i=0;i<n;i++) {
        BYTE next[64]; int v;
        memcpy(next,b,64); flips(next,list[i],who,TRUE);
        v=-search(next,3-who,depth-1,-beta,-alpha);
        if(v>best) best=v;
        if(v>alpha) alpha=v;
        if(alpha>=beta) break;
    }
    return best;
}
static int best_move(int who) {
    int list[32],n=moves(board,who,list),i,best=-1,best_value=-1000000;
    for(i=0;i<n;i++) {
        BYTE next[64]; int v;
        memcpy(next,board,64); flips(next,list[i],who,TRUE);
        v=-search(next,3-who,skill-1,-1000000,1000000);
        if(v>best_value) {best_value=v; best=list[i];}
    }
    return best;
}

/* --- drawing ------------------------------------------------------------------ */
static void square_rect(int pos,RECT *r) {SetRect(r,ox+(pos%8)*sq,oy+(pos/8)*sq,ox+(pos%8+1)*sq,oy+(pos/8+1)*sq);}
static void set_status(void) {
    int b=count(board,BLACK),w=count(board,WHITE);
    if(!over) wsprintf(status,"Black (you): %d    White: %d",b,w);
    else if(b>w) wsprintf(status,"You win %d to %d",b,w);
    else if(b<w) wsprintf(status,"You lose %d to %d",b,w);
    else wsprintf(status,"Tie game %d to %d",b,w);
}
static void paint(HDC dc) {
    RECT r; int i; HBRUSH green=CreateSolidBrush(RGB(0,128,0)),red=CreateSolidBrush(RGB(255,0,0));
    SetRect(&r,ox,oy,ox+8*sq,oy+8*sq); FillRect(dc,&r,green);
    for(i=0;i<=8;i++) {
        PatBlt(dc,ox+i*sq,oy,1,8*sq+1,BLACKNESS);
        PatBlt(dc,ox,oy+i*sq,8*sq+1,1,BLACKNESS);
    }
    SelectObject(dc,GetStockObject(BLACK_PEN));
    for(i=0;i<64;i++) if(board[i]) {
        int m=max(2,sq/8);
        square_rect(i,&r);
        SelectObject(dc,GetStockObject(board[i]==BLACK?BLACK_BRUSH:WHITE_BRUSH));
        Ellipse(dc,r.left+m,r.top+m,r.right-m+1,r.bottom-m+1);
    }
    if(sel>=0) {
        square_rect(sel,&r); r.right++; r.bottom++;
        FrameRect(dc,&r,red); InflateRect(&r,-1,-1); FrameRect(dc,&r,red);
    }
    SelectObject(dc,GetStockObject(WHITE_BRUSH));
    DeleteObject(green); DeleteObject(red);
    SetBkMode(dc,TRANSPARENT);
    {RECT c; GetClientRect(wnd,&c); SetRect(&r,0,oy+8*sq+4,c.right,c.bottom); DrawText(dc,status,-1,&r,DT_CENTER|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);}
}
static void redraw(void) {
    RECT r; GetClientRect(wnd,&r);
    InvalidateRect(wnd,&r,TRUE); UpdateWindow(wnd);
}
static int hit(int x,int y) {
    if(x<ox || y<oy || x>=ox+8*sq || y>=oy+8*sq) return -1;
    return (y-oy)/sq*8+(x-ox)/sq;
}

/* --- turns --------------------------------------------------------------------- */
static void new_game(void) {
    memset(board,0,sizeof(board));
    board[27]=board[36]=WHITE; board[28]=board[35]=BLACK;
    over=FALSE; sel=-1;
    set_status(); redraw();
}
/* The computer plays until it is your turn again or the game is over. */
static void computer_turn(void) {
    for(;;) {
        if(moves(board,WHITE,NULL)) {
            int pos; HCURSOR old;
            thinking=TRUE; old=SetCursor(hourglass);
            pos=best_move(WHITE);
            flips(board,pos,WHITE,TRUE);
            thinking=FALSE; SetCursor(old);
            set_status(); redraw();
        }
        if(moves(board,BLACK,NULL)) return;
        if(!moves(board,WHITE,NULL)) {over=TRUE; set_status(); redraw(); MessageBeep(0); return;}
        MessageBox(wnd,"You must pass.","Reversi",MB_OK|MB_ICONINFORMATION);
    }
}
static void play(int pos) {
    if(over || thinking || pos<0 || !flips(board,pos,BLACK,FALSE)) {MessageBeep(0); return;}
    flips(board,pos,BLACK,TRUE);
    sel=-1;
    set_status(); redraw();
    if(!moves(board,WHITE,NULL) && !moves(board,BLACK,NULL)) {over=TRUE; set_status(); redraw(); return;}
    if(!moves(board,WHITE,NULL)) {lstrcpy(status,"The computer must pass."); redraw(); return;}
    computer_turn();
}
static void layout(int w,int h) {
    TEXTMETRIC tm; HDC dc=GetDC(wnd); int status_h;
    GetTextMetrics(dc,&tm); ReleaseDC(wnd,dc);
    status_h=(int)tm.tmHeight+8;
    sq=min((w-16)/8,(h-16-status_h)/8);
    if(sq<8) sq=8;
    ox=(w-8*sq)/2; oy=8;
}
static void set_skill(HWND h,int level) {
    int i;
    skill=level;
    for(i=0;i<4;i++) CheckMenuItem(GetMenu(h),IDM_BEGINNER+i,i==level-1?MF_CHECKED:MF_UNCHECKED);
}

LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_SIZE: layout(LOWORD(lp),HIWORD(lp)); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint(dc); EndPaint(h,&ps); return 0;}
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h,&p);
            SetCursor(thinking?hourglass:!over && hit(p.x,p.y)>=0 && flips(board,hit(p.x,p.y),BLACK,FALSE)?cross:arrow);
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: play(hit(GET_X_LPARAM(lp),GET_Y_LPARAM(lp))); return 0;
    case WM_KEYDOWN: {
        int r,c;
        if(sel<0) sel=27;
        r=sel/8; c=sel%8;
        switch(wp) {
        case VK_LEFT: c=(c+7)%8; break;
        case VK_RIGHT: c=(c+1)%8; break;
        case VK_UP: r=(r+7)%8; break;
        case VK_DOWN: r=(r+1)%8; break;
        case VK_RETURN: case VK_SPACE: play(sel); return 0;
        default: return 0;
        }
        sel=r*8+c; redraw();
        return 0;
    }
    case WM_COMMAND:
        if(HelpCommand(h,LOWORD(wp),"REVERSI.HLP")) return 0;
        switch(LOWORD(wp)) {
        case IDM_HINT:
            if(over || !moves(board,BLACK,NULL)) {MessageBeep(0); return 0;}
            sel=best_move(BLACK); redraw();
            {RECT r; POINT p; square_rect(sel,&r); p.x=(r.left+r.right)/2; p.y=(r.top+r.bottom)/2; ClientToScreen(h,&p); SetCursorPos(p.x,p.y);}
            return 0;
        case IDM_PASS:
            if(over) {MessageBeep(0); return 0;}
            if(moves(board,BLACK,NULL)) {MessageBox(h,"You may only pass when you have no legal moves.","Reversi",MB_OK|MB_ICONEXCLAMATION); return 0;}
            computer_turn();
            return 0;
        case IDM_NEW: new_game(); return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        case IDM_BEGINNER: case IDM_NOVICE: case IDM_EXPERT: case IDM_MASTER: set_skill(h,LOWORD(wp)-IDM_BEGINNER+1); return 0;
        case IDM_ABOUT: MessageBox(h,"Reversi\nPlay black against the computer: take the most squares.","About Reversi",MB_OK|MB_ICONINFORMATION); return 0;
        }
        return 0;
    case WM_DESTROY: WinHelp(h,"REVERSI.HLP",HELP_QUIT,0); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; MSG m; HACCEL accel;
    (void)command;
    cross=LoadCursor(NULL,IDC_CROSS); arrow=LoadCursor(NULL,IDC_ARROW); hourglass=LoadCursor(NULL,IDC_WAIT);
    if(!previous) {
        memset(&wc,0,sizeof(wc));
        wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"REVERSI");
        wc.hCursor=NULL; wc.hbrBackground=(HBRUSH)GetStockObject(LTGRAY_BRUSH); wc.lpszMenuName="REVERSI"; wc.lpszClassName="Reversi";
        RegisterClass(&wc);
    }
    wnd=CreateWindow("Reversi","Reversi",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    new_game();
    ShowWindow(wnd,show); UpdateWindow(wnd);
    accel=LoadAccelerators(inst,"REVERSI");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
