/* SPDX-License-Identifier: GPL-2.0-or-later
 * WIN.COM: starts the Windows layer.
 *   WIN [/M:n] [:] [program [arguments]]
 * Without a program, runs the shell= of %WINDIR%\SYSTEM.INI [boot]
 * (PROGMAN.EXE by default). Claims the screen (mode n, else SYSTEM.INI's
 * [display] resolution=, else 800x600),
 * shows the start-up screen (not with :), loads USER.DLL with KERNEL.DLL
 * and GDI.DLL from %WINDIR%\SYSTEM (WINDIR defaults to C:\WINDOWS), starts the
 * program with KERNEL's WinExec and runs tasks until the last one ends. The
 * built-in WINHOST.DLL module gives those DLLs the services in winhost.h.
 */
#include "env.h"
#include "fiber.h"
#include "winhost.h"
#include "splash.h"
#define TASKS 32
#define STACK_BYTES (256*1024)
#define BACKING_BYTES (128*1024)
_Static_assert(WH_EVENT_KEY==IO_EVENT_KEY && WH_EVENT_POINTER==IO_EVENT_POINTER,"event types");
_Static_assert((int)WH_SCAN_PAUSE==(int)IO_SCAN_PAUSE && WH_MOD_LOGO==IO_MOD_LOGO,"scan codes");
_Static_assert(sizeof(WhRegs)==sizeof(DosRegs),"INT 21h registers");

enum {TASK_FREE,TASK_RUNNABLE,TASK_BLOCKED,TASK_DEAD};
typedef struct {
    u32 id,state,dos_task; int wake_pending,result;
    FiberContext context; void *stack,*backing;
    int (*entry)(void *); void *arg;
    void *slots[WH_TASK_SLOTS];
} Task;
static Task tasks[TASKS];
static Task *current;
static FiberContext scheduler;
static u32 next_task=1,host_pid;
static wh_u32 (*idle_hook)(void);
static EFI_BOOT_SERVICES *boot;
static const IoServices *io;
static PeLoader loader;
static char windows_dir[DOS_PATH_MAX],system_dir[DOS_PATH_MAX];
static u64 display_token;
static u32 display_mode,display_epoch;
/* A DOS program a task asked for, run by the scheduler on a fiber of its
 * own, in the task's DOS context, so that it can be set aside: NONE (free),
 * START (asked for), RUN (on the screen), AWAY (set aside by a switching
 * key), BACK (to go on), DONE (its fiber finished) and ENDED (the result
 * for its task to take). The image IO.SYS attributes actions to, and the
 * console's text, are kept while it is away. Several can be away at once. */
enum {SESSION_NONE,SESSION_START,SESSION_RUN,SESSION_AWAY,SESSION_BACK,SESSION_DONE,SESSION_ENDED};
#define SESSIONS 8
#define SESSION_STACK (1024*1024)
#define SESSION_BACKING (512*1024)
typedef struct {
    const char *path,*tail,*dir; int result,state; u32 task,flags;
    FiberContext context; void *stack,*backing,*image; int console_kept; u8 console[IO_CONSOLE_SAVE_BYTES];
    DosSwitchHook outer; /* the switch hook before the session's */
} Session;
static Session sessions[SESSIONS];
static void *host_image;
static int session_task(u32 id) {
    for(unsigned i=0;i<SESSIONS;i++) if(sessions[i].state!=SESSION_NONE && sessions[i].task==id) return 1;
    return 0;
}

/* --- tasks ---------------------------------------------------------------- */
static void switch_fiber(FiberContext *from,const FiberContext *to) {
    EFI_TPL old=boot->RaiseTPL(TPL_HIGH_LEVEL);
    fiber_switch(from,to);
    boot->RestoreTPL(old);
}
static Task *find_task(wh_u32 id) {
    for(unsigned i=0;i<TASKS;i++) if(id && tasks[i].id==id && tasks[i].state!=TASK_FREE) return &tasks[i];
    return NULL;
}
static void end_task(int code) {
    current->result=code; current->state=TASK_DEAD;
    for(;;) switch_fiber(&current->context,&scheduler);
}
/* A new fiber starts where switch_fiber left the TPL raised: lower it, as
 * the switch back into an older fiber does, or timer events never run. */
