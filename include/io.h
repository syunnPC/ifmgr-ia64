/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_IO_H
#define DOS_IO_H
#include "base.h"

/* IO.SYS boundary: native C types only, no EFI handles or protocol structures.
 * Tables are append-only within an ABI version; consumers check version/size.
 * Call at application priority on the boot CPU. Timer callbacks count ticks and
 * sample buffered UART claims at TPL_NOTIFY; they never call DOS, graphics or
 * console services. Boot Services own the interrupt controllers, so no device
 * IRQ reaches IO.SYS; "buffered" means timer-sampled, not interrupt-driven.
 */
#define IO_ABI_VERSION 1
#define IO_SERVICES_GUID {0x1e9400d4,0x1a64,0x4e11,{0x9d,0x47,0x69,0x6f,0x73,0x79,0x73,0x01}}
#define IO_CAP_FIRMWARE_BOOT 1ULL
#define IO_CAP_GRAPHICS 2ULL
#define IO_CAP_POINTER 4ULL
#define IO_CAP_TIMER 8ULL
#define IO_CAP_KEY_MODIFIERS 16ULL
#define IO_CAP_DISKS 32ULL
#define IO_CAP_CONSOLE_KEY 64ULL
#define IO_CAP_MODULES 128ULL
#define IO_CAP_LEGACY_PORTS 256ULL
#define IO_CAP_CLOCK 512ULL
#define IO_CAP_PHYSICAL 1024ULL
#define IO_CAP_RESIDENT 2048ULL
#define IO_CAP_PORT_BUFFER 4096ULL
#define IO_CAP_SERIAL 8192ULL
#define IO_CAP_CDROM 16384ULL
#define IO_CAP_IA32 32768ULL
#define IO_CAP_VGA_TEXT 65536ULL
#define IO_CAP_TEXT_SCREEN 131072ULL
#define IO_CAP_HMA 262144ULL
#define IO_CLOCK_DATE 1U
#define IO_CLOCK_TIME 2U
#define IO_PORT_UART 1U
#define IO_PORT_PRINTER 2U
#define IO_PORT_SERIAL 3U /* Firmware serial unit (EFI Serial I/O); never claimed by base. */
#define IO_PARITY_NONE 0U
#define IO_PARITY_ODD 1U
#define IO_PARITY_EVEN 2U
#define IO_PARITY_MARK 3U
#define IO_PARITY_SPACE 4U
#define IO_STOP_ONE 1U
#define IO_STOP_TWO 2U
#define IO_STOP_ONE_HALF 15U
#define IO_SERIAL_DTR 1U /* Control (set) and status (reported) bits. */
#define IO_SERIAL_RTS 2U
#define IO_SERIAL_LOOPBACK 4U
#define IO_SERIAL_HW_FLOW 8U
#define IO_SERIAL_CTS 16U /* Status only. */
#define IO_SERIAL_DSR 32U
#define IO_SERIAL_RING 64U
#define IO_SERIAL_CARRIER 128U
#define IO_SERIAL_INPUT_EMPTY 256U
#define IO_SERIAL_OUTPUT_EMPTY 512U
#define IO_KEY_PEEK 1U
#define IO_KEY_WAIT 2U
#define IO_DISK_BOOT 1U
#define IO_DISK_REMOVABLE 2U
#define IO_DISK_PRESENT 4U

typedef struct {
    void *ctx;
    int (*read)(void *, u32, void *);
    int (*write)(void *, u32, const void *);
    int (*flush)(void *);
    u64 sectors;
    int readonly;
} Disk;
/* Each unit keeps a stable identity for the lifetime of IO.SYS. Generation is
 * nonzero and changes on media replacement/removal or geometry/protection
 * changes. Disk callbacks are bound to this snapshot and MUST reject access
 * to another generation, including rollback and flush. Missing media is a
 * successful info query with PRESENT clear. Apps access files through DOS. */
typedef struct {u32 generation,flags; Disk disk;} IoDiskInfo;

