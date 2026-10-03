/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: Win16 tasks. Win16Main runs on the KERNEL task's fiber: it
 * loads the program, builds its PSP and registers as Windows 3.0 did
 * (AX 0, BX stack size, CX heap size, DI instance, SI previous instance,
 * ES the PSP, DS the automatic data segment), and runs it. Interruptions
 * are API thunks, INT 21h, and faults; IRET and SMSW (which the processor
 * intercepts) are emulated, CLI and STI (which fault at IOPL 0) are ignored,
 * and other faults end the program.
 */
#include "wow.h"
WORD return_thunk;
#define TASKS 16
static Task16 tasks[TASKS];
static unsigned live; /* Win16 tasks; the shared tables go with the last */
static void release_shared(void) {if(!live) {ThunkShutdown(); LdtShutdown();}}
HINSTANCE wow_instance;
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {(void)reason; (void)reserved; wow_instance=instance; return TRUE;}
/* The running fiber's Win16 task, kept in WOW's task slot. */
Task16 *CurrentTask16(void) {void **slots=wh_task_slots(0); return slots?(Task16 *)slots[3]:NULL;}
/* A Win16 task handle is its PSP selector. */
Task16 *Task16Of(WORD task) {
    unsigned i;
    for(i=0;i<TASKS;i++) if(tasks[i].module && tasks[i].psp==task) return &tasks[i];
    return NULL;
}
WORD Task16Handle(HTASK native) {
    unsigned i;
    for(i=0;i<TASKS;i++) if(tasks[i].module && tasks[i].native==native) return tasks[i].psp;
    return 0;
}

WORD Reg16(Task16 *t,int r) {return (WORD)t->cpu.gr[r];}
void SetReg16(Task16 *t,int r,WORD v) {t->cpu.gr[r]=(t->cpu.gr[r]&~(wh_u64)0xffff)|v;}
/* Segment registers: 0 ES, 1 CS, 2 SS, 3 DS. */
WORD Seg16(Task16 *t,int s) {
    switch(s) {
    case 0: return (WORD)(t->cpu.data_sel>>16);
    case 1: return (WORD)t->cpu.sys_sel;
    case 2: return (WORD)(t->cpu.sys_sel>>16);
    default: return (WORD)t->cpu.data_sel;
    }
}
void SetSeg16(Task16 *t,int s,WORD sel) {
    wh_u64 d=SelDescriptor(sel);
    switch(s) {
    case 0: t->cpu.data_sel=(t->cpu.data_sel&~((wh_u64)0xffff<<16))|(wh_u64)sel<<16; t->cpu.es=d; break;
    case 1: t->cpu.sys_sel=(t->cpu.sys_sel&~(wh_u64)0xffff)|sel; t->cpu.cs=d; break;
    case 2: t->cpu.sys_sel=(t->cpu.sys_sel&~((wh_u64)0xffff<<16))|(wh_u64)sel<<16; t->cpu.ss=d; break;
    default: t->cpu.data_sel=(t->cpu.data_sel&~(wh_u64)0xffff)|sel; t->cpu.ds=d; break;
    }
}
static WORD pop(Task16 *t) {
    WORD sp=Reg16(t,SP),v=Peek16(Seg16(t,2),sp);
    SetReg16(t,SP,(WORD)(sp+2)); return v;
}

void TaskEnd16(Task16 *t,int code) {
    unsigned i;
    /* Libraries first, while their WEPs can still run on this task. */
    for(i=0;i<sizeof(t->loaded)/sizeof(t->loaded[0]);i++) if(t->loaded[i]) {Module16 *l=t->loaded[i]; t->loaded[i]=NULL; NeRelease(l);}
    if(t->module) NeReleaseImports(t->module);
    if(t->scratch) {SelFree(t->scratch_sel); Free16(t->scratch,SCRATCH_BYTES);}
    t->scratch=NULL;
    if(t->environment) {Free16(SelPointer(t->environment,0),SelLimit(t->environment)+1); SelFree(t->environment);}
    t->environment=0;
    GlobalTaskEnded16(t); System16TaskEnded(t);
    if(t->module) {User16TaskEnded(t->module); LocalTaskEnded16(t->module); NeFree(t->module);}
    t->module=NULL;
    if(t->psp) {Free16(SelPointer(t->psp,0),256); SelFree(t->psp);}
    t->psp=0;
    if(CurrentTask16()==t) wh_task_slots(0)[3]=NULL;
    live--; release_shared();
    ExitProcess((UINT)code);
}
void Fault16(Task16 *t,LPCSTR what) {
    char line[120];
    wsprintf(line,"WOW: %s: %s at %04X:%04X",t->module?t->module->name:"?",what,Seg16(t,1),(WORD)t->cpu.eip);
    wh_trace(line);
    MessageBox(NULL,line+5,"Application Error",MB_OK|MB_ICONHAND);
    TaskEnd16(t,0xff);
}