static void task_main(void *arg) {
    Task *t=arg;
    boot->RestoreTPL(TPL_APPLICATION);
    end_task(t->entry(t->arg));
}
/* DOS contexts are children of WIN.COM's, so ending one task never reaps another. */
static int in_host_dos(int (*work)(void *),void *arg) {
    if(current && app_dos->task_select(host_pid)) return DE_ACCESS;
    int e=work(arg);
    if(current) app_dos->task_select(current->dos_task);
    return e;
}
static int create_dos_task(void *out) {return app_dos->task_create(out);}
wh_u32 wh_task_create(int (*entry)(void *),void *arg) {
    Task *t=NULL;
    for(unsigned i=0;i<TASKS && !t;i++) if(tasks[i].state==TASK_FREE) t=&tasks[i];
    if(!t || !entry) return 0;
    memset(t,0,sizeof(*t));
    /* The stack is the program's: below 2 GiB, as its pointers into it must be. */
    if(host_low_pages(STACK_BYTES/4096,&t->stack)) return 0;
    if(io->alloc_pages(io->context,BACKING_BYTES/4096,&t->backing)) {host_low_free(t->stack,STACK_BYTES/4096); return 0;}
    if(fiber_prepare(&t->context,t->stack,STACK_BYTES,t->backing,BACKING_BYTES,task_main,t) ||
       in_host_dos(create_dos_task,&t->dos_task)) {
        host_low_free(t->stack,STACK_BYTES/4096); io->free_pages(io->context,t->backing,BACKING_BYTES/4096);
        memset(t,0,sizeof(*t)); return 0;
    }
    t->entry=entry; t->arg=arg; t->id=next_task++; t->state=TASK_RUNNABLE;
    return t->id;
}
void wh_task_exit(int code) {if(current) end_task(code);}
int wh_task_kill(wh_u32 id) {
    Task *t=find_task(id);
    /* A DOS program's task stays while the program does: it ends there. */
    if(!t || t==current || session_task(id)) return DE_ACCESS;
    t->state=TASK_DEAD;
    return 0;
}
wh_u32 wh_task_current(void) {return current?current->id:0;}
void **wh_task_slots(wh_u32 id) {
    Task *t=id?find_task(id):current;
    return t?t->slots:NULL;
}
/* The firmware's clock only advances while it waits, so a task that polls
 * waits a moment when no other task could run. */
void wh_yield(void) {
    if(!current) return;
    int others=0; for(unsigned i=0;i<TASKS;i++) if(&tasks[i]!=current && tasks[i].state==TASK_RUNNABLE) others=1;
    if(!others) io->wait(io->context,1);
    switch_fiber(&current->context,&scheduler);
}
void wh_block(void) {
    if(!current) return;
    if(current->wake_pending) {current->wake_pending=0; return;}
    current->state=TASK_BLOCKED;
    switch_fiber(&current->context,&scheduler);
}
void wh_wake(wh_u32 id) {
    Task *t=find_task(id); if(!t) return;
    if(t->state==TASK_BLOCKED) t->state=TASK_RUNNABLE;
    else if(t->state==TASK_RUNNABLE) t->wake_pending=1;
}
void wh_set_idle(wh_u32 (*idle)(void)) {idle_hook=idle;}
static void reap(Task *t) {
    app_dos->task_destroy(t->dos_task);
    host_low_free(t->stack,STACK_BYTES/4096);
    io->free_pages(io->context,t->backing,BACKING_BYTES/4096);
    memset(t,0,sizeof(*t));
}
static int can_switch(void) {return io->size>=offsetof(IoServices,console_restore)+sizeof(io->console_restore) && app_dos->size>=offsetof(DosApi,switch_away)+sizeof(app_dos->switch_away);}
static int session_switch(void *context,u32 key);
static void session_main(void *arg) {
    Session *s=arg; char cwd[DOS_PATH_MAX]; unsigned drive=dos_current_drive(),to=drive; int moved=0,hooked=can_switch();
    boot->RestoreTPL(TPL_APPLICATION);
    /* MSDOS.SYS's switch hook for this DOS context, which the program inherits. */
    if(hooked) {DosSwitchHook mine={session_switch,s}; app_dos->switch_hook(&mine,&s->outer);}
    /* The start-up directory; the drive and its directory as they were after. */
    if(s->dir && s->dir[0]) {
        if(s->dir[1]==':') to=(unsigned)(upper(s->dir[0])-'A');
        if(!dos_drive_cwd(to,cwd) && (to==drive || !dos_select_drive(to))) {
            moved=1;
            if(s->dir[1]!=':' || s->dir[2]) dos_chdir(s->dir);
        }
    }
    s->result=dos_exec(s->path,s->tail);
    if(s->flags&WH_DOS_PAUSE) {
        DosRegs key={.ax=0x0800};
        print("\r\nPress any key to return to Interface Manager.");
        dos_call(&key);
        print("\r\n");
    }
    if(moved) {
        char back[DOS_PATH_MAX+2]={(char)('A'+to),':'};
        strappend(back,sizeof(back),cwd); dos_chdir(back);
        if(to!=drive) dos_select_drive(drive);
    }
    if(hooked) app_dos->switch_hook(&s->outer,NULL);
    s->state=SESSION_DONE;
    for(;;) switch_fiber(&s->context,&scheduler);
}
/* MSDOS.SYS's switch hook, on the session's fiber: a switching key the
 * program has not reserved (its PIF) sets it aside, its text screen kept,
 * until its task asks for it again. */
