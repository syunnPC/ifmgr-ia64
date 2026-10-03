/* SPDX-License-Identifier: GPL-2.0-or-later
 * DPMI 0.9: INT 2Fh 1687h exposes TRAP_SEG:DPMI_SWITCH for 16/32-bit
 * protected mode at CPL 3, with an LDT below 4 GiB. INT 31h manages
 * segments, DOS/linear memory, interrupt and exception handlers, real-mode
 * calls (0300h-0302h), callbacks and raw switches. Without paging, linear
 * addresses are physical. Memory is contiguous IO.SYS pages below 4 GiB;
 * locking is a no-op and physical mappings (0800h) return the same address.
 *
 * There is no IDT: protected-mode INTs dispatch here to registered handlers
 * or reflect to real mode. Registers and result flags pass both ways;
 * hardware frames retain interrupted flags. Default handlers and return
 * stubs in the host code segment use INT 67h, identified by their location.
 * Exceptions use the host's locked stack; defaults reflect INT 0-5 and 7,
 * or terminate for other exceptions. Timer delivery uses protected-mode
 * INT 8/1Ch. Selectors and cached descriptors must be updated together.
 */
#include "vdm.h"
#define ENTRIES 8192U
#define FIRST_CLIENT 16U /* indexes below are the host's */
#define HOST_CODE 0x000fU /* index 1 */
#define HOST_PAGES 18U /* the LDT (16 pages), the host's code and its locked stack */
#define LDT_PAGES 16U
/* The host's code page. */
#define PM_DEFAULT16 0x000U /* n*4: INT 67h, IRET */
#define PM_DEFAULT32 0x400U /* n*4: INT 67h, IRETD */
#define EXC_DEFAULT16 0x800U /* v*4: INT 67h, RETF */
#define EXC_DEFAULT32 0x880U /* v*4: INT 67h, RETFD */
#define EXC_RETURN 0x900U /* INT 67h */
#define CB_RETURN 0x904U
#define HW_RETURN 0x908U
#define SR_PM16 0x90cU /* RETF */
#define SR_PM32 0x910U /* RETFD */
#define RAW_PM 0x914U /* INT 67h */
#define GDT_OFF 0xf00U
#define LOCKED_SIZE 0x1000U
#define FRAMES 16
#define CALLBACKS 32
#define BLOCKS 256
#define DE_UNSUPPORTED 0x8001U
#define DE_DESCRIPTOR 0x8011U
#define DE_LINEAR 0x8012U
#define DE_VALUE 0x8021U
#define DE_SELECTOR 0x8022U
#define DE_HANDLE 0x8023U
#define DE_CALLBACK 0x8015U

static u8 *host; /* LDT, code page, locked stack */
static u8 owner[ENTRIES]; /* 0 free, 1 host, 2 the client */
typedef struct {u16 segment,selector;} SegmentAlias;
typedef struct {u16 segment,selector,count;} DosBlock; /* 0100h's: its selectors */
#define DOS_BLOCKS 64
static struct {
    int active,bits32; u16 psp,environment;
    u16 vec_sel[256]; u32 vec_off[256];
    u16 exc_sel[32]; u32 exc_off[32];
    u16 host_seg; u16 locked;
    SegmentAlias aliases[64]; unsigned alias_count;
    DosBlock dos_blocks[DOS_BLOCKS];
} client;
/* Mode frames: the protected-mode state kept while real-mode code runs
 * for it, or the real-mode state kept while a callback runs. */
enum {F_REFLECT=1,F_TRANSLATE,F_CALLBACK,F_HARDWARE};
typedef struct {int kind; IoIa32Context state; u16 struct_sel; u32 struct_off; unsigned vector;} Frame;
static Frame frames[FRAMES]; static unsigned frame_count;
typedef struct {int used; u16 sel; u32 off; u16 struct_sel; u32 struct_off;} Callback;
static Callback callbacks[CALLBACKS];
typedef struct {void *memory; u32 bytes,pages;} MemBlock;
static MemBlock blocks[BLOCKS];
static u32 used_kb;

/* --- descriptors ----------------------------------------------------------- */
static u8 *entry(u16 sel) {return host+(sel&~7U);}
static int valid(u16 sel) {return (sel&7)==7 && (sel>>3)<ENTRIES && owner[sel>>3]==2;}
static int host_or_valid(u16 sel) {return (sel&7)==7 && (sel>>3)<ENTRIES && owner[sel>>3];}
static u32 base_of(const u8 *e) {return e[2]|(u32)e[3]<<8|(u32)e[4]<<16|(u32)e[7]<<24;}
static u32 raw_limit(const u8 *e) {return e[0]|(u32)e[1]<<8|(u32)(e[6]&15)<<16;}
static u32 limit_of(const u8 *e) {u32 l=raw_limit(e); return e[6]&0x80?l<<12|0xfff:l;}
static void set_entry(u16 sel,u32 base,u32 limit,u8 access,u8 flags) {
    u8 *e=entry(sel);
    if(limit>0xfffff) {limit>>=12; flags|=0x80;}
    e[0]=(u8)limit; e[1]=(u8)(limit>>8); e[2]=(u8)base; e[3]=(u8)(base>>8); e[4]=(u8)(base>>16);
    e[5]=access; e[6]=(u8)((flags&0xf0)|((limit>>16)&15)); e[7]=(u8)(base>>24);
}
/* The register format: base 31:0, limit 51:32, access byte 59:52, flags 63:60. */
static u64 descriptor(u16 sel) {
    if(!host_or_valid(sel)) return 0;
    const u8 *e=entry(sel);
    return base_of(e)|(u64)raw_limit(e)<<32|(u64)e[5]<<52|(u64)(e[6]>>4)<<60;
}
static void pm_set(unsigned s,u16 sel) {
    unsigned shift; u64 *w=selector_word(s,&shift);
    *w=(*w&~(0xffffULL<<shift))|(u64)sel<<shift;
    *descriptor_of(s)=sel>=4?descriptor(sel):0;
}
static u16 pm_get(unsigned s) {return sreg(s);}
static int usable_data(u16 sel) {
    if(sel<4) return 1;
    if(!host_or_valid(sel)) return 0;
    const u8 *e=entry(sel);
    return (e[5]&0x80) && (e[5]&0x10) && (!(e[5]&8) || (e[5]&2));
}
static int usable_code(u16 sel) {
    if(!host_or_valid(sel)) return 0;
    const u8 *e=entry(sel);
    return (e[5]&0x98)==0x98 && (e[5]&0x60)==0x60;
}
/* After INT 31h changed descriptors: the segment registers again. */
static void reload_segments(void) {
    for(unsigned s=SR_ES;s<=SR_GS;s++) {
        u16 sel=pm_get(s);
        if(s==SR_CS || s==SR_SS) {pm_set(s,sel); continue;}
        pm_set(s,usable_data(sel)?sel:0);
    }
}
static u16 alloc_selectors(unsigned n) {
    for(unsigned i=FIRST_CLIENT;i+n<=ENTRIES;i++) {
        unsigned k; for(k=0;k<n && !owner[i+k];k++) {}
        if(k==n) {
            for(k=0;k<n;k++) {owner[i+k]=2; set_entry((u16)((i+k)*8+7),0,0,0xf3,client.bits32?0x40:0);}
            return (u16)(i*8+7);
        }
        i+=k;
    }
    return 0;
}
static void free_selector(u16 sel) {
    if(!valid(sel)) return;
    memset(entry(sel),0,8); owner[sel>>3]=0;
    for(unsigned s=SR_ES;s<=SR_GS;s++) if(s!=SR_CS && s!=SR_SS && pm_get(s)==sel) pm_set(s,0);
}
/* A data selector for a real-mode segment (0002h): kept, never freed. */
static u16 segment_selector(u16 seg) {
    for(unsigned i=0;i<client.alias_count;i++) if(client.aliases[i].segment==seg) return client.aliases[i].selector;
    if(client.alias_count==64) return 0;
    u16 sel=alloc_selectors(1); if(!sel) return 0;
    set_entry(sel,(u32)seg<<4,0xffff,0xf3,0);
    client.aliases[client.alias_count++]=(SegmentAlias){seg,sel};
    return sel;
}