typedef struct {u32 width,height;} IoDisplay;
#define IO_DISPLAY_CURRENT 1U /* IoDisplayMode flag: the active mode */
#define IO_DISPLAY_KEEP 0xffffffffU /* display_claim: stay in the active mode */
#define IO_DISPLAY_VGA_TEXT 0xfffffffeU /* display_claim: VGA mode 3 (IO_CAP_VGA_TEXT) */
typedef struct {u32 width,height,flags,reserved;} IoDisplayMode;
/* The text console as a screen of character cells (IO_CAP_TEXT_SCREEN):
 * its size, the cursor, the PC attribute text is written in (character
 * color 0-3, background 4-6, 7 blink or, with IO_TEXT_INTENSITY in flags,
 * a bright background). */
#define IO_TEXT_NOWRAP 1U /* text_attribute: text reaching the last column stays there */
#define IO_TEXT_INTENSITY 2U
typedef struct {u32 size,columns,rows,column,row,attribute,flags,reserved;} IoTextScreen;
/* A claimant's own text screen (display_text), as the text_* calls. */
typedef struct {
    u32 size,reserved;
    int (*query)(void *,IoTextScreen *);
    int (*locate)(void *,u32 column,u32 row);
    int (*attribute)(void *,u32 attribute,u32 flags);
    int (*erase)(void *,u32 column,u32 row,u32 cells);
    int (*mode)(void *,u32 mode);
} IoTextOps;
typedef struct {u32 year,month,day,hour,minute,second,nanosecond;} IoDateTime;
typedef struct {u8 blue,green,red,reserved;} IoPixel;
enum {IO_EVENT_NONE,IO_EVENT_KEY,IO_EVENT_POINTER};
enum {
    IO_SCAN_NONE,IO_SCAN_UP,IO_SCAN_DOWN,IO_SCAN_RIGHT,IO_SCAN_LEFT,
    IO_SCAN_HOME,IO_SCAN_END,IO_SCAN_INSERT,IO_SCAN_DELETE,IO_SCAN_PAGE_UP,
    IO_SCAN_PAGE_DOWN,IO_SCAN_F1,IO_SCAN_F2,IO_SCAN_F3,IO_SCAN_F4,IO_SCAN_F5,
    IO_SCAN_F6,IO_SCAN_F7,IO_SCAN_F8,IO_SCAN_F9,IO_SCAN_F10,IO_SCAN_F11,IO_SCAN_F12,
    IO_SCAN_ESCAPE,IO_SCAN_PAUSE,IO_SCAN_UNKNOWN=0xffff
};
#define IO_MOD_SHIFT 1U
#define IO_MOD_CONTROL 2U
#define IO_MOD_ALT 4U
#define IO_MOD_LOGO 8U
#define IO_MOD_RIGHT_CONTROL 0x10U /* with IO_MOD_CONTROL: the right one is down */
#define IO_MOD_RIGHT_ALT 0x20U /* with IO_MOD_ALT: the right one (AltGr) is down */
#define IO_MOD_SCROLL_LOCK 0x100U /* the lock states, with IO_KEY_TOGGLES_VALID */
#define IO_MOD_NUM_LOCK 0x200U
#define IO_MOD_CAPS_LOCK 0x400U
#define IO_KEY_MODIFIERS_VALID 1U
#define IO_KEY_RELEASE 2U /* Reserved for native input backends; EFI emits key strokes only. */
#define IO_KEY_TOGGLES_VALID 4U
typedef struct {
    u32 type,unicode,scan,buttons,flags,modifiers;
    int32_t dx,dy,dz;
    u64 time_ms;
} IoEvent;

/* Firmware serial line settings. timeout_us bounds each byte of a read or
 * write; zero never waits. Baud 50..921600; data 5..8; parity IO_PARITY_*;
 * stop IO_STOP_*; control IO_SERIAL_DTR/RTS/LOOPBACK/HW_FLOW. */
typedef struct {u32 size,baud,timeout_us,data_bits,parity,stop_bits,control,reserved;} IoSerialConfig;

/* A CD-ROM drive: 2048-byte sectors, read-only. flags: IO_DISK_PRESENT.
 * generation changes whenever the disc is changed or removed. */
typedef struct {u32 size,generation,flags,sector_size; u64 sectors;} IoCdInfo;

