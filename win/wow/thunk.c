/* SPDX-License-Identifier: GPL-2.0-or-later
 * Win16 import thunks: each system module has an INT THUNK_INT segment
 * with ordinal n at offset n*4. API tables describe arguments: w=WORD,
 * s=signed WORD, l=DWORD, p=far pointer converted to linear, P=raw far
 * pointer, R=resource string address or integer ID (zero selector).
 * Pascal arguments are reversed on the stack and callee-popped; A_CDECL
 * leaves them for the caller. A_REGS functions manage registers and return.
 */
#include "wow.h"
#include "api.h"
#define ORDINALS 1024U
#define PROCS 1024U
static WORD zero_sel; /* KERNEL's fixed selectors (kernel_constant) */
/* Native window procedures as 16-bit far procedures: entry n of this
 * segment (an INT at n*4) calls procs[n] with the arguments converted. */
static WORD proc_sel;
static WNDPROC procs[PROCS];
/* MakeProcInstance's thunks: "mov ax, DGROUP; jmp far procedure", eight
 * bytes each, past the procedure's "push ds; pop ax; nop" so that AX stays
 * (the procedure itself is left as it is, for direct calls). */
#define INSTANCES 4096U
static WORD instance_sel; static BYTE *instance_code; static unsigned instance_count;
typedef struct {const char *name; const Api16 *api; const unsigned *count; WORD sel;} ThunkModule;
static ThunkModule modules[]={
    {"KERNEL",kernel_api,&kernel_count,0},
    {"USER",user_api,&user_count,0},
    {"GDI",gdi_api,&gdi_count,0},
    {"COMMDLG",commdlg_api,&commdlg_count,0},
    {"SHELL",shell_api,&shell_count,0},
    {"MMSYSTEM",mmsystem_api,&mmsystem_count,0},
    {"LZEXPAND",lzexpand_api,&lzexpand_count,0},
    {"VER",ver_api,&ver_count,0},
    {"TOOLHELP",toolhelp_api,&toolhelp_count,0},
    {"KEYBOARD",keyboard_api,&keyboard_count,0},
    {"SOUND",sound_api,&sound_count,0},
    {"SYSTEM",system_api,&system_count,0},
    {"WIN87EM",win87em_api,&win87em_count,0},
    {"PSCRIPT",pscript_api,&pscript_count,0}, /* the printer driver's entry points, GDI's PSCRIPT */
};
#define MODULE_COUNT (sizeof(modules)/sizeof(modules[0]))

static WORD code_segment(DWORD bytes) {
    BYTE *p=(BYTE *)Alloc16(bytes); DWORD i; WORD sel;
    if(!p) return 0;
    for(i=0;i+3<bytes;i+=4) {p[i]=0xcd; p[i+1]=THUNK_INT; p[i+2]=0x90; p[i+3]=0x90;}
    sel=SelAlloc(SelLinear(p),bytes-1,SEL_CODE);
    if(!sel) Free16(p,bytes);
    return sel;
}
BOOL ThunkInit(void) {
    unsigned i;
    if(return_thunk) return TRUE;
    for(i=0;i<MODULE_COUNT;i++) if(!(modules[i].sel=code_segment(ORDINALS*4))) return FALSE;
    return_thunk=code_segment(16);
    proc_sel=code_segment(PROCS*4);
    return return_thunk!=0 && proc_sel!=0;
}
static void free_segment(WORD sel,DWORD bytes) {
    if(sel) {Free16(SelPointer(sel,0),bytes); SelFree(sel);}
}
void ThunkShutdown(void) {
    unsigned i;
    for(i=0;i<MODULE_COUNT;i++) {free_segment(modules[i].sel,ORDINALS*4); modules[i].sel=0;}
    free_segment(return_thunk,16); return_thunk=0;
    free_segment(proc_sel,PROCS*4); proc_sel=0; memset(procs,0,sizeof(procs));
    free_segment(instance_sel,INSTANCES*8); instance_sel=0; instance_code=NULL; instance_count=0;
    if(zero_sel) {free_segment(zero_sel,0x10000); zero_sel=0;}
}
static ThunkModule *module_named(LPCSTR name) {
    unsigned i;
    for(i=0;i<MODULE_COUNT;i++) if(!lstrcmpi(modules[i].name,name)) return &modules[i];
    return NULL;
}
/* KERNEL's constant entries: selector arithmetic and fixed selectors.
 * The selectors for the BIOS data and the adapter areas give zeros. */
