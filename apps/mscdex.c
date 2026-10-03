/* SPDX-License-Identifier: GPL-2.0-or-later
 * MSCDEX: the CD-ROM file system.
 *   MSCDEX /D:MSCD001 [/L:letter]
 * Opens the CD device (EFICD.SYS), takes its request routine through
 * DOS_CD_ENTRY, and serves the disc's ISO 9660 file system as a read-only
 * redirected drive (include/dos_redir.h), by default the first letter after
 * the disk drives; then stays resident. Like MSCDEX, it calls the driver
 * directly, never DOS, while serving DOS. A changed disc is mounted again.
 */
#include "runtime.h"
#include "dos_cdrom.h"
#include "iso9660.h"
#define MSCDEX_GUID {0x5ca1ab1e,0x3cd0,0x4e5f,{0x9a,0x01,0x4d,0x53,0x43,0x44,0x45,0x58}}
#define DIRECTORIES 1024
static int (*driver)(void *,DosDeviceRequest *);
static void *driver_context;
static u32 unit,disc;
static int mounted;
static IsoVolume volume;
static struct {u32 sector,size;} directories[DIRECTORIES];
static unsigned directory_count;
static EFI_HANDLE self;
static EFI_BOOT_SERVICES *boot;
static u32 installed;

static int control(DosCdControl *c) {
    DosDeviceRequest r={.size=sizeof(r),.command=DOS_DEV_IOCTL_READ,.buffer=c,.count=sizeof(*c)};
    return driver(driver_context,&r);
}
static int read_sectors(void *context,u32 sector,u32 count,void *buffer) {
    (void)context;
    DosCdControl c={.size=sizeof(c),.code=DOS_CD_READ,.unit=unit,.generation=disc,.sector=sector,.count=count,.buffer=buffer};
    return control(&c);
}
static u16 directory_id(u32 sector,u32 size) {
    for(unsigned i=0;i<directory_count;i++) if(directories[i].sector==sector) return (u16)(i+1);
    if(directory_count==DIRECTORIES) return 0;
    directories[directory_count].sector=sector; directories[directory_count].size=size;
    return (u16)++directory_count;
}
/* The disc now in the drive, mounted on first use and after a change. */
static u32 generation(void *context) {
    (void)context;
    DosCdControl c={.size=sizeof(c),.code=DOS_CD_STATUS,.unit=unit};
    if(control(&c) || !(c.flags&IO_DISK_PRESENT)) {mounted=0; return 0;}
    if(!mounted || c.generation!=disc) {
        disc=c.generation; directory_count=0;
        mounted=!iso_mount(&volume,read_sectors,NULL);
        if(mounted) directory_id(volume.root_sector,volume.root_size);
    }
    return mounted?disc:0;
}
static void fill(const IsoEntry *e,DosRedirEntry *out) {
    memset(out,0,sizeof(*out));
    out->size=(e->attributes&FA_DIR)?0:e->size; out->date=e->date; out->time=e->time;
    out->attributes=e->attributes; strcopy(out->name,sizeof(out->name),e->name);
    if(e->attributes&FA_DIR) out->directory=directory_id(e->sector,e->size);
}
static int stat_path(void *context,const char *path,DosRedirEntry *out) {
    (void)context; IsoEntry e;
    if(!mounted) return DE_NOTREADY;
    int r=iso_lookup(&volume,path,&e); if(r) return r;
    fill(&e,out);
    if((e.attributes&FA_DIR) && !out->directory) return DE_NOMEM;
    return 0;
}
static int open_path(void *context,const char *path,u64 *file,DosRedirEntry *out) {
    int r=stat_path(context,path,out); if(r) return r;
    IsoEntry e; iso_lookup(&volume,path,&e);
    *file=((u64)e.sector<<32)|e.size;
    return 0;
}
static int read_file(void *context,u64 file,u32 offset,void *buffer,u32 count,u32 *done) {
    (void)context;
    if(!mounted) return DE_NOTREADY;
    IsoEntry e={.sector=(u32)(file>>32),.size=(u32)file};
    return iso_read(&volume,&e,offset,buffer,count,done);
}
static int close_file(void *context,u64 file) {(void)context; (void)file; return 0;}
/* index 0 is the start; the root then yields the label. Otherwise index-1 is
 * the byte offset of the next record. */