static int session_switch(void *context,u32 key) {
    static const u32 reserved[]={0,WH_DOS_KEEP_ALT_TAB,WH_DOS_KEEP_ALT_ESC,WH_DOS_KEEP_CTRL_ESC};
    Session *s=context; u32 which=key&~DOS_SWITCH_QUERY;
    if(s->state!=SESSION_RUN || (s->flags&WH_DOS_NOSWITCH) || which>=sizeof(reserved)/sizeof(*reserved) || (s->flags&reserved[which])) return 0;
    if(key&DOS_SWITCH_QUERY) return 1;
    s->console_kept=io->size>offsetof(IoServices,console_save) && io->console_save &&
                    !io->console_save(io->context,s->console,sizeof(s->console));
    s->state=SESSION_AWAY;
    switch_fiber(&s->context,&scheduler);
    return 1;
}
/* Pages above the first MiB, the conventional memory a 16-bit program's
 * VDM takes over while it runs. */
static int alloc_high(u32 pages,void **out) {
    void *low[8]; int n=0,e;
    for(;;) {
        e=io->alloc_pages(io->context,pages,out);
        if(e || (uintptr_t)*out>=0x100000 || n==8) break;
        low[n++]=*out;
    }
    if(!e && (uintptr_t)*out<0x100000) {io->free_pages(io->context,*out,pages); *out=NULL; e=DE_NOMEM;}
    while(n) io->free_pages(io->context,low[--n],pages);
    return e;
}
static void free_session(Session *s) {
    if(s->stack) io->free_pages(io->context,s->stack,SESSION_STACK/4096);
    if(s->backing) io->free_pages(io->context,s->backing,SESSION_BACKING/4096);
    s->stack=s->backing=NULL;
}
/* The session started or taken up again, on the scheduler's stack; back
 * here when it ends or is set aside. It starts in its task's DOS context;
 * MSDOS.SYS selects the program's again when its switch hook returns. A DOS
 * program runs with the screen given back, as in Windows 3.0's standard
 * mode: no task runs while it is on the screen. When it ends, or is set
 * aside, the display is claimed again and GDI sends its whole copy of the
 * screen (display_epoch). */
