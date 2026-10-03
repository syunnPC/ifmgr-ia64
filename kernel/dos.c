/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * DOS 4 handle/process/API model re-expressed for the IA-64 native ABI.
 * See MS-DOS 4.0 DOS/HANDLE.ASM, DUP.ASM, PATH.ASM, EXEC.ASM, DISPATCH.ASM.
 * The build-BPB table follows MS-DOS 4 BIOS/MSINIT.ASM DiskTable2 (Copyright
 * (c) Microsoft Corporation, MIT License; see vendor/msdos4/LICENSE).
 */
#include "dos.h"
#include "console.h"
#include "fat_io.h"
#include "device.h"
#include "block.h"
#include "clock.h"
#include "calendar.h"
#include "fcb.h"
#include "nls.h"
#include "print.h"
#include "keyb.h"
Fat dos_volume;
Arena dos_arena;
/* One metadata record per open file, one position per open description.
 * Separate opens see each other's writes; DUP and EXEC also share a position. */
typedef struct {
    unsigned refs,drive; u32 generation; Fat *volume; Node node; char path[DOS_PATH_MAX];
    int remote; u64 remote_file; u32 remote_size; u16 remote_date,remote_time; /* redirected drives */
} OpenFile;
typedef struct {
    unsigned refs,mode; int device; OpenFile *file; u32 pos;
    void *device_cookie;
    u8 device_flags,line[258]; u16 line_pos,line_used;
} Sft;
typedef struct {Sft *owner; u32 pid; u64 start,end;} FileLock;
typedef struct {
    u32 pid,parent;
    unsigned errorlevel,allocation_method;
    DosExtendedError error;
    int running,dos16,switch_hooked; /* dos16: VDM.EXE running an 8086 program; switch_hooked: it put in a switch hook */
    DosSwitchHook switch_hook; /* inherited by EXEC's children */
    u32 append_mask; /* DosApi append_task, inherited too */
    int break_check,break_pending;
    u32 exit_kind,return_kind;
    int resident; u64 resident_token; /* AH=31h: memory and image stay. */
    DosBreakHandler break_handler;
    DosCriticalHandler critical_handler;
    int terminal_error,critical_failed,critical_pending;
    unsigned operation_drive,operation_generation;
    int operation_device;
    unsigned handle_count,env_used;
    int handles[DOS_MAX_HANDLES];
    char environment[DOS_ENV_CAPACITY];
    unsigned drive;
    char tail[256],cwd[DOS_DRIVES][DOS_PATH_MAX];
    char program[DOS_PATH_MAX]; /* the image EXEC ran, as programs name it */
    void *image; /* the arena block EXEC read it into, while it runs */
    union {u64 alignment; u8 bytes[128];} default_dta;
    void *dta; u32 dta_size;
} Process;
static Sft sft[DOS_MAX_FILES];
static OpenFile open_files[DOS_MAX_FILES];
static FileLock locks[DOS_MAX_LOCKS];
static Process *process; /* DOS_PROCESSES of them, after the arena */
_Static_assert(sizeof(Process)*DOS_PROCESSES<=DOS_PROCESS_BYTES,"the process table fits");
typedef struct {
    Fat *volume; IoDiskInfo media; u32 unit,generation,driver; int mounted;
    u32 lock_pid; int disabled,params_set; DosDeviceParams params;
    const DosRedirector *redir; u32 redir_pid,redir_seen; /* include/dos_redir.h */
} Drive;
static Drive drives[DOS_DRIVES];
static Fat other_volumes[DOS_DRIVES];
static const IoServices *disk_services;
static unsigned current_slot,in_dos;
static unsigned file_limit=DOS_DEFAULT_FILES;
static u32 next_pid=2;
static unsigned console_column;
static unsigned critical_suppress;
static int spooling; /* PRINT's turn (dos_spool_begin) */
static unsigned driver_callback,driver_count;
static u64 driver_modules[DOS_MAX_DRIVERS];
#define DOS_MAX_RESIDENT 16
typedef struct {u64 token; u32 owner,pid;} LoadedImage; /* AH=4B01h, owner = loading task */
static LoadedImage loaded_images[4];
static u64 resident_modules[DOS_MAX_RESIDENT];
static unsigned resident_count;
static Process *current(void) {return &process[current_slot];}
static int drive_exists(unsigned drive) {return drive<DOS_DRIVES && (drives[drive].volume || drives[drive].redir);}
/* SUBST, JOIN and ASSIGN (DosApi drive_map, assign), for the whole system
 * as DOS 4's current directory structure and ASSIGN's table keep them: a
 * SUBST letter stands for a directory of another drive; a JOINed drive
 * shows as a directory of another drive and is no letter itself; ASSIGN
 * sends a letter to another. Each task's current directories stay in the
 * letters' own terms (AH=47h); paths are translated before they reach a
 * volume, and drive numbers to the drive they mean. */
typedef struct {u8 kind,drive; char root[DOS_PATH_MAX];} DriveMap;
static DriveMap drive_maps[DOS_DRIVES];
static u8 assign_table[DOS_DRIVES]; static int assign_active;
static unsigned raw_letters; /* the kernel's own names: not ASSIGNed */
static DosAppend appended; /* APPEND's flags and list (dos_append) */
static u32 installed_values[DOS_INSTALLED_PROGRAMS]; /* dos_installed */
static u32 ansi_pending; /* DOS_INSTALLED_ANSI set by the DEVICE= image loading */
static unsigned assigned(unsigned letter) {return letter<DOS_DRIVES && assign_active && !raw_letters?assign_table[letter]:letter;}
/* A letter as the drive it means; DE_DRIVE for a JOINed one or none. */
static int physical_drive(unsigned letter,int assign,unsigned *drive) {
    if(assign) letter=assigned(letter);
    if(letter>=DOS_DRIVES || drive_maps[letter].kind==DOS_MAP_JOIN) return DE_DRIVE;
    *drive=drive_maps[letter].kind==DOS_MAP_SUBST?drive_maps[letter].drive:letter;
    return drive_exists(*drive)?0:DE_DRIVE;
}
static int letter_usable(unsigned letter) {unsigned drive; return !physical_drive(letter,1,&drive);}
/* A letter's path as its volume's: a SUBST's directory in front and its
 * drive, then, under a JOIN's directory, the JOINed drive. */
static int translate(unsigned *drive,char path[DOS_PATH_MAX]) {
    const DriveMap *m=&drive_maps[*drive];
    if(m->kind==DOS_MAP_JOIN) return DE_DRIVE;
    if(m->kind==DOS_MAP_SUBST) {
        char full[DOS_PATH_MAX]; int e=strcopy(full,sizeof(full),m->root);
        if(!e && strcmp(path,"\\")) e=strappend(full,sizeof(full),path);
        if(e) return DE_PATH;
        strcopy(path,DOS_PATH_MAX,full); *drive=m->drive;
    }
    for(unsigned j=0;j<DOS_DRIVES;j++) {
        const DriveMap *join=&drive_maps[j]; size_t n=strlen(join->root);
        if(join->kind!=DOS_MAP_JOIN || join->drive!=*drive || memcmp(path,join->root,n) || (path[n] && path[n]!='\\')) continue;
        char rest[DOS_PATH_MAX]; strcopy(rest,sizeof(rest),path[n]?path+n:"\\");
        strcopy(path,DOS_PATH_MAX,rest); *drive=j; break;
    }
    return 0;
}
static void drop_redirection(unsigned);
#define dos_errorlevel (current()->errorlevel)
#define last_error (current()->error.error)
u32 dos_pid(void) {return current()->pid;}
/* A DEVICE= image's parameters while it loads, as its command tail. */
static const char *loading_tail;
const char *dos_command_tail(void) {return loading_tail?loading_tail:current()->tail;}
unsigned dos_current_drive(void) {return current()->drive;}
static DosExtendedError classify_error(int error) {
    DosExtendedError result={.error=error,.error_class=DOS_CLASS_OTHER,.action=DOS_ACTION_ABORT,
        .locus=DOS_LOCUS_UNKNOWN,.drive=UINT32_MAX,.operation=UINT32_MAX,.sector=UINT64_MAX};
    switch(error) {
    case DE_NOMEM: case DE_HANDLES: case DE_ENV: case DE_LOCKS:
        result.error_class=DOS_CLASS_RESOURCE; result.action=DOS_ACTION_USER;
        result.locus=DOS_LOCUS_MEMORY; break;
    case DE_ARENA: case DE_BLOCK:
        result.error_class=DOS_CLASS_INTERNAL; result.locus=DOS_LOCUS_MEMORY; break;
    case DE_NOFILE: case DE_PATH: case DE_DRIVE: case DE_NOMORE:
        result.error_class=DOS_CLASS_NOT_FOUND; result.action=DOS_ACTION_USER; result.locus=DOS_LOCUS_DISK; break;
    case DE_ACCESS: case DE_CURRENT:
        result.error_class=DOS_CLASS_PERMISSION; result.action=DOS_ACTION_USER; result.locus=DOS_LOCUS_DISK; break;
    case DE_FORMAT: case DE_DATA: result.error_class=DOS_CLASS_FORMAT; result.action=DOS_ACTION_USER; break;
    case DE_IO: case DE_SEEK:
        result.error_class=DOS_CLASS_HARDWARE; result.action=DOS_ACTION_RETRY; result.locus=DOS_LOCUS_DISK; break;
    case DE_NOTREADY: case DE_CHANGED: case DE_READONLY:
        result.error_class=DOS_CLASS_MEDIA; result.action=DOS_ACTION_INTERVENE; result.locus=DOS_LOCUS_DISK; break;
    case DE_FULL:
        result.error_class=DOS_CLASS_RESOURCE; result.action=DOS_ACTION_USER; result.locus=DOS_LOCUS_DISK; break;
    case DE_SHARE: case DE_LOCK:
        result.error_class=DOS_CLASS_LOCKED; result.action=DOS_ACTION_DELAY; result.locus=DOS_LOCUS_DISK; break;
    case DE_EXISTS:
        result.error_class=DOS_CLASS_EXISTS; result.action=DOS_ACTION_USER; result.locus=DOS_LOCUS_DISK; break;
    case DE_BUSY: result.error_class=DOS_CLASS_TEMPORARY; result.action=DOS_ACTION_DELAY; break;
    case DE_FUNCTION: case DE_MODE: case DE_HANDLE: case DE_NOTSAME: case DE_BREAK:
        result.error_class=DOS_CLASS_APPLICATION; result.action=DOS_ACTION_USER; break;
    case DE_CRITICAL: result.error_class=DOS_CLASS_SYSTEM; break;
    case DE_EOF: result.error_class=DOS_CLASS_NOT_FOUND; result.action=DOS_ACTION_IGNORE; break;
    }
    return result;
}
static void api_begin(void) {
    if(!in_dos) print_tick(); /* PRINT's turn, as the timer would give it */
    current()->terminal_error=current()->critical_failed=0;
    current()->operation_drive=UINT32_MAX; current()->operation_generation=0;
    current()->operation_device=0;
}
static void error_context(int error) {
    Process *p=current(); p->error=classify_error(error);
    if(p->operation_device && p->error.locus==DOS_LOCUS_DISK) p->error.locus=DOS_LOCUS_DEVICE;
    if(p->error.locus==DOS_LOCUS_DISK) {
        p->error.drive=p->operation_drive; p->error.generation=p->operation_generation;
    }
}
int dos_preserve_error(int error) {
    Process *p=current();
    /* Once an operation fails, later cleanup may change the active device or
     * fail again. Keep the original cause and suppress secondary callbacks. */
    if(error && !p->terminal_error) {error_context(error); p->terminal_error=error;}
    return error;
}
static int api_result(int error) {
    Process *p=current();
    if(error) {
        if(p->terminal_error==error) {
            if(p->error.drive<DOS_DRIVES && (p->error.flags&DOS_ERROR_COMMIT) &&
               drives[p->error.drive].volume && drives[p->error.drive].volume->faulted)
                p->error.flags|=DOS_ERROR_UNRECOVERED;
            if(p->critical_failed) error=DE_CRITICAL;
        } else error_context(error);
    }
    if(p->critical_pending || p->break_pending) {
        p->exit_kind=p->critical_pending?DOS_EXIT_CRITICAL:DOS_EXIT_BREAK;
        p->critical_pending=p->break_pending=0;
        platform_exit(0);
    }
    return error;
}
int dos_critical_handler(const DosCriticalHandler *handler,DosCriticalHandler *previous) {
    if(in_dos) return DE_BUSY;
    DosCriticalHandler next=handler?*handler:current()->critical_handler;
    if(previous) *previous=current()->critical_handler;
    current()->critical_handler=next; return 0;
}
int dos_extended_error(DosExtendedError *info) {
    if(in_dos) return DE_BUSY;
    if(!info) return DE_FUNCTION;
    *info=current()->error; return 0;
}
static int read_datetime(DosDateTime *out) {
    IoDateTime t; current()->operation_device=1;
    int e=dos_clock_read(&t); if(e) return e;
    *out=(DosDateTime){t.year,t.month,t.day,t.hour,t.minute,t.second,t.nanosecond/10000000,
        calendar_weekday(t.year,t.month,t.day)}; return 0;
}
int dos_get_datetime(DosDateTime *out) {
    if(in_dos) return DE_BUSY;
    if(!out) return DE_FUNCTION;
    api_begin(); in_dos++; int e=read_datetime(out); in_dos--; return api_result(e);
}
int dos_config_country(u16 country,u16 page,const char *path) {
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; int e=nls_load(country,page,path); in_dos--; return api_result(e);
}
int dos_driver_request(const DosDeviceSpec *spec,DosDeviceRequest *request) {
    current()->operation_device=1;
    in_dos++; driver_callback++;
    int e=spec->request(spec->context,request);
    driver_callback--; in_dos--; return e<0?DE_IO:e;
}
int dos_block_request(const DosBlockSpec *spec,DosBlockRequest *request) {
    current()->operation_device=0;
    if(request->command==DOS_BLOCK_INIT || request->command==DOS_BLOCK_FINISH) {
        current()->operation_drive=UINT32_MAX; current()->operation_generation=0;
    }
    in_dos++; driver_callback++;
    int e=spec->request(spec->context,request);
    driver_callback--; in_dos--; return e<0?DE_IO:e;
}
int dos_block_register(const DosBlockSpec *spec) {
    if(driver_callback || in_dos>1) return DE_BUSY;
    if(in_dos!=1) return DE_ACCESS;
    return device_register_block(spec);
}
int dos_block_info(u32 index,DosBlockInfo *info) {
    if(in_dos) return DE_BUSY;
    return block_info(index,info);
}
int dos_device_register(const DosDeviceSpec *spec) {
    if(driver_callback || in_dos>1) return DE_BUSY;
    if(in_dos!=1) return DE_ACCESS;
    current()->operation_device=1;
    return device_register(spec);
}
int dos_device_info(u32 index,DosDeviceInfo *info) {
    if(in_dos) return DE_BUSY;
    return device_info(index,info);
}
static unsigned critical_error(void *context,const FatIoError *io) {
    Drive *d=context; unsigned drive=(unsigned)(d-drives); Process *p=current(); Fat *f=d->volume;
    /* Preserve the first terminal error during cleanup; recovery writes never
     * enter this hook. A callback may not reenter DOS or change its task. */
    if(p->terminal_error || p->critical_pending) return DOS_CRITICAL_FAIL;
    DosCriticalError event={.size=sizeof(event),.error=io->error,.operation=io->operation,
        .drive=drive,.generation=d->generation,.attempt=io->attempt,.pid=p->pid,.flags=io->flags,
        .sector=io->sector==UINT32_MAX?UINT64_MAX:io->sector,
        .allowed=(1U<<DOS_CRITICAL_ABORT)|(1U<<DOS_CRITICAL_FAIL)};
    if(io->error!=DE_CHANGED && io->attempt!=UINT32_MAX && !(io->operation==DOS_CRITICAL_WRITE && f && f->disk.readonly))
        event.allowed|=1U<<DOS_CRITICAL_RETRY;
    if(io->operation==DOS_CRITICAL_READ && (io->flags&DOS_ERROR_DATA) && (io->error==DE_IO || io->error==DE_SEEK))
        event.allowed|=1U<<DOS_CRITICAL_IGNORE;
    event.area=io->sector==UINT32_MAX || !f?4:io->sector<f->fat_start?0:io->sector<f->root_start?1:io->sector<f->data_start?2:3;
    DosExtendedError saved=classify_error(io->error);
    saved.drive=drive; saved.generation=d->generation; saved.operation=io->operation;
    saved.sector=event.sector; saved.flags=io->flags;
    p->error=saved;
    unsigned action=DOS_CRITICAL_FAIL; int called=0;
    if(!critical_suppress && p->critical_handler.handler) {
        called=1; in_dos++;
        action=p->critical_handler.handler(p->critical_handler.context,&event);
        in_dos--;
    }
    p->error=saved;
    if(action>3 || !(event.allowed&(1U<<action))) action=DOS_CRITICAL_FAIL;
    if(action==DOS_CRITICAL_IGNORE) p->error.flags|=DOS_ERROR_IGNORED;
    else if(action!=DOS_CRITICAL_RETRY) {
        p->terminal_error=io->error; p->critical_failed=called;
        if(action==DOS_CRITICAL_ABORT && p->running) p->critical_pending=1;
    }
    return action;
}
void dos_set_break_check(int enabled) {current()->break_check=!!enabled;}
int dos_break_handler(const DosBreakHandler *handler,DosBreakHandler *previous) {
    if(in_dos) return DE_BUSY;
    DosBreakHandler next=handler?*handler:current()->break_handler;
    if(previous) *previous=current()->break_handler;
    current()->break_handler=next; return 0;
}
int dos_last_exit(DosExitInfo *info) {
    if(in_dos) return DE_BUSY;
    if(!info) return DE_FUNCTION;
    *info=(DosExitInfo){dos_errorlevel,current()->return_kind}; return 0;
}
/* A character device's input starts afresh on every handle: no line kept,
 * no end of file. */
static void reset_line_input(int device) {
    for(unsigned i=0;i<ARRAY_SIZE(sft);i++) if(sft[i].refs && sft[i].device==device) {
        sft[i].line_pos=sft[i].line_used=0; sft[i].device_flags|=64;
    }
}
static int signal_break(void) {
    int action=DOS_BREAK_ABORT;
    if(current()->break_handler.handler) {
        in_dos++;
        action=current()->break_handler.handler(current()->break_handler.context);
        in_dos--;
    }
    if(action==DOS_BREAK_CONTINUE) return 0;
    console_flush(); reset_line_input(1);
    con_write("^C\r\n",4); console_column=0;
    if(action!=DOS_BREAK_CANCEL && current()->running) current()->break_pending=1;
    return DE_BREAK;
}
static int check_break(void) {
    u8 c; int e=console_byte(0,1,&c);
    if(e==DE_NOTREADY || e==DE_EOF) return 0;
    if(e) return e;
    if(c!=3) return 0;
    e=console_byte(0,0,&c); return e?e:signal_break();
}
/* Forget a mounted volume: new generation, remount on next access, and
 * restart every task's directory on it. Old handles/searches see DE_CHANGED. */
