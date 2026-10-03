/* SPDX-License-Identifier: GPL-2.0-or-later
 * IA32TEST: runs a small real-mode program on the processor's IA-32
 * instruction set through IO.SYS and services its interruptions: INT 21h
 * output and exit, a register check through INT 60h, the MOV SS trap and an
 * emulated IRET. The x87 stack must survive the exits between instructions.
 * A second program runs in 16-bit protected mode at privilege 3, as Win16
 * programs do: it loads selectors from a local descriptor table in memory and
 * far-calls a thunk whose INT 60h is answered with an emulated RETF 2.
 * /MAP shows the processor status (address translation) and the I/O port
 * base, and lists the 4 KiB pages below 640 KiB that hold nonzero bytes. */
#include "runtime.h"
#define SEG 0x2000U
#define LINEAR(seg,off) ((u8 *)(u64)(((u32)(seg)<<4)+(u16)(off)))
static const u8 program[]={
    0xb4,0x09,0xba,0x49,0x01,0xcd,0x21,0x8c,0xc8,0x8e,0xd0,0xbc,0xfe,0xff,0xd9,0xe8,
    0xd9,0xe8,0xde,0xc1,0xb8,0x11,0x11,0xbb,0x22,0x22,0xb9,0x33,0x33,0xba,0x44,0x44,
    0xbe,0x55,0x55,0xbf,0x66,0x66,0xbd,0x77,0x77,0xcd,0x60,0xd9,0xe8,0xde,0xc1,0xdf,
    0x1e,0x5b,0x01,0x9c,0x0e,0x68,0x39,0x01,0xcf,0xa0,0x5b,0x01,0x04,0x30,0x88,0xc2,
    0xb4,0x02,0xcd,0x21,0xb8,0x2a,0x4c,0xcd,0x21,
    'I','A','-','3','2',' ','r','e','a','l',' ','m','o','d','e',':',' ','$',0,0
};
static void set16(u64 *r,u16 v) {*r=(*r&~0xffffULL)|v;}
#define PM_BASE 0x30000U /* LDT, then code, data and thunk segments */
static const u8 pm_program[]={
    0xb8,0x17,0x00,0x8e,0xd8,0x8e,0xd0,0xbc,0xf0,0xff,0xc7,0x06,0x00,0x00,0x34,0x12,
    0x68,0x78,0x56,0x9a,0x00,0x00,0x1f,0x00,0xcd,0x61
};
/* A 16-bit, byte-granular descriptor in memory format; attr holds type,
 * S, DPL and P. The register format keeps those bits in the same order. */
