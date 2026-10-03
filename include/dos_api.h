/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_API_H
#define DOS_API_H
#include "io.h"
#include "dos_device.h"
#include "dos_block.h"
#include "dos_nls.h"
#include "dos_redir.h"
#define DOS_ABI_VERSION 1
/* The system's name, as the start-up banner and VER show it. */
#define DOS_PRODUCT "IA-64/EFI DOS Version 4.00"
#define DOS_ENV_CAPACITY 4096
#define DOS_ENV_NAME_MAX 127
#define DOS_API_GUID {0x7b57290a,0xa9eb,0x4d76,{0x93,0x6e,0xa4,0x46,0xb6,0x9f,0x21,0x04}}
/* A native call frame, not hardware registers. All pointer fields are 64-bit. */
typedef struct {u64 ax,bx,cx,dx,si,di,flags;} DosRegs;
typedef struct {
    u32 index; u16 dir; u8 mask[11],search_attr;
    u8 attr,cookie_low; u16 time,date; u32 size; char name[13]; u8 cookie_high[3];
} DosFind;
/* The old 44-byte DTA layout is preserved. Index/cookie fields are DOS-owned;
 * copy the entire DTA to retain a search, including across drive changes. */
_Static_assert(sizeof(DosFind)==44 && offsetof(DosFind,time)==20 && offsetof(DosFind,name)==28,"DTA ABI");
#define DOS_DRIVES 26
#define FA_RDONLY 1
#define FA_HIDDEN 2
#define FA_SYSTEM 4
#define FA_VOLUME 8
#define FA_DIR 16
#define FA_ARCHIVE 32
#define DOS_OPEN_READ 0U
#define DOS_OPEN_WRITE 1U
#define DOS_OPEN_RDWR 2U
#define DOS_SHARE_COMPAT 0U
#define DOS_SHARE_DENY_ALL 0x10U
#define DOS_SHARE_DENY_WRITE 0x20U
#define DOS_SHARE_DENY_READ 0x30U
#define DOS_SHARE_DENY_NONE 0x40U
#define DOS_OPEN_PRIVATE 0x80U
#define DOS_OPEN_FAIL_ERRORS 0x2000U
#define DOS_OPEN_COMMIT 0x4000U
#define DOS_CAP_PARTIAL_IO 1ULL
#define DOS_CAP_DRIVES 2ULL
#define DOS_CAP_CONSOLE 4ULL
#define DOS_CAP_CRITICAL 8ULL
#define DOS_CAP_DEVICES 16ULL
#define DOS_CAP_BLOCK_DRIVERS 32ULL
#define DOS_CAP_DATETIME 64ULL
#define DOS_CAP_FCB 128ULL
#define DOS_CAP_NLS 256ULL
#define DOS_CAP_DISK_IO 512ULL
#define DOS_CAP_PHYSICAL 1024ULL
#define DOS_CAP_RESIDENT 2048ULL
#define DOS_CAP_REDIRECT 4096ULL
#define DOS_CAP_DRIVE_MAP 8192ULL
#define DOS_CAP_APPEND 16384ULL
/* Byte arrays preserve DOS offsets without unaligned IA-64 word accesses.
 * Standard FCB: drive 0, 8.3 name 1, block 12, record size 14, file size 16,
 * date 20, time 22, DOS-owned token 24..31, record 32, random record 33..36.
 * Extended FCB: FFh, five reserved bytes, attribute byte, standard FCB.
 * Copies of an open FCB share a task-owned token; close invalidates all copies.
 * Tokens are not inherited. FILES bounds their shared system file table.
 */
typedef struct {u8 bytes[37];} DosFcb;
typedef struct {u8 marker,reserved[5],attr; DosFcb fcb;} DosExtendedFcb;
_Static_assert(sizeof(DosExtendedFcb)==44 && offsetof(DosExtendedFcb,fcb)==7,"FCB ABI");
/* With DOS_CAP_FCB, AH=1Ah/AL=1 sets DX=DTA, CX=capacity (u32); AL=0 uses
 * 128 bytes for legacy callers. AH=2Fh returns BX=DTA and CX=capacity.
 * FCB calls return DOS status in AL, CF=0, and a native DOS error in BX.
 * AH=27h/28h take/return a u32 record count in CX. DTA capacity replaces the
 * old segment-boundary limit. AH=29h takes SI=text, DI=standard FCB, AL=flags
 * and returns SI at the first unconsumed byte. Uses the active NLS profile.
 * A rejected call (e.g. reentry) uses the normal CF=1/AX=error convention.
 */
