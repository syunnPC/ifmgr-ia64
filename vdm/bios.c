/* SPDX-License-Identifier: GPL-2.0-or-later
 * BIOS services for 16-bit programs. The screen is the display adapter as
 * a VGA where IO.SYS can provide one (vga.c). Otherwise it is the native
 * DOS console, a teletype: INT 10h writes characters through CON (whose
 * code page maps the bytes) and keeps the cursor and mode in the BIOS data
 * area, and direct video memory is not shown. The keyboard is the native console's
 * key queue with PC scan codes; the timer tick count at 40:6Ch follows the
 * native millisecond clock. Disk, serial, extended-memory and mouse
 * services report that nothing is there.
 */
#include "vdm.h"
#define BDA 0x400U
static unsigned con=0xffff;
static u64 midnight_ms,clock_base;
static u8 ascii_scan[128];
/* The BIOS's keyboard buffer: what KEYB (DosApi keyb_key) or the BIOS's
 * own translation typed, as scan code << 8 | character. */
#define KEY_BUFFER 16U
static u16 key_buffer[KEY_BUFFER];
static unsigned key_head,key_count;
static int key_put(u16 w) {if(key_count==KEY_BUFFER) return 0; key_buffer[(key_head+key_count++)%KEY_BUFFER]=w; return 1;}

static void open_con(void) {if(con==0xffff && dos_open("CON",DOS_OPEN_RDWR,0,&con)) con=0xfffe;}
void console_out(const u8 *text,u32 n) {
    u32 done;
    open_con();
    if(con<0xfffe) dos_write(con,text,n,&done);
    else {DosRegs r={.ax=0x0200}; for(u32 i=0;i<n;i++) {r.dx=text[i]; app_dos->int21(&r);}}
}

/* ANSI.SYS's options while it is there (DOS_INSTALLED_ANSI), else 0. */
static u32 ansi(void) {u32 v=0; return dos_installed(DOS_INSTALLED_ANSI,NULL,&v)?0:v;}
void video_close(void) {
    if(con<0xfffe) dos_close(con);
    con=0xffff;
    vga_close();
}

static void cursor_advance(u8 c) {
    u8 col=*LINEAR(BDA+0x50),row=*LINEAR(BDA+0x51);
    if(c==13) col=0;
    else if(c==10) {if(row<24) row++;}
    else if(c==8) {if(col) col--;}
    else if(c==7) {}
    else if(++col>=80) {col=0; if(row<24) row++;}
    *LINEAR(BDA+0x50)=col; *LINEAR(BDA+0x51)=row;
}
static void teletype(u8 c) {console_out(&c,1); cursor_advance(c);}

void bios_init(void) {
    static const char *const keys[4]={"1234567890-=","qwertyuiop[]","asdfghjkl;'`","\\zxcvbnm,./"};
    static const char *const shifted[4]={"!@#$%^&*()_+","QWERTYUIOP{}","ASDFGHJKL:\"~","|ZXCVBNM<>?"};
    static const u8 first[4]={0x02,0x10,0x1e,0x2b};
    for(unsigned r=0;r<4;r++) for(unsigned i=0;keys[r][i];i++) {
        ascii_scan[(u8)keys[r][i]]=(u8)(first[r]+i); ascii_scan[(u8)shifted[r][i]]=(u8)(first[r]+i);
    }
    for(unsigned c=1;c<=26;c++) ascii_scan[c]=ascii_scan['a'+c-1];
    ascii_scan[8]=0x0e; ascii_scan[9]=0x0f; ascii_scan[13]=0x1c; ascii_scan[27]=0x01; ascii_scan[' ']=0x39; ascii_scan[127]=0x0e;
    poke16(BDA+0x10,0x0021); /* 80x25 colour, one diskette drive */
    poke16(BDA+0x13,(u16)(MEM_TOP>>6));
    poke16(BDA+0x1a,0x1e); poke16(BDA+0x1c,0x1e); poke16(BDA+0x80,0x1e); poke16(BDA+0x82,0x3e);
    *LINEAR(BDA+0x49)=3; poke16(BDA+0x4a,80); poke16(BDA+0x4c,4000); poke16(BDA+0x63,0x3d4);
    *LINEAR(BDA+0x84)=24; poke16(BDA+0x85,16); *LINEAR(BDA+0x87)=0x60; *LINEAR(BDA+0x88)=0xf9;
    poke16(BDA+0x60,0x0607);
    DosDateTime now;
    if(!dos_get_datetime(&now)) midnight_ms=((u64)now.hour*3600+now.minute*60+now.second)*1000+now.hundredth*10;
    clock_base=io->ticks_ms(io->context);
    key_head=key_count=0;
    vga_init();
}
/* 1193182/65536 ticks per second since midnight. */
void bios_ticks(void) {
    u64 ms=midnight_ms+io->ticks_ms(io->context)-clock_base,day=86400000ULL;
    if(ms>=day) {*LINEAR(BDA+0x70)=1; midnight_ms-=day; ms-=day;}
    poke32(BDA+0x6c,(u32)(ms*1193182ULL/65536ULL/1000ULL));
}

