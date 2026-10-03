/* SPDX-License-Identifier: GPL-2.0-or-later
 * IO.SYS block service. A Disk's opaque token binds it to one media generation;
 * old callbacks never acquire the current generation implicitly. Devices with
 * 1/2/4 KiB blocks are presented as 512-byte logical sectors by read-modify-
 * write; a torn block on power loss can therefore affect its neighbours.
 */
#include "efi_disk.h"
#define MAX_UNITS 64
#define MAX_PHYSICAL 32
#define MAX_BLOCK 4096U
#define MAX_LOGICAL 24U
/* offset/sectors are 512-byte logical units; identity and last_block are
 * device blocks. partition: 0 whole device, 1..4 MBR slot, 5+ logical drive. */
typedef struct {
    EFI_BLOCK_IO *io;
    EFI_DEVICE_PATH *path;
    unsigned path_bytes;
    u64 identity_start,identity_size,offset,sectors,last_block;
    u32 media_id,generation,block_size,map_generation;
    int present,readonly,removable,boot,partition,dead,physical;
    void *allocation;
    u8 *bounce;
} EfiDisk;
static EfiDisk units[MAX_UNITS],physicals[MAX_PHYSICAL];
static unsigned unit_count,physical_count;
static EFI_BOOT_SERVICES *bs;
static EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static int valid_block(u32 size) {return size>=512 && size<=MAX_BLOCK && !(size&(size-1));}
static u32 ratio(const EfiDisk *d) {return d->block_size/512;}
static int fat_type(u8 type) {return type==1 || type==4 || type==6 || type==0x0e || type==0xef;}
static int extended_type(u8 type) {return type==5 || type==0x0f || type==0x85;}
static int mbr_partition(const u8 *sector,unsigned slot,u64 blocks,u64 *start,u64 *length) {
    if(rd16(sector+510)!=0xaa55 || slot>=4) return DE_FORMAT;
    const u8 *p=sector+446+slot*16;
    if(!fat_type(p[4])) return DE_FORMAT;
    *start=rd32(p+8); *length=rd32(p+12);
    if(!*start || !*length || *start>=blocks || *length>blocks-*start) return DE_FORMAT;
    for(unsigned i=0;i<4;i++) if(i!=slot) {
        const u8 *other=sector+446+i*16; u64 a=rd32(other+8),n=rd32(other+12);
        if(other[4] && n && a<*start+*length && *start<a+n) return DE_FORMAT;
    }
    return 0;
}
static int observe(EfiDisk *d,EFI_STATUS status,int probe) {
    EFI_BLOCK_IO_MEDIA *m=d->io->Media;
    int present=status==EFI_NO_MEDIA?0:m->MediaPresent;
    if(!probe && !d->present && present) present=0;
    int changed=m->MediaId!=d->media_id || present!=d->present || m->ReadOnly!=d->readonly ||
        m->BlockSize!=d->block_size || m->LastBlock!=d->last_block;
    if(changed || status==EFI_MEDIA_CHANGED) {
        if(d->generation==UINT32_MAX) {d->dead=1; return DE_IO;}
        d->generation++;
        d->media_id=m->MediaId; d->present=present; d->readonly=m->ReadOnly;
        d->block_size=m->BlockSize; d->last_block=m->LastBlock;
        if(!d->partition) d->sectors=valid_block(m->BlockSize)?(m->LastBlock+1)*ratio(d):0;
    }
    return d->dead?DE_IO:0;
}
/* One 512-byte logical sector at an absolute device position. */
static int transfer(EfiDisk *d,u64 sector,void *buffer,int write) {
    u32 r=ratio(d),within=(u32)(sector%r)*512; u64 block=sector/r; EFI_STATUS s;
    if(!write || r>1) {
        s=d->io->ReadBlocks(d->io,d->media_id,block,d->block_size,d->bounce);
        int e=observe(d,s,1); if(e) return e;
        if(EFI_ERROR(s)) return efi_dos_error(s);
        if(!write) {memcpy(buffer,d->bounce+within,512); return 0;}
    }
    memcpy(d->bounce+within,buffer,512);
    s=d->io->WriteBlocks(d->io,d->media_id,block,d->block_size,d->bounce);
    int e=observe(d,s,1); if(e) return e;
    return efi_dos_error(s);
}
/* Walk the EBR chain to logical drive n, bounded and without overlaps. */
static int logical_partition(EfiDisk *d,const u8 *mbr,unsigned n,u8 *type,u64 *start,u64 *length) {
    u64 blocks=d->last_block+1,base=0,size=0;
    for(unsigned i=0;i<4;i++) {
        const u8 *p=mbr+446+i*16;
        if(!extended_type(p[4]) || !rd32(p+12)) continue;
        if(size) return DE_FORMAT;
        base=rd32(p+8); size=rd32(p+12);
    }
    if(!size || !base || base>=blocks || size>blocks-base) return DE_FORMAT;
    u64 ebr=base,floor=base; u8 sector[512];
    for(unsigned index=0;index<MAX_LOGICAL;index++) {
        u32 generation=d->generation;
        int e=transfer(d,ebr*ratio(d),sector,0); if(e) return e;
        if(generation!=d->generation) return DE_CHANGED;
        if(rd16(sector+510)!=0xaa55) return DE_FORMAT;
        const u8 *logical=sector+446,*link=sector+462;
        u64 first=ebr+rd32(logical+8),count=rd32(logical+12),next=0; int broken=0;
        if(!logical[4] || extended_type(logical[4]) || !count || !rd32(logical+8) ||
           first<floor || first>=base+size || count>base+size-first) return DE_FORMAT;
        if(link[4] || rd32(link+12)) {
            next=base+rd32(link+8);
            /* A following EBR inside this drive makes the drive itself suspect;
             * a backward or malformed link only ends the chain after it. */
            if(next>ebr && next<first+count) return DE_FORMAT;
            broken=!extended_type(link[4]) || !rd32(link+12) || next<=ebr || next>=base+size;
        }
        if(index==n) {*type=logical[4]; *start=first; *length=count; return 0;}
        if(broken) return DE_FORMAT;
        if(!next) return DE_NOMORE;
        floor=first+count; ebr=next;
    }
    return DE_NOMORE;
}
static int map_partition(EfiDisk *d,const u8 *mbr,unsigned partition,u64 *start,u64 *length) {
    if(partition<=4) return mbr_partition(mbr,partition-1,d->last_block+1,start,length);
    u8 type; int e=logical_partition(d,mbr,partition-5,&type,start,length);
    return e?e==DE_NOMORE?DE_FORMAT:e:fat_type(type)?0:DE_FORMAT;
}
static int probe(EfiDisk *d) {
    EFI_STATUS status=EFI_SUCCESS;
    if(d->removable && valid_block(d->io->Media->BlockSize)) {
        status=d->io->ReadBlocks(d->io,d->io->Media->MediaId,0,d->io->Media->BlockSize,d->bounce);
        if(status==EFI_MEDIA_CHANGED) {
            int e=observe(d,status,1); if(e) return e;
            status=d->io->ReadBlocks(d->io,d->io->Media->MediaId,0,d->io->Media->BlockSize,d->bounce);
        }
    }
    int e=observe(d,status,1); if(e) return e;
    if(!EFI_ERROR(status) && d->present && d->partition && d->map_generation!=d->generation) {
        u32 generation=d->generation;
        if(!valid_block(d->block_size)) return DE_FORMAT;
        u8 mbr[512]; e=transfer(d,0,mbr,0); if(e) return e;
        if(generation!=d->generation) return DE_CHANGED;
        u64 start,length; e=map_partition(d,mbr,d->partition,&start,&length); if(e) return e;
        if(generation!=d->generation) return DE_CHANGED;
        d->offset=start*ratio(d); d->sectors=length*ratio(d); d->map_generation=generation;
    }
    return EFI_ERROR(status) && status!=EFI_NO_MEDIA?efi_dos_error(status):0;
}
static void *token(const EfiDisk *d) {
    unsigned id=d->physical?0x80|(unsigned)(d-physicals+1):(unsigned)(d-units+1);
    return (void *)(uintptr_t)(((u64)d->generation<<8)|id);
}
static int checked(void *ctx,EfiDisk **out) {
    u64 value=(uintptr_t)ctx; unsigned n=value&255,index=(n&0x7f)-1;
    if(!(n&0x7f) || index>=((n&0x80)?physical_count:unit_count)) return DE_DRIVE;
    EfiDisk *d=(n&0x80)?&physicals[index]:&units[index]; int e=observe(d,EFI_SUCCESS,0); if(e) return e;
    if((value>>8)!=d->generation) return DE_CHANGED;
    if(!d->present) return DE_NOTREADY;
    if(d->partition && d->map_generation!=d->generation) return DE_CHANGED;
    if(!valid_block(d->block_size)) return DE_FORMAT;
    u64 total=(d->last_block+1)*ratio(d);
    if(d->offset>total || d->sectors>total-d->offset) return DE_FORMAT;
    *out=d; return 0;
}
static int same_parent(const EfiDisk *a,const EfiDisk *b) {
    return a->path && b->path && a->path_bytes==b->path_bytes &&
        !memcmp(a->path,b->path,a->path_bytes);
}
/* A volume lives on the physical device with the same Block I/O instance
 * (MBR parsed here) or whose device path is its partition's parent. */