static DWORD kernel_constant(WORD ordinal) {
    switch(ordinal) {
    case 113: return MAKELONG(3,3);           /* __AHSHIFT */
    case 114: return MAKELONG(8,8);           /* __AHINCR */
    case 178: return MAKELONG(0x425,0x425);   /* __WINFLAGS */
    case 173: case 174: case 179: case 181: case 182: case 183: case 190: case 193: case 194: case 195:
        if(!zero_sel) {void *p=Alloc16(0x10000); zero_sel=p?SelAlloc(SelLinear(p),0xffff,SEL_DATA):0;}
        return MAKELONG(zero_sel,zero_sel);
    default: return 0;
    }
}
DWORD ThunkAddress(LPCSTR module,WORD ordinal) {
    ThunkModule *m=module_named(module);
    if(!m || ordinal>=ORDINALS) return 0;
    if(m==&modules[0] && kernel_constant(ordinal)) return kernel_constant(ordinal);
    return (DWORD)m->sel<<16|(DWORD)ordinal*4;
}
LPCSTR ThunkModuleName(WORD sel) {
    unsigned i;
    for(i=0;i<MODULE_COUNT;i++) if(modules[i].sel==(sel|7)) return modules[i].name;
    return NULL;
}
WORD ThunkModuleHandle(LPCSTR name) {ThunkModule *m=module_named(name); return m?m->sel:0;}
WORD ThunkOrdinal(LPCSTR module,LPCSTR name) {
    ThunkModule *m=module_named(module); unsigned i;
    if(!m || !m->api) return 0;
    for(i=0;i<*m->count;i++) if(!lstrcmpi(m->api[i].name,name)) return m->api[i].ordinal;
    return 0;
}
/* Whether GetProcAddress may give it: the stubs are only for imports. */
BOOL ThunkImplemented(LPCSTR module,WORD ordinal) {
    ThunkModule *m=module_named(module); unsigned i;
    if(!m || !m->api) return FALSE;
    for(i=0;i<*m->count;i++) if(m->api[i].ordinal==ordinal) return m->api[i].fn!=W16_Unimplemented;
    return FALSE;
}
BOOL IsThunk(WORD sel) {
    unsigned i;
    if(sel==proc_sel) return TRUE;
    for(i=0;i<MODULE_COUNT;i++) if(modules[i].sel==sel) return TRUE;
    return FALSE;
}
DWORD NativeProc16(WNDPROC p) {
    unsigned i,free_slot=PROCS;
    if(!p) return 0;
    for(i=0;i<PROCS;i++) {if(procs[i]==p) return (DWORD)proc_sel<<16|i*4; if(!procs[i] && free_slot==PROCS) free_slot=i;}
    if(free_slot==PROCS) return 0;
    procs[free_slot]=p;
    return (DWORD)proc_sel<<16|free_slot*4;
}
DWORD InstanceThunk16(DWORD proc,WORD ds) {
    const BYTE *code=(const BYTE *)Lin16(proc); DWORD target=proc; unsigned i; BYTE *t;
    if(!code) return 0;
    if(!instance_sel) {
        if(!(instance_code=(BYTE *)Alloc16(INSTANCES*8))) return proc;
        if(!(instance_sel=SelAlloc(SelLinear(instance_code),INSTANCES*8-1,SEL_CODE))) {Free16(instance_code,INSTANCES*8); instance_code=NULL; return proc;}
    }
    if((code[0]==0x1e && code[1]==0x58 && code[2]==0x90) || (code[0]==0x8c && code[1]==0xd8 && code[2]==0x90))
        target=MAKELONG(LOWORD(proc)+3,HIWORD(proc));
    for(i=0;i<instance_count;i++) {
        t=instance_code+i*8;
        if(get16(t+1)==ds && get16(t+4)==LOWORD(target) && get16(t+6)==HIWORD(target)) return MAKELONG(i*8,instance_sel);
    }
    if(instance_count==INSTANCES) return proc;
    t=instance_code+instance_count*8;
    t[0]=0xb8; put16(t+1,ds); t[3]=0xea; put16(t+4,LOWORD(target)); put16(t+6,HIWORD(target));
    return MAKELONG(instance_count++*8,instance_sel);
}
WNDPROC NativeProcOf(DWORD segptr) {
    if(HIWORD(segptr)!=proc_sel || LOWORD(segptr)%4 || LOWORD(segptr)/4>=PROCS) return NULL;
    return procs[LOWORD(segptr)/4];
}

