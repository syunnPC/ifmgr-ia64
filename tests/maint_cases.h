/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Host cases for maintenance services: native INT 25h/26h, volume locks,
 * physical disks, generic block IOCTL, DPB queries and server/network calls.
 * Included by tests/host.c after its disk and module fixtures.
 */
static MemoryDisk physical_disk;
static u32 physical_generation=1,physical_writes;
static u64 physical_start=2048;
static int physical_read(void *ctx,u32 lba,void *buf) {
    (void)ctx; return (u64)lba*512+512>physical_disk.bytes?DE_IO:(memcpy(buf,physical_disk.data+(u64)lba*512,512),0);
}
static int physical_write(void *ctx,u32 lba,const void *buf) {
    (void)ctx; if((u64)lba*512+512>physical_disk.bytes) return DE_IO;
    /* IO.SYS protects published volumes on the device. */
    if(lba>=physical_start) return DE_ACCESS;
    physical_writes++; memcpy(physical_disk.data+(u64)lba*512,buf,512); return 0;
}
static int physical_flush(void *ctx) {(void)ctx; return 0;}
static u32 physical_count(void *ctx) {(void)ctx; return 1;}
static int physical_info(void *ctx,u32 n,IoDiskInfo *info) {
    (void)ctx; if(n) return DE_DRIVE;
    *info=(IoDiskInfo){.generation=physical_generation,.flags=IO_DISK_PRESENT,
        .disk={NULL,physical_read,physical_write,physical_flush,physical_disk.bytes/512,0}};
    return 0;
}
static int physical_location(void *ctx,u32 unit,u32 *physical,u64 *start) {
    (void)ctx; if(unit!=1) return DE_FUNCTION;
    *physical=0; *start=physical_start; return 0;
}
static void physical_restart(void *ctx) {(void)ctx;}
static int maint_call(unsigned ax,u64 bx,u64 cx,u64 dx,DosRegs *out) {
    DosRegs r={.ax=ax,.bx=bx,.cx=cx,.dx=dx}; int e=call(&r); if(out) *out=r; return e;
}
static int block_generic(unsigned drive,unsigned function,void *data) {
    DosRegs r={.ax=0x440d,.bx=drive+1,.cx=0x0800|function,.dx=(uintptr_t)data}; return call(&r);
}
static int test_device_params(unsigned drive,DosDeviceParams *p) {return block_generic(drive,0x60,p);}
static int test_set_device_params(unsigned drive,const DosDeviceParams *p) {return block_generic(drive,0x40,(void *)p);}
static int test_sector_io(unsigned drive,unsigned function,DosSectorIo *io) {return block_generic(drive,function,io);}
static int test_media_id(unsigned drive,DosMediaId *id,int set) {return block_generic(drive,set?0x46:0x66,id);}
static void test_maintenance(Disk *disk,MemoryDisk *md) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    u8 *saved=malloc(md->bytes); CHECK(saved!=NULL); memcpy(saved,md->data,md->bytes);
    MemoryDisk copies[2]; memset(copies,0,sizeof(copies));
    for(unsigned i=0;i<ARRAY_SIZE(copies);i++) {
        copies[i].bytes=md->bytes; copies[i].data=malloc(md->bytes); CHECK(copies[i].data!=NULL);
        memcpy(copies[i].data,md->data,md->bytes);
    }
    physical_disk.bytes=(size_t)(physical_start*512+md->bytes); physical_disk.data=calloc(1,physical_disk.bytes);
    CHECK(physical_disk.data!=NULL); physical_disk.data[510]=0x55; physical_disk.data[511]=0xaa;
    test_units[0]=(TestUnit){.disk=md,.generation=1,.flags=IO_DISK_BOOT|IO_DISK_PRESENT};
    test_units[1]=(TestUnit){.disk=&copies[0],.generation=1,.flags=IO_DISK_PRESENT};
    test_units[2]=(TestUnit){.disk=&copies[1],.generation=1,.flags=IO_DISK_REMOVABLE|IO_DISK_PRESENT};
    test_units[3]=(TestUnit){.disk=&copies[1],.generation=1,.flags=0};
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.capabilities=IO_CAP_DISKS|IO_CAP_PHYSICAL,
        .disk_count=unit_count,.disk_info=unit_info,.physical_count=physical_count,.physical_info=physical_info,
        .disk_location=physical_location,.restart=physical_restart};
    driver_io=&io;
    CHECK(!dos_init(disk,arena)); CHECK(!dos_attach_disks(&io));
    u8 sector[1024],other[512]; u32 n; unsigned h; DosRegs r; DosDriveInfo info; char path[DOS_PATH_MAX];

    /* Native INT 25h/26h. */
    CHECK(!dos_disk_read(2,0,2,sector,&n) && n==2 && !memcmp(sector,md->data,1024));
    CHECK(dos_disk_read(2,md->bytes/512-1,2,sector,&n)==DE_SEEK && !n);
    CHECK(dos_disk_read(26,0,1,sector,&n)==DE_DRIVE && dos_disk_read(1,0,1,sector,&n)==DE_DRIVE);
    CHECK(dos_disk_read(4,0,1,sector,&n)==DE_NOTREADY);
    CHECK(dos_disk_write(3,0,1,sector,&n)==DE_ACCESS && !n);
    CHECK(dos_volume_lock(3,2)==DE_FUNCTION);
    CHECK(!dos_open("D:\\HELD.TXT",2,1,&h)); CHECK(dos_volume_lock(3,1)==DE_ACCESS); CHECK(!dos_close(h));
    CHECK(!dos_mkdir("D:\\LOCKDIR") && !dos_chdir("D:\\LOCKDIR"));
    CHECK(!dos_drive_info(3,&info)); u32 generation=info.generation;
    DosFind find; CHECK(!dos_find_first("D:\\*.*",FA_DIR,&find));
    CHECK(!dos_volume_lock(3,1) && !dos_volume_lock(3,1));
    CHECK(dos_open("D:\\HELD.TXT",0,0,&h)==DE_ACCESS && dos_mkdir("D:\\NO")==DE_ACCESS);
    u32 root=dos_pid(),child; CHECK(!dos_task_create(&child) && !dos_task_select(child));
    CHECK(dos_volume_lock(3,1)==DE_ACCESS && dos_volume_lock(3,0)==DE_ACCESS);
    CHECK(dos_disk_read(3,0,1,sector,&n)==DE_ACCESS);
    CHECK(!dos_task_select(root));
    CHECK(!dos_disk_read(3,0,1,sector,&n) && n==1);
    memcpy(other,sector,512); memcpy(other+3,"LOCKTEST",8);
    CHECK(!dos_disk_write(3,0,1,other,&n) && n==1 && !memcmp(copies[0].data+3,"LOCKTEST",8));
    test_units[1].readonly=1; test_units[1].generation++;
    CHECK(dos_disk_write(3,0,1,sector,&n)==DE_READONLY);
    test_units[1].readonly=0; test_units[1].generation++;
    CHECK(!dos_disk_write(3,0,1,sector,&n));
    CHECK(!dos_volume_lock(3,0) && dos_volume_lock(3,0)==DE_ACCESS);
    CHECK(!dos_drive_info(3,&info) && info.generation>generation);
    CHECK(!dos_drive_cwd(3,path) && !strcmp(path,"\\") && dos_find_next(&find)==DE_CHANGED);
    CHECK(!dos_open("D:\\HELD.TXT",0,0,&h) && !dos_close(h));
    /* A task's lock disappears with the task and forces a remount. */
    CHECK(!dos_task_select(child) && !dos_volume_lock(3,1) && !dos_task_select(root));
    CHECK(dos_open("D:\\HELD.TXT",0,0,&h)==DE_ACCESS);
    CHECK(!dos_task_destroy(child)); CHECK(!dos_open("D:\\HELD.TXT",0,0,&h) && !dos_close(h));
    /* Raw access repairs a volume that FAT refuses to mount. */
    Fat *f=&dos_volume; u32 fat2=(f->fat_start+f->fat_sectors)*512;
    copies[0].data[fat2+4]^=0x5a; test_units[1].generation++;
    CHECK(dos_open("D:\\HELD.TXT",0,0,&h)==DE_IO);
    CHECK(!dos_disk_read(3,f->fat_start,1,sector,&n));
    CHECK(!dos_volume_lock(3,1) && !dos_disk_write(3,f->fat_start+f->fat_sectors,1,sector,&n) && !dos_volume_lock(3,0));
    CHECK(!dos_open("D:\\HELD.TXT",0,0,&h) && !dos_close(h));
    CHECK(!dos_remove("D:\\HELD.TXT",0) && !dos_remove("D:\\LOCKDIR",1));

    /* DPB and allocation queries. */
    CHECK(!maint_call(0x1f00,0,0,0,&r) && (r.ax&255)==0);
    const DosDpb *dpb=(const DosDpb *)(uintptr_t)r.bx;
    CHECK(dpb->size==sizeof(DosDpb) && dpb->drive==2 && dpb->sectors_per_cluster==f->spc && dpb->clusters==f->clusters &&
          dpb->first_data==f->data_start && dpb->fat_bits==f->bits && dpb->media==md->data[21] &&
          (1U<<dpb->cluster_shift)==f->spc && dpb->free_clusters<=f->clusters && dpb->sectors==f->total);
    CHECK(!maint_call(0x3200,0,0,4,&r) && (r.ax&255)==0 && ((const DosDpb *)(uintptr_t)r.bx)->drive==3);
    CHECK(!maint_call(0x3200,0,0,2,&r) && (r.ax&255)==255);
    CHECK(!maint_call(0x3200,0,0,5,&r) && (r.ax&255)==255); /* No medium. */
    CHECK(!maint_call(0x1c00,0,0,3,&r) && r.ax==f->spc && r.cx==512 && r.dx==f->clusters && *(u8 *)(uintptr_t)r.bx==md->data[21]);
    CHECK(!maint_call(0x1b00,0,0,0,&r) && r.dx==f->clusters);
    CHECK(!maint_call(0x1c00,0,0,27,&r) && (r.ax&255)==255);
    CHECK(!maint_call(0x3400,0,0,0,&r) && *(const unsigned *)(uintptr_t)r.bx==0);
    CHECK(!maint_call(0x3700,0,0,0,&r) && (r.dx&255)=='/' && !(r.ax&255));
    CHECK(!maint_call(0x3701,0,0,'-',NULL) && !maint_call(0x3700,0,0,0,&r) && (r.dx&255)=='-');
    CHECK(!maint_call(0x3701,0,0,'/',NULL) && !maint_call(0x3702,0,0,0,&r) && (r.dx&255)==255);
    CHECK(!maint_call(0x3709,0,0,0,&r) && (r.ax&255)==255);

    /* PSP-style task identity maps onto native task IDs. */
    CHECK(!maint_call(0x5100,0,0,0,&r) && r.bx==root && !maint_call(0x6200,0,0,0,&r) && r.bx==root);
    CHECK(!dos_task_create(&child)); CHECK(!maint_call(0x5000,child,0,0,NULL) && dos_pid()==child);
    CHECK(!maint_call(0x6200,0,0,0,&r) && r.bx==child && !maint_call(0x5000,root,0,0,NULL) && dos_pid()==root);
    CHECK(maint_call(0x5000,999,0,0,NULL)==DE_BLOCK);

    /* Server calls: commit, open-file list, close by name/task, errors. */
    unsigned a,b; CHECK(!dos_open("C:\\SERVER.TXT",2,1,&a) && !dos_lock(a,0,0,4));
    CHECK(!dos_task_select(child) && !dos_open("C:\\SERVER.TXT",DOS_OPEN_READ,0,&b) && !dos_task_select(root));
    CHECK(maint_call(0x5d00,0,0,0,NULL)==DE_FUNCTION && !maint_call(0x5d01,0,0,0,NULL));
    CHECK(!maint_call(0x5d05,0,0,0,&r) && !strcmp((const char *)(uintptr_t)r.di,"C:\\SERVER.TXT") && r.cx==1 && !r.bx);
    CHECK(maint_call(0x5d05,1,0,0,NULL)==DE_NOMORE);
    DosServerCall server={.regs={.ax=0x1900}};
    CHECK(!maint_call(0x5d00,0,0,(uintptr_t)&server,&r) && r.ax==2);
    server=(DosServerCall){.regs={.ax=0x5d00}}; CHECK(maint_call(0x5d00,0,0,(uintptr_t)&server,NULL)==DE_FUNCTION);
    server=(DosServerCall){.pid=child}; CHECK(!maint_call(0x5d04,0,0,(uintptr_t)&server,NULL));
    CHECK(!dos_task_select(child) && dos_close(b)==DE_HANDLE && !dos_task_select(root));
    CHECK(!maint_call(0x5d05,0,0,0,&r)); server=(DosServerCall){.pid=12345};
    CHECK(maint_call(0x5d04,0,0,(uintptr_t)&server,NULL)==DE_BLOCK);
    server=(DosServerCall){.regs={.dx=(uintptr_t)"C:\\SERVER.TXT"}};
    CHECK(!maint_call(0x5d02,0,0,(uintptr_t)&server,NULL) && dos_close(a)==DE_HANDLE);
    CHECK(maint_call(0x5d02,0,0,(uintptr_t)&server,NULL)==DE_NOFILE && maint_call(0x5d05,0,0,0,NULL)==DE_NOMORE);
    CHECK(!maint_call(0x5d03,0,0,0,NULL) && maint_call(0x5d06,0,0,0,NULL)==DE_FUNCTION);
    DosExtendedError set={.error=DE_LOCK,.error_class=DOS_CLASS_LOCKED,.action=DOS_ACTION_DELAY,.locus=DOS_LOCUS_NETWORK};
    CHECK(!maint_call(0x5d0a,0,0,(uintptr_t)&set,NULL) && !maint_call(0x5900,0,0,0,&r));
    CHECK(r.ax==DE_LOCK && r.bx==((DOS_CLASS_LOCKED<<8)|DOS_ACTION_DELAY) && r.cx==(DOS_LOCUS_NETWORK<<8));
    CHECK(!dos_remove("C:\\SERVER.TXT",0) && !dos_task_destroy(child));

    /* Machine names and drive enable/disable; no redirector exists. */
    char name[16]; CHECK(!maint_call(0x5e00,0,0,(uintptr_t)name,&r) && !strcmp(name,"               ") && !r.cx);
    CHECK(!maint_call(0x5e01,0,0x0105,(uintptr_t)"IA64HOST",NULL));
    CHECK(!maint_call(0x5e00,0,0,(uintptr_t)name,&r) && !strcmp(name,"IA64HOST       ") && r.cx==0x0105);
    CHECK(maint_call(0x5e02,0,0,0,NULL)==DE_FUNCTION && maint_call(0x5f02,0,0,0,NULL)==DE_FUNCTION);
    CHECK(maint_call(0x5f08,0,0,2,NULL)==DE_CURRENT && maint_call(0x5f08,0,0,1,NULL)==DE_DRIVE);
    CHECK(!maint_call(0x5f08,0,0,3,NULL) && dos_select_drive(3)==DE_DRIVE && dos_open("D:\\X",0,0,&h)==DE_DRIVE);
    CHECK(!maint_call(0x5f07,0,0,3,NULL) && !dos_drive_info(3,&info));

    /* Generic block IOCTL with native LBA parameter blocks. */
    DosDeviceParams params={.size=sizeof(params)};
    CHECK(!test_device_params(2,&params) && params.device_type==DOS_DEVICE_FIXED && params.attributes==DOS_DEVICE_NONREMOVABLE);
    u32 total=(u32)(md->bytes/512);
    CHECK(params.sectors==total && params.physical==UINT32_MAX && params.media==0xf8 && params.root_entries==512);
    if(total==65536) CHECK(params.sectors_per_cluster==4 && params.sectors_per_fat==64 && params.fat_bits==16);
    params=(DosDeviceParams){.size=sizeof(params),.flags=DOS_PARAMS_CURRENT};
    CHECK(!test_device_params(2,&params) && params.sectors_per_cluster==f->spc && params.sectors_per_fat==f->fat_sectors);
    params=(DosDeviceParams){.size=sizeof(params)};
    CHECK(!test_device_params(3,&params) && params.physical==0 && params.hidden==physical_start);
    /* Unit 2 is removable, so it became A:. */
    params=(DosDeviceParams){.size=sizeof(params)};
    CHECK(!test_device_params(0,&params) && params.device_type==DOS_DEVICE_OTHER && !params.attributes);
    DosDeviceParams wanted=params; wanted.sectors_per_cluster=16; wanted.root_entries=256;
    CHECK(!test_set_device_params(0,&wanted));
    params=(DosDeviceParams){.size=sizeof(params)};
    CHECK(!test_device_params(0,&params) && params.sectors_per_cluster==16 && params.root_entries==256);
    wanted.sectors_per_cluster=3; CHECK(test_set_device_params(0,&wanted)==DE_FUNCTION);
    wanted.sectors_per_cluster=16; wanted.root_entries=17; CHECK(test_set_device_params(0,&wanted)==DE_FUNCTION);
    wanted.root_entries=256; wanted.sectors_per_fat=1; CHECK(test_set_device_params(0,&wanted)==DE_FUNCTION);
    params=(DosDeviceParams){.size=sizeof(params),.flags=8}; CHECK(test_device_params(0,&params)==DE_FUNCTION);
    test_units[2].generation++; params=(DosDeviceParams){.size=sizeof(params)};
    CHECK(!test_device_params(0,&params) && params.sectors_per_cluster!=16);
    /* MS-DOS 4 DiskTable2 and diskette rules, on an absent fixed unit (E:). */
    const struct {u64 sectors; u32 spc,root,bits; u8 removable,media;} rules[]={
        {20000,8,512,12,0,0xf8},{32680,8,512,12,0,0xf8},{32681,4,512,16,0,0xf8},{0x40001,8,512,16,0,0xf8},
        {0x100001,32,512,16,0,0xf8},{2880,1,224,12,1,0xf0},{2880,8,512,12,0,0xf8},{720,2,112,12,1,0xfd}};
    MemoryDisk fake={0}; test_units[3].disk=&fake;
    for(unsigned i=0;i<ARRAY_SIZE(rules);i++) {
        fake.bytes=rules[i].sectors*512; test_units[3].flags=rules[i].removable?IO_DISK_REMOVABLE:0; test_units[3].generation++;
        DosDeviceParams rule={.size=sizeof(rule)}; CHECK(!test_device_params(4,&rule));
        CHECK(rule.sectors_per_cluster==rules[i].spc && rule.root_entries==rules[i].root && rule.fat_bits==rules[i].bits && rule.media==rules[i].media);
        u32 clusters=(u32)((rules[i].sectors-rule.reserved_sectors-2*rule.sectors_per_fat-rule.root_entries/16)/rule.sectors_per_cluster);
        CHECK((u64)(clusters+2)*rule.fat_bits<=(u64)rule.sectors_per_fat*512*8 && (clusters<4085)==(rule.fat_bits==12));
    }
    fake.bytes=(u64)0x800001*512; test_units[3].flags=0; test_units[3].generation++;
    params=(DosDeviceParams){.size=sizeof(params)}; CHECK(test_device_params(4,&params)==DE_FORMAT);
    test_units[3].disk=&copies[1]; test_units[3].generation++;
    DosSectorIo sio={.size=sizeof(sio),.sector=0,.count=2,.buffer=sector};
    CHECK(!test_sector_io(3,0x61,&sio) && sio.done==2 && !memcmp(sector,copies[0].data,1024));
    CHECK(test_sector_io(3,0x41,&sio)==DE_ACCESS && !sio.done);
    sio=(DosSectorIo){.size=sizeof(sio),.sector=10,.count=3}; CHECK(!test_sector_io(3,0x62,&sio) && sio.done==3);
    CHECK(test_sector_io(3,0x42,&sio)==DE_ACCESS);
    CHECK(!dos_volume_lock(3,1));
    memset(copies[0].data+10*512,0x77,3*512); CHECK(!test_sector_io(3,0x42,&sio) && sio.done==3);
    for(unsigned i=0;i<3*512;i++) CHECK(!copies[0].data[10*512+i]);
    sio=(DosSectorIo){.size=sizeof(sio),.sector=total-1,.count=2}; CHECK(test_sector_io(3,0x62,&sio)==DE_SEEK && sio.done==1);
    memcpy(copies[0].data+10*512,md->data+10*512,3*512); CHECK(!dos_volume_lock(3,0));
    DosMediaId id={.size=sizeof(id)};
    CHECK(!test_media_id(2,&id,0) && id.serial==rd32(md->data+39) && !memcmp(id.label,md->data+43,11) && !memcmp(id.file_system,md->data+54,8));
    DosMediaId next=id; next.serial=0x12345678; memcpy(next.label,"MEDIA ID   ",11);
    CHECK(!test_media_id(2,&next,1) && rd32(md->data+39)==0x12345678 && !memcmp(md->data+43,"MEDIA ID   ",11));
    CHECK(!test_media_id(2,&id,1) && !memcmp(md->data,saved,512));
    copies[1].data[38]=0; test_units[2].generation++; CHECK(test_media_id(0,&id,0)==DE_FORMAT);
    copies[1].data[38]=0x29; test_units[2].generation++; CHECK(!test_media_id(0,&id,0));
    CHECK(maint_call(0x440d,3,0x0960,(uintptr_t)&params,NULL)==DE_FUNCTION && maint_call(0x440d,3,0x0899,(uintptr_t)&params,NULL)==DE_FUNCTION);
    CHECK(maint_call(0x440d,27,0x0860,(uintptr_t)&params,NULL)==DE_DRIVE);
    CHECK(!maint_call(0x440e,3,0,0,&r) && !r.ax && !maint_call(0x440f,0,0,0,&r) && !r.ax);
    CHECK(maint_call(0x440e,2,0,0,NULL)==DE_DRIVE);
    CHECK(!maint_call(0x4409,3,0,0,&r) && r.dx==0x42);
    CHECK(maint_call(0x4404,3,0,0,NULL)==DE_FUNCTION);
    CHECK(!dos_open("C:\\IOCTL.TXT",2,1,&h) && !maint_call(0x4400,h,0,0,&r)); u64 word=r.dx;
    CHECK(!maint_call(0x440a,h,0,0,&r) && r.dx==word && !(r.dx&0x8000));
    CHECK(!maint_call(0x440a,1,0,0,&r) && !(r.dx&0x8000) && (r.dx&0x80));
    /* Sharing retries are bounded and configurable. */
    CHECK(maint_call(0x440b,0,0,70000,NULL)==DE_FUNCTION && !maint_call(0x440b,0,0,1,NULL));
    unsigned denied; CHECK(!dos_close(h) && !dos_open("C:\\IOCTL.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h));
    CHECK(dos_open("C:\\IOCTL.TXT",DOS_OPEN_READ|DOS_SHARE_DENY_NONE,0,&denied)==DE_SHARE);
    CHECK(!maint_call(0x440b,0,1,3,NULL) && !dos_close(h) && !dos_remove("C:\\IOCTL.TXT",0));

    /* Physical disks: identity, drive membership, protected volumes. */
    DosPhysicalInfo disk_info; CHECK(!dos_physical_info(0,&disk_info));
    CHECK(disk_info.size==sizeof(disk_info) && disk_info.flags==DOS_DRIVE_PRESENT && disk_info.drives==(1U<<3) && disk_info.sectors==physical_disk.bytes/512);
    CHECK(dos_physical_info(1,&disk_info)==DE_DRIVE);
    CHECK(!dos_physical_read(0,0,1,sector,&n) && n==1 && sector[510]==0x55);
    sector[446+4]=0x06; CHECK(!dos_physical_write(0,0,1,sector,&n) && n==1 && physical_disk.data[446+4]==0x06);
    CHECK(dos_physical_write(0,physical_start-1,2,other,&n)==DE_ACCESS && n==1);
    CHECK(dos_physical_read(0,physical_disk.bytes/512,1,sector,&n)==DE_SEEK);
    unsigned before=restarts; dos_restart(); CHECK(restarts==before+1);

    /* Block-driver IOCTL read/write reaches resident drivers. */
    IoServices modules={.version=IO_ABI_VERSION,.size=sizeof(modules),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload}; driver_io=&modules; module_mode=6;
    CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("RAMDRV.SYS","/SIZE:2048 /UNITS:2"));
    RamDiskUnitInfo unit={0};
    CHECK(!maint_call(0x4404,5,sizeof(unit),(uintptr_t)&unit,&r) && r.ax==sizeof(unit));
    CHECK(unit.size==sizeof(unit) && unit.unit==1 && unit.sectors==4096 && !unit.readonly);
    CHECK(!maint_call(0x4409,5,0,0,&r) && r.dx==0x4042);
    u32 readonly=1; CHECK(!maint_call(0x4405,5,4,(uintptr_t)&readonly,&r) && r.ax==4);
    CHECK(!dos_drive_info(4,&info) && (info.flags&DOS_DRIVE_READONLY) && dos_mkdir("E:\\RO")==DE_READONLY);
    CHECK(maint_call(0x4405,5,3,(uintptr_t)&readonly,NULL)==DE_FUNCTION);
    readonly=0; CHECK(!maint_call(0x4405,5,4,(uintptr_t)&readonly,NULL) && !dos_mkdir("E:\\RW"));
    CHECK(!dos_volume_lock(4,1) && !dos_disk_read(4,0,1,sector,&n) && !memcmp(sector+3,"NATVRAM ",8));
    CHECK(!dos_volume_lock(4,0) && !dos_finish_drivers() && !module_pages);
    driver_io=NULL;
    CHECK(!memcmp(md->data,saved,512)); memcpy(md->data,saved,md->bytes); /* Only test files changed C:. */
    CHECK(!dos_init(disk,arena));
    for(unsigned i=0;i<ARRAY_SIZE(copies);i++) free(copies[i].data);
    free(physical_disk.data); free(saved); free(arena);
    puts("PASS maintenance: native INT 25h/26h, volume locks, DPB, 440Dh device/media IDs, physical disks, server and network calls");
}
/* Resident programs and two-step EXEC. LOOPDRV.SYS is a subsystem-11 image;
 * IO.SYS (mocked by host.c) decides what may stay loaded. */
