/* SPDX-License-Identifier: GPL-2.0-or-later
 * EFICD.SYS: the CD-ROM drives IO.SYS finds, as the DOS character device
 * MSCD001 (include/dos_cdrom.h). DEVICE=C:\DOS\EFICD.SYS [/D:MSCD001]
 * Requests use only IO.SYS, so they are safe from inside DOS.
 */
#include "../apps/runtime.h"
#include "dos_cdrom.h"
static const IoServices *io;
#define HAS(member) (io->size>=offsetof(IoServices,member)+sizeof(io->member) && io->member)
static int request(void *context,DosDeviceRequest *r);
/* Only /D: naming this device is accepted; the name is fixed when it registers. */
static int parse(const char *tail) {
    while(*tail) {
        while(*tail==' ' || *tail=='\t') tail++;
        if(!*tail) break;
        if(*tail!='/' || upper(tail[1])!='D' || tail[2]!=':') return DE_FUNCTION;
        tail+=3;
        const char *name="MSCD001";
        while(*name && upper(*tail)==*name) {tail++; name++;}
        if(*name || (*tail && *tail!=' ' && *tail!='\t')) return DE_FUNCTION;
    }
    return 0;
}
static int control(DosCdControl *c) {
    u32 units=io->cd_count(io->context);
    if(c->size<sizeof(*c)) return DE_FUNCTION;
    c->units=units; c->abi=DOS_CD_ABI;
    switch(c->code) {
    case DOS_CD_ENTRY: c->request=request; c->context=NULL; return 0;
    case DOS_CD_STATUS: {
        IoCdInfo info={.size=sizeof(info)};
        if(c->unit>=units) return DE_DRIVE;
        int e=io->cd_info(io->context,c->unit,&info); if(e) return e;
        c->generation=info.generation; c->flags=info.flags; c->sector_size=info.sector_size; c->sectors=info.sectors;
        return 0;
    }
    case DOS_CD_READ: {
        if(c->unit>=units) return DE_DRIVE;
        c->transferred=0;
        int e=io->cd_read(io->context,c->unit,c->generation,c->sector,c->count,c->buffer);
        if(!e) c->transferred=c->count;
        return e;
    }
    }
    return DE_FUNCTION;
}
static int request(void *context,DosDeviceRequest *r) {
    (void)context;
    switch(r->command) {
    case DOS_DEV_INIT:
        io=r->io;
        if(!io || io->version!=IO_ABI_VERSION || !HAS(cd_read) || !(io->capabilities&IO_CAP_CDROM)) return DE_FUNCTION;
        if(parse(r->arguments?r->arguments:"")) return DE_FUNCTION;
        return io->cd_count(io->context)?0:DE_NOTREADY;
    case DOS_DEV_FINISH: case DOS_DEV_OPEN: case DOS_DEV_CLOSE: return 0;
    case DOS_DEV_IOCTL_READ: {
        if(!r->buffer || r->count<sizeof(DosCdControl)) return DE_FUNCTION;
        int e=control((DosCdControl *)r->buffer);
        if(!e) r->transferred=sizeof(DosCdControl);
        return e;
    }
    case DOS_DEV_INPUT_STATUS: case DOS_DEV_OUTPUT_STATUS: r->ready=1; return 0;
    }
    return DE_FUNCTION;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    DosDeviceSpec spec={.version=DOS_DEVICE_ABI,.size=sizeof(spec),
        .attributes=DOS_DEVICE_CHAR|DOS_DEVICE_IOCTL|DOS_DEVICE_OPEN_CLOSE,.capabilities=DOS_DEVICE_CAN_READ,
        .context=NULL,.request=request};
    strcopy(spec.name,sizeof(spec.name),"MSCD001");
    return dos_device_register(&spec)?EFI_LOAD_ERROR:EFI_SUCCESS;
}