typedef struct {u32 year,month,day,hour,minute,second,hundredth,weekday;} DosDateTime;
#define DOS_BREAK_ABORT 0
#define DOS_BREAK_CONTINUE 1
#define DOS_BREAK_CANCEL 2
typedef struct {int (*handler)(void *); void *context;} DosBreakHandler;
typedef struct {int (*hook)(void *,u32 key); void *context;} DosSwitchHook;
typedef struct {u32 size,kind; char path[DOS_PATH_MAX];} DosDriveMap;
#define DOS_MAP_NONE 0U
#define DOS_MAP_SUBST 1U
#define DOS_MAP_JOIN 2U
typedef struct {u32 size,flags; char list[DOS_PATH_MAX];} DosAppend;
/* PRINT's queue (DosApi print): DOS 4's resident part, which MSDOS.SYS is.
 * INSTALL takes the list device (a character device's name) and DOS 4's
 * /B /U /M /S /Q values (0 for the default); SUBMIT a full path; CANCEL a
 * full path whose last component may have ? and *, as a search's; STATUS
 * gives the queue, DOS_PRINT_ENTRY bytes an entry with the file being
 * printed first and an empty entry last, the count of list device errors
 * and the device's name, and holds printing until RELEASE (INT 2Fh
 * 0100h-0106h). */
#define DOS_PRINT_QUERY 0U
#define DOS_PRINT_INSTALL 1U
#define DOS_PRINT_SUBMIT 2U
#define DOS_PRINT_CANCEL 3U
#define DOS_PRINT_CANCEL_ALL 4U
#define DOS_PRINT_STATUS 5U
#define DOS_PRINT_RELEASE 6U
#define DOS_PRINT_ENTRY 64U
typedef struct {
    u32 size;
    char device[9];
    u32 buffer_bytes,busy_ticks,max_ticks,slice_ticks,queue_entries;
    char path[DOS_PRINT_ENTRY];
    const char *queue; u32 errors;
} DosPrintRequest;
/* KEYB resident API (MSDOS.SYS).
 * LOAD uses KEYBOARD.SYS TABLE_BUILD layout: state logic, common translation,
 * then consecutive length-prefixed code-page sections with enhanced-keyboard
 * (G_KB) states. Supply language, ID and an active page present in the sections.
 * CODE_PAGE (INT 2Fh AD81h) returns DE_NOFILE if no tables are loaded.
 * MODE selects US BIOS translation (0) or DOS_KEYB_FOREIGN (AD82h,
 * Ctrl+Alt+F1/F2). QUERY returns state, or DE_FUNCTION before the first LOAD.
 *
 * keyb_key translates IoServices console_key events for direct keyboard
 * readers (VDM INT 16h). keys holds scan<<8 | CON-code-page character;
 * return the count ORed with DOS_KEYB_BIOS if BIOS translation should follow.
 * Only IO_KEY_MODIFIERS_VALID events are mapped, assuming US firmware layout. */
#define DOS_KEYB_QUERY 0U
#define DOS_KEYB_LOAD 1U
#define DOS_KEYB_MODE 2U
#define DOS_KEYB_CODE_PAGE 3U
#define DOS_KEYB_FOREIGN 1U
#define DOS_KEYB_KEYS 4U
#define DOS_KEYB_BIOS 0x80000000U
#define DOS_KEYB_TABLE_MAX 4096U
typedef struct {
    u32 size,flags; /* flags: DOS_KEYB_FOREIGN */
    char language[2]; u16 id,code_page;
    const void *tables; u32 table_size; /* LOAD's */
} DosKeybRequest;
/* MEM (DosApi arena): the blocks of MSDOS.SYS's arena, where programs'
 * memory (AH=48h) and the images EXEC reads lie, in order from 0: where
 * its MCB is and its size, in paragraphs from the arena's start; what it
 * holds (DOS_ARENA_*); the process owning it and that program's name (its
 * image's, without directory or extension), for a DOS_ARENA_PROGRAM block
 * the program whose image it is. DE_NOMORE after the last. */