static int invalidate_drive(unsigned drive) {
    Drive *d=&drives[drive];
    if(d->generation==UINT32_MAX) return DE_IO;
    d->generation++; d->mounted=0;
    if(d->volume) {d->volume->cache_valid=0; d->volume->faulted=1;}
    for(unsigned letter=0;letter<DOS_DRIVES;letter++)
        if(letter==drive || (drive_maps[letter].kind==DOS_MAP_SUBST && drive_maps[letter].drive==drive))
            for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) strcopy(process[i].cwd[letter],DOS_PATH_MAX,"\\");
    for(unsigned i=0;i<ARRAY_SIZE(locks);i++)
        if(locks[i].owner && locks[i].owner->file->drive==drive) locks[i].owner=NULL;
    return 0;
}
static int release_volume(unsigned);
static void maintenance_reset(void);
static int poll_drive_once(unsigned drive) {
    if(!drive_exists(drive) || drives[drive].disabled) return DE_DRIVE;
    Drive *d=&drives[drive];
    if(d->redir) {
        u32 g=d->redir->generation(d->redir->context);
        if(!g) return DE_NOTREADY;
        if(g!=d->redir_seen) {d->redir_seen=g; return invalidate_drive(drive);}
        return 0;
    }
    if(!d->driver && !disk_services) return 0;
    IoDiskInfo info;
    int e=d->driver?block_media(d->unit,&info):disk_services->disk_info(disk_services->context,d->unit,&info);
    if(e) return e;
    if(!info.generation) return DE_IO;
    if(info.generation!=d->media.generation || info.flags!=d->media.flags ||
       info.disk.sectors!=d->media.disk.sectors || info.disk.readonly!=d->media.disk.readonly ||
       info.disk.ctx!=d->media.disk.ctx) {
        if(d->volume->tx_depth) return DE_BUSY;
        e=invalidate_drive(drive); if(e) return e;
        d->media=info; d->params_set=0;
    }
    return 0;
}
static int poll_drive(unsigned drive) {
    for(u32 attempt=1;;attempt++) {
        int e=poll_drive_once(drive);
        if(e!=DE_IO && e!=DE_NOTREADY && e!=DE_CHANGED) return e;
        FatIoError error={DOS_CRITICAL_MEDIA,UINT32_MAX,attempt,0,e};
        if(critical_error(&drives[drive],&error)!=DOS_CRITICAL_RETRY) return e;
    }
}
static int ready_drive(unsigned drive,Fat **out) {
    int e=poll_drive(drive); if(e) return e;
    Drive *d=&drives[drive];
    current()->operation_drive=drive; current()->operation_generation=d->generation;
    current()->operation_device=0;
    if(d->redir) return DE_REMOTE;
    for(u32 attempt=1;!(d->media.flags&IO_DISK_PRESENT);attempt++) {
        FatIoError error={DOS_CRITICAL_MEDIA,UINT32_MAX,attempt,0,DE_NOTREADY};
        if(critical_error(d,&error)!=DOS_CRITICAL_RETRY) return DE_NOTREADY;
        e=poll_drive(drive); if(e) return e;
    }
    if(d->lock_pid) return DE_ACCESS;
    if(!d->mounted) {
        e=fat_mount_ex(d->volume,&d->media.disk,critical_error,d); if(e) return e;
        d->mounted=1;
    }
    if(d->volume->faulted) return DE_IO;
    *out=d->volume; return 0;
}
static int ready_file(const OpenFile *file) {
    current()->operation_drive=file->drive; current()->operation_generation=file->generation;
    current()->operation_device=0;
    int e=poll_drive(file->drive); if(e) return e;
    if(file->generation!=drives[file->drive].generation) {
        FatIoError error={DOS_CRITICAL_MEDIA,UINT32_MAX,1,0,DE_CHANGED};
        critical_error(&drives[file->drive],&error); return DE_CHANGED;
    }
    return !file->remote && file->volume->faulted?DE_IO:0;
}
/* The current drive is the letter chosen; ASSIGN applies when it is used. */
int dos_select_drive(unsigned drive) {
    unsigned target; int e=physical_drive(drive,1,&target);
    if(e || drives[target].disabled) return DE_DRIVE;
    current()->drive=drive; return 0;
}
int dos_drive_cwd(unsigned drive,char out[DOS_PATH_MAX]) {
    unsigned target; int e=physical_drive(drive,1,&target); if(e) return e;
    e=poll_drive(target); if(e) return e;
    return strcopy(out,DOS_PATH_MAX,current()->cwd[assigned(drive)]);
}
int dos_init(const Disk *disk,void *memory) {
    if(driver_count) return DE_BUSY;
    int e=fat_mount(&dos_volume,disk); if(e) return e;
    e=nls_reset(); if(e) return e;
    arena_init(&dos_arena,memory,DOS_ARENA_BYTES);
    process=(Process *)((u8 *)memory+DOS_ARENA_BYTES);
    memset(sft,0,sizeof(sft)); memset(process,0,DOS_PROCESSES*sizeof(*process));
    memset(open_files,0,sizeof(open_files)); memset(locks,0,sizeof(locks)); fcb_reset();
    memset(drives,0,sizeof(drives)); memset(other_volumes,0,sizeof(other_volumes)); disk_services=NULL;
    memset(drive_maps,0,sizeof(drive_maps)); assign_active=0; raw_letters=0; memset(&appended,0,sizeof(appended)); memset(installed_values,0,sizeof(installed_values));
    for(unsigned i=0;i<DOS_DRIVES;i++) assign_table[i]=(u8)i;
    drives[2]=(Drive){.volume=&dos_volume,.media={1,IO_DISK_BOOT|IO_DISK_PRESENT,*disk},.generation=1,.mounted=1};
    dos_volume.error_handler=critical_error; dos_volume.error_context=&drives[2];
    current_slot=0; in_dos=0; next_pid=2; process[0].pid=1; file_limit=DOS_DEFAULT_FILES;
    critical_suppress=0; fat_verify_writes=0; print_reset(); spooling=0; api_begin(); maintenance_reset();
    driver_callback=driver_count=0; memset(driver_modules,0,sizeof(driver_modules)); device_reset();
    memset(loaded_images,0,sizeof(loaded_images)); memset(resident_modules,0,sizeof(resident_modules)); resident_count=0;
    console_reset(); console_column=0;
    process[0].handle_count=DOS_HANDLES; process[0].append_mask=~0u;
    process[0].drive=2; process[0].dta=&process[0].default_dta; process[0].dta_size=128;
    for(unsigned i=0;i<DOS_DRIVES;i++) strcopy(process[0].cwd[i],DOS_PATH_MAX,"\\");
    for(unsigned i=0;i<DOS_MAX_HANDLES;i++) process[0].handles[i]=-1;
    for(unsigned i=0;i<5;i++) {
        sft[i].refs=1; sft[i].device=i<3?DOS_CON_DEVICE:i==3?DOS_AUX_DEVICE:DOS_PRN_DEVICE;
        sft[i].device_flags=i<3?0xc3:0xe0;
        sft[i].mode=i==4?1:2; process[0].handles[i]=i; device_reference(sft[i].device,1);
    }
    e=dos_env_set("COMSPEC","C:\\COMMAND.COM"); if(e) return e;
    return dos_env_set("PATH","C:\\");
}
int dos_bind_standard_devices(void) {
    if(in_dos || current_slot) return DE_BUSY;
    for(unsigned i=3;i<5;i++) {
        const char *name=i==3?"AUX":"PRN";
        if(device_find(name)==(i==3?DOS_AUX_DEVICE:DOS_PRN_DEVICE)) continue;
        unsigned h; int e=dos_open(name,i==3?2:1,0,&h);
        if(e==DE_NOTREADY) continue;
        if(e) return e;
        e=dos_dup2(h,i); int closed=dos_close(h);
        if(e || closed) return e?e:closed;
    }
    return 0;
}
int dos_attach_disks(const IoServices *io) {
    if(in_dos || current_slot || driver_count) return DE_BUSY;
    if(!io || io->size<offsetof(IoServices,disk_info)+sizeof(io->disk_info) || !io->disk_count || !io->disk_info) return 0;
    for(unsigned i=1;i<DOS_PROCESSES;i++) if(process[i].pid) return DE_BUSY;
    for(unsigned i=5;i<ARRAY_SIZE(sft);i++) if(sft[i].refs) return DE_BUSY;
    unsigned count=io->disk_count(io->context); int have_boot=0;
    for(unsigned unit=0;unit<count;unit++) {
        IoDiskInfo info; int e=io->disk_info(io->context,unit,&info); if(e) return e;
        if(!info.generation || !info.disk.read || !info.disk.write) return DE_FORMAT;
        unsigned drive;
        if(info.flags&IO_DISK_BOOT) {if(have_boot++) return DE_FORMAT; drive=2;}
        else {
            drive=(info.flags&IO_DISK_REMOVABLE)?0:3;
            while(drive_exists(drive) || (drive<DOS_DRIVES && drive_maps[drive].kind)) drive++;
            if(drive==DOS_DRIVES) {print("No drive letter for disk unit %u\n",(unsigned long long)unit); continue;}
        }
        Drive *d=&drives[drive];
        *d=(Drive){.volume=drive==2?&dos_volume:&other_volumes[drive],.media=info,.unit=unit,.generation=1};
        if(info.flags&IO_DISK_PRESENT) {
            e=fat_mount_ex(d->volume,&info.disk,critical_error,d);
            if(e && drive==2) return e;
            if(e) print("%c: cannot mount media (%u)\n",'A'+drive,(unsigned long long)e);
            else d->mounted=1;
        }
    }
    if(!have_boot) return DE_DRIVE;
    disk_services=io; return 0;
}
int dos_attach_block_drives(unsigned owner) {
    u32 ids[DOS_DRIVES]; unsigned count=block_units(owner,ids);
    IoDiskInfo media[DOS_DRIVES]; unsigned letters[DOS_DRIVES]; u32 assigned=0;
    for(unsigned i=0;i<count;i++) {
        int e=block_media(ids[i],&media[i]); if(e) return e;
        unsigned letter=media[i].flags&IO_DISK_REMOVABLE?0:3;
        while(letter<DOS_DRIVES && (letter==2 || drives[letter].volume || (assigned&(1U<<letter)))) letter++;
        if(letter==DOS_DRIVES) return DE_DRIVE;
        if(drives[letter].generation==UINT32_MAX) return DE_IO;
        letters[i]=letter; assigned|=1U<<letter;
    }
    /* Publication cannot fail after this point. FAT mounts lazily, allowing
     * absent or unformatted media to retain its assigned drive letter. */
    for(unsigned i=0;i<count;i++) {
        unsigned letter=letters[i]; Drive *d=&drives[letter]; u32 generation=d->generation+1;
        memset(&other_volumes[letter],0,sizeof(other_volumes[letter]));
        *d=(Drive){.volume=&other_volumes[letter],.media=media[i],.unit=ids[i],.generation=generation,.driver=owner};
        d->volume->error_handler=critical_error; d->volume->error_context=d;
        block_drive(ids[i],letter);
    }
    return 0;
}
int dos_detach_block_drives(unsigned owner) {
    if(!owner) return DE_ACCESS;
    for(unsigned drive=0;drive<DOS_DRIVES;drive++) if(drives[drive].driver==owner) {
        if(drives[drive].volume->tx_depth || drives[drive].volume->tx_first) return DE_BUSY;
        for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) if(open_files[i].refs && open_files[i].drive==drive) return DE_BUSY;
    }
    for(unsigned drive=0;drive<DOS_DRIVES;drive++) if(drives[drive].driver==owner) {
        Drive *d=&drives[drive]; block_drive(d->unit,UINT32_MAX);
        u32 generation=d->generation;
        memset(d->volume,0,sizeof(*d->volume)); *d=(Drive){.generation=generation};
        for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) {
            strcopy(process[i].cwd[drive],DOS_PATH_MAX,"\\");
            if(process[i].drive==drive) process[i].drive=2;
        }
    }
    return 0;
}
static int env_name(const char *name,size_t *length) {
    if(!name || !*name) return DE_ENV;
    size_t n=0;
    while(name[n]) {
        if(n==DOS_ENV_NAME_MAX || (u8)name[n]<=32 || (u8)name[n]>126 || name[n]=='=') return DE_ENV;
        n++;
    }
    *length=n; return 0;
}
static unsigned env_offset(const char *name,size_t length) {
    Process *p=current(); unsigned pos=0;
    while(pos<p->env_used) {
        const char *entry=p->environment+pos; size_t i=0;
        while(i<length && entry[i] && entry[i]!='=' && entry[i]==upper(name[i])) i++;
        if(i==length && entry[i]=='=') return pos;
        pos+=strlen(entry)+1;
    }
    return pos;
}
static int env_copy(const char *value,char *out,u32 cap) {
    if(!out || !cap) return DE_ENV;
    size_t n=strlen(value); if(n>=cap) return DE_ENV;
    memcpy(out,value,n+1); return 0;
}
int dos_env_get(const char *name,char *out,u32 cap) {
    if(in_dos) return DE_BUSY;
    size_t length; int e=env_name(name,&length); if(e) return e;
    unsigned pos=env_offset(name,length);
    if(pos==current()->env_used) return DE_NOFILE;
    return env_copy(current()->environment+pos+length+1,out,cap);
}
int dos_env_set(const char *name,const char *value) {
    if(in_dos) return DE_BUSY;
    size_t length; int e=env_name(name,&length); if(e) return e;
    size_t vlen=value?strlen(value):0;
    if(vlen>=DOS_ENV_CAPACITY) return DE_ENV;
    Process *p=current(); unsigned pos=env_offset(name,length);
    unsigned old=pos<p->env_used?strlen(p->environment+pos)+1:0;
    unsigned added=vlen?length+vlen+2:0;
    if(p->env_used-old+added>=DOS_ENV_CAPACITY) return DE_ENV;
    in_dos++;
    memmove(p->environment+pos+added,p->environment+pos+old,p->env_used-pos-old+1);
    if(added) {
        for(size_t i=0;i<length;i++) p->environment[pos+i]=upper(name[i]);
        p->environment[pos+length]='=';
        memcpy(p->environment+pos+length+1,value,vlen+1);
    }
    p->env_used=p->env_used-old+added;
    if(!p->env_used) p->environment[1]=0;
    in_dos--; return 0;
}
int dos_env_list(u32 index,char *out,u32 cap) {
    if(in_dos) return DE_BUSY;
    Process *p=current(); unsigned pos=0;
    while(pos<p->env_used && index) {pos+=strlen(p->environment+pos)+1; index--;}
    return pos==p->env_used?DE_NOMORE:env_copy(p->environment+pos,out,cap);
}
int dos_set_files(unsigned count) {
    /* Boot-only setting: do not resize the system table under running tasks. */
    if(in_dos) return DE_BUSY;
    if(current_slot || count<8 || count>DOS_MAX_FILES) return DE_FUNCTION;
    for(unsigned i=1;i<DOS_PROCESSES;i++) if(process[i].pid) return DE_BUSY;
    for(unsigned i=count;i<DOS_MAX_FILES;i++) if(sft[i].refs) return DE_ACCESS;
    file_limit=count; return 0;
}
int dos_set_handle_count(unsigned count) {
    if(count>DOS_MAX_HANDLES) return DE_NOMEM;
    if(count<DOS_HANDLES) count=DOS_HANDLES;
    Process *p=current();
    for(unsigned i=count;i<p->handle_count;i++) if(p->handles[i]>=0) return DE_HANDLES;
    p->handle_count=count; return 0;
}
/* A name in its letter's own terms (ASSIGN applied): the letter and the
 * full path from its root, ".." stopping there. */
static int logical_path(const char *input,char out[DOS_PATH_MAX],unsigned *drive) {
    if(!input || !out) return DE_PATH;
    const char *p=input; char temp[DOS_PATH_MAX]; unsigned target;
    *drive=current()->drive;
    if(p[0] && !nls_lead((u8)p[0]) && p[1]==':') {*drive=(unsigned)(upper(p[0])-'A'); p+=2;}
    *drive=assigned(*drive);
    if(*drive>=DOS_DRIVES || drive_maps[*drive].kind==DOS_MAP_JOIN) return DE_DRIVE;
    /* The drive first: a changed medium restarts the directories on it. */
    target=drive_maps[*drive].kind==DOS_MAP_SUBST?drive_maps[*drive].drive:*drive;
    current()->operation_drive=target;
    int e=poll_drive(target); if(e) return e;
    current()->operation_generation=drives[target].generation;
    e=strcopy(temp,sizeof(temp),(*p=='\\'||*p=='/')?"\\":current()->cwd[*drive]); if(e) return e;
    while(*p) {
        while(*p=='\\'||*p=='/') p++;
        if(!*p) break;
        char part[13]; unsigned n=0;
        while(*p && *p!='\\' && *p!='/') {
            unsigned width=nls_char_size(p); if(!width || n+width>12) return DE_PATH;
            memcpy(part+n,p,width); n+=width; p+=width;
        }
        part[n]=0;
        if(!strcmp(part,".")) continue;
        if(!strcmp(part,"..")) {
            char *last=nls_last_sep(temp,"\\");
            if(last) {if(last==temp) last++; *last=0;}
            continue;
        }
        u8 key[11]; e=fat_name83(part,key); if(e) return e;
        /* Canonicalize 8.3 aliases (e.g. FOO. and FOO) before cwd comparisons. */
        Node component; memset(&component,0,sizeof(component)); memcpy(component.raw,key,11);
        fat_name(&component,part);
        if(strlen(temp)>1) {e=strappend(temp,sizeof(temp),"\\"); if(e) return e;}
        e=strappend(temp,sizeof(temp),part); if(e) return e;
    }
    return strcopy(out,DOS_PATH_MAX,temp);
}
/* A name as the drive and path of its volume. */
static int resolve_path(const char *input,char out[DOS_PATH_MAX],unsigned *drive) {
    int e=logical_path(input,out,drive); if(e) return e;
    unsigned letter=*drive,target=drive_maps[letter].kind==DOS_MAP_SUBST?drive_maps[letter].drive:letter;
    e=translate(drive,out); if(e) return e;
    if(*drive!=target) {
        current()->operation_drive=*drive;
        e=poll_drive(*drive); if(e) return e;
        current()->operation_generation=drives[*drive].generation;
    }
    return 0;
}
int dos_path(const char *input,char out[DOS_PATH_MAX]) {unsigned drive; return resolve_path(input,out,&drive);}
static int volume_path(const char *input,char out[DOS_PATH_MAX],unsigned *drive,Fat **volume) {
    int e=resolve_path(input,out,drive); if(e) return e;
    u32 generation=drives[*drive].generation;
    e=ready_drive(*drive,volume); if(e) return e;
    return generation==drives[*drive].generation?0:DE_CHANGED;
}
static int same_node(const Node *a,const Node *b) {return a->sector==b->sector && a->offset==b->offset;}
int dos_canonical(const char *input,char out[DOS_PATH_MAX]) {
    char path[DOS_PATH_MAX]; unsigned drive; int e=resolve_path(input,path,&drive); if(e) return e;
    if(strlen(path)+2>=DOS_PATH_MAX) return DE_PATH;
    out[0]='A'+drive; out[1]=':'; return strcopy(out+2,DOS_PATH_MAX-2,path);
}
int dos_full_path(const char *input,char out[DOS_PATH_MAX]) {
    char path[DOS_PATH_MAX]; unsigned drive; int e;
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++;
    e=logical_path(input,path,&drive);
    if(!e && strlen(path)+2>=DOS_PATH_MAX) e=DE_PATH;
    if(!e) {out[0]=(char)('A'+drive); out[1]=':'; e=strcopy(out+2,DOS_PATH_MAX-2,path);}
    in_dos--; return api_result(e);
}
int dos_program_path(char out[DOS_PATH_MAX]) {
    if(!out) return DE_FUNCTION;
    if(!current()->program[0]) return DE_NOFILE;
    return strcopy(out,DOS_PATH_MAX,current()->program);
}
/* A letter some task has as its current drive, through ASSIGN too. */
static int letter_current(unsigned letter) {
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid && assigned(process[i].drive)==letter) return 1;
    return 0;
}
static void restart_letter(unsigned letter) {
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) strcopy(process[i].cwd[letter],DOS_PATH_MAX,"\\");
}
/* A directory with nothing in it but "." and "..". */
static int directory_empty(Fat *f,const Node *dir) {
    Node n; u32 index=0; int e;
    while(!(e=fat_next(f,fat_cluster(dir),&index,&n))) if(n.raw[0]!='.') return DE_ACCESS;
    return e==DE_NOMORE || e==DE_NOFILE?0:e;
}
/* SUBST: a letter with no drive of its own, for a directory of a local
 * drive; JOIN: a local drive, current nowhere, at an empty directory one
 * level below the root of another; either ended. As SUBST and JOIN check,
 * neither touches a mapped or redirected drive, nor a task's current one. */