static u16 key_code(const IoEvent *k) {
    static const u8 ext[]={0,0x48,0x50,0x4d,0x4b,0x47,0x4f,0x52,0x53,0x49,0x51,
        0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x85,0x86,0x01,0};
    unsigned m=k->modifiers;
    if(k->unicode && k->unicode<128) {
        u8 c=(u8)k->unicode,scan=ascii_scan[c];
        if((m&IO_MOD_ALT) && scan) return (u16)(scan<<8);
        return (u16)(scan<<8|c);
    }
    if(k->unicode) return '?';
    if(k->scan>=sizeof ext || !ext[k->scan]) return 0;
    u8 scan=ext[k->scan];
    if(k->scan==IO_SCAN_ESCAPE) return 0x011b;
    if(k->scan>=IO_SCAN_F1 && k->scan<=IO_SCAN_F10) {
        unsigned f=k->scan-IO_SCAN_F1;
        if(m&IO_MOD_ALT) scan=(u8)(0x68+f); else if(m&IO_MOD_CONTROL) scan=(u8)(0x5e + f); else if(m&IO_MOD_SHIFT) scan=(u8)(0x54+f);
    } else if(m&IO_MOD_CONTROL) {
        switch(k->scan) {
        case IO_SCAN_LEFT: scan=0x73; break; case IO_SCAN_RIGHT: scan=0x74; break;
        case IO_SCAN_HOME: scan=0x77; break; case IO_SCAN_END: scan=0x75; break;
        case IO_SCAN_PAGE_UP: scan=0x84; break; case IO_SCAN_PAGE_DOWN: scan=0x76; break;
        default: break;
        }
    }
    return (u16)(scan<<8);
}
/* Alt+Tab, Alt+Esc and Ctrl+Esc, which switch away when Interface Manager
 * (MSDOS.SYS's switch hook) takes them. */