/* The processor's own IA-32 instruction set (IO_CAP_IA32: Itanium and
 * Itanium 2 processors before the 9000 series). ia32_run enters the image
 * below and returns at the next IA-32 interruption with the image updated.
 * Registers follow the IA-32 application register mapping (SDM vol. 1
 * 6.2): descriptors are in register format (base 31:0, limit 51:32, type
 * 55:52, s 56, dpl 58:57, p 59, d/b 62, g 63), data_sel packs DS|ES|FS|GS
 * and sys_sel CS|SS|LDT|TR as 16-bit fields, cflg is CR0|CR4<<32, and fp
 * holds FR8-FR31 as spill images (the x87/MMX and SSE registers). eip is
 * relative to the CS base. Linear addresses are physical addresses; the
 * firmware's vector table at 10000h-17FFFh and memory DOS did not allocate
 * must not be used. cpl is the privilege level to run at (0 for real mode,
 * 3 for virtual-8086 mode).
 * exit is IO_IA32_*; vector and code are the interruption's IA-32 vector
 * (or intercept kind) and code; iim holds the opcode bytes of an
 * instruction intercept or the old EFLAGS of a system flag trap; fault_ip is
 * the linear address of the instruction that trapped. After an INT n or a
 * trap, eip is the next instruction; after a fault or an intercept it is the
 * faulting one. IRET, HLT and other system instructions are intercepted,
 * as is every MOV SS/POP SS (resume at eip).
 * timer_ms, when not 0, also ends the run with IO_IA32_TIMER once that many
 * milliseconds have passed (between instructions; the timer is the
 * firmware's periodic callback, so the resolution is coarse). */
