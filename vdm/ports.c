/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest IN/OUT/INS/OUTS trap with CFLG.io clear. VGA ports are forwarded
 * to the claimed adapter (vga.c); other devices are emulated: PIT counts
 * from the millisecond clock, port 61h toggles refresh, and CMOS reads DOS
 * time. Timer delivery follows counter 0's reload, PIC mask and EOI.
 * Unhandled ports read as all ones; writes only update latch/index state.
 */
#include "vdm.h"
static u8 cmos_index,pic_mask=0xb8,pit_flip,pit_latched,toggle,refresh,pit_access,pit_high_next;
static u16 pit_latch,pit_reload;
int irq0_in_service;

/* Counter 0 runs at 1193182 Hz and reloads at its reload value (0: 65536). */
static u32 reload(void) {return pit_reload?pit_reload:0x10000U;}
static u16 pit_count(void) {
    u64 ms=io->ticks_ms(io->context);
    return (u16)(reload()-(ms*1193182ULL/1000ULL)%reload());
}
u32 timer_period_us(void) {return (u32)((u64)reload()*1000000ULL/1193182ULL);}
int irq0_masked(void) {return pic_mask&1;}
static u8 cmos(u8 index) {
    DosDateTime t;
    switch(index) {
    case 0x0a: return 0x26;
    case 0x0b: return 0x02;
    case 0x0d: return 0x80;
    case 0x0e: case 0x0f: return 0;
    case 0x10: return 0x40; /* one 1.44 MB diskette type */
    }
    if(dos_get_datetime(&t)) return 0;
    switch(index) {
    case 0x00: return bcd(t.second);
    case 0x02: return bcd(t.minute);
    case 0x04: return bcd(t.hour);
    case 0x06: return bcd(t.weekday+1);
    case 0x07: return bcd(t.day);
    case 0x08: return bcd(t.month);
    case 0x09: return bcd(t.year%100);
    case 0x32: return bcd(t.year/100);
    default: return 0;
    }
}
static u8 in8(u16 port) {
    u8 v;
    if(port>=0x3b0 && port<=0x3df && video_port(port,&v,0)) return v;
    switch(port) {
    case 0x20: case 0xa0: return 0;
    case 0x21: return pic_mask;
    case 0x40: {
        u16 v=pit_latched?pit_latch:pit_count();
        u8 b=pit_flip?(u8)(v>>8):(u8)v;
        if(pit_flip) pit_latched=0;
        pit_flip^=1; return b;
    }
    case 0x60: return 0;
    case 0x61: refresh^=0x10; return (u8)(0x20|refresh);
    case 0x64: return 0x1c;
    case 0x71: return cmos(cmos_index&0x7f);
    case 0x3ba: case 0x3da: toggle^=1; return toggle?0x09:0x00;
    default: return 0xff;
    }
}
static void out8(u16 port,u8 v) {
    if(port>=0x3b0 && port<=0x3df && video_port(port,&v,1)) return;
    switch(port) {
    case 0x20: if(v==0x20 || v==0x60) irq0_in_service=0; break; /* end of interrupt */
    case 0x21: pic_mask=v; break;
    case 0x40:
        if(pit_access==1) pit_reload=(u16)((pit_reload&0xff00)|v);
        else if(pit_access==2) pit_reload=(u16)((pit_reload&0xff)|v<<8);
        else if(pit_access==3) {
            if(pit_high_next) pit_reload=(u16)((pit_reload&0xff)|v<<8); else pit_reload=(u16)((pit_reload&0xff00)|v);
            pit_high_next^=1;
        }
        break;
    case 0x43:
        if(!(v&0xc0) && !(v&0x30)) {pit_latch=pit_count(); pit_latched=1; pit_flip=0;}
        else {pit_flip=0; if(!(v&0xc0)) {pit_access=(u8)((v>>4)&3); pit_high_next=0;}}
        break;
    case 0x70: cmos_index=v; break;
    default: break;
    }
}
u32 port_in(u16 port,unsigned size) {
    u32 v=0;
    for(unsigned i=0;i<size;i++) v|=(u32)in8((u16)(port+i))<<(i*8);
    return v;
}
void port_out(u16 port,unsigned size,u32 value) {
    for(unsigned i=0;i<size;i++) out8((u16)(port+i),(u8)(value>>(i*8)));
}