static int map_drive(unsigned letter,const DosDriveMap *set) {
    DriveMap *m=&drive_maps[letter]; char path[DOS_PATH_MAX]; unsigned target; Fat *f; Node n; int e;
    if(set->kind==DOS_MAP_NONE) {
        if(!m->kind) return DE_FUNCTION;
        if(letter_current(letter) && m->kind==DOS_MAP_SUBST) return DE_CURRENT;
        if(m->kind==DOS_MAP_JOIN) {
            /* Directories into the JOINed tree start again from the root. */
            size_t n=strlen(m->root);
            for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) {
                char *cwd=process[i].cwd[m->drive];
                if(!memcmp(cwd,m->root,n) && (!cwd[n] || cwd[n]=='\\')) strcopy(cwd,DOS_PATH_MAX,"\\");
            }
        }
        memset(m,0,sizeof(*m)); restart_letter(letter); return 0;
    }
    if(set->kind!=DOS_MAP_SUBST && set->kind!=DOS_MAP_JOIN) return DE_FUNCTION;
    /* DE_ACCESS is kept for a JOIN directory with files in it. */
    if(m->kind) return DE_FUNCTION;
    if(set->kind==DOS_MAP_SUBST && drive_exists(letter)) return DE_FUNCTION;
    if(set->kind==DOS_MAP_JOIN) {
        if(!drive_exists(letter) || drives[letter].disabled) return DE_DRIVE;
        if(drives[letter].redir) return DE_REMOTE;
        for(unsigned j=0;j<DOS_DRIVES;j++) if(drive_maps[j].kind && drive_maps[j].drive==letter) return DE_FUNCTION;
    }
    if(letter_current(letter)) return DE_CURRENT;
    raw_letters++; e=volume_path(set->path,path,&target,&f); raw_letters--;
    if(e) return e==DE_NOFILE?DE_PATH:e;
    if(target==letter || drive_maps[target].kind) return DE_FUNCTION;
    if(drives[target].redir) return DE_REMOTE;
    if(strlen(path)>64) return DE_PATH;
    if(set->kind==DOS_MAP_SUBST) {
        if(strcmp(path,"\\")) {e=fat_lookup(f,path,&n); if(e) return e==DE_NOFILE?DE_PATH:e; if(!(n.raw[11]&FA_DIR)) return DE_PATH;}
    } else {
        if(!strcmp(path,"\\") || strchr(path+1,'\\')) return DE_FUNCTION;
        e=fat_lookup(f,path,&n); if(e) return e==DE_NOFILE?DE_PATH:e;
        if(!(n.raw[11]&FA_DIR)) return DE_PATH;
        e=directory_empty(f,&n); if(e) return e;
    }
    m->kind=(u8)set->kind; m->drive=(u8)target; strcopy(m->root,sizeof(m->root),strcmp(path,"\\")?path:"");
    restart_letter(letter);
    return 0;
}
int dos_drive_map(u32 drive,const DosDriveMap *set,DosDriveMap *previous) {
    if(in_dos) return DE_BUSY;
    if(drive>=DOS_DRIVES || (set && set->size<sizeof(*set)) || (previous && previous->size<sizeof(*previous))) return DE_FUNCTION;
    api_begin(); in_dos++; int e=0;
    if(previous) {
        const DriveMap *m=&drive_maps[drive];
        previous->kind=m->kind; previous->path[0]=0;
        if(m->kind) {previous->path[0]=(char)('A'+m->drive); previous->path[1]=':'; strcopy(previous->path+2,DOS_PATH_MAX-2,m->root[0]?m->root:"\\");}
    }
    if(set) e=map_drive(drive,set);
    in_dos--; return api_result(e);
}
/* ASSIGN's letters; only letters past Z: are refused (ASSIGN checks the
 * drives). */
int dos_assign(const u8 *table,u8 *previous) {
    if(in_dos) return DE_BUSY;
    if(table) for(unsigned i=0;i<DOS_DRIVES;i++) if(table[i]>=DOS_DRIVES) return DE_FUNCTION;
    api_begin(); in_dos++;
    if(previous) for(unsigned i=0;i<DOS_DRIVES;i++) previous[i]=assign_active?assign_table[i]:(u8)i;
    if(table) {
        assign_active=0;
        for(unsigned i=0;i<DOS_DRIVES;i++) {assign_table[i]=table[i]; if(table[i]!=i) assign_active=1;}
    }
    in_dos--; return api_result(0);
}
static int busy(unsigned drive,const Node *n) {
    for(unsigned i=0;i<ARRAY_SIZE(open_files);i++)
        if(open_files[i].refs && open_files[i].drive==drive && open_files[i].generation==drives[drive].generation && same_node(n,&open_files[i].node)) return 1;
    return 0;
}
/* A directory some task is in, through any letter, or a JOIN's. */
static int directory_in_use(unsigned drive,const char *path) {
    size_t n=strlen(path);
    for(unsigned letter=0;letter<DOS_DRIVES;letter++) {
        const DriveMap *m=&drive_maps[letter];
        if(m->kind==DOS_MAP_JOIN && m->drive==drive && !memcmp(m->root,path,n) && (!m->root[n] || m->root[n]=='\\')) return 1;
        for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) {
            char cwd[DOS_PATH_MAX]; unsigned on=letter;
            if(strcopy(cwd,sizeof(cwd),process[i].cwd[letter]) || translate(&on,cwd) || on!=drive) continue;
            if(!strcmp(path,cwd) || (strlen(cwd)>n && !memcmp(path,cwd,n) && cwd[n]=='\\')) return 1;
        }
    }
    return 0;
}
static int slot(unsigned h,Sft **out) {
    if(h>=current()->handle_count || current()->handles[h]<0) return DE_HANDLE;
    *out=&sft[current()->handles[h]]; return 0;
}
static int spare_handle(unsigned *out) {
    for(unsigned i=0;i<current()->handle_count;i++) if(current()->handles[i]<0) {*out=i; return 0;}
    return DE_HANDLES;
}
static int share_conflict(unsigned a,unsigned b,u8 attr,int replace) {
    unsigned sa=(a>>4)&7,sb=(b>>4)&7;
    /* DOS 4 SHARE's GOM treats read-only compatibility opens as deny-write.
     * All native local tasks belong to the same local user, not distinct UIDs. */
    if(attr&FA_RDONLY) {if(!sa) sa=2; if(!sb) sb=2;}
    if(!sa && !sb) return 0;
    if(!sa || !sb) return 1;
    static const unsigned deny[]={0,3,2,1,0},access[]={1,2,3};
    return (deny[sa]&access[b&3]) || (deny[sb]&(access[a&3]|(replace?2U:0U)));
}
static int locked(OpenFile *file,Sft *owner,u64 start,u64 end) {
    if(start==end) return 0;
    for(unsigned i=0;i<ARRAY_SIZE(locks);i++) {
        FileLock *l=&locks[i];
        if(l->owner && l->owner->file==file && start<l->end && l->start<end &&
           (l->owner!=owner || l->pid!=dos_pid())) return DE_LOCK;
    }
    return 0;
}
static int device_transfer(Sft *s,unsigned command,void *buffer,u32 count,unsigned flags,DosDeviceRequest *out) {
    DosDeviceRequest r={.size=sizeof(r),.command=command,.pid=dos_pid(),.mode=s->mode,
        .cookie=s->device_cookie,.buffer=buffer,.count=count,.flags=flags};
    int e=device_request(s->device,&r); if(out) *out=r; return e;
}
static int flush_file(const Sft *s) {
    if(s->device) {
        DosDeviceRequest r={.size=sizeof(r),.command=DOS_DEV_OUTPUT_FLUSH,.pid=dos_pid(),.mode=s->mode,.cookie=s->device_cookie};
        return device_request(s->device,&r);
    }
    if(s->file->remote) return 0; /* read-only: nothing to write back */
    unsigned saved=critical_suppress;
    critical_suppress|=s->mode&DOS_OPEN_FAIL_ERRORS;
    int e=ready_file(s->file);
    if(!e) e=fat_io_flush(s->file->volume,0);
    critical_suppress=saved; return e;
}
static int open_options(unsigned mode,u8 attr,unsigned action) {
    if((mode&~0x60f7U) || (mode&7)>2 || ((mode>>4)&7)>4) return DE_MODE;
    if((action&~0x113U) || !(action&255) || (action&15)>2) return DE_FUNCTION;
    if(attr&~0x27U) return DE_ACCESS;
    return 0;
}
/* Redirected drives are read-only: an existing file opens for reading. */
static int open_remote(unsigned drive,const char *path,unsigned mode,unsigned action,OpenFile **out) {
    const DosRedirector *r=drives[drive].redir; DosRedirEntry entry; u64 handle; OpenFile *file=NULL;
    for(unsigned i=0;i<ARRAY_SIZE(open_files) && !file;i++) if(!open_files[i].refs) file=&open_files[i];
    if(!file) return DE_HANDLES;
    int e=r->open(r->context,path,&handle,&entry);
    if(e==DE_NOFILE && (action&0x10)) return DE_ACCESS;
    if(e) return e;
    if(!(action&15)) e=DE_EXISTS;
    else if((mode&3) || (action&15)==2 || (entry.attributes&(FA_DIR|FA_VOLUME))) e=DE_ACCESS;
    if(e) {r->close(r->context,handle); return e;}
    *file=(OpenFile){.drive=drive,.generation=drives[drive].generation,.remote=1,.remote_file=handle,
        .remote_size=entry.size,.remote_date=entry.date,.remote_time=entry.time};
    file->path[0]='A'+drive; file->path[1]=':'; strcopy(file->path+2,DOS_PATH_MAX-2,path);
    file->refs=1; *out=file; return 0;
}
static u32 file_size(const OpenFile *file) {return file->remote?file->remote_size:fat_size(&file->node);}
static int open_file(const char *name,unsigned mode,u8 attr,unsigned action,unsigned *reference,unsigned *result,int fcb) {
    int e=open_options(mode,attr,action); if(e) return e;
    unsigned s=5; while(s<file_limit && sft[s].refs) s++;
    if(s==file_limit) return DE_HANDLES;
    Sft fresh; memset(&fresh,0,sizeof(fresh)); fresh.mode=mode; fresh.refs=1;
    fresh.device=device_find(name);
    if(!fresh.device) {
        char path[DOS_PATH_MAX]; unsigned drive; Fat *volume;
        e=volume_path(name,path,&drive,&volume);
        if(e==DE_REMOTE) {
            e=open_remote(drive,path,mode,action,&fresh.file); if(e) return e;
            *result=1; goto opened;
        }
        if(e) return e;
        Node node; e=fat_lookup(volume,path,&node);
        OpenFile *file=NULL,*free_file=NULL;
        for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) {
            OpenFile *f=&open_files[i];
            if(!f->refs) {if(!free_file) free_file=f;}
            else if(!e && f->drive==drive && f->generation==drives[drive].generation && same_node(&f->node,&node)) file=f;
        }
        if(!file && !free_file) return DE_HANDLES;
        if(file) node=file->node;
        unsigned disposition=1;
        if(!e) {
            if(!(action&15)) return DE_EXISTS;
            if(node.raw[11]&(FA_DIR|FA_VOLUME)) return DE_ACCESS;
            if(fcb && (node.raw[11]&(FA_HIDDEN|FA_SYSTEM)&~attr)) return DE_NOFILE;
            if(fcb && (node.raw[11]&FA_RDONLY) && (action&15)==1) mode=DOS_OPEN_READ;
            if(((mode&3) || (action&15)==2) && (node.raw[11]&FA_RDONLY)) return DE_ACCESS;
            for(unsigned i=5;i<ARRAY_SIZE(sft);i++)
                if(file && sft[i].refs && sft[i].file==file && share_conflict(mode,sft[i].mode,node.raw[11],(action&15)==2)) return DE_SHARE;
            if((action&15)==2) {
                e=locked(file,NULL,0,1ULL<<32); if(e) return e;
                FatFile f={node,0}; e=fat_replace(volume,&f,attr);
                node=f.node; if(file) file->node=node;
                if(e) return e;
                disposition=3;
            }
        } else if(e==DE_NOFILE && (action&0x10)) {
            e=fat_create(volume,path,attr|FA_ARCHIVE,&node); if(e) return e;
            disposition=2;
        } else return e;
        if(!file) {
            file=free_file; *file=(OpenFile){.drive=drive,.generation=drives[drive].generation,.volume=volume,.node=node};
            file->path[0]='A'+drive; file->path[1]=':'; strcopy(file->path+2,DOS_PATH_MAX-2,path);
        }
        file->refs++; fresh.file=file; *result=disposition;
    }
opened:
    if(fresh.device) {
        const DosDeviceSpec *dev=device_spec(fresh.device);
        /* An FCB, or a create (AH=3Ch, 5Bh: read and write asked for), opens the
         * device for what it can do, as MS-DOS does for PRN. */
        if(fcb || ((action&0x10) && (mode&3)==DOS_OPEN_RDWR))
            mode=(mode&~3U)|((dev->capabilities&DOS_DEVICE_CAN_READ)?
                (dev->capabilities&DOS_DEVICE_CAN_WRITE)?DOS_OPEN_RDWR:DOS_OPEN_READ:DOS_OPEN_WRITE);
        fresh.mode=mode;
        unsigned needed=(mode&3)==0?DOS_DEVICE_CAN_READ:(mode&3)==1?DOS_DEVICE_CAN_WRITE:DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE;
        if((dev->capabilities&needed)!=needed) return DE_ACCESS;
        fresh.device_flags=fresh.device==DOS_CON_DEVICE?0xc3:fresh.device==DOS_NUL_DEVICE?0xc4:0xe0;
        if(dev->attributes&DOS_DEVICE_OPEN_CLOSE) {
            DosDeviceRequest r; e=device_transfer(&fresh,DOS_DEV_OPEN,NULL,0,0,&r); if(e) return e;
            fresh.device_cookie=r.cookie;
        }
        device_reference(fresh.device,1); *result=1;
    }
    if(mode&DOS_OPEN_COMMIT) {
        e=flush_file(&fresh); if(e) {
            dos_preserve_error(e);
            if(fresh.file) fresh.file->refs--;
            if(fresh.device) {
                if(device_spec(fresh.device)->attributes&DOS_DEVICE_OPEN_CLOSE) device_transfer(&fresh,DOS_DEV_CLOSE,NULL,0,0,NULL);
                device_reference(fresh.device,-1);
            }
            return e;
        }
    }
    fresh.mode=mode; sft[s]=fresh; *reference=s; return 0;
}
static unsigned share_retries=3,share_delay=1;
/* AH=440Bh: a sharing or lock conflict is retried after share_delay ms. */
static int share_retry(int error,unsigned *attempt) {
    if((error!=DE_SHARE && error!=DE_LOCK) || *attempt>=share_retries) return 0;
    (*attempt)++;
    const IoServices *io=platform_io_services();
    if(share_delay && io && io->size>=offsetof(IoServices,stall_us)+sizeof(io->stall_us) && io->stall_us)
        io->stall_us(io->context,MIN(share_delay,1000U)*1000);
    return 1;
}
int dos_open_ex(const char *name,unsigned mode,u8 attr,unsigned action,unsigned *handle,unsigned *result) {
    unsigned saved=critical_suppress; critical_suppress|=mode&DOS_OPEN_FAIL_ERRORS;
    unsigned reference,attempt=0; int e=open_options(mode,attr,action);
    if(!e) e=spare_handle(handle);
    if(!e) do e=open_file(name,mode,attr,action,&reference,result,0); while(share_retry(e,&attempt));
    if(!e) current()->handles[*handle]=reference;
    critical_suppress=saved; return e;
}
/* $CreateTempFile (DOS 4 FILE.ASM): append a separator unless the directory
 * already ends in one (a DBCS trail byte is not one), then create-new eight
 * hex digits, retrying on existing names. The caller's buffer has 13 spare
 * bytes; on failure its original text is restored. */