static int switched(const IoEvent *k) {
    u32 to=0;
    if(app_dos->size<offsetof(DosApi,switch_away)+sizeof(app_dos->switch_away) || !(k->flags&IO_KEY_MODIFIERS_VALID)) return 0;
    if((k->modifiers&IO_MOD_ALT) && k->unicode=='\t') to=DOS_SWITCH_ALT_TAB;
    else if((k->modifiers&IO_MOD_ALT) && k->scan==IO_SCAN_ESCAPE) to=DOS_SWITCH_ALT_ESC;
    else if((k->modifiers&IO_MOD_CONTROL) && k->scan==IO_SCAN_ESCAPE) to=DOS_SWITCH_CTRL_ESC;
    if(!to || !app_dos->switch_away(to|DOS_SWITCH_QUERY)) return 0;
    app_dos->switch_away(to);
    return 1;
}
/* PRINT's time while the program waits (DosApi idle): the ms it wants. */
static u32 print_idle(void) {
    return app_dos->size>=offsetof(DosApi,idle)+sizeof(app_dos->idle) && app_dos->idle?app_dos->idle():0;
}
static int have_keyb(void) {return app_dos->size>=offsetof(DosApi,keyb_key)+sizeof(app_dos->keyb_key) && app_dos->keyb && app_dos->keyb_key;}
/* Waits for a key (or only looks); 0 when none. */
static u16 read_key(int wait,int keep) {
    while(!key_count) {
        IoEvent k; int e=io->console_key(io->context,&k,0);
        if(e==DE_NOTREADY && wait) {
            u32 ms=print_idle();
            if(ms) {io->wait(io->context,ms); continue;}
            e=io->console_key(io->context,&k,IO_KEY_WAIT);
        }
        if(e) return 0;
        if(k.type!=IO_EVENT_KEY || (k.flags&IO_KEY_RELEASE)) continue;
        if(switched(&k)) continue;
        /* KEYB's characters, then the BIOS's when KEYB leaves the key to it. */
        u16 words[DOS_KEYB_KEYS]; u32 r=have_keyb()?app_dos->keyb_key(&k,words):DOS_KEYB_BIOS;
        for(u32 i=0;i<(r&~DOS_KEYB_BIOS);i++) key_put(words[i]);
        if(r&DOS_KEYB_BIOS) {u16 code=key_code(&k); if(code) key_put(code);}
    }
    u16 code=key_buffer[key_head];
    if(!keep) {key_head=(key_head+1)%KEY_BUFFER; key_count--;}
    return code;
}
/* KEYB's INT 2Fh (AD80h-AD82h) once MSDOS.SYS has its tables: installed,
 * with its shared data area at ES:DI as KEYB keeps its fixed part (no
 * tables there: their pointers are -1); 81h the code page made active
 * (BX), 82h US (BL=0) or national (BL=FFh) mode, CF and AX=1 on error. */
static void keyb_mux(u8 function) {
    DosKeybRequest r={.size=sizeof(r)};
    if(!have_keyb() || app_dos->keyb(DOS_KEYB_QUERY,&r)) return;
    if(function==0x80) {
        u8 *sd=LIN(TRAP_SEG,KEYB_DATA_OFF); memset(sd,0,KEYB_DATA_SIZE);
        poke16((TRAP_SEG<<4)+KEYB_DATA_OFF+12,0x1000); /* KEYB_TYPE: an enhanced keyboard */
        poke16((TRAP_SEG<<4)+KEYB_DATA_OFF+14,0xc000); /* SYSTEM_FLAG: an AT, extended INT 16h */
        sd[16]=1; /* TABLE_OK */
        sd[22]=(u8)r.language[0]; sd[23]=(u8)r.language[1];
        poke16((TRAP_SEG<<4)+KEYB_DATA_OFF+24,r.code_page);
        for(unsigned at=26;at<36;at+=2) poke16((TRAP_SEG<<4)+KEYB_DATA_OFF+at,0xffff);
        sd[39]=59; sd[40]=60; /* Ctrl+Alt+F1 and F2 */
        ww(EAX,0xffff); ww(EBX,0x0100); set_sreg(SR_ES,TRAP_SEG); ww(EDI,KEYB_DATA_OFF);
    } else if(function==0x81 || function==0x82) {
        int e;
        if(function==0x81) {r.code_page=rw(EBX); e=app_dos->keyb(DOS_KEYB_CODE_PAGE,&r);}
        else if(rl(EBX)==0 || rl(EBX)==0xff) {r.flags=rl(EBX)?DOS_KEYB_FOREIGN:0; e=app_dos->keyb(DOS_KEYB_MODE,&r);}
        else e=DE_FUNCTION;
        if(e) {ww(EAX,1); set_cf();} else clear_cf();
    }
}

/* PRINT's INT 2Fh (AH=01h) for MSDOS.SYS's resident part: 00h installed,
 * 01h queue the name a packet at DS:DX points to (a level byte, a far
 * pointer), 02h cancel the name at DS:DX (? and * allowed), 03h cancel all,
 * 04h the queue at DS:SI (64-byte entries, the last empty; printing holds
 * until 05h) with DX the list device's error count, 05h release it, 06h the
 * list device's header at DS:SI, CF set while files are queued. */