/* Decodes the faulting instruction; returns 1 when it was port I/O, HLT or
 * a read of a control register and has been carried out. Those reads are
 * what V86 monitors (EMM386, Windows) answer for DOS extenders that look
 * at CR0 before they find VCPI or DPMI: the processor's CR0 and CR4, and
 * CR2 and CR3 as 0 (no paging). */
int emulate_privileged(void) {
    u16 cs=sreg(SR_CS),ip=(u16)cpu.eip;
    unsigned n=0,size32=0,rep=0,seg=SR_DS;
    u8 op;
    for(;;) {
        op=*LIN(cs,(u16)(ip+n));
        if(op==0x66) size32^=1;
        else if(op==0xf3 || op==0xf2) rep=1;
        else if(op==0x26) seg=SR_ES; else if(op==0x2e) seg=SR_CS; else if(op==0x36) seg=SR_SS;
        else if(op==0x3e) seg=SR_DS; else if(op==0x64) seg=SR_FS; else if(op==0x65) seg=SR_GS;
        else if(op!=0x67 && op!=0xf0) break;
        if(++n>14) return 0;
    }
    n++;
    unsigned size=(op&1)?(size32?4:2):1;
    u16 port=rw(EDX);
    switch(op) {
    case 0xe4: case 0xe5: case 0xe6: case 0xe7: port=*LIN(cs,(u16)(ip+n)); n++; break;
    case 0xec: case 0xed: case 0xee: case 0xef: case 0x6c: case 0x6d: case 0x6e: case 0x6f: break;
    case 0xf4: io->stall_us(io->context,1000); cpu.eip=(u16)(ip+n); return 1;
    case 0x0f: {
        u8 second=*LIN(cs,(u16)(ip+n)),modrm=*LIN(cs,(u16)(ip+n+1)); unsigned cr=(modrm>>3)&7;
        if(second!=0x20 || (modrm>>6)!=3 || cr==1 || cr>4) return 0;
        cpu.gr[modrm&7]=cr==0?(u32)cpu.cflg:cr==4?(u32)(cpu.cflg>>32):0;
        cpu.eip=(u16)(ip+n+2); return 1;
    }
    default: return 0;
    }
    if(op==0xe4 || op==0xe5 || op==0xec || op==0xed) {
        u32 v=port_in(port,size);
        if(size==1) wl(EAX,(u8)v); else if(size==2) ww(EAX,(u16)v); else cpu.gr[EAX]=v;
    } else if(op==0xe6 || op==0xe7 || op==0xee || op==0xef) {
        port_out(port,size,(u32)cpu.gr[EAX]);
    } else {
        /* INS/OUTS, repeated CX times when prefixed. */
        u16 count=rep?rw(ECX):1; int step=(cpu.eflags&0x400)?-(int)size:(int)size;
        for(u16 i=0;i<count;i++) {
            if(op<=0x6d) {
                u32 v=port_in(port,size); u8 *d=LIN(sreg(SR_ES),rw(EDI));
                for(unsigned b=0;b<size;b++) d[b]=(u8)(v>>(b*8));
                ww(EDI,(u16)(rw(EDI)+step));
            } else {
                const u8 *s=LIN(sreg(seg),rw(ESI)); u32 v=0;
                for(unsigned b=0;b<size;b++) v|=(u32)s[b]<<(b*8);
                port_out(port,size,v); ww(ESI,(u16)(rw(ESI)+step));
            }
        }
        if(rep) ww(ECX,0);
    }
    cpu.eip=(u16)(ip+n); return 1;
}