static int create_temp(char *path,unsigned attr,unsigned *handle) {
    if(!path) return DE_PATH;
    if(attr&~0x27U) return DE_ACCESS;
    size_t n=0; int separator=0;
    for(const char *p=path;*p;) {
        unsigned width=nls_char_size(p); if(!width) return DE_PATH;
        separator=width==1 && (*p=='\\' || *p=='/'); p+=width; n+=width;
    }
    size_t base=n+(n && !separator);
    if(base+9>DOS_PATH_MAX) return DE_PATH;
    if(base>n) path[n]='\\';
    static u32 serial;
    IoDateTime t; u32 seed=dos_clock_read(&t)?0:(t.hour<<24)|(t.minute<<16)|(t.second<<8)|(t.nanosecond/10000000);
    for(unsigned attempt=0;attempt<4096;attempt++) {
        u32 value=seed+serial++;
        for(unsigned i=0;i<8;i++) path[base+i]="0123456789ABCDEF"[(value>>(28-4*i))&15];
        path[base+8]=0;
        unsigned result; int e=dos_open_ex(path,DOS_OPEN_RDWR,(u8)attr,0x10,handle,&result);
        if(!e) return 0;
        if(e!=DE_EXISTS && e!=DE_ACCESS) {path[n]=0; return e;}
    }
    path[n]=0; return DE_EXISTS;
}
int dos_open(const char *name,unsigned mode,unsigned action,unsigned *handle) {
    if(action>2) return DE_FUNCTION;
    unsigned result; return dos_open_ex(name,mode,0,action==0?1:action==1?0x12:0x10,handle,&result);
}
int dos_flush(void) {
    int error=0;
    for(unsigned i=0;i<DOS_DRIVES;i++) if(drives[i].volume) {
        current()->operation_drive=i; current()->operation_generation=drives[i].generation; current()->operation_device=0;
        int e=poll_drive(i);
        if(!e && drives[i].mounted) {
            Fat *f=drives[i].volume;
            e=f->faulted?DE_IO:fat_io_flush(f,0);
        }
        if(e && !error) error=dos_preserve_error(e);
    }
    for(unsigned i=0;i<ARRAY_SIZE(sft);i++) if(sft[i].refs && sft[i].device) {
        int e=flush_file(&sft[i]); if(e && !error) error=dos_preserve_error(e);
    }
    return error;
}
/* Shutdown and restart: the disks are flushed first; a failure cancels it. */
static void flush_and(void (*reset)(void),const char *what) {
    if(in_dos) return;
    api_begin(); in_dos++; int e=dos_flush(); in_dos--;
    e=api_result(e);
    if(!e) reset();
    else print("%s cancelled: %s (%u)\n",what,dos_error(e),(unsigned long long)e);
}
void dos_shutdown(void) {flush_and(platform_shutdown,"Shutdown");}
static int release_file(Sft *s,u32 pid) {
    int error=0;
    if(!--s->refs) {
        for(unsigned i=0;i<ARRAY_SIZE(locks);i++) if(locks[i].owner==s) locks[i].owner=NULL;
        if(s->file && !--s->file->refs && s->file->remote) {
            const DosRedirector *r=drives[s->file->drive].redir;
            if(r) r->close(r->context,s->file->remote_file);
        }
        if(s->device) {
            const DosDeviceSpec *dev=device_spec(s->device);
            if(dev && (dev->attributes&DOS_DEVICE_OPEN_CLOSE)) {
                DosDeviceRequest r={.size=sizeof(r),.command=DOS_DEV_CLOSE,.pid=pid,.mode=s->mode,.cookie=s->device_cookie};
                error=device_request(s->device,&r);
            }
            device_reference(s->device,-1);
        }
        memset(s,0,sizeof(*s));
    }
    return error;
}
static int release_handle(Process *p,unsigned h) {
    Sft *s=&sft[p->handles[h]]; p->handles[h]=-1;
    int owns=0;
    for(unsigned i=0;i<p->handle_count;i++) if(p->handles[i]>=0 && &sft[p->handles[i]]==s) {owns=1; break;}
    for(unsigned i=0;i<ARRAY_SIZE(locks);i++)
        if(locks[i].owner==s && !owns && locks[i].pid==p->pid) locks[i].owner=NULL;
    return release_file(s,p->pid);
}
int dos_close(unsigned h) {
    Sft *s; int e=slot(h,&s); if(e) return e;
    e=dos_preserve_error(flush_file(s)); int close=release_handle(current(),h); return e?e:close;
}
static int read_file(Sft *,void *,u32,u32 *);
static int read_char_file(Sft *s,int wait,int peek,int breaks,u8 *out) {
    int e;
    if((s->mode&3)==1) return DE_ACCESS;
    for(;;) {
        if(breaks) {e=check_break(); if(e) return e;}
        if(s->device==2) return DE_EOF;
        if(s->device==1) e=console_byte(wait,peek,out);
        else if(s->device) {
            DosDeviceRequest r;
            e=device_transfer(s,peek?DOS_DEV_PEEK:DOS_DEV_READ,out,1,wait?DOS_DEVICE_WAIT:0,&r);
            if(!e && !peek && !r.transferred) return DE_EOF;
        }
        else {
            u32 n,saved=s->pos;
            e=read_file(s,out,1,&n); if(peek) s->pos=saved;
            if(!e && !n) return DE_EOF;
        }
        if(e || !breaks || *out!=3) return e;
        if(peek) {e=read_char_file(s,wait,0,0,out); if(e) return e;}
        e=signal_break(); if(e) return e;
    }
}
static int read_char(unsigned h,int wait,int peek,int breaks,u8 *out) {
    Sft *s; int e=slot(h,&s); return e?e:read_char_file(s,wait,peek,breaks,out);
}
static int line_read(void *context,u8 *out) {
    return read_char_file(context,1,0,1,out);
}
static int line_write(void *context,const void *data,u32 count) {
    (void)context; u32 done; int e=dos_write(1,data,count,&done);
    return e?e:done==count?0:DE_FULL;
}
static int line_input_file(Sft *s,u8 *buffer) {
    ConsoleLineIo io={s,line_read,line_write,console_column};
    return console_line(buffer,&io);
}
static int line_input(unsigned h,u8 *buffer) {
    Sft *s; int e=slot(h,&s); return e?e:line_input_file(s,buffer);
}
static int output_flow(void) {
    int e=check_break(); if(e) return e;
    u8 c; e=console_byte(0,1,&c);
    if(e==DE_NOTREADY || e==DE_EOF) return 0;
    if(e || c!=19) return e;
    e=console_byte(0,0,&c); if(e) return e;
    do {
        e=console_byte(1,0,&c); if(e) return e;
        if(c==3) {e=signal_break(); if(e) return e;}
    } while(c==19 || c==3);
    return 0;
}
static int device_output(const void *buffer,u32 count,int raw,u32 *done) {
    const u8 *p=buffer;
    for(*done=0;*done<count;(*done)++) {
        u8 c=p[*done];
        if(!raw) {int e=output_flow(); if(e) return e;}
        if(c==9 && !raw) {
            unsigned spaces=8-(console_column&7);
            for(unsigned i=0;i<spaces;i++) console_write(" ",1);
            console_column=(console_column+spaces)&255;
        } else {
            console_write(&c,1);
            if(c=='\r' || c=='\n') console_column=0;
            else if(c=='\b') {if(console_column) console_column--;}
            else if(c>=32) console_column=(console_column+1)&255;
        }
    }
    return 0;
}
static int read_stream(Sft *s,void *buffer,u32 count,u32 *done) {
    *done=0; int e;
    if((s->mode&3)==1) return DE_ACCESS;
    if(!count || s->device==2) return 0;
    if(s->device) {
        if(s->device_flags&32) {
            DosDeviceRequest r; e=device_transfer(s,DOS_DEV_READ,buffer,count,DOS_DEVICE_WAIT,&r);
            *done=r.transferred; return e;
        }
        if(!(s->device_flags&64)) return 0;
        if(s->line_pos==s->line_used) {
            s->line[0]=255; s->line[1]=0; s->line_pos=s->line_used=0;
            e=line_input_file(s,s->line); if(e==DE_EOF) return 0; if(e) return e;
            unsigned length=s->line[1]; s->line[length+3]='\n'; s->line_used=length+2;
            e=line_write(NULL,"\n",1); if(e) return e;
        }
        u8 *p=buffer;
        while(*done<count && s->line_pos<s->line_used) {
            u8 c=s->line[2+s->line_pos++];
            if(c==26) {s->device_flags&=~64; s->line_pos=s->line_used; break;}
            p[(*done)++]=c;
        }
        return 0;
    }
    e=ready_file(s->file); if(e) return e;
    u32 size=file_size(s->file),take=s->pos<size?MIN(count,size-s->pos):0;
    e=locked(s->file,s,s->pos,(u64)s->pos+take); if(e) return e;
    if(s->file->remote) {
        const DosRedirector *r=drives[s->file->drive].redir;
        if(take) e=r->read(r->context,s->file->remote_file,s->pos,buffer,take,done);
        s->pos+=*done; return e;
    }
    FatFile f={s->file->node,s->pos}; e=fat_read(s->file->volume,&f,buffer,count,done);
    s->pos=f.pos; return e;
}
static int read_file(Sft *s,void *buffer,u32 count,u32 *done) {
    unsigned saved=critical_suppress; critical_suppress|=s->mode&DOS_OPEN_FAIL_ERRORS;
    int e=read_stream(s,buffer,count,done); critical_suppress=saved; return e;
}
int dos_read(unsigned h,void *buffer,u32 count,u32 *done) {
    *done=0; Sft *s; int e=slot(h,&s); return e?e:read_file(s,buffer,count,done);
}
static int write_stream(Sft *s,const void *buffer,u32 count,u32 *done) {
    *done=0; int e;
    if(!(s->mode&3)) return DE_ACCESS;
    if(s->device==2) {*done=count; return 0;}
    if(s->device==1) return device_output(buffer,count,s->device_flags&32,done);
    if(s->device) {
        DosDeviceRequest r; e=device_transfer(s,DOS_DEV_WRITE,(void *)buffer,count,DOS_DEVICE_WAIT,&r);
        *done=r.transferred; return e;
    }
    e=ready_file(s->file); if(e) return e;
    if(s->file->remote) return DE_ACCESS;
    if(count>UINT32_MAX-s->pos) return DE_FULL;
    u32 size=fat_size(&s->file->node);
    u64 start=MIN(s->pos,size),end=count?(u64)s->pos+count:s->pos>size?s->pos:size;
    e=locked(s->file,s,start,end); if(e) return e;
    FatFile f={s->file->node,s->pos}; e=fat_write(s->file->volume,&f,buffer,count,done);
    s->file->node=f.node; s->pos=f.pos;
    if(s->mode&DOS_OPEN_COMMIT) {int x=flush_file(s); if(!e) e=x;}
    return e;
}
static int write_file(Sft *s,const void *buffer,u32 count,u32 *done) {
    unsigned saved=critical_suppress; critical_suppress|=s->mode&DOS_OPEN_FAIL_ERRORS;
    int e=write_stream(s,buffer,count,done); critical_suppress=saved; return e;
}
int dos_write(unsigned h,const void *buffer,u32 count,u32 *done) {
    *done=0; Sft *s; int e=slot(h,&s); return e?e:write_file(s,buffer,count,done);
}
int dos_seek(unsigned h,i64 offset,unsigned origin,u32 *pos) {
    Sft *s; int e=slot(h,&s); if(e) return e;
    if(origin>2) return DE_FUNCTION;
    if(s->device) {*pos=0; return 0;}
    e=ready_file(s->file); if(e) return e;
    i64 base=origin==0?0:origin==1?s->pos:file_size(s->file);
    if(offset < -base || offset > (i64)UINT32_MAX-base) return DE_SEEK;
    s->pos=(u32)(base+offset); *pos=s->pos; return 0;
}
int dos_file_time(unsigned h,int set,u16 *date,u16 *time) {
    if(set<0 || set>1) return DE_FUNCTION;
    Sft *s; int e=slot(h,&s); if(e) return e;
    if(s->device) return DE_HANDLE;
    e=ready_file(s->file); if(e) return e;
    if(s->file->remote) {
        if(set) return DE_ACCESS;
        *time=s->file->remote_time; *date=s->file->remote_date; return 0;
    }
    if(set) {
        Node node=s->file->node; wr16(node.raw+22,*time); wr16(node.raw+24,*date);
        e=fat_sync_node(s->file->volume,&node); if(e) return e;
        s->file->node=node;
        return (s->mode&DOS_OPEN_COMMIT)?flush_file(s):0;
    }
    *time=rd16(s->file->node.raw+22); *date=rd16(s->file->node.raw+24); return 0;
}
int dos_lock(unsigned h,int unlock,u32 start,u32 length) {
    if(unlock>1 || unlock<0) return DE_FUNCTION;
    Sft *s; int e=slot(h,&s); if(e) return e;
    if(s->device) return DE_HANDLE;
    e=ready_file(s->file); if(e) return e;
    u64 end=(u64)start+length;
    if(!length || end>(1ULL<<32)) return DE_LOCK;
    FileLock *empty=NULL;
    for(unsigned i=0;i<ARRAY_SIZE(locks);i++) {
        FileLock *l=&locks[i];
        if(!l->owner) {if(!empty) empty=l; continue;}
        if(unlock) {
            if(l->owner==s && l->pid==dos_pid() && l->start==start && l->end==end) {l->owner=NULL; return 0;}
        } else if(l->owner->file==s->file && start<l->end && l->start<end) return DE_LOCK;
    }
    if(unlock) return DE_LOCK;
    if(!empty) return DE_LOCKS;
    *empty=(FileLock){s,dos_pid(),start,end}; return 0;
}
int dos_dup(unsigned old,unsigned *result) {
    Sft *s; int e=slot(old,&s); if(e) return e; e=spare_handle(result); if(e) return e;
    s->refs++; current()->handles[*result]=current()->handles[old]; return 0;
}
int dos_dup2(unsigned old,unsigned dest) {
    Sft *s; int e=slot(old,&s); if(e) return e; if(dest>=current()->handle_count) return DE_HANDLE;
    if(old==dest) return 0;
    if(current()->handles[dest]>=0) {e=dos_close(dest); if(e) return e;}
    s->refs++; current()->handles[dest]=current()->handles[old]; return 0;
}
static int remote_stat(unsigned drive,const char *path,DosRedirEntry *entry) {
    const DosRedirector *r=drives[drive].redir;
    return r->stat(r->context,path,entry);
}
/* The directory is kept in its letter's terms (a SUBST letter's from its
 * own root). */
int dos_chdir(const char *name) {
    if(device_find(name)) return DE_PATH;
    char path[DOS_PATH_MAX],own[DOS_PATH_MAX]; Node n; Fat *f; unsigned drive,letter;
    int e=logical_path(name,own,&letter); if(e) return e;
    e=volume_path(name,path,&drive,&f);
    if(e==DE_REMOTE) {
        DosRedirEntry entry; e=remote_stat(drive,path,&entry); if(e) return e==DE_NOFILE?DE_PATH:e;
        if(!(entry.attributes&FA_DIR)) return DE_PATH;
        return strcopy(current()->cwd[letter],DOS_PATH_MAX,own);
    }
    if(e) return e;
    e=fat_lookup(f,path,&n); if(e) return e;
    if(!(n.raw[11]&FA_DIR)) return DE_PATH;
    return strcopy(current()->cwd[letter],DOS_PATH_MAX,own);
}
int dos_mkdir(const char *name) {
    if(device_find(name)) return DE_ACCESS;
    char path[DOS_PATH_MAX]; Node n; Fat *f; unsigned drive;
    int e=volume_path(name,path,&drive,&f); if(e) return e==DE_REMOTE?DE_ACCESS:e;
    return fat_create(f,path,FA_DIR,&n);
}
int dos_remove(const char *name,int dir) {
    if(device_find(name)) return DE_ACCESS;
    char path[DOS_PATH_MAX]; Node n; Fat *f; unsigned drive;
    int e=volume_path(name,path,&drive,&f); if(e) return e==DE_REMOTE?DE_ACCESS:e;
    if(dir && directory_in_use(drive,path)) return DE_CURRENT;
    e=fat_lookup(f,path,&n); if(e) return e; if(busy(drive,&n)) return DE_ACCESS;
    return fat_remove(f,path,dir);
}
int dos_rename(const char *from,const char *to) {
    if(device_find(from) || device_find(to)) return DE_ACCESS;
    char a[DOS_PATH_MAX],b[DOS_PATH_MAX]; Node n; Fat *f; unsigned drive,other;
    int e=volume_path(from,a,&drive,&f); if(e) return e==DE_REMOTE?DE_ACCESS:e;
    u32 generation=drives[drive].generation;
    e=resolve_path(to,b,&other); if(e) return e; if(drive!=other) return DE_NOTSAME;
    if(generation!=drives[drive].generation) return DE_CHANGED;
    e=fat_lookup(f,a,&n); if(e) return e; if(busy(drive,&n)) return DE_ACCESS;
    if((n.raw[11]&FA_DIR) && directory_in_use(drive,a)) return DE_CURRENT;
    return fat_rename(f,a,b);
}
int dos_attribute(const char *name,int set,u8 *attr) {
    if(device_find(name)) {if(set) return DE_ACCESS; *attr=0x40; return 0;}
    char path[DOS_PATH_MAX]; Node n; Fat *f; unsigned drive;
    int e=volume_path(name,path,&drive,&f);
    if(e==DE_REMOTE) {
        DosRedirEntry entry;
        if(set) return DE_ACCESS;
        e=remote_stat(drive,path,&entry); if(!e) *attr=entry.attributes;
        return e;
    }
    if(e) return e;
    e=fat_lookup(f,path,&n); if(e) return e;
    if(!set) {*attr=n.raw[11]; return 0;}
    if((*attr&~0x27) || busy(drive,&n)) return DE_ACCESS;
    n.raw[11]=(n.raw[11]&(FA_DIR|FA_VOLUME))|*attr; return fat_sync_node(f,&n);
}
#define FIND_INDEX_MASK 0x07ffffffU
/* The drive generation a search started in, kept in the DTA's cookie bytes. */
static u32 find_generation(const DosFind *find) {
    return find->cookie_low | ((u32)find->cookie_high[0]<<8) |
        ((u32)find->cookie_high[1]<<16) | ((u32)find->cookie_high[2]<<24);
}
static void set_find_generation(DosFind *find,u32 generation) {
    find->cookie_low=generation; find->cookie_high[0]=generation>>8;
    find->cookie_high[1]=generation>>16; find->cookie_high[2]=generation>>24;
}
/* The search's attribute and 8.3 name rules for one directory entry. */
static int find_accepts(const DosFind *find,const u8 *name,u8 attr) {
    if(attr&FA_VOLUME) {if(!(find->search_attr&FA_VOLUME)) return 0;}
    else if((find->search_attr&0x1e)==FA_VOLUME) return 0;
    if(attr&(FA_HIDDEN|FA_SYSTEM|FA_DIR)&~find->search_attr) return 0;
    for(unsigned i=0;i<11;i++) {
        u8 c=!i && find->mask[i]==5?0xe5:find->mask[i];
        if(find->mask[i]!='?' && find->mask[i]!=name[i]) return 0;
        if(nls_lead(c) && i+1<(i<8?8:11)) {
            i++; if(find->mask[i]!=name[i]) return 0;
        }
    }
    return 1;
}
static int find_next_remote(DosFind *find,unsigned drive,Node *out) {
    const DosRedirector *r=drives[drive].redir; DosRedirEntry entry;
    u32 index=find->index&FIND_INDEX_MASK; int e;
    while(!(e=r->next(r->context,find->dir,&index,&entry))) {
        if(index>FIND_INDEX_MASK) return DE_NOMORE;
        find->index=(drive<<27)|index;
        u8 key[11]; memset(key,' ',11);
        if(entry.attributes&FA_VOLUME) {for(unsigned i=0;i<11 && entry.name[i];i++) key[i]=(u8)entry.name[i];}
        else if(fat_name83(entry.name,key)) continue;
        if(!find_accepts(find,key,entry.attributes)) continue;
        find->attr=entry.attributes; find->time=entry.time; find->date=entry.date; find->size=entry.size;
        strcopy(find->name,sizeof(find->name),entry.name);
        if(out) { /* the directory entry an FCB search returns */
            memset(out,0,sizeof(*out)); memcpy(out->raw,key,11); out->raw[11]=entry.attributes;
            wr16(out->raw+22,entry.time); wr16(out->raw+24,entry.date); wr32(out->raw+28,entry.size);
        }
        return 0;
    }
    return e;
}
static int find_next_node(DosFind *find,Node *out) {
    u32 generation=find_generation(find);
    if(!generation) return DE_NOMORE;
    unsigned drive=find->index>>27; Fat *f; int e=poll_drive(drive); if(e) return e;
    if(generation!=drives[drive].generation) return DE_CHANGED;
    if(drives[drive].redir) return find_next_remote(find,drive,out);
    e=ready_drive(drive,&f); if(e) return e;
    if(generation!=drives[drive].generation) return DE_CHANGED;
    Node n; u32 index=find->index&FIND_INDEX_MASK;
    while(!(e=fat_next(f,find->dir,&index,&n))) {
        find->index=(drive<<27)|index;
        if(!find_accepts(find,n.raw,n.raw[11])) continue;
        find->attr=n.raw[11]; find->time=rd16(n.raw+22); find->date=rd16(n.raw+24);
        find->size=fat_size(&n); fat_name(&n,find->name); if(out) *out=n; return 0;
    }
    find->index=(drive<<27)|index;
    return e;
}
int dos_find_next(DosFind *find) {return find_next_node(find,NULL);}
int dos_find_first(const char *pattern,u8 attr,DosFind *find) {
    char buf[DOS_PATH_MAX],path[DOS_PATH_MAX]; int e=strcopy(buf,sizeof(buf),pattern); if(e) return e;
    char *leaf=buf;
    for(char *p=buf;*p;) {
        unsigned width=nls_char_size(p); if(!width) return DE_PATH;
        if(width==1 && (*p=='\\'||*p=='/'||*p==':')) leaf=p+1;
        p+=width;
    }
    char mask[13]; e=strcopy(mask,sizeof(mask),leaf); if(e) return e;
    *leaf=0; unsigned drive; Fat *f; e=volume_path(buf,path,&drive,&f);
    u16 dir;
    if(e==DE_REMOTE) {
        DosRedirEntry entry; e=remote_stat(drive,path,&entry); if(e) return e==DE_NOFILE?DE_PATH:e;
        if(!(entry.attributes&FA_DIR)) return DE_PATH;
        dir=entry.directory;
    } else {
        if(e) return e;
        Node n; e=fat_lookup(f,path,&n); if(e) return e;
        if(!(n.raw[11]&FA_DIR)) return DE_PATH;
        dir=fat_cluster(&n);
    }
    memset(find,0,sizeof(*find)); find->dir=dir; find->search_attr=attr;
    find->index=drive<<27; set_find_generation(find,drives[drive].generation);
    memset(find->mask,' ',11); unsigned pos=0; int dot=0;
    if(!*mask) strcopy(mask,sizeof(mask),"*.*");
    for(char *p=mask;*p;) {
        unsigned width=nls_char_size(p); if(!width) return DE_PATH;
        unsigned end=dot?11:8;
        if(width==2) {
            if(pos+2>end) return DE_PATH;
            find->mask[pos++]=(u8)*p++; find->mask[pos++]=(u8)*p++; continue;
        }
        u8 c=nls_upper((u8)*p++,1);
        if(c=='.') {if(dot) return DE_PATH; dot=1; pos=8; continue;}
        if(c=='*') {
            while(pos<end) find->mask[pos++]='?';
            while(*p && *p!='.') {width=nls_char_size(p); if(!width) return DE_PATH; p+=width;}
        } else {if(pos>=end || !nls_file_char(c)) return DE_PATH; find->mask[pos++]=c;}
    }
    if(find->mask[0]==0xe5) find->mask[0]=5;
    /* COMMAND's bare '*' means all extensions. */
    if(!strcmp(mask,"*")) memset(find->mask+8,'?',3);
    return dos_find_next(find);
}
static void fcb_file_info(const Sft *s,FcbInfo *out) {
    *out=(FcbInfo){.device=s->device};
    if(s->file) {
        const Node *n=&s->file->node;
        out->drive=s->file->drive; out->size=fat_size(n);
        out->date=rd16(n->raw+24); out->time=rd16(n->raw+22);
    }
}
int fcb_ref_open(const char *name,u8 attr,int create,unsigned *ref,FcbInfo *info) {
    unsigned result; int e=open_file(name,DOS_OPEN_RDWR,attr,create?0x12:1,ref,&result,1);
    if(!e && sft[*ref].file && sft[*ref].file->remote) {release_file(&sft[*ref],dos_pid()); return DE_REMOTE;}
    if(!e) fcb_file_info(&sft[*ref],info);
    return e;
}
int fcb_ref_close(unsigned ref,u32 pid,const FcbInfo *before,const FcbInfo *wanted) {
    if(ref>=DOS_MAX_FILES || !sft[ref].refs) return DE_HANDLE;
    Sft *s=&sft[ref]; int e=0;
    if(before && !s->device) {
        e=ready_file(s->file);
        int resize=wanted->size!=before->size;
        int retime=wanted->date!=before->date || wanted->time!=before->time;
        if(!e && resize && !(s->mode&3)) e=DE_ACCESS;
        if(!e && resize) e=locked(s->file,s,MIN(wanted->size,fat_size(&s->file->node)),
            wanted->size>fat_size(&s->file->node)?wanted->size:fat_size(&s->file->node));
        if(!e && (resize || retime)) {
            Fat *f=s->file->volume; FatFile file={s->file->node,s->pos};
            e=fat_begin(f);
            if(!e) {
                if(resize) e=fat_truncate(f,&file,wanted->size);
                if(!e && retime) {
                    wr16(file.node.raw+24,wanted->date); wr16(file.node.raw+22,wanted->time);
                    e=fat_sync_node(f,&file.node);
                }
                e=fat_end(f,e); if(!e) s->file->node=file.node;
            }
        }
    }
    dos_preserve_error(e);
    if(before) {int x=flush_file(s); if(!e) e=dos_preserve_error(x);}
    int closed=release_file(s,pid); return e?e:closed;
}
int fcb_ref_io(unsigned ref,int write,u32 offset,void *data,u32 count,u32 *done,FcbInfo *info) {
    *done=0;
    if(ref>=DOS_MAX_FILES || !sft[ref].refs) return DE_HANDLE;
    Sft *s=&sft[ref]; s->pos=offset;
    int e=write?write_file(s,data,count,done):read_file(s,data,count,done);
    fcb_file_info(s,info); return e;
}
int fcb_path_info(const char *name,u8 attr,FcbInfo *info) {
    int device=device_find(name);
    if(device) {*info=(FcbInfo){.device=device}; return 0;}
    char path[DOS_PATH_MAX]; unsigned drive; Fat *f; Node n;
    int e=volume_path(name,path,&drive,&f); if(e) return e;
    e=fat_lookup(f,path,&n); if(e) return e;
    if(n.raw[11]&(FA_DIR|FA_VOLUME)) return DE_ACCESS;
    if(n.raw[11]&(FA_HIDDEN|FA_SYSTEM)&~attr) return DE_NOFILE;
    for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) {
        OpenFile *file=&open_files[i];
        if(file->refs && file->drive==drive && file->generation==drives[drive].generation && same_node(&file->node,&n)) {n=file->node; break;}
    }
    *info=(FcbInfo){fat_size(&n),rd16(n.raw+24),rd16(n.raw+22),drive,0}; return 0;
}
int fcb_find(unsigned drive,u8 attr,const u8 mask[11],DosFind *find,Node *out,int first) {
    if(first) {
        /* The letter's current directory, or for the label its drive's root. */
        char where[DOS_PATH_MAX]; u16 directory; unsigned letter=assigned(drive);
        if(letter>=DOS_DRIVES) return DE_DRIVE;
        if((attr&0x1e)==FA_VOLUME) {int e=physical_drive(letter,0,&drive); if(e) return e; strcopy(where,sizeof(where),"\\");}
        else {drive=letter; strcopy(where,sizeof(where),current()->cwd[letter]); int e=translate(&drive,where); if(e) return e;}
        Fat *f; int e=ready_drive(drive,&f);
        if(e==DE_REMOTE) {
            DosRedirEntry entry; e=remote_stat(drive,where,&entry); if(e) return e;
            if(!(entry.attributes&FA_DIR)) return DE_PATH;
            directory=entry.directory;
        } else {
            if(e) return e;
            Node dir; e=fat_lookup(f,where,&dir); if(e) return e;
            if(!(dir.raw[11]&FA_DIR)) return DE_PATH;
            directory=fat_cluster(&dir);
        }
        memset(find,0,sizeof(*find)); find->dir=directory; find->search_attr=attr;
        find->index=drive<<27; memcpy(find->mask,mask,11); set_find_generation(find,drives[drive].generation);
    }
    return find_next_node(find,out);
}
int fcb_mutate(const DosFind *find,const Node *node,const u8 *name) {
    unsigned drive=find->index>>27; Fat *f; u32 generation=find_generation(find);
    int e=ready_drive(drive,&f); if(e) return e;
    if(generation!=drives[drive].generation) return DE_CHANGED;
    if(busy(drive,node)) return DE_ACCESS;
    return fat_change_entry(f,find->dir,node,name);
}
/* Redirected drives (MSCDEX) are drives too, as in MS-DOS. */
int fcb_drive_exists(unsigned drive) {return letter_usable(drive);}
int fcb_label_create(unsigned drive,const u8 name[11],FcbInfo *info) {
    Fat *f; Node n; int e=physical_drive(drive,1,&drive); if(!e) e=ready_drive(drive,&f);
    if(e) return e;
    e=fat_create_label(f,name,&n);
    if(!e) *info=(FcbInfo){0,rd16(n.raw+24),rd16(n.raw+22),drive,0};
    return e;
}
static int task_index(u32 pid) {
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid==pid && pid) return (int)i;
    return -1;
}
static int new_task(unsigned *index,int exec) {
    unsigned i=1; while(i<DOS_PROCESSES && process[i].pid) i++;
    if(i==DOS_PROCESSES) return DE_NOMEM;
    Process *p=&process[i]; *p=*current();
    p->pid=next_pid++; p->parent=dos_pid(); p->running=0; p->dos16=p->switch_hooked=0; p->image=NULL;
    p->errorlevel=0; memset(&p->error,0,sizeof(p->error));
    p->return_kind=p->exit_kind=0; p->break_pending=0;
    p->break_handler=(DosBreakHandler){0};
    p->terminal_error=p->critical_failed=p->critical_pending=0;
    p->operation_drive=UINT32_MAX; p->operation_generation=0;
    p->operation_device=0;
    if(!exec) p->critical_handler=(DosCriticalHandler){0};
    memset(&p->default_dta,0,sizeof(p->default_dta)); p->dta=&p->default_dta; p->dta_size=128;
    /* EXEC inherits the original 20 slots; contexts clone the table size.
     * Private handles are excluded in both cases. */
    if(exec) p->handle_count=DOS_HANDLES;
    for(unsigned h=0;h<DOS_MAX_HANDLES;h++) {
        if(h>=p->handle_count) p->handles[h]=-1;
        if(p->handles[h]>=0 && (sft[p->handles[h]].mode&DOS_OPEN_PRIVATE)) p->handles[h]=-1;
        if(p->handles[h]>=0) sft[p->handles[h]].refs++;
    }
    *index=i; return 0;
}
int dos_task_create(u32 *pid) {
    if(in_dos) return DE_BUSY;
    if(!pid) return DE_FUNCTION;
    unsigned i; int e=new_task(&i,0); if(!e) *pid=process[i].pid;
    return e;
}
/* Switching away from a program (DosApi switch_hook): its DOS call, if any,
 * is set aside with InDOS clear while the hook lets others run. The hook is
 * the program's, put in by it or inherited from its parent. */
