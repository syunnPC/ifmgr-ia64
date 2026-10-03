/* SPDX-License-Identifier: GPL-2.0-or-later
 * INT 21h for 16-bit programs, on top of the native DOS API. Register
 * conventions follow DOS; segment:offset arguments become native pointers
 * (conventional memory is identity-mapped), DOS handles go through the
 * program's job file table, and results that native DOS returns in its own
 * structures (search records, country data, disk data) are converted.
 * Memory allocation, EXEC, PSPs, vectors and the DTA belong to the VDM.
 */
#include "vdm.h"
static u64 ptr(unsigned s,unsigned r) {return (u64)LIN(sreg(s),rw(r));}
static void result(int e) {
    if(e==DE_BREAK) {break_pending=1; fail(DE_FUNCTION);}
    else if(e) fail(e);
    else clear_cf();
}
static void copy_out(char *dst,const char *src,unsigned capacity) {
    unsigned n=0; while(src[n] && n<capacity-1) {dst[n]=src[n]; n++;}
    dst[n]=0;
}

/* Directory searches: the native record lives here, indexed from the
 * program's DTA so that a copied DTA continues the same search. */
#define SEARCHES 16
static DosFind searches[SEARCHES];
static unsigned next_search;
static void search(int first) {
    u8 *d=LINEAR(dta); unsigned slot;
    if(first) {slot=next_search++%SEARCHES; memset(&searches[slot],0,sizeof(DosFind));}
    else {
        if(memcmp(d+1,"VDM",3) || d[0]>=SEARCHES) {fail(DE_NOMORE); return;}
        slot=d[0];
    }
    DosRegs set={.ax=0x1a01,.dx=(u64)&searches[slot],.cx=sizeof(DosFind)};
    dos_call(&set);
    DosRegs r={.ax=first?0x4e00:0x4f00,.cx=rw(ECX),.dx=first?ptr(SR_DS,EDX):0};
    int e=dos_call(&r);
    sync_dta();
    if(e) {result(e); return;}
    const DosFind *f=&searches[slot];
    memset(d,0,43); d[0]=(u8)slot; memcpy(d+1,"VDM",3);
    d[21]=f->attr; poke16(dta+22,f->time); poke16(dta+24,f->date); poke32(dta+26,f->size);
    copy_out((char *)d+30,f->name,13);
    clear_cf();
}

static void country(void) {
    u16 code=rl(EAX)==0xff?rw(EBX):rl(EAX);
    if(rw(EDX)==0xffff) {DosRegs r={.ax=0x3801,.dx=0xffff,.bx=code}; result(dos_call(&r)); return;}
    DosCountryInfo c; memset(&c,0,sizeof(c)); c.size=sizeof(c);
    int e=dos_country_info(code?code:DOS_NLS_CURRENT,DOS_NLS_CURRENT,&c);
    if(e) {result(e); return;}
    u32 b=(u32)(u64)LIN(sreg(SR_DS),rw(EDX)); u8 *p=LINEAR(b);
    memset(p,0,34);
    poke16(b,c.date_order);
    memcpy(p+2,c.currency,5); memcpy(p+7,c.thousands,2); memcpy(p+9,c.decimal,2);
    memcpy(p+11,c.date_separator,2); memcpy(p+13,c.time_separator,2);
    p[15]=c.currency_flags; p[16]=c.currency_digits; p[17]=c.time_format;
    poke32(b+18,DOS_DATA_SEG<<16|CASEMAP_OFF);
    memcpy(p+22,c.list_separator,2);
    ww(EBX,c.country); ww(EAX,c.country); clear_cf();
}

/* Handle functions translate BX (and CX for AH=46h). */
static int native_bx(unsigned *h) {return handle_native(rw(EBX),h);}
/* Native handles of the EMM's device (EMMXXXX0, opened as NUL), which
 * IOCTL reports as the EMM driver's: a character device with IOCTL,
 * ready for output. */
