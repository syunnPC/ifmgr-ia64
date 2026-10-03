/* SPDX-License-Identifier: GPL-2.0-or-later
 * USER.DLL internals. C89.
 */
#ifndef USER_INTERNAL_H
#define USER_INTERNAL_H
#define USER_BUILD
#include <windows.h>
#include <winstart.h>
#include <winhost.h>
#include <string.h>
#include "gdi.h"

#define CLASSES 128
#define WINDOWS 1024
#define MENUS 512
#define ACCELS 64
#define ICONS 512
#define HOOKS 64
/* Handles are the 16-bit values Win16 programs see, so WOW passes them as
 * they are and a Win16 source may keep one in a WORD: a slot's index times
 * 4 above its kind's base. KERNEL's moveable memory starts at 0x4004, GDI's
 * objects at 0x8004. */
#define HWND_BASE 0x0004
#define HMENU_BASE 0x1004
#define HACCEL_BASE 0x1804
#define HICON_BASE 0x2004
#define HHOOK_BASE 0x2804
typedef char handle_ranges_apart[HWND_BASE+(WINDOWS-1)*4<HMENU_BASE && HMENU_BASE+(MENUS-1)*4<HACCEL_BASE &&
                                 HACCEL_BASE+(ACCELS-1)*4<HICON_BASE && HICON_BASE+(ICONS-1)*4<HHOOK_BASE &&
                                 HHOOK_BASE+(HOOKS-1)*4<0x4004?1:-1];
#define QUEUES 32
#define QUEUE_SIZE 256
#define TIMERS 128
#define INPUT_SIZE 128
/* Metrics of the Windows 3.0 look at VGA resolution. */
#define FRAME 4       /* thick frame, both border lines included */
#define CAPTION 20    /* caption bar and the line under it */
#define BOXSIZE 18    /* system menu, minimize and maximize boxes */
#define MENUBAR 19    /* menu bar and the line under it */
#define SCROLL 17     /* scroll bar width and arrow size */
#define ICON 32
#define ICON_SPACING 88 /* titles wrap to two lines within it */
#define ICON_ROW 72
/* Internal messages, handled by GetMessage and never seen by programs. */
#define UM_FIRST 0x0390
#define UM_ACTIVATE 0x0390   /* activate wParam's window in the owner's task */
#define UM_LAST 0x039f

typedef struct Class {
    BOOL used,system; char name[64]; ATOM atom; WNDCLASS wc; BYTE *extra; char menu_name[64];
} Class;
typedef struct {MSG msg; WORD ch; BYTE state;} QMsg; /* state: the shift keys and buttons with input (msg.c) */
/* A message sent to another task: the sender waits for done. */
typedef struct SendRec {
    struct SendRec *next; HWND hwnd; UINT msg; WPARAM wp; LPARAM lp;
    LRESULT result; BOOL done; struct Queue *from;
} SendRec;
typedef struct Queue {
    BOOL used; wh_u32 task; HTASK htask; HINSTANCE instance;
    QMsg ring[QUEUE_SIZE]; int head,count;
    SendRec *sent,*receiving;
    BOOL quit; int quit_code;
    DWORD key_time; WPARAM key_vk; WORD key_ch; BOOL key_sys;
    MSG last;             /* the last message retrieved (GetMessagePos/Time) */
    WORD hooking;         /* the hook kinds whose chains this task is running */
} Queue;
typedef struct Prop {struct Prop *next; char name[32]; ATOM atom; HANDLE data;} Prop;
typedef struct {int min,max,pos; BOOL hidden;} ScrollInfo;
typedef struct Wnd {
    BOOL used; HWND handle; Class *cls; WNDPROC proc;
    DWORD style,exstyle; struct Wnd *parent,*owner,*next,*child;
    RECT window,client;             /* screen coordinates */
    RECT normal;                    /* restored rectangle, parent client coordinates */
    POINT icon; BOOL icon_placed;   /* minimized position, parent client coordinates */
    char *text;
    HINSTANCE instance; HMENU menu; UINT_PTR id; LONG_PTR user;
    Queue *queue;
    RECT update;                    /* client coordinates; empty when valid */
    BOOL erase,ncpaint,redraw_off,validated; /* validated: by BeginPaint or ValidateRect, since WM_PAINT was sent */
    BYTE *extra; int extra_bytes;
    ScrollInfo scroll[2];           /* SB_HORZ and SB_VERT of WS_HSCROLL/WS_VSCROLL */
    HDC own_dc; Prop *props; HMENU system_menu;
    struct Wnd *last_active;        /* the last active popup it owns */
    HFONT font;                     /* WM_SETFONT for USER's controls */
    BOOL need_size;                 /* an overlapped window's first WM_SIZE and WM_MOVE wait for ShowWindow */
} Wnd;