static u16 print_held; /* the PSP of the program that held the queue (0104h) */
/* A program ends: a hold it left is released. */
void print_release(u16 psp) {
    DosPrintRequest r={.size=sizeof(r)};
    if(print_held && print_held==psp) {app_dos->print(DOS_PRINT_RELEASE,&r); print_held=0;}
}
static void print_mux(u8 function) {
    DosPrintRequest r={.size=sizeof(r)};
    int have=app_dos->size>=offsetof(DosApi,print)+sizeof(app_dos->print) && app_dos->print;
    int installed=have && !app_dos->print(DOS_PRINT_QUERY,&r);
    int e=0;
    if(function==0) {wl(EAX,installed?0xff:0); return;}
    if(!installed) e=DE_FUNCTION;
    else if(function==1 || function==2) {
        u32 at=function==1?peek32(((u32)sreg(SR_DS)<<4)+(u16)(rw(EDX)+1)):(u32)sreg(SR_DS)<<16|rw(EDX);
        char name[DOS_PATH_MAX],full[DOS_PATH_MAX]; unsigned n=0;
        for(const u8 *p=LIN(at>>16,(u16)at);n+1<sizeof(name) && p[n];n++) name[n]=(char)p[n];
        name[n]=0;
        e=dos_full_path(name,full);
        if(!e && strlen(full)>=DOS_PRINT_ENTRY) e=DE_PATH;
        /* As DOS 4's ADDFIL: level 0 only, and a file that opens. */
        if(!e && function==1 && *LIN(sreg(SR_DS),rw(EDX))) e=DE_FUNCTION;
        if(!e && function==1) {unsigned h; e=dos_open(full,DOS_OPEN_READ,0,&h); if(!e) dos_close(h);}
        if(!e) {strcopy(r.path,sizeof(r.path),full); e=app_dos->print(function==1?DOS_PRINT_SUBMIT:DOS_PRINT_CANCEL,&r);}
    }
    else if(function==3) e=app_dos->print(DOS_PRINT_CANCEL_ALL,&r);
    else if(function==4 || function==6) {
        int held=print_held;
        e=app_dos->print(DOS_PRINT_STATUS,&r);
        if(!e) {
            u8 *q=LIN(PRINT_SEG,0); unsigned i=0;
            for(;i<32 && r.queue[i*DOS_PRINT_ENTRY];i++) memcpy(q+i*DOS_PRINT_ENTRY,r.queue+i*DOS_PRINT_ENTRY,DOS_PRINT_ENTRY);
            memset(q+i*DOS_PRINT_ENTRY,0,DOS_PRINT_ENTRY);
            set_sreg(SR_DS,PRINT_SEG);
            if(function==4) {ww(ESI,0); ww(EDX,(u16)(r.errors>0xffff?0xffff:r.errors)); if(!print_held) print_held=cur_psp;}
            else {
                /* A character device's header for the list device. */
                u8 *h=q+PRINT_DEVICE_OFF; memset(h,0,18); poke32(((u32)PRINT_SEG<<4)+PRINT_DEVICE_OFF,0xffffffffU);
                h[4]=0x00; h[5]=0x80; memset(h+10,' ',8);
                for(unsigned k=0;k<8 && r.device[k];k++) h[10+k]=(u8)r.device[k];
                ww(ESI,PRINT_DEVICE_OFF);
                if(!held) app_dos->print(DOS_PRINT_RELEASE,&r); /* 06h holds nothing itself */
                if(i) {ww(EAX,DE_NOMEM); set_cf();} else {ww(EAX,0); clear_cf();}
                return;
            }
        }
    }
    else if(function==5) {e=app_dos->print(DOS_PRINT_RELEASE,&r); print_held=0;}
    else e=DE_FUNCTION;
    if(e) fail(e); else clear_cf();
}
static void int10(void) {
    u8 ah=rh(EAX),al=rl(EAX);
    if(vga_int10()) return;
    switch(ah) {
    case 0x00:
        io->console_clear(io->context);
        *LINEAR(BDA+0x49)=al&0x7f; poke16(BDA+0x50,0);
        break;
    case 0x01: poke16(BDA+0x60,rw(ECX)); break;
    case 0x02: if(rh(EBX)==0) poke16(BDA+0x50,rw(EDX)); break;
    case 0x03: ww(EDX,peek16(BDA+0x50)); ww(ECX,peek16(BDA+0x60)); break;
    case 0x05: *LINEAR(BDA+0x62)=al; break;
    case 0x06: case 0x07:
        if(al==0 && rw(ECX)==0 && rh(EDX)>=24 && rl(EDX)>=79) {io->console_clear(io->context); poke16(BDA+0x50,0);}
        break;
    case 0x08: ww(EAX,0x0720); break;
    case 0x09: case 0x0a: {
        u16 n=rw(ECX); u8 c=al;
        for(u16 i=0;i<n && i<2000;i++) console_out(&c,1);
        break;
    }
    case 0x0e: teletype(al); break;
    case 0x0f: wl(EAX,*LINEAR(BDA+0x49)); wh(EAX,80); wh(EBX,*LINEAR(BDA+0x62)); break;
    case 0x10: case 0x11: break;
    case 0x12:
        if(rl(EBX)==0x10) {ww(EBX,0x0003); ww(ECX,0x0009);}
        break;
    case 0x13: {
        u8 *s=LIN(sreg(SR_ES),rw(EBP)); u16 n=rw(ECX); int attrs=al&2;
        for(u16 i=0;i<n;i++) teletype(s[attrs?i*2:i]);
        break;
    }
    case 0x1a: if(al==0) {wl(EAX,0x1a); ww(EBX,0x0008);} break;
    case 0x4f: ww(EAX,0x014f); break; /* VBE: not available on this console */
    default: break;
    }
}

