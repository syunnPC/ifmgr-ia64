/* SPDX-License-Identifier: GPL-2.0-or-later
 * Paintbrush edits a scrollable memory bitmap. Dragging previews shapes
 * until button release. Read .BMP files and save 24-bit .BMP output.
 * /trace reports layout for QEMU tests; controls are documented in PBRUSH.RTF.
 */
#include <windows.h>
#include "winapp.h"
#include "pbrush.h"
#define TOOLW 58
#define CELL 26
#define PALH 40
#define TOOLS 9
#define WIDTHS 5
#define MAXSIZE 2048
enum {T_PENCIL,T_BRUSH,T_LINE,T_RECT,T_FRECT,T_ELLIPSE,T_FELLIPSE,T_ERASER,T_ROLLER};

static const COLORREF palette[16]={
    RGB(0,0,0),RGB(128,0,0),RGB(0,128,0),RGB(128,128,0),RGB(0,0,128),RGB(128,0,128),RGB(0,128,128),RGB(192,192,192),
    RGB(128,128,128),RGB(255,0,0),RGB(0,255,0),RGB(255,255,0),RGB(0,0,255),RGB(255,0,255),RGB(0,255,255),RGB(255,255,255)};
static const int widths[WIDTHS]={1,2,3,5,8};
static HINSTANCE instance;
static HWND main_wnd,view;
static HDC canvas,undo;
static HBITMAP canvas_bm,undo_bm;
static HGDIOBJ canvas_old,undo_old;
static int pic_w,pic_h,xoff,yoff;
static int tool=T_PENCIL,width_index=0;
static COLORREF fg=RGB(0,0,0),bg=RGB(255,255,255);
static BOOL dragging,modified,trace;
static POINT start,last;
static COLORREF drag_color;
static char file[260];

