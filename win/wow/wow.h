/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL internals: Windows 3.0 (Win16, NE format) programs on the
 * processor's IA-32 instruction set. C89.
 *
 * A Win16 program runs as an ordinary KERNEL task whose fiber loops on
 * wh_ia32_run: its segments are 16-bit protected-mode segments at privilege
 * 3, described by one local descriptor table shared by every Win16 task
 * (selectors are table index * 8 + 7). Imported functions are far
 * addresses in a thunk segment per module (KERNEL, USER, GDI, ...): the
 * thunk at ordinal * 4 is an INT instruction that ends the run, and the
 * function's entry in the API table reads the Pascal (or C) arguments from
 * the 16-bit stack, calls the native function and returns as RETF n would.
 * Native code calls 16-bit procedures (window and dialog procedures) on the
 * task's own stack, with a far return into the return thunk.
 */
#ifndef WOW_H
#define WOW_H
#include <windows.h>
#include <winstart.h>
#include <winhost.h>
#include <string.h>

/* Selectors. Memory below 4 GiB, 16-bit segments, DPL 3. */
#define SEL_CODE 0xfbU /* present, DPL 3, code, execute/read, accessed */
#define SEL_DATA 0xf3U /* present, DPL 3, data, read/write, accessed */
WORD SelAlloc(DWORD base,DWORD limit,BYTE access);
/* Consecutive selectors for a block over 64 KiB, 64 KiB apart (__AHINCR 8). */
WORD SelAllocTiled(DWORD base,DWORD size,BYTE access);
void SelSetTiled(WORD sel,DWORD base,DWORD size,BYTE access);
void SelFreeTiled(WORD sel,DWORD size);
void SelFree(WORD sel);
DWORD SelBase(WORD sel);
DWORD SelLimit(WORD sel);
void SelSet(WORD sel,DWORD base,DWORD limit,BYTE access);
BOOL SelValid(WORD sel);
BOOL SelIsCode(WORD sel);
wh_u64 SelDescriptor(WORD sel); /* register format for WhIa32 */
void *SelPointer(WORD sel,WORD offset);
DWORD SelLinear(void *p); /* 0 when above 4 GiB */
BOOL LdtInit(void);
void LdtShutdown(void);
extern wh_u64 ldt_descriptor,gdt_descriptor;
/* Memory for 16-bit segments, zeroed; below 4 GiB. */
void *Alloc16(DWORD bytes);
void Free16(void *p,DWORD bytes);

typedef struct Task16 Task16;
/* A loaded NE module. */
#define MAX_SEGMENTS 64
typedef struct {
    WORD sel,flags;
    DWORD size;
    BYTE *memory;
} Segment;
typedef struct Module16 {
    BOOL used;
    char name[9],path[128];
    BYTE *image; DWORD image_size; DWORD ne; /* file image, NE header offset */
    WORD segments; Segment seg[MAX_SEGMENTS];
    WORD autodata,heap,stack,flags;
    DWORD heap_start; /* DGROUP offset after the data and the stack */
    WORD cs,ip,ss,sp; /* segment numbers and offsets from the header */
    WORD instance; /* DGROUP selector (a library without data: the module handle) */
    WORD handle; /* module database selector: the NE header */
    BOOL library,initialized; WORD usage; /* libraries: LibEntry has run; holders */
    struct Module16 *imports[16]; WORD import_count; /* libraries this module holds */
} Module16;
int NeLoad(LPCSTR path,Module16 **out); /* a program: 0, or a WinExec error */
int NeLoadLibrary(LPCSTR name,LPCSTR near_path,Module16 **out); /* held once more */
void NeRelease(Module16 *library); /* WEP and unloading with the last holder */
void NeReleaseImports(Module16 *m);
BOOL NeInitLibraries(Task16 *t,Module16 *m);
void NeFree(Module16 *m);
const BYTE *NeResource(Module16 *m,LPCSTR type,LPCSTR name,DWORD *size);
Module16 *NeFromHandle(WORD instance_or_module);
Module16 *NeFromName(LPCSTR module_name);
Module16 *NeFromCode(WORD sel); /* the module a segment belongs to */
BOOL NeEntry(Module16 *m,WORD ordinal,DWORD *address);
WORD NeOrdinal(Module16 *m,LPCSTR name);
void NeModuleName(LPCSTR name,char *base); /* a file name's base name, at most eight characters */

/* Win16Main's result when the processor has no IA-32 instruction set. */
#define WOW_NO_IA32 0x100
/* A Win16 task. */
struct Task16 {
    WhIa32 cpu;
    Module16 *module;
    WORD psp; /* also the task handle */
    HTASK native;
    int show;
    char command[128];
    DWORD depth; /* nested calls into 16-bit code */
    BYTE *scratch; WORD scratch_sel,scratch_top; /* converted structures */
    DWORD dta; /* far pointer */
    WORD environment; /* selector of the DOS environment copy */
    Module16 *loaded[16]; /* LoadLibrary without FreeLibrary yet */
};
#define SCRATCH_BYTES 8192U
Task16 *CurrentTask16(void);
Task16 *Task16Of(WORD task);
WORD Task16Handle(HTASK native); /* 0 for a native task */
/* Arguments of a 16-bit call: values in declaration order. */
typedef struct {
    Task16 *task;
    DWORD a[16];   /* converted */
    DWORD raw[16]; /* as on the stack */
    WORD stack; /* SP of the first argument above the return address */
    const char *module,*name; /* the function called */
} Args16;
/* Calls a 16-bit far procedure with WORD/DWORD arguments (Pascal order);
 * sizes: 2 or 4 per argument. Returns DX:AX. */
