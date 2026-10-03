/* SPDX-License-Identifier: GPL-2.0-or-later
 * Volatile, page-backed native DOS block driver. DOS owns all runtime FAT I/O.
 */
#include "../include/ramdisk.h"
#include "dos_api.h"
typedef struct {u8 *data; u32 generation,readonly;} RamUnit;
static RamUnit disks[RAMDISK_MAX_UNITS];
static const IoServices *io;
static u32 unit_count,pages,removable;
static int options(const char *tail) {
    u32 size=2048; unit_count=1; removable=0;
    unsigned seen=0;
    while(*tail) {
        while(*tail==' ' || *tail=='\t') tail++;
        if(!*tail) break;
        char key[12]; unsigned n=0;
        while(*tail && *tail!=':' && *tail!=' ' && *tail!='\t' && n+1<sizeof(key)) key[n++]=upper(*tail++);
        key[n]=0;
        /* /REMOVABLE reports diskette-like units: they get letters from A:. */
        if(!strcmp(key,"/REMOVABLE") && (!*tail || *tail==' ' || *tail=='\t')) {
            if(seen&4) return DE_EXISTS;
            seen|=4; removable=1; continue;
        }
        if(*tail++!=':') return DE_FUNCTION;
        u32 value=0; n=0;
        while(*tail>='0' && *tail<='9') {
            if(value>6553) return DE_FUNCTION;
            value=value*10+(*tail++-'0'); n++;
        }
        if(!n || (*tail && *tail!=' ' && *tail!='\t')) return DE_FUNCTION;
        unsigned bit;
        if(!strcmp(key,"/SIZE")) {size=value; bit=1;}
        else if(!strcmp(key,"/UNITS")) {unit_count=value; bit=2;}
        else return DE_FUNCTION;
        if(seen&bit) return DE_EXISTS;
        seen|=bit;
    }
    if(size<128 || size>32768 || (size&3) || !unit_count || unit_count>RAMDISK_MAX_UNITS ||
       size*unit_count>65536) return DE_FUNCTION;
    pages=size/4; return 0;
}
static void format(unsigned unit) {
    RamUnit *d=&disks[unit]; u8 *b=d->data; u32 sectors=pages*8;
    u32 bits=12,fat_sectors=((sectors+2)*3+1023)/1024;
    u32 clusters=sectors-33-2*fat_sectors;
    if(clusters>=4085) {
        u32 fat16=((sectors+2)*2+511)/512;
        if(sectors-33-2*fat16>=4085) {bits=16; fat_sectors=fat16;}
    }
    /* Keep FAT12 data clusters below the reserved FF0..FF7 cluster IDs. */
    if(bits==12 && clusters>4078) fat_sectors+=(clusters-4078+1)/2;
    memset(b,0,(size_t)pages*4096);
    memcpy(b,"\xeb\x3c\x90NATVRAM ",11); wr16(b+11,512); b[13]=1;
    wr16(b+14,1); b[16]=2; wr16(b+17,512);
    if(sectors<65536) wr16(b+19,sectors); else wr32(b+32,sectors);
    b[21]=0xf8; wr16(b+22,fat_sectors); wr16(b+24,32); wr16(b+26,64);
    b[36]=0x80; b[38]=0x29; wr32(b+39,0x52000000U^(d->generation<<4)^unit);
    memcpy(b+43,"RAMDISK1   ",11); b[50]+=(u8)unit;
    memcpy(b+54,bits==12?"FAT12   ":"FAT16   ",8); b[510]=0x55; b[511]=0xaa;
    for(unsigned copy=0;copy<2;copy++) {
        u8 *fat=b+(1+copy*fat_sectors)*512;
        fat[0]=0xf8; fat[1]=fat[2]=0xff; if(bits==16) fat[3]=0xff;
    }
    u8 *label=b+(1+2*fat_sectors)*512;
    memcpy(label,b+43,11); label[11]=FA_VOLUME; wr16(label+24,0x21);
}
/* A unit's read-only flag from a u32 0 or 1; a change is a new generation. */
static int set_readonly(RamUnit *d,const void *buffer) {
    u32 readonly=rd32(buffer); if(readonly>1) return DE_FUNCTION;
    if(readonly!=d->readonly) {
        if(d->generation==UINT32_MAX) return DE_IO;
        d->readonly=readonly; d->generation++;
    }
    return 0;
}
static int block_request(void *context,DosBlockRequest *r) {
    (void)context;
    if(r->command==DOS_BLOCK_INIT) {
        memset(disks,0,sizeof(disks)); unit_count=pages=0; io=r->io;
        if(!io || !io->alloc_pages || !io->free_pages || !r->arguments) return DE_FUNCTION;
        int e=options(r->arguments); if(e) return e;
        for(unsigned i=0;i<unit_count;i++) {
            e=io->alloc_pages(io->context,pages,(void **)&disks[i].data); if(e) return e;
            disks[i].generation=1; format(i);
        }
        r->units=unit_count; return 0;
    }
    if(r->command==DOS_BLOCK_FINISH) {
        for(unsigned i=0;i<RAMDISK_MAX_UNITS;i++) if(disks[i].data) {
            io->free_pages(io->context,disks[i].data,pages); disks[i].data=NULL;
        }
        unit_count=pages=0; return 0;
    }
    if(r->unit>=unit_count || !disks[r->unit].data) return DE_DRIVE;
    RamUnit *d=&disks[r->unit];
    if(r->command==DOS_BLOCK_MEDIA) {
        if(!r->buffer || r->count!=sizeof(DosBlockMedia)) return DE_FUNCTION;
        DosBlockMedia m={.size=sizeof(m),.generation=d->generation,.flags=IO_DISK_PRESENT|(removable?IO_DISK_REMOVABLE:0),
            .sector_bytes=512,.sectors=(u64)pages*8,.readonly=d->readonly};
        memcpy(r->buffer,&m,sizeof(m)); r->transferred=sizeof(m); return 0;
    }
    if(r->command==DOS_BLOCK_IOCTL_READ) {
        if(!r->buffer || r->count<sizeof(RamDiskUnitInfo)) return DE_FUNCTION;
        RamDiskUnitInfo info={.size=sizeof(info),.unit=r->unit,.generation=d->generation,
            .readonly=d->readonly,.sectors=(u64)pages*8};
        memcpy(r->buffer,&info,sizeof(info)); r->transferred=sizeof(info); return 0;
    }
    if(r->command==DOS_BLOCK_IOCTL_WRITE) {
        if(!r->buffer || r->count!=4) return DE_FUNCTION;
        int e=set_readonly(d,r->buffer); if(!e) r->transferred=4;
        return e;
    }
    if(r->generation!=d->generation) return DE_CHANGED;
    if(r->command==DOS_BLOCK_FLUSH) return 0;
    if(r->command!=DOS_BLOCK_READ && r->command!=DOS_BLOCK_WRITE) return DE_FUNCTION;
    u64 sectors=(u64)pages*8;
    if(r->sector>sectors || r->count>sectors-r->sector || (r->count && !r->buffer)) return DE_SEEK;
    if(r->command==DOS_BLOCK_WRITE && d->readonly) return DE_READONLY;
    if(!r->count) return 0;
    u8 *data=d->data+(size_t)r->sector*512;
    if(r->command==DOS_BLOCK_READ) memcpy(r->buffer,data,(size_t)r->count*512);
    else memcpy(data,r->buffer,(size_t)r->count*512);
    r->transferred=r->count; return 0;
}
static int control_request(void *context,DosDeviceRequest *r) {
    (void)context;
    switch(r->command) {
    case DOS_DEV_INIT: case DOS_DEV_FINISH: case DOS_DEV_OUTPUT_FLUSH: return 0;
    case DOS_DEV_IOCTL_READ: {
        if(r->count<sizeof(RamDiskStats)) return DE_FUNCTION;
        RamDiskStats stats={.size=sizeof(stats),.units=unit_count,.pages=pages};
        for(unsigned i=0;i<unit_count;i++) stats.generation[i]=disks[i].generation;
        memcpy(r->buffer,&stats,sizeof(stats)); r->transferred=sizeof(stats); return 0;
    }
    case DOS_DEV_GENERIC_IOCTL: {
        if(!(r->mode&3)) return DE_ACCESS;
        if(r->argument>=unit_count) return DE_DRIVE;
        RamUnit *d=&disks[r->argument];
        if(r->control==RAMDISK_RESET) {
            if(r->count) return DE_FUNCTION;
            if(d->readonly) return DE_READONLY;
            if(d->generation==UINT32_MAX) return DE_IO;
            d->generation++; format(r->argument); return 0;
        }
        if(r->control==RAMDISK_PROTECT) {
            if(r->count!=4 || !r->buffer) return DE_FUNCTION;
            int e=set_readonly(d,r->buffer); if(!e) r->transferred=4;
            return e;
        }
        return DE_FUNCTION;
    }
    default: return DE_FUNCTION;
    }
}
void ramdisk_specs(DosBlockSpec *block,DosDeviceSpec *control) {
    *block=(DosBlockSpec){.version=DOS_BLOCK_ABI,.size=sizeof(*block),.name="RAMDISK",.request=block_request};
    *control=(DosDeviceSpec){.version=DOS_DEVICE_ABI,.size=sizeof(*control),.name="RAMCTL",
        .attributes=DOS_DEVICE_CHAR|DOS_DEVICE_IOCTL|DOS_DEVICE_GENERIC,
        .capabilities=DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE,.request=control_request};
}