static void run_session(Session *s) {
    if(s->state==SESSION_START) {
        Task *t=find_task(s->task); int e=t?0:DE_ACCESS;
        if(!e) e=alloc_high(SESSION_STACK/4096,&s->stack);
        if(!e) e=alloc_high(SESSION_BACKING/4096,&s->backing);
        if(!e) e=fiber_prepare(&s->context,s->stack,SESSION_STACK,s->backing,SESSION_BACKING,session_main,s)?DE_NOMEM:0;
        if(!e) e=app_dos->task_select(t->dos_task);
        if(e) {free_session(s); s->result=e; s->state=SESSION_ENDED; wh_wake(s->task); return;}
        io->display_release(io->context,display_token);
    } else {
        io->display_release(io->context,display_token);
        if(s->console_kept) io->console_restore(io->context,s->console,sizeof(s->console));
        io->image_switch(io->context,s->image);
    }
    s->state=SESSION_RUN;
    switch_fiber(&scheduler,&s->context);
    if(s->state==SESSION_AWAY) s->image=io->image_switch(io->context,host_image);
    else {free_session(s); s->state=SESSION_ENDED;}
    app_dos->task_select(host_pid);
    int e=io->display_claim(io->context,display_mode,&display_token);
    if(e) print("WIN: cannot claim the display again (%s)\n",dos_error(e));
    display_epoch++;
    wh_wake(s->task);
}
/* The task waits while the program is on the screen. */
static int session_wait(Session *s) {
    while(s->state!=SESSION_ENDED && s->state!=SESSION_AWAY) wh_block();
    if(s->state==SESSION_AWAY) return WH_DOS_AWAY;
    s->state=SESSION_NONE;
    return s->result;
}
int wh_dos_session(const char *path,const char *tail) {return wh_dos_session_ex(path,tail,NULL,0);}
int wh_dos_session_ex(const char *path,const char *tail,const char *dir,wh_u32 flags) {
    Session *s=NULL;
    if(!current || session_task(current->id)) return DE_ACCESS;
    for(unsigned i=0;i<SESSIONS && !s;i++) if(sessions[i].state==SESSION_NONE) s=&sessions[i];
    if(!s) return DE_NOMEM;
    s->path=path; s->tail=tail?tail:""; s->dir=dir; s->flags=flags;
    s->task=current->id; s->result=0; s->console_kept=0; s->state=SESSION_START;
    return session_wait(s);
}
int wh_dos_resume(void) {
    for(unsigned i=0;current && i<SESSIONS;i++) {
        Session *s=&sessions[i];
        if(s->state==SESSION_AWAY && s->task==current->id) {s->state=SESSION_BACK; return session_wait(s);}
    }
    return DE_ACCESS;
}
wh_u32 wh_display_epoch(void) {return display_epoch;}
_Static_assert(sizeof(WhIa32)==sizeof(IoIa32Context) && offsetof(WhIa32,cflg)==offsetof(IoIa32Context,cflg) &&
               offsetof(WhIa32,fault_ip)==offsetof(IoIa32Context,fault_ip) && WH_IA32_INTERRUPT==IO_IA32_INTERRUPT,
               "WhIa32 follows IoIa32Context");
int wh_ia32_run(WhIa32 *context) {
    if(io->size<offsetof(IoServices,ia32_run)+sizeof(io->ia32_run) || !(io->capabilities&IO_CAP_IA32)) return DE_FUNCTION;
    if(!context) return 0;
    return io->ia32_run(io->context,(IoIa32Context *)context);
}
/* Round robin over runnable tasks; when all are blocked, the idle hook and
 * then a wait for input or the hook's deadline. Ends with the last task. */
static void run(void) {
    for(unsigned next=0;;) {
        int ran=0,live=0;
        for(unsigned i=0;i<SESSIONS;i++) if(sessions[i].state==SESSION_START || sessions[i].state==SESSION_BACK) run_session(&sessions[i]);
        for(unsigned k=0;k<TASKS;k++) {
            Task *t=&tasks[(next+k)%TASKS];
            if(t->state==TASK_RUNNABLE) {
                current=t;
                if(!app_dos->task_select(t->dos_task)) switch_fiber(&scheduler,&t->context);
                else t->state=TASK_DEAD;
                app_dos->task_select(host_pid); current=NULL; ran=1;
                next=(unsigned)(t-tasks)+1;
            }
            if(t->state==TASK_DEAD) reap(t);
            if(t->state!=TASK_FREE) live++;
        }
        if(!live) return;
        if(ran) continue;
        wh_u32 wait=idle_hook?idle_hook():WH_FOREVER;
        int runnable=0; for(unsigned i=0;i<TASKS;i++) if(tasks[i].state==TASK_RUNNABLE) runnable=1;
        if(!runnable) {
            /* DOS's PRINT prints while nothing runs, as at DOS's idle. */
            u32 print_ms=app_dos->size>=offsetof(DosApi,idle)+sizeof(app_dos->idle) && app_dos->idle?app_dos->idle():0;
            if(print_ms) wait=MIN(wait,print_ms);
            io->wait(io->context,MIN(wait,1000U));
        }
    }
}