/* --- the picture ------------------------------------------------------------------ */
static void free_picture(void) {
    if(canvas) {SelectObject(canvas,canvas_old); DeleteDC(canvas); DeleteObject(canvas_bm); canvas=NULL;}
    if(undo) {SelectObject(undo,undo_old); DeleteDC(undo); DeleteObject(undo_bm); undo=NULL;}
}
static BOOL new_picture(int w,int h) {
    HDC screen=GetDC(NULL);
    free_picture();
    canvas_bm=CreateCompatibleBitmap(screen,w,h); undo_bm=CreateCompatibleBitmap(screen,w,h);
    canvas=CreateCompatibleDC(screen); undo=CreateCompatibleDC(screen);
    ReleaseDC(NULL,screen);
    if(!canvas_bm || !undo_bm || !canvas || !undo) {free_picture(); return FALSE;}
    canvas_old=SelectObject(canvas,canvas_bm); undo_old=SelectObject(undo,undo_bm);
    PatBlt(canvas,0,0,w,h,WHITENESS); PatBlt(undo,0,0,w,h,WHITENESS);
    pic_w=w; pic_h=h; xoff=yoff=0;
    return TRUE;
}
static void save_undo(void) {BitBlt(undo,0,0,pic_w,pic_h,canvas,0,0,SRCCOPY);}
static void set_title(void) {
    char title[300];
    wsprintf(title,"Paintbrush - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(main_wnd,title);
}
static void scroll_bars(void) {
    RECT r; GetClientRect(view,&r);
    xoff=max(0,min(xoff,pic_w-r.right)); yoff=max(0,min(yoff,pic_h-r.bottom));
    SetScrollRange(view,SB_HORZ,0,max(0,pic_w-r.right),FALSE); SetScrollPos(view,SB_HORZ,xoff,TRUE);
    SetScrollRange(view,SB_VERT,0,max(0,pic_h-r.bottom),FALSE); SetScrollPos(view,SB_VERT,yoff,TRUE);
}
static void refresh(void) {scroll_bars(); InvalidateRect(view,NULL,FALSE);}
static void message(LPCSTR format,LPCSTR name) {
    char text[400]; wsprintf(text,format,name); MessageBox(main_wnd,text,"Paintbrush",MB_OK|MB_ICONEXCLAMATION);
}
/* A 24-bit .BMP of the picture. */
static BOOL save_to(LPCSTR path) {
    struct {BITMAPINFOHEADER h; RGBQUAD colors[256];} info; BITMAPFILEHEADER fh;
    DWORD stride=(DWORD)((pic_w*24+31)/32)*4,size=stride*(DWORD)pic_h; BYTE *data; BOOL ok;
    if(!(data=(BYTE *)GlobalAlloc(GPTR,sizeof(fh)+sizeof(BITMAPINFOHEADER)+size))) {message("Not enough memory to save %s.",path); return FALSE;}
    memset(&info,0,sizeof(info)); info.h.biSize=sizeof(BITMAPINFOHEADER); info.h.biBitCount=24;
    SelectObject(canvas,canvas_old);
    GetDIBits(canvas,canvas_bm,0,(UINT)pic_h,data+sizeof(fh)+sizeof(BITMAPINFOHEADER),(BITMAPINFO *)&info,DIB_RGB_COLORS);
    SelectObject(canvas,canvas_bm);
    info.h.biSizeImage=size;
    memset(&fh,0,sizeof(fh)); fh.bfType=0x4d42; fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER); fh.bfSize=fh.bfOffBits+size;
    memcpy(data,&fh,sizeof(fh)); memcpy(data+sizeof(fh),&info.h,sizeof(BITMAPINFOHEADER));
    ok=WriteWholeFile(path,data,fh.bfSize);
    GlobalFree(data);
    if(!ok) {message("Cannot write to the %s file.",path); return FALSE;}
    modified=FALSE;
    return TRUE;
}
static BOOL load(LPCSTR path) {
    DWORD size; BYTE *data=(BYTE *)ReadWholeFile(path,&size); const BITMAPFILEHEADER *fh; const BITMAPINFOHEADER *h; int w,ht;
    if(!data) {message("Cannot open the %s file.",path); return FALSE;}
    fh=(const BITMAPFILEHEADER *)data; h=(const BITMAPINFOHEADER *)(data+sizeof(BITMAPFILEHEADER));
    if(size<sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER) || fh->bfType!=0x4d42 || fh->bfOffBits>=size || h->biSize<sizeof(BITMAPINFOHEADER) ||
       (w=(int)h->biWidth)<=0 || (ht=h->biHeight<0?-(int)h->biHeight:(int)h->biHeight)<=0 || w>MAXSIZE || ht>MAXSIZE) {
        GlobalFree(data); message("%s is not a bitmap file Paintbrush can read.",path); return FALSE;
    }
    if(!new_picture(w,ht)) {GlobalFree(data); message("Not enough memory to open %s.",path); return FALSE;}
    SelectObject(canvas,canvas_old);
    SetDIBits(canvas,canvas_bm,0,(UINT)ht,data+fh->bfOffBits,(const BITMAPINFO *)h,DIB_RGB_COLORS);
    SelectObject(canvas,canvas_bm);
    save_undo();
    GlobalFree(data);
    lstrcpy(file,path); AnsiUpper(file); set_title();
    modified=FALSE;
    refresh();
    return TRUE;
}
static BOOL save_as(void) {
    char path[260];
    lstrcpy(path,file[0]?file:"*.BMP");
    if(!FileSaveDialog(main_wnd,"Save As","*.BMP",path,sizeof(path)) || !save_to(path)) return FALSE;
    lstrcpy(file,path); set_title();
    return TRUE;
}
static BOOL save(void) {return file[0]?save_to(file):save_as();}
static BOOL query_save(void) {
    char text[400];
    if(!modified) return TRUE;
    wsprintf(text,"Save current changes to %s?",file[0]?FileTitle(file):"(Untitled)");
    switch(MessageBox(main_wnd,text,"Paintbrush",MB_YESNOCANCEL|MB_ICONEXCLAMATION)) {
    case IDYES: return save();
    case IDNO: return TRUE;
    }
    return FALSE;
}