int dos_switch_hook(const DosSwitchHook *hook,DosSwitchHook *previous) {
    Process *p=current();
    if(previous) *previous=p->switch_hook;
    if(hook) {p->switch_hook=*hook; if(hook->hook) p->switch_hooked=1;}
    return 0;
}
/* VDM puts its 8086 program's low memory and VGA aside in a hook of its own;
 * a program whose VDM could not set one up, here or above, keeps the keys
 * from switching. */
static int dos16_unhooked(void) {
    const Process *p=current();
    for(unsigned n=0;n<DOS_PROCESSES;n++) {
        if(p->dos16 && !p->switch_hooked) return 1;
        int i=task_index(p->parent); if(i<0) break;
        p=&process[i];
    }
    return 0;
}
int dos_switch_away(u32 key) {
    unsigned slot=current_slot,busy=in_dos; int taken; DosSwitchHook hook=current()->switch_hook;
    if(!hook.hook || dos16_unhooked()) return 0;
    if(key&DOS_SWITCH_QUERY) return hook.hook(hook.context,key);
    in_dos=0;
    taken=hook.hook(hook.context,key);
    in_dos=busy; current_slot=slot;
    return taken;
}
int dos_task_select(u32 pid) {
    if(in_dos) return DE_BUSY;
    int i=task_index(pid); if(i<0) return DE_BLOCK;
    current_slot=(unsigned)i; return 0;
}
static int reap_task(unsigned index) {
    Process *p=&process[index]; int e=0;
    for(unsigned i=1;i<DOS_PROCESSES;i++) if(process[i].pid && process[i].parent==p->pid) {
        int x=reap_task(i); if(x && !e) e=dos_preserve_error(x);
    }
    for(unsigned i=0;i<p->handle_count;i++) if(p->handles[i]>=0) {
        int x=release_handle(p,i); if(x && !e) e=dos_preserve_error(x);
    }
    int x=fcb_reap(p->pid); if(x && !e) e=dos_preserve_error(x);
    for(unsigned drive=0;drive<DOS_DRIVES;drive++) if(drives[drive].volume && drives[drive].lock_pid==p->pid) {
        x=release_volume(drive); if(x && !e) e=dos_preserve_error(x);
    }
    for(unsigned i=0;i<ARRAY_SIZE(loaded_images);i++) if(loaded_images[i].token && loaded_images[i].owner==p->pid) {
        x=platform_image_discard(loaded_images[i].token); memset(&loaded_images[i],0,sizeof(loaded_images[i]));
        if(x && !e) e=dos_preserve_error(x);
    }
    for(unsigned d=0;d<DOS_DRIVES;d++) if(drives[d].redir && drives[d].redir_pid==p->pid && !p->resident) drop_redirection(d);
    /* A terminated-and-resident task keeps its blocks under its unique PID. */
    if(!p->resident) {x=arena_free_process(&dos_arena,p->pid); if(!e) e=dos_preserve_error(x);}
    memset(p,0,sizeof(*p)); return e;
}
int dos_task_destroy(u32 pid) {
    if(in_dos) return DE_BUSY;
    int i=task_index(pid); if(i<0) return DE_BLOCK;
    if(i==0 || (unsigned)i==current_slot || process[i].running) return DE_ACCESS;
    /* Do not reap a running descendant or the selected context through its parent. */
    for(unsigned j=1;j<DOS_PROCESSES;j++) if(process[j].pid && (process[j].running || j==current_slot)) {
        u32 ancestor=process[j].parent;
        for(unsigned limit=0;ancestor && limit<DOS_PROCESSES;limit++) {
            if(ancestor==pid) return DE_ACCESS;
            int a=task_index(ancestor); if(a<0) break;
            ancestor=process[a].parent;
        }
    }
    api_begin(); in_dos++;
    int e=dos_flush(); int released=reap_task((unsigned)i); if(!e) e=released;
    in_dos--; return api_result(e);
}
/* A program's name as MEM shows it: its image's, without directory or
 * extension. */
static void program_name(const char *path,char name[9]) {
    const char *leaf=path; unsigned n=0;
    for(;*path;path++) if(*path=='\\' || *path==':') leaf=path+1;
    while(leaf[n] && leaf[n]!='.' && n<8) {name[n]=leaf[n]; n++;}
    name[n]=0;
}
int dos_arena_block(u32 index,DosArenaBlock *b) {
    if(in_dos) return DE_BUSY;
    if(!b || b->size<sizeof(*b)) return DE_FUNCTION;
    u32 paragraph,paragraphs,owner; int e=arena_block(&dos_arena,index,&paragraph,&paragraphs,&owner);
    if(e) return e;
    b->paragraph=paragraph; b->paragraphs=paragraphs; b->owner=owner; b->name[0]=0;
    const void *data=dos_arena.base+((u64)paragraph+1)*16;
    int t=task_index(owner),image=-1;
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid && process[i].image==data) image=(int)i;
    if(!owner) b->kind=DOS_ARENA_FREE;
    else if(image>=0) {b->kind=DOS_ARENA_PROGRAM; b->owner=process[image].pid; program_name(process[image].program,b->name);}
    else if(owner==0xffffffffU || owner==process[0].pid) {b->kind=DOS_ARENA_SYSTEM; strcopy(b->name,sizeof(b->name),"MSDOS");}
    else {b->kind=DOS_ARENA_DATA; if(t>=0) program_name(process[t].program,b->name);}
    return 0;
}
int dos_query(DosInfo *info) {
    if(in_dos) return DE_BUSY;
    if(!info) return DE_FUNCTION;
    memset(info,0,sizeof(*info));
    info->pid=dos_pid(); info->errorlevel=dos_errorlevel; info->in_dos=in_dos;
    api_begin(); in_dos++;
    Fat *f; unsigned drive; int e=physical_drive(current()->drive,1,&drive);
    if(!e) e=ready_drive(drive,&f);
    if(!e) {
        info->fat_bits=f->bits; info->sectors_per_cluster=f->spc; info->total_clusters=f->clusters;
        e=fat_free_space(f,&info->free_clusters);
    } else if(e==DE_REMOTE) e=0;
    if(!e) e=arena_check(&dos_arena,&info->largest_paragraphs);
    in_dos--; return api_result(e);
}
static int get_drive_info(unsigned drive,DosDriveInfo *info) {
    memset(info,0,sizeof(*info)); info->drive=drive;
    int e=poll_drive(drive); if(e) return e;
    Drive *d=&drives[drive]; info->generation=d->generation; info->sectors=d->media.disk.sectors;
    if(d->redir) {
        u32 clusters=0,free=0,bytes=0;
        info->flags=DOS_DRIVE_PRESENT|DOS_DRIVE_REMOVABLE|DOS_DRIVE_READONLY|DOS_DRIVE_REMOTE;
        e=d->redir->space(d->redir->context,&clusters,&free,&bytes); if(e) return e;
        info->sectors_per_cluster=bytes/512; info->total_clusters=clusters; info->free_clusters=free;
        info->sectors=(u64)clusters*(bytes/512);
        return 0;
    }
    if(d->media.disk.readonly) info->flags|=DOS_DRIVE_READONLY;
    if(d->media.flags&IO_DISK_REMOVABLE) info->flags|=DOS_DRIVE_REMOVABLE;
    if(!(d->media.flags&IO_DISK_PRESENT)) return 0;
    info->flags|=DOS_DRIVE_PRESENT;
    Fat *f; e=ready_drive(drive,&f); if(e) return e;
    if(info->generation!=d->generation) return DE_CHANGED;
    info->fat_bits=f->bits; info->sectors_per_cluster=f->spc; info->total_clusters=f->clusters;
    return fat_free_space(f,&info->free_clusters);
}
/* A SUBST or ASSIGNed letter gives the figures of its drive, and says so. */
int dos_drive_info(u32 drive,DosDriveInfo *info) {
    if(in_dos) return DE_BUSY;
    if(!info) return DE_FUNCTION;
    api_begin(); in_dos++;
    unsigned target; int e=drive>=DOS_DRIVES?DE_DRIVE:physical_drive(drive,1,&target);
    if(e) memset(info,0,sizeof(*info));
    else {
        e=get_drive_info(target,info); info->drive=drive;
        if(drive_maps[assigned(drive)].kind==DOS_MAP_SUBST) info->flags|=DOS_DRIVE_SUBST;
        if(assigned(drive)!=drive) info->flags|=DOS_DRIVE_ASSIGNED;
    }
    in_dos--; return api_result(e);
}
void dos_set_errorlevel(unsigned level) {if(!in_dos) {dos_errorlevel=level&255; current()->return_kind=0;}}
static int image_type(const void *image,u32 size,unsigned subsystem) {
    if(size<64) return DE_FORMAT;
    const u8 *b=image; u32 pe=rd32(b+60);
    if(rd16(b)!=0x5a4d || pe>size-24 || rd32(b+pe)!=0x4550 || rd16(b+pe+4)!=0x200) return DE_FORMAT;
    u32 optional=rd16(b+pe+20);
    if(optional<70 || optional>size-pe-24 || rd16(b+pe+24)!=0x20b || rd16(b+pe+24+68)!=subsystem) return DE_FORMAT;
    return 0;
}
int dos_load_driver(const char *name,const char *tail) {
    if(in_dos || current_slot) return DE_BUSY;
    if(!tail || strlen(tail)>255) return DE_ENV;
    if(driver_count==DOS_MAX_DRIVERS) return DE_NOMEM;
    const IoServices *io=platform_io_services();
    if(!io || io->size<offsetof(IoServices,module_unload)+sizeof(io->module_unload) || !io->module_load || !io->module_unload) return DE_FUNCTION;
    api_begin(); in_dos++;
    unsigned h; int e=dos_open(name,0,0,&h); void *image=NULL; u32 pages=0,size=0;
    u64 module=0; unsigned owner=driver_count+1;
    if(!e) {
        e=dos_seek(h,0,2,&size);
        if(!e && (size<64 || size>16*1024*1024)) e=DE_FORMAT;
        if(!e) {pages=(size+4095)/4096; e=io->alloc_pages(io->context,pages,&image);}
        u32 pos,done;
        if(!e) e=dos_seek(h,0,0,&pos);
        if(!e) {e=dos_read(h,image,size,&done); if(!e && done!=size) e=DE_IO;}
        dos_preserve_error(e);
        int close=dos_close(h); if(!e) e=close;
    }
    if(!e) e=image_type(image,size,11);
    if(!e) e=device_begin(owner);
    if(!e) {
        loading_tail=tail; ansi_pending=0;
        e=platform_module_load(image,size,&module);
        loading_tail=NULL;
        if(!e && !module) e=DE_FORMAT;
        if(!e) e=device_commit(owner,io,tail);
        else {
            int registration=device_load_error(owner); if(registration) e=registration;
            dos_preserve_error(e);
            device_cancel(owner); /* A failed image entry has already unloaded. */
        }
        if(e && module) {
            dos_preserve_error(e);
            device_cancel(owner); int unload=platform_module_unload(module);
            if(unload) driver_modules[driver_count++]=module;
        }
    }
    if(!e) driver_modules[driver_count++]=module;
    /* ANSI.SYS takes the console once it has loaded. */
    if(!e && ansi_pending) ansi_set_options(ansi_pending);
    ansi_pending=0;
    if(image) io->free_pages(io->context,image,pages);
    in_dos--; return api_result(e);
}
/* Open files on the drive lose their redirector; tasks there move to C:. */
static void drop_redirection(unsigned drive) {
    const DosRedirector *r=drives[drive].redir;
    for(unsigned i=0;i<ARRAY_SIZE(open_files);i++)
        if(open_files[i].refs && open_files[i].remote && open_files[i].drive==drive) {
            r->close(r->context,open_files[i].remote_file); open_files[i].generation=0;
        }
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid && process[i].drive==drive) process[i].drive=2;
    memset(&drives[drive],0,sizeof(drives[drive]));
}
int dos_redirect(u32 *drive,const DosRedirector *r) {
    if(in_dos) return DE_BUSY;
    if(!drive || !r || r->size<sizeof(*r) || !r->stat || !r->open || !r->read || !r->close || !r->next ||
       !r->space || !r->generation) return DE_FUNCTION;
    unsigned d=*drive;
    if(d==UINT32_MAX) {
        unsigned last=2;
        for(unsigned i=0;i<DOS_DRIVES;i++) if(drives[i].volume) last=i;
        for(d=last+1;drive_exists(d) || (d<DOS_DRIVES && drive_maps[d].kind);d++) {}
    }
    if(d>=DOS_DRIVES) return DE_DRIVE;
    if(drive_exists(d) || drive_maps[d].kind) return DE_ACCESS;
    api_begin(); in_dos++;
    drives[d]=(Drive){.redir=r,.redir_pid=dos_pid(),.redir_seen=r->generation(r->context),.generation=1};
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid) strcopy(process[i].cwd[d],DOS_PATH_MAX,"\\");
    *drive=d;
    in_dos--; return api_result(0);
}
int dos_unredirect(u32 drive) {
    if(in_dos) return DE_BUSY;
    if(drive>=DOS_DRIVES || !drives[drive].redir) return DE_DRIVE;
    for(unsigned i=0;i<ARRAY_SIZE(open_files);i++)
        if(open_files[i].refs && open_files[i].remote && open_files[i].drive==drive) return DE_ACCESS;
    drop_redirection(drive); return 0;
}
int dos_drivers_pending(void) {return driver_count!=0 || resident_count!=0;}
int dos_finish_drivers(void) {
    if(in_dos || current_slot) return DE_BUSY;
    api_begin(); in_dos++; int error=dos_flush();
    for(unsigned d=0;d<DOS_DRIVES;d++) if(drives[d].redir) drop_redirection(d);
    /* Resident programs may use drivers, so they leave first, newest first. */
    for(unsigned i=resident_count;i;i--) if(resident_modules[i-1]) {
        int e=platform_module_unload(resident_modules[i-1]);
        if(e) {if(!error) error=dos_preserve_error(e);} else resident_modules[i-1]=0;
    }
    while(resident_count && !resident_modules[resident_count-1]) resident_count--;
    for(unsigned p=1;p<DOS_PROCESSES;p++) if(process[p].pid) {int e=reap_task(p); if(e && !error) error=dos_preserve_error(e);}
    for(unsigned h=0;h<process[0].handle_count;h++) if(process[0].handles[h]>=0) {
        int e=release_handle(&process[0],h); if(e && !error) error=dos_preserve_error(e);
    }
    {int e=fcb_reap(process[0].pid); if(e && !error) error=dos_preserve_error(e);}
    for(unsigned owner=driver_count;owner;owner--) if(driver_modules[owner-1]) {
        int e=device_finish(owner); if(e && !error) error=dos_preserve_error(e);
        if(device_pending(owner)) continue;
        e=platform_module_unload(driver_modules[owner-1]);
        if(e) {if(!error) error=dos_preserve_error(e);} else driver_modules[owner-1]=0;
    }
    while(driver_count && !driver_modules[driver_count-1]) driver_count--;
    in_dos--; return api_result(error);
}
/* Programs for 8086 DOS run in VDM.EXE, on processors with the IA-32
 * instruction set: MZ images that are not IA-64 PE images, and .COM files
 * without an MZ header. VDM.EXE gets the program's path before its tail. */