#define DOS_ARENA_FREE 0U
#define DOS_ARENA_SYSTEM 1U /* MSDOS.SYS's own (code page tables) */
#define DOS_ARENA_PROGRAM 2U
#define DOS_ARENA_DATA 3U
typedef struct {u32 size,kind,paragraph,paragraphs,owner; char name[9];} DosArenaBlock;
/* The extended memory VDM gives 8086 programs, through XMS (HIMEM.SYS) and
 * DPMI together. */
#define DOS_EXTENDED_KB 16384U
#define DOS_APPEND_ENABLED 1U
#define DOS_APPEND_DRIVE 0x1000U /* names with a drive too (/PATH:ON) */
#define DOS_APPEND_PATH 0x2000U /* names with a directory too (/PATH:ON) */
#define DOS_APPEND_ENV 0x4000U /* the list is each task's APPEND= (/E) */
#define DOS_APPEND_X 0x8000U /* searches and EXEC too (/X) */
#define DOS_APPEND_INSTALLED 0x10000U
#define DOS_INSTALLED_SHARE 0U
#define DOS_INSTALLED_FASTOPEN 1U
#define DOS_INSTALLED_NLSFUNC 2U
#define DOS_INSTALLED_GRAFTABL 3U /* the value: its code page */
#define DOS_INSTALLED_ANSI 4U /* the value: DOS_ANSI_* */
#define DOS_ANSI_ON 1U
#define DOS_ANSI_X 2U /* /X: extended keys told apart for reassignment (ESC[1q, ESC[0q) */
#define DOS_ANSI_K 4U /* /K: an 84-key keyboard */
#define DOS_ANSI_L 8U /* /L: rows kept across mode changes */
#define DOS_INSTALLED_XMS 5U /* HIMEM.SYS: DOS_XMS_ON, /HMAMIN in 8-15, /NUMHANDLES in 16-31 */
#define DOS_XMS_ON 1U
#define DOS_INSTALLED_EMS 6U /* EMM386.SYS: kilobytes of expanded memory */
#define DOS_INSTALLED_PROGRAMS 8U
typedef struct {u32 code,kind;} DosExitInfo;
#define DOS_EXIT_NORMAL 0U
#define DOS_EXIT_BREAK 1U
#define DOS_EXIT_CRITICAL 2U
#define DOS_CRITICAL_IGNORE 0U
#define DOS_CRITICAL_RETRY 1U
#define DOS_CRITICAL_ABORT 2U
#define DOS_CRITICAL_FAIL 3U
#define DOS_CRITICAL_READ 0U
#define DOS_CRITICAL_WRITE 1U
#define DOS_CRITICAL_FLUSH 2U
#define DOS_CRITICAL_MEDIA 3U
#define DOS_ERROR_DATA 1U
#define DOS_ERROR_COMMIT 2U
#define DOS_ERROR_MOUNT 4U
#define DOS_ERROR_IGNORED 8U
#define DOS_ERROR_UNRECOVERED 16U
enum {DOS_CLASS_RESOURCE=1,DOS_CLASS_TEMPORARY,DOS_CLASS_PERMISSION,DOS_CLASS_INTERNAL,
      DOS_CLASS_HARDWARE,DOS_CLASS_SYSTEM,DOS_CLASS_APPLICATION,DOS_CLASS_NOT_FOUND,
      DOS_CLASS_FORMAT,DOS_CLASS_LOCKED,DOS_CLASS_MEDIA,DOS_CLASS_EXISTS,DOS_CLASS_OTHER};
enum {DOS_ACTION_RETRY=1,DOS_ACTION_DELAY,DOS_ACTION_USER,DOS_ACTION_ABORT,
      DOS_ACTION_PANIC,DOS_ACTION_IGNORE,DOS_ACTION_INTERVENE};
