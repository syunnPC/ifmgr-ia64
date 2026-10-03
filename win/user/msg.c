/* SPDX-License-Identifier: GPL-2.0-or-later
 * Per-task message queues, system input, cross-task sends and timers.
 * GetMessage handles cross-task SendMessage first (the sender waits for
 * processing in the receiver's context), then input, posted messages,
 * WM_QUIT, WM_PAINT and WM_TIMER. With no work, the task blocks.
 *
 * Requesting tasks and the idle hook poll input into the system queue;
 * the cursor moves immediately. Events retain order and dispatch in the
 * owning task: capture/window under the pointer for mouse, focus/active
 * window for keys. Dispatch performs hit testing, activation and cursor
 * updates. Keys arrive as strokes with immediate key-up. Journal hooks
 * (hook.c) observe or supply input.
 */
#include "user.h"

static Queue queues[QUEUES];
int cursor_x,cursor_y;
/* A played key (journal playback) carries its virtual key and character. */
typedef struct {WhEvent e; int x,y; BOOL move_only,played; WPARAM vk; WORD ch;} RawInput;
static RawInput input[INPUT_SIZE];
static int input_head,input_count;
static wh_u32 buttons,queued_buttons;
static BYTE key_state[256];
/* The shift keys and the buttons come with each key and mouse message, and
 * GetKeyState gives them as of the message last retrieved, as Windows
 * does: when keys wait, a later one's Ctrl must not show with an earlier
 * one (Enter taken for Ctrl+Enter, F5 missing its accelerator).
 * GetAsyncKeyState gives them as of the input last read. */
#define STATE_SHIFT 0x01
#define STATE_CONTROL 0x02
#define STATE_ALT 0x04
#define STATE_LBUTTON 0x08
#define STATE_RBUTTON 0x10
#define STATE_VALID 0x80
static BYTE input_state;
static const struct {int vk; BYTE bit;} state_keys[]={
    {VK_SHIFT,STATE_SHIFT},{VK_CONTROL,STATE_CONTROL},{VK_MENU,STATE_ALT},{VK_LBUTTON,STATE_LBUTTON},{VK_RBUTTON,STATE_RBUTTON}};
static void retrieved_state(BYTE state) {
    unsigned i;
    if(!(state&STATE_VALID)) return;
    for(i=0;i<sizeof(state_keys)/sizeof(state_keys[0]);i++)
        key_state[state_keys[i].vk]=(BYTE)((key_state[state_keys[i].vk]&1)|(state&state_keys[i].bit?0x80:0));
}
static struct {HWND hwnd; UINT msg; DWORD time; POINT pt;} click;
static UINT double_click_time=500;
static HCURSOR cursor_shape;
static int cursor_shown=0;

/* --- queues ----------------------------------------------------------------- */
Queue *CurrentQueue(void) {
    void **slots=wh_task_slots(0); int i;
    if(!slots) return NULL;
    if(slots[1]) return (Queue *)slots[1];
    for(i=0;i<QUEUES;i++) if(!queues[i].used) {
        Queue *q=&queues[i];
        memset(q,0,sizeof(*q)); q->used=TRUE; q->task=wh_task_current(); q->htask=GetCurrentTask(); slots[1]=q;
        return q;
    }
    return NULL;
}
Queue *QueueOfTask(HTASK task) {
    int i;
    for(i=0;i<QUEUES;i++) if(queues[i].used && queues[i].htask==task) return &queues[i];
    return NULL;
}
BOOL WINAPI InitApp(HINSTANCE instance) {Queue *q=CurrentQueue(); if(q) q->instance=instance; return q!=NULL;}
static void fill_msg(MSG *m,HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    m->hwnd=h; m->message=msg; m->wParam=wp; m->lParam=lp;
    m->time=(DWORD)wh_ticks(); m->pt.x=cursor_x; m->pt.y=cursor_y;
}
static BOOL post_input(Queue *q,HWND h,UINT msg,WPARAM wp,LPARAM lp,WORD ch,BYTE state) {
    QMsg *m;
    if(!q || !q->used || q->count==QUEUE_SIZE) return FALSE;
    m=&q->ring[(q->head+q->count)%QUEUE_SIZE]; q->count++;
    fill_msg(&m->msg,h,msg,wp,lp); m->ch=ch; m->state=state;
    wh_wake(q->task);
    return TRUE;
}
static BOOL PostQueue(Queue *q,HWND h,UINT msg,WPARAM wp,LPARAM lp) {return post_input(q,h,msg,wp,lp,0,0);}
static void push_front(Queue *q,HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(q->count==QUEUE_SIZE) return;
    q->head=(q->head+QUEUE_SIZE-1)%QUEUE_SIZE; q->count++;
    fill_msg(&q->ring[q->head].msg,h,msg,wp,lp); q->ring[q->head].ch=0; q->ring[q->head].state=0;
}
static void remove_at(Queue *q,int i) {
    for(;i+1<q->count;i++) q->ring[(q->head+i)%QUEUE_SIZE]=q->ring[(q->head+i+1)%QUEUE_SIZE];
    q->count--;
}
BOOL WINAPI PostMessage(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h);
    if(h==(HWND)0xffff) {
        Wnd *t; BOOL ok=TRUE;
        for(t=desktop->child;t;t=t->next) ok&=PostQueue(t->queue,t->handle,msg,wp,lp);
        return ok;
    }
    if(h && !w) return FALSE;
    return PostQueue(w?w->queue:CurrentQueue(),h,msg,wp,lp);
}
BOOL WINAPI PostAppMessage(HTASK task,UINT msg,WPARAM wp,LPARAM lp) {return PostQueue(QueueOfTask(task),NULL,msg,wp,lp);}
void WINAPI PostQuitMessage(int code) {
    Queue *q=CurrentQueue();
    if(q) {q->quit=TRUE; q->quit_code=code;}
}