static int locate(const EfiDisk *u,unsigned *index,u64 *start) {
    for(unsigned i=0;i<physical_count;i++) {
        const EfiDisk *p=&physicals[i];
        if(p->io==u->io) {*index=i; *start=u->offset; return 0;}
        if(u->partition==0 && u->path!=NULL && u->path_bytes && same_parent(p,u) && p->block_size==u->block_size) {
            *index=i; *start=u->identity_start*ratio(u); return 0;
        }
    }
    return DE_FUNCTION;
}
static int published(const EfiDisk *p,u64 sector) {
    unsigned index=(unsigned)(p-physicals);
    for(unsigned i=0;i<unit_count;i++) {
        unsigned at; u64 start;
        if(!locate(&units[i],&at,&start) && at==index && sector>=start && sector-start<units[i].sectors) return 1;
    }
    return 0;
}
static int disk_read(void *ctx,u32 sector,void *buffer) {
    EfiDisk *d; int e=checked(ctx,&d); if(e) return e;
    if(sector>=d->sectors) return DE_IO;
    e=transfer(d,d->offset+sector,buffer,0); if(e) return e;
    return ((u64)(uintptr_t)ctx>>8)!=d->generation?DE_CHANGED:0;
}
static int disk_write(void *ctx,u32 sector,const void *buffer) {
    EfiDisk *d; int e=checked(ctx,&d); if(e) return e;
    if(sector>=d->sectors) return DE_IO;
    if(d->readonly) return DE_READONLY;
    if(d->physical && published(d,sector)) return DE_ACCESS;
    e=transfer(d,d->offset+sector,(void *)buffer,1); if(e) return e;
    return ((u64)(uintptr_t)ctx>>8)!=d->generation?DE_CHANGED:0;
}
static int disk_flush(void *ctx) {
    EfiDisk *d; int e=checked(ctx,&d); if(e) return e;
    /* FlushBlocks has no MediaId parameter, so verify the snapshot again after
     * probing removable media. It must not flush an unrelated replacement. */
    e=probe(d); if(e) return e; e=checked(ctx,&d); if(e) return e;
    return efi_dos_error(d->io->FlushBlocks(d->io));
}
static int describe(EfiDisk *d,IoDiskInfo *info) {
    int e=probe(d); if(e) return e;
    *info=(IoDiskInfo){.generation=d->generation,
        .flags=(d->boot?IO_DISK_BOOT:0)|(d->removable?IO_DISK_REMOVABLE:0)|(d->present?IO_DISK_PRESENT:0),
        .disk={token(d),disk_read,disk_write,disk_flush,d->sectors,d->readonly}};
    return 0;
}
u32 efi_disk_count(void *ctx) {(void)ctx; return unit_count;}
int efi_disk_info(void *ctx,u32 unit,IoDiskInfo *info) {
    (void)ctx; if(unit>=unit_count || !info) return DE_DRIVE;
    return describe(&units[unit],info);
}
u32 efi_physical_count(void *ctx) {(void)ctx; return physical_count;}
int efi_physical_info(void *ctx,u32 index,IoDiskInfo *info) {
    (void)ctx; if(index>=physical_count || !info) return DE_DRIVE;
    return describe(&physicals[index],info);
}
int efi_disk_location(void *ctx,u32 unit,u32 *physical,u64 *start) {
    (void)ctx; if(unit>=unit_count || !physical || !start) return DE_DRIVE;
    unsigned index; int e=locate(&units[unit],&index,start); if(!e) *physical=index;
    return e;
}
static void identity(EfiDisk *d,EFI_HANDLE handle) {
    EFI_DEVICE_PATH *path=NULL;
    if(EFI_ERROR(bs->HandleProtocol(handle,&path_guid,(void **)&path))) return;
    d->path=path; d->identity_start=d->offset/ratio(d); d->identity_size=d->sectors/ratio(d);
    for(unsigned n=0;n<4096;) {
        EFI_DEVICE_PATH *p=(EFI_DEVICE_PATH *)((u8 *)path+n);
        unsigned len=DevicePathNodeLength(p);
        if(len<4 || len>4096-n) {d->path=NULL; return;}
        if(IsDevicePathEnd(p)) {d->path_bytes=n; return;}
        if(DevicePathType(p)==MEDIA_DEVICE_PATH && p->SubType==MEDIA_HARDDRIVE_DP && len>=42) {
            /* EFI device path nodes are packed; avoid unaligned UINT64 loads. */
            const u8 *raw=(const u8 *)p;
            d->identity_start=rd32(raw+8)|((u64)rd32(raw+12)<<32);
            d->identity_size=rd32(raw+16)|((u64)rd32(raw+20)<<32);
            d->path_bytes=n; return;
        }
        n+=len;
    }
    d->path=NULL;
}
/* start/length are device blocks. */
static int setup(EfiDisk *d,EFI_HANDLE handle,EFI_BLOCK_IO *io,u64 start,u64 length,int partition,int boot) {
    EFI_BLOCK_IO_MEDIA *m=io->Media;
    if(!valid_block(m->BlockSize)) return DE_FORMAT;
    u32 r=m->BlockSize/512;
    *d=(EfiDisk){.io=io,.offset=start*r,.sectors=length*r,.last_block=m->LastBlock,
        .media_id=m->MediaId,.generation=1,.block_size=m->BlockSize,.present=m->MediaPresent,
        .readonly=m->ReadOnly,.removable=m->RemovableMedia,.boot=boot,.partition=partition,.map_generation=1};
    identity(d,handle);
    size_t align=m->IoAlign; if(align<16) align=16;
    if(align>65536 || (align&(align-1))) return DE_FORMAT;
    EFI_STATUS status=bs->AllocatePool(EfiLoaderData,MAX_BLOCK+align,&d->allocation);
    if(EFI_ERROR(status)) return efi_dos_error(status);
    d->bounce=(u8 *)(((uintptr_t)d->allocation+align-1)&~(align-1));
    return 0;
}
static int add(EFI_HANDLE handle,EFI_BLOCK_IO *io,u64 start,u64 length,int partition,int boot) {
    EfiDisk d; int e=setup(&d,handle,io,start,length,partition,boot); if(e) return e;
    for(unsigned i=0;i<unit_count;i++) {
        EfiDisk *old=&units[i];
        if((old->io==io && old->offset==d.offset && old->sectors==d.sectors) ||
           (same_parent(old,&d) && old->identity_start==d.identity_start && old->identity_size==d.identity_size)) {
            bs->FreePool(d.allocation); return 0;
        }
    }
    if(unit_count==MAX_UNITS) {bs->FreePool(d.allocation); return DE_HANDLES;}
    units[unit_count++]=d; return 0;
}
static int add_physical(EFI_HANDLE handle,EFI_BLOCK_IO *io) {
    for(unsigned i=0;i<physical_count;i++) if(physicals[i].io==io) return 0;
    if(physical_count==MAX_PHYSICAL) return DE_HANDLES;
    EfiDisk d; int e=setup(&d,handle,io,0,io->Media->LastBlock+1,0,0); if(e) return e;
    d.physical=1; d.identity_start=d.identity_size=0; physicals[physical_count++]=d; return 0;
}
static int fat_bpb(const u8 *b) {
    return rd16(b+510)==0xaa55 && rd16(b+11)==512 && b[13] && rd16(b+14) &&
        b[16] && rd16(b+17) && rd16(b+22);
}
static EFI_HANDLE *candidates;
static UINTN candidate_count;
/* A firmware partition view without a FAT boot record is still a DOS drive
 * when its parent's MBR or EBR entry has a FAT type, as FDISK leaves a new
 * partition before FORMAT. Other types (and GPT views) stay unpublished. */