enum {DOS_LOCUS_UNKNOWN=1,DOS_LOCUS_DISK,DOS_LOCUS_NETWORK,DOS_LOCUS_DEVICE,DOS_LOCUS_MEMORY};
typedef struct {
    u32 size,error,operation,area;
    u32 drive,generation,attempt,allowed;
    u32 pid,flags;
    u64 sector; /* Volume-relative 512-byte sector; UINT64_MAX if unavailable. */
} DosCriticalError;
typedef struct {unsigned (*handler)(void *,const DosCriticalError *); void *context;} DosCriticalHandler;
typedef struct {
    u32 error,error_class,action,locus;
    u32 drive,operation,generation,flags;
    u64 sector;
} DosExtendedError;
#define DOS_DRIVE_PRESENT 1U
#define DOS_DRIVE_REMOVABLE 2U
#define DOS_DRIVE_READONLY 4U
#define DOS_DRIVE_REMOTE 8U /* a redirected drive (include/dos_redir.h) */
#define DOS_DRIVE_SUBST 16U /* a SUBST letter: its drive's figures */
#define DOS_DRIVE_ASSIGNED 32U /* an ASSIGNed letter: the figures of the drive it stands for */
typedef struct {
    u32 drive,flags,generation,fat_bits;
    u32 sectors_per_cluster,total_clusters,free_clusters,reserved;
    u64 sectors;
} DosDriveInfo;
/* Native INT 21h additions:
 * 4402h/4403h: BX character handle, CX length, DX buffer; AX/CX transferred.
 * 440Ch: BX character handle, CX category/function, DX buffer, SI length,
 *        DI driver argument; AX transferred. Driver callbacks cannot call DOS.
 * 57h: BX handle, AL get/set, CX packed time, DX packed date.
 * 5Ch: BX handle, AL lock/unlock, DX 32-bit start, CX 32-bit length.
 *      Locks belong to a task and an open description; dup shares ownership.
 *      Closing the task's last duplicate or destroying it releases its locks.
 * 60h: SI input path, DI output buffer of DOS_PATH_MAX bytes.
 * 6Ch: AL=0, SI path, BX open mode, CX attributes, DX action (01h/02h/10h/11h/12h,
 *      optionally 100h to skip code-page checks); AX handle, CX disposition 1/2/3.
 * With DOS_CAP_PARTIAL_IO, 3Fh/40h and 4402h/4403h return transferred bytes in CX,
 * including on failure (CF=1, AX=error). AX retains the normal success count.
 * 34h: BX points to the u32 InDOS counter. 37h: AL=0/1 get/set DL switch
 *      character; AL=2 returns DL=FFh (DEV prefix optional); others AL=FFh.
 * 50h/51h/62h: BX is a task ID, the native PSP; 50h selects that context.
 * 5Dh: AL=1 commit all; AL=2/4 DX=DosServerCall, close every handle of the
 *      named file (regs.dx) or of task pid; AL=3 has no remote users; AL=5
 *      BX=index returns DI=path, BX=0, CX=locks held; AL=0Ah DX=DosExtendedError.
 * 5Eh: AL=0/1 get/set the machine name (DX, 16 bytes) and number (CX).
 * 5Fh: AL=7/8 enable/disable drive DL (0=A); disabling a current drive fails.
 * 4404h/4405h: BL drive, CX bytes, DX buffer; driver IOCTL, AX transferred.
 * 4409h: DX=0042h (+4000h for driver units) or 1000h for a redirected drive,
 *      +8000h for a SUBST letter. 440Bh: DX retries, CX delay ms.
 * 440Eh/440Fh: AL=0, every unit has one drive letter.
 */
typedef struct {
    u32 pid,errorlevel,in_dos,fat_bits;
    u32 sectors_per_cluster,total_clusters,free_clusters,largest_paragraphs;
} DosInfo;
/* AH=1Fh/32h (DL drive, 0=default) return BX=kernel-owned snapshot, refreshed
 * by each call; AL=FFh for an invalid or unreadable drive. There is no device
 * header or DPB chain. AH=1Bh/1Ch return AL=sectors/cluster, CX=512,
 * DX=clusters and BX=pointer to a kernel-owned media descriptor byte. */