/* --- sent messages ---------------------------------------------------------- */
static LRESULT call(Wnd *w,UINT msg,WPARAM wp,LPARAM lp) {return CallProc(w->proc,w->handle,msg,wp,lp);}
static void ReceiveSent(Queue *q) {
    while(q && q->sent) {
        SendRec *r=q->sent,*outer=q->receiving; Wnd *w; LRESULT result;
        q->sent=r->next; q->receiving=r;
        w=WndFromHandle(r->hwnd);
        result=w?call(w,r->msg,r->wp,r->lp):0;
        if(q->receiving==r) {r->result=result; r->done=TRUE; wh_wake(r->from->task);}
        q->receiving=outer;
    }
}
LRESULT WINAPI SendMessage(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    Wnd *w=WndFromHandle(h); Queue *q=CurrentQueue(); SendRec r,**tail;
    if(h==(HWND)0xffff) {
        Wnd *list[64],*t; int n=0,i;
        for(t=desktop->child;t && n<64;t=t->next) list[n++]=t;
        for(i=0;i<n;i++) if(list[i]->used) SendMessage(list[i]->handle,msg,wp,lp);
        return 0;
    }
    if(!w || !w->proc) return 0;
    if(HookActive(WH_CALLWNDPROC)) {
        CWPSTRUCT c; c.lParam=lp; c.wParam=wp; c.message=msg; c.hwnd=h;
        CallHook(WH_CALLWNDPROC,HC_ACTION,w->queue==q,(LPARAM)&c);
        if(!w->used) return 0;
    }
    if(!w->queue || !q || w->queue==q || !w->queue->used) return call(w,msg,wp,lp);
    memset(&r,0,sizeof(r));
    r.hwnd=h; r.msg=msg; r.wp=wp; r.lp=lp; r.from=q;
    for(tail=&w->queue->sent;*tail;tail=&(*tail)->next);
    *tail=&r;
    wh_wake(w->queue->task);
    while(!r.done) {
        ReceiveSent(q);
        if(!r.done) wh_block();
    }
    return r.result;
}
LRESULT WINAPI CallWindowProc(WNDPROC proc,HWND h,UINT msg,WPARAM wp,LPARAM lp) {return CallProc(proc,h,msg,wp,lp);}
BOOL WINAPI InSendMessage(void) {Queue *q=CurrentQueue(); return q && q->receiving;}
void WINAPI ReplyMessage(LRESULT result) {
    Queue *q=CurrentQueue(); SendRec *r;
    if(!q || !(r=q->receiving)) return;
    r->result=result; r->done=TRUE; wh_wake(r->from->task);
    q->receiving=NULL;
}
/* A queue going away answers what was sent to it. */
static void answer_all(Queue *q) {
    while(q->sent) {SendRec *r=q->sent; q->sent=r->next; r->result=0; r->done=TRUE; wh_wake(r->from->task);}
    if(q->receiving) {q->receiving->done=TRUE; wh_wake(q->receiving->from->task); q->receiving=NULL;}
}

/* --- timers ------------------------------------------------------------------ */
typedef struct {BOOL used,system; Queue *queue; HWND hwnd; UINT_PTR id; UINT interval; DWORD due; TIMERPROC proc;} Timer;
static Timer timers[TIMERS];
static DWORD now(void) {return (DWORD)wh_ticks();}
static UINT_PTR add_timer(HWND h,UINT_PTR id,UINT interval,TIMERPROC proc,BOOL system) {
    Wnd *w=WndFromHandle(h); Queue *q=w?w->queue:CurrentQueue(); int i,free_slot=-1;
    if(h && !w) return 0;
    if(interval<10) interval=10;
    for(i=0;i<TIMERS;i++) {
        if(timers[i].used && timers[i].system==system && timers[i].queue==q && timers[i].hwnd==h && (h?timers[i].id==id:FALSE)) {free_slot=i; break;}
        if(!timers[i].used && free_slot<0) free_slot=i;
    }
    if(free_slot<0) return 0;
    if(!h) {
        /* Window-less timers get an id of their own. */
        static UINT_PTR next_id=0x7f00;
        id=++next_id;
    }
    timers[free_slot].used=TRUE; timers[free_slot].system=system; timers[free_slot].queue=q; timers[free_slot].hwnd=h;
    timers[free_slot].id=id; timers[free_slot].interval=interval; timers[free_slot].due=now()+interval; timers[free_slot].proc=proc;
    return h?(id?id:1):id;
}
UINT_PTR WINAPI SetTimer(HWND h,UINT_PTR id,UINT interval,TIMERPROC proc) {return add_timer(h,id,interval,proc,FALSE);}
static BOOL kill_timer(HWND h,UINT_PTR id,BOOL system) {
    int i; Queue *q=CurrentQueue();
    for(i=0;i<TIMERS;i++) if(timers[i].used && timers[i].system==system && timers[i].hwnd==h && timers[i].id==id && (h || timers[i].queue==q)) {
        timers[i].used=FALSE; return TRUE;
    }
    return FALSE;
}
BOOL WINAPI KillTimer(HWND h,UINT_PTR id) {return kill_timer(h,id,FALSE);}
void KillWindowTimers(Wnd *w) {int i; for(i=0;i<TIMERS;i++) if(timers[i].used && timers[i].hwnd==w->handle) timers[i].used=FALSE;}
static void TimersOfQueue(Queue *q) {int i; for(i=0;i<TIMERS;i++) if(timers[i].used && timers[i].queue==q) timers[i].used=FALSE;}
static Timer *due_timer(Queue *q,HWND filter,BOOL system) {
    int i; DWORD t=now();
    for(i=0;i<TIMERS;i++) {
        Timer *m=&timers[i];
        if(m->used && m->queue==q && m->system==system && (LONG)(t-m->due)>=0 && (!filter || m->hwnd==filter)) return m;
    }
    return NULL;
}
static wh_u32 timer_wait(void) {
    int i; DWORD t=now(); wh_u32 wait=WH_FOREVER;
    for(i=0;i<TIMERS;i++) if(timers[i].used) {
        LONG left=(LONG)(timers[i].due-t);
        if(left<=0) {wh_wake(timers[i].queue->task); wait=0;}
        else if((wh_u32)left<wait) wait=(wh_u32)left;
    }
    return wait;
}

