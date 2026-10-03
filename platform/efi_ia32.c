/* SPDX-License-Identifier: GPL-2.0-or-later
 * Hardware IA-32 execution for Itanium and pre-9000-series Itanium 2.
 * rfi enters the image (ia32_ia64.S). Firmware Debug Support forwards
 * IA_32_Exception/Intercept/Interrupt vectors to a callback that saves
 * state and resumes at an IA-64 landing pad. The caller handles the
 * interruption outside firmware context.
 *
 * Runs end on the first interruption and never overlap. A service's
 * IA-32 callback, such as a window procedure, starts a separate run.
 * Frames live on the caller's stack, including fiber stacks. Debug callbacks
 * are registered once and remain until IO.SYS exits. Timed runs enable
 * external interrupts; a firmware periodic callback ends the run at the
 * limit, preserving state between instructions.
 */
#include "efi_ia32.h"
#include <efidebug.h>
#define PSR_AC (1ULL<<3)
#define PSR_I (1ULL<<14)
#define PSR_DFH (1ULL<<19)
#define PSR_IS (1ULL<<34)
#define PSR_BN (1ULL<<44)
#define DCR_LC (1ULL<<2)
#define TYPE_EXCEPTION 45
#define TYPE_INTERCEPT 46
#define TYPE_INTERRUPT 47
_Static_assert(offsetof(IoIa32Context,gr)==384 && offsetof(IoIa32Context,eip)==448 &&
               offsetof(IoIa32Context,data_sel)==464 && offsetof(IoIa32Context,es)==480 &&
               offsetof(IoIa32Context,cs)==528 && offsetof(IoIa32Context,cflg)==544 &&
               offsetof(IoIa32Context,fdr)==576,"ia32_ia64.S offsets");
typedef struct {u64 regs[22]; u64 f[20][2];} __attribute__((aligned(16))) Frame;
void ia32_enter(IoIa32Context *,Frame *,u64 ipsr,u64 iip);
void ia32_landing(void);
static EFI_BOOT_SERVICES *bs;
static EFI_DEBUG_SUPPORT_PROTOCOL *debug;
static int registered;
static IoIa32Context *volatile active;
static volatile u64 resume_psr,deadline;
static int periodic;
static u64 itc_per_ms;

static u64 read_psr(void) {u64 v; __asm__ volatile("mov %0=psr" : "=r"(v)); return v;}
static u64 read_dcr(void) {u64 v; __asm__ volatile("mov %0=cr.dcr" : "=r"(v)); return v;}
static void write_dcr(u64 v) {__asm__ volatile("mov cr.dcr=%0;; srlz.d" :: "r"(v) : "memory");}
static u64 cpuid(u64 index) {u64 v; __asm__ volatile("mov %0=cpuid[%1]" : "=r"(v) : "r"(index)); return v;}
/* Family 7 is Itanium, 1Fh Itanium 2 up to the 9000 series (20h). */
static int native_ia32(void) {
    unsigned family=(unsigned)(cpuid(3)>>24)&255;
    return family==7 || family==0x1f;
}

static u64 read_itc(void) {u64 v; __asm__ volatile("mov %0=ar.itc" : "=r"(v)); return v;}
/* Records the IA-32 state and resumes in IA-64 code at the landing pad. */
static void capture(EFI_SYSTEM_CONTEXT_IPF *c,IoIa32Context *x,u32 exit) {
    const u64 *gr=&c->R8;
    for(unsigned i=0;i<8;i++) x->gr[i]=(u32)gr[i];
    const u64 *f=c->F8;
    for(unsigned i=0;i<24;i++) {x->fp[i][0]=f[i*2]; x->fp[i][1]=f[i*2+1];}
    x->data_sel=c->R16; x->sys_sel=c->R17;
    x->es=c->R24; x->ds=c->R27; x->fs=c->R28; x->gs=c->R29; x->ldt=c->R30; x->gdt=c->R31;
    x->cs=c->ArCsd; x->ss=c->ArSsd; x->eflags=(u32)c->ArEflag; x->cflg=c->ArCflg;
    x->fsr=c->ArFsr; x->fcr=c->ArFcr; x->fir=c->ArFir; x->fdr=c->ArFdr;
    x->eip=(u32)((u32)c->CrIip-(u32)x->cs);
    x->exit=exit;
    if(exit==IO_IA32_TIMER) {x->vector=x->code=0; x->iim=x->ifa=0; x->fault_ip=(u32)c->CrIip;}
    else {x->vector=(u32)(c->CrIsr>>16)&255; x->code=(u32)c->CrIsr&0xffff; x->iim=c->CrIim; x->ifa=c->CrIfa; x->fault_ip=(u32)c->CrIipa;}
    active=NULL; deadline=0;
    c->CrIpsr=resume_psr; c->CrIip=((const u64 *)(void *)ia32_landing)[0];
    c->CrIfs=1ULL<<63; /* an empty frame; the landing pad reloads the rest */
}
/* Runs on the firmware's interruption stack with address translation off;
 * DOS memory is identity-mapped, so the context and frame are reachable. */