DWORD Call16(Task16 *t,DWORD proc,int count,const DWORD *args,const BYTE *sizes);
void TaskEnd16(Task16 *t,int code);
BOOL CallLibEntry16(Task16 *t,Module16 *library);
void CallWep16(Task16 *t,Module16 *library);
void Fault16(Task16 *t,LPCSTR what);

/* Registers. */
#define AX 0
#define CX 1
#define DX 2
#define BX 3
#define SP 4
#define BP 5
#define SI 6
#define DI 7
WORD Reg16(Task16 *t,int r);
void SetReg16(Task16 *t,int r,WORD v);
WORD Seg16(Task16 *t,int s); /* 0 ES, 1 CS, 2 SS, 3 DS */
void SetSeg16(Task16 *t,int s,WORD sel);
void *Lin16(DWORD segptr); /* a far pointer as a native pointer, NULL for 0 */
WORD Peek16(WORD sel,WORD off);
/* WORDs and DWORDs of 16-bit structures and files (unaligned, little-endian). */
static __inline WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static __inline DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
static __inline void put16(BYTE *p,WORD v) {p[0]=(BYTE)v; p[1]=(BYTE)(v>>8);}
static __inline void put32(BYTE *p,DWORD v) {put16(p,LOWORD(v)); put16(p+2,HIWORD(v));}

/* API thunks. */
#define THUNK_INT 0x30
BOOL ThunkInit(void);
void ThunkShutdown(void);
DWORD ThunkAddress(LPCSTR module,WORD ordinal); /* 0 when unknown */
WORD ThunkOrdinal(LPCSTR module,LPCSTR name);
BOOL ThunkImplemented(LPCSTR module,WORD ordinal);
void ThunkCall(Task16 *t);
BOOL IsThunk(WORD sel);
LPCSTR ThunkModuleName(WORD sel); /* a system module handle's name */
/* A native window procedure as a 16-bit far procedure, and back. */
DWORD NativeProc16(WNDPROC p);
WNDPROC NativeProcOf(DWORD segptr);
DWORD InstanceThunk16(DWORD proc,WORD ds); /* MakeProcInstance */
WORD ThunkModuleHandle(LPCSTR name);
WORD GlobalAlloc16(Task16 *t,UINT flags,DWORD size); /* t NULL: no owner */
BOOL GlobalFree16(WORD sel);
DWORD GlobalSize16(WORD sel);
void Resources16Freed(Module16 *m);
void System16TaskEnded(Task16 *t); /* MMSYSTEM's timers */
void GlobalTaskEnded16(Task16 *t);
BOOL LocalInit16(WORD sel,WORD start,WORD end);
void LocalTaskEnded16(Module16 *m);
WORD Environment16(void);
extern WORD return_thunk;
void Int21(Task16 *t);

/* Scratch memory in the task's 16-bit space, taken and given back in LIFO
 * order, and selectors that alias native memory (below 4 GiB). */
void *Scratch16(Task16 *t,WORD bytes,DWORD *segptr);
DWORD AliasPointer(const void *p);
void AliasFree(DWORD segptr);

/* Window and dialog procedures (msg16.c). A native message goes to a
 * 16-bit procedure, or a 16-bit message to a native procedure, with its
 * parameters converted both ways. */
LRESULT CallProc16(Task16 *t,DWORD proc,HWND h,UINT msg,WPARAM wp,LPARAM lp);
DWORD CallNative16(Task16 *t,WNDPROC proc,WORD h,WORD msg,WORD wp,DWORD lp);
void Msg16To32(WORD msg16,WORD wp16,DWORD lp16,UINT *msg,WPARAM *wp,LPARAM *lp);
void Msg32To16(UINT msg,WPARAM wp,LPARAM lp,WORD *msg16,WORD *wp16,DWORD *lp16);
LRESULT WINAPI WowWndProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR WINAPI WowDlgProc(HWND,UINT,WPARAM,LPARAM);
DWORD FindReplace16(LPARAM lp); /* FINDMSGSTRING's structure for the program (comm16.c) */
DWORD ClassProc16(HWND h); /* the 16-bit procedure of a window's class (user16.c) */
DWORD WindowProc16(HWND h); /* its own (subclassed) or its class's */
void WindowDestroyed16(HWND h);
DWORD DialogProc16(HWND h); /* a 16-bit dialog's procedure, 0 for others */
BOOL SetDialogProc16(HWND h,DWORD proc);
void User16TaskEnded(Module16 *m);
void DialogBound16(HWND dlg); /* WM_INITDIALOG hook: icons (user16.c) */
extern DWORD pending_dialog_proc;

/* Handles: USER's and GDI's are the 16-bit values themselves (user.h), so
 * a 16-bit form is the native one's low word and back. 0xFFFF is
 * HWND_BROADCAST in both. */
static __inline WORD Handle16(ULONG_PTR v) {return (WORD)v;}
#define HWND16(h) Handle16((ULONG_PTR)(h))
static __inline HWND Hwnd32(DWORD h) {return (HWND)(ULONG_PTR)(WORD)h;}
#define HWND32(h) Hwnd32((DWORD)(h))
#define HGDI16(h) Handle16((ULONG_PTR)(h))
#define HGDI32(h) ((HGDIOBJ)(ULONG_PTR)(WORD)(h))
#define HMENU16(h) Handle16((ULONG_PTR)(h))
#define HMENU32(h) ((HMENU)(ULONG_PTR)(WORD)(h))
#define HACCEL16(h) Handle16((ULONG_PTR)(h))
#define HACCEL32(h) ((HACCEL)(ULONG_PTR)(WORD)(h))
#define HICON16(h) Handle16((ULONG_PTR)(h))
#define HICON32(h) ((HICON)(ULONG_PTR)(WORD)(h))
#endif