static int next(void *context,u16 directory,u32 *index,DosRedirEntry *out) {
    (void)context;
    if(!mounted) return DE_NOTREADY;
    if(!directory || directory>directory_count) return DE_PATH;
    if(!*index) {
        *index=1;
        if(directory==1 && volume.label[0]) {
            memset(out,0,sizeof(*out)); out->attributes=FA_VOLUME; strcopy(out->name,sizeof(out->name),volume.label);
            return 0;
        }
    }
    u32 offset=*index-1; IsoEntry e;
    int r=iso_next(&volume,directories[directory-1].sector,directories[directory-1].size,&offset,&e);
    *index=offset+1;
    if(r) return r;
    fill(&e,out);
    return 0;
}
static int space(void *context,u32 *clusters,u32 *free_clusters,u32 *bytes) {
    (void)context;
    if(!mounted) return DE_NOTREADY;
    *clusters=volume.volume_sectors; *free_clusters=0; *bytes=ISO_SECTOR; return 0;
}
static const DosRedirector redirector={sizeof(DosRedirector),0,NULL,stat_path,open_path,read_file,close_file,next,space,generation};

static int parse(const char *tail,char device[9],u32 *letter) {
    device[0]=0; *letter=UINT32_MAX;
    while(*tail) {
        while(*tail==' ' || *tail=='\t') tail++;
        if(!*tail) break;
        if(*tail!='/' || !tail[1] || tail[2]!=':') return DE_FUNCTION;
        char option=upper(tail[1]); tail+=3;
        if(option=='D') {
            unsigned n=0;
            while(*tail && *tail!=' ' && *tail!='\t') {if(n==8) return DE_FUNCTION; device[n++]=upper(*tail++);}
            device[n]=0; if(!n) return DE_FUNCTION;
        } else if(option=='L') {
            char c=upper(*tail++);
            if(c<'A' || c>'Z' || (*tail && *tail!=' ' && *tail!='\t')) return DE_FUNCTION;
            *letter=(u32)(c-'A');
        } else return DE_FUNCTION;
    }
    return device[0]?0:DE_FUNCTION;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    self=image; boot=st->BootServices;
    EFI_GUID guid=MSCDEX_GUID; void *existing; char device[9]; u32 drive;
    if(parse(app_dos->command_tail(),device,&drive)) {print("usage: MSCDEX /D:device [/L:letter]\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    if(!EFI_ERROR(boot->LocateProtocol(&guid,NULL,&existing))) {print("MSCDEX: already installed\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    unsigned h; int e=dos_open(device,DOS_OPEN_READ,0,&h);
    DosCdControl c={.size=sizeof(c),.code=DOS_CD_ENTRY};
    if(!e) {
        DosRegs r={.ax=0x4402,.bx=h,.cx=sizeof(c),.dx=(uintptr_t)&c};
        e=dos_call(&r);
        dos_close(h);
        if(!e && (c.abi!=DOS_CD_ABI || !c.request)) e=DE_FORMAT;
    }
    if(e) {print("MSCDEX: device %s is not a CD-ROM driver (%s)\n",device,dos_error(e)); dos_set_errorlevel(1); return EFI_SUCCESS;}
    driver=c.request; driver_context=c.context;
    if(!c.units) {print("MSCDEX: no CD-ROM drives\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    unit=0; generation(NULL);
    e=dos_redirect(&drive,&redirector);
    if(e) {print("MSCDEX: drive letter not available (%s)\n",dos_error(e)); dos_set_errorlevel(1); return EFI_SUCCESS;}
    installed=drive;
    if(EFI_ERROR(boot->InstallProtocolInterface(&self,&guid,EFI_NATIVE_INTERFACE,&installed))) {
        dos_unredirect(drive); print("MSCDEX: cannot register\n"); dos_set_errorlevel(1); return EFI_SUCCESS;
    }
    print("Drive %c: = Driver %s unit 0\n",'A'+drive,device);
    DosRegs r={.ax=0x3100}; e=dos_call(&r);
    /* Only refused residency returns. */
    boot->UninstallProtocolInterface(self,&guid,&installed); dos_unredirect(drive);
    print("MSCDEX: cannot stay resident (%s)\n",dos_error(e)); dos_set_errorlevel(1);
    return EFI_SUCCESS;
}