static u64 mem_desc(u32 base,u32 limit,u8 attr) {
    return (limit&0xffff)|(u64)(base&0xffffff)<<16|(u64)attr<<40|(u64)((limit>>16)&15)<<48|(u64)(base>>24)<<56;
}
static u64 reg_desc(u64 m) {
    u32 base=(u32)((m>>16)&0xffffff)|(u32)(m>>56)<<24,limit=(u32)(m&0xffff)|(u32)((m>>48)&15)<<16;
    return base|(u64)limit<<32|((m>>40)&0xff)<<52|((m>>52)&15)<<60;
}
static int protected_mode(const IoServices *io) {
    u8 *mem=(u8 *)(u64)PM_BASE;
    for(unsigned i=0;i<0x4000;i++) mem[i]=0;
    u64 *ldt=(u64 *)mem;
    ldt[1]=mem_desc(PM_BASE+0x1000,0xffff,0xfb); /* code, DPL 3 */
    ldt[2]=mem_desc(PM_BASE+0x2000,0xffff,0xf3); /* data, DPL 3 */
    ldt[3]=mem_desc(PM_BASE+0x3000,0xffff,0xfb); /* thunk code */
    for(unsigned i=0;i<sizeof pm_program;i++) mem[0x1000+i]=pm_program[i];
    mem[0x3000]=0xcd; mem[0x3001]=0x60;
    static IoIa32Context x;
    x=(IoIa32Context){0};
    x.cs=reg_desc(ldt[1]); x.ss=x.ds=x.es=reg_desc(ldt[2]);
    x.ldt=(u64)PM_BASE|0x1fULL<<32|0x82ULL<<52; x.gdt=(u64)(PM_BASE+0x3f00)|0xfULL<<32|0x80ULL<<52;
    x.sys_sel=0x0f|0x17ULL<<16|0x08ULL<<32; x.data_sel=0x17|0x17ULL<<16;
    x.cpl=3; x.eip=0; x.gr[4]=0xfff0; x.eflags=0x202; x.cflg=0x31;
    x.fsr=0x55550000ULL; x.fcr=0x37f|0x1f80ULL<<32;
    for(unsigned exits=0;exits<8;exits++) {
        if(io->ia32_run(io->context,&x)) return 0;
        if(x.exit==IO_IA32_INTERCEPT && x.vector==2) continue;
        if(x.exit!=IO_IA32_INTERRUPT) {
            print("IA32TEST: protected mode exit %u vector %x code %x at %x\n",(unsigned long long)x.exit,
                  (unsigned long long)x.vector,(unsigned long long)x.code,(unsigned long long)x.fault_ip);
            return 0;
        }
        const u8 *stack=(const u8 *)(u64)(PM_BASE+0x2000);
        u16 sp=(u16)x.gr[4];
        if(x.vector==0x60) {
            u16 ip=stack[sp]|stack[sp+1]<<8,cs=stack[sp+2]|stack[sp+3]<<8,arg=stack[sp+4]|stack[sp+5]<<8;
            if((u16)x.sys_sel!=0x1f || cs!=0x0f || arg!=0x5678) return 0;
            set16(&x.gr[0],(u16)(arg+1)); set16(&x.gr[4],(u16)(sp+6));
            x.eip=ip; x.sys_sel=(x.sys_sel&~0xffffULL)|cs; x.cs=reg_desc(ldt[cs>>3]);
            continue;
        }
        return x.vector==0x61 && (u16)x.gr[0]==0x5679 && sp==0xfff0 && stack[0]==0x34 && stack[1]==0x12;
    }
    return 0;
}

static u64 real_desc(u16 sel) {return (u64)sel<<4|0xffffULL<<32|3ULL<<52|1ULL<<56|1ULL<<59;}
static u16 seg_of(u64 sel,unsigned slot) {return (u16)(sel>>(slot*16));}