static void dispatch(Task16 *t) {
    WhIa32 *c=&t->cpu;
    WORD cs=Seg16(t,1);
    switch(c->exit) {
    case WH_IA32_INTERRUPT:
        if(c->vector==THUNK_INT && IsThunk(cs)) {ThunkCall(t); return;}
        if(c->vector==0x21) {Int21(t); return;}
        if(c->vector==0x2f) return;
        Fault16(t,"unexpected software interrupt");
        return;
    case WH_IA32_INTERCEPT:
        if(c->vector==2) return; /* MOV SS, POPF */
        if(c->vector==0 && (BYTE)c->iim==0xcf && !(c->code&2)) {
            WORD ip=pop(t),sel=pop(t),flags=pop(t);
            SetSeg16(t,1,sel); c->eip=ip;
            c->eflags=(c->eflags&~(wh_u64)0x0cd5)|(flags&0x0cd5)|2;
            return;
        }
        if(c->vector==0 && (c->iim&0xffff)==0x010f && ((c->iim>>19)&7)==4 && ((c->iim>>22)&3)==3) {
            /* SMSW to a register: the machine status word of CR0 (PE, ET, NE). */
            SetReg16(t,(int)((c->iim>>16)&7),(WORD)c->cflg);
            c->eip=(WORD)(c->eip+((c->code>>12)&15)+3);
            return;
        }
        Fault16(t,"unsupported instruction");
        return;
    default:
        if(c->vector==13) {
            const BYTE *ip=(const BYTE *)SelPointer(cs,(WORD)c->eip);
            if(ip[0]==0xfa || ip[0]==0xfb) {c->eip=(WORD)(c->eip+1); return;}
            Fault16(t,"general protection fault");
        }
        if(c->vector==0) Fault16(t,"divide error");
        Fault16(t,"processor exception");
    }
}
static void run(Task16 *t) {
    if(wh_ia32_run(&t->cpu)) Fault16(t,"no IA-32 instruction set");
}

/* The data segment for code in sel: a library's own, or the task's. */
static WORD data_segment(Task16 *t,WORD sel) {
    Module16 *m=NeFromCode(sel);
    return m && m->library && m->autodata?m->instance:t->module->instance;
}
typedef struct {WORD cx,si,di,ds,es;} Entry16;
static DWORD call16(Task16 *t,DWORD proc,int count,const DWORD *args,const BYTE *sizes,const Entry16 *e) {
    WhIa32 saved=t->cpu;
    WORD sp=Reg16(t,SP),ss=Seg16(t,2); int i; DWORD result;
    BYTE *stack=(BYTE *)SelPointer(ss,0);
    for(i=0;i<count;i++) {
        if(sizes[i]==4) {sp=(WORD)(sp-4); memcpy(stack+sp,&args[i],4);}
        else {WORD v=(WORD)args[i]; sp=(WORD)(sp-2); memcpy(stack+sp,&v,2);}
    }
    sp=(WORD)(sp-4); put32(stack+sp,MAKELONG(0,return_thunk));
    SetReg16(t,SP,sp);
    SetSeg16(t,1,HIWORD(proc)); t->cpu.eip=LOWORD(proc);
    /* Exported procedures load DS from AX (or from SS); both are the
     * automatic data segment here, or a library's for its code. */
    if(e) {
        SetReg16(t,AX,0); SetReg16(t,CX,e->cx); SetReg16(t,SI,e->si); SetReg16(t,DI,e->di);
        SetSeg16(t,3,e->ds); SetSeg16(t,0,e->es);
    } else {WORD ds=data_segment(t,HIWORD(proc)); SetReg16(t,AX,ds); SetSeg16(t,3,ds);}
    t->depth++;
    for(;;) {
        run(t);
        if(t->cpu.exit==WH_IA32_INTERRUPT && t->cpu.vector==THUNK_INT && Seg16(t,1)==return_thunk) break;
        dispatch(t);
    }
    t->depth--;
    result=Reg16(t,AX)|(DWORD)Reg16(t,DX)<<16;
    t->cpu=saved;
    return result;
}
DWORD Call16(Task16 *t,DWORD proc,int count,const DWORD *args,const BYTE *sizes) {return call16(t,proc,count,args,sizes,NULL);}
/* LibEntry: DI the instance, DS its data segment, CX the heap size, ES:SI
 * the command line (none); AX nonzero for success. */