/* --- other services ------------------------------------------------------- */
wh_u64 wh_ticks(void) {return io->ticks_ms(io->context);}
/* Memory for programs and their modules is below 2 GiB (host_low_pages). */
void *wh_alloc(wh_u64 bytes) {
    void *p; u32 pages=(u32)((bytes+4095)/4096);
    if(!bytes || host_low_pages(pages,&p)) return NULL;
    memset(p,0,(size_t)pages*4096); host_pages_out+=pages; return p;
}
/* For what IA-32 code addresses (WOW's segments): below 4 GiB, as all is. */
void *wh_alloc_low(wh_u64 bytes) {return wh_alloc(bytes);}
void wh_free(void *memory,wh_u64 bytes) {
    if(!memory) return;
    u32 pages=(u32)((bytes+4095)/4096);
    host_low_free(memory,pages); host_pages_out-=pages;
}
void wh_trace(const char *text) {print("%s\n",text);}
void wh_int21(WhRegs *regs) {app_dos->int21((DosRegs *)regs);}
int wh_display_size(wh_u32 *width,wh_u32 *height) {
    IoDisplay d; int e=io->display_info(io->context,&d); if(e) return e;
    *width=d.width; *height=d.height; return 0;
}
int wh_display_blt(const void *pixels,wh_u32 x,wh_u32 y,wh_u32 width,wh_u32 height,wh_u32 stride) {
    return io->display_blt(io->context,(IoPixel *)(uintptr_t)pixels,x,y,width,height,stride,0);
}
int wh_poll_event(WhEvent *event) {
    IoEvent e; int r=io->poll_event(io->context,&e); if(r) return r;
    *event=(WhEvent){e.type,e.unicode,e.scan,e.buttons,e.flags,e.modifiers,e.dx,e.dy,e.dz,0,e.time_ms};
    return 0;
}
static int is_id(const char *name) {return (uintptr_t)name<0x10000;}
void *wh_load_library(const char *name) {
    PeModule *m; if(is_id(name) || pe_load(&loader,name,NULL,&m)) return NULL;
    return m->base;
}
int wh_free_library(void *module) {
    PeModule *m=pe_from_base(&loader,module); return m?pe_free(&loader,m):DE_HANDLE;
}
void *wh_module_handle(const char *name) {
    PeModule *m=is_id(name)?NULL:pe_find(&loader,name);
    return m && !(m->flags&PE_FLAG_BUILTIN)?m->base:NULL;
}
int wh_module_file_name(void *module,char *buffer,wh_u32 size) {
    PeModule *m=pe_from_base(&loader,module); if(!m || !size) return DE_HANDLE;
    buffer[0]=0;
    if(strappend(buffer,size,m->dir) || strappend(buffer,size,m->name)) return DE_PATH;
    return 0;
}
const void *wh_proc_address(void *module,const char *name) {
    PeModule *m=pe_from_base(&loader,module); if(!m) return NULL;
    return is_id(name)?pe_proc_ordinal(&loader,m,(u32)(uintptr_t)name):pe_proc(&loader,m,name);
}
/* A resource type or name: an integer ID, "#n" for one, or a string. */
static PeResId res_id(const char *s) {
    PeResId r=is_id(s)?(PeResId){(u32)(uintptr_t)s,NULL}:(PeResId){0,s};
    if(r.name && r.name[0]=='#') {r.id=0; for(const char *p=r.name+1;*p>='0' && *p<='9';p++) r.id=r.id*10+(u32)(*p-'0'); r.name=NULL;}
    return r;
}
const void *wh_find_resource(void *module,const char *type,const char *name) {
    PeModule *m=pe_from_base(&loader,module);
    if(!m) return NULL;
    return pe_resource_entry(m,res_id(type),res_id(name),0);
}
/* A name without a directory is looked for in the current directory, then
 * in the Windows directory; ".EXE" is added when there is no extension. */