#define VDM_PATH "C:\\VDM.EXE"
static int dos16_image(const char *name,const u8 *b,u32 size) {
    if(size>=2 && ((b[0]=='M' && b[1]=='Z') || (b[0]=='Z' && b[1]=='M'))) {
        u32 pe=size>=64?rd32(b+60):0;
        return !(pe && pe<=size-6 && rd32(b+pe)==0x4550 && rd16(b+pe+4)==0x200);
    }
    const char *dot=NULL;
    for(const char *p=name;*p;p++) if(*p=='.') dot=p; else if(*p=='\\' || *p==':') dot=NULL;
    return dot && !stricmp(dot,".COM");
}
static int exec_prepare(const char *name,const char *tail,unsigned *child,void **image,u32 *size);
static int exec_dos16(const char *name,const char *tail,unsigned *child,void **image,u32 *size) {
    char full[DOS_PATH_MAX],vdm_tail[sizeof(current()->tail)];
    int e=dos_canonical(name,full); if(e) return e;
    if(!stricmp(full,VDM_PATH)) return DE_FORMAT;
    e=strcopy(vdm_tail,sizeof(vdm_tail),full);
    if(!e) e=strappend(vdm_tail,sizeof(vdm_tail)," ");
    if(!e) e=strappend(vdm_tail,sizeof(vdm_tail),tail);
    if(e) return DE_ENV;
    raw_letters++; e=exec_prepare(VDM_PATH,vdm_tail,child,image,size); raw_letters--;
    if(!e) process[*child].dos16=1;
    return e==DE_NOFILE || e==DE_PATH?DE_FORMAT:e;
}
/* EXEC accepts IA-64 applications (subsystem 10) and resident-capable boot
 * services images (subsystem 11, which may end with AH=31h). */
static int exec_prepare(const char *name,const char *tail,unsigned *child,void **image,u32 *size) {
    *image=NULL;
    if(!tail || strlen(tail)>=sizeof(current()->tail)) return DE_ENV;
    unsigned h; int e=dos_open(name,0,0,&h); if(e) return e;
    e=dos_seek(h,0,2,size);
    /* Too small for an IA-64 image, but not for an 8086 .COM program. */
    if(!e && *size && *size<64 && dos16_image(name,NULL,0)) {dos_close(h); return exec_dos16(name,tail,child,image,size);}
    if(e || *size<64 || *size>16*1024*1024) {e=dos_preserve_error(e?e:DE_FORMAT); dos_close(h); return e;}
    u32 ignored,largest;
    dos_arena.method=current()->allocation_method;
    e=arena_alloc(&dos_arena,(*size+15)/16,dos_pid(),image,&largest);
    if(e) {dos_preserve_error(e); dos_close(h); *image=NULL; return e;}
    e=dos_seek(h,0,0,&ignored); u32 got=0;
    if(!e) e=dos_read(h,*image,*size,&got);
    if(!e && got!=*size) e=DE_IO;
    dos_preserve_error(e); int ce=dos_close(h);
    if(!e && ce) e=ce;
    if(!e && image_type(*image,*size,10)) e=image_type(*image,*size,11);
    if(e==DE_FORMAT && dos16_image(name,*image,*size)) {
        arena_free(&dos_arena,*image,dos_pid()); *image=NULL;
        return exec_dos16(name,tail,child,image,size);
    }
    if(!e) e=new_task(child,1);
    if(!e) {
        Process *p=&process[*child]; char path[DOS_PATH_MAX]; unsigned drive;
        strcopy(p->tail,sizeof(p->tail),tail); p->image=*image;
        p->program[0]=0;
        if(!logical_path(name,path,&drive) && strlen(path)+2<DOS_PATH_MAX) {p->program[0]=(char)('A'+drive); p->program[1]=':'; strcopy(p->program+2,DOS_PATH_MAX-2,path);}
    }
    if(e) {arena_free(&dos_arena,*image,dos_pid()); *image=NULL;}
    return e;
}
static int exec_run(unsigned child,const void *image,u32 size,u64 token) {
    unsigned parent=current_slot,saved_busy=in_dos,result=0;
    process[child].running=1; current_slot=child;
    /* EXEC suspends its DOS transaction before entering application code.
     * The child can call DOS, but a callback inside an active DOS call cannot. */
    in_dos=0;
    int e=token?platform_image_start(token,process[child].tail,&result):platform_exec(image,size,process[child].tail,&result);
    in_dos=saved_busy;
    current_slot=parent; process[child].running=0;
    /* Preserve traditional EXEC's directory side effect; independent task
     * contexts keep their own directory while Interface Manager selects them. */
    memcpy(current()->cwd,process[child].cwd,sizeof(current()->cwd));
    current()->drive=process[child].drive;
    current()->return_kind=process[child].exit_kind;
    dos_preserve_error(e);
    if(process[child].resident) {
        if(resident_count<DOS_MAX_RESIDENT) resident_modules[resident_count++]=process[child].resident_token;
        else if(!e) e=dos_preserve_error(DE_NOMEM); /* The image stays; DOS cannot unload it. */
    }
    int x=dos_flush(); if(!e && x) e=x;
    x=reap_task(child); if(!e && x) e=x;
    dos_errorlevel=result; return e;
}
static int exec_file(const char *name,const char *tail) {
    unsigned child=0; void *image; u32 size;
    int e=exec_prepare(name,tail,&child,&image,&size); if(e) return e;
    e=exec_run(child,image,size,0);
    arena_free(&dos_arena,image,dos_pid()); return e;
}
static int exec_load(const char *name,DosExecLoad *load) {
    if(!load || load->size<sizeof(*load)) return DE_FUNCTION;
    unsigned slot=0; while(slot<ARRAY_SIZE(loaded_images) && loaded_images[slot].token) slot++;
    if(slot==ARRAY_SIZE(loaded_images)) return DE_HANDLES;
    unsigned child=0; void *image; u32 size;
    int e=exec_prepare(name,load->tail?load->tail:"",&child,&image,&size); if(e) return e;
    u64 token=0,base=0,bytes=0; const u8 *b=image;
    u32 entry=rd32(b+rd32(b+60)+24+16);
    e=platform_image_load(image,size,&token,&base,&bytes);
    arena_free(&dos_arena,image,dos_pid()); process[child].image=NULL;
    if(e) {dos_preserve_error(e); reap_task(child); return e;}
    loaded_images[slot]=(LoadedImage){token,dos_pid(),process[child].pid};
    load->pid=process[child].pid; load->token=token; load->base=base; load->image_size=bytes; load->entry=base+entry;
    return 0;
}
static int exec_loaded(u64 token,int start) {
    for(unsigned i=0;i<ARRAY_SIZE(loaded_images);i++) {
        LoadedImage *l=&loaded_images[i];
        if(!token || l->token!=token || l->owner!=dos_pid()) continue;
        int child=task_index(l->pid); *l=(LoadedImage){0};
        if(child<0) return DE_BLOCK;
        if(start) return exec_run((unsigned)child,NULL,0,token);
        int e=platform_image_discard(token); dos_preserve_error(e);
        int x=reap_task((unsigned)child); return e?e:x;
    }
    return DE_HANDLE;
}
static int load_overlay(const char *name,DosOverlay *overlay) {
    if(!overlay || overlay->size<sizeof(*overlay) || (overlay->capacity && !overlay->buffer)) return DE_FUNCTION;
    overlay->loaded=0;
    unsigned h; int e=dos_open(name,0,0,&h); if(e) return e;
    u32 size,pos,got=0; e=dos_seek(h,0,2,&size);
    if(!e && size>overlay->capacity) e=DE_NOMEM;
    if(!e) e=dos_seek(h,0,0,&pos);
    if(!e) e=dos_read(h,overlay->buffer,size,&got);
    if(!e && got!=size) e=DE_IO;
    dos_preserve_error(e); int c=dos_close(h); if(!e) e=c;
    if(!e) overlay->loaded=size;
    return e;
}
int dos_exec(const char *name,const char *tail) {
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; int e=exec_file(name,tail); in_dos--; return api_result(e);
}
/* Maintenance access for FORMAT/CHKDSK/SYS/LABEL/FDISK. Raw transfers never
 * mount FAT or run the critical-error handler: the cause is returned. */
