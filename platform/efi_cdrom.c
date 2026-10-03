/* SPDX-License-Identifier: GPL-2.0-or-later
 * IO.SYS CD-ROM drives: firmware Block I/O devices with removable media and
 * 2048-byte blocks. Firmware may publish one drive through several handles
 * with the same device path (only one of them follows disc changes); the
 * first is used. A changed or removed disc makes reads fail with
 * EFI_MEDIA_CHANGED or EFI_NO_MEDIA until the drive is reset, so a failed
 * read, or a status query while no disc is present, resets the drive to look
 * again. Every change advances the drive's generation. As DOS assumes for
 * floppies without a change line, a disc is taken as unchanged for a while
 * after it was last read; a status query after that reads a sector to check.
 */
#include "efi_cdrom.h"
#define MAX_CD 4
#define CD_SECTOR 2048U
#define CHUNK 16U /* sectors per firmware read */
#define TRUST_MS 500U
typedef struct {
    EFI_BLOCK_IO *io; const u8 *path; unsigned path_bytes;
    u32 media_id,generation; int present; u64 sectors,checked_ms;
    void *allocation; u8 *bounce;
} Cd;
static Cd cds[MAX_CD];
static unsigned cd_units;
static EFI_BOOT_SERVICES *bs;
static const IoServices *services;
static u64 now_ms(void) {return services->ticks_ms?services->ticks_ms(services->context):0;}
static EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;

static void observe(Cd *c) {
    EFI_BLOCK_IO_MEDIA *m=c->io->Media;
    int present=m->MediaPresent && m->BlockSize==CD_SECTOR;
    u64 sectors=present?(u64)m->LastBlock+1:0;
    if(m->MediaId!=c->media_id || present!=c->present || sectors!=c->sectors) {
        c->generation++; c->media_id=m->MediaId; c->present=present; c->sectors=sectors;
    }
}
/* Look again after a change; the generation moves even if the firmware kept
 * its media ID, since the old disc is gone. */
static void reprobe(Cd *c,u32 failed) {
    c->io->Reset(c->io,FALSE);
    observe(c);
    if(c->generation==failed) c->generation++;
}
static u32 cd_count(void *ctx) {(void)ctx; return cd_units;}
static int cd_info(void *ctx,u32 unit,IoCdInfo *info) {
    (void)ctx;
    if(unit>=cd_units || !info) return DE_FUNCTION;
    Cd *c=&cds[unit]; observe(c);
    if(!c->present) {c->io->Reset(c->io,FALSE); observe(c); c->checked_ms=now_ms();}
    else if(now_ms()-c->checked_ms>=TRUST_MS) {
        EFI_STATUS s=c->io->ReadBlocks(c->io,c->media_id,0,CD_SECTOR,c->bounce);
        if(s==EFI_MEDIA_CHANGED || s==EFI_NO_MEDIA) reprobe(c,c->generation);
        c->checked_ms=now_ms();
    }
    *info=(IoCdInfo){sizeof(*info),c->generation,c->present?IO_DISK_PRESENT:0,CD_SECTOR,c->sectors};
    return 0;
}
static int cd_read(void *ctx,u32 unit,u32 generation,u64 sector,u32 count,void *buffer) {
    (void)ctx;
    if(unit>=cd_units || (count && !buffer)) return DE_FUNCTION;
    Cd *c=&cds[unit]; observe(c);
    if(generation!=c->generation) return DE_CHANGED;
    if(!c->present) return DE_NOTREADY;
    if(sector>c->sectors || count>c->sectors-sector) return DE_SEEK;
    for(u32 done=0;done<count;) {
        u32 n=MIN(count-done,CHUNK);
        EFI_STATUS s=c->io->ReadBlocks(c->io,c->media_id,sector+done,(UINTN)n*CD_SECTOR,c->bounce);
        if(s==EFI_MEDIA_CHANGED || s==EFI_NO_MEDIA) {reprobe(c,generation); return DE_CHANGED;}
        if(EFI_ERROR(s)) return efi_dos_error(s);
        memcpy((u8 *)buffer+(u64)done*CD_SECTOR,c->bounce,(size_t)n*CD_SECTOR);
        done+=n;
    }
    c->checked_ms=now_ms();
    return 0;
}
void efi_cdrom_init(EFI_SYSTEM_TABLE *st,IoServices *io) {
    EFI_HANDLE *handles=NULL; UINTN count=0;
    bs=st->BootServices; services=io; cd_units=0; memset(cds,0,sizeof(cds));
    if(!EFI_ERROR(bs->LocateHandleBuffer(ByProtocol,&block_guid,NULL,&count,&handles))) {
        for(UINTN i=0;i<count && cd_units<MAX_CD;i++) {
            EFI_BLOCK_IO *b=NULL; u8 *path=NULL; unsigned bytes=0,duplicate=0;
            if(EFI_ERROR(bs->HandleProtocol(handles[i],&block_guid,(void **)&b)) || !b || !b->Media ||
               b->Media->LogicalPartition || !b->Media->RemovableMedia || b->Media->BlockSize!=CD_SECTOR) continue;
            if(!EFI_ERROR(bs->HandleProtocol(handles[i],&path_guid,(void **)&path)) && path) bytes=efi_path_bytes(path);
            for(unsigned k=0;k<cd_units;k++)
                if(bytes && cds[k].path_bytes==bytes && !memcmp(cds[k].path,path,bytes)) duplicate=1;
            if(duplicate) continue;
            Cd *c=&cds[cd_units];
            size_t align=b->Media->IoAlign<16?16:b->Media->IoAlign;
            if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,CHUNK*CD_SECTOR+align,&c->allocation))) continue;
            c->bounce=(u8 *)(((uintptr_t)c->allocation+align-1)&~(uintptr_t)(align-1));
            c->io=b; c->path=bytes?path:NULL; c->path_bytes=bytes;
            c->generation=1; c->media_id=b->Media->MediaId; c->present=b->Media->MediaPresent;
            c->sectors=c->present?(u64)b->Media->LastBlock+1:0;
            cd_units++;
        }
        bs->FreePool(handles);
    }
    io->cd_count=cd_count; io->cd_info=cd_info; io->cd_read=cd_read;
    io->capabilities|=IO_CAP_CDROM;
}
void efi_cdrom_close(void) {
    for(unsigned i=0;i<cd_units;i++) if(cds[i].allocation) bs->FreePool(cds[i].allocation);
    cd_units=0;
}
