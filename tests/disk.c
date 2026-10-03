/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host test for IO.SYS Block I/O publication with mocked EFI boot services:
 * MBR/EBR walking, 512-byte emulation on larger blocks, firmware partition
 * children, physical-disk protection of published volumes and media changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../platform/efi_disk.h"
static unsigned checks;
void con_write(const void *data,size_t n) {(void)data; (void)n;}
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"FAIL disk:%u: %s\n",__LINE__,#x); exit(1);}} while(0)
typedef struct {
    EFI_BLOCK_IO bio;
    EFI_BLOCK_IO_MEDIA media;
    u8 *data; u64 bytes;
    u8 path[128];
    unsigned reads,writes,flushes;
} Device;
static Device devices[4];
static unsigned device_count,pool_live;
static EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static Device *owner(EFI_BLOCK_IO *bio) {return (Device *)((u8 *)bio-offsetof(Device,bio));}
static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO *bio,UINT32 media,EFI_LBA lba,UINTN size,VOID *buffer) {
    Device *d=owner(bio); d->reads++;
    if(!d->media.MediaPresent) return EFI_NO_MEDIA;
    if(media!=d->media.MediaId) return EFI_MEDIA_CHANGED;
    if(size%d->media.BlockSize || (lba+size/d->media.BlockSize)*d->media.BlockSize>d->bytes) return EFI_INVALID_PARAMETER;
    memcpy(buffer,d->data+lba*d->media.BlockSize,size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_blocks(EFI_BLOCK_IO *bio,UINT32 media,EFI_LBA lba,UINTN size,VOID *buffer) {
    Device *d=owner(bio); d->writes++;
    if(!d->media.MediaPresent) return EFI_NO_MEDIA;
    if(media!=d->media.MediaId) return EFI_MEDIA_CHANGED;
    if(d->media.ReadOnly) return EFI_WRITE_PROTECTED;
    if(size%d->media.BlockSize || (lba+size/d->media.BlockSize)*d->media.BlockSize>d->bytes) return EFI_INVALID_PARAMETER;
    memcpy(d->data+lba*d->media.BlockSize,buffer,size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush_blocks(EFI_BLOCK_IO *bio) {owner(bio)->flushes++; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE handle,EFI_GUID *guid,VOID **out) {
    Device *d=handle;
    if(!memcmp(guid,&block_guid,sizeof(*guid))) {*out=&d->bio; return EFI_SUCCESS;}
    if(!memcmp(guid,&path_guid,sizeof(*guid))) {*out=d->path; return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI locate_handles(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **out) {
    CHECK(type==ByProtocol && !memcmp(guid,&block_guid,sizeof(*guid)) && !key);
    *out=malloc(sizeof(EFI_HANDLE)*(device_count+1)); CHECK(*out!=NULL); pool_live++;
    for(unsigned i=0;i<device_count;i++) (*out)[i]=&devices[i];
    *count=device_count; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type,UINTN size,VOID **out) {
    (void)type; *out=malloc(size); if(!*out) return EFI_OUT_OF_RESOURCES;
    pool_live++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pool(VOID *p) {CHECK(pool_live>0); pool_live--; free(p); return EFI_SUCCESS;}
/* Controller node + optional HD node (start/size in device blocks) + end. */
static void set_path(Device *d,unsigned controller,int partition,u64 start,u64 size) {
    u8 *p=d->path; memset(p,0,sizeof(d->path));
    p[0]=1; p[1]=1; wr16(p+2,6); p[4]=(u8)controller; p+=6;
    if(partition) {
        p[0]=4; p[1]=1; wr16(p+2,42); wr32(p+4,(u32)partition);
        wr32(p+8,(u32)start); wr32(p+12,(u32)(start>>32)); wr32(p+16,(u32)size); wr32(p+20,(u32)(size>>32));
        p[40]=1; p[41]=1; p+=42;
    }
    p[0]=0x7f; p[1]=0xff; wr16(p+2,4);
}
static Device *device(u8 *data,u64 bytes,u32 block,int logical,int removable,unsigned controller) {
    CHECK(device_count<4); Device *d=&devices[device_count++]; memset(d,0,sizeof(*d));
    d->data=data; d->bytes=bytes;
    d->media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.RemovableMedia=removable,.MediaPresent=1,.LogicalPartition=logical,
        .BlockSize=block,.IoAlign=8,.LastBlock=bytes/block-1};
    d->bio=(EFI_BLOCK_IO){.Revision=1,.Media=&d->media,.ReadBlocks=read_blocks,.WriteBlocks=write_blocks,.FlushBlocks=flush_blocks};
    set_path(d,controller,0,0,0); return d;
}
static void entry(u8 *table,unsigned slot,u8 type,u32 start,u32 count) {
    u8 *p=table+446+slot*16; memset(p,0,16); p[4]=type; wr32(p+8,start); wr32(p+12,count);
    table[510]=0x55; table[511]=0xaa;
}
static void bpb(u8 *b,u32 sectors) {
    memset(b,0,512); wr16(b+11,512); b[13]=4; wr16(b+14,1); b[16]=2; wr16(b+17,512);
    wr16(b+19,sectors<65536?sectors:0); wr32(b+32,sectors); b[21]=0xf8; wr16(b+22,16); b[510]=0x55; b[511]=0xaa;
}
static EFI_BOOT_SERVICES boot={.HandleProtocol=handle_protocol,.LocateHandleBuffer=locate_handles,
    .AllocatePool=allocate_pool,.FreePool=free_pool};
static EFI_SYSTEM_TABLE system_table={.BootServices=&boot};
static void reset(void) {efi_disks_close(); CHECK(!pool_live); device_count=0;}
static void test_mbr_ebr(void) {
    u64 bytes=40000ULL*512; u8 *data=calloc(1,bytes); CHECK(data!=NULL);
    entry(data,0,0x06,2048,4096); entry(data,1,0x05,8192,20000);
    u8 *ebr=data+8192ULL*512; entry(ebr,0,0x06,63,4000); entry(ebr,1,0x05,5000,6000);
    ebr=data+13192ULL*512; entry(ebr,0,0x07,63,3000); entry(ebr,1,0x05,9000,4000);
    ebr=data+17192ULL*512; entry(ebr,0,0x01,63,3900);
    for(unsigned i=0;i<512;i++) {data[2048*512+i]=(u8)i; data[8255ULL*512+i]=(u8)(i*3); data[17255ULL*512+i]=(u8)(i*7);}
    Device *raw=device(data,bytes,512,0,0,1); Disk out;
    CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==3 && efi_physical_count(NULL)==1);
    IoDiskInfo info; u8 sector[512];
    CHECK(!efi_disk_info(NULL,0,&info) && (info.flags&IO_DISK_BOOT) && info.disk.sectors==4096);
    CHECK(!out.read(out.ctx,0,sector) && sector[5]==5);
    CHECK(!efi_disk_info(NULL,1,&info) && !(info.flags&IO_DISK_BOOT) && info.disk.sectors==4000);
    CHECK(!info.disk.read(info.disk.ctx,0,sector) && sector[5]==15);
    CHECK(info.disk.read(info.disk.ctx,4000,sector)==DE_IO);
    CHECK(!efi_disk_info(NULL,2,&info) && info.disk.sectors==3900 && !info.disk.read(info.disk.ctx,0,sector) && sector[5]==35);
    u32 physical; u64 start;
    CHECK(!efi_disk_location(NULL,0,&physical,&start) && !physical && start==2048);
    CHECK(!efi_disk_location(NULL,1,&physical,&start) && start==8255);
    CHECK(!efi_disk_location(NULL,2,&physical,&start) && start==17255);
    CHECK(efi_disk_location(NULL,3,&physical,&start)==DE_DRIVE);
    CHECK(!efi_physical_info(NULL,0,&info) && (info.flags&IO_DISK_BOOT) && info.disk.sectors==40000);
    memset(sector,0xa5,512);
    CHECK(!info.disk.write(info.disk.ctx,1,sector) && data[512]==0xa5);
    CHECK(info.disk.write(info.disk.ctx,2048,sector)==DE_ACCESS && info.disk.write(info.disk.ctx,6143,sector)==DE_ACCESS);
    CHECK(!info.disk.write(info.disk.ctx,6144,sector) && !info.disk.write(info.disk.ctx,8192,sector));
    entry(data+8192ULL*512,0,0x06,63,4000); entry(data+8192ULL*512,1,0x05,5000,6000); /* Rebuild the overwritten EBR. */
    CHECK(info.disk.write(info.disk.ctx,8255,sector)==DE_ACCESS && info.disk.write(info.disk.ctx,17255+3899,sector)==DE_ACCESS);
    CHECK(!info.disk.write(info.disk.ctx,13192+63+10,sector)); /* Non-FAT logical drive is not published. */
    CHECK(efi_physical_info(NULL,1,&info)==DE_DRIVE);
    reset();
    /* Damage: a backwards link stops the walk after the first logical drive;
     * a self-link, overlap or overrun never loops or publishes beyond it. */
    entry(data+8192ULL*512,1,0x05,0,6000);
    raw=device(data,bytes,512,0,0,1); CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==2); reset();
    entry(data+8192ULL*512,1,0x05,5000,6000); entry(data+13192ULL*512,1,0x05,5000,4000);
    raw=device(data,bytes,512,0,0,1); CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==2); reset();
    entry(data+8192ULL*512,0,0x06,63,6000);
    raw=device(data,bytes,512,0,0,1); CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==1); reset();
    entry(data+8192ULL*512,0,0x06,63,30000); entry(data+8192ULL*512,1,0,0,0);
    raw=device(data,bytes,512,0,0,1); CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==1); reset();
    entry(data,2,0x06,9000,100); /* Overlaps the extended partition. */
    raw=device(data,bytes,512,0,0,1); CHECK(efi_disks_init(&system_table,raw,&out)==DE_FORMAT); reset();
    free(data);
}
static void test_large_blocks(void) {
    u64 bytes=4096ULL*2048; u8 *data=calloc(1,bytes); CHECK(data!=NULL);
    entry(data,0,0x0e,256,1024); bpb(data+256ULL*4096,8192);
    for(unsigned i=0;i<4096;i++) data[257ULL*4096+i]=(u8)(i/512+1);
    Device *raw=device(data,bytes,4096,0,1,2); Disk out; IoDiskInfo info; u8 sector[512];
    CHECK(!efi_disks_init(&system_table,raw,&out) && efi_disk_count(NULL)==1 && out.sectors==8192);
    CHECK(!out.read(out.ctx,10,sector) && sector[0]==3 && sector[511]==3);
    memset(sector,0xee,512); unsigned writes=raw->writes;
    CHECK(!out.write(out.ctx,10,sector) && raw->writes==writes+1);
    for(unsigned i=0;i<4096;i++) CHECK(data[257ULL*4096+i]==(i/512==2?0xee:(u8)(i/512+1)));
    u32 physical; u64 start;
    CHECK(!efi_disk_location(NULL,0,&physical,&start) && start==2048);
    CHECK(!efi_physical_info(NULL,0,&info) && info.disk.sectors==16384 && (info.flags&IO_DISK_REMOVABLE));
    CHECK(!info.disk.read(info.disk.ctx,0,sector) && sector[510]==0x55 && info.disk.write(info.disk.ctx,2048+8191,sector)==DE_ACCESS);
    CHECK(!info.disk.write(info.disk.ctx,7,sector) && data[7*512+510]==0x55 && !data[0] && data[510]==0x55);
    /* Media replacement invalidates old tokens, including physical ones. */
    raw->media.MediaId++;
    CHECK(out.read(out.ctx,0,sector)==DE_CHANGED && info.disk.read(info.disk.ctx,0,sector)==DE_CHANGED);
    CHECK(!efi_disk_info(NULL,0,&info) && !info.disk.read(info.disk.ctx,0,sector) && rd16(sector+11)==512);
    raw->media.MediaPresent=0; CHECK(!efi_disk_info(NULL,0,&info) && !(info.flags&IO_DISK_PRESENT));
    CHECK(info.disk.read(info.disk.ctx,0,sector)==DE_NOTREADY);
    raw->media.MediaPresent=1; raw->media.MediaId++;
    raw->media.BlockSize=520; CHECK(efi_disk_info(NULL,0,&info)==DE_FORMAT && info.disk.read(info.disk.ctx,0,sector)==DE_CHANGED);
    reset();
    Device *bad=device(data,bytes,520,0,0,2); CHECK(efi_disks_init(&system_table,bad,&out)==DE_FORMAT); reset();
    bad=device(data,bytes,8192,0,0,2); CHECK(efi_disks_init(&system_table,bad,&out)==DE_FORMAT); reset();
    free(data);
}
static void test_firmware_children(void) {
    u64 bytes=16384ULL*512; u8 *data=calloc(1,bytes); CHECK(data!=NULL);
    entry(data,0,0xef,2048,4096); entry(data,1,0x06,8192,4096);
    bpb(data+2048ULL*512,4096); bpb(data+8192ULL*512,4096); data[8192ULL*512+100]=0x42;
    Device *parent=device(data,bytes,512,0,0,3);
    Device *boot_part=device(data+2048ULL*512,4096ULL*512,512,1,0,3); set_path(boot_part,3,1,2048,4096);
    Device *second=device(data+8192ULL*512,4096ULL*512,512,1,0,3); set_path(second,3,2,8192,4096);
    Disk out; IoDiskInfo info; u8 sector[512];
    CHECK(!efi_disks_init(&system_table,boot_part,&out) && efi_disk_count(NULL)==2 && efi_physical_count(NULL)==1);
    CHECK(!efi_disk_info(NULL,1,&info) && !info.disk.read(info.disk.ctx,0,sector) && sector[100]==0x42);
    u32 physical; u64 start;
    CHECK(!efi_disk_location(NULL,0,&physical,&start) && !physical && start==2048);
    CHECK(!efi_disk_location(NULL,1,&physical,&start) && start==8192);
    CHECK(!efi_physical_info(NULL,0,&info) && (info.flags&IO_DISK_BOOT) && info.disk.sectors==16384);
    memset(sector,0x11,512);
    CHECK(info.disk.write(info.disk.ctx,2048,sector)==DE_ACCESS && info.disk.write(info.disk.ctx,8192+4095,sector)==DE_ACCESS);
    CHECK(!info.disk.write(info.disk.ctx,6144,sector) && data[6144ULL*512]==0x11 && parent->flushes==0);
    CHECK(!info.disk.flush(info.disk.ctx) && parent->flushes==1);
    reset();
    /* FDISK leaves new partitions without a boot record: a FAT-typed entry is
     * still published (FORMAT needs its letter); other types are not. */
    entry(data,2,0x06,12288,2048); entry(data,3,0x07,14336,2048);
    memset(data+12288ULL*512,0,512); memset(data+14336ULL*512,0,512);
    parent=device(data,bytes,512,0,0,3);
    boot_part=device(data+2048ULL*512,4096ULL*512,512,1,0,3); set_path(boot_part,3,1,2048,4096);
    Device *fresh=device(data+12288ULL*512,2048ULL*512,512,1,0,3); set_path(fresh,3,3,12288,2048);
    Device *other=device(data+14336ULL*512,2048ULL*512,512,1,0,3); set_path(other,3,4,14336,2048);
    CHECK(!efi_disks_init(&system_table,boot_part,&out) && efi_disk_count(NULL)==2);
    CHECK(!efi_disk_info(NULL,1,&info) && info.disk.sectors==2048 && !efi_disk_location(NULL,1,&physical,&start) && start==12288);
    (void)other; reset(); free(data);
}
int main(void) {
    test_mbr_ebr(); test_large_blocks(); test_firmware_children();
    printf("PASS IO.SYS disks: %u assertions (MBR/EBR walk, 512-byte emulation, firmware partitions, protected physical writes, media changes)\n",checks);
    return 0;
}
