/* SPDX-License-Identifier: GPL-2.0-or-later
 * WINHOST.DLL: the built-in module through which WIN.COM gives KERNEL, USER
 * and GDI tasks, display, input, memory, DOS and module services.
 *
 * Shared by the GCC host (C11) and WDK-built modules (C89), so it uses only
 * fixed-width fields, no long, no bitfields; function pointers are function
 * descriptors on both sides.
 *
 * Scheduling contract: tasks are cooperative fibers, each with its own DOS
 * task context, switched only inside wh_yield, wh_block and wh_task_exit.
 * A task with nothing to do calls wh_block; whoever gives it work calls
 * wh_wake (a wake before the block is remembered). When every task is
 * blocked the host calls the idle hook, which may wake tasks and returns the
 * milliseconds until it next needs to run (WH_FOREVER for none); the host then
 * waits for input or that time. The hook runs on the host stack and must not
 * block or yield. WIN.COM returns to DOS when the last task has ended.
 */
#ifndef WINHOST_H
#define WINHOST_H
#ifdef _MSC_VER
typedef unsigned __int64 wh_u64;
typedef __int64 wh_i64;
typedef unsigned int wh_u32;
typedef int wh_i32;
typedef unsigned short wh_u16;
typedef unsigned char wh_u8;
#define WH_IMPORT __declspec(dllimport)
#else
#include <stdint.h>
typedef uint64_t wh_u64;
typedef int64_t wh_i64;
typedef uint32_t wh_u32;
typedef int32_t wh_i32;
typedef uint16_t wh_u16;
typedef uint8_t wh_u8;
#define WH_IMPORT
#endif
#define WH_FOREVER 0xffffffffU
#define WH_TASK_SLOTS 4 /* per-task words: 0 KERNEL, 1 USER, 2 GDI, 3 WOW */

/* Input events; values match include/io.h (IO_EVENT_*, IO_SCAN_*, IO_MOD_*). */
#define WH_EVENT_KEY 1U
#define WH_EVENT_POINTER 2U
#define WH_MOD_SHIFT 1U
#define WH_MOD_CONTROL 2U
#define WH_MOD_ALT 4U
#define WH_MOD_LOGO 8U
#define WH_KEY_MODIFIERS_VALID 1U
enum {
    WH_SCAN_NONE,WH_SCAN_UP,WH_SCAN_DOWN,WH_SCAN_RIGHT,WH_SCAN_LEFT,
    WH_SCAN_HOME,WH_SCAN_END,WH_SCAN_INSERT,WH_SCAN_DELETE,WH_SCAN_PAGE_UP,
    WH_SCAN_PAGE_DOWN,WH_SCAN_F1,WH_SCAN_F2,WH_SCAN_F3,WH_SCAN_F4,WH_SCAN_F5,
    WH_SCAN_F6,WH_SCAN_F7,WH_SCAN_F8,WH_SCAN_F9,WH_SCAN_F10,WH_SCAN_F11,WH_SCAN_F12,
    WH_SCAN_ESCAPE,WH_SCAN_PAUSE
};
typedef struct {
    wh_u32 type,unicode,scan,buttons,flags,modifiers;
    wh_i32 dx,dy,dz,reserved;
    wh_u64 time_ms;
} WhEvent;
/* INT 21h registers (DosRegs); flags bit 0 is carry. */
typedef struct {wh_u64 ax,bx,cx,dx,si,di,flags;} WhRegs;

/* Tasks. entry(arg) runs on a new fiber; returning ends the task. */
WH_IMPORT wh_u32 wh_task_create(int (*entry)(void *),void *arg);
WH_IMPORT void wh_task_exit(int code);
/* End another task: it never runs again and its DOS context goes. */
WH_IMPORT int wh_task_kill(wh_u32 task);
WH_IMPORT wh_u32 wh_task_current(void);
/* WH_TASK_SLOTS words owned by the modules for a task (0: the current one). */
WH_IMPORT void **wh_task_slots(wh_u32 task);
WH_IMPORT void wh_yield(void);
WH_IMPORT void wh_block(void);
WH_IMPORT void wh_wake(wh_u32 task);
WH_IMPORT void wh_set_idle(wh_u32 (*idle)(void));

/* Time, memory and diagnostics. Memory is zeroed whole pages. */
WH_IMPORT wh_u64 wh_ticks(void);
WH_IMPORT void *wh_alloc(wh_u64 bytes);
WH_IMPORT void *wh_alloc_low(wh_u64 bytes); /* below 4 GiB, for IA-32 code */
WH_IMPORT void wh_free(void *memory,wh_u64 bytes);
WH_IMPORT void wh_trace(const char *text);