/* --- drawing ---------------------------------------------------------------------- */
static int pen_width(void) {return tool==T_PENCIL?1:tool==T_ERASER?max(8,widths[width_index]*2):widths[width_index];}
static void stroke(HDC dc,int dx,int dy,POINT a,POINT b,COLORREF c,int w) {
    HPEN pen=CreatePen(PS_SOLID,w,c); HGDIOBJ old=SelectObject(dc,pen);
    MoveTo(dc,a.x-dx,a.y-dy); LineTo(dc,b.x-dx,b.y-dy);
    SetPixel(dc,b.x-dx,b.y-dy,c);
    if(w>1) {RECT r; HBRUSH br=CreateSolidBrush(c); SetRect(&r,b.x-dx-w/2,b.y-dy-w/2,b.x-dx-w/2+w,b.y-dy-w/2+w); FillRect(dc,&r,br); DeleteObject(br);}
    SelectObject(dc,old); DeleteObject(pen);
}
static void shape(HDC dc,int dx,int dy,POINT a,POINT b,COLORREF c) {
    HPEN pen=CreatePen(PS_SOLID,widths[width_index],c); HBRUSH brush=NULL; HGDIOBJ op,ob;
    int l=min(a.x,b.x)-dx,t=min(a.y,b.y)-dy,r=max(a.x,b.x)-dx+1,bt=max(a.y,b.y)-dy+1;
    if(tool==T_LINE) {DeleteObject(pen); stroke(dc,dx,dy,a,b,c,widths[width_index]); return;}
    if(tool==T_FRECT || tool==T_FELLIPSE) brush=CreateSolidBrush(c);
    op=SelectObject(dc,pen); ob=SelectObject(dc,brush?brush:GetStockObject(NULL_BRUSH));
    if(tool==T_RECT || tool==T_FRECT) Rectangle(dc,l,t,r,bt); else Ellipse(dc,l,t,r,bt);
    SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen);
    if(brush) DeleteObject(brush);
}
/* Show the picture's area a..b (picture coordinates, inclusive) in the view. */
static void show(POINT a,POINT b,int margin) {
    HDC dc=GetDC(view); int l=min(a.x,b.x)-margin,t=min(a.y,b.y)-margin,r=max(a.x,b.x)+margin+1,bt=max(a.y,b.y)+margin+1;
    BitBlt(dc,l-xoff,t-yoff,r-l,bt-t,canvas,l,t,SRCCOPY);
    ReleaseDC(view,dc);
}
static DWORD dib_color(COLORREF c) {return (DWORD)GetRValue(c)<<16|(DWORD)GetGValue(c)<<8|GetBValue(c);}
/* Paint roller: fill the area of the clicked color, scan line by scan line. */
static void flood(POINT p,COLORREF c) {
    struct {BITMAPINFOHEADER h; RGBQUAD colors[4];} info; DWORD *px,target,fill=dib_color(c); int *stack,sp=0,cap=pic_w*pic_h/4+64;
    if(p.x<0 || p.y<0 || p.x>=pic_w || p.y>=pic_h) return;
    px=(DWORD *)GlobalAlloc(GPTR,(DWORD)pic_w*(DWORD)pic_h*4); stack=(int *)GlobalAlloc(GPTR,(DWORD)cap*2*sizeof(int));
    if(!px || !stack) {if(px) GlobalFree(px); if(stack) GlobalFree(stack); return;}
    memset(&info,0,sizeof(info)); info.h.biSize=sizeof(BITMAPINFOHEADER); info.h.biBitCount=32;
    SelectObject(canvas,canvas_old);
    GetDIBits(canvas,canvas_bm,0,(UINT)pic_h,px,(BITMAPINFO *)&info,DIB_RGB_COLORS);
#define AT(x,y) px[(pic_h-1-(y))*pic_w+(x)]
    target=AT(p.x,p.y)&0xffffff;
    if(target!=fill) {
        stack[sp++]=p.x; stack[sp++]=p.y;
        while(sp) {
            int y=stack[--sp],x=stack[--sp],l=x,r=x,i;
            if((AT(x,y)&0xffffff)!=target) continue;
            while(l>0 && (AT(l-1,y)&0xffffff)==target) l--;
            while(r<pic_w-1 && (AT(r+1,y)&0xffffff)==target) r++;
            for(i=l;i<=r;i++) AT(i,y)=fill;
            for(i=l;i<=r;i++) {
                if(y>0 && (AT(i,y-1)&0xffffff)==target && (i==l || (AT(i-1,y-1)&0xffffff)!=target) && sp<cap*2-2) {stack[sp++]=i; stack[sp++]=y-1;}
                if(y<pic_h-1 && (AT(i,y+1)&0xffffff)==target && (i==l || (AT(i-1,y+1)&0xffffff)!=target) && sp<cap*2-2) {stack[sp++]=i; stack[sp++]=y+1;}
            }
        }
    }
#undef AT
    info.h.biHeight=pic_h; info.h.biWidth=pic_w; info.h.biPlanes=1; info.h.biBitCount=32; info.h.biCompression=BI_RGB;
    SetDIBits(canvas,canvas_bm,0,(UINT)pic_h,px,(BITMAPINFO *)&info,DIB_RGB_COLORS);
    SelectObject(canvas,canvas_bm);
    GlobalFree(px); GlobalFree(stack);
}