/* --- memory the host touches for the program ----------------------------- */
/* Linear ranges VDM may read and write for the program: conventional memory
 * (but the firmware's hole), its memory blocks, its XMS blocks, the HMA. */
static int accessible(u32 at,u32 length) {
    u64 end=(u64)at+length;
    if(end<=0xa0000) return end<=(u32)HOLE_START<<4 || at>=(u32)HOLE_END<<4;
    if((io->capabilities&IO_CAP_HMA) && at>=0x100000 && end<=0x110000) return 1;
    for(unsigned i=0;i<BLOCKS;i++) if(blocks[i].memory) {
        u32 b=(u32)(uintptr_t)blocks[i].memory;
        if(at>=b && end<=(u64)b+blocks[i].bytes) return 1;
    }
    if(host && at>=(u32)(uintptr_t)host && end<=(u64)(uintptr_t)host+HOST_PAGES*4096ULL) return 1;
    return xms_owns(at,length);
}
/* sel:off for length bytes, or NULL. */
static u8 *pm_pointer(u16 sel,u32 off,u32 length) {
    if(!host_or_valid(sel)) return NULL;
    const u8 *e=entry(sel); u64 last=(u64)off+length;
    if(length && last-1>limit_of(e)) return NULL;
    u32 at=base_of(e)+off;
    return accessible(at,length)?LINEAR(at):NULL;
}
static int stack32(void) {return (cpu.ss>>62)&1;}
static u32 sp_value(void) {return stack32()?(u32)cpu.gr[ESP]:(u16)cpu.gr[ESP];}
static void set_sp(u32 v) {if(stack32()) cpu.gr[ESP]=v; else ww(ESP,(u16)v);}
static int pm_push(u32 v,unsigned size) {
    u32 sp=sp_value()-size; if(!stack32()) sp&=0xffff;
    u8 *p=pm_pointer(pm_get(SR_SS),sp,size); if(!p) return 0;
    for(unsigned i=0;i<size;i++) p[i]=(u8)(v>>(i*8));
    set_sp(sp); return 1;
}
static int pm_pop(u32 *v,unsigned size) {
    u32 sp=sp_value(); u8 *p=pm_pointer(pm_get(SR_SS),sp,size); if(!p) return 0;
    u32 x=0; for(unsigned i=0;i<size;i++) x|=(u32)p[i]<<(i*8);
    *v=x; set_sp(stack32()?sp+size:(u16)(sp+size)); return 1;
}
static void pm_jump(u16 sel,u32 off) {pm_set(SR_CS,sel); cpu.eip=off;}
static unsigned word_size(void) {return client.bits32?4:2;}
/* The program's (E)DI, (E)SI, (E)DX: 32-bit for a 32-bit program. */
static u32 reg_off(unsigned r) {return client.bits32?(u32)cpu.gr[r]:rw(r);}
/* A segment register's selector in a kept state. */
static u16 sreg_of(const IoIa32Context *x,unsigned s) {
    switch(s) {
    case SR_ES: return (u16)(x->data_sel>>16); case SR_CS: return (u16)x->sys_sel; case SR_SS: return (u16)(x->sys_sel>>16);
    case SR_DS: return (u16)x->data_sel; case SR_FS: return (u16)(x->data_sel>>32); default: return (u16)(x->data_sel>>48);
    }
}
/* Extended memory left of what VDM's programs may have in all, XMS blocks
 * and DPMI memory blocks together. */
static u32 dpmi_room_kb(void) {u32 used=xms_used_kb()+used_kb; return used<EXTENDED_LIMIT_KB?EXTENDED_LIMIT_KB-used:0;}
/* Port I/O and HLT in protected mode, emulated as VDM does in real mode:
 * the instruction at CS:EIP, its operand size from CS's D bit and 66h. */
static int dpmi_ports(void) {
    u32 at=(u32)cpu.cs+(u32)cpu.eip; unsigned n=0,size32=(cpu.cs>>62)&1; u8 op;
    if(!accessible(at,16)) return 0;
    for(;;) {
        op=*LINEAR(at+n);
        if(op==0x66) size32^=1; else if(op!=0x67 && op!=0xf0 && op!=0xf3 && op!=0xf2) break;
        if(++n>14) return 0;
    }
    n++;
    unsigned size=(op&1)?(size32?4:2):1; u16 port=rw(EDX);
    switch(op) {
    case 0xe4: case 0xe5: case 0xe6: case 0xe7: port=*LINEAR(at+n); n++; break;
    case 0xec: case 0xed: case 0xee: case 0xef: break;
    case 0xf4: io->stall_us(io->context,1000); cpu.eip+=n; return 1;
    default: return 0;
    }
    if(op==0xe4 || op==0xe5 || op==0xec || op==0xed) {
        u32 v=port_in(port,size);
        if(size==1) wl(EAX,(u8)v); else if(size==2) ww(EAX,(u16)v); else cpu.gr[EAX]=v;
    } else port_out(port,size,(u32)cpu.gr[EAX]);
    cpu.eip+=n; return 1;
}
static void end_client(const char *why);
static void real_interrupt_return(void);

/* --- mode frames ----------------------------------------------------------- */
static void keep_fp(IoIa32Context *to,const IoIa32Context *from) {
    memcpy(to->fp,from->fp,sizeof(to->fp));
    to->fsr=from->fsr; to->fcr=from->fcr; to->fir=from->fir; to->fdr=from->fdr;
}
static Frame *push_frame(int kind) {
    if(frame_count==FRAMES) return NULL;
    Frame *f=&frames[frame_count++]; f->kind=kind; f->state=cpu; return f;
}
/* Back to a kept state, the FPU as it is now. */
static void resume(const IoIa32Context *state) {
    IoIa32Context now=cpu; cpu=*state; keep_fp(&cpu,&now);
}
/* Real mode for the host's work: the program's registers, the host's
 * stack below 640 KiB for this level (512 bytes each in its 4 KiB). */