/* --- the caret ------------------------------------------------------------------ */
static struct {Wnd *wnd; int x,y,w,h,hidden; BOOL on; HBITMAP bitmap; UINT blink;} caret={NULL,0,0,0,0,1,FALSE,NULL,530};
static void caret_invert(void) {
    HDC dc; Wnd *w=caret.wnd;
    if(!w || !Shown(w)) return;
    dc=GetDC(w->handle);
    if(caret.bitmap) {
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,caret.bitmap);
        BitBlt(dc,caret.x,caret.y,caret.w,caret.h,mem,0,0,SRCINVERT);
        SelectObject(mem,old); DeleteDC(mem);
    } else PatBlt(dc,caret.x,caret.y,caret.w,caret.h,DSTINVERT);
    ReleaseDC(w->handle,dc);
    caret.on=!caret.on;
}
static void caret_hide(void) {if(caret.on) caret_invert();}
void WINAPI CreateCaret(HWND h,HBITMAP bitmap,int width,int height) {
    Wnd *w=WndFromHandle(h);
    if(!w) return;
    DestroyCaret();
    caret.wnd=w; caret.hidden=1; caret.on=FALSE; caret.x=caret.y=0;
    caret.bitmap=(ULONG_PTR)bitmap>1?bitmap:NULL;
    if(caret.bitmap) {BITMAP b; GetObject(bitmap,sizeof(b),&b); width=(int)b.bmWidth; height=(int)b.bmHeight;}
    caret.w=width?width:1; caret.h=height?height:CharHeight();
    add_timer(h,1,caret.blink,NULL,TRUE);
}
void WINAPI DestroyCaret(void) {
    if(!caret.wnd) return;
    caret_hide();
    kill_timer(caret.wnd->handle,1,TRUE);
    caret.wnd=NULL; caret.bitmap=NULL;
}
void CaretOff(Wnd *w) {if(caret.wnd==w) {kill_timer(w->handle,1,TRUE); caret.wnd=NULL; caret.on=FALSE;}}
void WINAPI HideCaret(HWND h) {
    if(!caret.wnd || (h && caret.wnd->handle!=h)) return;
    if(!caret.hidden++) caret_hide();
}
void WINAPI ShowCaret(HWND h) {
    if(!caret.wnd || (h && caret.wnd->handle!=h) || !caret.hidden) return;
    if(!--caret.hidden) {caret_invert(); add_timer(caret.wnd->handle,1,caret.blink,NULL,TRUE);}
}
void WINAPI SetCaretPos(int x,int y) {
    BOOL was;
    if(!caret.wnd || (caret.x==x && caret.y==y)) return;
    was=caret.on; caret_hide();
    caret.x=x; caret.y=y;
    if(was || !caret.hidden) {caret_invert(); add_timer(caret.wnd->handle,1,caret.blink,NULL,TRUE);}
}
void WINAPI GetCaretPos(LPPOINT p) {if(p) {p->x=caret.x; p->y=caret.y;}}
void WINAPI SetCaretBlinkTime(UINT ms) {caret.blink=ms?ms:530;}
UINT WINAPI GetCaretBlinkTime(void) {return caret.blink;}

/* --- the cursor ------------------------------------------------------------------ */
void SetCursorShape(HCURSOR c) {
    if(c==cursor_shape) return;
    cursor_shape=c;
    SetCursor(c);
}
HCURSOR WINAPI GetCursor(void) {return cursor_shape;}
int WINAPI ShowCursor(BOOL show) {
    cursor_shown+=show?1:-1;
    GdiMoveCursor(cursor_x,cursor_y,cursor_shown>=0);
    return cursor_shown;
}
void WINAPI GetCursorPos(LPPOINT p) {if(p) {p->x=cursor_x; p->y=cursor_y;}}
void WINAPI SetCursorPos(int x,int y) {
    cursor_x=max(0,min(x,screen_width-1)); cursor_y=max(0,min(y,screen_height-1));
    GdiMoveCursor(cursor_x,cursor_y,cursor_shown>=0);
}
void WINAPI ClipCursor(LPCRECT r) {(void)r;}
UINT WINAPI GetDoubleClickTime(void) {return double_click_time;}
void WINAPI SetDoubleClickTime(UINT ms) {double_click_time=ms?ms:500;}
BOOL WINAPI SwapMouseButton(BOOL swap) {(void)swap; return FALSE;}
DWORD WINAPI GetMessagePos(void) {Queue *q=CurrentQueue(); return q?(DWORD)MAKELONG(q->last.pt.x,q->last.pt.y):0;}
LONG WINAPI GetMessageTime(void) {Queue *q=CurrentQueue(); return q?(LONG)q->last.time:0;}

/* --- capture and keys ------------------------------------------------------------------ */
HWND WINAPI SetCapture(HWND h) {
    Wnd *w=WndFromHandle(h),*old=capture;
    capture=w;
    return old?old->handle:NULL;
}
void WINAPI ReleaseCapture(void) {capture=NULL;}
HWND WINAPI GetCapture(void) {return capture?capture->handle:NULL;}
BOOL KeyDown(int vk) {return (key_state[vk&0xff]&0x80)!=0;}
int WINAPI GetKeyState(int vk) {
    BYTE s=key_state[vk&0xff];
    return (s&0x80?(int)(short)0xff80:0)|(s&1);
}
int WINAPI GetAsyncKeyState(int vk) {
    unsigned i;
    for(i=0;i<sizeof(state_keys)/sizeof(state_keys[0]);i++) if(state_keys[i].vk==(vk&0xff)) return input_state&state_keys[i].bit?(int)(short)0x8000:0;
    return GetKeyState(vk)&0x8000?(int)(short)0x8000:0;
}
void WINAPI GetKeyboardState(LPBYTE out) {if(out) memcpy(out,key_state,256);}
void WINAPI SetKeyboardState(LPBYTE in) {if(in) memcpy(key_state,in,256);}
int WINAPI ToAscii(UINT vk,UINT scan,LPBYTE state,LPWORD out,UINT flags) {
    (void)scan; (void)state; (void)flags;
    if(!out) return 0;
    if(vk>='A' && vk<='Z') {*out=(WORD)(KeyDown(VK_SHIFT)?vk:vk+0x20); return 1;}
    if((vk>='0' && vk<='9') || vk==' ') {*out=(WORD)vk; return 1;}
    return 0;
}
UINT WINAPI MapVirtualKey(UINT code,UINT type) {(void)type; return code;}
int WINAPI GetKeyboardType(int which) {return which==0?4:which==2?12:0;}
BOOL WINAPI GetInputState(void) {PollInput(); return input_count>0;}
DWORD WINAPI GetQueueStatus(UINT flags) {Queue *q=CurrentQueue(); (void)flags; PollInput(); return (q && q->count)||input_count?0x10001:0;}
HWND WINAPI GetSysModalWindow(void) {return NULL;}
HWND WINAPI SetSysModalWindow(HWND h) {(void)h; return NULL;}
BOOL WINAPI EnableHardwareInput(BOOL enable) {(void)enable; return TRUE;}
UINT WINAPI RegisterWindowMessage(LPCSTR name) {
    static char names[64][32]; static int count; int i;
    if(!name) return 0;
    for(i=0;i<count;i++) if(!lstrcmpi(names[i],name)) return 0xc000+i;
    if(count==64) return 0;
    lstrcpyn(names[count],name,sizeof(names[0]));
    return 0xc000+count++;
}