typedef struct {
    u32 size,drive,unit,sector_bytes;
    u32 sectors_per_cluster,cluster_shift,reserved_sectors,fats;
    u32 root_entries,first_root,first_data,clusters;
    u32 sectors_per_fat,media,fat_bits,free_clusters;
    u64 sectors;
} DosDpb;
/* AH=440Dh generic block IOCTL, CH=08h: BX=drive (0=default), DX=buffer.
 * Native blocks address 512-byte LBA sectors instead of CHS tracks:
 * 60h/40h get/set DosDeviceParams, 61h/41h read/write DosSectorIo,
 * 62h/42h verify/format (zero-fill) DosSectorIo, 66h/46h get/set DosMediaId.
 * Writes and formatting require the drive's volume lock held by the caller.
 * 60h returns the BPB DOS 4 would build for the medium (DiskTable2/floppy
 * rules) unless DOS_PARAMS_CURRENT asks for the boot sector's BPB; a BPB set
 * with 40h replaces the build BPB until the medium changes. */
#define DOS_PARAMS_CURRENT 1U
enum {DOS_DEVICE_360K,DOS_DEVICE_1200K,DOS_DEVICE_720K,DOS_DEVICE_FIXED=5,DOS_DEVICE_OTHER=7};
#define DOS_DEVICE_NONREMOVABLE 1U
typedef struct {
    u32 size,flags,device_type,attributes;
    u64 sectors,hidden; /* hidden: first sector on its physical disk */
    u32 physical,media,sector_bytes,sectors_per_cluster; /* physical: UINT32_MAX if unknown */
    u32 reserved_sectors,fats,root_entries,sectors_per_fat;
    u32 fat_bits,sectors_per_track,heads,reserved;
} DosDeviceParams;
typedef struct {u32 size,flags; u64 sector; u32 count,done; void *buffer;} DosSectorIo;
typedef struct {u32 size,serial; char label[11],file_system[8]; u8 reserved[5];} DosMediaId;
_Static_assert(sizeof(DosMediaId)==32,"media ID ABI");
#define DOS_PHYSICAL_BOOT 8U /* With DOS_DRIVE_PRESENT/REMOVABLE/READONLY. */
typedef struct {
    u32 size,index,flags,generation;
    u64 sectors;
    u32 drives,reserved; /* drives: bit n set when drive n lives on this disk */
} DosPhysicalInfo;
/* AH=5Dh/AL=0 executes the DosServerCall registers for the current task and
 * returns that call's results; AL=0Ah sets DosExtendedError as the last error. */
typedef struct {DosRegs regs; u64 uid,pid;} DosServerCall;
/* AH=4B01h: DX path, BX DosExecLoad with size/tail set. The image is loaded
 * and its task created but not started; pid/token/base/image_size/entry are
 * returned. Native AH=4B80h (BX=token) starts it like AH=4B00h; AH=4B81h
 * discards it. Unstarted images are discarded when their loading task ends.
 * AH=4B03h: DX path, BX DosOverlay; copies the file into buffer unchanged
 * (no task, PSP or relocation). AH=31h (AL=exit code) ends a subsystem-11
 * image that stays loaded with the task's memory blocks (DOS_CAP_RESIDENT);
 * its handles close. MSDOS.SYS unloads resident images before drivers when
 * the shell returns. Subsystem-10 applications get DE_FORMAT. */
typedef struct {u32 size,pid; const char *tail; u64 token,base,image_size,entry;} DosExecLoad;
typedef struct {u32 size,capacity; void *buffer; u32 loaded,reserved;} DosOverlay;
/* DOS task contexts hold API state, not CPU stacks or a scheduler. Interface
 * Manager switches them only at cooperative safe points, InDOS == 0.
 * The first four fields preserve the original v1 application ABI.
 */