void *wh_load_application(const char *path) {
    char file[DOS_PATH_MAX]; PeModule *m;
    if(is_id(path) || strcopy(file,sizeof(file),path)) return NULL;
    const char *base=file; for(const char *p=file;*p;p++) if(*p=='\\' || *p==':') base=p+1;
    if(!strchr(base,'.') && strappend(file,sizeof(file),".EXE")) return NULL;
    int e=pe_load_exe(&loader,file,&m);
    if(e==DE_NOFILE && base==file) {
        char other[DOS_PATH_MAX]; strcopy(other,sizeof(other),windows_dir);
        if(!strappend(other,sizeof(other),"\\") && !strappend(other,sizeof(other),file)) e=pe_load_exe(&loader,other,&m);
    }
    if(e) {print("WIN: cannot load %s (%s) at %s %s\n",file,dos_error(e),loader.error_module,loader.error_symbol); return NULL;}
    return m->base;
}
const void *wh_entry(void *module) {
    PeModule *m=pe_from_base(&loader,module); return m?m->entry:NULL;
}
typedef struct {const char *name; char *out; u32 size; u32 index;} EnvQuery;
static int env_get_work(void *arg) {EnvQuery *q=arg; return dos_env_get(q->name,q->out,q->size);}
static int env_list_work(void *arg) {EnvQuery *q=arg; return dos_env_list(q->index,q->out,q->size);}
int wh_env_get(const char *name,char *out,wh_u32 size) {EnvQuery q={name,out,size,0}; return in_host_dos(env_get_work,&q);}
int wh_env_list(wh_u32 index,char *out,wh_u32 size) {EnvQuery q={NULL,out,size,index}; return in_host_dos(env_list_work,&q);}
#define EXPORT(f) {#f,0,(const void *)f}
static const PeExport host_exports[]={
    EXPORT(wh_task_create),EXPORT(wh_task_exit),EXPORT(wh_task_kill),EXPORT(wh_task_current),EXPORT(wh_task_slots),
    EXPORT(wh_yield),EXPORT(wh_block),EXPORT(wh_wake),EXPORT(wh_set_idle),
    EXPORT(wh_ticks),EXPORT(wh_alloc),EXPORT(wh_alloc_low),EXPORT(wh_free),EXPORT(wh_trace),EXPORT(wh_int21),
    EXPORT(wh_display_size),EXPORT(wh_display_blt),EXPORT(wh_poll_event),
    EXPORT(wh_load_library),EXPORT(wh_free_library),EXPORT(wh_module_handle),EXPORT(wh_module_file_name),
    EXPORT(wh_proc_address),EXPORT(wh_find_resource),EXPORT(wh_load_application),EXPORT(wh_entry),
    EXPORT(wh_env_get),EXPORT(wh_env_list),EXPORT(wh_dos_session),EXPORT(wh_dos_session_ex),EXPORT(wh_dos_resume),EXPORT(wh_display_epoch),EXPORT(wh_ia32_run),
};
static const char *const aliases[][2]={
    {"KERNEL32","KERNEL"},{"KRNL386.EXE","KERNEL"},{"KRNL286.EXE","KERNEL"},{"KERNEL.EXE","KERNEL"},
    {"USER32","USER"},{"USER.EXE","USER"},{"GDI32","GDI"},{"GDI.EXE","GDI"},
};

/* --- start-up ------------------------------------------------------------- */
static int same_prefix(const char *a,const char *b,size_t n) {
    for(;n;n--,a++,b++) if(upper(*a)!=upper(*b)) return 0;
    return 1;
}
/* A value of %WINDIR%\SYSTEM.INI: key= in [section]; FALSE without one. */
static int system_ini(const char *section,const char *key,char *out,size_t size) {
    char path[DOS_PATH_MAX],text[2048]; unsigned h; u32 n=0; int in=0,found=0; size_t k=strlen(key);
    strcopy(path,sizeof(path),windows_dir); strappend(path,sizeof(path),"\\SYSTEM.INI");
    if(dos_open(path,DOS_OPEN_READ,0,&h)) return 0;
    if(dos_read(h,text,sizeof(text)-1,&n)) n=0;
    dos_close(h); text[n]=0;
    for(char *line=text;*line;) {
        char *end=line; while(*end && *end!='\n' && *end!='\r') end++;
        char saved=*end; *end=0;
        while(*line==' ' || *line=='\t') line++;
        if(*line=='[') {
            char *close=line+1; while(*close && *close!=']') close++;
            in=*close && (size_t)(close-line-1)==strlen(section) && same_prefix(line+1,section,strlen(section));
        } else if(in && same_prefix(line,key,k)) {
            char *v=line+k; while(*v==' ') v++;
            if(*v=='=') {v++; while(*v==' ') v++; if(*v) {strcopy(out,size,v); found=1;}}
        }
        *end=saved; line=*end?end+1:end;
    }
    return found;
}
static void default_shell(char *out,size_t size) {
    if(!system_ini("boot","shell",out,size)) strcopy(out,size,"PROGMAN.EXE");
}
/* The mode to claim: n from /M:n, else the largest within SYSTEM.INI's
 * [display] resolution= (Windows Setup's choice, "1024x768"), else
 * 800x600, else the active one (by number, so a DOS session can claim it
 * again). */
