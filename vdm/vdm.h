/* SPDX-License-Identifier: GPL-2.0-or-later
 * VDM: runs 16-bit DOS programs (.COM and MZ .EXE) in IA-32 virtual-8086
 * mode on the processor's IA-32 instruction set (IoServices ia32_run). Conventional
 * memory is the physical memory below 640 KiB; DOS and BIOS services are
 * provided here on top of the native DOS API.
 *
 * Low memory: 0-3FFh interrupt vectors, 400h BIOS data, 500h DOS data and
 * the list of lists, 600h a two-byte INT n stub per vector (the default
 * handlers the vectors point to), 800h the entries the program calls
 * (TRAP_SEG), 900h the video BIOS's fonts and tables, 3320h PRINT's queue,
 * then the arena of memory control blocks from 3B80h. The firmware's own
 * vector table at 10000h-17FFFh is a block owned by DOS that programs never
 * get.
 */
#ifndef VDM_H
#define VDM_H
#include "dos_client.h"
extern DosApi *app_dos;
/* Conventional memory starts at address 0; the compiler must not treat
 * these low addresses as null-pointer arithmetic. */
static inline u8 *linear(u32 a) {u64 p=a; __asm__("" : "+r"(p)); return (u8 *)p;}
#define LIN(seg,off) linear(((u32)(u16)(seg)<<4)+(u16)(off))
#define LINEAR(a) linear((u32)(a))
enum {EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI};
enum {SR_ES,SR_CS,SR_SS,SR_DS,SR_FS,SR_GS};
#define FL_CF 0x1U
#define FL_ZF 0x40U
#define FL_TF 0x100U
#define FL_IF 0x200U
#define FL_ARITH 0x8d5U
#define EFLAGS_V86 0x23002U /* VM, IOPL 3 and the reserved bit */
#define DOS_DATA_SEG 0x0050U
#define INDOS_OFF 0x00U /* InDOS flag, always zero */
#define MEDIA_OFF 0x01U /* media byte for AH=1Bh/1Ch */
#define DBCS_OFF 0x02U  /* empty DBCS lead-byte table */
#define CASEMAP_OFF 0x04U /* RETF: country case-map routine */
#define UNUSED_IRET_OFF 0x05U /* IRET: where the vectors nothing sets lead */
#define SYSVARS_OFF 0x12U /* list of lists; the first MCB is at -2 */
#define STUB_SEG 0x0060U
#define STUB_LINEAR 0x600U
#define VIDEO_SEG 0x0090U /* 8x8, 8x14 and 8x16 fonts, functionality table */
#define FONT8_OFF 0x0000U
#define FONT14_OFF 0x0800U
#define FONT16_OFF 0x1600U
#define FUNCTIONALITY_OFF 0x2600U
#define NO_ALTERNATES_OFF 0x2610U /* an empty list of 9-dot alternates */
#define GRAFTABL_OFF 0x2620U /* GRAFTABL's characters 80h-FFh, 8x8, for INT 1Fh */
#define PRINT_SEG 0x0332U /* PRINT's queue as INT 2Fh 0104h shows it (33 entries), its device header */
#define PRINT_DEVICE_OFF 0x0840U
#define FIRST_MCB 0x03b8U /* after PRINT_SEG's 86h paragraphs */
_Static_assert(PRINT_SEG*16U+PRINT_DEVICE_OFF+18U<=FIRST_MCB*16U,"PRINT's area ends below the first MCB");
/* Entries the program calls into, each an INT 67h serviced when it comes
 * from its own place here (whatever the vector holds): the EMM device
 * header (with "EMMXXXX0" at 0Ah), INT 67h's entry (INT 67h, IRET), the XMS
 * entry (a short jump over three NOPs, then INT 67h, RETF) and the return
 * from EMS 56h's call (INT 67h, IRET). */
#define TRAP_SEG 0x0080U
#define EMS_ENTRY 0x12U
#define XMS_ENTRY 0x18U
#define EMS_RETURN 0x20U
/* DPMI (dpmi.c): the mode switch, the return from real-mode work, the raw
 * switch to protected mode (each an INT 67h), the state save (a RETF) and
 * 32 real-mode callbacks, an INT 67h each. */
#define DPMI_SWITCH 0x40U
#define DPMI_RETURN 0x44U
#define DPMI_RAW 0x48U
#define DPMI_SAVE 0x4cU
#define DPMI_CALLBACKS 0x50U
#define KEYB_DATA_OFF 0xa0U /* KEYB's shared data area as INT 2Fh AD80h gives it */
#define KEYB_DATA_SIZE 45U
_Static_assert(DPMI_CALLBACKS+64U<=KEYB_DATA_OFF && KEYB_DATA_OFF+KEYB_DATA_SIZE<=0x100U,"TRAP_SEG's parts");
#define EXTENDED_LIMIT_KB DOS_EXTENDED_KB /* XMS and DPMI memory together */
#define HOLE_START 0x1000U /* firmware vector table, 10000h-17FFFh */
#define HOLE_END 0x1800U
extern u16 mem_top; /* 9000h when EMS takes the top 64 KiB for its page frame */
#define MEM_TOP mem_top
#define MAX_DEPTH 8