BOOL CallLibEntry16(Task16 *t,Module16 *m) {
    Entry16 e;
    if(!m->cs) return TRUE;
    e.cx=m->heap; e.si=0; e.di=m->instance; e.ds=m->instance; e.es=0;
    return LOWORD(call16(t,(DWORD)m->seg[m->cs-1].sel<<16|m->ip,0,NULL,NULL,&e))!=0;
}
/* WEP(WEP_FREE_DLL). */
void CallWep16(Task16 *t,Module16 *m) {
    static const BYTE size=2; DWORD wep,arg=0;
    if(NeEntry(m,NeOrdinal(m,"WEP"),&wep)) call16(t,wep,1,&arg,&size,NULL);
}

static WORD make_psp(Task16 *t) {
    BYTE *p=(BYTE *)Alloc16(256); WORD sel; unsigned n;
    if(!p) return 0;
    sel=SelAlloc(SelLinear(p),255,SEL_DATA);
    if(!sel) {Free16(p,256); return 0;}
    p[0]=0xcd; p[1]=0x20;
    n=(unsigned)lstrlen(t->command); if(n>125) n=125;
    /* The tail ends with a 0, not DOS's CR: InitTask gives it to WinMain
     * as lpCmdLine. */
    p[0x80]=(BYTE)(n+(n?1:0));
    if(n) {p[0x81]=' '; memcpy(p+0x82,t->command,n);}
    p[0x81+p[0x80]]=0;
    return sel;
}

/* Exported to KERNEL: runs path as a Win16 program on the current task. */
int WINAPI Win16Main(LPCSTR path,LPCSTR command,int show) {
    Task16 *t=NULL; Module16 *m; int e; unsigned i;
    WhIa32 *c;
    for(i=0;i<TASKS && !t;i++) if(!tasks[i].module) t=&tasks[i];
    if(!t) return 8;
    if(wh_ia32_run(NULL)) return WOW_NO_IA32;
    if(!LdtInit() || !ThunkInit()) {release_shared(); return 8;}
    memset(t,0,sizeof(*t));
    e=NeLoad(path,&m);
    if(e) {release_shared(); return e;}
    t->module=m; t->show=show; t->native=GetCurrentTask();
    lstrcpyn(t->command,command?command:"",sizeof(t->command));
    t->psp=make_psp(t);
    t->scratch=(BYTE *)Alloc16(SCRATCH_BYTES);
    t->scratch_sel=t->scratch?SelAlloc(SelLinear(t->scratch),SCRATCH_BYTES-1,SEL_DATA):0;
    t->scratch_top=SCRATCH_BYTES;
    t->dta=(DWORD)t->psp<<16|0x80;
    t->environment=Environment16();
    if(t->environment) put16((BYTE *)SelPointer(t->psp,0)+0x2c,t->environment);
    if(!t->psp || !t->scratch_sel) {
        if(t->scratch) Free16(t->scratch,SCRATCH_BYTES);
        if(t->psp) {Free16(SelPointer(t->psp,0),256); SelFree(t->psp);}
        NeFree(m); t->module=NULL; release_shared(); return 8;
    }
    live++;
    c=&t->cpu;
    c->cpl=3; c->cflg=0x31; c->eflags=0x202;
    c->fsr=(wh_u64)0x55550000; c->fcr=0x37f|(wh_u64)0x1f80<<32;
    c->ldt=ldt_descriptor; c->gdt=gdt_descriptor; c->sys_sel=(wh_u64)0x08<<32;
    SetSeg16(t,1,m->seg[m->cs-1].sel); c->eip=m->ip;
    SetSeg16(t,2,m->seg[m->ss-1].sel); SetReg16(t,SP,m->sp);
    SetSeg16(t,3,m->instance); SetSeg16(t,0,t->psp);
    SetReg16(t,BX,m->stack); SetReg16(t,CX,m->heap); SetReg16(t,DI,m->instance);
    wh_task_slots(0)[3]=t;
    if(!NeInitLibraries(t,m)) {
        wh_trace("WOW: a library failed to initialize");
        MessageBox(NULL,"A library this program needs failed to initialize.","Application Error",MB_OK|MB_ICONHAND);
        TaskEnd16(t,0xff);
    }
    for(;;) {run(t); dispatch(t);}
}