static void *tsr_block;
static int tsr_error;
static u64 unloaded_resident;
static int resident_unload(void *context,u64 token) {(void)context; unloaded_resident=token; return 0;}
static int tsr_probe(void) {
    DosRegs r={.ax=0x4800,.bx=8}; CHECK(!call(&r)); tsr_block=(void *)(uintptr_t)r.ax;
    memcpy(tsr_block,"RESIDENT",8);
    r=(DosRegs){.ax=0x3107,.dx=1}; tsr_error=call(&r);
    return 0; /* Reached only when IO.SYS refuses residency. */
}
static u32 loaded_pid;
static int loaded_probe(void) {loaded_pid=dos_pid(); CHECK(!strcmp(dos_command_tail(),"loaded tail")); return 0;}
static void test_resident(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.module_unload=resident_unload}; driver_io=&io;
    CHECK(!dos_init(disk,arena));
    DosRegs r={.ax=0x3100}; CHECK(call(&r)==DE_FUNCTION); /* Not inside an EXEC. */
    u32 before,after; CHECK(!arena_check(&dos_arena,&before));
    exec_probe=tsr_probe; CHECK(!dos_exec("LOOPDRV.SYS",""));
    r=(DosRegs){.ax=0x4d00}; CHECK(!call(&r) && (r.ax&255)==7 && !memcmp(tsr_block,"RESIDENT",8));
    CHECK(!arena_check(&dos_arena,&after) && after<before && dos_drivers_pending());
    resident_refused=1; CHECK(!dos_exec("HELLO.EFI","") && tsr_error==DE_FORMAT);
    r=(DosRegs){.ax=0x4d00}; CHECK(!call(&r) && (r.ax&255)==37);
    u32 refused; CHECK(!arena_check(&dos_arena,&refused) && refused==after); resident_refused=0;
    CHECK(!dos_finish_drivers() && unloaded_resident==resident_next && !dos_drivers_pending());
    exec_probe=NULL;
    /* AH=4B01h loads a task without running it; 4B80h runs, 4B81h discards. */
    DosExecLoad load={.size=sizeof(load),.tail="loaded tail"};
    r=(DosRegs){.ax=0x4b01,.dx=(uintptr_t)"HELLO.EFI",.bx=(uintptr_t)&load}; CHECK(!call(&r));
    CHECK(load.pid && load.token && load.base==0x100000 && load.image_size && load.entry>load.base && image_loads==1);
    DosExecLoad second={.size=sizeof(second)};
    r=(DosRegs){.ax=0x4b01,.dx=(uintptr_t)"LOOPDRV.SYS",.bx=(uintptr_t)&second}; CHECK(!call(&r) && second.token!=load.token);
    r=(DosRegs){.ax=0x4b01,.dx=(uintptr_t)"README.TXT",.bx=(uintptr_t)&second}; CHECK(call(&r)==DE_FORMAT);
    u32 parent=dos_pid(),other; CHECK(!dos_task_create(&other) && !dos_task_select(other));
    r=(DosRegs){.ax=0x4b80,.bx=load.token}; CHECK(call(&r)==DE_HANDLE); CHECK(!dos_task_select(parent));
    exec_probe=loaded_probe;
    r=(DosRegs){.ax=0x4b80,.bx=load.token}; CHECK(!call(&r) && loaded_pid==load.pid && image_starts==1);
    r=(DosRegs){.ax=0x4d00}; CHECK(!call(&r) && (r.ax&255)==37);
    r=(DosRegs){.ax=0x4b80,.bx=load.token}; CHECK(call(&r)==DE_HANDLE);
    r=(DosRegs){.ax=0x4b81,.bx=second.token}; CHECK(!call(&r) && image_discards==1);
    CHECK(dos_task_select(second.pid)==DE_BLOCK && dos_task_select(load.pid)==DE_BLOCK);
    /* A task's unstarted images are discarded with it. */
    CHECK(!dos_task_select(other));
    r=(DosRegs){.ax=0x4b01,.dx=(uintptr_t)"HELLO.EFI",.bx=(uintptr_t)&load}; CHECK(!call(&r));
    CHECK(!dos_task_select(parent) && !dos_task_destroy(other) && image_discards==2 && dos_task_select(load.pid)==DE_BLOCK);
    exec_probe=NULL;
    /* AH=4B03h copies an overlay without a task. */
    unsigned h; u32 size,n; CHECK(!dos_open("README.TXT",0,0,&h) && !dos_seek(h,0,2,&size) && !dos_close(h));
    u8 *buffer=malloc(size+1),*expect=malloc(size); CHECK(buffer && expect);
    CHECK(!dos_open("README.TXT",0,0,&h) && !dos_read(h,expect,size,&n) && n==size && !dos_close(h));
    DosOverlay overlay={.size=sizeof(overlay),.capacity=size+1,.buffer=buffer};
    r=(DosRegs){.ax=0x4b03,.dx=(uintptr_t)"README.TXT",.bx=(uintptr_t)&overlay}; CHECK(!call(&r) && overlay.loaded==size && !memcmp(buffer,expect,size));
    overlay.capacity=size-1; r=(DosRegs){.ax=0x4b03,.dx=(uintptr_t)"README.TXT",.bx=(uintptr_t)&overlay};
    CHECK(call(&r)==DE_NOMEM && !overlay.loaded);
    r=(DosRegs){.ax=0x4b02,.dx=(uintptr_t)"README.TXT",.bx=(uintptr_t)&overlay}; CHECK(call(&r)==DE_FUNCTION);
    free(buffer); free(expect);
    driver_io=NULL; CHECK(!dos_init(disk,arena)); free(arena);
    puts("PASS resident: AH=31h keeps memory and image, refused residency, 4B01h/4B80h/4B81h ownership, 4B03h overlays");
}