/* --- input ---------------------------------------------------------------------------------- */
/* The US layout's punctuation keys and their virtual keys. */
static const char punctuation[]=";=,-./`[\\]'";
static const BYTE punctuation_vk[]={0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,0xdb,0xdc,0xdd,0xde};
static WPARAM key_of(const WhEvent *e,WORD *ch) {
    wh_u32 c=e->unicode; const char *p;
    *ch=0;
    if(e->scan>=WH_SCAN_F1 && e->scan<=WH_SCAN_F12) return VK_F1+(e->scan-WH_SCAN_F1);
    switch(e->scan) {
    case WH_SCAN_UP: return VK_UP;
    case WH_SCAN_DOWN: return VK_DOWN;
    case WH_SCAN_LEFT: return VK_LEFT;
    case WH_SCAN_RIGHT: return VK_RIGHT;
    case WH_SCAN_HOME: return VK_HOME;
    case WH_SCAN_END: return VK_END;
    case WH_SCAN_INSERT: return VK_INSERT;
    case WH_SCAN_DELETE: return VK_DELETE;
    case WH_SCAN_PAGE_UP: return VK_PRIOR;
    case WH_SCAN_PAGE_DOWN: return VK_NEXT;
    case WH_SCAN_ESCAPE: *ch=0x1b; return VK_ESCAPE;
    case WH_SCAN_PAUSE: /* Ctrl+Pause is Ctrl+Break */
        return (e->flags&WH_KEY_MODIFIERS_VALID) && (e->modifiers&WH_MOD_CONTROL)?VK_CANCEL:VK_PAUSE;
    }
    if(!c) return 0;
    *ch=(WORD)(c<0x100?c:'?');
    if(c>='a' && c<='z') return c-0x20;
    if((c>='A' && c<='Z') || (c>='0' && c<='9')) return c;
    if(c>=1 && c<=26 && c!='\b' && c!='\t' && c!='\r') return 'A'+c-1;
    switch(c) {
    case ' ': return VK_SPACE;
    case '\r': return VK_RETURN;
    case '\b': return VK_BACK;
    case '\t': return VK_TAB;
    case 0x1b: return VK_ESCAPE;
    case 0x7f: *ch=0; return VK_DELETE;
    }
    if(c<0x80 && (p=strchr(punctuation,(int)c))!=NULL) return punctuation_vk[p-punctuation];
    return 0xff;
}
static Wnd *key_target(void) {return focus?focus:active;}
static Wnd *pointer_target(const RawInput *r) {
    POINT p; Wnd *w;
    if(capture) return capture;
    p.x=r->x; p.y=r->y; w=WndFromPoint(p);
    return w;
}
static void queue_input(const RawInput *in) {
    RawInput *r; Wnd *target;
    if(input_count==INPUT_SIZE) return;
    r=&input[(input_head+input_count)%INPUT_SIZE]; input_count++;
    *r=*in;
    if(r->e.type==WH_EVENT_POINTER) queued_buttons=r->e.buttons;
    target=r->e.type==WH_EVENT_KEY?key_target():pointer_target(r);
    /* The desktop has no task of its own: the active window's takes its clicks. */
    if(target==desktop) target=active;
    if(target && target->queue) wh_wake(target->queue->task);
}