static void int1a(void) {
    switch(rh(EAX)) {
    case 0x00: {
        bios_ticks(); u32 t=peek32(BDA+0x6c);
        ww(ECX,(u16)(t>>16)); ww(EDX,(u16)t); wl(EAX,*LINEAR(BDA+0x70)); *LINEAR(BDA+0x70)=0;
        break;
    }
    case 0x02: case 0x04: {
        DosDateTime t; if(dos_get_datetime(&t)) {set_cf(); break;}
        if(rh(EAX)==2) {wh(ECX,bcd(t.hour)); wl(ECX,bcd(t.minute)); wh(EDX,bcd(t.second)); wl(EDX,0);}
        else {wh(ECX,bcd(t.year/100)); wl(ECX,bcd(t.year%100)); wh(EDX,bcd(t.month)); wl(EDX,bcd(t.day));}
        clear_cf(); break;
    }
    case 0x01: case 0x03: case 0x05: clear_cf(); break;
    default: set_cf();
    }
}
static void int16(void) {
    switch(rh(EAX)) {
    case 0x00: case 0x10: {
        u16 k=read_key(1,0); ww(EAX,k);
        break;
    }
    case 0x01: case 0x11: {
        u16 k=read_key(0,1);
        if(k) {ww(EAX,k); cpu.eflags&=~(u64)FL_ZF;} else cpu.eflags|=FL_ZF;
        break;
    }
    case 0x02: case 0x12: wl(EAX,*LINEAR(BDA+0x17)); if(rh(EAX)==0x12) wh(EAX,0); break;
    case 0x05: wl(EAX,key_put(rw(ECX))?0:1); break;
    default: break;
    }
}