static void map_low(void) {
    u64 psr,k0; __asm__ volatile("mov %0=psr" : "=r"(psr)); __asm__ volatile("mov %0=ar.k0" : "=r"(k0));
    print("PSR %x, I/O base %x\n",(unsigned long long)psr,(unsigned long long)k0);
    print("Nonzero pages:");
    for(u32 page=0;page<0xa0;page++) {
        const volatile u64 *p=(const volatile u64 *)(u64)(page<<12);
        for(unsigned i=0;i<512;i++) if(p[i]) {print(" %x",(unsigned long long)page); break;}
    }
    print("\n");
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const IoServices *io=dos_io_services();
    const char *tail=app_dos->command_tail();
    if(tail && (tail[0]=='/' && (tail[1]=='m' || tail[1]=='M'))) {map_low(); return EFI_SUCCESS;}
    if(!io || io->size<offsetof(IoServices,ia32_run)+sizeof(io->ia32_run) || !(io->capabilities&IO_CAP_IA32)) {
        print("IA32TEST: this processor has no IA-32 instruction set\n"); dos_set_errorlevel(2); return EFI_SUCCESS;
    }
    u8 *base=LINEAR(SEG,0);
    for(unsigned i=0;i<0x10000;i++) base[i]=0;
    for(unsigned i=0;i<sizeof program;i++) base[0x100+i]=program[i];
    static IoIa32Context x;
    x=(IoIa32Context){0};
    x.cs=x.ss=x.ds=x.es=x.fs=x.gs=real_desc(SEG);
    x.data_sel=SEG|(u64)SEG<<16|(u64)SEG<<32|(u64)SEG<<48; x.sys_sel=SEG|(u64)SEG<<16;
    x.eip=0x100; x.gr[4]=0xfffe; x.eflags=0x202; x.cflg=0x30;
    x.fsr=0x55550000ULL; x.fcr=0x37f|0x1f80ULL<<32;
    unsigned checks=0,exits=0;
    for(;;) {
        int e=io->ia32_run(io->context,&x);
        if(e) {print("IA32TEST: ia32_run failed %u\n",(unsigned long long)e); dos_set_errorlevel(1); return EFI_SUCCESS;}
        exits++;
        u16 ax=(u16)x.gr[0],dx=(u16)x.gr[2];
        u16 ds=seg_of(x.data_sel,0),ss=seg_of(x.sys_sel,1),cs=seg_of(x.sys_sel,0);
        if(x.exit==IO_IA32_INTERRUPT && x.vector==0x21) {
            if((ax>>8)==9) {
                u8 *s=LINEAR(ds,dx); char buf[80]; unsigned n=0;
                while(n<79 && s[n]!='$') {buf[n]=(char)s[n]; n++;}
                buf[n]=0; print("%s",buf);
            } else if((ax>>8)==2) print("%c",(int)(dx&255));
            else if((ax>>8)==0x4c) {
                print("\nIA32TEST: exit code %u after %u exits, %u checks\n",
                      (unsigned long long)(ax&255),(unsigned long long)exits,(unsigned long long)checks);
                print("IA32TEST: protected mode %s\n",protected_mode(io)?"ok":"FAILED");
                dos_set_errorlevel(ax&255); return EFI_SUCCESS;
            }
        } else if(x.exit==IO_IA32_INTERRUPT && x.vector==0x60) {
            static const u16 want[8]={0x1111,0x3333,0x4444,0x2222,0xfffe,0x7777,0x5555,0x6666};
            for(unsigned i=0;i<8;i++) if((u16)x.gr[i]!=want[i]) {
                print("\nIA32TEST: register %u is %x\n",(unsigned long long)i,(unsigned long long)x.gr[i]);
                dos_set_errorlevel(1); return EFI_SUCCESS;
            }
            if(ss!=SEG || cs!=SEG) {print("\nIA32TEST: segments %x %x\n",(unsigned long long)cs,(unsigned long long)ss); return EFI_SUCCESS;}
            checks++;
        } else if(x.exit==IO_IA32_INTERCEPT && x.vector==2) {
            checks++; /* MOV SS: resume after it */
        } else if(x.exit==IO_IA32_INTERCEPT && x.vector==0 && (x.iim&255)==0xcf && !(x.code&2)) {
            u16 sp=(u16)x.gr[4]; u8 *s=LINEAR(ss,0);
            u16 ip=s[sp]|s[(u16)(sp+1)]<<8,ncs=s[(u16)(sp+2)]|s[(u16)(sp+3)]<<8;
            u16 flags=s[(u16)(sp+4)]|s[(u16)(sp+5)]<<8;
            set16(&x.gr[4],(u16)(sp+6));
            x.eip=ip; x.eflags=(x.eflags&~0xffffULL)|flags|2;
            x.sys_sel=(x.sys_sel&~0xffffULL)|ncs; x.cs=real_desc(ncs);
            checks++;
        } else {
            print("\nIA32TEST: exit %u vector %x code %x at %x (iim %x)\n",(unsigned long long)x.exit,
                  (unsigned long long)x.vector,(unsigned long long)x.code,(unsigned long long)x.fault_ip,
                  (unsigned long long)x.iim);
            dos_set_errorlevel(1); return EFI_SUCCESS;
        }
    }
}