/* --- the journal ------------------------------------------------------------ */
static const struct {wh_u32 mod; WPARAM vk;} shifts[3]={{WH_MOD_SHIFT,VK_SHIFT},{WH_MOD_CONTROL,VK_CONTROL},{WH_MOD_ALT,VK_MENU}};
static wh_u32 recorded_mods,recorded_buttons,played_mods,played_buttons;
static void record(UINT msg,UINT l,UINT h,Wnd *w) {
    EVENTMSG e; e.message=msg; e.paramL=l; e.paramH=h; e.time=(DWORD)wh_ticks(); e.hwnd=w?w->handle:NULL;
    CallHook(WH_JOURNALRECORD,HC_ACTION,0,(LPARAM)&e);
}
static void record_event(const WhEvent *e) {
    if(e->type==WH_EVENT_KEY) {
        WORD ch; WPARAM vk=key_of(e,&ch); wh_u32 mods=e->flags&WH_KEY_MODIFIERS_VALID?e->modifiers:0; int i; BOOL sys;
        Wnd *w=key_target(); UINT code=(UINT)vk|(e->scan&0xff)<<8;
        if(!vk) return;
        for(i=0;i<3;i++) if((mods&shifts[i].mod) && !(recorded_mods&shifts[i].mod)) record(WM_KEYDOWN,(UINT)shifts[i].vk,1,w);
        for(i=0;i<3;i++) if(!(mods&shifts[i].mod) && (recorded_mods&shifts[i].mod)) record(WM_KEYUP,(UINT)shifts[i].vk,1,w);
        recorded_mods=mods&(WH_MOD_SHIFT|WH_MOD_CONTROL|WH_MOD_ALT);
        sys=(mods&WH_MOD_ALT) || vk==VK_F10;
        record(sys?WM_SYSKEYDOWN:WM_KEYDOWN,code,1,w); record(sys?WM_SYSKEYUP:WM_KEYUP,code,1,w);
    } else {
        RawInput r; Wnd *w; wh_u32 down=e->buttons&~recorded_buttons,up=recorded_buttons&~e->buttons;
        r.x=cursor_x; r.y=cursor_y; w=pointer_target(&r);
        if(e->dx || e->dy) record(WM_MOUSEMOVE,(UINT)cursor_x,(UINT)cursor_y,w);
        if(down&1) record(WM_LBUTTONDOWN,(UINT)cursor_x,(UINT)cursor_y,w);
        if(up&1) record(WM_LBUTTONUP,(UINT)cursor_x,(UINT)cursor_y,w);
        if(down&2) record(WM_RBUTTONDOWN,(UINT)cursor_x,(UINT)cursor_y,w);
        if(up&2) record(WM_RBUTTONUP,(UINT)cursor_x,(UINT)cursor_y,w);
        recorded_buttons=e->buttons;
    }
}
/* The character of a key, with the shift keys a playback holds down. */
static WORD vk_char(WPARAM vk,BOOL shift) {
    static const char shifted[]=":+<_>?~{|}\"",digits[]=")!@#$%^&*(";
    int i;
    if(vk>='A' && vk<='Z') return (WORD)(shift?vk:vk+0x20);
    if(vk>='0' && vk<='9') return (WORD)(shift?digits[vk-'0']:vk);
    switch(vk) {
    case VK_SPACE: return ' ';
    case VK_RETURN: return '\r';
    case VK_BACK: return '\b';
    case VK_TAB: return '\t';
    case VK_ESCAPE: return 0x1b;
    }
    for(i=0;i<11;i++) if(punctuation_vk[i]==vk) return (WORD)(BYTE)(shift?shifted[i]:punctuation[i]);
    return 0;
}
static void play(const EVENTMSG *m) {
    RawInput r; int i;
    memset(&r,0,sizeof(r));
    r.e.time_ms=wh_ticks();
    switch(m->message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        WPARAM vk=m->paramL&0xff; BOOL down=m->message==WM_KEYDOWN || m->message==WM_SYSKEYDOWN;
        for(i=0;i<3;i++) if(shifts[i].vk==vk) {if(down) played_mods|=shifts[i].mod; else played_mods&=~shifts[i].mod; return;}
        if(!down) return;
        r.e.type=WH_EVENT_KEY; r.e.flags=WH_KEY_MODIFIERS_VALID; r.e.modifiers=played_mods; r.e.scan=(m->paramL>>8)&0xff;
        r.played=TRUE; r.vk=vk; r.ch=vk_char(vk,(played_mods&WH_MOD_SHIFT)!=0);
        r.x=cursor_x; r.y=cursor_y;
        queue_input(&r);
        return;
    }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int x=max(0,min((int)m->paramL,screen_width-1)),y=max(0,min((int)m->paramH,screen_height-1));
        if(m->message==WM_LBUTTONDOWN) played_buttons|=1;
        if(m->message==WM_LBUTTONUP) played_buttons&=~1U;
        if(m->message==WM_RBUTTONDOWN) played_buttons|=2;
        if(m->message==WM_RBUTTONUP) played_buttons&=~2U;
        r.e.type=WH_EVENT_POINTER; r.e.buttons=played_buttons; r.e.dx=x-cursor_x; r.e.dy=y-cursor_y;
        cursor_x=x; cursor_y=y; GdiMoveCursor(cursor_x,cursor_y,cursor_shown>=0);
        r.x=x; r.y=y; r.move_only=played_buttons==queued_buttons;
        queue_input(&r);
        return;
    }
    }
}
/* The event the playback gave, until it is due. */
static struct {BOOL pending; EVENTMSG event; DWORD due;} playback;
static void play_journal(void) {
    WhEvent e; int n;
    /* The keyboard and mouse are ignored; Ctrl+Esc or Ctrl+Break ends it. */
    while(!wh_poll_event(&e))
        if(e.type==WH_EVENT_KEY && (e.flags&WH_KEY_MODIFIERS_VALID) && (e.modifiers&WH_MOD_CONTROL) &&
           (e.scan==WH_SCAN_ESCAPE || e.scan==WH_SCAN_PAUSE)) {EndPlayback(); playback.pending=FALSE; return;}
    for(n=0;n<32 && HookActive(WH_JOURNALPLAYBACK) && input_count<INPUT_SIZE;n++) {
        if(!playback.pending) {
            LRESULT wait;
            memset(&playback.event,0,sizeof(playback.event));
            wait=CallHook(WH_JOURNALPLAYBACK,HC_GETNEXT,0,(LPARAM)&playback.event);
            playback.pending=TRUE; playback.due=(DWORD)wh_ticks()+(DWORD)(wait>0?wait:0);
        }
        if((LONG)(playback.due-(DWORD)wh_ticks())>0) return;
        playback.pending=FALSE;
        play(&playback.event);
        CallHook(WH_JOURNALPLAYBACK,HC_SKIP,0,0);
    }
}
static wh_u32 playback_wait(void) {
    LONG left;
    if(!playback.pending || !HookActive(WH_JOURNALPLAYBACK)) return WH_FOREVER;
    left=(LONG)(playback.due-(DWORD)wh_ticks());
    return left>0?(wh_u32)left:0;
}