enum {IO_IA32_EXCEPTION=1,IO_IA32_INTERCEPT,IO_IA32_INTERRUPT,IO_IA32_TIMER};
typedef struct {
    u64 fp[24][2];
    u64 gr[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    u64 eip,eflags,data_sel,sys_sel;
    u64 es,ds,fs,gs,ldt,gdt,cs,ss;
    u64 cflg,fsr,fcr,fir,fdr;
    u32 cpl,exit,vector,code;
    u64 iim,ifa,fault_ip;
    u32 timer_ms,reserved;
} __attribute__((aligned(16))) IoIa32Context;

typedef struct IoServices {
    u32 version,size;
    u64 capabilities;
    void *context;
    Disk boot_disk; /* MSDOS.SYS owns this volume; GUI code must use DOS file calls. */
    void (*console_write)(void *,const void *,size_t);
    int (*console_read)(void *);
    void (*console_clear)(void *);
    int (*datetime)(void *,unsigned[7]);
    int (*alloc_pages)(void *,u32,void **);
    void (*free_pages)(void *,void *,u32);
    int (*exec)(void *,const void *,u32,const char *,unsigned *);
    void (*exit_image)(void *,unsigned);
    void (*shutdown)(void *);
    int (*publish_dos)(void *,void *);
    /* Nonblocking input and bounded idle for a message loop. */
    int (*poll_event)(void *,IoEvent *);
    int (*wait)(void *,u32);
    u64 (*ticks_ms)(void *);
    int (*display_info)(void *,IoDisplay *);
    /* BGRA pixels, stride in pixels; readback!=0 reads into the buffer. */
    int (*display_blt)(void *,IoPixel *,u32,u32,u32,u32,u32,int);
    u32 (*disk_count)(void *);
    int (*disk_info)(void *,u32,IoDiskInfo *);
    /* Keyboard-only access to the same queue used by poll_event. PEEK retains
     * the event for either consumer; WAIT blocks until a key or error. Pointer
     * events must not be consumed by console reads or flushes. */
    int (*console_key)(void *,IoEvent *,unsigned);
    /* Resident native driver images. Tokens are opaque and unique for each
     * successful load; unload follows DOS FINISH and removal of all references.
     * Failed startup leaves no executable image or callable entry points. */
    int (*module_load)(void *,const void *,u32,u64 *);
    int (*module_unload)(void *,u64);
    /* Explicitly configured, exclusive legacy peripheral resources. IO.SYS
     * validates the bus/range and protects the firmware console. Tokens are
     * unique until release; register offsets are checked on every operation.
     * No IRQ, DMA or direct firmware types cross this boundary. */
    int (*port_claim)(void *,u32,u32,u64 *); /* kind, I/O base, token */
    int (*port_release)(void *,u64);
    int (*port_read)(void *,u64,u32,u8 *);
    int (*port_write)(void *,u64,u32,u8);
    int (*stall_us)(void *,u32); /* bounded delay, no console input consumed */
    /* A single local wall-clock snapshot. Years 1900-9999; precision depends
     * on the clock backend. DATE/TIME select fields to update; other fields
     * and backend timezone/daylight metadata are preserved. No timezone
     * conversion is implied. Setting wall time must not change ticks_ms.
     * Unsupported hardware returns DE_FUNCTION; failures are never emulated
     * as successful writes. Calls follow the boot-CPU rule above. */
    int (*clock_get)(void *,IoDateTime *);
    int (*clock_set)(void *,const IoDateTime *,u32);
    /* UTF-16 console text; LF becomes CR LF and U+0000 is skipped, as with
     * console_write. MSDOS.SYS transcodes CON bytes with its code page. */
    void (*console_write_text)(void *,const u16 *,size_t);
    /* Whole devices for partitioning tools, with the same snapshot rules as
     * disk_info. Blocks larger than 512 bytes are presented as 512-byte
     * sectors by read-modify-write. Writes overlapping any published
     * disk_info unit on the device fail with DE_ACCESS. */
    u32 (*physical_count)(void *);
    int (*physical_info)(void *,u32,IoDiskInfo *);
    /* Physical index and first 512-byte sector of a disk_info unit. */
    int (*disk_location)(void *,u32,u32 *,u64 *);
    void (*restart)(void *);
    /* A running subsystem-11 image started by exec ends but stays loaded; its
     * module token is stored before it exits. Returns only on failure.
     * module_unload releases it. Other exec images unload on return. */
    int (*exit_resident)(void *,unsigned,u64 *);
    /* EXEC in two steps (AH=4B01h): load without starting (token, image base
     * and size), then start (as exec) or discard. Unstarted images belong to
     * IO.SYS, so their loader may exit first. */
    int (*image_load)(void *,const void *,u32,u64 *,u64 *,u64 *);
    int (*image_start)(void *,u64,const char *,unsigned *);
    int (*image_discard)(void *,u64);
    /* Timer-sampled receive buffering for a claimed UART (IO_CAP_PORT_BUFFER).
     * While enabled, IO.SYS moves received bytes from the 16550 FIFO into a
     * 4 KiB ring whenever its timer runs. Register access stays transparent:
     * RBR reads (DLAB clear) return ring bytes in arrival order, LSR reports DR
     * for ring data and every line error seen since the previous LSR read, and
     * an FCR receive reset also discards the ring. A full ring leaves bytes in
     * the FIFO. Releasing a claim stops buffering. Pending counts ring bytes. */
    int (*port_buffer)(void *,u64,u32); /* token, nonzero enables */
    int (*port_pending)(void *,u64,u32 *);
    /* Firmware serial units (IO_CAP_SERIAL), excluding every console device.
     * Units are stable for the IO.SYS lifetime; open is exclusive and close
     * restores the line settings found at open. Read/write report completed
     * bytes even on failure; DE_NOTREADY means the per-byte timeout expired. */
    u32 (*serial_count)(void *);
    int (*serial_open)(void *,u32,u64 *);
    int (*serial_close)(void *,u64);
    int (*serial_config)(void *,u64,const IoSerialConfig *);
    int (*serial_status)(void *,u64,u32 *); /* IO_SERIAL_* bits */
    int (*serial_read)(void *,u64,void *,u32,u32 *);
    int (*serial_write)(void *,u64,const void *,u32,u32 *);
    /* Graphics modes (IO_CAP_GRAPHICS) by index from 0; DE_NOMORE ends the
     * list. display_blt pixels are BGRA in every mode. */
    int (*display_mode)(void *,u32,IoDisplayMode *);
    /* Graphical claims select a mode (or IO_DISPLAY_KEEP) and clear it to
     * black. While claimed, console text goes only to identified firmware serial
     * (or is dropped), and console_clear leaves the screen alone. Release
     * restores the previous mode and clears text. Image exit/unload releases
     * its claims.
     *
     * IO_DISPLAY_VGA_TEXT (IO_CAP_VGA_TEXT) claims VGA as the DOS console.
     * First use sets mode 3: 80x25 CP437 character/attribute cells at B8000h,
     * initialized from the console shadow and cursor. Later claims preserve
     * adapter state; vga_port accesses registers. Console output uses the
     * displayed page/hardware cursor and serial unless display_console handles
     * it; console_clear clears that page. Nested VGA claims return the adapter
     * to the previous claimant. After final release, VGA remains the console,
     * restoring mode 3 if needed, until a graphics claim or IO.SYS exit sets a
     * GOP mode. display_blt fails while VGA is active. */
    int (*display_claim)(void *,u32,u64 *);
    int (*display_release)(void *,u64);
    /* CD-ROM drives (IO_CAP_CDROM), stable for the IO.SYS lifetime. A read
     * names the generation it expects and fails with DE_CHANGED once the disc
     * has changed; without a disc it fails with DE_NOTREADY. Status queries
     * and failed reads look for a newly inserted disc. */
    u32 (*cd_count)(void *);
    int (*cd_info)(void *,u32,IoCdInfo *);
    int (*cd_read)(void *,u32 unit,u32 generation,u64 sector,u32 count,void *buffer);
    int (*ia32_run)(void *,IoIa32Context *); /* see IoIa32Context */
    /* A VGA register port (3B0h-3DFh) while token holds the screen in
     * IO_DISPLAY_VGA_TEXT: one byte read or written. */
    int (*vga_port)(void *,u64 token,u32 port,u8 *value,int write);
    /* While token holds the screen, console text goes to draw (as well as to
     * the serial device) instead of the screen, for a claimant that keeps
     * its own; a console_clear comes as text NULL. draw NULL ends that. */
    int (*display_console)(void *,u64 token,void (*draw)(void *context,const u16 *text,size_t n),void *context);
    /* The image an action is attributed to (display claims, which end with
     * it, and exit_image): returned, and replaced unless image is NULL. A
     * program running another on a fiber of its own keeps each one's across
     * a switch between them. */
    void *(*image_switch)(void *,void *image);
    /* The firmware console's text screen while no claim holds the display
     * (80x25 characters and attributes, the cursor) kept in a buffer of
     * IO_CONSOLE_SAVE_BYTES, and later shown again: for a DOS program set
     * aside while another uses the screen. DE_ACCESS while it is claimed or
     * the console is on the VGA; DE_FUNCTION without a VGA adapter. */
    int (*console_save)(void *,void *,u32);
    int (*console_restore)(void *,const void *,u32);
    /* Text-screen operations (IO_CAP_TEXT_SCREEN) for ANSI.SYS:
     * query=size/cursor; locate=move cursor; attribute=set output/erase/scroll
     * attributes, with IO_TEXT_NOWRAP retaining the last column; erase=blank
     * from a position, keeping the cursor except for full-screen erase (home);
     * mode=set/clear a PC video mode (2/3 use 80x25 text). Before attribute is
     * set, output preserves screen attributes. Firmware drops background
     * intensity/blink; partial erases leave the bottom-right cell to avoid scroll.
     *
     * A display_console claimant supplies IoTextOps via display_text, receiving
     * the current attribute immediately. Missing ops or a graphics claim returns
     * DE_ACCESS. Serial output under VGA/claims uses ANSI sequences. */
    int (*text_query)(void *,IoTextScreen *);
    int (*text_locate)(void *,u32 column,u32 row);
    int (*text_attribute)(void *,u32 attribute,u32 flags);
    int (*text_erase)(void *,u32 column,u32 row,u32 cells);
    int (*text_mode)(void *,u32 mode);
    int (*display_text)(void *,u64 token,const IoTextOps *,void *context);
    /* Pages whose physical addresses lie wholly within [low,high) (below
     * 4 GiB, for memory IA-32 code reaches), released with free_pages or
     * when the image they were taken for ends (range_free: the pages free
     * there and the largest run of them). With IO_CAP_HMA IO.SYS holds the
     * 64 KiB at 100000h, the IA-32 high memory area, for VDMs to share
     * (each keeping its own contents). */
    int (*alloc_pages_range)(void *,u32 pages,u64 low,u64 high,void **out);
    int (*range_free)(void *,u64 low,u64 high,u64 *free_pages,u64 *largest);
} IoServices;
#define IO_CONSOLE_SAVE_BYTES 4096u
#define IO_SERVICES_V1_SIZE offsetof(IoServices,disk_count)
#endif