extern Wnd *desktop,*active,*focus,*capture;
extern int screen_width,screen_height;
extern HINSTANCE user_instance;
extern int cursor_x,cursor_y;
/* wnd.c */
Wnd *WndFromHandle(HWND);
Wnd *WndFromPoint(POINT);
Wnd *TopLevel(Wnd *);
BOOL Shown(Wnd *);
BOOL Enabled(Wnd *);
void InvalidateWnd(Wnd *,const RECT *client,BOOL erase,BOOL frame);
Wnd *PaintWindow(Queue *,HWND filter);
void DestroyTaskWindows(Queue *);
void UnregisterTaskClasses(HINSTANCE);
void PaintDesktop(void);
void Activate(Wnd *,int how);
void TaskList(Wnd *); /* nc.c */
BOOL DialogEnded(HWND); /* dialog.c */
LRESULT CallProc(WNDPROC,HWND,UINT,WPARAM,LPARAM); /* pack16.c: Win16 sources' procedures get Windows 3.0's packing */
BOOL DDEBlock(LPARAM); /* dde.c: a block of PackDDElParam's */
LRESULT CallHookProc(HOOKPROC,int id,int code,WPARAM,LPARAM); /* pack16.c: a hook procedure called */
int HookKind(HHOOK); /* hook.c: WH_MSGFILTER-1 for none */
void CalcClient(Wnd *);
void SetWndText(Wnd *,LPCSTR);
void PlaceWindow(Wnd *,const RECT *window,BOOL notify);
void ShowState(Wnd *,int command);
Class *FindClass(LPCSTR,HINSTANCE);
ATOM RegisterSystemClass(LPCSTR name,WNDPROC proc,UINT style,int extra,HCURSOR cursor,int background);
void WndInit(void);
/* msg.c */
Queue *CurrentQueue(void);
Queue *QueueOfTask(HTASK);
void PollInput(void);
BOOL KeyDown(int vk);
void SetCursorShape(HCURSOR);
void MsgInit(void);
void KillWindowTimers(Wnd *);
void CaretOff(Wnd *);
/* hook.c */
BOOL HookActive(int id);
LRESULT CallHook(int id,int code,WPARAM,LPARAM);
void HooksTaskEnded(Queue *);
void CommTaskEnded(HTASK); /* comm.c */
void EndPlayback(void);
void HookInit(void);
/* nc.c */
int HitTest(Wnd *,POINT);
int FrameWidth(Wnd *);
void *ualloc(DWORD bytes); /* wnd.c: shared memory of USER's */
void PaintFrame(Wnd *);
LRESULT NcButtonDown(Wnd *,int hit,POINT,BOOL dblclk);
void SysCommand(Wnd *,WPARAM,LPARAM);
void MaximizedRect(Wnd *,RECT *);
void ArrangeIcons(Wnd *parent);
void DrawScrollBar(HDC,const RECT *,BOOL vertical,const ScrollInfo *,int pressed,BOOL enabled);
void TrackScroll(Wnd *owner,HWND notify,Wnd *control,int bar,const RECT *r,POINT start);
void NcInit(void);
void NcShutdown(void);
/* menu.c */
void MenuBarRect(Wnd *,RECT *);
void PaintMenuBar(Wnd *,HDC);
int MenuBarHeight(Wnd *);
void TrackMenuBar(Wnd *,int item,POINT,BOOL keyboard);
BOOL MenuKey(Wnd *,WPARAM);
void SystemMenuPopup(Wnd *,BOOL keyboard);
void FreeWindowMenus(Wnd *);
void MenuInit(void);
void MenuTaskEnded(Queue *);
/* draw.c */
HBRUSH SysBrush(int);
HFONT SystemFont(void);
int CharHeight(void);
int CharWidth(void); /* the system font's, for dialog units */
int LetterWidth(HDC);
void DrawInit(void);
void DrawShutdown(void);
void DrawButtonFace(HDC,const RECT *,BOOL pressed);
void DrawPrefixText(HDC,int x,int y,LPCSTR,int count,BOOL gray);
int PrefixTextWidth(HDC,LPCSTR,int count);
char PrefixChar(LPCSTR);
HBRUSH ControlColor(Wnd *,HDC,UINT msg);
/* resource.c */
HICON ArtIcon(const char *const *rows,int scale); /* resource.c's letters for colors, scaled; the system's */
HCURSOR StockCursor(LPCSTR);
const void *Resource(HINSTANCE,LPCSTR type,LPCSTR name,DWORD *size);
int WideToAnsi(const WORD *,char *,int size);
const WORD *SkipWide(const WORD *);
void ResourceInit(void);
void ResourceShutdown(void);
void ResourceTaskEnded(Queue *);
/* dialog.c */
void DialogInit(void);
LRESULT SendNotify(Wnd *,UINT code);
/* controls */
LRESULT CALLBACK ButtonProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK StaticProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK EditProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK ListBoxProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK ComboBoxProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK ComboListProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK ScrollBarProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK MDIClientProc(HWND,UINT,WPARAM,LPARAM);
/* clip.c */
void ClipboardWindowGone(Wnd *);
#endif