/* Read the host's events into the system queue and wake their tasks. */
void PollInput(void) {
    WhEvent e;
    if(HookActive(WH_JOURNALPLAYBACK)) {play_journal(); return;}
    playback.pending=FALSE; played_mods=0;
    while(!wh_poll_event(&e)) {
        RawInput r;
        if(e.type==WH_EVENT_POINTER) {
            cursor_x+=e.dx; cursor_y+=e.dy;
            if(cursor_x<0) cursor_x=0;
            if(cursor_y<0) cursor_y=0;
            if(cursor_x>=screen_width) cursor_x=screen_width-1;
            if(cursor_y>=screen_height) cursor_y=screen_height-1;
            GdiMoveCursor(cursor_x,cursor_y,cursor_shown>=0);
        } else if(e.type!=WH_EVENT_KEY) continue;
        if(HookActive(WH_JOURNALRECORD)) record_event(&e);
        if(e.type==WH_EVENT_POINTER && input_count && e.buttons==queued_buttons) {
            /* A move after a move replaces it. */
            RawInput *last=&input[(input_head+input_count-1)%INPUT_SIZE];
            if(last->e.type==WH_EVENT_POINTER && last->move_only) {
                last->x=cursor_x; last->y=cursor_y; last->e.dx+=e.dx; last->e.dy+=e.dy; continue;
            }
        }
        memset(&r,0,sizeof(r));
        r.e=e; r.x=cursor_x; r.y=cursor_y;
        r.move_only=e.type==WH_EVENT_POINTER && e.buttons==queued_buttons;
        queue_input(&r);
    }
}
/* Clicks are timed when they happened, not when a busy task got to them. */
static BOOL double_click(Wnd *w,UINT msg,POINT p,BOOL nc,DWORD t) {
    BOOL dbl;
    if(!nc && !(w->cls->wc.style&CS_DBLCLKS)) {click.hwnd=NULL; return FALSE;}
    dbl=click.hwnd==w->handle && click.msg==msg && t-click.time<=double_click_time &&
        p.x-click.pt.x<=4 && click.pt.x-p.x<=4 && p.y-click.pt.y<=4 && click.pt.y-p.y<=4;
    if(dbl) click.hwnd=NULL;
    else {click.hwnd=w->handle; click.msg=msg; click.time=t; click.pt=p;}
    return dbl;
}
static void post_pointer(Queue *q,Wnd *w,UINT msg,int hit,POINT p,WPARAM mk) {
    if(hit==HTCLIENT || capture==w) {
        LPARAM lp=MAKELPARAM(p.x-w->client.left,p.y-w->client.top);
        if(msg==WM_MOUSEMOVE && q->count) {
            QMsg *last=&q->ring[(q->head+q->count-1)%QUEUE_SIZE];
            if(last->msg.message==WM_MOUSEMOVE && last->msg.hwnd==w->handle) {fill_msg(&last->msg,w->handle,msg,mk,lp); last->state=(BYTE)(STATE_VALID|input_state); return;}
        }
        post_input(q,w->handle,msg,mk,lp,0,(BYTE)(STATE_VALID|input_state));
    } else {
        UINT nc=msg-WM_MOUSEMOVE+WM_NCMOUSEMOVE;
        post_input(q,w->handle,nc,(WPARAM)hit,MAKELPARAM(p.x,p.y),0,(BYTE)(STATE_VALID|input_state));
    }
}
/* A double click on the desktop brings up the Task List, through the
 * active window's system command when there is one. */
static void desktop_click(const RawInput *r) {
    static DWORD last; static int x,y;
    DWORD t=(DWORD)r->e.time_ms;
    if(last && t-last<=double_click_time && r->x-x<=4 && x-r->x<=4 && r->y-y<=4 && y-r->y<=4) {
        last=0;
        if(active && active->queue) PostQueue(active->queue,TopLevel(active)->handle,WM_SYSCOMMAND,SC_TASKLIST,0);
        else TaskList(NULL);
        return;
    }
    last=t?t:1; x=r->x; y=r->y;
}
/* Turn the system queue's events for this task into messages; stop at one
 * for another task (and wake it), so input stays in order. */
static void process_input(Queue *q) {
    while(input_count) {
        RawInput r=input[input_head]; Wnd *target;
        if(r.e.type==WH_EVENT_KEY) {
            WORD ch; WPARAM vk; BOOL alt,ctrl,shift; LPARAM lp;
            target=key_target();
            if(target && target->queue!=q) {if(target->queue) wh_wake(target->queue->task); return;}
            input_head=(input_head+1)%INPUT_SIZE; input_count--;
            if(!target) continue;
            if(r.played) {vk=r.vk; ch=r.ch;} else vk=key_of(&r.e,&ch);
            if(!vk) continue;
            alt=(r.e.flags&WH_KEY_MODIFIERS_VALID) && (r.e.modifiers&WH_MOD_ALT);
            ctrl=(r.e.flags&WH_KEY_MODIFIERS_VALID) && (r.e.modifiers&WH_MOD_CONTROL);
            shift=(r.e.flags&WH_KEY_MODIFIERS_VALID) && (r.e.modifiers&WH_MOD_SHIFT);
            input_state=(BYTE)((input_state&(STATE_LBUTTON|STATE_RBUTTON))|(alt?STATE_ALT:0)|(ctrl?STATE_CONTROL:0)|(shift?STATE_SHIFT:0));
            /* Ctrl+Esc is the system's: the Task List. */
            if(vk==VK_ESCAPE && ctrl && !alt) {PostQueue(q,TopLevel(target)->handle,WM_SYSCOMMAND,SC_TASKLIST,0); continue;}
            if(ctrl && vk>='A' && vk<='Z') ch=(WORD)(vk-'A'+1);
            lp=1|((LPARAM)(r.e.scan&0xff)<<16)|(alt?((LPARAM)1<<29):0);
            /* Alt+key and F10 are system keys, as in Windows. */
            post_input(q,target->handle,alt || vk==VK_F10?WM_SYSKEYDOWN:WM_KEYDOWN,vk,lp,ch,(BYTE)(STATE_VALID|input_state));
            post_input(q,target->handle,alt || vk==VK_F10?WM_SYSKEYUP:WM_KEYUP,vk,lp|((LPARAM)3<<30),0,(BYTE)(STATE_VALID|input_state));
        } else {
            wh_u32 down=r.e.buttons&~buttons,up=buttons&~r.e.buttons; POINT p; int hit; WPARAM mk; Wnd *top;
            target=pointer_target(&r);
            if(target && target!=desktop && target->queue!=q) {if(target->queue) wh_wake(target->queue->task); return;}
            input_head=(input_head+1)%INPUT_SIZE; input_count--;
            buttons=r.e.buttons;
            input_state=(BYTE)((input_state&~(STATE_LBUTTON|STATE_RBUTTON))|(buttons&1?STATE_LBUTTON:0)|(buttons&2?STATE_RBUTTON:0));
            if(target==desktop && (down&1) && !capture) desktop_click(&r);
            if(!target || target==desktop) continue;
            p.x=r.x; p.y=r.y;
            hit=capture?HTCLIENT:(int)SendMessage(target->handle,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));
            if(!target->used) continue;
            mk=(buttons&1?MK_LBUTTON:0)|(buttons&2?MK_RBUTTON:0)|(input_state&STATE_SHIFT?MK_SHIFT:0)|(input_state&STATE_CONTROL?MK_CONTROL:0);
            top=TopLevel(target);
            if(down && !capture) {
                /* A disabled window passes the click to its last active popup. */
                if(!Enabled(target) || !Enabled(top)) {
                    Wnd *popup=WndFromHandle(GetLastActivePopup(top->handle));
                    if(popup && popup!=top) {Activate(popup,WA_CLICKACTIVE); MessageBeep(0);}
                    continue;
                }
                if(top!=active) {
                    LRESULT a=SendMessage(target->handle,WM_MOUSEACTIVATE,(WPARAM)top->handle,MAKELPARAM(hit,down&1?WM_LBUTTONDOWN:WM_RBUTTONDOWN));
                    if(a!=MA_NOACTIVATE) Activate(top,WA_CLICKACTIVE);
                    if(a==MA_ACTIVATEANDEAT || !target->used) continue;
                }
            }
            if(!capture && (r.e.dx || r.e.dy || down)) {
                SendMessage(target->handle,WM_SETCURSOR,(WPARAM)target->handle,MAKELPARAM(hit,down&1?WM_LBUTTONDOWN:down&2?WM_RBUTTONDOWN:WM_MOUSEMOVE));
                if(!target->used) continue;
            }
            if(r.e.dx || r.e.dy) post_pointer(q,target,WM_MOUSEMOVE,hit,p,mk);
            if(down&1) post_pointer(q,target,double_click(target,WM_LBUTTONDOWN,p,hit!=HTCLIENT && !capture,(DWORD)r.e.time_ms)?WM_LBUTTONDBLCLK:WM_LBUTTONDOWN,hit,p,mk);
            if(up&1) post_pointer(q,target,WM_LBUTTONUP,hit,p,mk);
            if(down&2) post_pointer(q,target,double_click(target,WM_RBUTTONDOWN,p,hit!=HTCLIENT && !capture,(DWORD)r.e.time_ms)?WM_RBUTTONDBLCLK:WM_RBUTTONDOWN,hit,p,mk);
            if(up&2) post_pointer(q,target,WM_RBUTTONUP,hit,p,mk);
        }
    }
}