int bios_int(unsigned n) {
    switch(n) {
    case 0x08: case 0x28: print_idle(); return 1; /* the timer tick and DOS's idle: PRINT's time */
    case 0x05: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x1b: case 0x1c: case 0x2a: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74:
    case 0x75: case 0x76: case 0x77:
        return 1;
    case 0x10: int10(); return 1;
    case 0x11: ww(EAX,peek16(BDA+0x10)); return 1;
    case 0x12: ww(EAX,peek16(BDA+0x13)); return 1;
    case 0x13: if(rh(EAX)==0) {wh(EAX,0); clear_cf();} else {wh(EAX,1); set_cf();} return 1;
    case 0x14: wh(EAX,0x80); return 1;
    case 0x15:
        switch(rh(EAX)) {
        case 0x86: {u32 us=(u32)rw(ECX)<<16|rw(EDX); while(us) {u32 s=us>100000?100000:us; io->stall_us(io->context,s); us-=s;} clear_cf(); break;}
        case 0x88: ww(EAX,0); clear_cf(); break;
        default: wh(EAX,0x86); set_cf();
        }
        return 1;
    case 0x16: int16(); return 1;
    case 0x17:
        if(rh(EAX)==0) {DosRegs r={.ax=0x0500,.dx=rl(EAX)}; app_dos->int21(&r);}
        wh(EAX,0x90); return 1;
    case 0x19: request_exit(0,0,0); return 1;
    case 0x1a: int1a(); return 1;
    /* ANSI.SYS takes INT 29h, DOS's fast console output, as its CON does. */
    case 0x29: {u8 c=rl(EAX); if(ansi()) console_out(&c,1); else if(!vga_tty(c,7)) teletype(c); return 1;}
    case 0x2f: {
        u16 ax=rw(EAX); DosAppend a={.size=sizeof(a)}; u8 table[DOS_DRIVES];
        if(ax==0x1600) wl(EAX,0);
        /* HIMEM.SYS: installed, and its entry. */
        else if(ax==0x4300) wl(EAX,xms_present()?0x80:0);
        else if(ax==0x4310 && xms_present()) {set_sreg(SR_ES,TRAP_SEG); ww(EBX,XMS_ENTRY);}
        /* DPMI 0.9: 32-bit programs too, a 486, 4 KiB of host data, the entry. */
        else if(ax==0x1687 && dpmi_available()) {
            ww(EAX,0); ww(EBX,1); wl(ECX,4); ww(EDX,0x005a); ww(ESI,0x0100);
            set_sreg(SR_ES,TRAP_SEG); ww(EDI,DPMI_SWITCH);
        }
        else if(ax==0x1680) {if(!print_idle()) io->stall_us(io->context,1000); wl(EAX,0);}
        else if((ax&0xff00)==0x0100) print_mux(rl(EAX));
        else if(ax>=0xad80 && ax<=0xad82) keyb_mux(rl(EAX));
        /* Installation checks: sharing and NLSFUNC's code page switching are
         * MSDOS.SYS's own, ASSIGN and APPEND are there once used. */
        else if(ax==0x1000 || ax==0x1400) wl(EAX,0xff);
        else if(ax==0xb000) {u32 page=0; dos_installed(DOS_INSTALLED_GRAFTABL,NULL,&page); wl(EAX,page?0xff:0);}
        /* ANSI.SYS: installed, its display IOCTL (CL=7Fh, 5Fh) and the /L state. */
        else if(ax==0x1a00) {if(ansi()) wl(EAX,0xff);}
        else if((ax==0x1a01 || ax==0x1a02) && ansi()) {
            u8 *block=LIN(sreg(SR_DS),rw(EDX));
            if(ax==0x1a02) {if(block[0]==1) block[2]=ansi()&DOS_ANSI_L?1:0; ww(EAX,0x021a); clear_cf();}
            else {
                u8 packet[18]; memcpy(packet,block,sizeof(packet)); open_con();
                DosRegs r={.ax=0x440c,.bx=con,.cx=0x0300|rl(ECX),.dx=(u64)packet,.si=sizeof(packet)};
                int e=con<0xfffe?dos_call(&r):DE_FUNCTION;
                if(!e) {memcpy(block,packet,sizeof(packet)); ww(EAX,0x011a); clear_cf();}
                else fail(e);
            }
        }
        else if(ax==0x0600) {
            int any=0;
            if(!dos_assign(NULL,table)) for(unsigned i=0;i<DOS_DRIVES;i++) if(table[i]!=i) any=1;
            wl(EAX,any?0xff:0);
        } else if((ax&0xff00)==0xb700) {
            int installed=!dos_append(NULL,&a) && (a.flags&DOS_APPEND_INSTALLED);
            if(ax==0xb700) wl(EAX,installed?0xff:0);
            else if(ax==0xb702 && installed) ww(EAX,0xffff);
            else if(ax==0xb706 && installed) ww(EBX,(u16)a.flags);
            else if(ax==0xb707 && installed) {a.flags=(a.flags&~0xffffu)|rw(EBX); dos_append(&a,NULL);}
        }
        return 1;
    }
    case 0x33: if(rw(EAX)==0) ww(EAX,0); return 1;
    case 0x67: wh(EAX,0x80); return 1;
    default: return 0;
    }
}