static u32 pick_mode(const char *option) {
    IoDisplayMode m; u32 chosen=IO_DISPLAY_KEEP,active=IO_DISPLAY_KEEP,wanted=IO_DISPLAY_KEEP,width=0,height=0;
    char resolution[32];
    if(option) return (u32)(*option-'0');
    if(system_ini("display","resolution",resolution,sizeof(resolution))) {
        const char *p=resolution;
        while(*p>='0' && *p<='9') width=width*10+(u32)(*p++-'0');
        while(*p==' ' || *p=='x' || *p=='X') p++;
        while(*p>='0' && *p<='9') height=height*10+(u32)(*p++-'0');
    }
    u64 best=0;
    for(u32 i=0;!io->display_mode(io->context,i,&m);i++) {
        if(m.width==800 && m.height==600) chosen=i;
        if(width && m.width<=width && m.height<=height && (u64)m.width*m.height>best) {wanted=i; best=(u64)m.width*m.height;}
        if(m.flags&IO_DISPLAY_CURRENT) active=i;
    }
    return wanted!=IO_DISPLAY_KEEP?wanted:chosen!=IO_DISPLAY_KEEP?chosen:active;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    boot=st->BootServices; io=dos_io_services();
    if(!io || io->size<offsetof(IoServices,display_release)+sizeof(io->display_release) || !(io->capabilities&IO_CAP_GRAPHICS)) {
        print("WIN: graphics services are required\n"); return 1;
    }
    char tail[256],*args=tail; const char *mode=NULL;
    strcopy(tail,sizeof(tail),app_dos->command_tail());
    while(*args==' ') args++;
    if((args[0]=='/' || args[0]=='-') && upper(args[1])=='M' && args[2]==':' && args[3]>='0' && args[3]<='9') {
        mode=args+3; args+=4; while(*args==' ') args++;
    }
    /* "WIN :" goes without the start-up screen, as in Windows 3.0. */
    int splash=1;
    if(args[0]==':' && (args[1]==' ' || !args[1])) {splash=0; args++; while(*args==' ') args++;}
    if(dos_env_get("WINDIR",windows_dir,sizeof(windows_dir))) strcopy(windows_dir,sizeof(windows_dir),"C:\\WINDOWS");
    char shell[128];
    if(!*args) {default_shell(shell,sizeof(shell)); args=shell;}
    strcopy(system_dir,sizeof(system_dir),windows_dir); strappend(system_dir,sizeof(system_dir),"\\SYSTEM");
    DosInfo info; dos_query(&info); host_pid=info.pid;
    if(io->size>=offsetof(IoServices,image_switch)+sizeof(io->image_switch)) host_image=io->image_switch(io->context,NULL);
    display_mode=pick_mode(mode);
    int e=io->display_claim(io->context,display_mode,&display_token);
    if(e) {print("WIN: cannot claim the display (%s)\n",dos_error(e)); return 1;}
    if(splash) splash_show(io);
    PeEnv env; host_env(&env,io,system_dir);
    pe_init(&loader,&env);
    e=pe_builtin(&loader,"WINHOST",host_exports,ARRAY_SIZE(host_exports),NULL);
    for(unsigned i=0;i<ARRAY_SIZE(aliases) && !e;i++) e=pe_alias(&loader,aliases[i][0],aliases[i][1]);
    PeModule *user=NULL,*kernel=NULL; int result=1;
    if(!e) e=pe_load(&loader,"USER",NULL,&user);
    if(e) print("WIN: cannot load the system (%s) at %s %s\n",dos_error(e),loader.error_module,loader.error_symbol);
    else if(!(kernel=pe_find(&loader,"KERNEL"))) print("WIN: USER does not use KERNEL\n");
    else {
        const void *exec=pe_proc(&loader,kernel,"WinExec");
        u32 started=exec?((u32 (*)(const char *,u32))(uintptr_t)exec)(args,1):0;
        if(started<32) print("WIN: cannot start %s (%u)\n",args,(unsigned long long)started);
        else {run(); result=0;}
    }
    pe_shutdown(&loader);
    io->display_release(io->context,display_token);
    if(host_pages_out || host_files_out)
        print("WIN: %u pages and %u files leaked\n",(unsigned long long)host_pages_out,(unsigned long long)host_files_out);
    return result;
}