static void enter_real(void) {
    u64 flags=cpu.eflags&(FL_ARITH|0x400|FL_IF);
    cpu.eflags=flags|EFLAGS_V86;
    u16 seg=client.host_seg; u16 top=(u16)(0x1000-0x200*(frame_count>7?7:frame_count));
    for(unsigned s=SR_ES;s<=SR_GS;s++) set_sreg(s,seg);
    cpu.gr[ESP]=top;
}

/* --- interrupts ------------------------------------------------------------- */
static u32 default_vector(unsigned n) {return (client.bits32?PM_DEFAULT32:PM_DEFAULT16)+n*4;}
/* The handler at sel:off, entered with an interrupt's frame. */
static void deliver(u16 sel,u32 off) {
    unsigned size=word_size();
    if(!pm_push(((u32)cpu.eflags&0xffff)|(size==4?(u32)cpu.eflags&0x3f0000:0),size) ||
       !pm_push(pm_get(SR_CS),size) || !pm_push((u32)cpu.eip,size)) {end_client("stack fault"); return;}
    cpu.eflags&=~(u64)(FL_IF|FL_TF);
    pm_jump(sel,off);
}
static void reflect(unsigned n) {
    Frame *f=push_frame(F_REFLECT); if(!f) {end_client("too deep"); return;}
    f->vector=n;
    enter_real();
    u32 v=ivt(n);
    push16((u16)cpu.eflags); push16(TRAP_SEG); push16(DPMI_RETURN);
    cpu.eflags&=~(u64)(FL_IF|FL_TF);
    jump((u16)(v>>16),(u16)v);
}
static void dpmi_int31(void);
/* What a vector does when nothing is set for it. */
static void default_action(unsigned n) {
    if(n==0x31) {dpmi_int31(); return;}
    if(n==0x2f && rw(EAX)==0x1686) {ww(EAX,0); return;}
    reflect(n);
}
/* --- the switch into protected mode ----------------------------------------- */
/* The host needs IO.SYS's pages below 4 GiB. */
int dpmi_available(void) {return io->size>=offsetof(IoServices,range_free)+sizeof(io->range_free) && io->alloc_pages_range;}
static int host_init(void) {
    if(host) return 1;
    if(!dpmi_available() || io->alloc_pages_range(io->context,HOST_PAGES,0x110000,0x100000000ULL,(void **)&host)) {host=NULL; return 0;}
    memset(host,0,HOST_PAGES*4096);
    memset(owner,0,sizeof(owner)); owner[0]=1; owner[1]=1; owner[2]=1;
    u8 *code=host+LDT_PAGES*4096;
    for(unsigned n=0;n<256;n++) {
        u8 *p=code+PM_DEFAULT16+n*4; p[0]=0xcd; p[1]=0x67; p[2]=0xcf; p[3]=0x90;
        p=code+PM_DEFAULT32+n*4; p[0]=0xcd; p[1]=0x67; p[2]=0x66; p[3]=0xcf;
    }
    for(unsigned v=0;v<32;v++) {
        u8 *p=code+EXC_DEFAULT16+v*4; p[0]=0xcd; p[1]=0x67; p[2]=0xcb; p[3]=0x90;
        p=code+EXC_DEFAULT32+v*4; p[0]=0xcd; p[1]=0x67; p[2]=0x66; p[3]=0xcb;
    }
    static const u16 traps[]={EXC_RETURN,CB_RETURN,HW_RETURN,RAW_PM};
    for(unsigned i=0;i<4;i++) {code[traps[i]]=0xcd; code[traps[i]+1]=0x67;}
    code[SR_PM16]=0xcb; code[SR_PM32]=0x66; code[SR_PM32+1]=0xcb;
    set_entry(HOST_CODE,(u32)(uintptr_t)code,0xfff,0xfb,0);
    return 1;
}
/* The descriptor tables in the processor's state: the LDT, an empty GDT. */
static void tables(void) {
    cpu.ldt=(u64)(uintptr_t)host|(u64)(ENTRIES*8-1)<<32|0x82ULL<<52;
    cpu.gdt=(u64)(uintptr_t)(host+LDT_PAGES*4096+GDT_OFF)|15ULL<<32|0x80ULL<<52;
    cpu.sys_sel=(cpu.sys_sel&~0xffff00000000ULL)|0x08ULL<<32;
}
static u16 data_selector(u32 base,u32 limit) {
    u16 sel=alloc_selectors(1); if(sel) set_entry(sel,base,limit,0xf3,0);
    return sel;
}
/* The far call to the mode switch entry: AX bit 0 a 32-bit program, ES
 * the host's data in conventional memory. */
void dpmi_switch(void) {
    u16 ip=pop16(),cs=pop16();
    if(client.active || !host_init()) {jump(cs,ip); set_cf(); return;}
    memset(&client,0,sizeof(client));
    client.active=1; client.bits32=rw(EAX)&1; client.psp=cur_psp; client.host_seg=sreg(SR_ES);
    for(unsigned n=0;n<256;n++) {client.vec_sel[n]=HOST_CODE; client.vec_off[n]=default_vector(n);}
    for(unsigned v=0;v<32;v++) {client.exc_sel[v]=HOST_CODE; client.exc_off[v]=(client.bits32?EXC_DEFAULT32:EXC_DEFAULT16)+v*4;}
    frame_count=0;
    u16 ds=sreg(SR_DS),ss=sreg(SR_SS);
    u16 code=alloc_selectors(1); set_entry(code,(u32)cs<<4,0xffff,0xfb,0);
    u16 data=data_selector((u32)ds<<4,0xffff),stack=ss==ds?data:data_selector((u32)ss<<4,0xffff);
    u16 psp=data_selector((u32)cur_psp<<4,0xff);
    u16 env=peek16(((u32)cur_psp<<4)+0x2c); client.environment=env;
    if(env) poke16(((u32)cur_psp<<4)+0x2c,data_selector((u32)env<<4,0x7fff));
    /* The locked stack, for exceptions, hardware interrupts and callbacks. */
    client.locked=alloc_selectors(1);
    set_entry(client.locked,(u32)(uintptr_t)(host+(LDT_PAGES+1)*4096),LOCKED_SIZE-1,0xf3,client.bits32?0x40:0);
    if(!code || !data || !stack || !psp || !client.locked) {client.active=0; jump(cs,ip); set_cf(); return;}
    u32 sp=rw(ESP);
    cpu.eflags=(cpu.eflags&(FL_ARITH|0x400|FL_IF))|0x3000|2;
    clear_cf();
    tables();
    pm_set(SR_CS,code); pm_set(SR_SS,stack); pm_set(SR_DS,data); pm_set(SR_ES,psp); pm_set(SR_FS,0); pm_set(SR_GS,0);
    cpu.eip=ip; cpu.gr[ESP]=sp;
}
int dpmi_active(void) {return client.active;}