typedef struct DosApi {
    u32 version,size;
    void (*int21)(DosRegs *);
    const char *(*command_tail)(void); /* a DEVICE= image's, while it loads: its line after the name */
    int (*query)(DosInfo *);
    void (*set_errorlevel)(unsigned);
    void (*console_clear)(void);
    void (*shutdown)(void);
    int (*task_create)(u32 *);
    int (*task_select)(u32);
    int (*task_destroy)(u32);
    const IoServices *io;
    /* Native environment API. Names are case-insensitive printable ASCII without
     * spaces or '='; stored names are uppercase. Values preserve case. Each task
     * owns a copy, bounded by DOS_ENV_CAPACITY bytes including separators.
     * Get/list copy into caller storage (DE_ENV if too small, no truncation).
     * Missing names return DE_NOFILE; list ends with DE_NOMORE.
     * A NULL or empty value deletes a name. Failed updates leave state intact.
     */
    int (*env_get)(const char *,char *,u32);
    int (*env_set)(const char *,const char *);
    int (*env_list)(u32,char *,u32);
    u64 capabilities;
    int (*drive_info)(u32,DosDriveInfo *);
    /* Native INT 23h equivalent. NULL queries; previous is optional. A NULL
     * handler aborts the current image. Callbacks run inside DOS and may not
     * reenter it. CONTINUE resumes, CANCEL returns DE_BREAK from the DOS call,
     * ABORT exits at a safe API boundary (exit kind BREAK). New tasks/images
     * start with the default handler; suspended parents keep their handlers. */
    int (*break_handler)(const DosBreakHandler *,DosBreakHandler *);
    int (*last_exit)(DosExitInfo *); /* Query without consuming AH=4Dh status. */
    /* Native INT 24h. Default: return the underlying error. Handlers may use
     * IO.SYS console services, but cannot reenter DOS. allowed uses 1<<action.
     * Retry repeats only the failed I/O on the same media snapshot. Ignore is
     * offered only for data reads and zero-fills the failed sector. Metadata,
     * writes and flushes cannot be ignored. Fail returns 83; AH=59h/this query
     * retain the original cause. Abort completes rollback before exiting.
     * EXEC inherits the suspended parent's handler; independent tasks do not.
     * DOS_OPEN_FAIL_ERRORS bypasses handlers for open and later handle calls.
     */
    int (*critical_handler)(const DosCriticalHandler *,DosCriticalHandler *);
    int (*extended_error)(DosExtendedError *);
    int (*device_register)(const DosDeviceSpec *); /* DEVICE entry shims only. */
    int (*device_info)(u32,DosDeviceInfo *); /* Stable registration order; DE_NOMORE after the last. */
    int (*block_register)(const DosBlockSpec *); /* DEVICE entry shims only. */
    int (*block_info)(u32,DosBlockInfo *);
    /* One local clock snapshot, Sunday=0; precision follows IO.SYS. Clock
     * changes are system-wide. AH=2Bh/2Dh set date/time (AL=FFh for invalid
     * fields, CF for backend failure). DOS date setting accepts 1980-2099. */
    int (*datetime)(DosDateTime *);
    /* Native INT 25h/26h (DOS_CAP_DISK_IO): drive, volume-relative 512-byte
     * sector, count, buffer, sectors transferred (also on failure). Reads work
     * on unmounted or unmountable media. Writes require volume_lock by this
     * task. Neither runs the critical-error handler; causes return directly. */
    int (*disk_read)(u32,u64,u32,void *,u32 *);
    int (*disk_write)(u32,u64,u32,const void *,u32 *);
    /* Exclusive maintenance lock (1=lock, 0=unlock). Locking fails while files
     * are open on the drive or a transaction is active. While locked, FAT
     * access to the drive fails with DE_ACCESS. Unlocking, or the owner's exit,
     * flushes and forces a remount; searches and directories restart at root. */
    int (*volume_lock)(u32,int);
    /* Whole physical disks (DOS_CAP_PHYSICAL), 512-byte sectors. Writes that
     * overlap a published DOS volume fail with DE_ACCESS. Partition changes
     * become visible after restart, which flushes like shutdown first. */
    int (*physical_info)(u32,DosPhysicalInfo *);
    int (*physical_read)(u32,u64,u32,void *,u32 *);
    int (*physical_write)(u32,u64,u32,const void *,u32 *);
    void (*restart)(void);
    /* Redirected drives (include/dos_redir.h). *drive UINT32_MAX takes the
     * first free letter after the disk drives. The redirector must stay
     * resident; it goes when its task ends without staying resident, or when
     * resident programs are unloaded. unredirect fails while files are open. */
    int (*redirect)(u32 *drive,const DosRedirector *);
    int (*unredirect)(u32 drive);
    /* DOS switch hooks for Alt+Tab/Alt+Esc/Ctrl+Esc during keyboard waits/polls.
     * DOS_SWITCH_QUERY asks whether the hook accepts a key. Accepted keys are
     * consumed; the hook runs with InDOS clear and returns when the program
     * resumes, with the caller's DOS context selected. Rejected keys reach the
     * program. EXEC children inherit the hook. Installing replaces it (NULL
     * removes it) and returns the old hook in previous; VDM saves its state
     * before chaining. For direct keyboard readers, switch_away queries with
     * DOS_SWITCH_QUERY or returns nonzero after switching. */
    int (*switch_hook)(const DosSwitchHook *,DosSwitchHook *previous);
    int (*switch_away)(u32 key);
    /* System-wide SUBST/JOIN/ASSIGN tables (DOS_CAP_DRIVE_MAP).
     * drive_map returns previous and applies set (NULL=query). DOS_MAP_SUBST
     * maps a letter without its own drive to another local drive's directory.
     * DOS_MAP_JOIN hides the drive letter and mounts it at an empty directory
     * one level below another local drive's root. DOS_MAP_NONE removes either;
     * current task drives cannot be mapped. assign returns/replaces 26 letters
     * (NULL=query); an entry equal to its index is unmapped. full_path retains
     * the user-visible drive/directories, unlike AH=60h (TRUENAME), which resolves
     * the underlying drive/path. */
    int (*full_path)(const char *,char[DOS_PATH_MAX]);
    int (*drive_map)(u32 drive,const DosDriveMap *set,DosDriveMap *previous);
    int (*assign)(const u8 *table,u8 *previous);
    /* APPEND fallback lookup (DOS_CAP_APPEND): retry a missing file's last
     * component in each listed directory ("D:\DIR;..."). Applies to opens
     * 0Fh/23h/3Dh/6Ch (existing files); DOS_APPEND_X also covers searches
     * 11h/4Eh and EXEC 4B00h/4B03h. DOS_APPEND_DRIVE/PATH controls qualified names.
     * append returns previous and applies set to system flags/list (INT 2Fh
     * B706h); the first change sets DOS_APPEND_INSTALLED. append_task masks
     * flags for the caller and EXEC children; file utilities clear DOS_APPEND_X
     * as DOS 4 B707h does. */
    int (*append)(const DosAppend *set,DosAppend *previous);
    int (*append_task)(u32 mask,u32 *previous);
    /* DOS 4 programs whose work MSDOS.SYS (sharing, directory caching, code
     * page switching) or VDM (GRAFTABL's table) does itself: the value each
     * was installed with since boot, 0 before (previous), and set to change
     * it, for their "already installed" and INT 2Fh's checks. ANSI.SYS's
     * (DOS_INSTALLED_ANSI) MSDOS.SYS does too: only a DEVICE= image, while
     * it loads, sets it first, turning on the escape sequences of CON
     * output and key reassignment once it has loaded (DE_ACCESS otherwise,
     * DE_FUNCTION without IO.SYS's text screen); later only DOS_ANSI_X
     * changes. HIMEM.SYS's and EMM386.SYS's (DOS_INSTALLED_XMS and EMS),
     * which VDM provides, are set only by a DEVICE= image as it loads. */
    int (*installed)(u32 program,const u32 *set,u32 *previous);
    /* PRINT (DOS_PRINT_*): DE_FUNCTION before INSTALL but for it and
     * QUERY; SUBMIT gives DE_NOMEM with the queue full, CANCEL DE_NOFILE
     * when no entry matches. MSDOS.SYS prints in time it takes from DOS
     * calls and waits for keys; idle gives it such time from programs that
     * wait otherwise (VDM's INT 16h and 28h, a message loop) and tells in
     * how many ms it wants more, 0 when it has nothing to print. */
    int (*print)(u32 function,DosPrintRequest *request);
    u32 (*idle)(void);
    /* KEYB (DOS_KEYB_*), above. */
    int (*keyb)(u32 function,DosKeybRequest *request);
    u32 (*keyb_key)(const IoEvent *event,u16 keys[DOS_KEYB_KEYS]);
    /* The full name of the running program's image as EXEC found it
     * (DOS 3's name after the environment), VDM.EXE's for 8086 programs. */
    int (*program_path)(char path[DOS_PATH_MAX]);
    /* MEM (DOS_ARENA_*), above. */
    int (*arena)(u32 index,DosArenaBlock *block);
} DosApi;
#define DOS_SWITCH_ALT_TAB 1u
#define DOS_SWITCH_ALT_ESC 2u
#define DOS_SWITCH_CTRL_ESC 3u
#define DOS_SWITCH_QUERY 0x100u
#define DOS_API_V1_SIZE offsetof(DosApi,query)
#endif