static u8 emm_handles[32]; static unsigned emm_count;
static int emm_handle(unsigned h) {for(unsigned i=0;i<emm_count;i++) if(emm_handles[i]==h) return 1; return 0;}
static void emm_forget(unsigned h) {for(unsigned i=0;i<emm_count;i++) if(emm_handles[i]==h) {emm_handles[i]=emm_handles[--emm_count]; return;}}
static void emm_opened(unsigned h) {if(emm_count<sizeof(emm_handles)) emm_handles[emm_count++]=(u8)h;}
/* A native handle closed outside AH=3Eh (a program's end): not EMMXXXX0's any more. */
void emm_closed(unsigned h) {emm_forget(h);}

static void ioctl(void) {
    u8 sub=rl(EAX); unsigned h=0; int e=0;
    DosRegs r={.ax=0x4400|sub,.cx=rw(ECX),.dx=rw(EDX)};
    if((sub==0 || sub==7 || sub==2) && !native_bx(&h) && emm_handle(h)) {
        if(sub==0) ww(EDX,0x4080); else if(sub==7) wl(EAX,0xff);
        result(sub==2?DE_FUNCTION:0); return;
    }
    switch(sub) {
    case 0x00: case 0x01: case 0x06: case 0x07: case 0x0a:
        e=native_bx(&h); if(e) break;
        r.bx=h; e=dos_call(&r); if(e) break;
        if(sub==0 || sub==0x0a) ww(EDX,(u16)r.dx); else if(sub==6 || sub==7) wl(EAX,(u8)r.ax);
        break;
    case 0x02: case 0x03:
        e=native_bx(&h); if(e) break;
        r.bx=h; r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(!e) ww(EAX,(u16)r.ax);
        break;
    case 0x04: case 0x05:
        r.bx=rl(EBX); r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(!e) ww(EAX,(u16)r.ax);
        break;
    case 0x08: case 0x09:
        r.bx=rl(EBX); e=dos_call(&r);
        if(!e) {if(sub==8) ww(EAX,(u16)r.ax); else ww(EDX,(u16)r.dx);}
        break;
    case 0x0b: r.bx=rw(EBX); e=dos_call(&r); break;
    case 0x0e: case 0x0f: r.bx=rl(EBX); e=dos_call(&r); if(!e) wl(EAX,(u8)r.ax); break;
    default: e=DE_FUNCTION;
    }
    result(e);
}

/* A native program started from a 16-bit one gets the program's standard
 * handles (sync_std). */
static int exec_native(const char *path,u32 block) {
    u32 t=peek32(block+2); const u8 *src=LIN(t>>16,t);
    char tail[128]; unsigned len=src[0]>126?126:src[0];
    memcpy(tail,src+1,len); tail[len]=0;
    for(unsigned i=0;i<len;i++) if(tail[i]==13) {tail[i]=0; break;}
    int e=dos_exec(path,tail);
    vga_resume();
    if(!e) {
        DosRegs r={.ax=0x4d00}; dos_call(&r);
        return_code=(u16)r.ax;
    }
    return e;
}

static void exec(void) {
    char path[DOS_PATH_MAX]; copy_out(path,(const char *)ptr(SR_DS,EDX),sizeof(path));
    u32 block=(u32)(u64)LIN(sreg(SR_ES),rw(EBX));
    int e;
    switch(rl(EAX)) {
    case 0: e=is_dos16(path)?exec_program(path,block):exec_native(path,block); break;
    case 3: e=load_overlay(path,block); break;
    default: e=DE_FUNCTION;
    }
    result(e);
}

