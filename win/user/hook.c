/* SPDX-License-Identifier: GPL-2.0-or-later
 * Hooks run newest first; CallNextHookEx/DefHookProc continues the chain.
 * They see the installing task's messages unless global (SetWindowsHookEx
 * thread 0; journal hooks are always global). Task exit removes its hooks
 * except global hooks owned by a DLL. A chain cannot reenter for the same
 * task, preventing recursive hooks during message sends or queue reads.
 *
 * WH_JOURNALRECORD receives input as read, with separate key-down/up events
 * and surrounding modifier transitions. WH_JOURNALPLAYBACK replaces input:
 * HC_GETNEXT schedules an event; HC_SKIP advances. Keyboard/mouse input is
 * ignored except Ctrl+Esc/Ctrl+Break, which stop playback.
 */
#include "user.h"
#define KINDS (WH_SHELL-WH_MSGFILTER+1)

typedef struct {
    BOOL used; int id; HOOKPROC proc; Queue *queue,*owner; HINSTANCE module; DWORD order;
} Hook;
static Hook hooks[HOOKS];
static DWORD next_order=1;
static int counts[KINDS];

static int kind(int id) {return id-WH_MSGFILTER;}
static HHOOK hhook(const Hook *h) {return (HHOOK)(ULONG_PTR)(HHOOK_BASE+(h-hooks)*4);}
static Hook *hook_of(HHOOK hh) {
    ULONG_PTR v=(ULONG_PTR)hh;
    if(v<HHOOK_BASE || (v-HHOOK_BASE)%4 || (v-HHOOK_BASE)/4>=HOOKS || !hooks[(v-HHOOK_BASE)/4].used) return NULL;
    return &hooks[(v-HHOOK_BASE)/4];
}
static BOOL applies(const Hook *h,Queue *q) {return !h->queue || h->queue==q;}
/* The next hook of a kind for this task, after the one set at 'below'. */
static Hook *next_hook(int id,DWORD below,Queue *q) {
    Hook *best=NULL; int i;
    for(i=0;i<HOOKS;i++) {
        Hook *h=&hooks[i];
        if(h->used && h->id==id && h->order<below && applies(h,q) && (!best || h->order>best->order)) best=h;
    }
    return best;
}
BOOL HookActive(int id) {return id>=WH_MSGFILTER && id<=WH_SHELL && counts[kind(id)] && next_hook(id,0xffffffffUL,CurrentQueue())!=NULL;}
LRESULT CallHook(int id,int code,WPARAM wp,LPARAM lp) {
    Queue *q; Hook *h; LRESULT r; WORD bit;
    if(id<WH_MSGFILTER || id>WH_SHELL || !counts[kind(id)]) return 0;
    q=CurrentQueue(); bit=(WORD)(1u<<kind(id));
    if(q && (q->hooking&bit)) return 0;
    if(!(h=next_hook(id,0xffffffffUL,q))) return 0;
    if(q) q->hooking|=bit;
    r=CallHookProc(h->proc,id,code,wp,lp);
    if(q) q->hooking&=(WORD)~bit;
    return r;
}
static Hook *add(int id,HOOKPROC proc,Queue *queue,HINSTANCE module) {
    int i;
    if(id<WH_MSGFILTER || id>WH_SHELL || !proc) return NULL;
    if(id==WH_JOURNALRECORD || id==WH_JOURNALPLAYBACK) queue=NULL;
    for(i=0;i<HOOKS;i++) if(!hooks[i].used) {
        Hook *h=&hooks[i];
        h->used=TRUE; h->id=id; h->proc=proc; h->queue=queue; h->owner=CurrentQueue(); h->module=module; h->order=next_order++;
        counts[kind(id)]++;
        return h;
    }
    return NULL;
}
static void drop(Hook *h) {counts[kind(h->id)]--; h->used=FALSE;}
HHOOK WINAPI SetWindowsHookEx(int id,HOOKPROC proc,HINSTANCE module,DWORD thread) {
    Queue *q=NULL; Hook *h;
    if(thread && !(q=QueueOfTask((HTASK)(ULONG_PTR)thread))) return NULL;
    h=add(id,proc,q,module);
    return h?hhook(h):NULL;
}
BOOL WINAPI UnhookWindowsHookEx(HHOOK hh) {
    Hook *h=hook_of(hh);
    if(!h) return FALSE;
    drop(h); return TRUE;
}
LRESULT WINAPI CallNextHookEx(HHOOK hh,int code,WPARAM wp,LPARAM lp) {
    Hook *h=hook_of(hh),*n;
    if(!h || !(n=next_hook(h->id,h->order,CurrentQueue()))) return 0;
    return CallHookProc(n->proc,n->id,code,wp,lp);
}
int HookKind(HHOOK hh) {Hook *h=hook_of(hh); return h?h->id:WH_MSGFILTER-1;}
/* Windows 3.0: the task's hook; what comes back is passed (by address) to
 * DefHookProc. */
HOOKPROC WINAPI SetWindowsHook(int id,HOOKPROC proc) {
    Hook *h=add(id,proc,CurrentQueue(),NULL);
    return h?(HOOKPROC)hhook(h):NULL;
}
BOOL WINAPI UnhookWindowsHook(int id,HOOKPROC proc) {
    Queue *q=CurrentQueue(); int i;
    for(i=0;i<HOOKS;i++) if(hooks[i].used && hooks[i].id==id && hooks[i].proc==proc && (hooks[i].owner==q || !hooks[i].owner)) {drop(&hooks[i]); return TRUE;}
    return FALSE;
}
LRESULT WINAPI DefHookProc(int code,WPARAM wp,LPARAM lp,HOOKPROC FAR *next) {
    return next?CallNextHookEx((HHOOK)*next,code,wp,lp):0;
}
BOOL WINAPI CallMsgFilter(LPMSG msg,int code) {
    if(!msg) return FALSE;
    if(CallHook(WH_SYSMSGFILTER,code,0,(LPARAM)msg)) return TRUE;
    return CallHook(WH_MSGFILTER,code,0,(LPARAM)msg)!=0;
}
void EndPlayback(void) {
    int i;
    for(i=0;i<HOOKS;i++) if(hooks[i].used && hooks[i].id==WH_JOURNALPLAYBACK) drop(&hooks[i]);
}
/* A task's hooks end with it. */
void HooksTaskEnded(Queue *q) {
    int i;
    for(i=0;i<HOOKS;i++) {
        Hook *h=&hooks[i];
        if(h->used && (h->queue==q || (h->owner==q && (!h->module || h->module==q->instance)))) drop(h);
    }
}
void HookInit(void) {memset(hooks,0,sizeof(hooks)); memset(counts,0,sizeof(counts)); next_order=1;}