static void EFIAPI trap(EFI_EXCEPTION_TYPE type,EFI_SYSTEM_CONTEXT system) {
    EFI_SYSTEM_CONTEXT_IPF *c=system.SystemContextIpf;
    IoIa32Context *x=active;
    if(!x || !(c->CrIpsr&PSR_IS)) return;
    capture(c,x,type==TYPE_EXCEPTION?IO_IA32_EXCEPTION:type==TYPE_INTERCEPT?IO_IA32_INTERCEPT:IO_IA32_INTERRUPT);
}
/* The firmware's timer: a run past its limit ends between instructions. */
static void EFIAPI tick(EFI_SYSTEM_CONTEXT system) {
    EFI_SYSTEM_CONTEXT_IPF *c=system.SystemContextIpf;
    IoIa32Context *x=active;
    if(!x || !deadline || !(c->CrIpsr&PSR_IS) || (i64)(read_itc()-deadline)<0) return;
    capture(c,x,IO_IA32_TIMER);
}

static int enable(void) {
    if(registered) return 0;
    if(!debug) return DE_FUNCTION;
    static const EFI_EXCEPTION_TYPE types[3]={TYPE_EXCEPTION,TYPE_INTERCEPT,TYPE_INTERRUPT};
    for(unsigned i=0;i<3;i++)
        if(EFI_ERROR(debug->RegisterExceptionCallback(debug,0,trap,types[i]))) {
            while(i--) debug->RegisterExceptionCallback(debug,0,NULL,types[i]);
            return DE_FUNCTION;
        }
    registered=1;
    return 0;
}
static int enable_timer(void) {
    if(periodic) return 0;
    if(EFI_ERROR(debug->RegisterPeriodicCallback(debug,0,tick))) return DE_FUNCTION;
    periodic=1;
    return 0;
}

static int io_ia32_run(void *ctx,IoIa32Context *x) {
    (void)ctx;
    int e=enable();
    if(e) return e;
    Frame frame;
    u64 psr=read_psr()|PSR_BN,run_psr;
    resume_psr=psr;
    x->exit=0;
    deadline=0;
    run_psr=(psr|PSR_IS|(u64)(x->cpl&3)<<32)&~(PSR_AC|PSR_DFH);
    if(x->timer_ms && itc_per_ms && !enable_timer()) {
        deadline=read_itc()+(u64)x->timer_ms*itc_per_ms;
        run_psr|=PSR_I;
    }
    active=x;
    /* Clear PSR.ac so IA-64 alignment checks do not reject IA-32 accesses;
     * IA-32 uses EFLAGS.AC/CR0.AM. Enable high FP registers. Clear firmware
     * DCR.lc to avoid Lock intercepts for references crossing 8 bytes or using
     * non-write-back memory. Hardware bus locks/non-atomic RMW are sufficient
     * because only one IA-32 program runs. */
    u64 dcr=read_dcr();
    if(dcr&DCR_LC) write_dcr(dcr&~DCR_LC);
    ia32_enter(x,&frame,run_psr,(u32)((u32)x->cs+(u32)x->eip));
    if(dcr&DCR_LC) write_dcr(dcr);
    active=NULL;
    return x->exit?0:DE_FUNCTION;
}

void efi_ia32_init(EFI_SYSTEM_TABLE *st,IoServices *services,u64 itc_ms) {
    bs=st->BootServices; itc_per_ms=itc_ms;
    EFI_GUID guid=EFI_DEBUG_SUPPORT_PROTOCOL_GUID;
    if(!native_ia32() || EFI_ERROR(bs->LocateProtocol(&guid,NULL,(void **)&debug)) ||
       debug->Isa!=IsaIpf) {debug=NULL; return;}
    services->ia32_run=io_ia32_run;
    services->capabilities|=IO_CAP_IA32;
}

void efi_ia32_close(void) {
    if(periodic) {debug->RegisterPeriodicCallback(debug,0,NULL); periodic=0;}
    if(!registered) return;
    debug->RegisterExceptionCallback(debug,0,NULL,TYPE_EXCEPTION);
    debug->RegisterExceptionCallback(debug,0,NULL,TYPE_INTERCEPT);
    debug->RegisterExceptionCallback(debug,0,NULL,TYPE_INTERRUPT);
    registered=0;
}