void int21(void) {
    u8 ah=rh(EAX),al=rl(EAX); unsigned h=0; int e=0;
    DosRegs r={.ax=rw(EAX),.bx=rw(EBX),.cx=rw(ECX),.dx=rw(EDX)};
    switch(ah) {
    case 0x00: request_exit(0,0,0); return;
    case 0x01: case 0x03: case 0x07: case 0x08: case 0x0b:
        e=dos_call(&r); if(!e) wl(EAX,(u8)r.ax); else if(e==DE_BREAK) break_pending=1;
        return;
    case 0x02: case 0x04: case 0x05:
        e=dos_call(&r); if(e==DE_BREAK) break_pending=1; else if(ah==2) wl(EAX,(u8)r.ax);
        return;
    case 0x06:
        e=dos_call(&r); if(e==DE_BREAK) {break_pending=1; return;}
        wl(EAX,(u8)r.ax);
        cpu.eflags=(cpu.eflags&~(u64)FL_ZF)|(r.flags&FL_ZF);
        return;
    case 0x09: r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(e==DE_BREAK) break_pending=1; wl(EAX,'$'); return;
    case 0x0a: r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(e==DE_BREAK) break_pending=1; return;
    case 0x0c:
        if(al==0x0a) r.dx=ptr(SR_DS,EDX);
        e=dos_call(&r); if(e==DE_BREAK) break_pending=1; else if(al!=0x0a) wl(EAX,(u8)r.ax);
        return;
    case 0x0d: dos_call(&r); return;
    case 0x0e: r.dx=rl(EDX); dos_call(&r); wl(EAX,(u8)r.ax); return;
    case 0x19: dos_call(&r); wl(EAX,(u8)r.ax); return;
    case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0x21: case 0x22: case 0x23: case 0x24: case 0x27: case 0x28:
        r.dx=ptr(SR_DS,EDX);
        app_dos->int21(&r);
        wl(EAX,(u8)r.ax); if(ah==0x27 || ah==0x28) ww(ECX,(u16)r.cx);
        return;
    case 0x29: {
        DosFcb temp; u8 *fcb=LIN(sreg(SR_ES),rw(EDI));
        memcpy(temp.bytes,fcb,12); memset(temp.bytes+12,0,sizeof(temp)-12);
        u64 text=ptr(SR_DS,ESI);
        r.si=text; r.di=(u64)&temp; app_dos->int21(&r);
        memcpy(fcb,temp.bytes,12);
        ww(ESI,(u16)(rw(ESI)+(r.si-text))); wl(EAX,(u8)r.ax);
        return;
    }
    case 0x1a: dta=(u32)(u64)LIN(sreg(SR_DS),rw(EDX)); sync_dta(); return;
    case 0x2f: {
        /* Report the DTA as the program set it when it is in a segment. */
        set_sreg(SR_ES,(u16)(dta>>4)); ww(EBX,(u16)(dta&15)); return;
    }
    case 0x1b: case 0x1c: {
        if(ah==0x1c) r.dx=rl(EDX);
        app_dos->int21(&r);
        wl(EAX,(u8)r.ax);
        if((u8)r.ax!=0xff) {
            *LIN(DOS_DATA_SEG,MEDIA_OFF)=*(const u8 *)r.bx;
            ww(ECX,(u16)r.cx); ww(EDX,r.dx>0xffff?0xffff:(u16)r.dx);
            set_sreg(SR_DS,DOS_DATA_SEG); ww(EBX,MEDIA_OFF);
        }
        return;
    }
    case 0x1f: case 0x32: wl(EAX,0xff); return;
    case 0x25: set_ivt(al,(u32)sreg(SR_DS)<<16|rw(EDX)); return;
    case 0x35: {u32 v=ivt(al); set_sreg(SR_ES,(u16)(v>>16)); ww(EBX,(u16)v); return;}
    case 0x26: {
        u16 seg=rw(EDX); memcpy(LIN(seg,0),LIN(cur_psp,0),256);
        poke16(((u32)seg<<4)+2,(u16)(peek16(((u32)cur_psp<<4)+2)));
        return;
    }
    case 0x2a: case 0x2c: dos_call(&r); ww(EAX,(u16)r.ax); ww(ECX,(u16)r.cx); ww(EDX,(u16)r.dx); return;
    case 0x2b: case 0x2d: dos_call(&r); wl(EAX,(u8)r.ax); return;
    case 0x2e: dos_call(&r); return;
    case 0x30:
        ww(EAX,4); ww(EBX,0xff00); ww(ECX,0); return;
    case 0x33:
        if(al==5) {wl(EDX,3); return;}
        if(al==6) {ww(EBX,4); ww(EDX,0); return;}
        e=dos_call(&r); if(e) {wl(EAX,0xff); return;}
        if(al==0 || al==2) wl(EDX,(u8)r.dx);
        return;
    case 0x34: set_sreg(SR_ES,DOS_DATA_SEG); ww(EBX,INDOS_OFF); return;
    case 0x36: {
        r.dx=rl(EDX); app_dos->int21(&r);
        if((r.flags&1) || (u16)r.ax==0xffff) {ww(EAX,0xffff); return;}
        ww(EAX,(u16)r.ax); ww(EBX,r.bx>0xffff?0xffff:(u16)r.bx);
        ww(ECX,(u16)r.cx); ww(EDX,r.dx>0xffff?0xffff:(u16)r.dx);
        return;
    }
    case 0x37: e=dos_call(&r); if(!e) {wl(EAX,(u8)r.ax); wl(EDX,(u8)r.dx);} else wl(EAX,0xff); return;
    case 0x38: country(); return;
    case 0x39: case 0x3a: case 0x3b: case 0x41:
        r.dx=ptr(SR_DS,EDX); result(dos_call(&r)); return;
    case 0x3c: case 0x3d: case 0x5a: case 0x5b: {
        int emm=ah==0x3d && ems_device((const char *)ptr(SR_DS,EDX));
        r.dx=emm?(u64)"NUL":ptr(SR_DS,EDX); e=dos_call(&r);
        if(!e && emm) emm_opened((unsigned)r.ax);
        if(!e) {u16 n; e=handle_new((unsigned)r.ax,&n); if(!e) ww(EAX,n);}
        result(e); return;
    }
    case 0x6c: {
        int emm=ems_device((const char *)ptr(SR_DS,ESI));
        r.si=emm?(u64)"NUL":ptr(SR_DS,ESI); e=dos_call(&r);
        if(!e && emm) emm_opened((unsigned)r.ax);
        if(!e) {u16 n; e=handle_new((unsigned)r.ax,&n); if(!e) {ww(EAX,n); ww(ECX,(u16)r.cx);}}
        result(e); return;
    }
    case 0x3e:
        e=native_bx(&h); if(!e) {r.bx=h; e=dos_call(&r); jft(cur_psp)[rw(EBX)]=0xff; emm_forget(h);}
        result(e); return;
    case 0x3f: case 0x40:
        e=native_bx(&h);
        if(!e) {r.bx=h; r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(!e || ah==0x40) ww(EAX,(u16)r.ax);}
        result(e); return;
    case 0x42: {
        e=native_bx(&h);
        if(!e) {
            r.bx=h; r.dx=(u64)(i64)(int32_t)((u32)rw(ECX)<<16|rw(EDX)); e=dos_call(&r);
            if(!e) {ww(EAX,(u16)r.ax); ww(EDX,(u16)(r.ax>>16));}
        }
        result(e); return;
    }
    case 0x43: r.dx=ptr(SR_DS,EDX); e=dos_call(&r); if(!e) ww(ECX,(u16)r.cx); result(e); return;
    case 0x44: ioctl(); return;
    case 0x45: {
        e=native_bx(&h); unsigned dup;
        if(!e) e=dos_dup(h,&dup);
        if(!e && emm_handle(h)) emm_opened(dup);
        if(!e) {u16 n; e=handle_new(dup,&n); if(!e) ww(EAX,n);}
        result(e); return;
    }
    case 0x46: {
        u16 to=rw(ECX); unsigned dup;
        e=native_bx(&h);
        if(!e && to>=jft_size(cur_psp)) e=DE_HANDLE;
        if(!e && to!=rw(EBX)) {
            e=dos_dup(h,&dup);
            if(!e) {
                u8 *t=jft(cur_psp); if(t[to]!=0xff) {dos_close(t[to]); emm_forget(t[to]);}
                t[to]=(u8)dup; if(emm_handle(h)) emm_opened(dup);
            }
        }
        if(!e && to<5) sync_std();
        result(e); return;
    }
    case 0x47: {
        char cwd[DOS_PATH_MAX]; r.si=(u64)cwd; e=dos_call(&r);
        if(!e) copy_out((char *)LIN(sreg(SR_DS),rw(ESI)),cwd,64);
        result(e); return;
    }
    case 0x48: {
        u16 seg,largest; e=mem_alloc(rw(EBX),cur_psp,&seg,&largest);
        if(e) {fail(e); ww(EBX,largest);} else {ww(EAX,seg); clear_cf();}
        return;
    }
    case 0x49: result(mem_free(sreg(SR_ES))); return;
    case 0x4a: {
        u16 largest=0; e=mem_resize(sreg(SR_ES),rw(EBX),&largest);
        if(e) {fail(e); if(e==DE_NOMEM) ww(EBX,largest);} else clear_cf();
        return;
    }
    case 0x4b: exec(); return;
    case 0x31: request_exit(al,0,rw(EDX)); return;
    case 0x4c: request_exit(al,0,0); return;
    case 0x4d: ww(EAX,return_code); return_code=0; return;
    case 0x4e: search(1); return;
    case 0x4f: search(0); return;
    case 0x50: cur_psp=rw(EBX); return;
    case 0x51: case 0x62: ww(EBX,cur_psp); return;
    case 0x52: set_sreg(SR_ES,DOS_DATA_SEG); ww(EBX,SYSVARS_OFF); return;
    case 0x54: dos_call(&r); wl(EAX,(u8)r.ax); return;
    case 0x56: r.dx=ptr(SR_DS,EDX); r.di=ptr(SR_ES,EDI); result(dos_call(&r)); return;
    case 0x57:
        e=native_bx(&h);
        if(!e) {r.bx=h; e=dos_call(&r); if(!e && al==0) {ww(ECX,(u16)r.cx); ww(EDX,(u16)r.dx);}}
        result(e); return;
    case 0x58:
        if(al==0) {ww(EAX,(u16)alloc_strategy); clear_cf();}
        else if(al==1 && rw(EBX)<=2) {alloc_strategy=rw(EBX); clear_cf();}
        else fail(DE_FUNCTION);
        return;
    case 0x59:
        r.bx=0; dos_call(&r);
        ww(EAX,(u16)r.ax); ww(EBX,(u16)r.bx); ww(ECX,(u16)r.cx);
        return;
    case 0x5c: {
        e=native_bx(&h);
        if(!e) {
            r.bx=h; r.dx=(u32)rw(ECX)<<16|rw(EDX); r.cx=(u32)rw(ESI)<<16|rw(EDI);
            e=dos_call(&r);
        }
        result(e); return;
    }
    case 0x60: {
        char out[DOS_PATH_MAX]; r.si=ptr(SR_DS,ESI); r.di=(u64)out; e=dos_call(&r);
        if(!e) copy_out((char *)LIN(sreg(SR_ES),rw(EDI)),out,128);
        result(e); return;
    }
    case 0x63:
        if(al==0) {set_sreg(SR_DS,DOS_DATA_SEG); ww(ESI,DBCS_OFF); wl(EAX,0); clear_cf();}
        else fail(DE_FUNCTION);
        return;
    case 0x66: e=dos_call(&r); if(!e) {ww(EBX,(u16)r.bx); ww(EDX,(u16)r.dx);} result(e); return;
    case 0x67: {
        u16 want=rw(EBX);
        if(want<=jft_size(cur_psp)) {clear_cf(); return;}
        if(want>255) {fail(DE_HANDLES); return;}
        u16 seg,largest; e=mem_alloc((u16)((want+15)/16),cur_psp,&seg,&largest);
        if(e) {fail(e); return;}
        u8 *t=LIN(seg,0); memset(t,0xff,want); memcpy(t,jft(cur_psp),jft_size(cur_psp));
        u32 p=(u32)cur_psp<<4; poke16(p+0x32,want); poke32(p+0x34,(u32)seg<<16);
        clear_cf(); return;
    }
    case 0x68: e=native_bx(&h); if(!e) {r.bx=h; e=dos_call(&r);} result(e); return;
    default: wl(EAX,0); fail(DE_FUNCTION); return;
    }
}