extern IoIa32Context cpu;
extern const IoServices *io;
static inline u16 rw(unsigned r) {return (u16)cpu.gr[r];}
static inline void ww(unsigned r,u16 v) {cpu.gr[r]=(cpu.gr[r]&~0xffffULL)|v;}
static inline u8 rl(unsigned r) {return (u8)cpu.gr[r];}
static inline u8 rh(unsigned r) {return (u8)(cpu.gr[r]>>8);}
static inline void wl(unsigned r,u8 v) {cpu.gr[r]=(cpu.gr[r]&~0xffULL)|v;}
static inline void wh(unsigned r,u8 v) {cpu.gr[r]=(cpu.gr[r]&~0xff00ULL)|(u64)v<<8;}
static inline u16 peek16(u32 a) {const u8 *p=LINEAR(a); return (u16)(p[0]|p[1]<<8);}
static inline void poke16(u32 a,u16 v) {u8 *p=LINEAR(a); p[0]=(u8)v; p[1]=(u8)(v>>8);}
static inline u32 peek32(u32 a) {return peek16(a)|(u32)peek16(a+2)<<16;}
static inline void poke32(u32 a,u32 v) {poke16(a,(u16)v); poke16(a+2,(u16)(v>>16));}
static inline void set_cf(void) {cpu.eflags|=FL_CF;}
static inline void clear_cf(void) {cpu.eflags&=~(u64)FL_CF;}
static inline void fail(int e) {ww(EAX,(u16)e); set_cf();}
/* Two decimal digits in BCD, as the CMOS clock keeps them. */
static inline u8 bcd(unsigned v) {return (u8)(((v/10)<<4)|(v%10));}
/* A segment register's selector in the processor's state (the word and its
 * shift there), and its descriptor. */
static inline u64 *selector_word(unsigned s,unsigned *shift) {
    switch(s) {
    case SR_ES: *shift=16; return &cpu.data_sel;
    case SR_CS: *shift=0; return &cpu.sys_sel;
    case SR_SS: *shift=16; return &cpu.sys_sel;
    case SR_DS: *shift=0; return &cpu.data_sel;
    case SR_FS: *shift=32; return &cpu.data_sel;
    default: *shift=48; return &cpu.data_sel;
    }
}
static inline u64 *descriptor_of(unsigned s) {
    switch(s) {
    case SR_ES: return &cpu.es; case SR_CS: return &cpu.cs; case SR_SS: return &cpu.ss;
    case SR_DS: return &cpu.ds; case SR_FS: return &cpu.fs; default: return &cpu.gs;
    }
}
u16 sreg(unsigned);
void set_sreg(unsigned,u16);
u64 real_desc(u16);
void push16(u16);
u16 pop16(void);
u32 ivt(unsigned);
void set_ivt(unsigned,u32);
u32 stub(unsigned);
/* The interrupted program's next instruction, for exits and EXEC. */
void jump(u16 cs,u16 ip);

/* Memory control blocks (main.c). */
int mem_alloc(u16 paras,u16 owner,u16 *seg,u16 *largest);
int mem_free(u16 seg);
int mem_resize(u16 seg,u16 paras,u16 *largest);
void mem_free_owner(u16 psp);
extern unsigned alloc_strategy;

/* Processes (main.c). */
extern u16 cur_psp;
extern u32 dta; /* linear */
extern u16 return_code;
u8 *jft(u16 psp);
u16 jft_size(u16 psp);
int handle_native(u16 h,unsigned *native);
int handle_new(unsigned native,u16 *h);
int exec_program(const char *path,u32 block); /* AH=4B00h */
int load_overlay(const char *path,u32 block); /* AH=4B03h */
void terminate(u8 code,u8 kind,u16 keep_paras); /* keep_paras: AH=31h */
int is_dos16(const char *path);
void sync_dta(void);
void sync_std(void);
void request_exit(u8 code,u8 kind,u16 keep);
extern int break_pending;

/* Services. */
void int21(void);
int bios_int(unsigned n); /* 0 if not a BIOS service */
void bios_ticks(void);
void bios_init(void);
void video_close(void);
void console_out(const u8 *,u32);
void print_release(u16 psp); /* PRINT's queue held by INT 2Fh 0104h (bios.c) */
/* The VGA (vga.c) while the program has it; the calls return 0 otherwise. */
void vga_init(void); /* fonts and vectors in low memory */
void vga_open(void);
void vga_close(void);
void vga_resume(void); /* after a native child */
#define VGA_PLANE 65536u
int vga_away(u8 *planes); /* 4*VGA_PLANE bytes; 0 when VDM has no adapter */
void vga_back(const u8 *planes);
int vga_int10(void);
int vga_tty(u8 c,u8 color);
int video_port(u16 port,u8 *value,int write); /* 3B0h-3DFh */
/* XMS (xms.c) and EMS (ems.c), when HIMEM.SYS and EMM386.SYS are there. */
void xms_init(void);
int xms_present(void);
void xms_call(void);
void xms_close(void);
u32 xms_used_kb(void);
int xms_owns(u32 linear,u32 length);
void ems_init(void);
int ems_present(void);
u16 ems_frame(void);
void ems_call(void);
void ems_return(void);
void ems_close(void);
int ems_device(const char *path); /* the EMMXXXX0 device's name */
void emm_closed(unsigned native); /* dos.c */
/* DPMI (dpmi.c). */
void dpmi_switch(void);
void dpmi_return(void);
void dpmi_raw_to_pm(void);
void dpmi_callback(unsigned);
void dpmi_exit(void);
int dpmi_timer(void);
int dpmi_real_interrupt(unsigned n);
int dpmi_active(void);
int dpmi_available(void);
void dpmi_terminate(u16 psp);
void dpmi_close(void);
u32 dpmi_used_kb(void);
/* Ports (ports.c). */
u32 port_in(u16 port,unsigned size);
void port_out(u16 port,unsigned size,u32 value);
int emulate_privileged(void); /* port I/O and HLT after a fault */
u32 timer_period_us(void); /* counter 0's period */
int irq0_masked(void);
extern int irq0_in_service; /* until the handler's end of interrupt */
#endif