static int raw_drive(unsigned drive,int present,Drive **out) {
    int e=poll_drive_once(drive); if(e) return e;
    Drive *d=&drives[drive];
    if(d->redir) return DE_REMOTE;
    current()->operation_drive=drive; current()->operation_generation=d->generation; current()->operation_device=0;
    if(d->lock_pid && d->lock_pid!=dos_pid()) return DE_ACCESS;
    if(present && !(d->media.flags&IO_DISK_PRESENT)) return DE_NOTREADY;
    *out=d; return 0;
}
/* 512-byte sectors in turn; *done counts those transferred. */
static int disk_sectors(const Disk *disk,u64 sector,u32 count,void *buffer,int write,u32 *done) {
    u8 *p=buffer;
    for(;*done<count;(*done)++) {
        u32 lba=(u32)(sector+*done); u8 *at=p+(u64)*done*512;
        int e=write?disk->write(disk->ctx,lba,at):disk->read(disk->ctx,lba,at);
        if(e) return e;
    }
    return 0;
}
static int raw_transfer(unsigned drive,u64 sector,u32 count,void *buffer,int write,u32 *done) {
    *done=0; Drive *d; int e=raw_drive(drive,1,&d); if(e) return e;
    if(count && !buffer) return DE_FUNCTION;
    u64 total=MIN(d->media.disk.sectors,(u64)UINT32_MAX+1);
    if(sector>total || count>total-sector) return DE_SEEK;
    if(write && d->lock_pid!=dos_pid()) return DE_ACCESS;
    if(write && d->media.disk.readonly) return DE_READONLY;
    return disk_sectors(&d->media.disk,sector,count,buffer,write,done);
}
static int release_volume(unsigned drive) {
    Drive *d=&drives[drive]; d->lock_pid=0;
    int e=(d->media.flags&IO_DISK_PRESENT) && d->media.disk.flush?d->media.disk.flush(d->media.disk.ctx):0;
    int x=invalidate_drive(drive); return e?e:x;
}
static int lock_volume(unsigned drive,int lock) {
    int e=poll_drive_once(drive); if(e) return e;
    Drive *d=&drives[drive]; u32 pid=dos_pid();
    if(!lock) return d->lock_pid==pid?release_volume(drive):DE_ACCESS;
    if(d->lock_pid) return d->lock_pid==pid?0:DE_ACCESS;
    if(d->volume->tx_depth || d->volume->tx_first) return DE_BUSY;
    for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) if(open_files[i].refs && open_files[i].drive==drive) return DE_ACCESS;
    if(d->mounted && !d->volume->faulted) {e=fat_io_flush(d->volume,0); if(e) return e;}
    d->lock_pid=pid; return 0;
}
/* INT 25h/26h go to the drive a letter means, as ASSIGN and SUBST send them. */
static int disk_call(u32 drive,u64 sector,u32 count,void *buffer,int write,u32 *done) {
    u32 ignored; if(!done) done=&ignored; *done=0;
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; unsigned target; int e=drive>=DOS_DRIVES?DE_DRIVE:physical_drive(drive,1,&target);
    if(!e) e=raw_transfer(target,sector,count,buffer,write,done);
    in_dos--; return api_result(e);
}
int dos_disk_read(u32 drive,u64 sector,u32 count,void *buffer,u32 *done) {return disk_call(drive,sector,count,buffer,0,done);}
int dos_disk_write(u32 drive,u64 sector,u32 count,const void *buffer,u32 *done) {return disk_call(drive,sector,count,(void *)buffer,1,done);}
int dos_volume_lock(u32 drive,int lock) {
    if(in_dos) return DE_BUSY;
    if(lock<0 || lock>1) return DE_FUNCTION;
    api_begin(); in_dos++; unsigned target; int e=drive>=DOS_DRIVES?DE_DRIVE:physical_drive(drive,1,&target);
    if(!e) e=lock_volume(target,lock);
    in_dos--; return api_result(e);
}
static const IoServices *physical_io(void) {
    const IoServices *io=platform_io_services();
    return io && io->size>=offsetof(IoServices,disk_location)+sizeof(io->disk_location) &&
        io->physical_count && io->physical_info?io:NULL;
}
static int physical_lookup(u32 index,IoDiskInfo *out) {
    const IoServices *io=physical_io(); if(!io) return DE_FUNCTION;
    if(index>=io->physical_count(io->context)) return DE_DRIVE;
    int e=io->physical_info(io->context,index,out); if(e) return e;
    return out->generation && out->disk.read && out->disk.write?0:DE_FORMAT;
}
/* Physical disk index and first sector of a DOS drive, when IO.SYS knows. */
static int drive_location(unsigned drive,u32 *physical,u64 *start) {
    *physical=UINT32_MAX; *start=0;
    const IoServices *io=physical_io(); Drive *d=&drives[drive];
    if(!io || !io->disk_location || d->driver || !disk_services || !d->volume) return DE_FUNCTION;
    return io->disk_location(io->context,d->unit,physical,start);
}
int dos_physical_info(u32 index,DosPhysicalInfo *out) {
    if(in_dos) return DE_BUSY;
    if(!out) return DE_FUNCTION;
    api_begin(); in_dos++;
    IoDiskInfo disk; int e=physical_lookup(index,&disk);
    if(!e) {
        *out=(DosPhysicalInfo){.size=sizeof(*out),.index=index,.generation=disk.generation,.sectors=disk.disk.sectors};
        if(disk.flags&IO_DISK_PRESENT) out->flags|=DOS_DRIVE_PRESENT;
        if(disk.flags&IO_DISK_REMOVABLE) out->flags|=DOS_DRIVE_REMOVABLE;
        if(disk.flags&IO_DISK_BOOT) out->flags|=DOS_PHYSICAL_BOOT;
        if(disk.disk.readonly) out->flags|=DOS_DRIVE_READONLY;
        for(unsigned drive=0;drive<DOS_DRIVES;drive++) {
            u32 physical; u64 start;
            if(!drive_location(drive,&physical,&start) && physical==index) out->drives|=1U<<drive;
        }
    }
    in_dos--; return api_result(e);
}
static int physical_transfer(u32 index,u64 sector,u32 count,void *buffer,int write,u32 *done) {
    IoDiskInfo disk; int e=physical_lookup(index,&disk); if(e) return e;
    if(!(disk.flags&IO_DISK_PRESENT)) return DE_NOTREADY;
    if(count && !buffer) return DE_FUNCTION;
    u64 total=MIN(disk.disk.sectors,(u64)UINT32_MAX+1);
    if(sector>total || count>total-sector) return DE_SEEK;
    if(write && disk.disk.readonly) return DE_READONLY;
    e=disk_sectors(&disk.disk,sector,count,buffer,write,done);
    if(!e && write && count && disk.disk.flush) e=disk.disk.flush(disk.disk.ctx);
    return e;
}
static int physical_call(u32 index,u64 sector,u32 count,void *buffer,int write,u32 *done) {
    u32 ignored; if(!done) done=&ignored; *done=0;
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; int e=physical_transfer(index,sector,count,buffer,write,done); in_dos--; return api_result(e);
}
int dos_physical_read(u32 index,u64 sector,u32 count,void *buffer,u32 *done) {return physical_call(index,sector,count,buffer,0,done);}
int dos_physical_write(u32 index,u64 sector,u32 count,const void *buffer,u32 *done) {return physical_call(index,sector,count,(void *)buffer,1,done);}
void dos_restart(void) {flush_and(platform_restart,"Restart");}
static DosDpb dpbs[DOS_DRIVES];
static u8 media_bytes[DOS_DRIVES];
static int drive_dpb(unsigned drive) {
    Fat *f; int e=ready_drive(drive,&f); if(e) return e;
    u8 boot[512]; u32 free; e=fat_sector_read(f,0,boot); if(!e) e=fat_free_space(f,&free);
    if(e) return e;
    unsigned shift=0; while((1U<<shift)<f->spc) shift++;
    dpbs[drive]=(DosDpb){.size=sizeof(DosDpb),.drive=drive,.unit=drives[drive].unit,.sector_bytes=512,
        .sectors_per_cluster=f->spc,.cluster_shift=shift,.reserved_sectors=f->fat_start,.fats=f->copies,
        .root_entries=f->root_entries,.first_root=f->root_start,.first_data=f->data_start,.clusters=f->clusters,
        .sectors_per_fat=f->fat_sectors,.media=boot[21],.fat_bits=f->bits,.free_clusters=free,.sectors=f->total};
    media_bytes[drive]=boot[21]; return 0;
}
/* MS-DOS 4 BIOS/MSINIT.ASM DiskTable2 (hard disks) and standard diskettes. */
static int build_bpb(u64 sectors,int removable,DosDeviceParams *p) {
    static const struct {u16 sectors; u8 media,spc,spf,spt,type; u16 root;} floppies[]={
        {720,0xfd,2,2,9,DOS_DEVICE_360K,112},{1440,0xf9,2,3,9,DOS_DEVICE_720K,112},
        {2400,0xf9,1,7,15,DOS_DEVICE_1200K,224},{2880,0xf0,1,9,18,DOS_DEVICE_OTHER,224},
        {5760,0xf0,2,9,36,DOS_DEVICE_OTHER,240}};
    static const struct {u32 limit; u8 spc,big;} disks[]={
        {32680,8,0},{0x40000,4,1},{0x80000,8,1},{0x100000,16,1},{0x200000,32,1},{0x400000,64,1},{0x800000,128,1}};
    p->reserved_sectors=1; p->fats=2; p->sector_bytes=512;
    for(unsigned i=0;removable && i<ARRAY_SIZE(floppies);i++) if(sectors==floppies[i].sectors) {
        p->media=floppies[i].media; p->sectors_per_cluster=floppies[i].spc; p->sectors_per_fat=floppies[i].spf;
        p->root_entries=floppies[i].root; p->sectors_per_track=floppies[i].spt; p->heads=2;
        p->device_type=floppies[i].type; p->fat_bits=12; return 0;
    }
    unsigned row=0; while(row<ARRAY_SIZE(disks) && sectors>disks[row].limit) row++;
    if(row==ARRAY_SIZE(disks) || sectors<64) return DE_FORMAT;
    u32 spc=disks[row].spc,t=(u32)sectors;
    p->media=0xf8; p->sectors_per_cluster=spc; p->root_entries=512; p->sectors_per_track=63; p->heads=255;
    p->device_type=removable?DOS_DEVICE_OTHER:DOS_DEVICE_FIXED;
    if(!disks[row].big) {
        u32 n=(1+(t+spc-1)/spc)&~1U; p->sectors_per_fat=(n+n/2+511)/512;
    } else p->sectors_per_fat=(t-32+256*spc)/(256*spc+2);
    u32 clusters=(t-1-2*p->sectors_per_fat-32)/spc;
    p->fat_bits=clusters<4085?12:16;
    return clusters>=65525 || clusters<16?DE_FORMAT:0;
}
static int bpb_fields(const u8 *b,u64 sectors,DosDeviceParams *p) {
    FatBpb f; int e=fat_parse_bpb(b,sectors,&f); if(e) return e;
    p->sectors_per_cluster=f.spc; p->reserved_sectors=f.reserved; p->fats=f.fats; p->root_entries=f.root_entries;
    p->sectors_per_fat=f.fat_sectors; p->media=b[21]; p->fat_bits=f.bits;
    p->sectors_per_track=rd16(b+24); p->heads=rd16(b+26); return 0;
}
static int device_params(unsigned drive,DosDeviceParams *out) {
    if(!out || out->size<sizeof(*out) || (out->flags&~DOS_PARAMS_CURRENT)) return DE_FUNCTION;
    Drive *d; int e=raw_drive(drive,0,&d); if(e) return e;
    int removable=!!(d->media.flags&IO_DISK_REMOVABLE);
    DosDeviceParams p={.size=sizeof(p),.flags=out->flags,.sectors=d->media.disk.sectors,.sector_bytes=512,
        .attributes=removable?0:DOS_DEVICE_NONREMOVABLE};
    drive_location(drive,&p.physical,&p.hidden);
    if(out->flags&DOS_PARAMS_CURRENT) {
        if(!(d->media.flags&IO_DISK_PRESENT)) return DE_NOTREADY;
        u8 boot[512]; e=d->media.disk.read(d->media.disk.ctx,0,boot); if(e) return e;
        DosDeviceParams built=p; e=bpb_fields(boot,p.sectors,&p); if(e) return e;
        p.device_type=build_bpb(p.sectors,removable,&built)?removable?DOS_DEVICE_OTHER:DOS_DEVICE_FIXED:built.device_type;
    } else if(d->params_set) {
        DosDeviceParams saved=d->params; saved.flags=0; saved.sectors=p.sectors; saved.hidden=p.hidden;
        saved.physical=p.physical; saved.attributes=p.attributes; p=saved;
    } else {e=build_bpb(p.sectors,removable,&p); if(e) return e;}
    *out=p; return 0;
}
static int set_device_params(unsigned drive,const DosDeviceParams *in) {
    if(!in || in->size<sizeof(*in) || in->sector_bytes!=512) return DE_FUNCTION;
    Drive *d; int e=raw_drive(drive,0,&d); if(e) return e;
    u8 boot[512]={0}; DosDeviceParams check=*in;
    boot[13]=in->sectors_per_cluster; wr16(boot+11,512); wr16(boot+14,in->reserved_sectors); boot[16]=in->fats;
    wr16(boot+17,in->root_entries); wr16(boot+22,in->sectors_per_fat); boot[21]=in->media; wr16(boot+510,0xaa55);
    if(in->sectors_per_cluster>255 || in->reserved_sectors>65535 || in->fats>255 || in->root_entries>65535 ||
       in->sectors_per_fat>65535 || in->media<0xf0 || in->media>255 || d->media.disk.sectors>UINT32_MAX ||
       (in->root_entries&15)) return DE_FUNCTION;
    wr32(boot+32,(u32)d->media.disk.sectors);
    e=bpb_fields(boot,d->media.disk.sectors,&check); if(e) return DE_FUNCTION;
    d->params=*in; d->params.fat_bits=check.fat_bits; d->params_set=1; return 0;
}
static int media_id(unsigned drive,DosMediaId *id,int set) {
    if(!id || id->size<sizeof(*id)) return DE_FUNCTION;
    Drive *d; int e=raw_drive(drive,1,&d); if(e) return e;
    if(set && d->media.disk.readonly) return DE_READONLY;
    u8 boot[512]; e=d->media.disk.read(d->media.disk.ctx,0,boot); if(e) return e;
    if(rd16(boot+510)!=0xaa55 || rd16(boot+11)!=512 || boot[38]!=0x29) return DE_FORMAT;
    if(!set) {
        id->serial=rd32(boot+39); memcpy(id->label,boot+43,11); memcpy(id->file_system,boot+54,8); return 0;
    }
    /* The boot record is outside every FAT transaction; a mounted volume
     * caches no field changed here. */
    wr32(boot+39,id->serial); memcpy(boot+43,id->label,11); memcpy(boot+54,id->file_system,8);
    e=d->media.disk.write(d->media.disk.ctx,0,boot); if(e) return e;
    return d->media.disk.flush?d->media.disk.flush(d->media.disk.ctx):0;
}
static int sector_io(unsigned drive,unsigned function,DosSectorIo *io) {
    if(!io || io->size<sizeof(*io) || io->flags) return DE_FUNCTION;
    io->done=0;
    if(function==0x61 || function==0x41) return raw_transfer(drive,io->sector,io->count,io->buffer,function==0x41,&io->done);
    u8 sector[512]; memset(sector,0,sizeof(sector));
    for(;io->done<io->count;io->done++) {
        u32 n; int e=raw_transfer(drive,io->sector+io->done,1,sector,function==0x42,&n);
        if(e) return e;
    }
    return 0;
}
static int block_ioctl(DosRegs *r,unsigned drive,unsigned function) {
    if((r->cx>>8)!=8) return DE_FUNCTION;
    void *buffer=(void *)(uintptr_t)r->dx;
    switch(function) {
    case 0x60: return device_params(drive,buffer);
    case 0x40: return set_device_params(drive,buffer);
    case 0x61: case 0x41: case 0x62: case 0x42: return sector_io(drive,function,buffer);
    case 0x66: case 0x46: return media_id(drive,buffer,function==0x46);
    default: return DE_FUNCTION;
    }
}
static char machine_name[16]="               ";
static u64 machine_number;
static u8 switch_char='/';
static void maintenance_reset(void) {
    memcpy(machine_name,"               ",16); machine_number=0; switch_char='/';
    share_retries=3; share_delay=1;
}
/* A drive number (0 the current one, 1 A:) as the local drive it means. */
static unsigned drive_argument(u64 value,unsigned *drive) {
    if(value>DOS_DRIVES) return DE_DRIVE;
    int e=physical_drive(value?(unsigned)value-1:current()->drive,1,drive); if(e) return e;
    return drives[*drive].volume?0:DE_DRIVE;
}
static int close_open_file(OpenFile *file,u32 only_pid) {
    int error=0;
    for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid && (!only_pid || process[i].pid==only_pid))
        for(unsigned h=0;h<process[i].handle_count;h++)
            if(process[i].handles[h]>=0 && sft[process[i].handles[h]].file && (!file || sft[process[i].handles[h]].file==file)) {
                int e=release_handle(&process[i],h); if(e && !error) error=dos_preserve_error(e);
            }
    return error;
}
static int server_call(DosRegs *r,unsigned sub) {
    DosServerCall *call=(DosServerCall *)(uintptr_t)r->dx;
    switch(sub) {
    case 1: return dos_flush();
    case 2: {
        if(!call) return DE_FUNCTION;
        char path[DOS_PATH_MAX]; unsigned drive; Fat *f; Node n;
        int e=volume_path((const char *)(uintptr_t)call->regs.dx,path,&drive,&f); if(e) return e;
        e=fat_lookup(f,path,&n); if(e) return e;
        for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) {
            OpenFile *file=&open_files[i];
            if(file->refs && file->drive==drive && file->generation==drives[drive].generation && same_node(&file->node,&n)) {
                e=dos_flush(); if(e) return e;
                return close_open_file(file,0);
            }
        }
        return DE_NOFILE;
    }
    case 3: return 0; /* No remote users exist on this local system. */
    case 4: {
        if(!call || !call->pid || call->pid>UINT32_MAX || task_index((u32)call->pid)<0) return DE_BLOCK;
        int e=dos_flush(); return e?e:close_open_file(NULL,(u32)call->pid);
    }
    case 5: {
        u64 index=r->bx;
        for(unsigned i=0;i<ARRAY_SIZE(open_files);i++) if(open_files[i].refs && !index--) {
            unsigned held=0;
            for(unsigned j=0;j<ARRAY_SIZE(locks);j++) if(locks[j].owner && locks[j].owner->file==&open_files[i]) held++;
            r->di=(uintptr_t)open_files[i].path; r->bx=0; r->cx=held; return 0;
        }
        return DE_NOMORE;
    }
    case 0x0a: {
        const DosExtendedError *error=(const DosExtendedError *)(uintptr_t)r->dx;
        if(!error) return DE_FUNCTION;
        current()->error=*error; return 0;
    }
    default: return DE_FUNCTION; /* Server swap areas and network spooling. */
    }
}
static void done(DosRegs *r,int e) {
    r->flags&=~1ULL; if(e) {r->flags|=1; r->ax=e;}
}
static void int21(DosRegs *r);
/* --- APPEND ------------------------------------------------------------ */
int dos_append(const DosAppend *set,DosAppend *previous) {
    if(in_dos) return DE_BUSY;
    if((set && set->size<sizeof(*set)) || (previous && previous->size<sizeof(*previous))) return DE_FUNCTION;
    if(set && strlen(set->list)>=DOS_PATH_MAX) return DE_FUNCTION;
    api_begin(); in_dos++;
    if(previous) {previous->flags=appended.flags; strcopy(previous->list,DOS_PATH_MAX,appended.list);}
    if(set) {appended.flags=(set->flags&(DOS_APPEND_ENABLED|DOS_APPEND_DRIVE|DOS_APPEND_PATH|DOS_APPEND_ENV|DOS_APPEND_X))|DOS_APPEND_INSTALLED; strcopy(appended.list,DOS_PATH_MAX,set->list);}
    in_dos--; return api_result(0);
}
int dos_installed(u32 program,const u32 *set,u32 *previous) {
    if(program>=DOS_INSTALLED_PROGRAMS) return DE_FUNCTION;
    if(program==DOS_INSTALLED_ANSI) {
        u32 now=ansi_options();
        if(previous) *previous=now;
        if(!set) return 0;
        if(now) { /* A second ANSI.SYS loads too, changing nothing else. */
            if(device_loading()) device_configure();
            ansi_set_options((now&~DOS_ANSI_X)|(*set&DOS_ANSI_X)); return 0;
        }
        if(!(*set&DOS_ANSI_ON) || (*set&~(DOS_ANSI_ON|DOS_ANSI_X|DOS_ANSI_K|DOS_ANSI_L))) return DE_FUNCTION;
        if(!platform_text_available()) return DE_FUNCTION;
        int e=device_configure(); if(e) return e;
        ansi_pending=*set; return 0;
    }
    if(previous) *previous=installed_values[program];
    if(set && (program==DOS_INSTALLED_XMS || program==DOS_INSTALLED_EMS)) {
        int e=device_configure(); if(e) return e;
    }
    if(set) installed_values[program]=*set;
    return 0;
}
/* --- PRINT ------------------------------------------------------------------ */
static struct {
    DosExtendedError error; int terminal_error,critical_failed,critical_pending,operation_device;
    unsigned operation_drive,operation_generation,suppress;
} spool_saved;
int dos_spool_begin(void) {
    if(spooling) return DE_BUSY;
    Process *p=current(); spooling=1;
    spool_saved.error=p->error; spool_saved.terminal_error=p->terminal_error;
    spool_saved.critical_failed=p->critical_failed; spool_saved.critical_pending=p->critical_pending;
    spool_saved.operation_device=p->operation_device; spool_saved.operation_drive=p->operation_drive;
    spool_saved.operation_generation=p->operation_generation; spool_saved.suppress=critical_suppress;
    critical_suppress|=DOS_OPEN_FAIL_ERRORS; in_dos++; return 0;
}
void dos_spool_end(void) {
    Process *p=current();
    p->error=spool_saved.error; p->terminal_error=spool_saved.terminal_error;
    p->critical_failed=spool_saved.critical_failed; p->critical_pending=spool_saved.critical_pending;
    p->operation_device=spool_saved.operation_device; p->operation_drive=spool_saved.operation_drive;
    p->operation_generation=spool_saved.operation_generation; critical_suppress=spool_saved.suppress;
    in_dos--; spooling=0;
}
int dos_spool_open(const char *path,int device,unsigned *reference) {
    unsigned result;
    return open_file(path,(device?DOS_OPEN_WRITE:DOS_OPEN_READ|0x40)|DOS_OPEN_FAIL_ERRORS,0,1,reference,&result,0);
}
int dos_spool_read(unsigned reference,void *buffer,u32 count,u32 *done) {
    *done=0; return read_file(&sft[reference],buffer,count,done);
}
int dos_spool_write(unsigned reference,const void *buffer,u32 count,u32 *done) {
    Sft *s=&sft[reference]; *done=0;
    if(!s->device) return DE_ACCESS;
    DosDeviceRequest r; int e=device_transfer(s,DOS_DEV_WRITE,(void *)buffer,count,0,&r);
    *done=r.transferred; return e;
}
void dos_spool_close(unsigned reference) {release_file(&sft[reference],dos_pid());}
int dos_print(u32 function,DosPrintRequest *request) {
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; int e=print_request(function,request); in_dos--; return api_result(e);
}
/* A program that waits without DOS (VDM's INT 16h and 28h, a message loop):
 * PRINT's turn. */
u32 dos_idle(void) {return in_dos?0:print_idle();}
int dos_keyb(u32 function,DosKeybRequest *request) {
    if(in_dos) return DE_BUSY;
    api_begin(); in_dos++; int e=keyb_request(function,request); in_dos--; return api_result(e);
}
u32 dos_keyb_key(const IoEvent *event,u16 *keys) {return keyb_key(event,keys);}
int dos_append_task(u32 mask,u32 *previous) {
    if(in_dos) return DE_BUSY;
    if(previous) *previous=current()->append_mask;
    current()->append_mask=mask; return 0;
}
static int appendable(unsigned fn,unsigned sub,const DosRegs *r,u32 flags) {
    switch(fn) {
    case 0x0f: case 0x23: case 0x3d: return 1;
    case 0x6c: return !(r->dx&0xf0);
    case 0x11: case 0x4e: return (flags&DOS_APPEND_X)!=0;
    case 0x4b: return (sub==0 || sub==3) && (flags&DOS_APPEND_X);
    default: return 0;
    }
}
static int not_found(unsigned fn,const DosRegs *r) {
    if(fn==0x0f || fn==0x23 || fn==0x11) return (r->ax&255)==255;
    return (r->flags&1) && (r->ax==DE_NOFILE || ((fn==0x4e) && r->ax==DE_NOMORE));
}
/* A not-found open, search or EXEC tried again in each APPEND directory;
 * the first result that is not "not found" stands, else the first one. */