/* --- the view ----------------------------------------------------------------------- */
static POINT picture_point(LPARAM lp) {POINT p; p.x=GET_X_LPARAM(lp)+xoff; p.y=GET_Y_LPARAM(lp)+yoff; return p;}
static void begin(HWND h,LPARAM lp,BOOL right) {
    POINT p=picture_point(lp);
    save_undo();
    drag_color=tool==T_ERASER?bg:right?bg:fg;
    start=last=p;
    if(tool==T_ROLLER) {flood(p,drag_color); modified=TRUE; InvalidateRect(view,NULL,FALSE); return;}
    dragging=TRUE; SetCapture(h);
    if(tool==T_PENCIL || tool==T_BRUSH || tool==T_ERASER) {stroke(canvas,0,0,p,p,drag_color,pen_width()); show(p,p,pen_width()); modified=TRUE;}
}
static void follow(LPARAM lp) {
    POINT p=picture_point(lp);
    if(!dragging || (p.x==last.x && p.y==last.y)) return;
    if(tool==T_PENCIL || tool==T_BRUSH || tool==T_ERASER) {
        stroke(canvas,0,0,last,p,drag_color,pen_width()); show(last,p,pen_width());
    } else {
        /* Put back what the last outline covered, then draw the new one on the view. */
        HDC dc=GetDC(view); RECT r; GetClientRect(view,&r);
        BitBlt(dc,0,0,r.right,r.bottom,canvas,xoff,yoff,SRCCOPY);
        shape(dc,xoff,yoff,start,p,drag_color);
        ReleaseDC(view,dc);
    }
    last=p;
}
static void end(LPARAM lp) {
    POINT p=picture_point(lp);
    if(!dragging) return;
    dragging=FALSE; ReleaseCapture();
    if(tool!=T_PENCIL && tool!=T_BRUSH && tool!=T_ERASER) shape(canvas,0,0,start,p,drag_color);
    else stroke(canvas,0,0,last,p,drag_color,pen_width());
    modified=TRUE;
    InvalidateRect(view,NULL,FALSE);
}
LRESULT CALLBACK ViewProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT r; GetClientRect(h,&r);
        BitBlt(dc,0,0,min((int)r.right,pic_w-xoff),min((int)r.bottom,pic_h-yoff),canvas,xoff,yoff,SRCCOPY);
        if(r.right>pic_w-xoff) {RECT g=r; g.left=pic_w-xoff; FillRect(dc,&g,(HBRUSH)GetStockObject(GRAY_BRUSH));}
        if(r.bottom>pic_h-yoff) {RECT g=r; g.top=pic_h-yoff; g.right=min((int)r.right,pic_w-xoff); FillRect(dc,&g,(HBRUSH)GetStockObject(GRAY_BRUSH));}
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: scroll_bars(); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: if(!dragging) begin(h,lp,msg==WM_RBUTTONDOWN); return 0;
    case WM_MOUSEMOVE: follow(lp); return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: end(lp); return 0;
    case WM_HSCROLL: case WM_VSCROLL: {
        BOOL horz=msg==WM_HSCROLL; int *pos=horz?&xoff:&yoff,lo,hi,v; RECT r; GetClientRect(h,&r);
        GetScrollRange(h,horz?SB_HORZ:SB_VERT,&lo,&hi); v=*pos;
        switch(LOWORD(wp)) {
        case SB_LINEUP: v-=16; break;
        case SB_LINEDOWN: v+=16; break;
        case SB_PAGEUP: v-=horz?r.right:r.bottom; break;
        case SB_PAGEDOWN: v+=horz?r.right:r.bottom; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: v=(short)HIWORD(wp); break;
        case SB_TOP: v=lo; break;
        case SB_BOTTOM: v=hi; break;
        default: return 0;
        }
        v=max(lo,min(v,hi));
        if(v!=*pos) {*pos=v; SetScrollPos(h,horz?SB_HORZ:SB_VERT,v,TRUE); InvalidateRect(h,NULL,FALSE);}
        return 0;
    }
    }
    return DefWindowProc(h,msg,wp,lp);
}