static int dos_partition(const EfiDisk *d) {
    if(!d->path || !d->path_bytes) return 0;
    for(UINTN i=0;i<candidate_count;i++) {
        EFI_BLOCK_IO *io=NULL; EfiDisk parent;
        if(EFI_ERROR(bs->HandleProtocol(candidates[i],&block_guid,(void **)&io)) || io->Media->LogicalPartition ||
           io->Media->BlockSize!=d->block_size || setup(&parent,candidates[i],io,0,io->Media->LastBlock+1,0,0)) continue;
        int match=same_parent(&parent,d),found=0; u8 mbr[512];
        if(match && !transfer(&parent,0,mbr,0) && rd16(mbr+510)==0xaa55) {
            for(unsigned slot=0;slot<4;slot++) {
                const u8 *e=mbr+446+slot*16;
                if(rd32(e+8)==d->identity_start && rd32(e+12) && fat_type(e[4])) found=1;
            }
            for(unsigned n=0;!found && n<MAX_LOGICAL;n++) {
                u8 type; u64 start,length;
                if(logical_partition(&parent,mbr,n,&type,&start,&length)) break;
                if(start==d->identity_start && fat_type(type)) found=1;
            }
        }
        bs->FreePool(parent.allocation);
        if(match) return found;
    }
    return 0;
}
static int discover(EFI_HANDLE handle,int boot) {
    EFI_BLOCK_IO *io=NULL;
    if(EFI_ERROR(bs->HandleProtocol(handle,&block_guid,(void **)&io))) return DE_DRIVE;
    EFI_BLOCK_IO_MEDIA *m=io->Media;
    if(!valid_block(m->BlockSize)) return DE_FORMAT;
    unsigned initial=unit_count;
    int e=add(handle,io,0,m->LastBlock+1,0,boot); if(e || unit_count==initial) return e;
    EfiDisk *d=&units[initial];
    if(!m->MediaPresent) return 0;
    u8 sector[512]; e=disk_read(token(d),0,sector);
    if(!e && (fat_bpb(sector) || (m->LogicalPartition && dos_partition(d)))) return 0;
    /* Logical partition views are already bounded by firmware. Raw devices
     * without child handles expose FAT primaries and EBR logical drives. */
    EfiDisk raw=*d; unit_count--;
    if(e || m->LogicalPartition || rd16(sector+510)!=0xaa55) {bs->FreePool(raw.allocation); return e?e:DE_FORMAT;}
    unsigned found=0;
    for(unsigned i=0;i<4 && !e;i++) {
        const u8 *p=sector+446+i*16; u64 start,length;
        if(!fat_type(p[4])) continue;
        e=mbr_partition(sector,i,m->LastBlock+1,&start,&length);
        if(!e) e=add(handle,io,start,length,i+1,boot&&!found);
        if(!e) found++;
    }
    for(unsigned n=0;!e && n<MAX_LOGICAL;n++) {
        u8 type; u64 start,length; int walk=logical_partition(&raw,sector,n,&type,&start,&length);
        if(walk) break; /* Like firmware, keep the drives before a damaged EBR. */
        if(!fat_type(type)) continue;
        e=add(handle,io,start,length,5+n,boot&&!found); if(!e) found++;
    }
    bs->FreePool(raw.allocation);
    return e?e:found?0:DE_FORMAT;
}
int efi_disks_init(EFI_SYSTEM_TABLE *st,EFI_HANDLE boot,Disk *out) {
    bs=st->BootServices; unit_count=physical_count=0; memset(units,0,sizeof(units)); memset(physicals,0,sizeof(physicals));
    EFI_HANDLE *handles=NULL; UINTN count=0;
    EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&block_guid,NULL,&count,&handles);
    if(EFI_ERROR(status)) return efi_dos_error(status);
    candidates=handles; candidate_count=count;
    int e=discover(boot,1); if(e) {candidate_count=0; bs->FreePool(handles); return e;}
    /* Prefer firmware's partition views, including GPT and logical partitions. */
    for(unsigned pass=0;pass<2;pass++) for(UINTN i=0;i<count;i++) {
        if(handles[i]==boot) continue;
        EFI_BLOCK_IO *io=NULL;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&block_guid,(void **)&io))) continue;
        if(!!io->Media->LogicalPartition!=!pass) continue;
        if(!valid_block(io->Media->BlockSize)) continue;
        if(pass) {
            EfiDisk raw={.io=io,.block_size=io->Media->BlockSize,.sectors=io->Media->LastBlock+1}; identity(&raw,handles[i]);
            int child=0;
            for(unsigned j=0;j<unit_count;j++) if(same_parent(&raw,&units[j]) && units[j].identity_start) child=1;
            if(child) continue;
        }
        e=discover(handles[i],0);
        if(e && e!=DE_FORMAT && e!=DE_NOTREADY) {candidate_count=0; bs->FreePool(handles); return e;}
    }
    candidate_count=0;
    /* Whole devices for partitioning tools; partition views are excluded. */
    for(UINTN i=0;i<count;i++) {
        EFI_BLOCK_IO *io=NULL;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&block_guid,(void **)&io)) || io->Media->LogicalPartition) continue;
        e=add_physical(handles[i],io);
        if(e && e!=DE_FORMAT) {bs->FreePool(handles); return e;}
    }
    bs->FreePool(handles);
    for(unsigned i=0;i<unit_count;i++) {
        unsigned index; u64 start;
        if(units[i].boot && !locate(&units[i],&index,&start)) physicals[index].boot=1;
    }
    IoDiskInfo info; e=efi_disk_info(NULL,0,&info); if(!e) *out=info.disk;
    return e;
}
void efi_disks_close(void) {
    for(unsigned i=0;i<unit_count;i++) if(units[i].allocation) bs->FreePool(units[i].allocation);
    for(unsigned i=0;i<physical_count;i++) if(physicals[i].allocation) bs->FreePool(physicals[i].allocation);
    unit_count=physical_count=0;
}