static void append_call(unsigned fn,DosRegs *r,u32 flags) {
    DosRegs first=*r; char list[DOS_PATH_MAX],leaf[13],candidate[DOS_PATH_MAX];
    int21(r);
    if(!not_found(fn,r)) return;
    if(flags&DOS_APPEND_ENV) {
        size_t length=6; unsigned pos=env_offset("APPEND",length);
        if(pos==current()->env_used || env_copy(current()->environment+pos+length+1,list,sizeof(list))) return;
    } else strcopy(list,sizeof(list),appended.list);
    int fcb=fn==0x0f || fn==0x23 || fn==0x11;
    u8 *block=fcb?(u8 *)(uintptr_t)first.dx:NULL;
    if(fcb) {
        if(block[0]==0xff) block+=7;
        if(block[0] && !(flags&DOS_APPEND_DRIVE)) return;
    } else {
        const char *name=(const char *)(uintptr_t)(fn==0x6c?first.si:first.dx),*last=name;
        if(!name) return;
        if(name[0] && name[1]==':' && !(flags&DOS_APPEND_DRIVE)) return;
        for(const char *p=name;*p;p++) if(*p=='\\' || *p=='/' || (p==name+1 && *p==':')) last=p+1;
        if(last!=name && last!=name+2 && !(flags&DOS_APPEND_PATH)) return;
        if(strcopy(leaf,sizeof(leaf),last)) return;
    }
    for(char *entry=list;*entry;) {
        char *end=entry; while(*end && *end!=';') end++;
        char saved=*end; *end=0;
        DosRegs again=first; int tried=0;
        if(*entry) {
            if(fcb) {
                /* The FCB's drive and that drive's directory, for the call. */
                char where[DOS_PATH_MAX]; unsigned letter=(unsigned)(upper(entry[0])-'A');
                if(entry[1]==':' && letter<DOS_DRIVES && !dos_full_path(entry,where)) {
                    char kept[DOS_PATH_MAX]; u8 drive_byte=block[0];
                    strcopy(kept,sizeof(kept),current()->cwd[letter]); strcopy(current()->cwd[letter],DOS_PATH_MAX,where+2);
                    block[0]=(u8)(letter+1); raw_letters++; int21(&again); raw_letters--; tried=1;
                    strcopy(current()->cwd[letter],DOS_PATH_MAX,kept);
                    if(not_found(fn,&again)) block[0]=drive_byte;
                }
            } else if(!strcopy(candidate,sizeof(candidate),entry) &&
                      (end[-1]=='\\' || end[-1]==':' || !strappend(candidate,sizeof(candidate),"\\")) &&
                      !strappend(candidate,sizeof(candidate),leaf)) {
                if(fn==0x6c) again.si=(uintptr_t)candidate; else again.dx=(uintptr_t)candidate;
                int21(&again); tried=1;
                again.dx=first.dx; again.si=first.si;
            }
        }
        *end=saved;
        if(tried && !not_found(fn,&again)) {*r=again; return;}
        entry=*end?end+1:end;
    }
}
void dos_int21(DosRegs *r) {
    unsigned fn=(r->ax>>8)&255,sub=r->ax&255; u32 flags=appended.flags&current()->append_mask;
    if(!in_dos && (flags&DOS_APPEND_ENABLED) && appendable(fn,sub,r,flags)) append_call(fn,r,flags);
    else int21(r);
}
static void int21(DosRegs *r) {
    if(!in_dos && r->ax==0x5d00) {
        /* Server DOS call: run the listed registers as this task's call. */
        const DosServerCall *call=(const DosServerCall *)(uintptr_t)r->dx;
        if(call && call->regs.ax>>8!=0x5d) {u64 flags=r->flags; *r=call->regs; r->flags=flags; dos_int21(r); return;}
        r->flags|=1; r->ax=DE_FUNCTION; return;
    }
    if(in_dos) {
        unsigned fn=(r->ax>>8)&255;
        if(fn==0x27 || fn==0x28 || fn==0x3f || fn==0x40 || (fn==0x44 && ((r->ax&255)==2 || (r->ax&255)==3))) r->cx=0;
        done(r,DE_BUSY); return;
    }
    api_begin(); in_dos++;
    unsigned saved_suppress=critical_suppress;
    unsigned fn=(r->ax>>8)&255, sub=r->ax&255; int e=0,fcb_result=0; u32 n=0; unsigned h=0;
    const char *path=(const char *)(uintptr_t)r->dx;
    /* Native registers are wider than DOS handle numbers. Reject overflow
     * before any narrowing conversion can select an unrelated live handle. */
    if((fn>=0x3e && fn<=0x40) || fn==0x42 || (fn==0x44 && (sub<=3 || sub==6 || sub==7 || sub==0x0a || sub==0x0c)) || fn==0x45 || fn==0x46 ||
       fn==0x57 || fn==0x5c || fn==0x68) {
        if(r->bx>=DOS_MAX_HANDLES || (fn==0x46 && r->cx>=DOS_MAX_HANDLES)) {
            if(fn==0x3f || fn==0x40) r->cx=0;
            e=DE_HANDLE; goto complete;
        }
        Sft *s; if(!slot(r->bx,&s)) critical_suppress|=s->mode&DOS_OPEN_FAIL_ERRORS;
    }
    if((current()->break_check && fn>0x0c && fn!=0x33 && fn!=0x4c && fn!=0x4d) || (fn>=2 && fn<=5) || fn==9) {
        e=check_break(); if(e) {
            if(fn==0x27 || fn==0x28 || fn==0x3f || fn==0x40) r->cx=0;
            goto complete;
        }
    }
dispatch:
    switch(fn) {
    case 0x01: case 0x07: case 0x08: {
        u8 c; e=read_char(0,1,0,fn!=7,&c);
        if(!e) {r->ax=c; if(fn==1) e=dos_write(1,&c,1,&n);}
        break;
    }
    case 0x06: {
        u8 c=r->dx;
        if(c==255) {
            e=read_char(0,0,0,0,&c);
            if(e==DE_NOTREADY || e==DE_EOF) {r->flags|=0x40; r->ax=0; e=0;}
            else if(!e) {r->flags&=~0x40ULL; r->ax=c;}
        } else {
            Sft *s; e=slot(1,&s);
            if(!e && !(s->mode&3)) e=DE_ACCESS;
            if(!e) e=s->device==1?device_output(&c,1,1,&n):dos_write(1,&c,1,&n);
            r->ax=c;
        }
        break;
    }
    case 0x02: {u8 c=r->dx; e=dos_write(1,&c,1,&n); r->ax=c==9?' ':c; break;}
    case 0x03: {u8 c; e=read_char(3,1,0,0,&c); if(!e) r->ax=c; break;}
    case 0x04: case 0x05: {u8 c=r->dx; e=dos_write(fn==4?3:4,&c,1,&n); break;}
    case 0x09: {
        const char *p=path; while(*p!='$') p++; e=dos_write(1,path,p-path,&n); r->ax=0x24; break;
    }
    case 0x0a: e=line_input(0,(u8 *)(uintptr_t)r->dx); break;
    case 0x0b: {
        u8 c; e=read_char(0,0,1,1,&c);
        r->ax=e==DE_NOTREADY || e==DE_EOF?0:255;
        if(e==DE_NOTREADY || e==DE_EOF) e=0;
        break;
    }
    case 0x0c: {
        Sft *s; e=slot(0,&s); if(e) break;
        if(s->device) {
            e=device_transfer(s,DOS_DEV_INPUT_FLUSH,NULL,0,0,NULL); if(e) break;
            reset_line_input(s->device);
        }
        if(sub==1 || sub==6 || sub==7 || sub==8 || sub==0x0a) {fn=sub; goto dispatch;}
        break;
    }
    case 0x0d: e=dos_flush(); break;
    case 0x0e:
        e=r->dx>=DOS_DRIVES?DE_DRIVE:dos_select_drive(r->dx);
        if(!e) {unsigned high=DOS_DRIVES; while(high && !letter_usable(high-1)) high--; r->ax=high;} break;
    case 0x19: r->ax=current()->drive; break;
    case 0x1b: case 0x1c: case 0x1f: case 0x32: {
        unsigned drive; u64 wanted=fn==0x1b || fn==0x1f?0:r->dx;
        if(drive_argument(wanted,&drive) || drive_dpb(drive)) {r->ax=0xff; break;}
        if(fn==0x1f || fn==0x32) {r->ax=0; r->bx=(uintptr_t)&dpbs[drive]; break;}
        r->ax=dpbs[drive].sectors_per_cluster; r->cx=512; r->dx=dpbs[drive].clusters; r->bx=(uintptr_t)&media_bytes[drive];
        break;
    }
    case 0x34: r->bx=(uintptr_t)&in_dos; break;
    case 0x37:
        if(!sub) r->dx=(r->dx&~255ULL)|switch_char;
        else if(sub==1) switch_char=(u8)r->dx;
        else if(sub==2) r->dx=(r->dx&~255ULL)|255; /* A DEV directory prefix is optional. */
        else if(sub!=3) {r->ax|=255; break;}
        r->ax&=~255ULL; break;
    case 0x50: {int i=task_index(r->bx>UINT32_MAX?0:(u32)r->bx); if(i<0) e=DE_BLOCK; else current_slot=(unsigned)i; break;}
    case 0x51: case 0x62: r->bx=dos_pid(); break;
    case 0x5d: e=server_call(r,sub); break;
    case 0x5e:
        if(!sub) {
            char *out=(char *)(uintptr_t)r->dx; if(!out) {e=DE_FUNCTION; break;}
            memcpy(out,machine_name,15); out[15]=0; r->cx=machine_number;
        } else if(sub==1) {
            const char *in=(const char *)(uintptr_t)r->dx; if(!in || r->cx>65535) {e=DE_FUNCTION; break;}
            unsigned n=0; while(n<15 && in[n]) {machine_name[n]=in[n]; n++;}
            while(n<15) machine_name[n++]=' ';
            machine_number=r->cx;
        } else e=DE_FUNCTION; /* Printer setup belongs to a network redirector. */
        break;
    case 0x5f:
        if(sub==7 || sub==8) {
            unsigned drive=(unsigned)(r->dx&255);
            if(r->dx>=DOS_DRIVES || !drive_exists(drive)) {e=DE_DRIVE; break;}
            if(sub==8) for(unsigned i=0;i<DOS_PROCESSES;i++) if(process[i].pid && process[i].drive==drive) e=DE_CURRENT;
            if(!e) drives[drive].disabled=sub==8;
        } else e=DE_FUNCTION; /* No network redirector is installed. */
        break;
    case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x21: case 0x22: case 0x23: case 0x24: case 0x27: case 0x28: case 0x29:
        fcb_result=1; e=fcb_call(r,current()->dta,current()->dta_size); break;
    case 0x1a: {
        u64 size=sub?r->cx:128;
        if(sub>1 || size>UINT32_MAX || (size && (!r->dx || r->dx>UINTPTR_MAX-size))) e=DE_FUNCTION;
        else {current()->dta=(void *)(uintptr_t)r->dx; current()->dta_size=size;}
        break;
    }
    case 0x2f: r->bx=(uintptr_t)current()->dta; r->cx=current()->dta_size; break;
    case 0x2a: case 0x2c: {
        DosDateTime t; e=read_datetime(&t);
        if(!e && fn==0x2a) {r->cx=t.year; r->dx=(t.month<<8)|t.day; r->ax=t.weekday;}
        else if(!e) {r->cx=(t.hour<<8)|t.minute; r->dx=(t.second<<8)|t.hundredth; r->ax=0;}
        break;
    }
    case 0x2b: case 0x2d: {
        r->ax=255;
        if(r->cx>65535 || r->dx>65535) break;
        IoDateTime t={0}; unsigned fields;
        if(fn==0x2b) {
            t.year=r->cx; t.month=r->dx>>8; t.day=r->dx&255;
            if(t.year<1980 || t.year>2099 || !calendar_date(t.year,t.month,t.day)) break;
            fields=IO_CLOCK_DATE;
        } else {
            t.hour=r->cx>>8; t.minute=r->cx&255; t.second=r->dx>>8;
            if((r->dx&255)>99) break;
            t.nanosecond=(r->dx&255)*10000000;
            if(!calendar_time(t.hour,t.minute,t.second,t.nanosecond)) break;
            fields=IO_CLOCK_TIME;
        }
        current()->operation_device=1; e=dos_clock_write(&t,fields);
        if(!e) r->ax=0;
        break;
    }
    case 0x30: r->ax=4; r->bx=0; r->cx=0; break;
    case 0x33:
        if(!sub) r->dx=current()->break_check;
        else if((sub==1 || sub==2) && r->dx<=1) {
            unsigned old=current()->break_check; dos_set_break_check(r->dx); if(sub==2) r->dx=old;
        } else if(sub==5) r->dx=3;
        else e=DE_FUNCTION;
        break;
    case 0x36: {
        unsigned drive=DOS_DRIVES; Fat *f;
        if(r->dx<=DOS_DRIVES && physical_drive(r->dx?(unsigned)r->dx-1:current()->drive,1,&drive)) drive=DOS_DRIVES;
        if(drive<DOS_DRIVES && drives[drive].redir) {
            DosDriveInfo info; e=get_drive_info(drive,&info);
            if(e) {r->ax=0xffff; e=0; break;}
            r->ax=info.sectors_per_cluster; r->bx=info.free_clusters; r->cx=512; r->dx=info.total_clusters; break;
        }
        if(drive>=DOS_DRIVES || (e=ready_drive(drive,&f))) {r->ax=0xffff; if(e==DE_DRIVE) e=0; break;}
        e=fat_free_space(f,&n); r->ax=f->spc; r->bx=n; r->cx=512; r->dx=f->clusters; break;
    }
    case 0x38: case 0x63: case 0x65: case 0x66: e=nls_dispatch(r); break;
    case 0x39: e=dos_mkdir(path); break;
    case 0x3a: e=dos_remove(path,1); break;
    case 0x3b: e=dos_chdir(path); break;
    case 0x3c: case 0x5b:
        if(r->cx&~0x27ULL) {e=DE_ACCESS; break;}
        e=dos_open_ex(path,2,r->cx,fn==0x5b?0x10:0x12,&h,&n); r->ax=h; break;
    case 0x3d: e=dos_open(path,sub,0,&h); r->ax=h; break;
    case 0x3e: e=dos_close(r->bx); break;
    case 0x3f: e=r->cx>UINT32_MAX?DE_FUNCTION:dos_read(r->bx,(void *)(uintptr_t)r->dx,(u32)r->cx,&n); r->ax=n; r->cx=n; break;
    case 0x40: e=r->cx>UINT32_MAX?DE_FUNCTION:dos_write(r->bx,path,(u32)r->cx,&n); r->ax=n; r->cx=n; break;
    case 0x41: e=dos_remove(path,0); break;
    case 0x42: e=dos_seek(r->bx,(i64)r->dx,sub,&n); r->ax=n; break;
    case 0x43: {u8 attr=r->cx; if(sub>1) e=DE_FUNCTION; else {e=dos_attribute(path,sub,&attr); r->cx=attr;} break;}
    case 0x44:
        if(!sub) {Sft *s; e=slot(r->bx,&s); if(!e) {e=s->device?0:ready_file(s->file); if(!e) r->dx=s->device?(device_spec(s->device)->attributes&0xc000)|s->device_flags:s->file->drive;}}
        else if(sub==1) {
            Sft *s; e=slot(r->bx,&s);
            if(!e) {
                if(!s->device || r->dx>255) e=DE_FUNCTION;
                else {s->device_flags=(u8)r->dx|128; s->line_pos=s->line_used=0;}
            }
        } else if(sub==2 || sub==3 || sub==0x0c) {
            Sft *s; e=slot(r->bx,&s); if(e) break;
            const DosDeviceSpec *dev=s->device?device_spec(s->device):NULL;
            if(!dev || !(dev->attributes&(sub==0x0c?DOS_DEVICE_GENERIC:DOS_DEVICE_IOCTL))) {e=DE_FUNCTION; break;}
            /* Control writes need a handle open for writing; control reads
             * (printer status) work on write-only handles too. */
            if(sub==3 && !(s->mode&3)) {e=DE_ACCESS; break;}
            u64 count=sub==0x0c?r->si:r->cx; if(count>UINT32_MAX || (sub==0x0c && r->cx>65535)) {e=DE_FUNCTION; break;}
            DosDeviceRequest request={.size=sizeof(request),.command=sub==2?DOS_DEV_IOCTL_READ:sub==3?DOS_DEV_IOCTL_WRITE:DOS_DEV_GENERIC_IOCTL,
                .pid=dos_pid(),.mode=s->mode,.cookie=s->device_cookie,.buffer=(void *)(uintptr_t)r->dx,.count=count,
                .control=sub==0x0c?r->cx:0,.argument=r->di};
            e=device_request(s->device,&request); r->ax=request.transferred;
            if(sub!=0x0c) n=request.transferred;
        } else if(sub==0x0a) {
            /* Same word as 4400h; bit 15 marks a file on a redirected drive. */
            Sft *s; e=slot(r->bx,&s); if(!e) {e=s->device?0:ready_file(s->file); if(!e) r->dx=((s->device?(device_spec(s->device)->attributes&0x4000)|s->device_flags:s->file->drive)&0x7fff)|(!s->device && s->file->remote?0x8000:0);}
        } else if(sub==6 || sub==7) {
            Sft *s; e=slot(r->bx,&s); if(e) break;
            if(s->device>2 || (s->device && sub==7)) {
                if(sub==6 && !(s->device_flags&32) && !(s->device_flags&64)) r->ax=0;
                else if(sub==6 && s->line_pos<s->line_used) r->ax=255;
                else {DosDeviceRequest request; e=device_transfer(s,sub==6?DOS_DEV_INPUT_STATUS:DOS_DEV_OUTPUT_STATUS,NULL,0,0,&request); r->ax=request.ready?255:0;}
            } else if(sub==7) {e=ready_file(s->file); if(!e) r->ax=255;}
            else if(s->device==2 || (s->device==1 && !(s->device_flags&32) && !(s->device_flags&64))) r->ax=0;
            else if(s->device==1 && s->line_pos<s->line_used) r->ax=255;
            else {
                u8 c; e=read_char(r->bx,0,1,0,&c); r->ax=e?0:255;
                if(e==DE_NOTREADY || e==DE_EOF) e=0;
            }
        }
        else if(sub==8 || sub==9) {
            unsigned letter=r->bx?(unsigned)r->bx-1:current()->drive,drive=0;
            e=r->bx>DOS_DRIVES?DE_DRIVE:physical_drive(letter,1,&drive);
            if(!e) e=poll_drive(drive);
            /* 4409h: generic IOCTL, 32-bit sectors, IOCTL read/write for drivers;
             * bit 15 marks a SUBST letter. */
            if(!e && drives[drive].redir) {if(sub==8) e=DE_REMOTE; else r->dx=0x1000;}
            else if(!e) {if(sub==8) r->ax=!(drives[drive].media.flags&IO_DISK_REMOVABLE); else r->dx=0x42|(drives[drive].driver?0x4000:0);}
            if(!e && sub==9 && drive_maps[assigned(letter)].kind==DOS_MAP_SUBST) r->dx|=0x8000;
        } else if(sub==4 || sub==5) {
            unsigned drive; e=drive_argument(r->bx,&drive);
            if(!e && r->cx>UINT32_MAX) e=DE_FUNCTION;
            if(!e) e=poll_drive_once(drive);
            if(!e) e=drives[drive].driver?block_ioctl_transfer(drives[drive].unit,sub==5,(void *)(uintptr_t)r->dx,(u32)r->cx,&n):DE_FUNCTION;
            r->ax=n;
        } else if(sub==0x0b) {
            if(r->dx>65535 || r->cx>65535) e=DE_FUNCTION; else {share_retries=r->dx; share_delay=r->cx;}
        } else if(sub==0x0d) {
            unsigned drive; e=drive_argument(r->bx,&drive);
            if(!e) e=block_ioctl(r,drive,(unsigned)(r->cx&255));
        } else if(sub==0x0e || sub==0x0f) {
            unsigned drive; e=drive_argument(r->bx,&drive);
            if(!e) r->ax=0; /* Each unit has exactly one drive letter. */
        } else e=DE_FUNCTION;
        break;
    case 0x45: e=dos_dup(r->bx,&h); r->ax=h; break;
    case 0x46: e=dos_dup2(r->bx,r->cx); break;
    case 0x47: {
        char cwd[DOS_PATH_MAX]; e=r->dx>DOS_DRIVES?DE_DRIVE:dos_drive_cwd(r->dx?r->dx-1:current()->drive,cwd);
        if(!e) e=strcopy((char *)(uintptr_t)r->si,DOS_PATH_MAX,cwd+1);
        break;
    }
    case 0x48: {void *p; dos_arena.method=current()->allocation_method; e=arena_alloc(&dos_arena,r->bx,dos_pid(),&p,&n); r->ax=(uintptr_t)p; if(e) r->bx=n; break;}
    case 0x49: e=arena_free(&dos_arena,(void *)(uintptr_t)r->dx,dos_pid()); break;
    case 0x4a: e=arena_resize(&dos_arena,(void *)(uintptr_t)r->dx,r->bx,dos_pid(),&n); if(e) r->bx=n; break;
    case 0x4b:
        if(!sub) e=exec_file(path,(const char *)(uintptr_t)r->bx);
        else if(sub==1) e=exec_load(path,(DosExecLoad *)(uintptr_t)r->bx);
        else if(sub==3) e=load_overlay(path,(DosOverlay *)(uintptr_t)r->bx);
        else if(sub==0x80 || sub==0x81) e=exec_loaded(r->bx,sub==0x80);
        else e=DE_FUNCTION;
        break;
    case 0x31:
        /* Terminate and stay resident: the image and the task's memory blocks
         * remain; handles close. Only subsystem-11 images can stay loaded. */
        if(current()->running) {
            current()->exit_kind=DOS_EXIT_NORMAL; current()->resident=1;
            in_dos--; e=platform_exit_resident(sub,&current()->resident_token); in_dos++;
            current()->resident=0; if(!e) e=DE_FUNCTION;
        } else e=DE_FUNCTION;
        break;
    case 0x4c:
        if(current()->running) {current()->exit_kind=DOS_EXIT_NORMAL; in_dos--; platform_exit(sub); in_dos++;}
        e=DE_FUNCTION; break;
    case 0x4d: r->ax=dos_errorlevel|(current()->return_kind<<8); dos_errorlevel=0; current()->return_kind=0; break;
    case 0x4e: case 0x4f: {
        if(current()->dta_size<sizeof(DosFind)) {e=DE_NOMEM; break;}
        DosFind find; memcpy(&find,current()->dta,sizeof(find));
        e=fn==0x4e?dos_find_first(path,r->cx,&find):dos_find_next(&find);
        memcpy(current()->dta,&find,sizeof(find)); break;
    }
    case 0x2e: fat_verify_writes=sub&1; break;
    case 0x54: r->ax=fat_verify_writes; break;
    case 0x5a: e=r->cx>0xffff?DE_ACCESS:create_temp((char *)(uintptr_t)r->dx,(unsigned)r->cx,&h); if(!e) r->ax=h; break;
    case 0x56: e=dos_rename(path,(const char *)(uintptr_t)r->di); break;
    case 0x57: {
        u16 date=r->dx,time=r->cx;
        e=sub>1?DE_FUNCTION:dos_file_time(r->bx,sub,&date,&time);
        if(!e) {r->dx=date; r->cx=time;} break;
    }
    case 0x58: if(!sub) r->ax=current()->allocation_method; else if(sub==1 && r->bx<=2) current()->allocation_method=r->bx; else e=DE_FUNCTION; break;
    case 0x59:
        if(r->bx) e=DE_FUNCTION;
        else {r->ax=last_error; r->bx=(current()->error.error_class<<8)|current()->error.action; r->cx=current()->error.locus<<8;}
        break;
    case 0x5c: e=(r->dx>UINT32_MAX || r->cx>UINT32_MAX)?DE_LOCK:dos_lock(r->bx,sub,r->dx,r->cx); break;
    case 0x60: e=dos_canonical((const char *)(uintptr_t)r->si,(char *)(uintptr_t)r->di); break;
    case 0x67: e=r->bx==65535?DE_FUNCTION:r->bx>DOS_MAX_HANDLES?DE_NOMEM:dos_set_handle_count((unsigned)r->bx); break;
    case 0x68: {Sft *s; e=slot(r->bx,&s); if(!e) e=flush_file(s); break;}
    case 0x6c:
        if(sub || r->bx>65535 || r->dx>65535) {e=DE_FUNCTION; break;}
        if(r->cx&~0x27ULL) {e=DE_ACCESS; break;}
        e=dos_open_ex((const char *)(uintptr_t)r->si,r->bx,r->cx,r->dx,&h,&n);
        if(!e) {r->ax=h; r->cx=n;} break;
    default: e=DE_FUNCTION;
    }
complete:
    if(fn==0x44 && (sub==2 || sub==3)) r->cx=n;
    critical_suppress=saved_suppress; in_dos--;
    e=api_result(e);
    if(fcb_result) {r->bx=e; r->flags&=~1ULL;}
    else done(r,e);
}