/* DOS calls in the current task's DOS context. */
WH_IMPORT void wh_int21(WhRegs *regs);

/* The screen WIN.COM claimed: 32-bit BGRX pixels, stride in pixels. */
WH_IMPORT int wh_display_size(wh_u32 *width,wh_u32 *height);
WH_IMPORT int wh_display_blt(const void *pixels,wh_u32 x,wh_u32 y,wh_u32 width,wh_u32 height,wh_u32 stride);
/* Keyboard and pointer input; nonzero when there is none. */
WH_IMPORT int wh_poll_event(WhEvent *event);

/* Modules. Handles are image bases (HINSTANCE/HMODULE). A name or type below
 * 0x10000 is an integer ID (MAKEINTRESOURCE) or ordinal. */
WH_IMPORT void *wh_load_library(const char *name);
WH_IMPORT int wh_free_library(void *module);
WH_IMPORT void *wh_module_handle(const char *name);
WH_IMPORT int wh_module_file_name(void *module,char *buffer,wh_u32 size);
WH_IMPORT const void *wh_proc_address(void *module,const char *name);
/* The resource's IMAGE_RESOURCE_DATA_ENTRY: {data RVA, size, code page, 0}. */
WH_IMPORT const void *wh_find_resource(void *module,const char *type,const char *name);
/* An application image (never shared) and its entry descriptor. */
WH_IMPORT void *wh_load_application(const char *path);
WH_IMPORT const void *wh_entry(void *module);
/* WIN.COM's environment: a variable's value, or the index'th "NAME=value". */
WH_IMPORT int wh_env_get(const char *name,char *out,wh_u32 size);
WH_IMPORT int wh_env_list(wh_u32 index,char *out,wh_u32 size);
/* Run a DOS program full screen, as Windows 3.0's standard mode did: the
 * display goes back to DOS and no task runs while it is on the screen; the
 * result is EXEC's DOS error (0 when it ran), or WH_DOS_AWAY when Alt+Tab,
 * Alt+Esc or Ctrl+Esc set it aside (while it waited for a key), after which
 * wh_dos_resume takes it up again, with the same results. One for each
 * task, in its DOS context; several can be set aside at once.
 * The display epoch changes each time the display is claimed again, when
 * GDI must send the whole screen. */
WH_IMPORT int wh_dos_session(const char *path,const char *tail);
/* The same in a start-up directory (NULL or "" for the current one), which
 * goes back afterwards; WH_DOS_PAUSE waits for a key before Interface
 * Manager returns, WH_DOS_NOSWITCH keeps the switching keys from it. */
#define WH_DOS_PAUSE 1
#define WH_DOS_NOSWITCH 2
/* Keys the program keeps for itself (the PIF's Reserve Shortcut Keys). */
#define WH_DOS_KEEP_ALT_TAB 4
#define WH_DOS_KEEP_ALT_ESC 8
#define WH_DOS_KEEP_CTRL_ESC 16
#define WH_DOS_AWAY 0x7fff0001
WH_IMPORT int wh_dos_session_ex(const char *path,const char *tail,const char *dir,wh_u32 flags);
WH_IMPORT int wh_dos_resume(void);
WH_IMPORT wh_u32 wh_display_epoch(void);

/* The processor's IA-32 instruction set, for Win16 programs: IoIa32Context
 * in include/io.h, with the same layout. The image runs until its next IA-32
 * interruption; nonzero means the processor has no IA-32 instruction set
 * (a NULL context only asks).
 * Linear addresses are physical: segments must lie below 4 GiB. */
#ifdef _MSC_VER
typedef __declspec(align(16)) struct {
#else
typedef struct __attribute__((aligned(16))) {
#endif
    wh_u64 fp[24][2];
    wh_u64 gr[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    wh_u64 eip,eflags,data_sel,sys_sel;
    wh_u64 es,ds,fs,gs,ldt,gdt,cs,ss;
    wh_u64 cflg,fsr,fcr,fir,fdr;
    wh_u32 cpl,exit,vector,code;
    wh_u64 iim,ifa,fault_ip;
    wh_u32 timer_ms,reserved;
} WhIa32;
#define WH_IA32_EXCEPTION 1U
#define WH_IA32_INTERCEPT 2U
#define WH_IA32_INTERRUPT 3U
#define WH_IA32_TIMER 4U
WH_IMPORT int wh_ia32_run(WhIa32 *context);
#endif