/* Interrupts the machine raises (the timer's INT 8 and 1Ch, IRQs 8-15):
 * their handlers leave the interrupted program's registers and flags. */
static int hardware(unsigned n) {return (n>=8 && n<=0x0f) || n==0x1c || (n>=0x70 && n<=0x77);}
/* In a default handler (one a program's handler chained to), the IRET that
 * comes next takes the flags from its frame: they carry the arithmetic
 * flags the service left, as a DOS call's or INT 31h's carry, and the
 * interrupt flag 0900h and 0901h set. */
static void frame_flags(u64 flags,u16 mask) {
    if(pm_get(SR_CS)!=HOST_CODE || cpu.eip<PM_DEFAULT16+2 || cpu.eip>=PM_DEFAULT32+0x400) return;
    unsigned size=cpu.eip>=PM_DEFAULT32?4:2;
    u8 *frame=pm_pointer(pm_get(SR_SS),sp_value()+2*size,2);
    if(!frame) return;
    u16 f=(u16)(((frame[0]|frame[1]<<8)&~mask)|(flags&mask));
    frame[0]=(u8)f; frame[1]=(u8)(f>>8);
}

/* --- returns from real mode ----------------------------------------------------- */
/* The registers of a real-mode call structure, as they come in it. */
static const unsigned order[8]={EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX};
/* A reflected interrupt or a translation call came back to TRAP_SEG:DPMI_RETURN. */
void dpmi_return(void) {
    if(!frame_count) return;
    Frame *f=&frames[--frame_count];
    IoIa32Context real=cpu;
    resume(&f->state);
    if(f->kind==F_REFLECT) {
        if(hardware(f->vector)) return; /* what it interrupted goes on as it was */
        for(unsigned r=0;r<8;r++) if(r!=ESP) cpu.gr[r]=real.gr[r];
        cpu.eflags=(cpu.eflags&~(u64)FL_ARITH)|(real.eflags&FL_ARITH);
        frame_flags(real.eflags,FL_ARITH);
        return;
    }
    if(f->kind==F_TRANSLATE) {
        u8 *s=pm_pointer(f->struct_sel,f->struct_off,0x32);
        if(!s) return;
        for(unsigned i=0;i<8;i++) if(order[i]!=ESP) {u32 v=(u32)real.gr[order[i]]; s[i*4]=(u8)v; s[i*4+1]=(u8)(v>>8); s[i*4+2]=(u8)(v>>16); s[i*4+3]=(u8)(v>>24);}
        u16 fl=(u16)real.eflags; s[0x20]=(u8)fl; s[0x21]=(u8)(fl>>8);
        u16 segs[4]={sreg_of(&real,SR_ES),sreg_of(&real,SR_DS),sreg_of(&real,SR_FS),sreg_of(&real,SR_GS)};
        for(unsigned i=0;i<4;i++) {s[0x22+i*2]=(u8)segs[i]; s[0x23+i*2]=(u8)(segs[i]>>8);}
        clear_cf(); frame_flags(cpu.eflags,FL_CF);
    }
}