/* --- retrieving messages ------------------------------------------------------------------- */
static BOOL matches(const MSG *m,HWND filter,UINT low,UINT high) {
    if(filter && m->hwnd!=filter) {
        Wnd *w=WndFromHandle(m->hwnd);
        while(w && w->handle!=filter) w=w->parent;
        if(!w) return FALSE;
    }
    return (!low && !high) || (m->message>=low && m->message<=high);
}
/* WH_KEYBOARD and WH_MOUSE see keys and the mouse as they are retrieved;
 * TRUE when one discards the message. */
static BOOL input_hooked(const MSG *m,UINT flags) {
    int code=flags&PM_REMOVE?HC_ACTION:HC_NOREMOVE;
    if(m->message==WM_KEYDOWN || m->message==WM_KEYUP || m->message==WM_SYSKEYDOWN || m->message==WM_SYSKEYUP) {
        if(!HookActive(WH_KEYBOARD) || !CallHook(WH_KEYBOARD,code,m->wParam,m->lParam)) return FALSE;
        CallHook(WH_CBT,HCBT_KEYSKIPPED,m->wParam,m->lParam);
        return TRUE;
    }
    if((m->message>=WM_MOUSEFIRST && m->message<=WM_MOUSELAST) || (m->message>=WM_NCMOUSEMOVE && m->message<=WM_NCMOUSEMOVE+9)) {
        MOUSEHOOKSTRUCT h;
        if(!HookActive(WH_MOUSE)) return FALSE;
        h.pt=m->pt; h.hwnd=m->hwnd; h.wHitTestCode=m->message>=WM_MOUSEFIRST?HTCLIENT:(UINT)m->wParam; h.dwExtraInfo=0;
        if(!CallHook(WH_MOUSE,code,m->message,(LPARAM)&h)) return FALSE;
        CallHook(WH_CBT,HCBT_CLICKSKIPPED,m->message,(LPARAM)&h);
        return TRUE;
    }
    return FALSE;
}
/* A message is retrieved: WH_GETMESSAGE sees it, and may change it. */
static BOOL got(Queue *q,LPMSG msg,UINT flags) {
    q->last=*msg;
    CallHook(WH_GETMESSAGE,HC_ACTION,flags&PM_REMOVE,(LPARAM)msg);
    return TRUE;
}
static void internal(const MSG *m) {
    if(m->message==UM_ACTIVATE) {Wnd *w=WndFromHandle((HWND)m->wParam); if(w) Activate(w,(int)m->lParam);}
}
static BOOL peek(Queue *q,LPMSG msg,HWND filter,UINT low,UINT high,UINT flags) {
    int i; Wnd *w; Timer *t;
    for(;;) {
        ReceiveSent(q);
        PollInput();
        process_input(q);
        for(i=0;i<q->count;i++) {
            QMsg *m=&q->ring[(q->head+i)%QUEUE_SIZE];
            if(m->msg.message>=UM_FIRST && m->msg.message<=UM_LAST) {MSG copy=m->msg; remove_at(q,i); internal(&copy); break;}
            if(!matches(&m->msg,filter,low,high)) continue;
            *msg=m->msg;
            /* WH_KEYBOARD's hooks see the shift keys that came with the key. */
            retrieved_state(m->state);
            if(input_hooked(msg,flags)) {remove_at(q,i); i--; continue;}
            if(msg->message==WM_KEYDOWN || msg->message==WM_SYSKEYDOWN) {
                q->key_time=msg->time; q->key_vk=msg->wParam; q->key_ch=m->ch; q->key_sys=msg->message==WM_SYSKEYDOWN;
            }
            if(flags&PM_REMOVE) {
                remove_at(q,i);
                if(msg->message==WM_KEYDOWN || msg->message==WM_SYSKEYDOWN) key_state[msg->wParam&0xff]|=0x80;
                if(msg->message==WM_KEYUP || msg->message==WM_SYSKEYUP) key_state[msg->wParam&0xff]&=0x7f;
            }
            return got(q,msg,flags);
        }
        if(i<q->count) continue; /* an internal message was handled */
        break;
    }
    if(q->quit && !filter) {
        fill_msg(msg,NULL,WM_QUIT,(WPARAM)q->quit_code,0);
        if(flags&PM_REMOVE) q->quit=FALSE;
        return got(q,msg,flags);
    }
    if(((!low && !high) || (low<=WM_PAINT && high>=WM_PAINT)) && (w=PaintWindow(q,filter))!=NULL) {
        /* A minimized window with a class icon gets WM_PAINTICON: DefWindowProc draws the icon. */
        fill_msg(msg,w->handle,(w->style&WS_MINIMIZE) && w->cls->wc.hIcon?WM_PAINTICON:WM_PAINT,0,0);
        return got(q,msg,flags);
    }
    while((t=due_timer(q,NULL,TRUE))!=NULL) {
        t->due=now()+t->interval;
        if(caret.wnd && caret.wnd->handle==t->hwnd && !caret.hidden) caret_invert();
    }
    if(((!low && !high) || (low<=WM_TIMER && high>=WM_TIMER)) && (t=due_timer(q,filter,FALSE))!=NULL) {
        fill_msg(msg,t->hwnd,WM_TIMER,t->id,(LPARAM)t->proc);
        if(flags&PM_REMOVE) t->due=now()+t->interval;
        return got(q,msg,flags);
    }
    return FALSE;
}
BOOL WINAPI GetMessage(LPMSG msg,HWND filter,UINT low,UINT high) {
    Queue *q=CurrentQueue();
    if(!q || !msg) return -1;
    for(;;) {
        if(peek(q,msg,filter,low,high,PM_REMOVE)) return msg->message!=WM_QUIT;
        PaintDesktop(); GdiFlush();
        if(!q->sent) wh_block();
    }
}
BOOL WINAPI PeekMessage(LPMSG msg,HWND filter,UINT low,UINT high,UINT flags) {
    Queue *q=CurrentQueue();
    if(!q || !msg) return FALSE;
    if(peek(q,msg,filter,low,high,flags)) return TRUE;
    PaintDesktop(); GdiFlush();
    if(!(flags&PM_NOYIELD)) wh_yield();
    return FALSE;
}
void WINAPI WaitMessage(void) {
    Queue *q=CurrentQueue(); MSG m;
    if(!q || peek(q,&m,NULL,0,0,PM_NOREMOVE)) return;
    PaintDesktop(); GdiFlush();
    if(!q->sent) wh_block();
}
BOOL WINAPI TranslateMessage(const MSG FAR *msg) {
    Queue *q=CurrentQueue();
    if(!q || !msg || (msg->message!=WM_KEYDOWN && msg->message!=WM_SYSKEYDOWN)) return FALSE;
    if(!q->key_ch || msg->time!=q->key_time || msg->wParam!=q->key_vk) return FALSE;
    push_front(q,msg->hwnd,q->key_sys?WM_SYSCHAR:WM_CHAR,q->key_ch,msg->lParam);
    q->key_ch=0;
    return TRUE;
}
LRESULT WINAPI DispatchMessage(const MSG FAR *msg) {
    Wnd *w;
    if(!msg) return 0;
    if(msg->message==WM_TIMER && msg->lParam) {((TIMERPROC)msg->lParam)(msg->hwnd,WM_TIMER,(UINT_PTR)msg->wParam,msg->time); return 0;}
    if(!(w=WndFromHandle(msg->hwnd)) || !w->proc) return 0;
    if(msg->message==WM_PAINT || msg->message==WM_PAINTICON) {
        LRESULT r;
        w->validated=FALSE;
        r=CallProc(w->proc,msg->hwnd,msg->message,msg->wParam,msg->lParam);
        /* A window that did not validate itself would be painted forever;
         * one that did and was invalidated again while painting paints again. */
        if(w->used && !w->validated && (!IsRectEmpty(&w->update) || w->ncpaint)) {w->ncpaint=FALSE; SetRectEmpty(&w->update);}
        return r;
    }
    return CallProc(w->proc,msg->hwnd,msg->message,msg->wParam,msg->lParam);
}