/* --- toolbox and palette --------------------------------------------------------------- */
static void tool_rect(int i,RECT *r) {SetRect(r,4+(i%2)*CELL,4+(i/2)*CELL,4+(i%2)*CELL+CELL-2,4+(i/2)*CELL+CELL-2);}
static void width_rect(int i,RECT *r) {int y=8+((TOOLS+1)/2)*CELL+i*14; SetRect(r,4,y,TOOLW-6,y+12);}
static void swatch_rect(HWND h,int i,RECT *r) {
    RECT c; GetClientRect(h,&c);
    SetRect(r,48+(i%8)*24,(int)c.bottom-PALH+4+(i/8)*18,48+(i%8)*24+22,(int)c.bottom-PALH+4+(i/8)*18+16);
}
static void glyph(HDC dc,int i,const RECT *r) {
    int l=r->left+5,t=r->top+5,rt=r->right-5,b=r->bottom-5; HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
    switch(i) {
    case T_PENCIL: MoveTo(dc,l,b); LineTo(dc,rt,t); break;
    case T_BRUSH: {HPEN p=CreatePen(PS_SOLID,4,RGB(0,0,0)); HGDIOBJ op=SelectObject(dc,p); MoveTo(dc,l+2,b-2); LineTo(dc,rt-2,t+2); SelectObject(dc,op); DeleteObject(p); break;}
    case T_LINE: MoveTo(dc,l,(t+b)/2); LineTo(dc,rt+1,(t+b)/2); break;
    case T_RECT: Rectangle(dc,l,t+2,rt+1,b-1); break;
    case T_FRECT: SelectObject(dc,GetStockObject(BLACK_BRUSH)); Rectangle(dc,l,t+2,rt+1,b-1); break;
    case T_ELLIPSE: Ellipse(dc,l,t+2,rt+1,b-1); break;
    case T_FELLIPSE: SelectObject(dc,GetStockObject(BLACK_BRUSH)); Ellipse(dc,l,t+2,rt+1,b-1); break;
    case T_ERASER: SelectObject(dc,GetStockObject(WHITE_BRUSH)); Rectangle(dc,l+2,t+3,rt-1,b-2); break;
    case T_ROLLER: {
        RECT f; SetRect(&f,l,t,rt,t+6); FillRect(dc,&f,(HBRUSH)GetStockObject(BLACK_BRUSH));
        MoveTo(dc,(l+rt)/2,t+6); LineTo(dc,(l+rt)/2,b+1); break;
    }
    }
    SelectObject(dc,ob);
}
static void paint_tools(HWND h,HDC dc) {
    int i; RECT r,c;
    GetClientRect(h,&c);
    for(i=0;i<TOOLS;i++) {
        tool_rect(i,&r);
        FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        glyph(dc,i,&r);
        if(i==tool) {InflateRect(&r,-1,-1); InvertRect(dc,&r);}
    }
    for(i=0;i<WIDTHS;i++) {
        RECT l; width_rect(i,&r);
        FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
        SetRect(&l,r.left+4,(r.top+r.bottom-widths[i])/2,r.right-4,(r.top+r.bottom-widths[i])/2+widths[i]);
        FillRect(dc,&l,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(i==width_index) FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    /* The palette: the current colors, then the 16 colors. */
    {
        RECT p; HBRUSH b;
        SetRect(&p,0,c.bottom-PALH,c.right,c.bottom); FillRect(dc,&p,(HBRUSH)(COLOR_BTNFACE+1));
        SetRect(&r,16,c.bottom-PALH+12,40,c.bottom-PALH+36); b=CreateSolidBrush(bg); FillRect(dc,&r,b); DeleteObject(b); FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        SetRect(&r,6,c.bottom-PALH+4,30,c.bottom-PALH+28); b=CreateSolidBrush(fg); FillRect(dc,&r,b); DeleteObject(b); FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        for(i=0;i<16;i++) {
            swatch_rect(h,i,&r); b=CreateSolidBrush(palette[i]); FillRect(dc,&r,b); DeleteObject(b);
            FrameRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        }
    }
}
static void click_tools(HWND h,int x,int y,BOOL right) {
    int i; RECT r; POINT p; p.x=x; p.y=y;
    for(i=0;i<TOOLS;i++) {tool_rect(i,&r); if(PtInRect(&r,p)) {tool=i; InvalidateRect(h,NULL,TRUE); return;}}
    for(i=0;i<WIDTHS;i++) {width_rect(i,&r); if(PtInRect(&r,p)) {width_index=i; InvalidateRect(h,NULL,TRUE); return;}}
    for(i=0;i<16;i++) {
        swatch_rect(h,i,&r);
        if(PtInRect(&r,p)) {
            char text[48];
            if(right) bg=palette[i]; else fg=palette[i];
            if(trace) {wsprintf(text,"PBRUSH: %s %d",right?"background":"foreground",i); OutputDebugString(text);}
            InvalidateRect(h,NULL,TRUE);
            return;
        }
    }
}
static void layout(HWND h) {
    RECT c; GetClientRect(h,&c);
    MoveWindow(view,TOOLW,0,max(0,(int)c.right-TOOLW),max(0,(int)c.bottom-PALH),TRUE);
}
static void command(HWND h,UINT id) {
    char path[260];
    if(HelpCommand(h,id,"PBRUSH.HLP")) return;
    switch(id) {
    case IDM_NEW:
        if(!query_save()) return;
        new_picture(640,480); file[0]=0; modified=FALSE; set_title(); refresh();
        return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.BMP");
        if(FileOpenDialog(h,"Open","*.BMP",path,sizeof(path))) load(path);
        return;
    case IDM_SAVE: save(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(h,WM_CLOSE,0,0); return;
    case IDM_UNDO: BitBlt(canvas,0,0,pic_w,pic_h,undo,0,0,SRCCOPY); InvalidateRect(view,NULL,FALSE); return;
    case IDM_CLEAR: {
        HBRUSH b=CreateSolidBrush(bg); RECT r; SetRect(&r,0,0,pic_w,pic_h);
        save_undo(); FillRect(canvas,&r,b); DeleteObject(b); modified=TRUE; InvalidateRect(view,NULL,FALSE);
        return;
    }
    case IDM_ABOUT: MessageBox(h,"Paintbrush\nDraws pictures and keeps them in .BMP files.","About Paintbrush",MB_OK|MB_ICONINFORMATION); return;
    }
}
static void report_layout(HWND h) {
    char text[80]; POINT c,v; RECT r; c.x=c.y=v.x=v.y=0;
    ClientToScreen(h,&c); ClientToScreen(view,&v); GetClientRect(h,&r);
    wsprintf(text,"PBRUSH: client %d %d %d %d",c.x,c.y,r.right,r.bottom); OutputDebugString(text);
    wsprintf(text,"PBRUSH: view %d %d",v.x,v.y); OutputDebugString(text);
}
LRESULT CALLBACK MainProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        view=CreateWindow("PBView",NULL,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_HSCROLL|WS_VSCROLL,TOOLW,0,10,10,h,(HMENU)1,instance,NULL);
        return 0;
    case WM_SIZE: layout(h); InvalidateRect(h,NULL,TRUE); return 0;
    case WM_PAINT: {PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); paint_tools(h,dc); EndPaint(h,&ps); return 0;}
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: click_tools(h,GET_X_LPARAM(lp),GET_Y_LPARAM(lp),msg==WM_RBUTTONDOWN); return 0;
    case WM_COMMAND: command(h,LOWORD(wp)); return 0;
    case WM_CLOSE: if(query_save()) DestroyWindow(h); return 0;
    case WM_QUERYENDSESSION: return query_save();
    case WM_DESTROY: WinHelp(h,"PBRUSH.HLP",HELP_QUIT,0); free_picture(); PostQuitMessage(0); return 0;
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
        wc.lpfnWndProc=MainProc; wc.hInstance=inst; wc.hIcon=LoadIcon(inst,"PBRUSH"); wc.hCursor=LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszMenuName="PBRUSH"; wc.lpszClassName="Paintbrush";
        RegisterClass(&wc);
        wc.lpfnWndProc=ViewProc; wc.hIcon=NULL; wc.hCursor=LoadCursor(NULL,IDC_CROSS); wc.hbrBackground=NULL; wc.lpszMenuName=NULL; wc.lpszClassName="PBView";
        RegisterClass(&wc);
    }
    if(!new_picture(640,480)) {MessageBox(NULL,"Not enough memory for a picture.","Paintbrush",MB_OK|MB_ICONSTOP); return 1;}
    main_wnd=CreateWindow("Paintbrush","Paintbrush",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,CW_USEDEFAULT,0,NULL,NULL,inst,NULL);
    set_title();
    ShowWindow(main_wnd,show); UpdateWindow(main_wnd);
    if(arg[0]) load(arg);
    if(trace) report_layout(main_wnd);
    accel=LoadAccelerators(inst,"PBRUSH");
    while(GetMessage(&m,NULL,0,0)) if(!TranslateAccelerator(main_wnd,accel,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