/* --- INT 31h -------------------------------------------------------------- */
static u32 cx_dx(void) {return (u32)rw(ECX)<<16|rw(EDX);}
static u32 bx_cx(void) {return (u32)rw(EBX)<<16|rw(ECX);}
/* 0300h-0302h: the real-mode call structure at ES:(E)DI. */
static void translate(u8 al) {
    u16 sel=pm_get(SR_ES); u32 off=reg_off(EDI),words=rw(ECX); u8 vector=rl(EBX);
    u8 *s=pm_pointer(sel,off,0x32); if(!s || rh(EBX)) {fail(DE_VALUE); return;}
    u32 stack_from=sp_value(); u8 *args=words?pm_pointer(pm_get(SR_SS),stack_from,words*2):NULL;
    if(words && !args) {fail(DE_VALUE); return;}
    Frame *f=push_frame(F_TRANSLATE); if(!f) {fail(DE_UNSUPPORTED); return;}
    f->struct_sel=sel; f->struct_off=off;
    u16 ss=(u16)(s[0x30]|s[0x31]<<8),sp=(u16)(s[0x2e]|s[0x2f]<<8);
    enter_real();
    for(unsigned i=0;i<8;i++) if(order[i]!=ESP) cpu.gr[order[i]]=(u32)(s[i*4]|s[i*4+1]<<8|s[i*4+2]<<16|(u32)s[i*4+3]<<24);
    /* The structure's flags, the interrupt flag included: 0301h runs with
     * them, 0300h and 0302h push them and enter without IF and TF. */
    cpu.eflags=(cpu.eflags&~0xffffULL)|((s[0x20]|s[0x21]<<8)&0x0ed5)|EFLAGS_V86|2;
    set_sreg(SR_ES,(u16)(s[0x22]|s[0x23]<<8)); set_sreg(SR_DS,(u16)(s[0x24]|s[0x25]<<8));
    set_sreg(SR_FS,(u16)(s[0x26]|s[0x27]<<8)); set_sreg(SR_GS,(u16)(s[0x28]|s[0x29]<<8));
    if(ss || sp) {set_sreg(SR_SS,ss); cpu.gr[ESP]=sp;}
    for(u32 i=words;i--;) push16((u16)(args[i*2]|args[i*2+1]<<8));
    if(al==0) {
        u32 v=ivt(vector);
        push16((u16)cpu.eflags); push16(TRAP_SEG); push16(DPMI_RETURN);
        cpu.eflags&=~(u64)(FL_IF|FL_TF);
        jump((u16)(v>>16),(u16)v);
    } else {
        if(al==2) push16((u16)cpu.eflags);
        push16(TRAP_SEG); push16(DPMI_RETURN);
        if(al==2) cpu.eflags&=~(u64)(FL_IF|FL_TF);
        jump((u16)(s[0x2c]|s[0x2d]<<8),(u16)(s[0x2a]|s[0x2b]<<8));
    }
}
static void memory_info(void) {
    u8 *p=pm_pointer(pm_get(SR_ES),reg_off(EDI),0x30); if(!p) {fail(DE_VALUE); return;}
    u64 free_pages=0,run=0; u32 room_kb=dpmi_room_kb();
    if(io->range_free(io->context,0x110000,0x100000000ULL,&free_pages,&run)) free_pages=run=0;
    u64 largest=run*4096; if(largest>(u64)room_kb*1024) largest=(u64)room_kb*1024;
    memset(p,0xff,0x30);
    u32 v=(u32)largest; p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24);
    clear_cf();
}
static unsigned block_handle(u32 handle) {
    unsigned i=(handle&0xffff)-1;
    return (handle>>16)==1 && i<BLOCKS && blocks[i].memory?i:BLOCKS;
}
static int new_block(u32 bytes,unsigned *index) {
    unsigned i; for(i=0;i<BLOCKS && blocks[i].memory;i++) {}
    if(i==BLOCKS || !bytes) return DE_HANDLE;
    if(bytes>0xfffff000U) return DE_LINEAR;
    u32 pages=(bytes+4095)/4096;
    if(pages*4>dpmi_room_kb()) return DE_LINEAR;
    void *m; if(io->alloc_pages_range(io->context,pages,0x110000,0x100000000ULL,&m)) return DE_LINEAR;
    memset(m,0,pages*4096ULL);
    blocks[i]=(MemBlock){m,bytes,pages}; used_kb+=pages*4; *index=i; return 0;
}
static void free_block(unsigned i) {io->free_pages(io->context,blocks[i].memory,blocks[i].pages); used_kb-=blocks[i].pages*4; blocks[i].memory=NULL;}
static void answer_block(unsigned i) {
    u32 a=(u32)(uintptr_t)blocks[i].memory,h=0x10000|(i+1);
    ww(EBX,(u16)(a>>16)); ww(ECX,(u16)a); ww(ESI,(u16)(h>>16)); ww(EDI,(u16)h); clear_cf();
}
/* 0100h's blocks: the one whose first selector is sel, or a free entry (0). */
static DosBlock *dos_block(u16 sel) {
    for(unsigned i=0;i<DOS_BLOCKS;i++)
        if(sel?client.dos_blocks[i].count && client.dos_blocks[i].selector==sel:!client.dos_blocks[i].count) return &client.dos_blocks[i];
    return NULL;
}
static unsigned selectors_for(u16 paragraphs) {return paragraphs?(paragraphs+0xfffu)/0x1000u:1;}
/* Its descriptors, 64 KiB apart; for a 32-bit program the first spans it. */
static void dos_block_descriptors(const DosBlock *k,u16 paragraphs) {
    u32 base=(u32)k->segment<<4,bytes=(u32)paragraphs*16;
    for(unsigned i=0;i<k->count;i++) {
        u32 left=bytes>i*0x10000u?bytes-i*0x10000u:0;
        set_entry((u16)(k->selector+i*8),base+i*0x10000,(left>0x10000?0x10000:left?left:1)-1,0xf3,0);
    }
    if(client.bits32 && k->count>1) set_entry(k->selector,base,bytes-1,0xf3,0x40);
}
static void dpmi_int31(void) {
    u16 ax=rw(EAX),bx=rw(EBX); u8 *p; u16 sel;
    clear_cf();
    switch(ax) {
    case 0x0000: {
        u16 n=rw(ECX); if(!n || !(sel=alloc_selectors(n))) {fail(DE_DESCRIPTOR); return;}
        ww(EAX,sel); return;
    }
    case 0x0001: if(!valid(bx)) {fail(DE_SELECTOR); return;} free_selector(bx); return;
    case 0x0002: if(!(sel=segment_selector(bx))) {fail(DE_DESCRIPTOR); return;} ww(EAX,sel); return;
    case 0x0003: ww(EAX,8); return;
    case 0x0004: case 0x0005: return;
    case 0x0006: {
        if(!host_or_valid(bx)) {fail(DE_SELECTOR); return;}
        u32 b=base_of(entry(bx)); ww(ECX,(u16)(b>>16)); ww(EDX,(u16)b); return;
    }
    case 0x0007: {
        if(!valid(bx)) {fail(DE_SELECTOR); return;}
        u8 *e=entry(bx); u32 b=cx_dx();
        e[2]=(u8)b; e[3]=(u8)(b>>8); e[4]=(u8)(b>>16); e[7]=(u8)(b>>24);
        reload_segments(); return;
    }
    case 0x0008: {
        if(!valid(bx)) {fail(DE_SELECTOR); return;}
        u32 limit=cx_dx(); u8 *e=entry(bx);
        if(limit>0xfffff && (limit&0xfff)!=0xfff) {fail(DE_VALUE); return;}
        u8 flags=(u8)(e[6]&0x70); set_entry(bx,base_of(e),limit,e[5],flags);
        reload_segments(); return;
    }
    case 0x0009: {
        if(!valid(bx)) {fail(DE_SELECTOR); return;}
        u8 access=rl(ECX),ext=rh(ECX),*e=entry(bx);
        if((access&0x70)!=0x70) {fail(DE_VALUE); return;}
        e[5]=(u8)(access|1); e[6]=(u8)((e[6]&0x0f)|(ext&0xd0));
        reload_segments(); return;
    }
    case 0x000a: {
        if(!valid(bx)) {fail(DE_SELECTOR); return;}
        if(!(sel=alloc_selectors(1))) {fail(DE_DESCRIPTOR); return;}
        memcpy(entry(sel),entry(bx),8); entry(sel)[5]=(u8)((entry(sel)[5]&0xf0)|0x03);
        ww(EAX,sel); return;
    }
    case 0x000b: case 0x000c: {
        if(!valid(bx)) {fail(DE_SELECTOR); return;}
        if(!(p=pm_pointer(pm_get(SR_ES),reg_off(EDI),8))) {fail(DE_VALUE); return;}
        if(ax==0x000b) {memcpy(p,entry(bx),8); return;}
        if((p[5]&0x70)!=0x70 && (p[5]&0x80)) {fail(DE_VALUE); return;}
        memcpy(entry(bx),p,8); entry(bx)[5]|=1; reload_segments(); return;
    }
    case 0x0100: {
        DosBlock *k=dos_block(0); if(!k) {fail(DE_DESCRIPTOR); return;}
        u16 seg,largest; int e=mem_alloc(bx,cur_psp,&seg,&largest);
        if(e) {ww(EBX,largest); fail((u16)e); return;}
        unsigned n=selectors_for(bx);
        if(!(sel=alloc_selectors(n))) {mem_free(seg); fail(DE_DESCRIPTOR); return;}
        *k=(DosBlock){seg,sel,(u16)n};
        dos_block_descriptors(k,bx);
        ww(EAX,seg); ww(EDX,sel); return;
    }
    case 0x0101: {
        DosBlock *k=dos_block(rw(EDX)); if(!k) {fail(DE_SELECTOR); return;}
        if(mem_free(k->segment)) {fail(DE_SELECTOR); return;}
        for(unsigned i=0;i<k->count;i++) free_selector((u16)(k->selector+i*8));
        memset(k,0,sizeof(*k)); return;
    }
    case 0x0102: {
        DosBlock *k=dos_block(rw(EDX)); u16 largest; if(!k) {fail(DE_SELECTOR); return;}
        /* More selectors than it has must follow its own, free. */
        unsigned n=selectors_for(bx);
        for(unsigned i=k->count;i<n;i++) {
            unsigned index=(k->selector>>3)+i;
            if(index>=ENTRIES || owner[index]) {fail(DE_DESCRIPTOR); return;}
        }
        int e=mem_resize(k->segment,bx,&largest); if(e) {ww(EBX,largest); fail((u16)e); return;}
        for(unsigned i=k->count;i<n;i++) owner[(k->selector>>3)+i]=2;
        for(unsigned i=n;i<k->count;i++) free_selector((u16)(k->selector+i*8));
        k->count=(u16)n; dos_block_descriptors(k,bx);
        reload_segments(); return;
    }
    case 0x0200: {u32 v=ivt(rl(EBX)); ww(ECX,(u16)(v>>16)); ww(EDX,(u16)v); return;}
    case 0x0201: set_ivt(rl(EBX),(u32)rw(ECX)<<16|rw(EDX)); return;
    case 0x0202: case 0x0203: {
        u8 v=rl(EBX); if(v>=32) {fail(DE_VALUE); return;}
        if(ax==0x0202) {ww(ECX,client.exc_sel[v]); if(client.bits32) cpu.gr[EDX]=client.exc_off[v]; else ww(EDX,(u16)client.exc_off[v]); return;}
        if(!usable_code(rw(ECX))) {fail(DE_SELECTOR); return;}
        client.exc_sel[v]=rw(ECX); client.exc_off[v]=reg_off(EDX); return;
    }
    case 0x0204: {
        u8 n=rl(EBX); ww(ECX,client.vec_sel[n]);
        if(client.bits32) cpu.gr[EDX]=client.vec_off[n]; else ww(EDX,(u16)client.vec_off[n]);
        return;
    }
    case 0x0205: {
        u8 n=rl(EBX); if(!usable_code(rw(ECX))) {fail(DE_SELECTOR); return;}
        client.vec_sel[n]=rw(ECX); client.vec_off[n]=reg_off(EDX); return;
    }
    case 0x0300: case 0x0301: case 0x0302: translate(rl(EAX)); return;
    case 0x0303: {
        unsigned i; for(i=0;i<CALLBACKS && callbacks[i].used;i++) {}
        if(i==CALLBACKS) {fail(DE_CALLBACK); return;}
        /* DS:(E)SI is the procedure. */
        callbacks[i]=(Callback){1,pm_get(SR_DS),reg_off(ESI),pm_get(SR_ES),reg_off(EDI)};
        ww(ECX,TRAP_SEG); ww(EDX,(u16)(DPMI_CALLBACKS+i*2)); return;
    }
    case 0x0304: {
        unsigned i=(rw(EDX)-DPMI_CALLBACKS)/2;
        if(rw(ECX)!=TRAP_SEG || rw(EDX)<DPMI_CALLBACKS || i>=CALLBACKS || !callbacks[i].used) {fail(DE_CALLBACK); return;}
        callbacks[i].used=0; return;
    }
    case 0x0305:
        ww(EAX,0); ww(EBX,TRAP_SEG); ww(ECX,DPMI_SAVE);
        ww(ESI,HOST_CODE); if(client.bits32) cpu.gr[EDI]=SR_PM32; else ww(EDI,SR_PM16);
        return;
    case 0x0306:
        ww(EBX,TRAP_SEG); ww(ECX,DPMI_RAW);
        ww(ESI,HOST_CODE); if(client.bits32) cpu.gr[EDI]=RAW_PM; else ww(EDI,RAW_PM);
        return;
    case 0x0400: ww(EAX,0x005a); ww(EBX,1); wl(ECX,4); ww(EDX,0x0870); return;
    case 0x0500: memory_info(); return;
    case 0x0501: {
        unsigned i; int e=new_block(bx_cx(),&i); if(e) {fail((u16)e); return;}
        answer_block(i); return;
    }
    case 0x0502: {
        unsigned i=block_handle((u32)rw(ESI)<<16|rw(EDI)); if(i==BLOCKS) {fail(DE_HANDLE); return;}
        free_block(i); return;
    }
    case 0x0503: {
        unsigned i=block_handle((u32)rw(ESI)<<16|rw(EDI)),k; if(i==BLOCKS) {fail(DE_HANDLE); return;}
        u32 bytes=bx_cx(); int e=new_block(bytes,&k); if(e) {fail((u16)e); return;}
        memcpy(blocks[k].memory,blocks[i].memory,blocks[i].bytes<bytes?blocks[i].bytes:bytes);
        free_block(i); blocks[i]=blocks[k]; blocks[k].memory=NULL;
        answer_block(i); return;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0702: case 0x0703: return;
    case 0x0604: ww(EBX,0); ww(ECX,0x1000); return;
    /* Linear addresses are physical: a device's memory maps to itself. */
    case 0x0800: if(!rw(ESI) && !rw(EDI)) fail(DE_VALUE); return;
    case 0x0801: return;
    case 0x0900: wl(EAX,(cpu.eflags&FL_IF)?1:0); cpu.eflags&=~(u64)FL_IF; return;
    case 0x0901: wl(EAX,(cpu.eflags&FL_IF)?1:0); cpu.eflags|=FL_IF; return;
    case 0x0902: wl(EAX,(cpu.eflags&FL_IF)?1:0); return;
    default: fail(DE_UNSUPPORTED); return;
    }
}

/* --- protected mode's interruptions ---------------------------------------- */
/* Where the host's next frame on the locked stack goes: below what is in
 * use when the program is on it already (a handler interrupted there), else
 * the top of this depth's part. */
static u32 locked_top(void) {
    if(pm_get(SR_SS)==client.locked) return (sp_value()&~3u)-0x20;
    return LOCKED_SIZE-0x100*(frame_count>15?15:frame_count);
}
/* Protected mode as the program last had it: the state kept by the
 * innermost of the first n frames that runs real-mode work for it, or NULL. */
static const IoIa32Context *pm_state(unsigned n) {
    while(n--) if(frames[n].kind==F_REFLECT || frames[n].kind==F_TRANSLATE) return &frames[n].state;
    return NULL;
}
/* A callback's real-mode call came to TRAP_SEG:DPMI_CALLBACKS+2i: the
 * registers into the structure, the procedure entered on the locked stack
 * with DS:(E)SI the real-mode stack and ES:(E)DI the structure. */
void dpmi_callback(unsigned i) {
    if(i>=CALLBACKS || !callbacks[i].used) return;
    Callback *c=&callbacks[i];
    u8 *s=pm_pointer(c->struct_sel,c->struct_off,0x32); if(!s) return;
    for(unsigned k=0;k<8;k++) {u32 v=(u32)cpu.gr[order[k]]; if(order[k]==ESP) v=rw(ESP); s[k*4]=(u8)v; s[k*4+1]=(u8)(v>>8); s[k*4+2]=(u8)(v>>16); s[k*4+3]=(u8)(v>>24);}
    u16 fl=(u16)cpu.eflags; s[0x20]=(u8)fl; s[0x21]=(u8)(fl>>8);
    u16 segs[4]={sreg(SR_ES),sreg(SR_DS),sreg(SR_FS),sreg(SR_GS)};
    for(unsigned k=0;k<4;k++) {s[0x22+k*2]=(u8)segs[k]; s[0x23+k*2]=(u8)(segs[k]>>8);}
    u16 ip=(u16)cpu.eip,cs=sreg(SR_CS); s[0x2a]=(u8)ip; s[0x2b]=(u8)(ip>>8); s[0x2c]=(u8)cs; s[0x2d]=(u8)(cs>>8);
    u16 sp=rw(ESP),ss=sreg(SR_SS); s[0x2e]=(u8)sp; s[0x2f]=(u8)(sp>>8); s[0x30]=(u8)ss; s[0x31]=(u8)(ss>>8);
    Frame *f=push_frame(F_CALLBACK); if(!f) return;
    f->struct_sel=c->struct_sel; f->struct_off=c->struct_off;
    /* Protected mode as the program last had it, then the callback's state. */
    const IoIa32Context *pm=pm_state(frame_count-1);
    if(pm) resume(pm); else {tables(); cpu.eflags=(cpu.eflags&FL_ARITH)|0x3002;}
    cpu.eflags=(cpu.eflags&~(u64)(FL_IF|FL_TF|0x20000))|0x3000;
    u16 stack_sel=segment_selector(ss);
    pm_set(SR_DS,stack_sel); if(client.bits32) cpu.gr[ESI]=sp; else ww(ESI,sp);
    pm_set(SR_ES,c->struct_sel); if(client.bits32) cpu.gr[EDI]=c->struct_off; else ww(EDI,(u16)c->struct_off);
    u32 top=locked_top(); pm_set(SR_SS,client.locked); cpu.gr[ESP]=top;
    unsigned size=word_size();
    pm_push(0x3002|((u32)cpu.eflags&FL_ARITH),size); pm_push(HOST_CODE,size); pm_push(CB_RETURN,size);
    pm_jump(c->sel,c->off);
}
/* The callback's IRET came to CB_RETURN: real mode from the structure at ES:(E)DI. */
static void callback_return(void) {
    if(!frame_count || frames[frame_count-1].kind!=F_CALLBACK) {end_client("callback return"); return;}
    Frame *f=&frames[--frame_count];
    u8 *s=pm_pointer(pm_get(SR_ES),reg_off(EDI),0x32);
    resume(&f->state);
    if(!s) return;
    for(unsigned k=0;k<8;k++) if(order[k]!=ESP) cpu.gr[order[k]]=(u32)(s[k*4]|s[k*4+1]<<8|s[k*4+2]<<16|(u32)s[k*4+3]<<24);
    cpu.eflags=(cpu.eflags&~0xffffULL)|((s[0x20]|s[0x21]<<8)&0x0fd5)|EFLAGS_V86|2;
    set_sreg(SR_ES,(u16)(s[0x22]|s[0x23]<<8)); set_sreg(SR_DS,(u16)(s[0x24]|s[0x25]<<8));
    set_sreg(SR_FS,(u16)(s[0x26]|s[0x27]<<8)); set_sreg(SR_GS,(u16)(s[0x28]|s[0x29]<<8));
    jump((u16)(s[0x2c]|s[0x2d]<<8),(u16)(s[0x2a]|s[0x2b]<<8));
    set_sreg(SR_SS,(u16)(s[0x30]|s[0x31]<<8)); cpu.gr[ESP]=(u16)(s[0x2e]|s[0x2f]<<8);
}
/* The raw switch from real mode (TRAP_SEG:DPMI_RAW): DS=AX, ES=CX, SS=DX,
 * (E)SP=BX, CS=SI, (E)IP=DI. */
void dpmi_raw_to_pm(void) {
    u16 ds=rw(EAX),es=rw(ECX),ss=rw(EDX),cs=rw(ESI); u32 sp=client.bits32?(u32)cpu.gr[EBX]:rw(EBX),ip=client.bits32?(u32)cpu.gr[EDI]:rw(EDI);
    if(!usable_code(cs) || !usable_data(ds) || !usable_data(es) || !usable_data(ss) || ss<4) {end_client("raw switch"); return;}
    cpu.eflags=(cpu.eflags&(FL_ARITH|0x400|FL_IF))|0x3002;
    tables();
    pm_set(SR_CS,cs); pm_set(SR_DS,ds); pm_set(SR_ES,es); pm_set(SR_SS,ss); pm_set(SR_FS,0); pm_set(SR_GS,0);
    cpu.eip=ip; cpu.gr[ESP]=sp;
}
static void raw_to_real(void) {
    u16 ds=rw(EAX),es=rw(ECX),ss=rw(EDX),cs=rw(ESI); u16 sp=rw(EBX),ip=rw(EDI);
    cpu.eflags=(cpu.eflags&(FL_ARITH|0x400|FL_IF))|EFLAGS_V86;
    set_sreg(SR_DS,ds); set_sreg(SR_ES,es); set_sreg(SR_SS,ss); set_sreg(SR_FS,0); set_sreg(SR_GS,0);
    cpu.gr[ESP]=sp; jump(cs,ip);
}
/* An exception handler returned (RETF to EXC_RETURN): the program goes on
 * from the frame, which the handler may have changed. */
static void exception_return(void) {
    unsigned size=word_size(); u32 code,ip,cs,flags,sp,ss;
    if(!pm_pop(&code,size) || !pm_pop(&ip,size) || !pm_pop(&cs,size) || !pm_pop(&flags,size) || !pm_pop(&sp,size) || !pm_pop(&ss,size)) {end_client("exception frame"); return;}
    if(!usable_code((u16)cs) || !usable_data((u16)ss) || (u16)ss<4) {end_client("exception frame"); return;}
    cpu.eflags=(flags&0x0fd5)|0x3002;
    pm_set(SR_SS,(u16)ss); cpu.gr[ESP]=sp; pm_jump((u16)cs,ip);
}
static void exception(unsigned v) {
    /* Port I/O and HLT are the machine's: emulated as in real mode. */
    if(v==13 && dpmi_ports()) return;
    if(v<32 && !(client.exc_sel[v]==HOST_CODE && client.exc_off[v]==(client.bits32?EXC_DEFAULT32:EXC_DEFAULT16)+v*4)) {
        u32 code=cpu.code,ip=(u32)cpu.eip,flags=(u32)cpu.eflags,sp=(u32)cpu.gr[ESP]; u16 cs=pm_get(SR_CS),ss=pm_get(SR_SS);
        if(!stack32()) sp&=0xffff;
        unsigned size=word_size();
        u32 top=ss==client.locked?(sp&~3u)-0x20:LOCKED_SIZE-0x100*(frame_count>15?15:frame_count)-0x80;
        pm_set(SR_SS,client.locked); cpu.gr[ESP]=top;
        if(!pm_push(ss,size) || !pm_push(sp,size) || !pm_push(flags,size) || !pm_push(cs,size) || !pm_push(ip,size) ||
           !pm_push(code,size) || !pm_push(HOST_CODE,size) || !pm_push(EXC_RETURN,size)) {end_client("exception stack"); return;}
        cpu.eflags&=~(u64)(FL_IF|FL_TF);
        pm_jump(client.exc_sel[v],client.exc_off[v]);
        return;
    }
    if(v<=5 || v==7) {
        /* Faults (all but 1, 3 and 4): the handler sees the faulting instruction. */
        if(client.vec_sel[v]==HOST_CODE && client.vec_off[v]==default_vector(v)) {
            if(v==1 || v==3) {cpu.eflags&=~(u64)FL_TF; return;}
            end_client(v==0?"divide error":"processor exception"); return;
        }
        deliver(client.vec_sel[v],client.vec_off[v]); return;
    }
    end_client(v==13?"general protection fault":v==12?"stack fault":v==11?"segment not present":v==6?"invalid instruction":"processor exception");
}
/* INT 67h at a place in the host's code. */
static void host_trap(u32 at) {
    if(at<PM_DEFAULT32+0x400 && !(at&3)) {
        unsigned n=(at-(at>=PM_DEFAULT32?PM_DEFAULT32:PM_DEFAULT16))/4; u16 ax=rw(EAX);
        default_action(n);
        /* Done here, not reflected to real mode. */
        if(!(cpu.eflags&0x20000)) frame_flags(cpu.eflags,n!=0x31?FL_ARITH:ax==0x0900 || ax==0x0901?FL_CF|FL_IF:FL_CF);
        return;
    }
    if(at>=EXC_DEFAULT16 && at<EXC_DEFAULT32+0x80 && !(at&3)) {
        /* A handler chained to the default: as if none were set. */
        unsigned v=((at-EXC_DEFAULT16)&0x7f)/4;
        end_client(v==13?"general protection fault":"processor exception"); return;
    }
    switch(at) {
    case EXC_RETURN: exception_return(); return;
    case CB_RETURN: callback_return(); return;
    case HW_RETURN: real_interrupt_return(); return;
    case RAW_PM: raw_to_real(); return;
    default: end_client("host entry");
    }
}
static void pm_iret(void) {
    unsigned size=cpu.code&2?4:2; u32 ip,cs,flags;
    if(!pm_pop(&ip,size) || !pm_pop(&cs,size) || !pm_pop(&flags,size)) {end_client("stack fault"); return;}
    if(!usable_code((u16)cs)) {end_client("bad IRET"); return;}
    if(size==2) flags=(flags&0xffff)|((u32)cpu.eflags&0xffff0000U);
    cpu.eflags=(flags&0x240fd5)|0x3002; /* the flags a program may set, IOPL 3 */
    pm_jump((u16)cs,ip);
}
/* Each interruption in protected mode. */
void dpmi_exit(void) {
    if(cpu.exit==IO_IA32_INTERRUPT) {
        unsigned n=cpu.vector;
        if(pm_get(SR_CS)==HOST_CODE && n==0x67) {host_trap((u32)cpu.eip-2); return;}
        if(client.vec_sel[n]==HOST_CODE && client.vec_off[n]==default_vector(n)) default_action(n);
        else deliver(client.vec_sel[n],client.vec_off[n]);
        return;
    }
    if(cpu.exit==IO_IA32_EXCEPTION) {exception(cpu.vector); return;}
    if(cpu.exit==IO_IA32_INTERCEPT) {
        if(cpu.vector==2) return; /* MOV SS, POPF of TF: resume */
        if(cpu.vector==0 && (u8)cpu.iim==0xcf) {pm_iret(); return;}
        if(cpu.vector==0 && (u8)cpu.iim==0xf4) {io->stall_us(io->context,1000); cpu.eip+=1+((cpu.code>>12)&15); return;}
        end_client("unsupported instruction");
    }
}
/* The timer, while the program runs in protected mode: its INT 8 or 1Ch
 * handler, as the interrupt would. */
static int pm_handler(unsigned n) {return client.vec_sel[n]!=HOST_CODE || client.vec_off[n]!=default_vector(n);}
int dpmi_timer(void) {
    if(!(cpu.eflags&FL_IF) || irq0_masked() || irq0_in_service) return 0;
    /* Its protected-mode handler, else the real-mode ones (reflected). */
    if(pm_handler(8)) {irq0_in_service=1; deliver(client.vec_sel[8],client.vec_off[8]); return 1;}
    if(ivt(8)!=stub(8)) {irq0_in_service=1; reflect(8); return 1;}
    if(pm_handler(0x1c)) {deliver(client.vec_sel[0x1c],client.vec_off[0x1c]); return 1;}
    if(ivt(0x1c)!=stub(0x1c)) {reflect(0x1c); return 1;}
    return 0;
}
/* The timer's interrupts, INT 1Ch and INT 23h, while the program's
 * real-mode work runs: to its protected-mode handler if it set one, on the
 * locked stack in protected mode as the program last had it, and back to
 * real mode where it was (HW_RETURN). 1 when taken. */
int dpmi_real_interrupt(unsigned n) {
    if(!client.active || !pm_handler(n)) return 0;
    const IoIa32Context *pm=pm_state(frame_count);
    if(!pm) return 0;
    Frame *f=push_frame(F_HARDWARE); if(!f) return 0;
    f->vector=n;
    u64 flags=cpu.eflags;
    resume(pm);
    cpu.eflags=(cpu.eflags&~(u64)(FL_IF|FL_TF|FL_ARITH))|(flags&FL_ARITH)|0x3002;
    u32 top=locked_top(); pm_set(SR_SS,client.locked); cpu.gr[ESP]=top;
    unsigned size=word_size();
    pm_push(((u32)flags&0xfd5)|0x3002,size); pm_push(HOST_CODE,size); pm_push(HW_RETURN,size);
    pm_jump(client.vec_sel[n],client.vec_off[n]);
    return 1;
}
/* Its IRET came to HW_RETURN: real mode as it was. */
static void real_interrupt_return(void) {
    if(!frame_count || frames[frame_count-1].kind!=F_HARDWARE) {end_client("interrupt return"); return;}
    Frame *f=&frames[--frame_count]; resume(&f->state);
}
static void release(void) {
    for(unsigned i=FIRST_CLIENT;i<ENTRIES;i++) if(owner[i]==2) {memset(host+i*8,0,8); owner[i]=0;}
    for(unsigned i=0;i<BLOCKS;i++) if(blocks[i].memory) free_block(i);
    memset(callbacks,0,sizeof(callbacks));
    frame_count=0;
}
/* The program ends (INT 21h 4Ch and the like): its environment segment
 * back in its PSP and all it had freed. */
void dpmi_terminate(u16 psp) {
    if(!client.active || client.psp!=psp) return;
    if(client.environment) poke16(((u32)psp<<4)+0x2c,client.environment);
    release();
    client.active=0;
}
static void end_client(const char *why) {
    char text[80]; unsigned n=0; const char *head="VDM: DPMI program ended: ";
    while(*head) text[n++]=*head++;
    while(*why && n<76) text[n++]=*why++;
    text[n++]='\r'; text[n++]='\n';
    console_out((const u8 *)text,n);
    /* Real mode again for the end. */
    cpu.eflags=(cpu.eflags&FL_ARITH)|EFLAGS_V86;
    for(unsigned s=SR_ES;s<=SR_GS;s++) set_sreg(s,client.psp);
    request_exit(255,0,0);
}
void dpmi_close(void) {
    if(!host) return;
    release();
    io->free_pages(io->context,host,HOST_PAGES);
    host=NULL; client.active=0;
}
u32 dpmi_used_kb(void) {return used_kb;}