/* WINHOST calls this when every task is blocked. */
static wh_u32 idle(void) {
    wh_u32 timers,playing;
    PollInput(); PaintDesktop(); GdiFlush();
    timers=timer_wait(); playing=playback_wait();
    return playing<timers?playing:timers;
}
static void CALLBACK task_signal(HTASK task,WORD signal) {
    Queue *q=QueueOfTask(task); void **slots;
    if(signal!=SG_EXIT || !q) return;
    HooksTaskEnded(q);
    CommTaskEnded(task);
    DestroyTaskWindows(q);
    TimersOfQueue(q);
    answer_all(q);
    if(caret.wnd && caret.wnd->queue==q) caret.wnd=NULL;
    /* What the task made goes with it: its classes, icons, menus and GDI objects. */
    if(q->instance) UnregisterTaskClasses(q->instance);
    ResourceTaskEnded(q); MenuTaskEnded(q);
    GdiTaskEnded(task);
    q->used=FALSE;
    slots=wh_task_slots(GetTaskId(task));
    if(slots) slots[1]=NULL;
}
void MsgInit(void) {
    memset(queues,0,sizeof(queues)); memset(timers,0,sizeof(timers)); memset(key_state,0,sizeof(key_state)); input_state=0;
    input_head=input_count=0; click.hwnd=NULL; queued_buttons=0;
    cursor_x=screen_width/2; cursor_y=screen_height/2; buttons=0; cursor_shown=0; cursor_shape=NULL;
    GdiMoveCursor(cursor_x,cursor_y,TRUE);
    wh_set_idle(idle);
    SetTaskSignalProc(NULL,task_signal);
}