static unsigned size_of(char c) {return c=='w' || c=='s'?2U:4U;}
/* A function there is no implementation of returns 0, said once each. */
DWORD W16_Unimplemented(Args16 *a) {
    static const char *said[64]; static unsigned count; unsigned i; char line[96];
    for(i=0;i<count && said[i]!=a->name;i++) {}
    if(i==count) {
        if(count<64) said[count++]=a->name;
        wsprintf(line,"WOW: %s.%s is not implemented; it returns 0",a->module,a->name);
        wh_trace(line);
    }
    return 0;
}
void Return16(Task16 *t,WORD bytes) {
    WORD sp=Reg16(t,SP),ss=Seg16(t,2),ip=Peek16(ss,sp),cs=Peek16(ss,(WORD)(sp+2));
    SetReg16(t,SP,(WORD)(sp+4+bytes)); SetSeg16(t,1,cs); t->cpu.eip=ip;
}
/* A native window procedure, called as a Pascal (hwnd, msg, wParam,
 * lParam): above the return address lie lParam, wParam, msg, hwnd. */
static void native_proc(Task16 *t,WORD entry) {
    WORD sp=Reg16(t,SP),ss=Seg16(t,2); DWORD r;
    WNDPROC p=entry<PROCS?procs[entry]:NULL;
    if(!p) Fault16(t,"call to a released window procedure");
    r=CallNative16(t,p,Peek16(ss,(WORD)(sp+12)),Peek16(ss,(WORD)(sp+10)),Peek16(ss,(WORD)(sp+8)),
                   Peek16(ss,(WORD)(sp+4))|(DWORD)Peek16(ss,(WORD)(sp+6))<<16);
    SetReg16(t,AX,LOWORD(r)); SetReg16(t,DX,HIWORD(r));
    Return16(t,10);
}
void ThunkCall(Task16 *t) {
    WORD cs=Seg16(t,1),ordinal=(WORD)((t->cpu.eip-2)/4);
    ThunkModule *m=NULL; const Api16 *api=NULL; unsigned i,n,total=0;
    Args16 a; DWORD result; WORD sp,ss,pos;
    if(cs==proc_sel) {native_proc(t,ordinal); return;}
    for(i=0;i<MODULE_COUNT && !m;i++) if(modules[i].sel==cs) m=&modules[i];
    if(m && m->api) for(i=0;i<*m->count && !api;i++) if(m->api[i].ordinal==ordinal) api=&m->api[i];
    if(!api) {
        char what[64];
        wsprintf(what,"%s.%u is not available",m?m->name:"?",ordinal);
        Fault16(t,what);
    }
    memset(&a,0,sizeof(a)); a.task=t; a.module=m->name; a.name=api->name;
    n=(unsigned)lstrlen(api->args);
    for(i=0;i<n;i++) total+=size_of(api->args[i]);
    sp=Reg16(t,SP); ss=Seg16(t,2); a.stack=(WORD)(sp+4);
    if(api->flags&A_REGS) {api->fn(&a); return;}
    pos=(WORD)(api->flags&A_CDECL?sp+4:sp+4+total);
    for(i=0;i<n;i++) {
        unsigned size=size_of(api->args[i]); DWORD v;
        if(!(api->flags&A_CDECL)) pos=(WORD)(pos-size);
        v=size==2?Peek16(ss,pos):Peek16(ss,pos)|(DWORD)Peek16(ss,(WORD)(pos+2))<<16;
        a.raw[i]=v;
        switch(api->args[i]) {
        case 's': v=(DWORD)(LONG)(short)v; break;
        case 'p': v=SelLinear(Lin16(v)); break;
        case 'R': if(HIWORD(v)) v=SelLinear(Lin16(v)); break;
        default: break;
        }
        a.a[i]=v;
        if(api->flags&A_CDECL) pos=(WORD)(pos+size);
    }
    result=api->fn(&a);
    SetReg16(t,AX,LOWORD(result));
    if(!(api->flags&A_RET16)) SetReg16(t,DX,HIWORD(result));
    Return16(t,(WORD)(api->flags&A_CDECL?0:total));
}
