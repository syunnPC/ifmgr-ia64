/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "dos.h"
#include "config.h"
#include "device.h"
#include "../drivers/ports.h"
#include "../drivers/ramdisk.h"
#include "block.h"
#include "clock.h"
#include "nls.h"
#include "console.h"
#include "codepage.h"
static const IoServices *driver_io;
static const IoServices clock_io;
const IoServices *platform_io_services(void) {return driver_io?driver_io:&clock_io;}
int platform_module_load(const void *data,u32 size,u64 *token) {return driver_io->module_load(driver_io->context,data,size,token);}
int platform_module_unload(u64 token) {return driver_io->module_unload(driver_io->context,token);}
static unsigned checks;
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"FAIL %s:%u: %s\n",__FILE__,__LINE__,#x); exit(1);}} while(0)
static IoDateTime host_time={2026,9,29,12,32,0,0};
static unsigned clock_reads,clock_writes,clock_reentry;
static int clock_read_error,clock_write_error;
static void clock_probe(void) {
    if(!clock_reentry) return;
    DosDateTime t; CHECK(dos_get_datetime(&t)==DE_BUSY);
    DosRegs r={.ax=0x2d00}; dos_int21(&r); CHECK((r.flags&1) && r.ax==DE_BUSY);
    r=(DosRegs){.ax=0x6602,.bx=850}; dos_int21(&r); CHECK((r.flags&1) && r.ax==DE_BUSY);
    CHECK(dos_config_country(1,850,"C:\\COUNTRY.SYS")==DE_BUSY);
}
static int host_clock_get(void *context,IoDateTime *out) {
    (void)context; clock_reads++; clock_probe();
    if(clock_read_error) return clock_read_error;
    *out=host_time; return 0;
}
static int host_clock_set(void *context,const IoDateTime *t,u32 fields) {
    (void)context; clock_writes++; clock_probe();
    if(clock_write_error) return clock_write_error;
    if(fields&IO_CLOCK_DATE) {host_time.year=t->year; host_time.month=t->month; host_time.day=t->day;}
    if(fields&IO_CLOCK_TIME) {host_time.hour=t->hour; host_time.minute=t->minute; host_time.second=t->second; host_time.nanosecond=t->nanosecond;}
    return 0;
}
static int host_datetime(void *context,unsigned t[7]) {
    IoDateTime now; int e=host_clock_get(context,&now); if(e) return e;
    t[0]=now.year; t[1]=now.month; t[2]=now.day; t[3]=now.hour; t[4]=now.minute; t[5]=now.second; t[6]=99;
    return 0;
}
static const IoServices clock_io={.version=IO_ABI_VERSION,.size=sizeof(IoServices),.capabilities=IO_CAP_CLOCK,
    .datetime=host_datetime,.clock_get=host_clock_get,.clock_set=host_clock_set};
typedef struct {
    u8 *data; size_t bytes;
    int fail_write,fail_read,fail_flush;
    u32 fail_read_lba,fail_write_lba,flushes;
    unsigned reads,writes,fail_read_at,fail_write_at,fail_flush_at;
    unsigned failed_write_bytes;
    int fail_following_writes;
    unsigned corrupt_write_at;
} MemoryDisk;
static unsigned probe_reentry,reentry_hits;
static unsigned fat_pages,fat_allocations,fail_fat_allocation;
int platform_fat_page(void **p) {
    fat_allocations++;
    if(fail_fat_allocation && fat_allocations==fail_fat_allocation) return DE_NOMEM;
    *p=aligned_alloc(4096,4096); if(!*p) return DE_NOMEM;
    fat_pages++; return 0;
}
void platform_fat_free_page(void *p) {CHECK(fat_pages>0); fat_pages--; free(p);}
static int read_block(void *ctx,u32 lba,void *buf) {
    MemoryDisk *d=ctx; d->reads++;
    if(d->fail_read || (d->fail_read_lba && lba==d->fail_read_lba) ||
       (d->fail_read_at && d->reads==d->fail_read_at) || (u64)lba*512+512>d->bytes) return DE_IO;
    if(probe_reentry) {
        probe_reentry=0; reentry_hits++;
        u32 pid=dos_pid(),child=0; DosInfo info;
        CHECK(dos_task_select(pid)==DE_BUSY);
        CHECK(dos_task_create(&child)==DE_BUSY);
        CHECK(dos_task_destroy(pid)==DE_BUSY);
        CHECK(dos_query(&info)==DE_BUSY);
        char value[16];
        CHECK(dos_env_get("PATH",value,sizeof(value))==DE_BUSY);
        CHECK(dos_env_set("PROBE","bad")==DE_BUSY);
        CHECK(dos_env_list(0,value,sizeof(value))==DE_BUSY);
        DosRegs r={.ax=0x3000}; dos_int21(&r);
        CHECK((r.flags&1) && r.ax==DE_BUSY && dos_pid()==pid);
    }
    memcpy(buf,d->data+(u64)lba*512,512); return 0;
}
static int write_block(void *ctx,u32 lba,const void *buf) {
    MemoryDisk *d=ctx; d->writes++;
    if((u64)lba*512+512>d->bytes) return DE_IO;
    if(d->fail_write || (d->fail_write_lba && lba==d->fail_write_lba)) return DE_IO;
    if(d->fail_write_at && (d->writes==d->fail_write_at || (d->fail_following_writes && d->writes>d->fail_write_at))) {
        if(d->writes==d->fail_write_at) memcpy(d->data+(u64)lba*512,buf,d->failed_write_bytes);
        return DE_IO;
    }
    memcpy(d->data+(u64)lba*512,buf,512);
    if(d->corrupt_write_at && d->writes==d->corrupt_write_at) d->data[(u64)lba*512+17]^=0x5a;
    return 0;
}
static int flush_block(void *ctx) {
    MemoryDisk *d=ctx; d->flushes++;
    return d->fail_flush || (d->fail_flush_at && d->flushes==d->fail_flush_at)?DE_IO:0;
}
static IoEvent keys[1024];
static unsigned key_read,key_count;
static const char *delayed_keys;
static char console_output[16384];
static unsigned console_size;
static int capture_console;
static void key_append(const char *s) {
    while(*s) {CHECK(key_count<ARRAY_SIZE(keys)); keys[key_count++]=(IoEvent){.type=IO_EVENT_KEY,.unicode=(u8)*s++};}
}
static void key_string(const char *s) {key_read=key_count=0; key_append(s);}
static void key_scan(unsigned scan) {
    CHECK(key_count<ARRAY_SIZE(keys)); keys[key_count++]=(IoEvent){.type=IO_EVENT_KEY,.scan=scan};
}
int platform_console_key(IoEvent *out,unsigned flags) {
    if(key_read==key_count && (flags&IO_KEY_WAIT) && delayed_keys) {
        const char *s=delayed_keys; delayed_keys=NULL; key_string(s);
    }
    if(key_read==key_count) return flags&IO_KEY_WAIT?DE_EOF:DE_NOTREADY;
    *out=keys[key_read]; if(!(flags&IO_KEY_PEEK)) key_read++;
    return 0;
}
void con_write(const void *data,size_t n) {
    if(!capture_console) return;
    CHECK(console_size+n<sizeof(console_output)); memcpy(console_output+console_size,data,n);
    console_size+=n; console_output[console_size]=0;
}
void con_puts(const char *data) {con_write(data,strlen(data));}
/* UTF-16 console text is logged as code units and echoed as UTF-8 bytes. */
static u16 console_text[4096];
static unsigned console_text_size;
void platform_console_text(const u16 *text,size_t n) {
    for(size_t i=0;i<n;i++) {
        u16 c=text[i]; char bytes[3]; size_t m=0;
        if(capture_console) {CHECK(console_text_size<ARRAY_SIZE(console_text)); console_text[console_text_size++]=c;}
        if(c<0x80) bytes[m++]=(char)c;
        else if(c<0x800) {bytes[m++]=(char)(0xc0|c>>6); bytes[m++]=(char)(0x80|(c&63));}
        else {bytes[m++]=(char)(0xe0|c>>12); bytes[m++]=(char)(0x80|((c>>6)&63)); bytes[m++]=(char)(0x80|(c&63));}
        con_write(bytes,m);
    }
}
int con_getch(void) {return 26;}
void platform_wait(u32 ms) {(void)ms;}
void con_clear(void) {}
static int (*exec_probe)(void);
/* Like firmware Exit, a resident exit unwinds to the StartImage caller. */
static jmp_buf exec_jump;
static int exec_jump_ready,resident_refused;
static unsigned exec_exit_code,image_loads,image_starts,image_discards;
static u64 resident_next=0x7000;
static int run_probe(unsigned *r) {
    if(!exec_probe) return DE_FORMAT;
    *r=37;
    if(setjmp(exec_jump)) {exec_jump_ready=0; *r=exec_exit_code; return 0;}
    exec_jump_ready=1; int e=exec_probe(); exec_jump_ready=0; return e;
}
int platform_exec(const void *p,u32 n,const char *t,unsigned *r) {
    (void)p; (void)n; (void)t; return run_probe(r);
}
int platform_exit_resident(unsigned code,u64 *token) {
    if(!exec_jump_ready || resident_refused) return DE_FORMAT;
    *token=++resident_next; exec_exit_code=code; longjmp(exec_jump,1);
}
int platform_image_load(const void *image,u32 size,u64 *token,u64 *base,u64 *bytes) {
    CHECK(image && size>=64); image_loads++; *token=0x9000+image_loads; *base=0x100000; *bytes=size; return 0;
}
int platform_image_start(u64 token,const char *tail,unsigned *r) {
    CHECK(token>0x9000 && tail); image_starts++; return run_probe(r);
}
int platform_image_discard(u64 token) {CHECK(token>0x9000); image_discards++; return 0;}
void platform_exit(unsigned code) {(void)code; abort();}
static unsigned shutdowns;
void platform_shutdown(void) {shutdowns++;}
static unsigned restarts;
void platform_restart(void) {restarts++;}

static void test_arena(void) {
    _Alignas(16) u8 mem[4096]; Arena a; void *p,*q,*r; u32 max;
    for(unsigned method=0;method<3;method++) {
        arena_init(&a,mem,sizeof(mem)); a.method=method;
        CHECK(!arena_alloc(&a,17,10,&p,&max)); CHECK(!arena_alloc(&a,33,10,&q,&max));
        CHECK(p!=q && !((uintptr_t)p&15));
        CHECK(arena_free(&a,p,11)==DE_BLOCK);
        CHECK(arena_free(&a,(u8 *)p+1,10)==DE_BLOCK);
        CHECK(!arena_free(&a,p,10)); CHECK(arena_free(&a,p,10)==DE_BLOCK);
        CHECK(!arena_free_process(&a,10)); CHECK(!arena_check(&a,&max) && max==255);
        CHECK(arena_alloc(&a,256,10,&p,&max)==DE_NOMEM && max==255 && !p);
    }
    arena_init(&a,mem,sizeof(mem));
    CHECK(!arena_alloc(&a,20,1,&p,&max)); CHECK(!arena_alloc(&a,30,1,&q,&max));
    CHECK(!arena_alloc(&a,40,1,&r,&max)); CHECK(!arena_free(&a,q,1));
    CHECK(!arena_resize(&a,p,45,1,&max));
    CHECK(arena_resize(&a,p,100,1,&max)==DE_NOMEM && max==51);
    CHECK(!arena_resize(&a,p,5,1,&max)); CHECK(!arena_free(&a,r,1));
    CHECK(!arena_free(&a,p,1)); CHECK(!arena_check(&a,&max)&&max==255);
    /* A forged signature in user data is not sufficient to make a valid block. */
    CHECK(!arena_alloc(&a,32,1,&p,&max)); memset(p,0,32*16); ((u8 *)p)[8]='M';
    CHECK(arena_free(&a,(u8 *)p+16,1)==DE_BLOCK);
    mem[8]='?'; CHECK(arena_check(&a,&max)==DE_ARENA);
    /* First, best and high-end last fit choose observably different holes. */
    for(unsigned method=0;method<3;method++) {
        void *guard,*small,*guard2,*chosen;
        arena_init(&a,mem,sizeof(mem));
        CHECK(!arena_alloc(&a,40,1,&p,&max)); CHECK(!arena_alloc(&a,1,1,&guard,&max));
        CHECK(!arena_alloc(&a,20,1,&small,&max)); CHECK(!arena_alloc(&a,1,1,&guard2,&max));
        CHECK(!arena_free(&a,p,1)); CHECK(!arena_free(&a,small,1));
        a.method=method; CHECK(!arena_alloc(&a,16,1,&chosen,&max));
        CHECK(method==0?chosen==p:method==1?chosen==small:(u8 *)chosen> (u8 *)guard2);
    }
}
static void test_fat(Disk *disk,MemoryDisk *memory) {
    Fat f; CHECK(!fat_mount(&f,disk)); u32 free_before; CHECK(!fat_free_space(&f,&free_before));
    if(f.bits==12) {
        u16 a,b,c,v; CHECK(!fat_get(&f,340,&a)); CHECK(!fat_get(&f,341,&b)); CHECK(!fat_get(&f,342,&c));
        CHECK(!fat_set(&f,340,0x123)); CHECK(!fat_set(&f,342,0x789)); CHECK(!fat_set(&f,341,0x456));
        CHECK(!fat_get(&f,340,&v)&&v==0x123); CHECK(!fat_get(&f,341,&v)&&v==0x456); CHECK(!fat_get(&f,342,&v)&&v==0x789);
        CHECK(!fat_set(&f,340,a)); CHECK(!fat_set(&f,341,b)); CHECK(!fat_set(&f,342,c));
        CHECK(fat_eof(&f,0xff0)); CHECK(!fat_eof(&f,0xff7)); CHECK(fat_eof(&f,0xff8));
    }
    Node dir,file; CHECK(!fat_create(&f,"\\HOST",FA_DIR,&dir));
    CHECK(fat_create(&f,"\\HOST",FA_DIR,&dir)==DE_EXISTS);
    CHECK(!fat_create(&f,"\\HOST\\BIG.BIN",FA_ARCHIVE,&file));
    CHECK(fat_remove(&f,"\\HOST",1)==DE_ACCESS);
    FatFile ff={file,0}; u8 data[10013],out[10013];
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(i*31+i/251)&255;
    u32 done; CHECK(!fat_write(&f,&ff,data,sizeof(data),&done)&&done==sizeof(data));
    ff.pos=0; CHECK(!fat_read(&f,&ff,out,sizeof(out),&done)&&done==sizeof(out));
    CHECK(!memcmp(data,out,sizeof(data)));
    CHECK(!fat_truncate(&f,&ff,513)); CHECK(fat_size(&ff.node)==513);
    ff.pos=1999; CHECK(!fat_write(&f,&ff,data,5,&done)&&done==5);
    ff.pos=513; memset(out,1,sizeof(out)); CHECK(!fat_read(&f,&ff,out,1491,&done)&&done==1491);
    for(unsigned i=0;i<1486;i++) CHECK(!out[i]);
    CHECK(!memcmp(out+1486,data,5));
    ff.pos=2500; CHECK(!fat_write(&f,&ff,NULL,0,&done)); CHECK(fat_size(&ff.node)==2500);
    CHECK(!fat_truncate(&f,&ff,0)); CHECK(!fat_cluster(&ff.node));
    CHECK(!fat_remove(&f,"\\HOST\\BIG.BIN",0));
    /* More than one cluster of directory entries; reuse after deletion. */
    for(unsigned i=0;i<70;i++) {
        char name[32]; snprintf(name,sizeof(name),"\\HOST\\F%03u.DAT",i);
        CHECK(!fat_create(&f,name,FA_ARCHIVE,&file));
    }
    for(unsigned i=0;i<70;i++) {
        char name[32]; snprintf(name,sizeof(name),"\\HOST\\F%03u.DAT",i);
        CHECK(!fat_lookup(&f,name,&file)); CHECK(!fat_remove(&f,name,0));
    }
    CHECK(!fat_remove(&f,"\\HOST",1));
    u32 free_after; CHECK(!fat_free_space(&f,&free_after)&&free_before==free_after);
    CHECK(!memcmp(memory->data+f.fat_start*512,memory->data+(f.fat_start+f.fat_sectors)*512,f.fat_sectors*512));
    memory->fail_read=1; CHECK(fat_lookup(&f,"\\README.TXT",&file)==DE_IO); memory->fail_read=0;
    memory->fail_write=1; CHECK(fat_create(&f,"\\FAIL.TXT",0,&file)==DE_IO); memory->fail_write=0;
    CHECK(fat_lookup(&f,"\\FAIL.TXT",&file)==DE_NOFILE);
    f.disk.readonly=1; CHECK(fat_create(&f,"\\FAIL.TXT",0,&file)==DE_READONLY);
    u8 spc=memory->data[13]; memory->data[13]=3; CHECK(fat_mount(&f,disk)==DE_FORMAT); memory->data[13]=spc;
    CHECK(!fat_mount(&f,disk));
    /* A full disk returns an error without claiming an unwritten byte. */
    CHECK(!fat_create(&f,"\\FULL.TXT",0,&file)); ff=(FatFile){file,0};
    u8 *fat_backup=malloc(f.fat_sectors*512*f.copies); CHECK(fat_backup!=NULL);
    memcpy(fat_backup,memory->data+f.fat_start*512,f.fat_sectors*512*f.copies);
    for(u32 c=2;c<f.clusters+2;c++) {u16 v; CHECK(!fat_get(&f,c,&v)); if(!v) CHECK(!fat_set(&f,c,0xffff));}
    CHECK(fat_write(&f,&ff,data,1,&done)==DE_FULL && !done && !fat_size(&ff.node));
    memcpy(memory->data+f.fat_start*512,fat_backup,f.fat_sectors*512*f.copies); free(fat_backup);
    f.cache_valid=0; CHECK(!fat_remove(&f,"\\FULL.TXT",0));
}
static void test_kernel(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    char path[DOS_PATH_MAX]; CHECK(!dos_path("c:/FOO/../bar.txt",path)&&!strcmp(path,"\\BAR.TXT"));
    CHECK(dos_path("D:\\A",path)==DE_DRIVE); CHECK(dos_path("toolongname.txt",path)==DE_PATH);
    CHECK(!dos_mkdir("TREE")); CHECK(!dos_chdir("TREE")); CHECK(dos_remove(".",1)==DE_CURRENT);
    CHECK(dos_remove("C:\\TREE.",1)==DE_CURRENT);
    unsigned h; CHECK(!dos_open("NUL",2,0,&h)); u32 done;
    CHECK(!dos_write(h,"abc",3,&done)&&done==3); CHECK(!dos_read(h,path,3,&done)&&!done); CHECK(!dos_close(h));
    CHECK(!dos_open("A.TXT",2,2,&h));
    CHECK(!dos_write(h,"abc",3,&done)); CHECK(dos_remove("A.TXT",0)==DE_ACCESS);
    unsigned dup; CHECK(!dos_dup(h,&dup)); u32 pos; CHECK(!dos_seek(dup,1,0,&pos));
    CHECK(!dos_read(h,path,1,&done)&&done==1&&path[0]=='b');
    CHECK(dos_seek(h,-4,0,&pos)==DE_SEEK); CHECK(!dos_close(dup)); CHECK(!dos_close(h));
    CHECK(dos_close(h)==DE_HANDLE); CHECK(!dos_rename("A.TXT","B.TXT"));
    DosFind find; CHECK(!dos_find_first("*.TXT",0,&find)&&!strcmp(find.name,"B.TXT"));
    CHECK(dos_find_next(&find)==DE_NOMORE);
    unsigned handles[DOS_HANDLES-5];
    for(unsigned i=0;i<ARRAY_SIZE(handles);i++) CHECK(!dos_open("NUL",0,0,&handles[i]));
    CHECK(dos_open("MUSTNOT.TXT",2,1,&h)==DE_HANDLES);
    Node n; CHECK(fat_lookup(&dos_volume,"\\TREE\\MUSTNOT.TXT",&n)==DE_NOFILE);
    for(unsigned i=0;i<ARRAY_SIZE(handles);i++) CHECK(!dos_close(handles[i]));
    CHECK(!dos_remove("B.TXT",0)); CHECK(!dos_chdir("..")); CHECK(!dos_remove("TREE",1));
    free(arena);
}
static int call(DosRegs *r) {dos_int21(r); return r->flags&1?(int)r->ax:0;}
static void test_tasks(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    DosInfo before,info; CHECK(!dos_query(&before)); u32 root=before.pid,a,b,c;
    DosFind root_dta,a_dta;
    DosRegs r={.ax=0x1a00,.dx=(uintptr_t)&root_dta}; CHECK(!call(&r));
    CHECK(!dos_mkdir("TASKDIR"));
    unsigned h; u32 n,pos; char buf[DOS_PATH_MAX];
    CHECK(!dos_open("STATE.TXT",2,2,&h)); CHECK(!dos_write(h,"abcdef",6,&n));
    CHECK(!dos_seek(h,0,0,&pos)); CHECK(!dos_task_create(&a)); CHECK(!dos_task_create(&b));
    CHECK(a!=b && a!=root && b!=root);
    CHECK(!dos_task_select(a)); CHECK(!dos_chdir("TASKDIR"));
    dos_set_errorlevel(73);
    r=(DosRegs){.ax=0x1a00,.dx=(uintptr_t)&a_dta}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x5801,.bx=2}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x3e00,.bx=99}; CHECK(call(&r)==DE_HANDLE);
    r=(DosRegs){.ax=0x4800,.bx=31}; CHECK(!call(&r)); uintptr_t allocation=r.ax;
    {   /* MEM's walk of the arena: every block, this one the task's. */
        DosArenaBlock block={.size=sizeof(block)}; u64 bytes=0; int mine=0; u32 i=0;
        CHECK(!dos_query(&info));
        for(;!dos_arena_block(i,&block);i++) {
            bytes+=((u64)block.paragraphs+1)*16;
            if(block.kind==DOS_ARENA_DATA && block.paragraphs==31 && block.owner==info.pid) mine++;
        }
        CHECK(dos_arena_block(i,&block)==DE_NOMORE && bytes==DOS_ARENA_BYTES && mine==1);
        block.size=4; CHECK(dos_arena_block(0,&block)==DE_FUNCTION);
    }
    CHECK(!dos_read(h,buf,2,&n) && n==2 && !memcmp(buf,"ab",2));
    CHECK(!dos_task_select(b)); CHECK(!dos_path("",buf) && !strcmp(buf,"\\"));
    r=(DosRegs){.ax=0x5800}; CHECK(!call(&r) && r.ax==0);
    r=(DosRegs){.ax=0x2f00}; CHECK(!call(&r) && r.bx!=(uintptr_t)&a_dta && r.bx!=(uintptr_t)&root_dta);
    r=(DosRegs){.ax=0x5900}; CHECK(!call(&r) && !r.ax);
    CHECK(!dos_query(&info) && !info.errorlevel);
    CHECK(!dos_read(h,buf,2,&n) && n==2 && !memcmp(buf,"cd",2)); CHECK(!dos_close(h));
    CHECK(!dos_task_select(root));
    r=(DosRegs){.ax=0x2f00}; CHECK(!call(&r) && r.bx==(uintptr_t)&root_dta);
    CHECK(!dos_read(h,buf,2,&n) && n==2 && !memcmp(buf,"ef",2));
    r=(DosRegs){.ax=0x4900,.dx=allocation}; CHECK(call(&r)==DE_BLOCK);
    CHECK(dos_remove("TASKDIR",1)==DE_CURRENT); CHECK(dos_rename("TASKDIR","MOVED")==DE_CURRENT);
    CHECK(!dos_task_select(a)); CHECK(!dos_query(&info) && info.errorlevel==73);
    r=(DosRegs){.ax=0x5900}; CHECK(!call(&r) && r.ax==DE_HANDLE);
    r=(DosRegs){.ax=0x5800}; CHECK(!call(&r) && r.ax==2);
    r=(DosRegs){.ax=0x2f00}; CHECK(!call(&r) && r.bx==(uintptr_t)&a_dta);
    CHECK(!dos_task_create(&c)); CHECK(!dos_task_select(c));
    r=(DosRegs){.ax=0x4800,.bx=47}; CHECK(!call(&r));
    CHECK(dos_task_destroy(a)==DE_ACCESS); CHECK(dos_task_destroy(c)==DE_ACCESS);
    CHECK(!dos_task_select(root)); CHECK(dos_task_destroy(root)==DE_ACCESS);
    CHECK(!dos_close(h)); CHECK(dos_remove("STATE.TXT",0)==DE_ACCESS);
    CHECK(!dos_task_destroy(a)); CHECK(dos_task_select(a)==DE_BLOCK); CHECK(dos_task_select(c)==DE_BLOCK);
    CHECK(!dos_query(&info) && info.largest_paragraphs==before.largest_paragraphs);
    CHECK(!dos_task_destroy(b)); CHECK(!dos_remove("STATE.TXT",0)); CHECK(!dos_remove("TASKDIR",1));
    /* A callback during an INT 21h disk read must not reenter or change context. */
    CHECK(!dos_open("README.TXT",0,0,&h)); probe_reentry=1;
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=1,.dx=(uintptr_t)buf}; CHECK(!call(&r) && r.ax==1);
    CHECK(reentry_hits==1 && !probe_reentry && dos_pid()==root); CHECK(!dos_close(h));
    /* Query scans FAT too, so it needs the same non-reentrancy protection. */
    dos_volume.cache_valid=0; probe_reentry=1; CHECK(!dos_query(&info));
    CHECK(reentry_hits==2 && !probe_reentry);
    u32 ids[DOS_PROCESSES-1];
    for(unsigned i=0;i<ARRAY_SIZE(ids);i++) CHECK(!dos_task_create(&ids[i]));
    CHECK(dos_task_create(&a)==DE_NOMEM);
    for(unsigned i=0;i<ARRAY_SIZE(ids);i++) CHECK(!dos_task_destroy(ids[i]));
    CHECK(!dos_query(&info) && info.largest_paragraphs==before.largest_paragraphs);
    /* A reported flush failure must not keep a dead task's memory or locks. */
    CHECK(!dos_task_create(&a)); CHECK(!dos_task_select(a));
    r=(DosRegs){.ax=0x4800,.bx=64}; CHECK(!call(&r));
    CHECK(!dos_open("REAP.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&h));
    CHECK(!dos_write(h,"kept",4,&n)); CHECK(!dos_lock(h,0,0,4));
    CHECK(!dos_task_select(root)); MemoryDisk *backing=disk->ctx; backing->fail_flush=1;
    CHECK(dos_task_destroy(a)==DE_IO); backing->fail_flush=0;
    CHECK(dos_task_select(a)==DE_BLOCK);
    CHECK(!dos_query(&info) && info.largest_paragraphs==before.largest_paragraphs);
    CHECK(!dos_open("REAP.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h));
    CHECK(!dos_lock(h,0,0,4)); CHECK(!dos_read(h,buf,4,&n) && n==4 && !memcmp(buf,"kept",4));
    CHECK(!dos_close(h)); CHECK(!dos_remove("REAP.TMP",0));
    free(memory);
}
static int parse_config(DosConfig *c,const char *s) {
    char line[600]; CHECK(!strcopy(line,sizeof(line),s)); return config_line(c,line);
}
static void test_environment(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    char out[DOS_ENV_CAPACITY],large[DOS_ENV_CAPACITY];
    CHECK(!dos_env_get("comspec",out,sizeof(out)) && !strcmp(out,"C:\\COMMAND.COM"));
    CHECK(!dos_env_set("MiXeD","Keep Case"));
    CHECK(!dos_env_get("mixed",out,sizeof(out)) && !strcmp(out,"Keep Case"));
    CHECK(!dos_env_set("SECOND","unchanged"));
    CHECK(!dos_env_set("MIXED","a longer replacement"));
    CHECK(!dos_env_set("MIXED","x"));
    CHECK(!dos_env_get("SECOND",out,sizeof(out)) && !strcmp(out,"unchanged"));
    strcopy(out,sizeof(out),"sentinel");
    CHECK(dos_env_get("SECOND",out,2)==DE_ENV && !strcmp(out,"sentinel"));
    CHECK(dos_env_get("ABSENT",out,sizeof(out))==DE_NOFILE && !strcmp(out,"sentinel"));
    CHECK(dos_env_set("BAD=KEY","x")==DE_ENV); CHECK(dos_env_set("BAD KEY","x")==DE_ENV);
    CHECK(dos_env_set("","x")==DE_ENV); CHECK(dos_env_set(NULL,"x")==DE_ENV);
    memset(large,'a',sizeof(large)); large[sizeof(large)-1]=0;
    CHECK(dos_env_set("MIXED",large)==DE_ENV);
    CHECK(!dos_env_get("mixed",out,sizeof(out)) && !strcmp(out,"x"));
    unsigned entries=0;
    while(!dos_env_list(entries,out,sizeof(out))) entries++;
    CHECK(entries==4 && dos_env_list(entries,out,sizeof(out))==DE_NOMORE);
    CHECK(!dos_env_list(2,out,sizeof(out)) && !strcmp(out,"MIXED=x"));
    u32 parent=dos_pid(),child,grandchild;
    CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    CHECK(!dos_env_get("mixed",out,sizeof(out)) && !strcmp(out,"x"));
    CHECK(!dos_env_set("MIXED","child")); CHECK(!dos_env_set("SECOND",NULL));
    CHECK(!dos_task_create(&grandchild)); CHECK(!dos_task_select(grandchild));
    CHECK(!dos_env_get("mixed",out,sizeof(out)) && !strcmp(out,"child"));
    CHECK(!dos_task_select(parent));
    CHECK(!dos_env_get("mixed",out,sizeof(out)) && !strcmp(out,"x"));
    CHECK(!dos_env_get("second",out,sizeof(out)) && !strcmp(out,"unchanged"));
    CHECK(!dos_task_destroy(child));
    CHECK(!dos_env_set("mixed","")); CHECK(!dos_env_set("mixed",NULL));
    CHECK(!dos_env_set("SECOND","")); CHECK(!dos_env_set("COMSPEC","")); CHECK(!dos_env_set("PATH",""));
    CHECK(dos_env_list(0,out,sizeof(out))==DE_NOMORE);
    /* Fill the block exactly, then prove a failed growth preserves the value. */
    memset(large,'z',sizeof(large)); large[DOS_ENV_CAPACITY-4]=0;
    CHECK(!dos_env_set("K",large)); CHECK(dos_env_set("B","x")==DE_ENV);
    CHECK(!dos_env_get("K",out,sizeof(out)) && strlen(out)==DOS_ENV_CAPACITY-4);
    CHECK(!dos_env_set("K","short")); CHECK(!dos_env_set("B","now fits"));
    free(memory);
}
static void test_config(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    DosConfig config; config_defaults(&config);
    CHECK(config.files==DOS_DEFAULT_FILES && !strcmp(config.tail,"/P"));
    CHECK(!config.break_check); CHECK(!parse_config(&config,"BREAK=ON") && config.break_check);
    CHECK(parse_config(&config,"BREAK=MAYBE")==DE_FUNCTION && config.break_check);
    CHECK(!parse_config(&config,"break off") && !config.break_check);
    CHECK(!parse_config(&config,"  files = 8  ")); CHECK(config.files==8);
    CHECK(!parse_config(&config,"FILES 255")); CHECK(config.files==255);
    CHECK(parse_config(&config,"FILES=256")==DE_FUNCTION && config.files==255);
    CHECK(parse_config(&config,"FILES=7")==DE_FUNCTION); CHECK(parse_config(&config,"FILES=32garbage")==DE_FUNCTION);
    CHECK(parse_config(&config,"FILES=9999999999999999999999999")==DE_FUNCTION);
    CHECK(!parse_config(&config,"SHELL=\\TOOLS\\ALT.COM /C echo Config"));
    CHECK(!strcmp(config.shell,"C:\\TOOLS\\ALT.COM") && !strcmp(config.tail,"/C echo Config"));
    CHECK(parse_config(&config,"SHELL=\"C:\\BAD.COM\"junk /P")==DE_PATH);
    CHECK(!strcmp(config.shell,"C:\\TOOLS\\ALT.COM"));
    CHECK(!parse_config(&config,"shell \"C:\\COMMAND.COM\" /P"));
    CHECK(parse_config(&config,"SHELL=")==DE_ENV); CHECK(!parse_config(&config,"DEVICE=TEST.SYS /MODE=1"));
    CHECK(config.device_count==1 && !strcmp(config.devices[0].path,"C:\\TEST.SYS") && !strcmp(config.devices[0].tail,"/MODE=1"));
    CHECK(!config.devices[0].install);
    CHECK(!parse_config(&config,"INSTALL=SHARE.EXE /F:2048") && config.device_count==2 && config.devices[1].install);
    CHECK(!strcmp(config.devices[1].path,"C:\\SHARE.EXE") && !strcmp(config.devices[1].tail,"/F:2048"));
    config.device_count=1;
    CHECK(parse_config(&config,"DEVICE=")==DE_ENV && config.device_count==1);
    CHECK(parse_config(&config,"DEVICE=\"TEST.SYS\"junk")==DE_PATH && config.device_count==1);
    for(unsigned i=1;i<DOS_CONFIG_DEVICES;i++) CHECK(!parse_config(&config,"DEVICE=TEST.SYS"));
    CHECK(parse_config(&config,"DEVICE=TEST.SYS")==DE_NOMEM && config.device_count==DOS_CONFIG_DEVICES);
    CHECK(!parse_config(&config,"REM anything")); CHECK(!parse_config(&config,"; comment"));
    /* Exercise the actual file reader, line boundaries, invalid lines and ^Z. */
    unsigned h; u32 n;
    CHECK(!dos_open("CONFIG.SYS",2,1,&h));
    const char *text="REM test\r\nFILES=8\r\nFILES=bad\nDEVICE=NOPE.SYS\rSHELL=C:\\HELLO.EFI chosen\n";
    CHECK(!dos_write(h,text,strlen(text),&n));
    char oversized[700]; memset(oversized,'A',sizeof(oversized));
    CHECK(!dos_write(h,oversized,sizeof(oversized),&n));
    text="\nFILES=40\n\x1a" "FILES=9\n";
    CHECK(!dos_write(h,text,strlen(text),&n)); CHECK(!dos_close(h));
    CHECK(!config_load(&config));
    CHECK(config.files==40 && config.warnings==2 && !strcmp(config.shell,"C:\\HELLO.EFI") && !strcmp(config.tail,"chosen"));
    CHECK(config.device_count==1 && config.devices[0].line==4 && !strcmp(config.devices[0].path,"C:\\NOPE.SYS"));
    CHECK(!dos_remove("CONFIG.SYS",0)); CHECK(!config_load(&config));
    CHECK(config.files==DOS_DEFAULT_FILES && !config.warnings && !strcmp(config.tail,"/P"));
    CHECK(!dos_open("CONFIG.SYS",2,1,&h));
    text="DEVICE=C:\\\x81""a.SYS\nCOUNTRY=81,932\nCOUNTRY=999,932\n";
    CHECK(!dos_write(h,text,strlen(text),&n) && !dos_close(h) && !config_load(&config));
    CHECK(config.country==81 && config.code_page==932 && config.warnings==1);
    CHECK(config.device_count==1 && config.devices[0].line==1 && !strcmp(config.devices[0].path,"C:\\\x81""a.SYS"));
    /* Restore the fixture so later tests can also load it. */
    CHECK(!dos_open("CONFIG.SYS",2,1,&h)); text="FILES=64\nSHELL=C:\\COMMAND.COM /P\n";
    CHECK(!dos_write(h,text,strlen(text),&n)); CHECK(!dos_close(h));
    free(memory);
}
static void test_handle_limits(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    CHECK(dos_set_files(7)==DE_FUNCTION); CHECK(!dos_set_files(8));
    unsigned handles[40],h;
    for(unsigned i=0;i<3;i++) CHECK(!dos_open("NUL",2,0,&handles[i]));
    CHECK(dos_open("LIMIT.TXT",2,1,&h)==DE_HANDLES);
    Node node; CHECK(fat_lookup(&dos_volume,"\\LIMIT.TXT",&node)==DE_NOFILE);
    u32 root=dos_pid(),child;
    CHECK(!dos_task_create(&child)); CHECK(dos_set_files(64)==DE_BUSY);
    CHECK(!dos_task_select(child)); CHECK(dos_open("NUL",0,0,&h)==DE_HANDLES);
    CHECK(!dos_close(handles[0])); CHECK(dos_open("NUL",0,0,&h)==DE_HANDLES);
    CHECK(!dos_task_select(root)); CHECK(!dos_task_destroy(child));
    for(unsigned i=0;i<3;i++) CHECK(!dos_close(handles[i]));
    CHECK(!dos_set_files(DOS_DEFAULT_FILES));
    DosRegs r={.ax=0x6700,.bx=40}; CHECK(!call(&r));
    for(unsigned i=0;i<35;i++) CHECK(!dos_open("NUL",2,0,&handles[i]));
    CHECK(handles[34]==39 && dos_open("NUL",0,0,&h)==DE_HANDLES);
    r=(DosRegs){.ax=0x6700,.bx=20}; CHECK(call(&r)==DE_HANDLES);
    CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    CHECK(!dos_close(39)); CHECK(!dos_open("NUL",0,0,&h) && h==39);
    CHECK(!dos_task_select(root)); CHECK(!dos_task_destroy(child));
    for(unsigned i=15;i<35;i++) CHECK(!dos_close(handles[i]));
    r=(DosRegs){.ax=0x6700,.bx=0}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x4400,.bx=20}; CHECK(call(&r)==DE_HANDLE);
    CHECK(dos_open("NUL",0,0,&h)==DE_HANDLES);
    for(unsigned i=0;i<15;i++) CHECK(!dos_close(handles[i]));
    r=(DosRegs){.ax=0x6700,.bx=256}; CHECK(call(&r)==DE_NOMEM);
    r=(DosRegs){.ax=0x6700,.bx=65535}; CHECK(call(&r)==DE_FUNCTION);
    r=(DosRegs){.ax=0x6700,.bx=255}; CHECK(!call(&r));
    CHECK(!dos_dup2(0,254)); CHECK(!dos_close(254));
    free(memory);
}
static unsigned share_mode(unsigned index) {return (index/3)*16+index%3;}
static void test_sharing(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    unsigned a,b; u32 n,pos,root=dos_pid(),child; char buf[32];
    CHECK(!dos_open("SHARE.DAT",2,2,&a)); CHECK(!dos_write(a,"abcdef",6,&n)); CHECK(!dos_close(a));
    CHECK(!dos_task_create(&child));
    /* Independent oracle: CUCA from Microsoft DOS 4 CMD/SHARE/GSHARE.ASM
     * (Microsoft copyright, MIT; license in vendor/msdos4). Same-user
     * compatibility pairs are accepted before looking up this table. */
    static const u16 matrix[]={0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,
        0xdf7f,0xdbff,0xdfff,0xbeff,0xb7ff,0xbfff,0x1c7f,0x03ff,0x1fff};
    for(unsigned other=0;other<2;other++) for(unsigned old=0;old<15;old++) for(unsigned new=0;new<15;new++) {
        CHECK(!dos_open("SHARE.DAT",share_mode(old),0,&a));
        if(other) CHECK(!dos_task_select(child));
        int expected=(old<3 && new<3) || !(matrix[new]&(1U<<(old+1)));
        int e=dos_open("share.dat",share_mode(new),0,&b);
        CHECK(e==(expected?0:DE_SHARE)); if(!e) CHECK(!dos_close(b));
        if(other) CHECK(!dos_task_select(root));
        CHECK(!dos_close(a));
    }
    CHECK(!dos_task_destroy(child));
    u8 attr=FA_RDONLY; CHECK(!dos_attribute("SHARE.DAT",1,&attr));
    for(unsigned old=0;old<5;old++) for(unsigned new=0;new<5;new++) {
        CHECK(!dos_open("SHARE.DAT",old*16,0,&a));
        unsigned x=old?old*3:6,y=new?new*3:6;
        int expected=!(matrix[y]&(1U<<(x+1))),e=dos_open("SHARE.DAT",new*16,0,&b);
        CHECK(e==(expected?0:DE_SHARE)); if(!e) CHECK(!dos_close(b));
        CHECK(!dos_close(a));
    }
    CHECK(dos_open("SHARE.DAT",0x41,0,&a)==DE_ACCESS);
    attr=0; CHECK(!dos_attribute("SHARE.DAT",1,&attr));
    CHECK(!dos_open("SHARE.DAT",0x42,0,&a)); CHECK(!dos_open("SHARE.DAT",0x42,0,&b));
    CHECK(!dos_seek(a,0,2,&pos) && pos==6); CHECK(!dos_write(a,"ghij",4,&n));
    CHECK(!dos_seek(b,0,2,&pos) && pos==10);
    CHECK(!dos_seek(b,0,0,&pos)); CHECK(!dos_read(b,buf,10,&n) && n==10 && !memcmp(buf,"abcdefghij",10));
    CHECK(!dos_seek(b,3,0,&pos)); CHECK(!dos_write(b,NULL,0,&n));
    CHECK(!dos_seek(a,0,2,&pos) && pos==3);
    CHECK(!dos_write(a,"xyz",3,&n)); CHECK(!dos_seek(b,0,0,&pos));
    CHECK(!dos_read(b,buf,10,&n) && n==6 && !memcmp(buf,"abcxyz",6));
    CHECK(!dos_seek(a,0,0,&pos)); CHECK(!dos_write(a,NULL,0,&n));
    CHECK(!dos_seek(b,0,2,&pos) && !pos); CHECK(!dos_write(b,"new chain",9,&n));
    CHECK(!dos_read(a,buf,9,&n) && n==9 && !memcmp(buf,"new chain",9));
    CHECK(!dos_close(a)); CHECK(!dos_close(b));
    CHECK(!dos_open("SHARE.DAT",0x20,0,&a));
    unsigned result; CHECK(dos_open_ex("SHARE.DAT",0x40,0,2,&b,&result)==DE_SHARE);
    CHECK(!dos_seek(a,0,2,&pos) && pos==9); CHECK(!dos_close(a));
    CHECK(!dos_remove("SHARE.DAT",0)); free(memory);
}
static void test_locks(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    unsigned a,b,dup; u32 n,pos,root=dos_pid(),child; char buf[32];
    CHECK(!dos_open("LOCK.DAT",0x42,2,&a)); CHECK(!dos_write(a,"0123456789abcdef",16,&n));
    CHECK(!dos_open("LOCK.DAT",0x42,0,&b)); CHECK(!dos_dup(a,&dup));
    CHECK(!dos_lock(a,0,2,2)); CHECK(dos_lock(a,0,2,2)==DE_LOCK);
    CHECK(dos_lock(b,0,3,2)==DE_LOCK); CHECK(dos_lock(b,1,2,2)==DE_LOCK);
    CHECK(dos_lock(a,1,2,1)==DE_LOCK); CHECK(dos_lock(a,0,0,0)==DE_LOCK);
    CHECK(dos_lock(a,0,UINT32_MAX,2)==DE_LOCK);
    CHECK(!dos_lock(a,0,UINT32_MAX,1)); CHECK(!dos_lock(a,1,UINT32_MAX,1));
    CHECK(!dos_lock(a,0,4,2)); CHECK(dos_lock(a,1,2,4)==DE_LOCK); CHECK(!dos_lock(a,1,4,2));
    CHECK(dos_read(b,buf,6,&n)==DE_LOCK && !n);
    CHECK(!dos_seek(b,0,1,&pos) && !pos);
    CHECK(!dos_seek(dup,2,0,&pos)); CHECK(!dos_read(dup,buf,2,&n) && n==2 && !memcmp(buf,"23",2));
    CHECK(!dos_close(a)); CHECK(dos_read(b,buf,6,&n)==DE_LOCK);
    CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    CHECK(!dos_seek(dup,2,0,&pos)); CHECK(dos_read(dup,buf,2,&n)==DE_LOCK && !n);
    CHECK(dos_lock(dup,1,2,2)==DE_LOCK); CHECK(!dos_lock(b,0,8,2));
    CHECK(!dos_task_select(root)); CHECK(!dos_seek(b,8,0,&pos)); CHECK(dos_read(b,buf,2,&n)==DE_LOCK);
    CHECK(!dos_task_destroy(child)); CHECK(!dos_read(b,buf,2,&n) && n==2);
    CHECK(!dos_close(dup)); CHECK(!dos_seek(b,2,0,&pos)); CHECK(!dos_read(b,buf,2,&n) && n==2);
    CHECK(!dos_open("LOCK.DAT",0x42,0,&a));
    CHECK(!dos_lock(a,0,5,1)); CHECK(!dos_seek(b,4,0,&pos));
    CHECK(dos_write(b,NULL,0,&n)==DE_LOCK && !n);
    CHECK(!dos_seek(b,0,2,&pos) && pos==16);
    CHECK(!dos_lock(a,1,5,1)); CHECK(!dos_lock(a,0,20,1));
    CHECK(!dos_seek(b,22,0,&pos)); CHECK(dos_write(b,"x",1,&n)==DE_LOCK && !n);
    CHECK(!dos_seek(b,0,2,&pos) && pos==16);
    CHECK(!dos_seek(b,16,0,&pos)); CHECK(!dos_read(b,buf,16,&n) && !n);
    unsigned fresh,result;
    CHECK(dos_open_ex("LOCK.DAT",0x42,0,2,&fresh,&result)==DE_LOCK);
    CHECK(!dos_lock(a,1,20,1));
    for(unsigned i=0;i<DOS_MAX_LOCKS;i++) CHECK(!dos_lock(a,0,i*2+100,1));
    CHECK(dos_lock(a,0,9999,1)==DE_LOCKS);
    CHECK(!dos_close(a)); CHECK(!dos_lock(b,0,100,1)); CHECK(!dos_lock(b,1,100,1));
    /* Closing the owner's last handle releases locks even if an inherited
     * reference keeps the underlying open description alive. */
    CHECK(!dos_lock(b,0,0,1)); CHECK(!dos_task_create(&child)); CHECK(!dos_close(b));
    CHECK(!dos_task_select(child)); CHECK(!dos_seek(b,0,0,&pos)); CHECK(!dos_read(b,buf,1,&n) && n==1);
    CHECK(!dos_task_select(root)); CHECK(!dos_task_destroy(child));
    CHECK(!dos_open("LOCK.DAT",0xc2,0,&a)); CHECK(!dos_task_create(&child));
    CHECK(!dos_task_select(child)); CHECK(dos_close(a)==DE_HANDLE);
    CHECK(!dos_task_select(root)); CHECK(!dos_task_destroy(child)); CHECK(!dos_close(a));
    CHECK(!dos_remove("LOCK.DAT",0)); free(memory);
}
static void test_file_api(Disk *disk,MemoryDisk *md) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    unsigned h,other,status; u32 n,pos; char buf[DOS_PATH_MAX];
    const unsigned actions[]={1,2,0x10,0x11,0x12};
    for(unsigned exists=0;exists<2;exists++) for(unsigned i=0;i<ARRAY_SIZE(actions);i++) {
        if(exists) {CHECK(!dos_open("EXT.DAT",2,2,&h)); CHECK(!dos_write(h,"keep",4,&n)); CHECK(!dos_close(h));}
        DosRegs r={.ax=0x6c00,.bx=0x42,.cx=FA_HIDDEN,.dx=actions[i],.si=(uintptr_t)"EXT.DAT"};
        int e=call(&r),expected=exists?((actions[i]&15)?0:DE_EXISTS):(actions[i]&0x10)?0:DE_NOFILE;
        CHECK(e==expected);
        if(!e) {
            CHECK(r.cx==(exists?((actions[i]&15)==2?3U:1U):2U)); h=r.ax;
            CHECK(!dos_seek(h,0,2,&pos) && pos==(exists && (actions[i]&15)==1?4U:0U)); CHECK(!dos_close(h));
        }
        if(exists || !e) CHECK(!dos_remove("EXT.DAT",0));
    }
    CHECK(!dos_open_ex("EXT.DAT",0x42,0,0x10,&h,&status)); CHECK(status==2);
    CHECK(!dos_write(h,"keep",4,&n)); CHECK(!dos_close(h));
    const unsigned bad_modes[]={3,4,8,0x50,0x60,0x70,0x100,0x8000};
    for(unsigned i=0;i<ARRAY_SIZE(bad_modes);i++) {
        CHECK(dos_open_ex("EXT.DAT",bad_modes[i],0,2,&h,&status)==DE_MODE);
        if(bad_modes[i]<256) {DosRegs r={.ax=0x3d00|bad_modes[i],.dx=(uintptr_t)"EXT.DAT"}; CHECK(call(&r)==DE_MODE);}
    }
    const unsigned bad_actions[]={0,3,4,0x13,0x20,0x100,0x200};
    for(unsigned i=0;i<ARRAY_SIZE(bad_actions);i++) CHECK(dos_open_ex("EXT.DAT",2,0,bad_actions[i],&h,&status)==DE_FUNCTION);
    CHECK(dos_open_ex("EXT.DAT",2,FA_DIR,2,&h,&status)==DE_ACCESS);
    CHECK(!dos_open("EXT.DAT",0x40,0,&h)); CHECK(!dos_read(h,buf,4,&n) && n==4 && !memcmp(buf,"keep",4));
    CHECK(!dos_open("EXT.DAT",0x40,0,&other));
    u16 date=0x5d3d,time=0x4321,d,t; CHECK(!dos_file_time(h,1,&date,&time));
    CHECK(!dos_file_time(other,0,&d,&t) && d==date && t==time);
    md->fail_write=1; date=0x1234; CHECK(dos_file_time(h,1,&date,&time)==DE_IO); md->fail_write=0;
    CHECK(!dos_file_time(h,0,&d,&t) && d==0x5d3d && t==time);
    CHECK(!dos_close(h)); CHECK(!dos_close(other));
    CHECK(!dos_open("EXT.DAT",0,0,&h)); CHECK(!dos_file_time(h,0,&d,&t) && d==0x5d3d && t==time); CHECK(!dos_close(h));
    unsigned handles[DOS_HANDLES-5];
    for(unsigned i=0;i<ARRAY_SIZE(handles);i++) CHECK(!dos_open("NUL",0,0,&handles[i]));
    CHECK(dos_open_ex("EXT.DAT",2,0,2,&h,&status)==DE_HANDLES);
    for(unsigned i=0;i<ARRAY_SIZE(handles);i++) CHECK(!dos_close(handles[i]));
    CHECK(!dos_open("EXT.DAT",0,0,&h)); CHECK(!dos_seek(h,0,2,&pos) && pos==4); CHECK(!dos_close(h));
    CHECK(!dos_remove("EXT.DAT",0));
    CHECK(!dos_open_ex("READONLY.DAT",2,FA_RDONLY,0x10,&h,&status));
    CHECK(!dos_write(h,"data",4,&n) && n==4); CHECK(!dos_close(h));
    CHECK(dos_open("READONLY.DAT",2,0,&h)==DE_ACCESS);
    u8 attr=0; CHECK(!dos_attribute("READONLY.DAT",1,&attr)); CHECK(!dos_remove("READONLY.DAT",0));
    char canonical[DOS_PATH_MAX]; CHECK(!dos_mkdir("CANON")); CHECK(!dos_chdir("CANON"));
    CHECK(!dos_canonical("c:.././CANON./FILE.",canonical) && !strcmp(canonical,"C:\\CANON\\FILE"));
    CHECK(!dos_chdir("\\")); CHECK(!dos_remove("CANON",1));
    CHECK(!dos_open_ex("PARTIAL.DAT",0x42|DOS_OPEN_COMMIT|DOS_OPEN_FAIL_ERRORS,0,0x110,&h,&status));
    u8 data[1024]; memset(data,'p',sizeof(data)); unsigned flushes=md->flushes;
    CHECK(!dos_write(h,data,sizeof(data),&n) && n==sizeof(data) && md->flushes>flushes);
    CHECK(!dos_open("PARTIAL.DAT",0x42,0,&other));
    CHECK(!dos_seek(h,0,0,&pos)); md->fail_write=1;
    CHECK(dos_write(h,NULL,0,&n)==DE_IO && !n); md->fail_write=0;
    CHECK(!dos_seek(other,0,2,&pos) && pos==sizeof(data)); CHECK(!dos_close(other));
    Node node; CHECK(!fat_lookup(&dos_volume,"\\PARTIAL.DAT",&node));
    u16 c=fat_cluster(&node),next=c; if(dos_volume.spc==1) CHECK(!fat_get(&dos_volume,c,&next));
    u32 second=dos_volume.data_start+(next-2)*dos_volume.spc+(dos_volume.spc==1?0:1);
    md->fail_read_lba=second; CHECK(!dos_seek(h,0,0,&pos));
    DosRegs r={.ax=0x3f00,.bx=h,.cx=sizeof(data),.dx=(uintptr_t)data};
    CHECK(call(&r)==DE_IO && r.cx==512); CHECK(!dos_seek(h,0,1,&pos) && pos==512); md->fail_read_lba=0;
    md->fail_write_lba=second; CHECK(!dos_seek(h,0,0,&pos));
    memset(data,'q',sizeof(data));
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=sizeof(data),.dx=(uintptr_t)data};
    CHECK(call(&r)==DE_IO && r.cx==512); CHECK(!dos_seek(h,0,1,&pos) && pos==512); md->fail_write_lba=0;
    r=(DosRegs){.ax=0x3e00,.bx=(1ULL<<32)|h}; CHECK(call(&r)==DE_HANDLE);
    r=(DosRegs){.ax=0x3f00,.bx=(1ULL<<32)|h,.cx=4,.dx=(uintptr_t)data}; CHECK(call(&r)==DE_HANDLE && !r.cx);
    r=(DosRegs){.ax=0x4600,.bx=h,.cx=(1ULL<<32)|1}; CHECK(call(&r)==DE_HANDLE);
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=1ULL<<32,.dx=(uintptr_t)data}; CHECK(call(&r)==DE_FUNCTION && !r.cx);
    CHECK(!dos_lock(h,0,0,1)); md->fail_flush=1; CHECK(dos_close(h)==DE_IO); md->fail_flush=0;
    CHECK(!dos_open("PARTIAL.DAT",2,0,&h)); CHECK(!dos_lock(h,0,0,1)); CHECK(!dos_close(h));
    CHECK(!dos_remove("PARTIAL.DAT",0)); free(memory);
}
static void reset_faults(MemoryDisk *md) {
    *md=(MemoryDisk){.data=md->data,.bytes=md->bytes};
    fat_allocations=0; fail_fat_allocation=0;
}
static int fault_operation(Fat *f,unsigned op,FatFile *file) {
    Node node; u8 data[512]; memset(data,0x6d,sizeof(data)); u32 done;
    switch(op) {
    case 0: {u16 value; int e=fat_get(f,341,&value); return e?e:fat_set(f,341,value^0x123);}
    case 1: return fat_create(f,"\\TXNB\\NEW.DAT",0,&node);
    case 2: return fat_create(f,"\\TXNB\\NEW",FA_DIR,&node);
    case 3: return fat_create(f,"\\TXFULL\\NEXT.DAT",0,&node);
    case 4: case 5: return fat_write(f,file,data,sizeof(data),&done);
    case 6: return fat_truncate(f,file,513);
    case 7: return fat_replace(f,file,FA_RDONLY|FA_HIDDEN);
    case 8: return fat_remove(f,"\\TXNA\\DATA.BIN",0);
    case 9: return fat_remove(f,"\\TXNA\\EMPTY",1);
    case 10: return fat_rename(f,"\\TXNA\\DATA.BIN","\\TXNA\\RENAME.BIN");
    case 11: return fat_rename(f,"\\TXNA\\DATA.BIN","\\TXNB\\MOVE.BIN");
    case 12: return fat_rename(f,"\\TXNA\\EMPTY","\\TXNB\\MOVED");
    case 13: node=file->node; wr16(node.raw+22,0x4567); return fat_sync_node(f,&node);
    case 14: return fat_truncate(f,file,4166);
    case 15: return fat_rename(f,"\\TXNA\\EMPTY","\\TXFULL\\MOVED");
    default: return DE_FUNCTION;
    }
}
static void test_transactions(Disk *disk,MemoryDisk *md) {
    Fat f; Node node,source; CHECK(!fat_mount(&f,disk)); u32 before_free;
    CHECK(!fat_free_space(&f,&before_free));
    CHECK(!fat_create(&f,"\\TXNA",FA_DIR,&node)); CHECK(!fat_create(&f,"\\TXNB",FA_DIR,&node));
    CHECK(!fat_create(&f,"\\TXFULL",FA_DIR,&node));
    CHECK(!fat_create(&f,"\\TXNA\\EMPTY",FA_DIR,&node));
    CHECK(!fat_create(&f,"\\TXNA\\DATA.BIN",0,&source));
    FatFile file={source,0}; u8 data[4096]; memset(data,0x35,sizeof(data)); u32 done;
    CHECK(!fat_write(&f,&file,data,sizeof(data),&done) && done==sizeof(data)); source=file.node;
    for(unsigned i=0;i<f.spc*16U-2;i++) {
        char name[40]; snprintf(name,sizeof(name),"\\TXFULL\\F%03u.DAT",i);
        CHECK(!fat_create(&f,name,0,&node));
    }
    u8 *snapshot=malloc(md->bytes); CHECK(snapshot!=NULL); memcpy(snapshot,md->data,md->bytes);
    unsigned scenarios=0;
    for(unsigned op=0;op<16;op++) {
        reset_faults(md); memcpy(md->data,snapshot,md->bytes); CHECK(!fat_mount(&f,disk)); reset_faults(md);
        file=(FatFile){source,op==5?4096:0}; CHECK(!fault_operation(&f,op,&file));
        unsigned limits[]={md->reads,md->writes,md->writes,md->writes,md->flushes,fat_allocations};
        CHECK(limits[0] && limits[1] && limits[4] && limits[5]);
        for(unsigned mode=0;mode<ARRAY_SIZE(limits);mode++) for(unsigned at=1;at<=limits[mode];at++) {
            reset_faults(md); memcpy(md->data,snapshot,md->bytes); CHECK(!fat_mount(&f,disk)); reset_faults(md);
            file=(FatFile){source,op==5?4096:0}; FatFile original=file;
            if(mode==0) md->fail_read_at=at;
            else if(mode<=3) {md->fail_write_at=at; md->failed_write_bytes=(mode-1)*256;}
            else if(mode==4) md->fail_flush_at=at;
            else fail_fat_allocation=at;
            int e=fault_operation(&f,op,&file);
            int equal=!memcmp(md->data,snapshot,md->bytes);
            if(e!=(mode==5?DE_NOMEM:DE_IO) || f.faulted || !equal)
                fprintf(stderr,"transaction op=%u mode=%u at=%u error=%d faulted=%d restored=%d\n",op,mode,at,e,f.faulted,equal);
            CHECK(e==(mode==5?DE_NOMEM:DE_IO)); CHECK(!f.faulted && equal);
            CHECK(!fat_pages && !f.tx_depth && !f.tx_first && !f.tx_last);
            if((op>=4 && op<=7) || op==14)
                CHECK(file.pos==original.pos && file.node.sector==original.node.sector &&
                      file.node.offset==original.node.offset && !memcmp(file.node.raw,original.node.raw,32));
            scenarios++;
        }
    }
    /* If the device stops accepting all writes midway through commit, a
     * rollback cannot be assumed. Subsequent reads/writes must fail closed. */
    reset_faults(md); memcpy(md->data,snapshot,md->bytes); CHECK(!fat_mount(&f,disk)); reset_faults(md);
    u16 link; CHECK(!fat_get(&f,341,&link));
    md->fail_write_at=2; md->fail_following_writes=1;
    CHECK(fat_set(&f,341,link^0x123)==DE_IO && f.faulted);
    unsigned writes=md->writes;
    CHECK(fat_create(&f,"\\TXNB\\DENIED",0,&node)==DE_IO && md->writes==writes);
    CHECK(fat_lookup(&f,"\\TXNA\\DATA.BIN",&node)==DE_IO);
    CHECK(fat_get(&f,2,&link)==DE_IO);
    reset_faults(md); CHECK(fat_mount(&f,disk)==DE_IO && f.faulted); CHECK(!fat_pages);
    /* A persistent flush failure also leaves durability unknown. */
    reset_faults(md); memcpy(md->data,snapshot,md->bytes); CHECK(!fat_mount(&f,disk)); reset_faults(md);
    md->fail_flush=1; CHECK(fat_create(&f,"\\TXNB\\FLUSH",0,&node)==DE_IO && f.faulted);
    CHECK(!memcmp(md->data,snapshot,md->bytes) && !fat_pages);
    reset_faults(md); memcpy(md->data,snapshot,md->bytes); CHECK(!fat_mount(&f,disk));
    /* Cycles and premature free links cannot turn a failed delete into an
     * unlink or free unrelated directory entries. */
    u16 first=fat_cluster(&source),next; CHECK(!fat_get(&f,first,&next));
    CHECK(!fat_set(&f,first,first));
    u8 *damaged=malloc(md->bytes); CHECK(damaged!=NULL); memcpy(damaged,md->data,md->bytes);
    CHECK(fat_remove(&f,"\\TXNA\\DATA.BIN",0)==DE_IO && !memcmp(damaged,md->data,md->bytes));
    CHECK(!fat_set(&f,first,0)); memcpy(damaged,md->data,md->bytes);
    CHECK(fat_remove(&f,"\\TXNA\\DATA.BIN",0)==DE_IO && !memcmp(damaged,md->data,md->bytes));
    CHECK(!fat_set(&f,first,next)); free(damaged); free(snapshot);
    CHECK(!fat_remove(&f,"\\TXNA\\DATA.BIN",0)); CHECK(!fat_remove(&f,"\\TXNA\\EMPTY",1));
    CHECK(!fat_remove(&f,"\\TXNA",1)); CHECK(!fat_remove(&f,"\\TXNB",1));
    for(unsigned i=0;i<f.spc*16U-2;i++) {
        char name[40]; snprintf(name,sizeof(name),"\\TXFULL\\F%03u.DAT",i); CHECK(!fat_remove(&f,name,0));
    }
    CHECK(!fat_remove(&f,"\\TXFULL",1)); u32 after_free;
    CHECK(!fat_free_space(&f,&after_free) && after_free==before_free);
    printf("PASS FAT%u transaction faults: %u injected read/write/torn-write/flush/allocation failures\n",f.bits,scenarios);
}
static void test_moves(Disk *disk) {
    void *memory=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(memory!=NULL); CHECK(!dos_init(disk,memory));
    CHECK(!dos_mkdir("MOVEA")); CHECK(!dos_mkdir("MOVEB"));
    CHECK(!dos_mkdir("MOVEA\\SUB")); CHECK(!dos_mkdir("MOVEA\\SUB\\CHILD"));
    unsigned h; u32 n; char text[4];
    CHECK(!dos_open("MOVEA\\SUB\\DATA.TXT",2,2,&h)); CHECK(!dos_write(h,"move",4,&n)); CHECK(!dos_close(h));
    CHECK(dos_rename("MOVEA","MOVEA\\SUB\\BAD")==DE_ACCESS);
    CHECK(dos_rename("MOVEA\\SUB","MOVEA\\SUB\\CHILD\\BAD")==DE_ACCESS);
    CHECK(!dos_chdir("MOVEA\\SUB")); CHECK(dos_rename("C:\\MOVEA\\SUB","C:\\MOVEB\\RENAMED")==DE_CURRENT);
    CHECK(!dos_chdir("\\")); CHECK(!dos_rename("MOVEA\\SUB","MOVEB\\RENAMED"));
    CHECK(dos_open("MOVEA\\SUB\\DATA.TXT",0,0,&h)==DE_PATH);
    CHECK(!dos_open("MOVEB\\RENAMED\\DATA.TXT",0,0,&h));
    CHECK(!dos_read(h,text,4,&n) && n==4 && !memcmp(text,"move",4)); CHECK(!dos_close(h));
    Node dir,node; CHECK(!fat_lookup(&dos_volume,"\\MOVEB",&dir));
    CHECK(!fat_lookup(&dos_volume,"\\MOVEB\\RENAMED",&node));
    u32 index=1; CHECK(!fat_next(&dos_volume,fat_cluster(&node),&index,&node));
    CHECK(!memcmp(node.raw,"..         ",11) && fat_cluster(&node)==fat_cluster(&dir));
    CHECK(!dos_rename("MOVEB\\RENAMED\\DATA.TXT","MOVEA\\FINAL.TXT"));
    CHECK(!dos_rename("MOVEB\\RENAMED","ROOTMOVE"));
    CHECK(!fat_lookup(&dos_volume,"\\ROOTMOVE",&node));
    index=1; CHECK(!fat_next(&dos_volume,fat_cluster(&node),&index,&node)); CHECK(!fat_cluster(&node));
    /* Exhausting the fixed root directory after staging the source unlink
     * must leave the source name, contents and allocation intact. */
    unsigned created=0; int e;
    for(;;) {
        char name[32]; snprintf(name,sizeof(name),"\\R%03u.TMP",created);
        e=fat_create(&dos_volume,name,0,&node); if(e) break;
        CHECK(++created<=dos_volume.root_entries);
    }
    CHECK(e==DE_FULL && created>0);
    CHECK(dos_rename("MOVEA\\FINAL.TXT","ROOTF.TXT")==DE_FULL);
    CHECK(!dos_open("MOVEA\\FINAL.TXT",0,0,&h));
    CHECK(!dos_read(h,text,4,&n) && n==4 && !memcmp(text,"move",4)); CHECK(!dos_close(h));
    for(unsigned i=0;i<created;i++) {
        char name[32]; snprintf(name,sizeof(name),"\\R%03u.TMP",i); CHECK(!dos_remove(name,0));
    }
    CHECK(!dos_remove("MOVEA\\FINAL.TXT",0)); CHECK(!dos_remove("ROOTMOVE\\CHILD",1));
    CHECK(!dos_remove("ROOTMOVE",1)); CHECK(!dos_remove("MOVEA",1)); CHECK(!dos_remove("MOVEB",1));
    free(memory);
}
/* Unlike MemoryDisk, these descriptors bind access to a media snapshot. A
 * replacement during a write must also reject FAT's recovery writes. */
typedef struct {
    MemoryDisk *disk,*replacement;
    u32 generation,flags;
    int readonly,replace_on_write;
} TestUnit;
static TestUnit test_units[4];
static int unit_check(void *ctx,TestUnit **out) {
    u64 token=(uintptr_t)ctx; unsigned unit=token&255;
    if(!unit || unit>ARRAY_SIZE(test_units)) return DE_DRIVE;
    TestUnit *d=&test_units[unit-1];
    if(token>>8!=d->generation) return DE_CHANGED;
    if(!(d->flags&IO_DISK_PRESENT)) return DE_NOTREADY;
    *out=d; return 0;
}
static int unit_read(void *ctx,u32 lba,void *buf) {
    TestUnit *d; int e=unit_check(ctx,&d); return e?e:read_block(d->disk,lba,buf);
}
static int unit_write(void *ctx,u32 lba,const void *buf) {
    TestUnit *d; int e=unit_check(ctx,&d); if(e) return e;
    if(d->readonly) return DE_READONLY;
    if(d->replace_on_write && !--d->replace_on_write) {
        /* The old medium receives a torn write before it is replaced. */
        CHECK((u64)lba*512+512<=d->disk->bytes);
        memcpy(d->disk->data+(u64)lba*512,buf,256);
        d->disk=d->replacement; d->generation++; return DE_CHANGED;
    }
    return write_block(d->disk,lba,buf);
}
static int unit_flush(void *ctx) {
    TestUnit *d; int e=unit_check(ctx,&d); return e?e:flush_block(d->disk);
}
static u32 unit_count(void *ctx) {(void)ctx; return ARRAY_SIZE(test_units);}
static int unit_info(void *ctx,u32 n,IoDiskInfo *info) {
    (void)ctx; if(n>=ARRAY_SIZE(test_units)) return DE_DRIVE;
    TestUnit *d=&test_units[n];
    *info=(IoDiskInfo){.generation=d->generation,.flags=d->flags,
        .disk={(void *)(uintptr_t)(((u64)d->generation<<8)|(n+1)),unit_read,unit_write,unit_flush,d->disk->bytes/512,d->readonly}};
    return 0;
}
static void test_drives(Disk *boot,MemoryDisk *source) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    MemoryDisk copies[4]; memset(copies,0,sizeof(copies));
    for(unsigned i=0;i<ARRAY_SIZE(copies);i++) {
        copies[i].bytes=source->bytes; copies[i].data=malloc(source->bytes); CHECK(copies[i].data!=NULL);
        memcpy(copies[i].data,source->data,source->bytes);
    }
    test_units[0]=(TestUnit){.disk=source,.generation=1,.flags=IO_DISK_BOOT|IO_DISK_PRESENT};
    test_units[1]=(TestUnit){.disk=&copies[0],.generation=1,.flags=IO_DISK_PRESENT};
    test_units[2]=(TestUnit){.disk=&copies[1],.generation=1,.flags=IO_DISK_REMOVABLE};
    test_units[3]=(TestUnit){.disk=&copies[2],.generation=1,.flags=IO_DISK_PRESENT,.readonly=1};
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.capabilities=IO_CAP_DISKS,.disk_count=unit_count,.disk_info=unit_info};
    CHECK(!dos_init(boot,arena)); CHECK(!dos_attach_disks(&io));
    DosDriveInfo info; char path[DOS_PATH_MAX],data[16]; unsigned h,c,d; u32 n,pos,pid,root=dos_pid();
    CHECK(!dos_drive_info(0,&info) && info.flags==DOS_DRIVE_REMOVABLE);
    CHECK(dos_drive_info(1,&info)==DE_DRIVE); CHECK(dos_drive_info(26,&info)==DE_DRIVE);
    CHECK(!dos_drive_info(4,&info) && (info.flags&DOS_DRIVE_READONLY));
    CHECK(dos_open("E:\\NEW.TXT",2,1,&h)==DE_READONLY);
    CHECK(!dos_select_drive(0)); CHECK(dos_open("README.TXT",0,0,&h)==DE_NOTREADY);
    CHECK(!dos_canonical("file.txt",path) && !strcmp(path,"A:\\FILE.TXT"));
    CHECK(!dos_select_drive(2)); CHECK(dos_select_drive(1)==DE_DRIVE && dos_current_drive()==2);
    CHECK(!dos_mkdir("C:\\MULTI")); CHECK(!dos_mkdir("D:\\MULTI"));
    CHECK(!dos_chdir("D:\\MULTI") && dos_current_drive()==2);
    CHECK(!dos_canonical("D:hello.txt",path) && !strcmp(path,"D:\\MULTI\\HELLO.TXT"));
    CHECK(!dos_canonical("C:hello.txt",path) && !strcmp(path,"C:\\HELLO.TXT"));
    CHECK(!dos_chdir("C:\\MULTI"));
    CHECK(!dos_open("C:SAME.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&c));
    CHECK(!dos_open("D:SAME.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&d));
    CHECK(!dos_write(c,"C-file",6,&n) && n==6); CHECK(!dos_write(d,"D-file",6,&n) && n==6);
    CHECK(!dos_lock(c,0,0,6)); CHECK(!dos_lock(d,0,0,6));
    CHECK(!dos_seek(d,0,0,&pos) && !dos_read(d,data,6,&n) && n==6 && !memcmp(data,"D-file",6));
    CHECK(!dos_seek(c,0,0,&pos) && !dos_read(c,data,6,&n) && n==6 && !memcmp(data,"C-file",6));
    CHECK(dos_rename("C:SAME.TXT","D:OTHER.TXT")==DE_NOTSAME);
    CHECK(!dos_close(d)); CHECK(!dos_remove("D:SAME.TXT",0)); CHECK(!dos_close(c));
    CHECK(!dos_open("D:ONE.TXT",2,2,&d)); CHECK(!dos_close(d));
    CHECK(!dos_open("D:TWO.TXT",2,2,&d)); CHECK(!dos_close(d));
    struct {u64 before; DosFind find; u64 after;} dta={.before=0x12345678,.after=0x87654321};
    CHECK(!dos_find_first("D:*.TXT",0,&dta.find) && !strcmp(dta.find.name,"ONE.TXT"));
    DosFind copied=dta.find;
    CHECK(!dos_select_drive(3)); CHECK(!dos_select_drive(2));
    CHECK(!dos_find_next(&copied) && !strcmp(copied.name,"TWO.TXT"));
    CHECK(dos_find_next(&copied)==DE_NOMORE && dta.before==0x12345678 && dta.after==0x87654321);
    CHECK(!dos_task_create(&pid)); CHECK(!dos_task_select(pid));
    CHECK(!dos_select_drive(3)); CHECK(!dos_chdir("C:\\"));
    CHECK(!dos_task_select(root) && dos_current_drive()==2);
    CHECK(!dos_drive_cwd(2,path) && !strcmp(path,"\\MULTI"));
    CHECK(!dos_chdir("D:\\")); CHECK(dos_remove("D:\\MULTI",1)==DE_CURRENT);
    CHECK(!dos_open("D:\\MULTI\\ONE.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&d));
    CHECK(!dos_lock(d,0,0,8));
    CHECK(!dos_drive_info(3,&info)); u32 generation=info.generation;
    test_units[1].disk=&copies[3]; test_units[1].generation++;
    CHECK(dos_read(d,data,1,&n)==DE_CHANGED && !n);
    CHECK(dos_write(d,"bad",3,&n)==DE_CHANGED && !n);
    CHECK(dos_seek(d,0,0,&pos)==DE_CHANGED); CHECK(dos_lock(d,1,0,8)==DE_CHANGED);
    u16 date=0,time=0; CHECK(dos_file_time(d,0,&date,&time)==DE_CHANGED);
    DosRegs r={.ax=0x6800,.bx=d}; CHECK(call(&r)==DE_CHANGED);
    CHECK(dos_find_next(&dta.find)==DE_CHANGED);
    CHECK(dos_close(d)==DE_CHANGED && dos_close(d)==DE_HANDLE);
    CHECK(!copies[3].writes && !copies[3].flushes);
    CHECK(!dos_drive_info(3,&info) && info.generation!=generation);
    CHECK(!dos_task_select(pid) && dos_current_drive()==3);
    CHECK(!dos_drive_cwd(3,path) && !strcmp(path,"\\"));
    CHECK(!dos_task_select(root)); CHECK(!dos_task_destroy(pid));
    CHECK(!dos_open("D:\\AFTER.TXT",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,2,&h));
    CHECK(!dos_lock(h,0,0,8)); CHECK(!dos_write(h,"new",3,&n) && n==3); CHECK(!dos_close(h));
    test_units[2].flags|=IO_DISK_PRESENT; test_units[2].generation++;
    CHECK(!dos_open("A:\\README.TXT",0,0,&h));
    CHECK(!dos_mkdir("A:\\MEDIA")); CHECK(!dos_chdir("A:\\MEDIA"));
    test_units[2].flags&=~IO_DISK_PRESENT; test_units[2].generation++;
    CHECK(dos_read(h,data,1,&n)==DE_CHANGED); CHECK(dos_close(h)==DE_CHANGED);
    CHECK(!dos_drive_cwd(0,path) && !strcmp(path,"\\"));
    CHECK(!dos_flush());
    test_units[2].flags|=IO_DISK_PRESENT; test_units[2].generation++;
    CHECK(!dos_open("A:\\README.TXT",0,0,&h));
    CHECK(!dos_drive_info(0,&info)); generation=info.generation;
    test_units[2].readonly=1; /* Even broken providers that keep the ID are detected. */
    CHECK(dos_read(h,data,1,&n)==DE_CHANGED); CHECK(dos_close(h)==DE_CHANGED);
    CHECK(!dos_drive_info(0,&info) && info.generation!=generation && (info.flags&DOS_DRIVE_READONLY));
    CHECK(dos_open("A:\\BLOCK.TXT",2,1,&h)==DE_READONLY);
    test_units[2].readonly=0;
    CHECK(!dos_open("D:\\AFTER.TXT",0,0,&h));
    unsigned boot_flushes=source->flushes,other_flushes=copies[2].flushes,drive_flushes=copies[3].flushes;
    r=(DosRegs){.ax=0x6800,.bx=h}; CHECK(!call(&r));
    CHECK(source->flushes==boot_flushes && copies[2].flushes==other_flushes && copies[3].flushes==drive_flushes+1);
    CHECK(!dos_close(h)); CHECK(!dos_flush()); CHECK(source->flushes==boot_flushes+1 && copies[2].flushes==other_flushes+1);
    /* Replace A: during commit; rollback must not touch the replacement. */
    CHECK(!dos_open("A:\\README.TXT",0,0,&h)); CHECK(!dos_close(h));
    test_units[2].replacement=&copies[3]; test_units[2].replace_on_write=1;
    u8 *before=malloc(copies[3].bytes); CHECK(before!=NULL); memcpy(before,copies[3].data,copies[3].bytes);
    int e=dos_mkdir("A:\\TORN"); CHECK(e==DE_CHANGED || e==DE_IO);
    CHECK(!memcmp(before,copies[3].data,copies[3].bytes)); free(before);
    CHECK(!dos_open("A:\\AFTER.TXT",0,0,&h));
    CHECK(!dos_read(h,data,3,&n) && n==3 && !memcmp(data,"new",3)); CHECK(!dos_close(h));
    CHECK(!dos_remove("C:\\MULTI\\SAME.TXT",0)); CHECK(!dos_chdir("C:\\")); CHECK(!dos_remove("C:\\MULTI",1));
    CHECK(!fat_pages);
    /* Reset all DOS references before releasing providers and snapshots. */
    CHECK(!dos_init(boot,arena));
    for(unsigned i=0;i<ARRAY_SIZE(copies);i++) free(copies[i].data);
    free(arena); puts("PASS drives: per-drive paths/tasks/searches, media snapshots and replacement during commit");
}
static unsigned breaks_seen;
static int break_action;
static int test_break(void *context) {
    CHECK(context==&breaks_seen); breaks_seen++;
    DosRegs r={.ax=0x3000}; CHECK(call(&r)==DE_BUSY);
    r=(DosRegs){.ax=0x2800,.cx=10}; CHECK(call(&r)==DE_BUSY && !r.cx);
    DosExitInfo status; CHECK(dos_last_exit(&status)==DE_BUSY);
    CHECK(dos_break_handler(NULL,NULL)==DE_BUSY);
    u32 pid; CHECK(dos_task_create(&pid)==DE_BUSY);
    return break_action;
}
static int console_call(unsigned fn) {DosRegs r={.ax=fn<<8}; int e=call(&r); return e?-e:(int)r.ax;}
static int buffered_input(u8 *buffer) {DosRegs r={.ax=0x0a00,.dx=(uintptr_t)buffer}; return call(&r);}
static void test_console(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    capture_console=1; console_size=0; key_string("A");
    CHECK(console_call(0x0b)==255 && console_call(0x0b)==255 && !key_read);
    CHECK(console_call(1)=='A' && console_size==1 && console_output[0]=='A');
    CHECK(console_call(0x0b)==0);
    DosRegs r={.ax=0x0600,.dx=255}; CHECK(!call(&r) && (r.flags&0x40) && !r.ax);
    key_string("B"); r=(DosRegs){.ax=0x0600,.dx=255,.flags=0x40}; CHECK(!call(&r) && !(r.flags&0x40) && r.ax=='B');
    key_string("\3"); r=(DosRegs){.ax=0x0600,.dx='X'}; CHECK(!call(&r) && !key_read && console_output[console_size-1]=='X');
    CHECK(console_call(7)==3); key_string("Y"); unsigned echoed=console_size;
    CHECK(console_call(8)=='Y' && console_size==echoed);
    const char embedded[]={0,'Z','$'};
    r=(DosRegs){.ax=0x0900,.dx=(uintptr_t)embedded}; CHECK(!call(&r) && r.ax==0x24 && console_size==echoed+2);
    key_string(""); key_scan(IO_SCAN_F1); CHECK(console_call(7)==0); CHECK(console_call(0x0b)==255);
    CHECK(console_call(7)==59); CHECK(console_call(0x0b)==0);
    u8 guarded[264]; memset(guarded,0xa5,sizeof(guarded)); u8 *buffer=guarded+4;
    buffer[0]=8; buffer[1]=0; key_string("ab\bC\r"); CHECK(!buffered_input(buffer));
    CHECK(buffer[1]==2 && !memcmp(buffer+2,"aC\r",3) && guarded[3]==0xa5 && guarded[14]==0xa5);
    buffer[0]=1; buffer[1]=0; key_string("abcdef\r"); CHECK(!buffered_input(buffer) && !buffer[1] && buffer[2]=='\r');
    buffer[0]=0; buffer[1]=99; key_string("Q"); CHECK(!buffered_input(buffer) && buffer[1]==99 && !key_read);
    buffer[0]=16; buffer[1]=5; memcpy(buffer+2,"HELLO\r",6);
    key_string(""); key_scan(IO_SCAN_F1); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==5 && !memcmp(buffer+2,"HELLO\r",6));
    buffer[1]=5; memcpy(buffer+2,"ABCDE\r",6);
    key_string(""); key_scan(IO_SCAN_F2); key_append("D"); key_scan(IO_SCAN_DELETE); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==4 && !memcmp(buffer+2,"ABCE\r",5));
    buffer[1]=5; memcpy(buffer+2,"ABCDE\r",6);
    key_string(""); key_scan(IO_SCAN_F4); key_append("D"); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==2 && !memcmp(buffer+2,"DE\r",3));
    buffer[1]=2; memcpy(buffer+2,"AB\r",3);
    key_string(""); key_scan(IO_SCAN_F1); key_scan(IO_SCAN_INSERT); key_append("X"); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==3 && !memcmp(buffer+2,"AXB\r",4));
    key_string("XY"); key_scan(IO_SCAN_F5); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==2 && !memcmp(buffer+2,"XY\r",3));
    key_string("junk\x1b"); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==2 && !memcmp(buffer+2,"XY\r",3));
    key_string("z"); key_scan(IO_SCAN_F6); key_append("\r");
    CHECK(!buffered_input(buffer) && buffer[1]==2 && buffer[2]=='z' && buffer[3]==26);
    buffer[0]=16; buffer[1]=15; /* Invalid template: must never read beyond maximum. */
    memset(buffer+2,'x',16); key_string(""); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(buffer) && !buffer[1]);
    buffer[1]=0; key_string("A\n\b\x1b\r");
    CHECK(!buffered_input(buffer) && !buffer[1]);
    key_string("\x13q"); r=(DosRegs){.ax=0x0200,.dx='Z'}; CHECK(!call(&r) && key_read==key_count);
    key_string("stale"); delayed_keys="R"; r=(DosRegs){.ax=0x0c08}; CHECK(!call(&r) && r.ax=='R' && !delayed_keys);
    key_string(""); key_scan(IO_SCAN_F2); CHECK(console_call(7)==0);
    r=(DosRegs){.ax=0x0c00}; CHECK(!call(&r)); CHECK(console_call(0x0b)==0);
    /* Cooked handle reads keep the rest of a line across short reads and DUP. */
    char data[32]; u32 n; unsigned duplicate;
    key_string("abcd\r"); r=(DosRegs){.ax=0x3f00,.bx=0,.cx=2,.dx=(uintptr_t)data};
    CHECK(!call(&r) && r.ax==2 && !memcmp(data,"ab",2)); CHECK(!dos_dup(0,&duplicate));
    r=(DosRegs){.ax=0x4406,.bx=duplicate}; CHECK(!call(&r) && r.ax==255);
    CHECK(!dos_read(duplicate,data,4,&n) && n==4 && !memcmp(data,"cd\r\n",4)); CHECK(!dos_close(duplicate));
    key_string("z\x1a\r"); CHECK(!dos_read(0,data,sizeof(data),&n) && n==1 && data[0]=='z');
    CHECK(!dos_read(0,data,sizeof(data),&n) && !n);
    r=(DosRegs){.ax=0x4400,.bx=0}; CHECK(!call(&r) && !(r.dx&64));
    r=(DosRegs){.ax=0x4406,.bx=0}; CHECK(!call(&r) && !r.ax);
    r=(DosRegs){.ax=0x4401,.bx=0,.dx=0xe3}; CHECK(!call(&r)); echoed=console_size;
    key_string("\3\x1a\r\n"); CHECK(!dos_read(0,data,4,&n) && n==4 && !memcmp(data,"\3\x1a\r\n",4) && console_size==echoed);
    r=(DosRegs){.ax=0x4401,.bx=0,.dx=0xc3}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x4401,.bx=0,.dx=256}; CHECK(call(&r)==DE_FUNCTION);
    r=(DosRegs){.ax=0x4406,.bx=1ULL<<32}; CHECK(call(&r)==DE_HANDLE);
    /* Redirected legacy character APIs obey handle 0, including EOF and peek. */
    unsigned file,saved; u32 pos;
    CHECK(!dos_open("CONIN.TMP",2,2,&file)); CHECK(!dos_write(file,"JK\3Oline\r",9,&n));
    CHECK(!dos_seek(file,0,0,&pos)); CHECK(!dos_dup(0,&saved)); CHECK(!dos_dup2(file,0));
    key_string("physical"); echoed=console_size;
    CHECK(console_call(1)=='J' && console_size==echoed+1);
    CHECK(console_call(0x0b)==255 && !dos_seek(file,0,1,&pos) && pos==1);
    CHECK(console_call(7)=='K'); CHECK(console_call(7)==3); CHECK(console_call(8)=='O');
    buffer[0]=16; buffer[1]=0; CHECK(!buffered_input(buffer) && buffer[1]==4 && !memcmp(buffer+2,"line\r",5));
    CHECK(console_call(7)==-DE_EOF && console_call(0x0b)==0);
    r=(DosRegs){.ax=0x0c00}; CHECK(!call(&r) && !key_read);
    CHECK(!dos_seek(file,0,0,&pos)); r=(DosRegs){.ax=0x0c07}; CHECK(!call(&r) && r.ax=='J' && !key_read);
    r=(DosRegs){.ax=0x4401,.bx=file,.dx=32}; CHECK(call(&r)==DE_FUNCTION);
    CHECK(!dos_dup2(saved,0)); CHECK(!dos_close(saved)); CHECK(!dos_close(file)); CHECK(!dos_remove("CONIN.TMP",0));
    /* Break handlers are task-local and callbacks cannot reenter DOS. */
    key_string(""); breaks_seen=0; break_action=DOS_BREAK_CONTINUE;
    DosBreakHandler handler={test_break,&breaks_seen},old;
    CHECK(!dos_break_handler(&handler,&old) && !old.handler);
    key_string("\3X"); CHECK(console_call(8)=='X' && breaks_seen==1);
    break_action=DOS_BREAK_CANCEL;
    key_string("\3discard"); CHECK(console_call(8)==-DE_BREAK && breaks_seen==2 && key_read==key_count);
    key_string("c"); keys[0].modifiers=IO_MOD_CONTROL; keys[0].flags=IO_KEY_MODIFIERS_VALID;
    CHECK(console_call(8)==-DE_BREAK && breaks_seen==3);
    key_string(""); key_scan(IO_SCAN_PAUSE); keys[0].modifiers=IO_MOD_CONTROL; keys[0].flags=IO_KEY_MODIFIERS_VALID;
    CHECK(console_call(8)==-DE_BREAK && breaks_seen==4);
    r=(DosRegs){.ax=0x3300}; CHECK(!call(&r) && !r.dx);
    r=(DosRegs){.ax=0x3302,.dx=1}; CHECK(!call(&r) && !r.dx);
    key_string("\3"); CHECK(console_call(7)==3 && breaks_seen==4);
    key_string("\3"); r=(DosRegs){.ax=0x4800,.bx=17}; CHECK(call(&r)==DE_BREAK && breaks_seen==5);
    u32 child,parent=dos_pid(); CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    CHECK(!dos_break_handler(NULL,&old) && !old.handler);
    r=(DosRegs){.ax=0x3300}; CHECK(!call(&r) && r.dx==1);
    r=(DosRegs){.ax=0x3301,.dx=0}; CHECK(!call(&r));
    CHECK(!dos_task_select(parent)); CHECK(!dos_break_handler(NULL,&old) && old.handler==test_break);
    r=(DosRegs){.ax=0x3300}; CHECK(!call(&r) && r.dx==1); CHECK(!dos_task_destroy(child));
    dos_set_errorlevel(37); DosExitInfo status; CHECK(!dos_last_exit(&status) && status.code==37 && status.kind==DOS_EXIT_NORMAL);
    r=(DosRegs){.ax=0x4d00}; CHECK(!call(&r) && r.ax==37); CHECK(!dos_last_exit(&status) && !status.code && !status.kind);
    key_string(""); capture_console=0; free(arena);
    puts("PASS console: character/line input, editing, redirection, raw mode, EOF, status and task-local break callbacks");
}
static void key_unicode(u32 c) {CHECK(key_count<ARRAY_SIZE(keys)); keys[key_count++]=(IoEvent){.type=IO_EVENT_KEY,.unicode=c};}
static int text_was(const u16 *want,unsigned n) {
    int same=console_text_size==n && !memcmp(console_text,want,n*sizeof(*want));
    console_text_size=0; return same;
}
static int cp_ioctl(unsigned h,unsigned function,void *packet,u32 size,u64 *stored) {
    DosRegs r={.ax=0x440c,.bx=h,.cx=(DOS_CP_CATEGORY<<8)|function,.dx=(uintptr_t)packet,.si=size};
    int e=call(&r); if(stored) *stored=r.ax; return e;
}
static int cp_page(unsigned h,unsigned function,u16 page) {u8 packet[4]; wr16(packet,2); wr16(packet+2,page); return cp_ioctl(h,function,packet,4,NULL);}
static u16 cp_selected(unsigned h) {u8 q[4]; u64 n; CHECK(!cp_ioctl(h,DOS_CP_QUERY,q,4,&n) && n==4 && rd16(q)==2); return rd16(q+2);}
/* Prepare start, stream table data in short chunks, then prepare end. */
static int cp_prepare(unsigned h,const u16 *pages,unsigned count,const u8 *data,u32 size) {
    u8 packet[64]; wr16(packet,0); wr16(packet+2,2+2*count); wr16(packet+4,count);
    for(unsigned i=0;i<count;i++) wr16(packet+6+2*i,pages[i]);
    int e=cp_ioctl(h,DOS_CP_PREPARE_START,packet,6+2*count,NULL); if(e) return e;
    for(u32 at=0;at<size;) {
        u32 take=MIN(size-at,1000); DosRegs r={.ax=0x4403,.bx=h,.cx=take,.dx=(uintptr_t)(data+at)};
        e=call(&r); if(e) {cp_ioctl(h,DOS_CP_PREPARE_END,NULL,0,NULL); return e;}
        CHECK(r.ax==take); at+=take;
    }
    return cp_ioctl(h,DOS_CP_PREPARE_END,NULL,0,NULL);
}
static void cp_list(unsigned h,const u16 *slots) {
    u8 q[64]; u64 n; CHECK(!cp_ioctl(h,DOS_CP_QUERY_LIST,q,sizeof(q),&n) && n==24 && rd16(q)==22);
    CHECK(rd16(q+2)==1 && rd16(q+4)==437 && rd16(q+6)==DOS_CP_PREPARED_MAX);
    for(unsigned i=0;i<DOS_CP_PREPARED_MAX;i++) CHECK(rd16(q+8+2*i)==slots[i]);
}
static int global_page(void) {DosRegs r={.ax=0x6601}; CHECK(!call(&r)); return (int)r.bx;}
static int set_page(u16 page) {DosRegs r={.ax=0x6602,.bx=page}; return call(&r);}
static int set_country(u16 country) {DosRegs r={.ax=0x38ff,.bx=country,.dx=65535}; return call(&r);}
static void test_codepage(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    unsigned file,con,reader; u32 size,n; u8 *cpi=malloc(65536); CHECK(cpi!=NULL);
    CHECK(!dos_open("EFI.CPI",0,0,&file) && !dos_read(file,cpi,65536,&size) && size>1000 && size<65536 && !dos_close(file));
    capture_console=1; console_text_size=0;
    /* Hardware page 437 is active at boot. */
    CHECK(!dos_write(1,"A\x82\xe1\xd5",4,&n) && n==4);
    {const u16 want[]={'A',0xe9,0xdf,0x2552}; CHECK(text_was(want,4));}
    DosRegs r={.ax=0x4400,.bx=1}; CHECK(!call(&r) && (r.dx&0xc080)==0xc080);
    CHECK(!dos_open("CON",2,0,&con) && !dos_open("CON",0,0,&reader));
    CHECK(cp_selected(con)==437);
    const u16 empty[DOS_CP_PREPARED_MAX]={0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff};
    cp_list(con,empty);
    u8 small[2]; CHECK(cp_ioctl(con,DOS_CP_QUERY,small,2,NULL)==DE_FUNCTION);
    CHECK(cp_ioctl(con,0x99,small,2,NULL)==DE_FUNCTION);
    r=(DosRegs){.ax=0x440c,.bx=con,.cx=0x056a,.dx=(uintptr_t)small,.si=2}; CHECK(call(&r)==DE_FUNCTION);
    /* Unprepared pages: device select and AH=6602h fail without side effects. */
    CHECK(cp_page(con,DOS_CP_SELECT,850)==DOS_CP_NOT_PREPARED && cp_selected(con)==437);
    CHECK(set_page(850)==DOS_CP_SYSTEM_NOT_PREPARED && global_page()==437 && cp_selected(con)==437);
    DosExtendedError error; CHECK(!dos_extended_error(&error) && error.error==DOS_CP_SYSTEM_NOT_PREPARED);
    CHECK(set_page(999)==DE_NOFILE && cp_selected(con)==437);
    /* Prepare state machine, packet validation and access control. */
    CHECK(cp_ioctl(con,DOS_CP_PREPARE_END,NULL,0,NULL)==DOS_CP_BAD_FILE);
    r=(DosRegs){.ax=0x4403,.bx=con,.cx=4,.dx=(uintptr_t)cpi}; CHECK(call(&r)==DOS_CP_BAD_FILE);
    CHECK(cp_page(reader,DOS_CP_SELECT,437)==DE_ACCESS);
    r=(DosRegs){.ax=0x4403,.bx=reader,.cx=4,.dx=(uintptr_t)cpi}; CHECK(call(&r)==DE_ACCESS);
    u8 packet[32]={1,0,4,0,1,0,0x52,3}; CHECK(cp_ioctl(con,DOS_CP_PREPARE_START,packet,8,NULL)==DE_FUNCTION);
    packet[0]=0; CHECK(cp_ioctl(con,DOS_CP_PREPARE_START,packet,7,NULL)==DE_FUNCTION);
    {const u16 pages[]={0xffff,0xffff}; CHECK(cp_prepare(con,pages,2,cpi,size)==DOS_CP_DEVICE_ERROR);}
    {const u16 pages[]={850,850}; CHECK(cp_prepare(con,pages,2,cpi,size)==DOS_CP_DEVICE_ERROR);}
    {const u16 pages[]={0}; CHECK(cp_prepare(con,pages,1,cpi,size)==DOS_CP_DEVICE_ERROR);}
    {const u16 pages[9]={850}; CHECK(cp_prepare(con,pages,9,cpi,size)==DOS_CP_DEVICE_ERROR);}
    {const u16 pages[]={850,936}; CHECK(cp_prepare(con,pages,2,cpi,size)==DOS_CP_NOT_IN_FILE);}
    cp_list(con,empty);
    u8 *bad=malloc(size); CHECK(bad!=NULL); memcpy(bad,cpi,size); bad[1]='X';
    {const u16 pages[]={850}; CHECK(cp_prepare(con,pages,1,bad,size)==DOS_CP_BAD_FILE);}
    {const u16 pages[]={850}; CHECK(cp_prepare(con,pages,1,cpi,size-1)==DOS_CP_BAD_FILE);}
    /* A control byte that is not identity mapped would corrupt CR/LF. */
    u32 at,length; unsigned kind; CHECK(!codepage_find(cpi,size,850,&at,&length,&kind) && kind==1 && length==512);
    memcpy(bad,cpi,size); wr16(bad+at+2*10,'X');
    {const u16 pages[]={850}; CHECK(cp_prepare(con,pages,1,bad,size)==DOS_CP_BAD_FILE);}
    CHECK(!codepage_find(cpi,size,932,&at,&length,&kind) && kind==2 && length==16+512+60*512);
    memcpy(bad,cpi,size); bad[at+2]=0x70; /* Lead ranges must ascend. */
    {const u16 pages[]={932}; CHECK(cp_prepare(con,pages,1,bad,size)==DOS_CP_BAD_FILE);}
    memcpy(bad,cpi,size); wr32(bad+12+8,510); /* SBCS tables are exactly 256 units. */
    CHECK(codepage_find(bad,size,437,&at,&length,&kind)==DOS_CP_BAD_FILE);
    memcpy(bad,cpi,size); wr16(bad+12+12,437); /* Duplicate page numbers. */
    CHECK(codepage_find(bad,size,850,&at,&length,&kind)==DOS_CP_BAD_FILE);
    CHECK(codepage_find(cpi,size,936,&at,&length,&kind)==DOS_CP_NOT_IN_FILE);
    memcpy(packet,"\0\0\4\0\1\0\x52\x03",8); CHECK(!cp_ioctl(con,DOS_CP_PREPARE_START,packet,8,NULL));
    u8 *huge=calloc(129*1024,1); CHECK(huge!=NULL); r=(DosRegs){.ax=0x4403,.bx=con,.cx=129*1024,.dx=(uintptr_t)huge};
    CHECK(call(&r)==DOS_CP_BAD_FILE && !r.cx); free(huge);
    CHECK(cp_ioctl(con,DOS_CP_PREPARE_END,NULL,0,NULL)==DOS_CP_BAD_FILE);
    cp_list(con,empty); free(bad);
    u32 largest_before; CHECK(!arena_check(&dos_arena,&largest_before));
    {const u16 pages[]={850,932}; CHECK(!cp_prepare(con,pages,2,cpi,size));}
    {const u16 slots[DOS_CP_PREPARED_MAX]={850,932,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff}; cp_list(con,slots);}
    /* MODE CON CP REFRESH: zero pages, then prepare end. */
    memset(packet,0,8); wr16(packet+2,2); CHECK(!cp_ioctl(con,DOS_CP_PREPARE_START,packet,6,NULL) && !cp_ioctl(con,DOS_CP_PREPARE_END,NULL,0,NULL));
    CHECK(!cp_page(con,DOS_CP_SELECT,850) && cp_selected(con)==850 && global_page()==437);
    CHECK(!dos_write(1,"\x82\xd5",2,&n));
    {const u16 want[]={0xe9,0x131}; CHECK(text_was(want,2));}
    /* AH=6602h switches NLS and CON together. */
    CHECK(!set_page(437) && cp_selected(con)==437 && !set_page(850) && cp_selected(con)==850 && global_page()==850);
    CHECK(set_page(932)==DE_NOFILE && cp_selected(con)==850);
    CHECK(!set_page(437) && !set_country(81) && !set_page(932) && cp_selected(con)==932 && global_page()==932);
    r=(DosRegs){.ax=0x6300}; CHECK(!call(&r) && r.cx==6);
    /* DBCS output, including a pair split across AH=02h calls. */
    CHECK(!dos_write(1,"a\x82\xa0\xb1",4,&n));
    {const u16 want[]={'a',0x3042,0xff71}; CHECK(text_was(want,3));}
    r=(DosRegs){.ax=0x0200,.dx=0x88}; CHECK(!call(&r)); CHECK(!console_text_size);
    r=(DosRegs){.ax=0x0200,.dx=0x9f}; CHECK(!call(&r));
    {const u16 want[]={0x4e9c}; CHECK(text_was(want,1));}
    CHECK(!dos_write(1,"\x82\r\x85\x40\xa0",5,&n));
    {const u16 want[]={'?','\r','?','?'}; CHECK(text_was(want,4));}
    /* DBCS keys: the trail is queued; peeks never consume it. */
    key_string(""); key_unicode(0x3042); key_unicode(0xe9); key_append("z");
    CHECK(console_call(0x0b)==255 && console_call(8)==0x82 && console_call(0x0b)==255 && console_call(8)==0xa0);
    CHECK(console_call(8)=='z');
    key_string(""); key_unicode(0x4e9c); r=(DosRegs){.ax=0x0600,.dx=255}; CHECK(!call(&r) && r.ax==0x88);
    r=(DosRegs){.ax=0x0600,.dx=255}; CHECK(!call(&r) && r.ax==0x9f);
    /* Line editing deletes and copies whole characters. */
    u8 line[40]; line[0]=16; line[1]=0; console_text_size=0;
    key_string("a"); key_unicode(0x3042); key_append("\bb\r"); CHECK(!buffered_input(line) && line[1]==2 && !memcmp(line+2,"ab\r",3));
    {const u16 want[]={'a',0x3042,'\b',' ','\b','\b',' ','\b','b','\r'}; CHECK(text_was(want,10));}
    line[0]=3; line[1]=0; key_string("a"); key_unicode(0x3042); key_append("\r");
    CHECK(!buffered_input(line) && line[1]==1 && !memcmp(line+2,"a\r",2));
    line[0]=16; line[1]=4; memcpy(line+2,"\x82\xa0xy\r",5);
    key_string(""); key_scan(IO_SCAN_F1); key_append("\r");
    CHECK(!buffered_input(line) && line[1]==2 && !memcmp(line+2,"\x82\xa0\r",3));
    line[1]=5; memcpy(line+2,"a\x82\xa0" "bc\r",6);
    key_string(""); key_scan(IO_SCAN_F2); key_append("b\r");
    CHECK(!buffered_input(line) && line[1]==3 && !memcmp(line+2,"a\x82\xa0\r",4));
    line[1]=5; memcpy(line+2,"a\x82\xa0" "bc\r",6);
    key_string(""); key_scan(IO_SCAN_F2); key_unicode(0x3042); key_append("\r");
    CHECK(!buffered_input(line) && line[1]==1 && line[2]=='a');
    line[1]=5; memcpy(line+2,"a\x82\xa0" "bc\r",6);
    key_string(""); key_scan(IO_SCAN_F1); key_scan(IO_SCAN_DELETE); key_scan(IO_SCAN_F3); key_append("\r");
    CHECK(!buffered_input(line) && line[1]==3 && !memcmp(line+2,"abc\r",4));
    key_string(""); console_text_size=0;
    /* Replacing the selected page's slot falls back to the hardware page. */
    {const u16 pages[]={0xffff,860}; CHECK(!cp_prepare(con,pages,2,cpi,size));}
    {const u16 slots[DOS_CP_PREPARED_MAX]={850,860,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff}; cp_list(con,slots);}
    CHECK(cp_selected(con)==437 && global_page()==932);
    CHECK(set_page(932)==DOS_CP_SYSTEM_NOT_PREPARED && global_page()==932 && cp_selected(con)==437);
    r=(DosRegs){.ax=0x6300}; CHECK(!call(&r) && r.cx==6);
    CHECK(!set_page(437) && !set_country(1) && !set_page(850) && !set_country(351) && !set_page(860));
    CHECK(cp_selected(con)==860 && global_page()==860);
    CHECK(set_page(865)==DE_NOFILE && cp_selected(con)==860);
    {const u16 pages[]={865}; CHECK(!cp_prepare(con,pages,1,cpi,size));}
    CHECK(!cp_page(con,DOS_CP_SELECT,865) && !cp_page(con,DOS_CP_SELECT,437));
    CHECK(!dos_close(reader) && !dos_close(con));
    u32 largest_after; CHECK(!arena_check(&dos_arena,&largest_after) && largest_after<largest_before);
    capture_console=0; console_text_size=0; free(cpi); free(arena);
    puts("PASS code pages: CON transcoding, DBCS input/output/editing, prepare/select IOCTLs and atomic AH=6602h");
}
/* KEYB: a language's tables from KEYBOARD.SYS as KEYB.COM builds them,
 * with only the code pages listed. */
static u32 keyb_tables(const u8 *f,u32 size,const char *language,const u16 *pages,unsigned count,u8 *out) {
    u32 languages=rd16(f+26),entry=0;
    CHECK(size>28 && f[0]==0xff && !memcmp(f+1,"KEYB   ",7));
    for(u32 i=0;i<languages;i++) if(!memcmp(f+28+6*i,language,2)) entry=rd32(f+30+6*i);
    CHECK(entry && entry+10<size);
    u32 logic=rd32(f+entry+4),used=rd16(f+logic); used+=rd16(f+logic+used);
    memcpy(out,f+logic,used);
    for(unsigned p=0;p<count;p++) for(unsigned c=0;c<f[entry+9];c++) if(rd16(f+entry+10+6*c)==pages[p]) {
        u32 at=rd32(f+entry+12+6*c),n=rd16(f+at); memcpy(out+used,f+at,n); used+=n;
    }
    return used;
}
static int keyb_load(const u8 *tables,u32 n,const char *language,u16 page) {
    DosKeybRequest r={.size=sizeof(r),.language={language[0],language[1]},.code_page=page,.tables=tables,.table_size=n};
    return dos_keyb(DOS_KEYB_LOAD,&r);
}
/* A key as the firmware gives it with its shift state. */
static void key_mod(u32 c,unsigned scan,u32 modifiers) {
    CHECK(key_count<ARRAY_SIZE(keys));
    keys[key_count++]=(IoEvent){.type=IO_EVENT_KEY,.unicode=c,.scan=scan,.modifiers=modifiers,.flags=IO_KEY_MODIFIERS_VALID};
}
static void test_keyb(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    unsigned file,con; u32 size,n; u8 *kbd=malloc(65536),*cpi=malloc(65536),tables[DOS_KEYB_TABLE_MAX]; CHECK(kbd && cpi);
    CHECK(!dos_open("KEYBOARD.SYS",0,0,&file) && !dos_read(file,kbd,65536,&size) && size>20000 && size<65536 && !dos_close(file));
    CHECK(!dos_open("EFI.CPI",0,0,&file) && !dos_read(file,cpi,65536,&n) && !dos_close(file));
    CHECK(!dos_open("CON",2,0,&con));
    DosKeybRequest q={.size=sizeof(q)};
    CHECK(dos_keyb(DOS_KEYB_QUERY,&q)==DE_FUNCTION && dos_keyb(DOS_KEYB_MODE,&q)==DE_FUNCTION);
    key_string(""); key_mod('y',0,0); CHECK(console_call(8)=='y');
    /* German: Y and Z change places, AltGr, dead keys, the code page's bytes. */
    const u16 both[]={437,850}; u32 gr=keyb_tables(kbd,size,"GR",both,2,tables);
    CHECK(keyb_load(tables,gr,"GR",865)==DE_NOFILE && dos_keyb(DOS_KEYB_QUERY,&q)==DE_FUNCTION);
    CHECK(keyb_load(tables,4,"GR",437)==DE_FORMAT && keyb_load(tables,DOS_KEYB_TABLE_MAX+1,"GR",437)==DE_NOMEM);
    CHECK(!keyb_load(tables,gr,"GR",437));
    CHECK(!dos_keyb(DOS_KEYB_QUERY,&q) && !memcmp(q.language,"GR",2) && q.code_page==437 && q.flags==DOS_KEYB_FOREIGN);
    key_string(""); key_mod('y',0,0); key_mod('z',0,0); key_mod('Y',0,IO_MOD_SHIFT); key_mod('q',0,IO_MOD_ALT|IO_MOD_RIGHT_ALT);
    key_mod('@',0,IO_MOD_SHIFT); key_mod('#',0,IO_MOD_SHIFT); key_mod('-',0,0);
    CHECK(console_call(8)=='z' && console_call(8)=='y' && console_call(8)=='Z' && console_call(8)=='@');
    CHECK(console_call(8)=='"' && console_call(8)==0x15 && console_call(8)==0xe1);
    /* Without its shift state a key is not KEYB's: a serial terminal's. */
    key_string("y"); CHECK(console_call(8)=='y');
    /* The acute dead key, then e; then x, which it does not take: the
     * accent's own character, then x. A look takes the dead key away. */
    key_string(""); key_mod('=',0,0); key_mod('e',0,0); key_mod('=',0,0); key_mod('x',0,0);
    CHECK(console_call(8)==0x82 && console_call(8)=='\'' && console_call(8)=='x' && key_read==key_count);
    key_string(""); key_mod('=',0,0); CHECK(console_call(0x0b)==0 && key_read==1);
    key_append(""); key_mod(' ',0,0); CHECK(console_call(0x0b)==255 && console_call(0x0b)==255 && console_call(8)=='\'');
    /* A key KEYB types stays the firmware's while it is only looked at:
     * VDM's INT 16h reads the firmware itself. */
    key_string(""); key_mod('y',0,0); CHECK(console_call(0x0b)==255 && !key_read && console_call(8)=='z' && key_read==1);
    key_string(""); key_mod('=',0,0); key_mod('x',0,0);
    CHECK(console_call(0x0b)==255 && key_read==1 && console_call(8)=='\'' && key_read==2);
    CHECK(console_call(0x0b)==255 && console_call(8)=='x' && key_read==2);
    /* Function keys keep the BIOS's codes; Ctrl+Alt+F1 and F2 switch to US
     * and back, typing nothing. */
    key_string(""); key_mod(0,IO_SCAN_F1,0); CHECK(console_call(8)==0 && console_call(8)==59);
    key_string(""); key_mod(0,IO_SCAN_F1,IO_MOD_CONTROL|IO_MOD_ALT); key_mod('y',0,0);
    key_mod(0,IO_SCAN_F2,IO_MOD_CONTROL|IO_MOD_ALT); key_mod('y',0,0);
    CHECK(console_call(8)=='y' && console_call(8)=='z' && key_read==key_count);
    q.flags=0; CHECK(!dos_keyb(DOS_KEYB_MODE,&q) && !dos_keyb(DOS_KEYB_QUERY,&q) && !q.flags);
    key_string(""); key_mod('y',0,0); CHECK(console_call(8)=='y');
    q.flags=DOS_KEYB_FOREIGN; CHECK(!dos_keyb(DOS_KEYB_MODE,&q));
    /* What VDM's INT 16h asks of it. */
    u16 words[DOS_KEYB_KEYS]; IoEvent e={.type=IO_EVENT_KEY,.unicode='y',.flags=IO_KEY_MODIFIERS_VALID};
    CHECK(dos_keyb_key(&e,words)==1 && words[0]==0x157a);
    e.unicode='='; CHECK(dos_keyb_key(&e,words)==0);
    e.unicode='x'; CHECK(dos_keyb_key(&e,words)==(1|DOS_KEYB_BIOS) && words[0]==0x0027);
    e.unicode='a'; CHECK(dos_keyb_key(&e,words)==DOS_KEYB_BIOS);
    e.flags=0; e.unicode='y'; CHECK(dos_keyb_key(&e,words)==DOS_KEYB_BIOS);
    /* CON switches; KEYB follows with the tables it has (DISPLAY.SYS's AD81h),
     * or refuses the page after the display has switched. */
    {const u16 pages[]={850}; CHECK(!cp_prepare(con,pages,1,cpi,n));}
    CHECK(!cp_page(con,DOS_CP_SELECT,850) && !dos_keyb(DOS_KEYB_QUERY,&q) && q.code_page==850);
    key_string(""); key_mod('#',0,IO_MOD_SHIFT); CHECK(console_call(8)==0xf5); /* 850's section sign */
    CHECK(!cp_page(con,DOS_CP_SELECT,437) && !dos_keyb(DOS_KEYB_QUERY,&q) && q.code_page==437);
    gr=keyb_tables(kbd,size,"GR",both,1,tables); CHECK(!keyb_load(tables,gr,"GR",437));
    CHECK(cp_page(con,DOS_CP_SELECT,850)==DOS_CP_NOT_IN_FILE && cp_selected(con)==850);
    CHECK(!dos_keyb(DOS_KEYB_QUERY,&q) && q.code_page==437);
    q.code_page=850; CHECK(dos_keyb(DOS_KEYB_CODE_PAGE,&q)==DE_NOFILE);
    CHECK(set_page(850)==DOS_CP_SYSTEM_NOT_PREPARED && global_page()==850);
    CHECK(!cp_page(con,DOS_CP_SELECT,437) && !set_page(437));
    /* Japanese: the JIS symbols; Hankaku/Zenkaku types nothing; letters are
     * the BIOS's; Ctrl+@ is NUL with the 2 key's scan code. */
    const u16 jp_pages[]={932,437}; u32 jp=keyb_tables(kbd,size,"JP",jp_pages,2,tables);
    CHECK(!keyb_load(tables,jp,"JP",437));
    key_string(""); key_mod('[',0,0); key_mod(']',0,0); key_mod('=',0,0); key_mod('`',0,0); key_mod('a',0,0);
    key_mod('@',0,IO_MOD_SHIFT); key_mod(')',0,IO_MOD_SHIFT); key_mod('\'',0,IO_MOD_SHIFT); key_mod('[',0,IO_MOD_CONTROL);
    CHECK(console_call(8)=='@' && console_call(8)=='[' && console_call(8)=='^' && console_call(8)=='a');
    /* (AH=07h: the 3 after the NUL would be ^C to a break check, as DOS's.) */
    CHECK(console_call(8)=='"' && console_call(8)=='*' && console_call(7)==0 && console_call(7)==3 && key_read==key_count);
    CHECK(!dos_close(con)); free(kbd); free(cpi); free(arena);
    puts("PASS keyb: DOS 4's KEYBOARD.SYS state logic for German and Japanese, dead keys, hot keys, VDM's keys and CON's code pages");
}
typedef struct {
    unsigned action,calls;
    DosCriticalError event;
    MemoryDisk *disk;
    unsigned clear_at;
} CriticalProbe;
static unsigned critical_probe(void *context,const DosCriticalError *event) {
    CriticalProbe *probe=context; probe->calls++; probe->event=*event;
    CHECK(event->size==sizeof(*event) && event->pid==dos_pid());
    DosRegs r={.ax=0x3000}; CHECK(call(&r)==DE_BUSY);
    DosInfo info; DosExtendedError error; DosCriticalHandler handler;
    CHECK(dos_query(&info)==DE_BUSY && dos_extended_error(&error)==DE_BUSY);
    CHECK(dos_exec("HELLO.EFI","")==DE_BUSY);
    CHECK(dos_critical_handler(NULL,&handler)==DE_BUSY);
    if(probe->clear_at==probe->calls) probe->disk->fail_read=0;
    return probe->action;
}
static int critical_mkdir(void) {
    DosRegs r={.ax=0x3900,.dx=(uintptr_t)"CRITDIR"}; return call(&r);
}
static int critical_read(unsigned h,u8 *data,u32 length,u32 *done) {
    DosRegs r={.ax=0x3f00,.bx=h,.cx=length,.dx=(uintptr_t)data};
    int e=call(&r); *done=r.cx; return e;
}
/* The text screen ANSI.SYS works on: a cursor, an attribute and the last
 * erase and mode set, as IO.SYS's text_* calls leave them. */
static IoTextScreen text_screen={.size=sizeof(IoTextScreen),.columns=80,.rows=25,.attribute=7};
static u32 text_flags,erase_at[3],mode_set,text_calls;
int platform_text_available(void) {return 1;}
int platform_text_query(IoTextScreen *s) {text_calls++; *s=text_screen; return 0;}
int platform_text_locate(u32 column,u32 row) {
    text_calls++; if(column>=text_screen.columns || row>=text_screen.rows) return DE_FUNCTION;
    text_screen.column=column; text_screen.row=row; return 0;
}
int platform_text_attribute(u32 attribute,u32 flags) {text_calls++; text_screen.attribute=attribute; text_flags=flags; return 0;}
int platform_text_erase(u32 column,u32 row,u32 cells) {text_calls++; erase_at[0]=column; erase_at[1]=row; erase_at[2]=cells; return 0;}
int platform_text_mode(u32 mode) {text_calls++; mode_set=mode; return 0;}
static unsigned ansi_con;
static void ansi_out(const char *s) {DosRegs r={.ax=0x4000,.bx=ansi_con,.cx=strlen(s),.dx=(uintptr_t)s}; CHECK(!call(&r) && r.ax==strlen(s));}
static void ansi_bytes(const char *s) {while(*s) {DosRegs r={.ax=0x0200,.dx=(u8)*s++}; CHECK(!call(&r));}}
static int at(u32 column,u32 row) {return text_screen.column==column && text_screen.row==row;}
static void test_ansi(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    capture_console=1; console_text_size=0; CHECK(!dos_open("CON",2,0,&ansi_con)); text_calls=0;
    /* Without ANSI.SYS the sequences are text, and nothing else may turn it on. */
    ansi_out("\x1b[2J"); CHECK(console_text_size==4 && console_text[0]==27 && !text_calls);
    u32 value=DOS_ANSI_ON,now=99; CHECK(dos_installed(DOS_INSTALLED_ANSI,&value,NULL)==DE_ACCESS);
    /* HIMEM.SYS's and EMM386.SYS's values too come only from DEVICE=. */
    CHECK(dos_installed(DOS_INSTALLED_XMS,&value,NULL)==DE_ACCESS && dos_installed(DOS_INSTALLED_EMS,&value,NULL)==DE_ACCESS);
    CHECK(!dos_installed(DOS_INSTALLED_XMS,NULL,&now) && !now);
    CHECK(!dos_installed(DOS_INSTALLED_ANSI,NULL,&now) && !now);
    u8 packet[18]={0}; wr16(packet+2,14);
    DosRegs r={.ax=0x440c,.bx=ansi_con,.cx=0x037f,.dx=(uintptr_t)packet,.si=sizeof(packet)}; CHECK(call(&r)==DE_FUNCTION);
    ansi_set_options(DOS_ANSI_ON); CHECK(text_screen.attribute==7);
    console_text_size=0;
    /* Cursor movement, a sequence split across AH=02h calls, edges. */
    ansi_out("\x1b[5;10H"); CHECK(at(9,4) && !console_text_size);
    ansi_bytes("\x1b[3A"); CHECK(at(9,1));
    ansi_out("\x1b[9A\x1b[B"); CHECK(at(9,1));
    ansi_out("\x1b[200C"); CHECK(at(79,1));
    ansi_out("\x1b[D\x1b[2D"); CHECK(at(76,1));
    ansi_out("\x1b[30B"); CHECK(at(76,24));
    ansi_out("\x1b[26;1H"); CHECK(at(76,24)); /* a row below the screen: no move */
    ansi_out("\x1b[25;99f"); CHECK(at(79,24));
    ansi_out("\x1b[H"); CHECK(at(0,0));
    ansi_out("\x1b[7;3H\x1b[s\x1b[1;1H\x1b[u"); CHECK(at(2,6));
    /* Erasing: the screen (whatever the parameter) homes the cursor; a line from it on. */
    ansi_out("\x1b[K"); CHECK(erase_at[0]==2 && erase_at[1]==6 && erase_at[2]==78 && at(2,6));
    ansi_out("\x1b[0J"); CHECK(!erase_at[0] && !erase_at[1] && erase_at[2]==2000 && at(0,0));
    /* Attributes by DOS 4's masks: bold blue on white, reverse, concealed, reset. */
    ansi_out("\x1b[1;34;47m"); CHECK(text_screen.attribute==0x79);
    ansi_out("\x1b[7m"); CHECK(text_screen.attribute==0x78);
    ansi_out("\x1b[8m"); CHECK(text_screen.attribute==0x08);
    ansi_out("\x1b[5;99m"); CHECK(text_screen.attribute==0x88);
    ansi_out("\x1b[m"); CHECK(text_screen.attribute==0x07);
    /* Modes: wrap off and on, a video mode, others passed over. */
    ansi_out("\x1b[?7l"); CHECK(text_flags==IO_TEXT_NOWRAP);
    ansi_out("\x1b[=7h"); CHECK(!text_flags);
    ansi_out("\x1b[=3h"); CHECK(mode_set==3);
    mode_set=99; ansi_out("\x1b[=9h\x1b[=20l"); CHECK(mode_set==99);
    /* Text around sequences; an unknown letter shows, as does a character after a lone ESC. */
    console_text_size=0; ansi_out("ab\x1b[1;1Hcd\x1b[zy\x1bxw");
    static const u16 shown[]={'a','b','c','d','z','y','x','w'}; CHECK(text_was(shown,8));
    /* The cursor report is typed back; AH=0Ch's flush drops it. */
    ansi_out("\x1b[12;34H\x1b[6n"); u8 reply[10];
    for(unsigned i=0;i<9;i++) {DosRegs k={.ax=0x0800}; CHECK(!call(&k)); reply[i]=(u8)k.ax;}
    CHECK(!memcmp(reply,"\x1b[12;34R\r",9));
    ansi_out("\x1b[6n"); r=(DosRegs){.ax=0x0c00}; CHECK(!call(&r)); CHECK(console_call(0x0b)==0);
    /* Key reassignment: a character, an extended key, quoted strings; redefinition and removal. */
    ansi_out("\x1b[65;\"xyz\"p"); key_string("AB");
    for(unsigned i=0;i<4;i++) {DosRegs k={.ax=0x0700}; CHECK(!call(&k)); reply[i]=(u8)k.ax;}
    CHECK(!memcmp(reply,"xyzB",4));
    ansi_out("\x1b[0;59;\"dir\";13p"); key_string(""); key_scan(IO_SCAN_F1);
    CHECK(console_call(0x0b)==255);
    for(unsigned i=0;i<4;i++) {DosRegs k={.ax=0x0700}; CHECK(!call(&k)); reply[i]=(u8)k.ax;}
    CHECK(!memcmp(reply,"dir\r",4));
    ansi_out("\x1b[65;'q'p"); key_string("A"); CHECK(console_call(7)=='q');
    ansi_out("\x1b[65;65p"); key_string("A"); CHECK(console_call(7)=='A');
    ansi_out("\x1b[65p"); key_string("A"); CHECK(console_call(7)=='A');
    key_string(""); key_scan(IO_SCAN_F1); CHECK(console_call(7)=='d');
    r=(DosRegs){.ax=0x0c00}; CHECK(!call(&r));
    /* /X by sequence, seen through the installed value. */
    ansi_out("\x1b[1q"); CHECK(!dos_installed(DOS_INSTALLED_ANSI,NULL,&now) && now==(DOS_ANSI_ON|DOS_ANSI_X));
    ansi_out("\x1b[0q"); CHECK(!dos_installed(DOS_INSTALLED_ANSI,NULL,&now) && now==DOS_ANSI_ON);
    /* Display information: get, and set to what it is. */
    memset(packet,0,sizeof(packet)); wr16(packet+2,14);
    r=(DosRegs){.ax=0x440c,.bx=ansi_con,.cx=0x037f,.dx=(uintptr_t)packet,.si=sizeof(packet)};
    CHECK(!call(&r) && rd16(packet+2)==14 && packet[6]==1 && rd16(packet+8)==16 && rd16(packet+14)==80 && rd16(packet+16)==25);
    r=(DosRegs){.ax=0x440c,.bx=ansi_con,.cx=0x035f,.dx=(uintptr_t)packet,.si=sizeof(packet)}; CHECK(!call(&r));
    wr16(packet+16,43); r=(DosRegs){.ax=0x440c,.bx=ansi_con,.cx=0x035f,.dx=(uintptr_t)packet,.si=sizeof(packet)}; CHECK(call(&r)==DE_FUNCTION);
    CHECK(!dos_close(ansi_con)); key_string(""); capture_console=0; console_text_size=0; ansi_reset(); free(arena);
    puts("PASS ansi: ANSI.SYS cursor, erase, attribute, mode, report and key reassignment sequences, display IOCTL");
}
static void put_text(const char *name,const char *text) {
    unsigned h; u32 done; CHECK(!dos_open(name,2,1,&h));
    CHECK(!dos_write(h,text,(u32)strlen(text),&done) && done==strlen(text)); CHECK(!dos_close(h));
}
/* PRINT's queue and output through MSDOS.SYS (DosApi print and idle). */
static void test_print(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    DosPrintRequest r={.size=sizeof(r)};
    CHECK(dos_print(DOS_PRINT_QUERY,&r)==DE_FUNCTION && dos_print(DOS_PRINT_SUBMIT,&r)==DE_FUNCTION);
    strcopy(r.device,sizeof(r.device),"NOWHERE"); CHECK(dos_print(DOS_PRINT_INSTALL,&r)==DE_NOFILE);
    put_text("PRT1.TXT","a\tb\r\nxy\tz\r\n\x1a" "after"); put_text("PRT2.TXT","two\r\n");
    strcopy(r.device,sizeof(r.device),"CON:"); r.queue_entries=4;
    CHECK(!dos_print(DOS_PRINT_INSTALL,&r) && dos_print(DOS_PRINT_INSTALL,&r)==DE_ACCESS && !dos_print(DOS_PRINT_QUERY,&r));
    /* Held, four fill the queue, a fifth does not fit. */
    CHECK(!dos_print(DOS_PRINT_STATUS,&r) && !r.queue[0] && !strcmp(r.device,"CON"));
    const char *names[]={"C:\\PRT1.TXT","c:\\prt2.txt","C:\\PRT2.TXT","C:\\PRT1.TXT"};
    for(unsigned i=0;i<4;i++) {strcopy(r.path,sizeof(r.path),names[i]); CHECK(!dos_print(DOS_PRINT_SUBMIT,&r));}
    CHECK(dos_print(DOS_PRINT_SUBMIT,&r)==DE_NOMEM);
    CHECK(!dos_print(DOS_PRINT_STATUS,&r) && !strcmp(r.queue,"C:\\PRT1.TXT") && !strcmp(r.queue+64,"C:\\PRT2.TXT") &&
          !strcmp(r.queue+192,"C:\\PRT1.TXT") && !r.queue[256]);
    /* Cancels: a name (every entry), none matching, a pattern. */
    strcopy(r.path,sizeof(r.path),"C:\\PRT2.TXT"); CHECK(!dos_print(DOS_PRINT_CANCEL,&r));
    CHECK(!strcmp(r.queue,"C:\\PRT1.TXT") && !strcmp(r.queue+64,"C:\\PRT1.TXT") && !r.queue[128]);
    strcopy(r.path,sizeof(r.path),"C:\\Q*.TXT"); CHECK(dos_print(DOS_PRINT_CANCEL,&r)==DE_NOFILE);
    strcopy(r.path,sizeof(r.path),"C:\\PRT1.*"); CHECK(!dos_print(DOS_PRINT_CANCEL,&r) && !r.queue[0]);
    for(unsigned i=0;i<2;i++) {strcopy(r.path,sizeof(r.path),names[i]); CHECK(!dos_print(DOS_PRINT_SUBMIT,&r));}
    /* Nothing prints while held; released, the idle time prints both. */
    capture_console=1; console_size=0;
    CHECK(dos_idle()==0 && !console_size);
    CHECK(!dos_print(DOS_PRINT_RELEASE,&r));
    for(unsigned i=0;i<64 && (dos_idle() || r.queue[0]);i++) {}
    CHECK(!strcmp(console_output,"a       b\r\nxy      z\r\n\f" "two\r\n\f"));
    CHECK(!dos_print(DOS_PRINT_STATUS,&r) && !r.queue[0] && !dos_print(DOS_PRINT_RELEASE,&r));
    capture_console=0;
    CHECK(!dos_remove("PRT1.TXT",0) && !dos_remove("PRT2.TXT",0));
    free(arena);
}
static void test_critical(Disk *disk,MemoryDisk *md) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    CHECK(!dos_init(disk,arena));
    unsigned h; u32 n,pos;
    u8 data[1024],out[1024]; for(unsigned i=0;i<sizeof(data);i++) data[i]=(i*29+17)&255;
    CHECK(!dos_open("CRIT.TMP",2,2,&h)); CHECK(!dos_write(h,data,sizeof(data),&n) && n==sizeof(data));
    CHECK(!dos_close(h));
    u8 *before=malloc(md->bytes); CHECK(before!=NULL); memcpy(before,md->data,md->bytes);
    CriticalProbe probe={.action=DOS_CRITICAL_FAIL,.disk=md};
    DosCriticalHandler handler={critical_probe,&probe},previous;
    CHECK(!dos_critical_handler(&handler,&previous) && !previous.handler);
    /* Failed writes can have changed any prefix of a sector. Fail must return
     * the native DOS error while retaining the original cause and restoring
     * the whole volume. Recovery never calls the application handler. */
    for(unsigned torn=0;torn<=512;torn+=256) {
        probe.calls=0; md->fail_write_at=md->writes+1; md->failed_write_bytes=torn;
        CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1);
        CHECK(!memcmp(before,md->data,md->bytes) && !dos_volume.faulted && !fat_pages);
        CHECK(probe.event.operation==DOS_CRITICAL_WRITE && probe.event.drive==2 && probe.event.generation==1);
        CHECK((probe.event.flags&DOS_ERROR_COMMIT) && !(probe.event.allowed&(1U<<DOS_CRITICAL_IGNORE)));
        DosExtendedError error; CHECK(!dos_extended_error(&error));
        CHECK(error.error==DE_IO && error.error_class==DOS_CLASS_HARDWARE && error.locus==DOS_LOCUS_DISK);
        CHECK(error.sector==probe.event.sector && !(error.flags&DOS_ERROR_UNRECOVERED));
        DosRegs r={.ax=0x5900}; CHECK(!call(&r) && r.ax==DE_IO && r.bx==((DOS_CLASS_HARDWARE<<8)|DOS_ACTION_RETRY) && r.cx==(DOS_LOCUS_DISK<<8));
        md->fail_write_at=0;
    }
    probe.action=DOS_CRITICAL_IGNORE; probe.calls=0; md->fail_write_at=md->writes+1;
    CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1 && !memcmp(before,md->data,md->bytes)); md->fail_write_at=0;
    probe.action=DOS_CRITICAL_RETRY; probe.calls=0; md->fail_write_at=md->writes+1; md->failed_write_bytes=256;
    CHECK(!critical_mkdir() && probe.calls==1 && !fat_pages); md->fail_write_at=0;
    CHECK(!dos_remove("CRITDIR",1)); memcpy(before,md->data,md->bytes);
    probe.action=DOS_CRITICAL_FAIL; probe.calls=0; md->fail_flush_at=md->flushes+1;
    CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1 && probe.event.operation==DOS_CRITICAL_FLUSH);
    CHECK(!memcmp(before,md->data,md->bytes) && !dos_volume.faulted); md->fail_flush_at=0;
    /* Read retry resumes only the failed sector; ignored data is explicitly
     * zero-filled, whereas metadata can never be ignored. */
    CHECK(!dos_open("CRIT.TMP",0,0,&h)); probe.action=DOS_CRITICAL_RETRY; probe.calls=0;
    md->fail_read_at=md->reads+1; CHECK(!critical_read(h,out,sizeof(out),&n) && n==sizeof(out));
    CHECK(probe.calls==1 && !memcmp(out,data,sizeof(out))); md->fail_read_at=0;
    CHECK(!dos_seek(h,0,0,&pos)); probe.action=DOS_CRITICAL_IGNORE; probe.calls=0;
    md->fail_read_at=md->reads+1; memset(out,1,sizeof(out));
    CHECK(!critical_read(h,out,sizeof(out),&n) && n==sizeof(out)); md->fail_read_at=0;
    CHECK(probe.calls==1 && (probe.event.flags&DOS_ERROR_DATA));
    for(unsigned i=0;i<512;i++) CHECK(!out[i]);
    CHECK(!memcmp(out+512,data+512,512) && !memcmp(before,md->data,md->bytes));
    DosExtendedError error; CHECK(!dos_extended_error(&error) && (error.flags&DOS_ERROR_IGNORED));
    CHECK(!dos_close(h));
    /* Retrying a torn data write must leave the position and byte count at
     * exactly one completed request, including when the file grows. */
    CHECK(!dos_open("CRIT.TMP",2,0,&h)); CHECK(!dos_seek(h,0,2,&pos));
    probe.action=DOS_CRITICAL_RETRY; probe.calls=0;
    md->fail_write_at=md->writes+1; md->failed_write_bytes=256;
    DosRegs write={.ax=0x4000,.bx=h,.cx=sizeof(data),.dx=(uintptr_t)data};
    CHECK(!call(&write) && write.cx==sizeof(data) && probe.calls==1); md->fail_write_at=0;
    CHECK(!dos_seek(h,0,2,&pos) && pos==2*sizeof(data));
    CHECK(!dos_seek(h,sizeof(data),0,&pos));
    CHECK(!critical_read(h,out,sizeof(out),&n) && n==sizeof(out) && !memcmp(out,data,sizeof(out)));
    md->fail_flush_at=md->flushes+1; probe.calls=0;
    write=(DosRegs){.ax=0x6800,.bx=h};
    CHECK(!call(&write) && probe.calls==1 && probe.event.operation==DOS_CRITICAL_FLUSH);
    md->fail_flush_at=0; CHECK(!dos_close(h));
    probe.action=DOS_CRITICAL_IGNORE; probe.calls=0; md->fail_read=1;
    CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1 && !(probe.event.allowed&(1U<<DOS_CRITICAL_IGNORE)));
    probe.action=DOS_CRITICAL_RETRY; probe.calls=0; probe.clear_at=3;
    CHECK(!critical_mkdir() && probe.calls==3 && probe.event.attempt==3); probe.clear_at=0;
    CHECK(!dos_remove("CRITDIR",1));
    /* Extended-open fail-errors survives DUP and suppresses callbacks on data
     * reads, redirected character reads, commit and close. */
    probe.action=DOS_CRITICAL_FAIL; probe.calls=0; md->fail_read=1;
    DosRegs r={.ax=0x6c00,.si=(uintptr_t)"CRIT.TMP",.bx=DOS_OPEN_FAIL_ERRORS,.dx=1};
    CHECK(call(&r)==DE_IO && !probe.calls); md->fail_read=0;
    r=(DosRegs){.ax=0x6c00,.si=(uintptr_t)"CRIT.TMP",.bx=DOS_OPEN_FAIL_ERRORS,.dx=1}; CHECK(!call(&r)); h=r.ax;
    unsigned duplicate; CHECK(!dos_dup(h,&duplicate)); md->fail_read=1;
    CHECK(critical_read(duplicate,out,1,&n)==DE_IO && !n && !probe.calls);
    unsigned saved; CHECK(!dos_dup(0,&saved)); CHECK(!dos_dup2(h,0));
    r=(DosRegs){.ax=0x0700}; CHECK(call(&r)==DE_IO && !probe.calls);
    CHECK(!dos_dup2(saved,0)); CHECK(!dos_close(saved)); md->fail_read=0;
    md->fail_flush=1; r=(DosRegs){.ax=0x6800,.bx=h}; CHECK(call(&r)==DE_IO && !probe.calls);
    r=(DosRegs){.ax=0x3e00,.bx=h}; CHECK(call(&r)==DE_IO && !probe.calls);
    md->fail_flush=0; CHECK(!dos_close(duplicate));
    /* Queries, task destruction and shutdown are DOS entries too. Cleanup
     * must finish and a failed flush must never reach the hardware shutdown. */
    md->fail_read=1; dos_volume.cache_valid=0; probe.calls=0; DosInfo info;
    CHECK(dos_query(&info)==DE_CRITICAL && probe.calls==1);
    DosDriveInfo drive; probe.calls=0; CHECK(dos_drive_info(2,&drive)==DE_CRITICAL && probe.calls==1); md->fail_read=0;
    u32 child,parent=dos_pid(); CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    CHECK(!dos_critical_handler(NULL,&previous) && !previous.handler);
    CHECK(!dos_extended_error(&error) && !error.error);
    r=(DosRegs){.ax=0xff00}; CHECK(call(&r)==DE_FUNCTION);
    CHECK(!dos_task_select(parent)); CHECK(!dos_extended_error(&error) && error.error==DE_IO);
    probe.calls=0; md->fail_flush_at=md->flushes+1;
    CHECK(dos_task_destroy(child)==DE_CRITICAL && probe.calls==1 && dos_task_select(child)==DE_BLOCK);
    md->fail_flush_at=0; probe.calls=0; md->fail_flush=1;
    unsigned old_shutdowns=shutdowns; dos_shutdown(); CHECK(shutdowns==old_shutdowns && probe.calls==1);
    md->fail_flush=0; dos_shutdown(); CHECK(shutdowns==old_shutdowns+1);
    dos_volume.disk.readonly=1; probe.calls=0;
    CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1 && probe.event.error==DE_READONLY);
    CHECK(!(probe.event.allowed&(1U<<DOS_CRITICAL_RETRY))); dos_volume.disk.readonly=0;
    /* Failed restoration poisons the volume without additional callbacks. */
    memcpy(before,md->data,md->bytes); md->fail_write_at=md->writes+1; md->failed_write_bytes=512;
    md->fail_following_writes=1; probe.calls=0;
    CHECK(critical_mkdir()==DE_CRITICAL && probe.calls==1 && dos_volume.faulted);
    CHECK(!dos_extended_error(&error) && (error.flags&DOS_ERROR_UNRECOVERED));
    md->fail_write_at=md->fail_following_writes=0; md->failed_write_bytes=0;
    memcpy(md->data,before,md->bytes); CHECK(!dos_init(disk,arena));
    md->fail_read=1; CHECK(critical_mkdir()==DE_IO); md->fail_read=0;
    CHECK(!dos_remove("CRIT.TMP",0)); free(before); free(arena);
    puts("PASS critical errors: retries, rollback, extended causes, ignore restrictions, task isolation and safe shutdown");
}
static void test_critical_media(Disk *boot,MemoryDisk *source) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    MemoryDisk old={.data=malloc(source->bytes),.bytes=source->bytes};
    MemoryDisk replacement={.data=malloc(source->bytes),.bytes=source->bytes};
    CHECK(old.data && replacement.data);
    memcpy(old.data,source->data,source->bytes); memcpy(replacement.data,source->data,source->bytes);
    CHECK(!dos_init(boot,arena));
    for(unsigned i=0;i<ARRAY_SIZE(test_units);i++)
        test_units[i]=(TestUnit){.disk=source,.generation=1,.flags=IO_DISK_PRESENT|(i?0:IO_DISK_BOOT)};
    test_units[1].disk=&old; test_units[1].replacement=&replacement;
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.disk_count=unit_count,.disk_info=unit_info};
    CHECK(!dos_attach_disks(&io));
    CriticalProbe probe={.action=DOS_CRITICAL_RETRY,.disk=&old};
    DosCriticalHandler handler={critical_probe,&probe}; CHECK(!dos_critical_handler(&handler,NULL));
    test_units[1].replace_on_write=1;
    DosRegs r={.ax=0x3900,.dx=(uintptr_t)"D:\\CRITDIR"};
    CHECK(call(&r)==DE_CRITICAL && probe.calls==1 && probe.event.error==DE_CHANGED);
    CHECK(probe.event.drive==3 && !(probe.event.allowed&(1U<<DOS_CRITICAL_RETRY)));
    DosExtendedError error; CHECK(!dos_extended_error(&error));
    CHECK(error.error==DE_CHANGED && (error.flags&DOS_ERROR_UNRECOVERED));
    CHECK(!memcmp(replacement.data,source->data,source->bytes));
    DosDriveInfo drive; CHECK(!dos_drive_info(3,&drive) && drive.generation==2);
    r=(DosRegs){.ax=0x3900,.dx=(uintptr_t)"D:\\CRITDIR"}; CHECK(!call(&r));
    CHECK(!dos_remove("D:\\CRITDIR",1));
    CHECK(!dos_init(boot,arena)); free(old.data); free(replacement.data); free(arena);
    puts("PASS critical media: replacement forbids retry and rollback never writes the new medium");
}
typedef struct {
    unsigned calls[257],live,next,closed_pid;
    int init_error,finish_error,open_error,close_error,flush_error,read_error,overcount;
} DeviceProbe;
static DeviceProbe device_probes[2];
static unsigned module_mode,module_live,module_starts,module_stops,module_pages;
static unsigned module_allocations,module_fail_allocation;
static int block_probe_request(void *,DosBlockRequest *);
static int module_unload_failure;
static int probe_device(void *context,DosDeviceRequest *r) {
    DeviceProbe *p=context; CHECK(r->size==sizeof(*r) && r->command<ARRAY_SIZE(p->calls)); p->calls[r->command]++;
    DosRegs recursive={.ax=0x3000}; CHECK(call(&recursive)==DE_BUSY);
    CHECK(dos_device_register(NULL)==DE_BUSY);
    CHECK(dos_exec("HELLO.EFI","")==DE_BUSY);
    u32 child; CHECK(dos_task_create(&child)==DE_BUSY);
    switch(r->command) {
    case DOS_DEV_INIT: CHECK(r->io==driver_io && r->arguments); return p->init_error;
    case DOS_DEV_FINISH: CHECK(!p->live); return p->finish_error;
    case DOS_DEV_OPEN:
        if(p->open_error) return p->open_error;
        r->cookie=(void *)(uintptr_t)++p->next; p->live++; return 0;
    case DOS_DEV_CLOSE:
        CHECK(p->live && r->cookie); p->live--; p->closed_pid=r->pid; return p->close_error;
    case DOS_DEV_OUTPUT_FLUSH: return p->flush_error;
    case DOS_DEV_READ: case DOS_DEV_IOCTL_READ:
        r->transferred=MIN(r->count,2); if(r->transferred) memcpy(r->buffer,"AB",r->transferred);
        if(p->overcount) r->transferred=r->count+1;
        return p->read_error;
    case DOS_DEV_WRITE: case DOS_DEV_IOCTL_WRITE: r->transferred=r->count; return 0;
    case DOS_DEV_PEEK: *(u8 *)r->buffer='A'; return 0;
    case DOS_DEV_INPUT_STATUS: case DOS_DEV_OUTPUT_STATUS: r->ready=1; return 0;
    case DOS_DEV_INPUT_FLUSH: return 0;
    case DOS_DEV_GENERIC_IOCTL: return r->control==0x8102 && r->argument==77?0:DE_FUNCTION;
    default: return DE_FUNCTION;
    }
}
static DosDeviceSpec probe_spec(unsigned index) {
    DosDeviceSpec spec={.version=DOS_DEVICE_ABI,.size=sizeof(spec),.attributes=DOS_DEVICE_CHAR|DOS_DEVICE_OPEN_CLOSE|DOS_DEVICE_IOCTL|DOS_DEVICE_GENERIC,
        .capabilities=DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE,.context=&device_probes[index],.request=probe_device};
    strcopy(spec.name,sizeof(spec.name),index?"QOTHER":"QDEV"); return spec;
}
static int module_alloc(void *context,u32 pages,void **memory) {
    (void)context; *memory=NULL;
    if(++module_allocations==module_fail_allocation) return DE_NOMEM;
    *memory=aligned_alloc(4096,(size_t)pages*4096);
    if(!*memory) return DE_NOMEM;
    module_pages+=pages; return 0;
}
static void module_free(void *context,void *memory,u32 pages) {(void)context; CHECK(module_pages>=pages); module_pages-=pages; free(memory);}
static int module_load(void *context,const void *data,u32 size,u64 *token) {
    (void)context; CHECK(data && size>64); *token=0;
    if(module_mode==6) {
        DosBlockSpec block; DosDeviceSpec control; ramdisk_specs(&block,&control);
        int e=dos_device_register(&control); if(e) return e;
        e=dos_block_register(&block); if(e) return e;
    } else if(module_mode==7 || module_mode==8 || module_mode==9) {
        if(module_mode==9) {
            DosDeviceSpec control=probe_spec(0); int e=dos_device_register(&control); if(e) return e;
        }
        DosBlockSpec block={.version=DOS_BLOCK_ABI,.size=sizeof(block),.name="BPROBE",.request=block_probe_request};
        int e=dos_block_register(&block); if(e) return e;
        if(module_mode==8) return DE_FORMAT;
    } else if(module_mode==4) {
        for(unsigned i=0;i<PORT_DEVICE_COUNT;i++) {
            DosDeviceSpec spec; port_device_spec(i,&spec); int e=dos_device_register(&spec); if(e) return e;
        }
    } else if(module_mode!=5) {
        DosDeviceSpec spec=probe_spec(0); int e=dos_device_register(&spec); if(e) return e;
        if(module_mode==2) {spec=probe_spec(1); e=dos_device_register(&spec); if(e) return e;}
        if(module_mode==3) return DE_FORMAT;
    }
    module_live++; *token=++module_starts; return 0;
}
static int module_unload(void *context,u64 token) {
    (void)context; CHECK(token && module_live);
    if(module_unload_failure) return DE_IO;
    module_live--; module_stops++; return 0;
}
static void test_devices(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload}; driver_io=&io;
    memset(device_probes,0,sizeof(device_probes)); module_mode=2;
    device_probes[1].init_error=DE_IO;
    CHECK(dos_load_driver("LOOPDRV.SYS","two devices")==DE_IO);
    CHECK(!device_find("QDEV") && !device_find("QOTHER") && !module_live && !module_pages);
    CHECK(device_probes[0].calls[DOS_DEV_FINISH]==1 && device_probes[1].calls[DOS_DEV_FINISH]==1);
    module_mode=3; CHECK(dos_load_driver("LOOPDRV.SYS","")==DE_FORMAT);
    CHECK(!device_find("QDEV") && !module_live && !module_pages);
    module_mode=5; CHECK(dos_load_driver("LOOPDRV.SYS","")==DE_FORMAT && !module_live && !module_pages);
    module_mode=0; CHECK(!dos_load_driver("LOOPDRV.SYS","normal"));
    CHECK(module_live==1 && !module_pages && device_find("qdev.txt")==5);
    CHECK(dos_init(disk,arena)==DE_BUSY && device_find("QDEV")==5);
    CHECK(dos_load_driver("LOOPDRV.SYS","")==DE_EXISTS && module_live==1 && !module_pages);
    CHECK(dos_load_driver("HELLO.EFI","")==DE_FORMAT && module_live==1);
    CHECK(dos_device_register(NULL)==DE_ACCESS);
    DosDeviceInfo info; CHECK(!dos_device_info(4,&info) && !strcmp(info.name,"QDEV") && !info.open_descriptions);
    CHECK(dos_device_info(5,&info)==DE_NOMORE);
    unsigned h,dup; DeviceProbe *probe=&device_probes[0];
    probe->open_error=DE_ACCESS;
    CHECK(dos_open("QDEV",2,0,&h)==DE_ACCESS && !probe->live);
    probe->open_error=0; CHECK(!dos_open("C:\\QDEV:",2,0,&h) && probe->live==1);
    CHECK(!dos_dup(h,&dup) && probe->live==1); CHECK(!dos_device_info(4,&info) && info.open_descriptions==1);
    u8 buffer[4]={0}; probe->read_error=DE_IO;
    DosRegs r={.ax=0x3f00,.bx=dup,.cx=4,.dx=(uintptr_t)buffer};
    CHECK(call(&r)==DE_IO && r.cx==2 && !memcmp(buffer,"AB",2));
    DosExtendedError error; CHECK(!dos_extended_error(&error) && error.locus==DOS_LOCUS_DEVICE);
    probe->read_error=0; probe->overcount=1;
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=4,.dx=(uintptr_t)buffer}; CHECK(call(&r)==DE_IO && !r.cx);
    probe->overcount=0;
    r=(DosRegs){.ax=0x4402,.bx=h,.cx=sizeof(buffer),.dx=(uintptr_t)buffer}; CHECK(!call(&r) && r.ax==2 && r.cx==2);
    r=(DosRegs){.ax=0x4403,.bx=h,.cx=sizeof(buffer),.dx=(uintptr_t)buffer}; CHECK(!call(&r) && r.ax==sizeof(buffer));
    r=(DosRegs){.ax=0x440c,.bx=h,.cx=0x8102,.di=77}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x440c,.bx=h,.cx=0x8103,.di=77}; CHECK(call(&r)==DE_FUNCTION);
    r=(DosRegs){.ax=0x4406,.bx=h}; CHECK(!call(&r) && r.ax==255);
    r=(DosRegs){.ax=0x4407,.bx=h}; CHECK(!call(&r) && r.ax==255);
    r=(DosRegs){.ax=0x440a,.bx=h}; CHECK(!call(&r) && (r.dx&0x80) && !(r.dx&0x8000)); /* Local device word. */
    r=(DosRegs){.ax=0x4402,.bx=1ULL<<32,.cx=sizeof(buffer),.dx=(uintptr_t)buffer}; CHECK(call(&r)==DE_HANDLE && !r.cx);
    r=(DosRegs){.ax=0x4402,.bx=h,.cx=1ULL<<32,.dx=(uintptr_t)buffer}; CHECK(call(&r)==DE_FUNCTION && !r.cx);
    r=(DosRegs){.ax=0x4402,.bx=0,.cx=sizeof(buffer),.dx=(uintptr_t)buffer}; CHECK(call(&r)==DE_FUNCTION && !r.cx);
    unsigned saved; CHECK(!dos_dup(0,&saved) && !dos_dup2(h,0));
    r=(DosRegs){.ax=0x0c00}; CHECK(!call(&r) && probe->calls[DOS_DEV_INPUT_FLUSH]==1);
    CHECK(!dos_dup2(saved,0) && !dos_close(saved));
    u8 attr; CHECK(!dos_attribute("QDEV",0,&attr) && attr==0x40);
    CHECK(dos_remove("QDEV",0)==DE_ACCESS && dos_mkdir("QDEV")==DE_ACCESS && dos_chdir("QDEV")==DE_PATH);
    CHECK(dos_rename("QDEV","FILE")==DE_ACCESS && dos_rename("FILE","QDEV")==DE_ACCESS);
    CHECK(!dos_close(dup) && probe->live==1);
    u32 child,parent=dos_pid(); CHECK(!dos_task_create(&child)); CHECK(!dos_task_select(child));
    unsigned local; CHECK(!dos_open("QDEV",2,0,&local) && probe->live==2);
    CHECK(!dos_task_select(parent)); CHECK(!dos_task_destroy(child));
    CHECK(probe->live==1 && probe->closed_pid==child);
    probe->close_error=DE_IO; CHECK(dos_close(h)==DE_IO && !probe->live);
    CHECK(dos_close(h)==DE_HANDLE); probe->close_error=0;
    /* A failed commit-open still sends CLOSE, and repeated opens do not leak
     * the slot or the driver's cookie when flushing fails. */
    probe->flush_error=DE_IO;
    for(unsigned i=0;i<30;i++) CHECK(dos_open("QDEV",DOS_OPEN_RDWR|DOS_OPEN_COMMIT,0,&h)==DE_IO && !probe->live);
    probe->flush_error=0; CHECK(!dos_open("QDEV",2,0,&h));
    module_unload_failure=1;
    CHECK(dos_finish_drivers()==DE_IO && !probe->live && module_live==1 && dos_drivers_pending());
    unsigned finished=probe->calls[DOS_DEV_FINISH]; module_unload_failure=0;
    CHECK(!dos_finish_drivers() && !probe->live && !module_live && !module_pages && module_starts==module_stops);
    CHECK(probe->calls[DOS_DEV_FINISH]==finished && !dos_drivers_pending());
    CHECK(!device_find("QDEV") && dos_device_info(4,&info)==DE_NOMORE);
    CHECK(!dos_init(disk,arena));
    probe->init_error=DE_IO; module_unload_failure=1;
    CHECK(dos_load_driver("LOOPDRV.SYS","")==DE_IO && module_live==1 && dos_drivers_pending());
    CHECK(!device_find("QDEV")); module_unload_failure=0; probe->init_error=0;
    CHECK(!dos_finish_drivers() && !module_live && !dos_drivers_pending());
    /* Validation is atomic even when a driver ignores a registration error. */
    CHECK(!dos_init(disk,arena)); CHECK(!device_begin(1));
    DosDeviceSpec spec=probe_spec(0); CHECK(!device_register(&spec));
    spec.version=99; CHECK(device_register(&spec)==DE_FORMAT);
    CHECK(device_commit(1,&io,"")==DE_FORMAT && !device_find("QDEV"));
    CHECK(!device_begin(1)); spec=probe_spec(0); memset(spec.name,'X',sizeof(spec.name));
    CHECK(device_register(&spec)==DE_PATH); device_cancel(1);
    driver_io=NULL; free(arena);
    puts("PASS devices: resident lifecycle, atomic registration, request ABI, partial I/O, controls and shared handle cleanup");
}
typedef struct {
    u64 token;
    u8 regs[8],dll,dlm,rx[64],tx[1024];
    unsigned rx_pos,rx_used,tx_used,stop_after;
    u8 line_errors,modem,printer_status,no_fifo;
} PortModel;
static PortModel port_uart,port_printer;
static u64 port_sequence;
static unsigned port_operations,port_fail_at,port_delay,port_claims;
static int port_fail(void) {return ++port_operations==port_fail_at?DE_IO:0;}
static void port_model_reset(void) {
    CHECK(!port_claims);
    memset(&port_uart,0,sizeof(port_uart)); memset(&port_printer,0,sizeof(port_printer));
    port_uart.regs[1]=9; port_uart.regs[3]=3; port_uart.regs[4]=3; port_uart.regs[7]=0x57;
    port_uart.dll=12; port_uart.modem=0xb0;
    port_printer.regs[0]=0x37; port_printer.regs[2]=0x0c; port_printer.printer_status=0xd8;
    port_uart.stop_after=port_printer.stop_after=1024;
    port_operations=port_fail_at=port_delay=0;
}
static int port_claim(void *context,u32 kind,u32 base,u64 *token) {
    (void)context; *token=0;
    if(base==0x3f8) return DE_BUSY;
    PortModel *p=kind==IO_PORT_UART && base==0x2f8?&port_uart:kind==IO_PORT_PRINTER && base==0x378?&port_printer:NULL;
    if(!p) return DE_NOTREADY;
    if(p->token) return DE_BUSY;
    p->token=++port_sequence; *token=p->token; port_claims++; return 0;
}
static PortModel *port_lookup(u64 token) {
    return token && token==port_uart.token?&port_uart:token && token==port_printer.token?&port_printer:NULL;
}
static int port_buffering,port_buffer_calls,port_buffer_fail;
static int port_release(void *context,u64 token) {
    (void)context; PortModel *p=port_lookup(token); CHECK(p && port_claims);
    CHECK(!(port_buffering && p==&port_uart)); /* FINISH stops sampling before release. */
    p->token=0; port_claims--; return 0;
}
static void port_rx(u8 value) {
    CHECK(port_uart.rx_pos+port_uart.rx_used<sizeof(port_uart.rx));
    port_uart.rx[port_uart.rx_pos+port_uart.rx_used++]=value;
}
static int port_read(void *context,u64 token,u32 reg,u8 *value) {
    (void)context; PortModel *p=port_lookup(token); CHECK(p && reg<(p==&port_uart?8U:3U));
    int e=port_fail(); if(e) return e;
    if(p==&port_printer) {
        *value=reg==1?(p->printer_status&~(p->tx_used>=p->stop_after?0x80:0)):p->regs[reg]; return 0;
    }
    if(reg==0 && (p->regs[3]&0x80)) *value=p->dll;
    else if(reg==1 && (p->regs[3]&0x80)) *value=p->dlm;
    else if(reg==0) {
        CHECK(p->rx_used); *value=p->rx[p->rx_pos++]; if(!--p->rx_used) p->rx_pos=0;
    } else if(reg==2) *value=1|((p->regs[2]&1) && !p->no_fifo?0xc0:0);
    else if(reg==5) {
        *value=(p->tx_used>=p->stop_after?0:0x60)|(p->rx_used?1:0)|p->line_errors; p->line_errors=0;
    } else if(reg==6) *value=p->regs[4]&0x10?((p->regs[4]&2)?0x10:0)|((p->regs[4]&1)?0x20:0):p->modem;
    else *value=p->regs[reg];
    return 0;
}
static int port_write(void *context,u64 token,u32 reg,u8 value) {
    (void)context; PortModel *p=port_lookup(token); CHECK(p && reg<(p==&port_uart?8U:3U));
    int e=port_fail(); if(e) return e;
    if(p==&port_printer) {
        if(reg==2 && (value&1) && !(p->regs[2]&1)) {
            CHECK(p->tx_used<sizeof(p->tx)); p->tx[p->tx_used++]=p->regs[0];
        }
        p->regs[reg]=value; return 0;
    }
    if(reg==0 && (p->regs[3]&0x80)) p->dll=value;
    else if(reg==1 && (p->regs[3]&0x80)) p->dlm=value;
    else if(reg==0) {
        CHECK(p->tx_used<sizeof(p->tx)); p->tx[p->tx_used++]=value;
        if(p->regs[4]&0x10) port_rx(value);
    } else if(reg==2) {
        if(!value) CHECK(!port_buffering); /* Register restoration follows buffer shutdown. */
        p->regs[2]=value&1; if(value&2) p->rx_pos=p->rx_used=0;
    }
    else p->regs[reg]=value;
    return 0;
}
static int port_stall(void *context,u32 us) {(void)context; CHECK(us<=1000000); port_delay+=us; return 0;}
static int port_info(unsigned h,DosPortInfo *info) {
    DosRegs r={.ax=0x4402,.bx=h,.cx=sizeof(*info),.dx=(uintptr_t)info}; return call(&r);
}
static int port_config(unsigned h,const DosPortConfig *config) {
    DosRegs r={.ax=0x4403,.bx=h,.cx=sizeof(*config),.dx=(uintptr_t)config}; return call(&r);
}
static void port_restored(void) {
    CHECK(!port_claims && !port_uart.token && !port_printer.token);
    CHECK(port_uart.dll==12 && !port_uart.dlm && port_uart.regs[1]==9 && port_uart.regs[3]==3 &&
          port_uart.regs[4]==3 && port_uart.regs[7]==0x57);
    CHECK(port_printer.regs[0]==0x37 && port_printer.regs[2]==0x0c);
    CHECK(!module_live && !module_pages && !dos_drivers_pending());
}
static void test_ports(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.capabilities=IO_CAP_LEGACY_PORTS,
        .alloc_pages=module_alloc,.free_pages=module_free,.module_load=module_load,.module_unload=module_unload,
        .port_claim=port_claim,.port_release=port_release,.port_read=port_read,.port_write=port_write,.stall_us=port_stall};
    driver_io=&io; module_mode=4; key_string(""); port_model_reset(); CHECK(!dos_init(disk,arena));
    u8 bytes[16]; u32 n; unsigned h,alias,printer; DosRegs r;
    CHECK(dos_read(3,bytes,1,&n)==DE_NOTREADY && !n);
    CHECK(dos_write(4,"X",1,&n)==DE_NOTREADY && !n);
    CHECK(dos_open("C:\\COM4.txt",2,1,&h)==DE_NOTREADY);
    CHECK(dos_open("AUX:",2,0,&h)==DE_NOTREADY && dos_open("PRN",1,0,&h)==DE_NOTREADY);
    r=(DosRegs){.ax=0x0400,.dx='!'}; CHECK(call(&r)==DE_NOTREADY);
    r=(DosRegs){.ax=0x0500,.dx='!'}; CHECK(call(&r)==DE_NOTREADY);
    CHECK(!dos_load_driver("PORTDRV.SYS","COM1=2F8 LPT1=378"));
    unsigned startup_operations=port_operations;
    CHECK(port_claims==2 && !dos_bind_standard_devices());
    port_printer.printer_status=0;
    CHECK(!dos_flush()); /* An unused offline printer must not block disk flush. */
    port_printer.printer_status=0xd8;
    CHECK(device_find("aux.txt")==device_find("c:\\com1:") && device_find("prn:")==device_find("LPT1"));
    CHECK(!dos_open("COM1",2,0,&h) && !dos_open("AUX",2,0,&alias) && !dos_open("PRN",1,0,&printer));
    unsigned absent; CHECK(dos_open("COM2",2,0,&absent)==DE_NOTREADY);
    /* A create opens the write-only printer for writing, as MS-DOS does. */
    {unsigned created,action; CHECK(!dos_open_ex("LPT1:",2,0,0x12,&created,&action) && !dos_close(created));}
    CHECK(dos_open("LPT1",2,0,&absent)==DE_ACCESS);
    DosPortInfo info; CHECK(!port_info(3,&info) && info.kind==IO_PORT_UART && info.base==0x2f8);
    DosPortConfig config=info.config; config.timeout_us=300;
    CHECK(!port_config(alias,&config)); CHECK(!port_info(3,&info) && info.config.timeout_us==300);
    DosPortConfig bad=config; bad.baud=12345; CHECK(port_config(h,&bad)==DE_FUNCTION);
    bad=config; bad.timeout_us=1000001; CHECK(port_config(h,&bad)==DE_FUNCTION);
    bad=config; bad.data_bits=5; bad.stop_bits=DOS_STOP_TWO; CHECK(port_config(h,&bad)==DE_FUNCTION);
    bad=config; bad.data_bits=8; bad.stop_bits=DOS_STOP_ONE_HALF; CHECK(port_config(h,&bad)==DE_FUNCTION);
    config.flags|=DOS_PORT_LOOPBACK; CHECK(!port_config(h,&config));
    CHECK(!dos_write(h,"A\0\3",3,&n) && n==3);
    unsigned saved; CHECK(!dos_dup(0,&saved) && !dos_dup2(alias,0));
    r=(DosRegs){.ax=0x0b00}; CHECK(!call(&r) && r.ax==255);
    r=(DosRegs){.ax=0x0b00}; CHECK(!call(&r) && r.ax==255);
    CHECK(!dos_dup2(saved,0) && !dos_close(saved));
    r=(DosRegs){.ax=0x3f00,.bx=3,.cx=4,.dx=(uintptr_t)bytes};
    CHECK(call(&r)==DE_NOTREADY && r.cx==3 && !memcmp(bytes,"A\0\3",3) && port_delay==300);
    DosExtendedError extended; CHECK(!dos_extended_error(&extended) && extended.locus==DOS_LOCUS_DEVICE);
    config.flags&=~DOS_PORT_LOOPBACK; config.flags|=DOS_PORT_CTS_FLOW; CHECK(!port_config(h,&config));
    port_uart.modem=0; port_delay=0;
    CHECK(dos_write(h,"blocked",7,&n)==DE_NOTREADY && !n && port_delay==300);
    port_uart.modem=0x10; port_uart.stop_after=port_uart.tx_used+2;
    r=(DosRegs){.ax=0x4000,.bx=h,.cx=4,.dx=(uintptr_t)"1234"}; CHECK(call(&r)==DE_NOTREADY && r.cx==2);
    port_uart.stop_after=1024; port_rx('R'); port_uart.line_errors=4;
    r=(DosRegs){.ax=0x4406,.bx=h}; CHECK(!call(&r) && r.ax==255);
    CHECK(dos_read(h,bytes,1,&n)==DE_IO && !n); /* A status poll cannot erase a receive error. */
    CHECK(!port_info(h,&info) && info.receive_errors==4);
    r=(DosRegs){.ax=0x440c,.bx=h,.cx=DOS_PORT_CLEAR_INPUT}; CHECK(!call(&r) && !port_uart.rx_used);
    port_rx(3); r=(DosRegs){.ax=0x0300}; CHECK(!call(&r) && r.ax==3);
    r=(DosRegs){.ax=0x0400,.dx='@'}; CHECK(!call(&r) && port_uart.tx[port_uart.tx_used-1]=='@');
    CHECK(!port_info(4,&info) && info.kind==IO_PORT_PRINTER);
    DosPortConfig pc=info.config; pc.timeout_us=200; CHECK(!port_config(printer,&pc));
    port_printer.stop_after=2; port_delay=0;
    r=(DosRegs){.ax=0x4000,.bx=4,.cx=4,.dx=(uintptr_t)"abcd"}; CHECK(call(&r)==DE_NOTREADY && r.cx==2);
    CHECK(port_printer.tx_used==2 && !memcmp(port_printer.tx,"ab",2) && port_delay==204);
    port_printer.stop_after=1024;
    r=(DosRegs){.ax=0x0500,.dx='!'}; CHECK(!call(&r) && port_printer.tx[2]=='!');
    port_printer.printer_status=0xf8; CHECK(!port_info(4,&info) && (info.status&DOS_PORT_PAPER_OUT));
    r=(DosRegs){.ax=0x4407,.bx=4}; CHECK(!call(&r) && !r.ax);
    port_printer.printer_status=0xd8;
    u32 parent=dos_pid(),child; CHECK(!dos_task_create(&child) && !dos_task_select(child));
    CHECK(!dos_write(3,"child",5,&n) && n==5 && !dos_write(4,"kid",3,&n) && n==3);
    CHECK(!dos_task_select(parent) && !dos_task_destroy(child));
    CHECK(!dos_close(h) && !dos_close(alias) && !dos_close(printer));
    CHECK(!dos_finish_drivers()); port_restored();
    /* Every register access during initialization can fail. Registration must
     * stay atomic, and FINISH must restore everything already changed. */
    for(unsigned fault=1;fault<=startup_operations;fault++) {
        port_model_reset(); CHECK(!dos_init(disk,arena)); port_fail_at=fault;
        CHECK(dos_load_driver("PORTDRV.SYS","COM1=2F8 LPT1=378")==DE_IO);
        CHECK(device_find("COM1")==DOS_AUX_DEVICE && device_find("LPT1")==DOS_PRN_DEVICE);
        port_restored();
    }
    const char *invalid[]={"", "COM1=", "COM1=0", "COM5=2F8", "COM1=12345", "COM1=zz", "COM1=2F8 COM1=2F8",
        "COM1=2F8 COM2=2F8", "COM1=3F8", "COM1=2F8 LPT1=278"};
    for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
        port_model_reset(); CHECK(!dos_init(disk,arena)); CHECK(dos_load_driver("PORTDRV.SYS",invalid[i])); port_restored();
    }
    port_model_reset(); port_uart.no_fifo=1; CHECK(!dos_init(disk,arena));
    CHECK(dos_load_driver("PORTDRV.SYS","COM1=2F8")==DE_NOTREADY); port_restored();
    for(unsigned fault=3;fault<=4;fault++) {
        port_model_reset(); CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("PORTDRV.SYS","LPT1=378"));
        CHECK(!dos_bind_standard_devices()); port_fail_at=port_operations+fault;
        r=(DosRegs){.ax=0x4000,.bx=4,.cx=2,.dx=(uintptr_t)"xy"};
        CHECK(call(&r)==DE_IO && r.cx==(fault==4?1U:0U));
        CHECK(port_printer.tx_used==r.cx); /* Strobe committed before deassertion failed. */
        CHECK(!port_info(4,&info) && (info.status&DOS_PORT_FAULTED));
        CHECK(dos_write(4,"z",1,&n)==DE_IO && !n);
        CHECK(dos_finish_drivers()==DE_IO); port_restored();
        CHECK(port_printer.tx_used==(fault==4?1U:0U)); /* Cleanup cannot print again. */
    }
    port_model_reset(); CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("PORTDRV.SYS","COM1=2F8"));
    CHECK(!dos_bind_standard_devices()); port_fail_at=port_operations+6;
    r=(DosRegs){.ax=0x4000,.bx=3,.cx=3,.dx=(uintptr_t)"123"};
    CHECK(call(&r)==DE_IO && r.cx==1 && port_uart.tx_used==1 && port_uart.tx[0]=='1');
    port_rx('X'); port_rx('Y'); port_fail_at=port_operations+6;
    r=(DosRegs){.ax=0x3f00,.bx=3,.cx=2,.dx=(uintptr_t)bytes};
    CHECK(call(&r)==DE_IO && r.cx==1 && bytes[0]=='X' && port_uart.rx_used==1);
    CHECK(!dos_read(3,bytes,1,&n) && n==1 && bytes[0]=='Y');
    CHECK(!dos_finish_drivers()); port_restored();
    /* Failed configuration must not silently continue with partly written
     * framing/divisor registers. Unload still releases the resources. */
    for(unsigned fault=1;fault<=6;fault++) {
        port_model_reset(); CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("PORTDRV.SYS","COM1=2F8"));
        CHECK(!dos_bind_standard_devices() && !port_info(3,&info)); config=info.config; config.baud=9600;
        port_fail_at=port_operations+2+fault; /* Two status reads before the six configuration writes. */
        CHECK(port_config(3,&config)==DE_IO); CHECK(!port_info(3,&info) && (info.status&DOS_PORT_FAULTED));
        CHECK(dos_write(3,"bad",3,&n)==DE_IO && !n);
        CHECK(dos_finish_drivers()==DE_IO); port_restored();
    }
    driver_io=NULL; free(arena);
    printf("PASS ports: UART/printer I/O, aliases, standard handles, flow/timeouts, partial transfers, receive errors and %u startup faults\n",startup_operations);
}
static int port_buffer_mock(void *context,u64 token,u32 enable) {
    (void)context; CHECK(token && token==port_uart.token); port_buffer_calls++;
    if(port_buffer_fail) return DE_NOMEM;
    port_buffering=!!enable; return 0;
}
static int port_pending_mock(void *context,u64 token,u32 *bytes) {
    (void)context; CHECK(token==port_uart.token && port_buffering); *bytes=port_uart.rx_used; return 0;
}
typedef struct {
    u64 token; unsigned opens,closes,configs; int read_error,config_error;
    IoSerialConfig config; u32 lines; u8 rx[32],tx[64]; unsigned rx_pos,rx_used,tx_used;
} SerialModel;
static SerialModel firmware_uart;
static u32 serial_units(void *context) {(void)context; return 1;}
static int serial_open_mock(void *context,u32 unit,u64 *token) {
    (void)context; *token=0;
    if(unit) return DE_DRIVE;
    if(firmware_uart.token) return DE_BUSY;
    firmware_uart.token=0x5e00+ ++firmware_uart.opens; *token=firmware_uart.token; return 0;
}
static int serial_close_mock(void *context,u64 token) {
    (void)context; CHECK(token && token==firmware_uart.token); firmware_uart.token=0; firmware_uart.closes++; return 0;
}
static int serial_config_mock(void *context,u64 token,const IoSerialConfig *c) {
    (void)context; CHECK(token==firmware_uart.token && c && c->size==sizeof(*c)); firmware_uart.configs++;
    if(firmware_uart.config_error) return firmware_uart.config_error;
    firmware_uart.config=*c; return 0;
}
static int serial_status_mock(void *context,u64 token,u32 *bits) {
    (void)context; CHECK(token==firmware_uart.token);
    *bits=firmware_uart.lines|(firmware_uart.rx_used?0:IO_SERIAL_INPUT_EMPTY)|IO_SERIAL_OUTPUT_EMPTY|
        (firmware_uart.config.control&(IO_SERIAL_DTR|IO_SERIAL_RTS)); return 0;
}
static int serial_read_mock(void *context,u64 token,void *buffer,u32 count,u32 *done) {
    (void)context; CHECK(token==firmware_uart.token && count==1); *done=0;
    if(!firmware_uart.rx_used) return DE_NOTREADY;
    u8 value=firmware_uart.rx[firmware_uart.rx_pos++]; firmware_uart.rx_used--;
    if(firmware_uart.read_error) {int e=firmware_uart.read_error; firmware_uart.read_error=0; return e;}
    *(u8 *)buffer=value; *done=1; return 0;
}
static int serial_write_mock(void *context,u64 token,const void *buffer,u32 count,u32 *done) {
    (void)context; CHECK(token==firmware_uart.token && count==1 && firmware_uart.tx_used<sizeof(firmware_uart.tx));
    firmware_uart.tx[firmware_uart.tx_used++]=*(const u8 *)buffer; *done=1; return 0;
}
static void serial_rx(const char *text) {
    size_t n=strlen(text); CHECK(firmware_uart.rx_pos+firmware_uart.rx_used+n<=sizeof(firmware_uart.rx));
    memcpy(firmware_uart.rx+firmware_uart.rx_pos+firmware_uart.rx_used,text,n); firmware_uart.rx_used+=n;
}
static void test_serial_ports(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.capabilities=IO_CAP_LEGACY_PORTS|IO_CAP_PORT_BUFFER,
        .alloc_pages=module_alloc,.free_pages=module_free,.module_load=module_load,.module_unload=module_unload,
        .port_claim=port_claim,.port_release=port_release,.port_read=port_read,.port_write=port_write,.stall_us=port_stall,
        .port_buffer=port_buffer_mock,.port_pending=port_pending_mock};
    driver_io=&io; module_mode=4; key_string(""); port_model_reset(); CHECK(!dos_init(disk,arena));
    /* IO.SYS buffering is enabled after the UART is programmed and reported
     * through AH=4402; the peeked byte counts as buffered too. */
    port_buffer_calls=0; CHECK(!dos_load_driver("PORTDRV.SYS","COM1=2F8 LPT1=378") && port_buffering && port_buffer_calls==1);
    CHECK(!dos_bind_standard_devices());
    DosPortInfo info; u8 bytes[8]; u32 n; DosRegs r; unsigned saved,h;
    CHECK(!port_info(3,&info) && (info.status&DOS_PORT_BUFFERED) && !info.buffered);
    CHECK(!port_info(4,&info) && !(info.status&DOS_PORT_BUFFERED) && !info.buffered);
    port_rx('a'); port_rx('b');
    CHECK(!port_info(3,&info) && info.buffered==2 && (info.status&DOS_PORT_RX_READY));
    CHECK(!dos_dup(0,&saved) && !dos_dup2(3,0));
    r=(DosRegs){.ax=0x0b00}; CHECK(!call(&r) && r.ax==255);
    CHECK(!port_info(3,&info) && info.buffered==2 && port_uart.rx_used==1);
    CHECK(!dos_dup2(saved,0) && !dos_close(saved));
    CHECK(!dos_read(3,bytes,2,&n) && n==2 && !memcmp(bytes,"ab",2) && !port_info(3,&info) && !info.buffered);
    CHECK(!dos_finish_drivers() && !port_buffering && port_buffer_calls==2); port_restored();
    /* Buffering is optional; a failed enable leaves the UART polled. */
    port_buffer_fail=1; port_model_reset(); CHECK(!dos_init(disk,arena));
    CHECK(!dos_load_driver("PORTDRV.SYS","COM1=2F8") && !port_buffering && !dos_bind_standard_devices());
    CHECK(!port_info(3,&info) && !(info.status&DOS_PORT_BUFFERED));
    CHECK(!dos_finish_drivers()); port_restored(); port_buffer_fail=0;
    /* COMn=EFIu binds a DOS device to an IO.SYS firmware serial unit. */
    memset(&firmware_uart,0,sizeof(firmware_uart));
    IoServices efi={.version=IO_ABI_VERSION,.size=sizeof(efi),.capabilities=IO_CAP_SERIAL,
        .alloc_pages=module_alloc,.free_pages=module_free,.module_load=module_load,.module_unload=module_unload,
        .stall_us=port_stall,.serial_count=serial_units,.serial_open=serial_open_mock,.serial_close=serial_close_mock,
        .serial_config=serial_config_mock,.serial_status=serial_status_mock,.serial_read=serial_read_mock,.serial_write=serial_write_mock};
    driver_io=&efi;
    const char *invalid[]={"COM1=EFI","COM1=EFI16","COM1=EFI123","COM1=EFI0x","LPT1=EFI0","COM1=EFI1","COM1=EFI0 COM2=EFI0","COM1=2F8"};
    for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
        CHECK(!dos_init(disk,arena)); CHECK(dos_load_driver("PORTDRV.SYS",invalid[i]));
        CHECK(!firmware_uart.token && firmware_uart.opens==firmware_uart.closes && !module_live && !dos_drivers_pending());
    }
    efi.capabilities=0; CHECK(!dos_init(disk,arena)); CHECK(dos_load_driver("PORTDRV.SYS","COM2=EFI0")==DE_FUNCTION);
    efi.capabilities=IO_CAP_SERIAL; CHECK(!dos_init(disk,arena)); firmware_uart.configs=0;
    CHECK(!dos_load_driver("PORTDRV.SYS","com2=efi0") && firmware_uart.token && firmware_uart.configs==1);
    CHECK(firmware_uart.config.baud==115200 && firmware_uart.config.timeout_us==100000 && firmware_uart.config.data_bits==8);
    CHECK(firmware_uart.config.parity==IO_PARITY_NONE && firmware_uart.config.stop_bits==IO_STOP_ONE);
    CHECK(firmware_uart.config.control==(IO_SERIAL_DTR|IO_SERIAL_RTS));
    CHECK(!dos_open("COM2",2,0,&h));
    CHECK(!port_info(h,&info) && info.kind==IO_PORT_SERIAL && !info.base && !(info.status&DOS_PORT_BUFFERED));
    CHECK((info.status&(DOS_PORT_TX_READY|DOS_PORT_TX_EMPTY))==(DOS_PORT_TX_READY|DOS_PORT_TX_EMPTY));
    CHECK(info.line_status==0x60 && !info.modem_status && !(info.status&DOS_PORT_RX_READY));
    firmware_uart.lines=IO_SERIAL_CTS|IO_SERIAL_DSR|IO_SERIAL_RING|IO_SERIAL_CARRIER;
    CHECK(!port_info(h,&info) && info.modem_status==0xf0);
    CHECK((info.status&(DOS_PORT_CTS|DOS_PORT_DSR|DOS_PORT_CARRIER))==(DOS_PORT_CTS|DOS_PORT_DSR|DOS_PORT_CARRIER));
    DosPortConfig c=info.config; c.baud=9600; c.data_bits=7; c.parity=DOS_PARITY_EVEN; c.stop_bits=DOS_STOP_TWO;
    c.flags=DOS_PORT_DTR|DOS_PORT_LOOPBACK; c.timeout_us=300;
    CHECK(!port_config(h,&c) && firmware_uart.config.baud==9600 && firmware_uart.config.data_bits==7);
    CHECK(firmware_uart.config.parity==IO_PARITY_EVEN && firmware_uart.config.stop_bits==IO_STOP_TWO);
    CHECK(firmware_uart.config.control==(IO_SERIAL_DTR|IO_SERIAL_LOOPBACK) && firmware_uart.config.timeout_us==300);
    unsigned configs=firmware_uart.configs; DosPortConfig bad=c; bad.baud=12345;
    CHECK(port_config(h,&bad)==DE_FUNCTION && firmware_uart.configs==configs);
    firmware_uart.config_error=DE_FUNCTION; CHECK(port_config(h,&c)==DE_FUNCTION); firmware_uart.config_error=0;
    CHECK(!port_info(h,&info) && !(info.status&DOS_PORT_FAULTED) && info.config.timeout_us==300);
    CHECK(!dos_write(h,"AB",2,&n) && n==2 && firmware_uart.tx_used==2 && !memcmp(firmware_uart.tx,"AB",2));
    serial_rx("XY"); port_delay=0;
    r=(DosRegs){.ax=0x3f00,.bx=h,.cx=3,.dx=(uintptr_t)bytes};
    CHECK(call(&r)==DE_NOTREADY && r.cx==2 && !memcmp(bytes,"XY",2) && port_delay==300);
    serial_rx("P"); CHECK(!dos_dup(0,&saved) && !dos_dup2(h,0));
    r=(DosRegs){.ax=0x0b00}; CHECK(!call(&r) && r.ax==255);
    CHECK(!port_info(h,&info) && info.buffered==1 && (info.status&DOS_PORT_RX_READY) && !firmware_uart.rx_used);
    CHECK(!dos_dup2(saved,0) && !dos_close(saved));
    CHECK(!dos_read(h,bytes,1,&n) && n==1 && bytes[0]=='P');
    /* Firmware receive errors have no cause; they latch LSR bit 7 until cleared. */
    serial_rx("E"); firmware_uart.read_error=DE_IO; CHECK(dos_read(h,bytes,1,&n)==DE_IO && !n);
    CHECK(!port_info(h,&info) && info.receive_errors==0x80 && (info.status&DOS_PORT_ERROR) && (info.line_status&0x80));
    serial_rx("QR"); CHECK(dos_read(h,bytes,1,&n)==DE_IO && !n && firmware_uart.rx_used==2);
    r=(DosRegs){.ax=0x440c,.bx=h,.cx=DOS_PORT_CLEAR_INPUT}; CHECK(!call(&r) && !firmware_uart.rx_used);
    CHECK(!port_info(h,&info) && !info.receive_errors && !(info.status&DOS_PORT_ERROR));
    c.flags=DOS_PORT_CTS_FLOW; CHECK(!port_config(h,&c) && !firmware_uart.config.control);
    firmware_uart.lines=0; port_delay=0;
    CHECK(dos_write(h,"blocked",7,&n)==DE_NOTREADY && !n && port_delay==300 && firmware_uart.tx_used==2);
    firmware_uart.lines=IO_SERIAL_CTS; CHECK(!dos_write(h,"ok",2,&n) && n==2 && !memcmp(firmware_uart.tx+2,"ok",2));
    r=(DosRegs){.ax=0x6800,.bx=h}; CHECK(!call(&r));
    /* A device failure while reprogramming faults the unit until reload. */
    firmware_uart.config_error=DE_IO; CHECK(port_config(h,&c)==DE_IO); firmware_uart.config_error=0;
    CHECK(!port_info(h,&info) && (info.status&DOS_PORT_FAULTED) && dos_write(h,"x",1,&n)==DE_IO && !n);
    CHECK(dos_close(h)==DE_IO && !dos_finish_drivers()); /* The handle closes despite the failed flush. */
    CHECK(!firmware_uart.token && firmware_uart.opens==firmware_uart.closes && !module_live && !module_pages && !dos_drivers_pending());
    driver_io=NULL; free(arena);
    puts("PASS serial ports: IO.SYS receive buffering, firmware serial binding, configuration, timeouts, errors and faults");
}
typedef struct {
    MemoryDisk *disk,*replacement;
    u32 generation,units,flags,readonly;
    unsigned initialized,finished,calls,replace_on_write;
    int init_error,finish_error,media_error,media_error_unit,bad_media,bad_transfer;
} BlockProbe;
static BlockProbe block_probe;
static int block_probe_request(void *context,DosBlockRequest *r) {
    (void)context; BlockProbe *p=&block_probe; p->calls++;
    DosBlockInfo bi; DosInfo di; u32 task;
    CHECK(dos_block_info(0,&bi)==DE_BUSY && dos_block_register(NULL)==DE_BUSY);
    CHECK(dos_device_register(NULL)==DE_BUSY && dos_query(&di)==DE_BUSY && dos_task_create(&task)==DE_BUSY);
    CHECK(dos_exec("HELLO.EFI","")==DE_BUSY);
    if(r->command==DOS_BLOCK_INIT) {p->initialized++; r->units=p->units; return p->init_error;}
    if(r->command==DOS_BLOCK_FINISH) {p->finished++; return p->finish_error;}
    CHECK(r->unit<p->units);
    if(r->command==DOS_BLOCK_MEDIA) {
        if(p->media_error && (int)r->unit==p->media_error_unit) return p->media_error;
        DosBlockMedia media={.size=sizeof(media),.generation=p->generation,.flags=p->flags,
            .sector_bytes=512,.sectors=p->disk->bytes/512,.readonly=p->readonly};
        if(p->bad_media==1) media.generation=0;
        if(p->bad_media==2) media.sector_bytes=4096;
        if(p->bad_media==3) media.flags|=IO_DISK_BOOT;
        memcpy(r->buffer,&media,sizeof(media)); r->transferred=p->bad_media==4?0:sizeof(media); return 0;
    }
    if(r->generation!=p->generation) return DE_CHANGED;
    if(!(p->flags&IO_DISK_PRESENT)) return DE_NOTREADY;
    int e;
    if(r->command==DOS_BLOCK_FLUSH) return flush_block(p->disk);
    CHECK(r->count==1 && r->sector<=UINT32_MAX);
    if(r->command==DOS_BLOCK_READ) e=read_block(p->disk,r->sector,r->buffer);
    else {
        CHECK(r->command==DOS_BLOCK_WRITE);
        if(p->readonly) return DE_READONLY;
        if(p->replace_on_write && !--p->replace_on_write) {
            CHECK(r->sector*512+512<=p->disk->bytes);
            memcpy(p->disk->data+r->sector*512,r->buffer,256);
            p->disk=p->replacement; p->generation++; return DE_CHANGED;
        }
        e=write_block(p->disk,r->sector,r->buffer);
    }
    if(!e) r->transferred=p->bad_transfer==1?0:p->bad_transfer==2?2:1;
    return e;
}
static int ram_control(unsigned h,u32 command,u32 unit,u32 *value) {
    DosRegs r={.ax=0x440c,.bx=h,.cx=command,.di=unit,.si=value?4:0,.dx=(uintptr_t)value}; return call(&r);
}
static void test_ramdisk(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload}; driver_io=&io; module_mode=6;
    CHECK(!dos_init(disk,arena)); CHECK(dos_block_register(NULL)==DE_ACCESS);
    CHECK(!dos_load_driver("RAMDRV.SYS","/SIZE:2048 /UNITS:2"));
    CHECK(dos_load_driver("RAMDRV.SYS","")==DE_EXISTS && module_pages==1024);
    CHECK(dos_attach_disks(&io)==DE_BUSY);
    DosBlockInfo b; CHECK(!dos_block_info(0,&b) && b.units==2 && b.drive_mask==0x18 && !strcmp(b.name,"RAMDISK"));
    CHECK(dos_block_info(1,&b)==DE_NOMORE);
    DosDriveInfo d; CHECK(!dos_drive_info(3,&d) && d.fat_bits==12 && d.sectors==4096);
    CHECK(!dos_drive_info(4,&d) && d.free_clusters==d.total_clusters);
    unsigned ctl,h,other; u32 n,pos; u8 data[1500],out[1500];
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(u8)(i*13);
    CHECK(!dos_open("RAMCTL",2,0,&ctl));
    CHECK(!dos_mkdir("D:\\WORK") && !dos_open("D:\\WORK\\FILE.BIN",2,1,&h));
    CHECK(!dos_write(h,data,sizeof(data),&n) && n==sizeof(data));
    CHECK(!dos_seek(h,0,0,&pos) && !dos_read(h,out,sizeof(out),&n) && n==sizeof(out) && !memcmp(out,data,n));
    CHECK(!dos_open("E:\\KEEP.BIN",2,1,&other) && !dos_write(other,data,100,&n) && n==100 && !dos_close(other));
    DosFind find; CHECK(!dos_find_first("D:\\WORK\\*.*",0,&find));
    u32 parent=dos_pid(),child; CHECK(!dos_task_create(&child) && !dos_task_select(child));
    CHECK(!dos_chdir("D:\\WORK") && !dos_task_select(parent));
    u32 ids[DOS_DRIVES]; CHECK(block_units(1,ids)==2); IoDiskInfo snapshot;
    CHECK(!block_media(ids[0],&snapshot));
    CHECK(!ram_control(ctl,RAMDISK_RESET,0,NULL));
    CHECK(snapshot.disk.read(snapshot.disk.ctx,0,out)==DE_CHANGED);
    CHECK(snapshot.disk.write(snapshot.disk.ctx,0,data)==DE_CHANGED && snapshot.disk.flush(snapshot.disk.ctx)==DE_CHANGED);
    CHECK(!dos_drive_info(3,&d) && d.generation==2);
    CHECK(dos_find_next(&find)==DE_CHANGED && dos_read(h,out,1,&n)==DE_CHANGED && !n);
    CHECK(!dos_task_select(child)); char cwd[DOS_PATH_MAX]; CHECK(!dos_drive_cwd(3,cwd) && !strcmp(cwd,"\\"));
    CHECK(!dos_task_select(parent) && !dos_task_destroy(child));
    CHECK(dos_close(h)==DE_CHANGED && dos_close(h)==DE_HANDLE);
    CHECK(dos_open("D:\\WORK\\FILE.BIN",0,0,&h)==DE_PATH);
    CHECK(!dos_open("E:\\KEEP.BIN",0,0,&h) && !dos_read(h,out,100,&n) && n==100 && !memcmp(out,data,100) && !dos_close(h));
    u32 protection=1; CHECK(!ram_control(ctl,RAMDISK_PROTECT,1,&protection));
    CHECK(!dos_drive_info(4,&d) && (d.flags&DOS_DRIVE_READONLY));
    CHECK(dos_open("E:\\NEW.BIN",2,1,&h)==DE_READONLY);
    CHECK(ram_control(ctl,RAMDISK_RESET,1,NULL)==DE_READONLY);
    protection=0; CHECK(!ram_control(ctl,RAMDISK_PROTECT,1,&protection));
    CHECK(!dos_open("E:\\NEW.BIN",2,1,&h) && !dos_close(h));
    CHECK(!dos_close(ctl) && !dos_finish_drivers());
    CHECK(!module_pages && !module_live && dos_drive_info(3,&d)==DE_DRIVE && dos_block_info(0,&b)==DE_NOMORE);
    CHECK(snapshot.disk.read(snapshot.disk.ctx,0,out)==DE_DRIVE);
    /* Unit IDs are not recycled even if an owner/module slot is reused. */
    CHECK(!dos_load_driver("RAMDRV.SYS","/SIZE:128"));
    CHECK(snapshot.disk.read(snapshot.disk.ctx,0,out)==DE_DRIVE);
    CHECK(!dos_finish_drivers() && !module_pages);
    const unsigned sizes[]={128,2048,2072,2076,4096,32768};
    for(unsigned i=0;i<ARRAY_SIZE(sizes);i++) {
        CHECK(!dos_init(disk,arena)); char tail[32]; snprintf(tail,sizeof(tail),"/SIZE:%u",sizes[i]);
        CHECK(!dos_load_driver("RAMDRV.SYS",tail));
        CHECK(!dos_drive_info(3,&d) && d.fat_bits==(sizes[i]<=2072?12U:16U) && d.free_clusters==d.total_clusters);
        CHECK(!dos_open("D:\\SIZE.TXT",2,1,&h) && !dos_write(h,"size",4,&n) && n==4 && !dos_close(h));
        CHECK(!dos_finish_drivers() && !module_pages);
    }
    for(unsigned fail=1;fail<=3;fail++) {
        CHECK(!dos_init(disk,arena)); module_allocations=0; module_fail_allocation=fail;
        CHECK(dos_load_driver("RAMDRV.SYS","/SIZE:2048 /UNITS:2")==DE_NOMEM);
        CHECK(!module_pages && !module_live && !device_find("RAMCTL") && dos_block_info(0,&b)==DE_NOMORE);
        CHECK(dos_drive_info(3,&d)==DE_DRIVE); module_fail_allocation=0;
    }
    const char *invalid[]={"/SIZE:0","/SIZE:129","/SIZE:65536","/UNITS:0","/UNITS:9","/SIZE:32768 /UNITS:3",
        "/SIZE:2048 /SIZE:2048","/UNITS:1 /UNITS:2","/SIZE","/OTHER:1"};
    for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
        CHECK(!dos_init(disk,arena)); CHECK(dos_load_driver("RAMDRV.SYS",invalid[i]));
        CHECK(!module_pages && !module_live && !device_find("RAMCTL") && dos_block_info(0,&b)==DE_NOMORE);
    }
    CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("RAMDRV.SYS","/SIZE:128"));
    module_unload_failure=1;
    CHECK(dos_finish_drivers()==DE_IO && dos_drivers_pending() && !module_pages);
    CHECK(dos_drive_info(3,&d)==DE_DRIVE && dos_block_info(0,&b)==DE_NOMORE);
    module_unload_failure=0; CHECK(!dos_finish_drivers() && !module_live && !dos_drivers_pending());
    driver_io=NULL; free(arena);
    puts("PASS RAM disk: FAT12/16 sizes, multiple units, independent files, media/protection changes, snapshots and allocation rollback");
}
static void test_block_drivers(Disk *disk,MemoryDisk *source) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    MemoryDisk old={.bytes=source->bytes,.data=malloc(source->bytes)},replacement={.bytes=source->bytes,.data=malloc(source->bytes)};
    CHECK(old.data && replacement.data); memcpy(old.data,source->data,source->bytes); memcpy(replacement.data,source->data,source->bytes);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload}; driver_io=&io; module_mode=7;
    for(unsigned mode=0;mode<8;mode++) {
        CHECK(!dos_init(disk,arena));
        block_probe=(BlockProbe){.disk=&old,.generation=1,.flags=IO_DISK_PRESENT,.units=mode==6?26:mode==7?2:1,
            .init_error=mode==0?DE_IO:0,.bad_media=mode>=1 && mode<=4?(int)mode:0,
            .media_error=mode==7?DE_IO:0,.media_error_unit=1};
        if(mode==5) block_probe.units=0;
        CHECK(dos_load_driver("RAMDRV.SYS","") && block_probe.initialized==1 && block_probe.finished==1);
        DosBlockInfo b; DosDriveInfo d;
        CHECK(dos_block_info(0,&b)==DE_NOMORE && dos_drive_info(3,&d)==DE_DRIVE && !module_live && !module_pages);
    }
    CHECK(!dos_init(disk,arena)); module_mode=8;
    block_probe=(BlockProbe){.disk=&old,.generation=1,.flags=IO_DISK_PRESENT,.units=1};
    CHECK(dos_load_driver("RAMDRV.SYS","")==DE_FORMAT && !block_probe.initialized && !block_probe.finished && !module_live);
    module_mode=7;
    CHECK(!dos_init(disk,arena)); CHECK(!dos_load_driver("RAMDRV.SYS",""));
    DosBlockInfo b; CHECK(!dos_block_info(0,&b) && b.drive_mask==8 && b.units==1);
    unsigned h; u32 n; u8 buffer[512];
    CHECK(!dos_open("D:\\BLOCK.TXT",2,1,&h));
    CHECK(device_finish(1)==DE_BUSY && device_pending(1) && module_live==1);
    CHECK(!dos_write(h,"block",5,&n) && n==5 && !dos_close(h));
    u32 ids[DOS_DRIVES]; CHECK(block_units(1,ids)==1); IoDiskInfo snapshot; CHECK(!block_media(ids[0],&snapshot));
    block_probe.readonly=1; CHECK(block_media(ids[0],&snapshot)==DE_IO); block_probe.readonly=0;
    block_probe.generation=2; CHECK(!block_media(ids[0],&snapshot));
    block_probe.generation=1; CHECK(block_media(ids[0],&snapshot)==DE_CHANGED); block_probe.generation=2;
    CHECK(!dos_finish_drivers() && block_probe.finished==1 && !module_live);
    CHECK(snapshot.disk.read(snapshot.disk.ctx,0,buffer)==DE_DRIVE);
    for(int bad=1;bad<=2;bad++) {
        CHECK(!dos_init(disk,arena)); block_probe=(BlockProbe){.disk=&old,.generation=1,.flags=IO_DISK_PRESENT,.units=1,.bad_transfer=bad};
        CHECK(!dos_load_driver("RAMDRV.SYS","")); DosDriveInfo d;
        CHECK(dos_drive_info(3,&d)==DE_IO); block_probe.bad_transfer=0;
        CHECK(!dos_drive_info(3,&d)); CHECK(!dos_finish_drivers());
    }
    /* A provider can lose its medium during a failed write. Every recovery
     * request still carries the old generation, so the replacement is intact. */
    memcpy(old.data,source->data,source->bytes); CHECK(!dos_init(disk,arena));
    block_probe=(BlockProbe){.disk=&old,.replacement=&replacement,.generation=1,.flags=IO_DISK_PRESENT,.units=1};
    CHECK(!dos_load_driver("RAMDRV.SYS",""));
    CHECK(!dos_open("D:\\TORN.TXT",2,1,&h)); block_probe.replace_on_write=1;
    DosRegs r={.ax=0x4000,.bx=h,.cx=5,.dx=(uintptr_t)"torn!"}; CHECK(call(&r)==DE_CHANGED && !r.cx);
    CHECK(!memcmp(replacement.data,source->data,source->bytes));
    DosExtendedError error; CHECK(!dos_extended_error(&error) && error.locus==DOS_LOCUS_DISK && error.drive==3);
    CHECK(dos_close(h)==DE_CHANGED);
    CHECK(dos_open("D:\\TORN.TXT",0,0,&h)==DE_NOFILE);
    CHECK(!dos_finish_drivers() && !module_live && !module_pages);
    /* Absent removable media reserves A:, then mounts on first insertion. */
    CHECK(!dos_init(disk,arena)); block_probe=(BlockProbe){.disk=&old,.generation=1,.flags=IO_DISK_REMOVABLE,.units=1};
    CHECK(!dos_load_driver("RAMDRV.SYS","")); DosDriveInfo d;
    CHECK(!dos_block_info(0,&b) && b.drive_mask==1 && !dos_drive_info(0,&d) && !(d.flags&DOS_DRIVE_PRESENT));
    block_probe.disk=&replacement; block_probe.generation++; block_probe.flags|=IO_DISK_PRESENT;
    CHECK(!dos_drive_info(0,&d) && (d.flags&DOS_DRIVE_PRESENT));
    CHECK(!dos_finish_drivers() && !module_pages && block_probe.calls>1);
    driver_io=NULL; free(old.data); free(replacement.data); free(arena);
    puts("PASS block drivers: atomic publication, media validation, drive exhaustion, reentry, partial I/O rejection and generation-safe recovery");
}
#define FCB_CHECK CHECK
#define FCB_CALL call
#define FCB_DIAGNOSTIC(fn,want,status,error) fprintf(stderr,"FCB function %x: expected %u, status %u, error %u\n",fn,want,status,error)
#include "fcb_cases.h"
static u32 fcb_unit_count(void *context) {(void)context; return 2;}
static void test_fcb(Disk *disk,MemoryDisk *md) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    CHECK(!fcb_test_suite());
    u8 data[1024]; CHECK(!fcb_test_dta(data,17));
    DosFcb parent,child_fcb,other,copy; u32 root=dos_pid(),child,records;
    CHECK(!fcb_test_name(&parent,"C:FCBKEEP.DAT") && !fcb_test_call(0x0f,&parent,NULL,0));
    CHECK(!dos_set_files(8) && !dos_task_create(&child) && !dos_task_select(child));
    DosRegs r={.ax=0x2f00}; CHECK(!call(&r) && r.cx==128 && r.bx!=(uintptr_t)data);
    CHECK(fcb_test_call(0x14,&parent,NULL,1)==DE_HANDLE);
    CHECK(fcb_test_call(0x10,&parent,NULL,255)==DE_HANDLE);
    CHECK(!fcb_test_name(&child_fcb,"NUL") && !fcb_test_call(0x0f,&child_fcb,NULL,0));
    CHECK(!fcb_test_name(&other,"NUL") && !fcb_test_call(0x0f,&other,NULL,0));
    CHECK(!fcb_test_name(&copy,"NUL") && fcb_test_call(0x0f,&copy,NULL,255)==DE_HANDLES);
    CHECK(!dos_task_select(root)); r=(DosRegs){.ax=0x2f00};
    CHECK(!call(&r) && r.cx==17 && r.bx==(uintptr_t)data);
    CHECK(!dos_task_destroy(child));
    CHECK(fcb_test_call(0x14,&child_fcb,NULL,1)==DE_HANDLE);
    CHECK(!fcb_test_call(0x0f,&copy,NULL,0) && !fcb_test_call(0x10,&copy,NULL,0));
    CHECK(!fcb_test_call(0x10,&parent,NULL,0)); CHECK(!dos_set_files(64));
    CHECK(!fcb_test_name(&parent,"CON") && !fcb_test_call(0x0f,&parent,NULL,0));
    CHECK(!fcb_test_dta(data,sizeof(data))); wr16(parent.bytes+14,8); key_string("AB\r");
    CHECK(!fcb_test_call(0x14,&parent,NULL,3) && !memcmp(data,"AB\r\n\0\0\0\0",8));
    CHECK(!fcb_test_call(0x10,&parent,NULL,0)); key_string("");

    /* A failed second sector preserves the first committed sector and reports
     * record counts independently of byte counts. Close edits are atomic. */
    CHECK(!fcb_test_name(&parent,"C:FCBERR.BIN") && !fcb_test_call(0x16,&parent,NULL,0));
    wr16(parent.bytes+14,1024); memset(data,0x3a,sizeof(data)); records=1;
    md->fail_flush_at=md->flushes+2;
    CHECK(fcb_test_call(0x28,&parent,&records,1)==DE_IO && !records && rd32(parent.bytes+16)==512 && rd32(parent.bytes+33)==1);
    md->fail_flush_at=0; unsigned h; u32 n,pos;
    CHECK(!dos_open("C:FCBERR.BIN",0,0,&h) && !dos_read(h,data,sizeof(data),&n) && n==512);
    for(unsigned i=0;i<n;i++) CHECK(data[i]==0x3a);
    CHECK(!dos_close(h));
    u8 *before=malloc(md->bytes); CHECK(before!=NULL); memcpy(before,md->data,md->bytes);
    wr32(parent.bytes+16,1500); wr16(parent.bytes+20,0x285d); wr16(parent.bytes+22,0x645c);
    md->fail_write_at=md->writes+1; md->failed_write_bytes=256;
    CHECK(fcb_test_call(0x10,&parent,NULL,255)==DE_IO); md->fail_write_at=0; md->failed_write_bytes=0;
    CHECK(!memcmp(before,md->data,md->bytes) && !dos_volume.faulted && !fat_pages);
    CHECK(!dos_open("C:FCBERR.BIN",0,0,&h) && !dos_seek(h,0,2,&pos) && pos==512 && !dos_close(h));
    CHECK(!dos_remove("C:FCBERR.BIN",0));
    CriticalProbe critical={.action=DOS_CRITICAL_FAIL};
    DosCriticalHandler handler={critical_probe,&critical}; CHECK(!dos_critical_handler(&handler,NULL));
    CHECK(!fcb_test_name(&parent,"C:FCBERR.BIN") && !fcb_test_call(0x16,&parent,NULL,0));
    md->fail_write_at=md->writes+1;
    CHECK(fcb_test_call(0x15,&parent,NULL,1)==DE_CRITICAL && critical.calls==1 && !rd32(parent.bytes+16));
    md->fail_write_at=0; DosExtendedError detail;
    CHECK(!dos_extended_error(&detail) && detail.error==DE_IO && detail.drive==2 && (detail.flags&DOS_ERROR_COMMIT));
    critical.action=DOS_CRITICAL_RETRY; critical.calls=0; md->fail_write_at=md->writes+1;
    CHECK(!fcb_test_call(0x15,&parent,NULL,0) && critical.calls==1 && rd32(parent.bytes+16)==128);
    md->fail_write_at=0; handler=(DosCriticalHandler){0}; CHECK(!dos_critical_handler(&handler,NULL));
    CHECK(!fcb_test_call(0x10,&parent,NULL,0) && !dos_remove("C:FCBERR.BIN",0));
    DosExtendedFcb label={.marker=255,.attr=FA_VOLUME}; label.fcb.bytes[0]=3;
    memset(label.fcb.bytes+1,'?',11); CHECK(!fcb_test_call(0x11,&label,NULL,0));
    u8 original_label[11]; memcpy(original_label,data+8,11); CHECK(!fcb_test_call(0x13,&label,NULL,0));
    memcpy(label.fcb.bytes+1,"FCB FAILURE",11); memcpy(before,md->data,md->bytes);
    md->fail_write_at=md->writes+2; md->failed_write_bytes=256;
    CHECK(fcb_test_call(0x16,&label,NULL,255)==DE_IO); md->fail_write_at=0; md->failed_write_bytes=0;
    CHECK(!memcmp(before,md->data,md->bytes) && !dos_volume.faulted && !fat_pages);
    CHECK(!fcb_test_call(0x16,&label,NULL,0)); memcpy(before,md->data,md->bytes);
    memcpy(label.fcb.bytes+17,"FCB RENAMED",11); md->fail_flush_at=md->flushes+1;
    CHECK(fcb_test_call(0x17,&label,NULL,255)==DE_IO); md->fail_flush_at=0;
    CHECK(!memcmp(before,md->data,md->bytes));
    CHECK(!fcb_test_call(0x13,&label,NULL,0)); free(before);
    memcpy(label.fcb.bytes+1,original_label,11); CHECK(!fcb_test_call(0x16,&label,NULL,0));

    /* FCB device references use the same callback ABI and cleanup discipline. */
    IoServices driver={.version=IO_ABI_VERSION,.size=sizeof(driver),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload};
    driver_io=&driver; module_mode=0; memset(device_probes,0,sizeof(device_probes));
    CHECK(!dos_load_driver("LOOPDRV.SYS",""));
    CHECK(!fcb_test_name(&parent,"QDEV") && !fcb_test_call(0x0f,&parent,NULL,0));
    DeviceProbe *probe=&device_probes[0]; CHECK(probe->live==1);
    wr16(parent.bytes+14,4); records=2; memset(data,0xcc,sizeof(data)); probe->read_error=DE_IO;
    CHECK(fcb_test_call(0x27,&parent,&records,3)==DE_IO && records==1 && !memcmp(data,"AB\0\0",4) && data[4]==0xcc);
    probe->read_error=0; probe->flush_error=DE_NOTREADY; probe->close_error=DE_ACCESS;
    CHECK(fcb_test_call(0x10,&parent,NULL,255)==DE_NOTREADY && !probe->live);
    DosExtendedError error; CHECK(!dos_extended_error(&error) && error.error==DE_NOTREADY && error.locus==DOS_LOCUS_DEVICE);
    probe->flush_error=probe->close_error=0;
    CHECK(!dos_task_create(&child) && !dos_task_select(child));
    CHECK(!fcb_test_name(&child_fcb,"QDEV") && !fcb_test_call(0x0f,&child_fcb,NULL,0));
    CHECK(!dos_task_select(root) && !dos_task_destroy(child));
    CHECK(!probe->live && probe->closed_pid==child && !dos_finish_drivers()); driver_io=NULL;

    /* Old open/search tokens and rollback must never reach replacement media. */
    MemoryDisk old={.bytes=md->bytes},replacement={.bytes=md->bytes};
    old.data=malloc(md->bytes); replacement.data=malloc(md->bytes); CHECK(old.data && replacement.data);
    memcpy(old.data,md->data,md->bytes); memcpy(replacement.data,md->data,md->bytes);
    CHECK(!dos_init(disk,arena));
    test_units[0]=(TestUnit){.disk=md,.generation=1,.flags=IO_DISK_BOOT|IO_DISK_PRESENT};
    test_units[1]=(TestUnit){.disk=&old,.generation=1,.flags=IO_DISK_PRESENT};
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.capabilities=IO_CAP_DISKS,.disk_count=fcb_unit_count,.disk_info=unit_info};
    CHECK(!dos_attach_disks(&io) && !fcb_test_dta(data,sizeof(data)));
    CHECK(!fcb_test_name(&parent,"D:FCBMEDIA.DAT") && !fcb_test_call(0x16,&parent,NULL,0));
    CHECK(!fcb_test_name(&copy,"D:FCBMEDIA.*") && !fcb_test_call(0x11,&copy,NULL,0));
    CHECK(!fcb_test_name(&other,"D:TORN.DAT") && !fcb_test_call(0x16,&other,NULL,0) && !fcb_test_call(0x10,&other,NULL,0));
    memcpy(other.bytes+17,"RENAMED DAT",11);
    test_units[1].replacement=&replacement; test_units[1].replace_on_write=1;
    CHECK(fcb_test_call(0x17,&other,NULL,255)==DE_CHANGED);
    CHECK(!memcmp(replacement.data,md->data,md->bytes));
    CHECK(fcb_test_call(0x14,&parent,NULL,1)==DE_CHANGED);
    CHECK(fcb_test_call(0x12,&copy,NULL,255)==DE_CHANGED);
    CHECK(fcb_test_call(0x10,&parent,NULL,255)==DE_CHANGED);
    CHECK(!dos_finish_drivers());
    free(old.data); free(replacement.data); free(arena);
    puts("PASS FCB: records, DTA, sharing, locks, metadata, search, labels, tasks, devices, rollback and media replacement");
}
static void test_clock(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    IoDateTime saved=host_time; DosDateTime t;
    CHECK(!dos_get_datetime(&t) && t.year==2026 && t.month==9 && t.day==29 && t.weekday==2);
    CHECK(dos_get_datetime(NULL)==DE_FUNCTION); clock_reentry=1;
    DosRegs r={.ax=0x2d00,.cx=(12<<8)|34,.dx=(57<<8)|89};
    CHECK(!call(&r) && !r.ax && host_time.nanosecond==890000000);
    CHECK(host_time.year==2026 && host_time.month==9 && host_time.day==29);
    /* Every year in DOS's supported range, including the leap century 2000. */
    for(unsigned year=1980;year<2100;year++) {
        unsigned writes=clock_writes;
        r=(DosRegs){.ax=0x2b00,.cx=year,.dx=0x21d}; CHECK(!call(&r));
        if(year%4) CHECK(r.ax==255 && clock_writes==writes);
        else CHECK(!r.ax && clock_writes==writes+1 && host_time.year==year && host_time.day==29);
        CHECK(host_time.hour==12 && host_time.minute==34 && host_time.second==57 && host_time.nanosecond==890000000);
    }
    const DosRegs invalid[]={
        {.ax=0x2b00,.cx=1979,.dx=0x101},{.ax=0x2b00,.cx=2100,.dx=0x101},
        {.ax=0x2b00,.cx=2000,.dx=0x1f},{.ax=0x2b00,.cx=2000,.dx=0xd01},
        {.ax=0x2b00,.cx=2000,.dx=0x100},{.ax=0x2b00,.cx=2000,.dx=0x41f},
        {.ax=0x2b00,.cx=(1ULL<<32)+2000,.dx=0x101},{.ax=0x2b00,.cx=2000,.dx=(1ULL<<32)+0x101},
        {.ax=0x2d00,.cx=24<<8},{.ax=0x2d00,.cx=60},{.ax=0x2d00,.dx=60<<8},
        {.ax=0x2d00,.dx=100},{.ax=0x2d00,.cx=1ULL<<32},{.ax=0x2d00,.dx=1ULL<<32}};
    IoDateTime before=host_time; unsigned reads=clock_reads,writes=clock_writes;
    for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
        r=invalid[i]; CHECK(!call(&r) && r.ax==255);
        CHECK(clock_reads==reads && clock_writes==writes && !memcmp(&host_time,&before,sizeof(before)));
    }
    r=(DosRegs){.ax=0x2b00,.cx=2000,.dx=0x21d}; CHECK(!call(&r) && !r.ax);
    r=(DosRegs){.ax=0x2a00}; CHECK(!call(&r) && r.cx==2000 && r.dx==0x21d && r.ax==2);
    r=(DosRegs){.ax=0x2c00}; CHECK(!call(&r) && r.cx==((12<<8)|34) && r.dx==((57<<8)|89) && !r.ax);
    CHECK(!dos_get_datetime(&t) && t.weekday==2 && t.hundredth==89);
    clock_reentry=0;
    unsigned h; u16 date,time; u32 n;
    CHECK(!dos_open("CLOCK.TMP",2,2,&h)); CHECK(!dos_write(h,"clock",5,&n) && n==5);
    CHECK(!dos_file_time(h,0,&date,&time) && date==((20<<9)|(2<<5)|29) && time==((12<<11)|(34<<5)|28));
    CHECK(!dos_close(h) && !dos_open("CLOCK.TMP",0,0,&h));
    CHECK(!dos_file_time(h,0,&date,&time) && date==((20<<9)|(2<<5)|29) && time==((12<<11)|(34<<5)|28));
    CHECK(!dos_close(h) && !dos_remove("CLOCK.TMP",0));
    u32 child,parent=dos_pid(); CHECK(!dos_task_create(&child) && !dos_task_select(child));
    r=(DosRegs){.ax=0x2d00,.cx=0,.dx=0}; CHECK(!call(&r) && !r.ax);
    CHECK(!dos_task_select(parent) && !dos_get_datetime(&t) && !t.hour && !t.minute && !t.second && !t.hundredth);
    CHECK(!dos_task_destroy(child));
    before=host_time; clock_write_error=DE_IO;
    r=(DosRegs){.ax=0x2b00,.cx=1980,.dx=0x101}; CHECK(call(&r)==DE_IO && !memcmp(&before,&host_time,sizeof(before)));
    DosExtendedError error; CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.locus==DOS_LOCUS_DEVICE);
    clock_write_error=0; clock_read_error=DE_IO;
    memset(&t,0xa5,sizeof(t)); DosDateTime untouched=t;
    CHECK(dos_get_datetime(&t)==DE_IO && !memcmp(&t,&untouched,sizeof(t)));
    r=(DosRegs){.ax=0x2c00,.cx=123,.dx=456}; CHECK(call(&r)==DE_IO && r.cx==123 && r.dx==456);
    platform_fat_time(&date,&time); CHECK(date==0x21 && !time); clock_read_error=0;
    /* Invalid clock data must never become a nonsensical FAT timestamp. */
    host_time=(IoDateTime){2100,2,29,0,0,0,0}; CHECK(dos_get_datetime(&t)==DE_IO);
    host_time.day=28; CHECK(!dos_get_datetime(&t) && t.weekday==0);
    host_time.nanosecond=1000000000; CHECK(dos_get_datetime(&t)==DE_IO);
    platform_fat_time(&date,&time); CHECK(date==0x21 && !time);
    host_time=(IoDateTime){2108,1,1,0,0,0,0}; platform_fat_time(&date,&time); CHECK(date==0x21 && !time);
    host_time=before; host_time.nanosecond=780000000;
    IoServices legacy=clock_io; legacy.size=offsetof(IoServices,clock_get); driver_io=&legacy;
    CHECK(!dos_get_datetime(&t) && t.year==2000 && t.weekday==2 && !t.hundredth);
    writes=clock_writes; r=(DosRegs){.ax=0x2d00}; CHECK(call(&r)==DE_FUNCTION && clock_writes==writes);
    legacy.size=offsetof(IoServices,datetime); CHECK(dos_get_datetime(&t)==DE_FUNCTION);
    driver_io=NULL; host_time=saved; free(arena);
    puts("PASS clock: DOS date/time limits, leap years, hundredths, global task state, FAT stamps, failure and legacy ABI");
}
static int cleanup_exec(void) {
    unsigned h; CHECK(!dos_open("QDEV",2,0,&h)); return DE_IO;
}
static void test_cleanup_errors(Disk *disk,MemoryDisk *source) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL);
    MemoryDisk other={.bytes=source->bytes,.data=malloc(source->bytes)}; CHECK(other.data!=NULL);
    memcpy(other.data,source->data,source->bytes);
    IoServices io={.version=IO_ABI_VERSION,.size=sizeof(io),.alloc_pages=module_alloc,.free_pages=module_free,
        .module_load=module_load,.module_unload=module_unload}; driver_io=&io; module_mode=9;
    CHECK(!dos_init(disk,arena)); memset(device_probes,0,sizeof(device_probes));
    DeviceProbe *device=&device_probes[0];
    block_probe=(BlockProbe){.disk=&other,.generation=1,.flags=IO_DISK_PRESENT,.units=1,.init_error=DE_IO};
    /* A mixed module's character FINISH must not relabel a block INIT error. */
    CHECK(dos_load_driver("RAMDRV.SYS","")==DE_IO);
    DosExtendedError error; CHECK(!dos_extended_error(&error));
    CHECK(error.error==DE_IO && error.locus==DOS_LOCUS_DISK && error.drive==UINT32_MAX);
    CHECK(block_probe.finished==1 && device->calls[DOS_DEV_FINISH]==1 && !module_live && !module_pages);
    /* An unload failure retains the image for retry without replacing the
     * initialization error with a secondary cleanup error. */
    device->init_error=DE_NOTREADY; device->finish_error=DE_IO; module_unload_failure=1;
    CHECK(dos_load_driver("RAMDRV.SYS","")==DE_NOTREADY && module_live==1 && dos_drivers_pending());
    CHECK(!dos_extended_error(&error) && error.error==DE_NOTREADY && error.locus==DOS_LOCUS_DEVICE);
    unsigned finished=device->calls[DOS_DEV_FINISH]; module_unload_failure=0;
    CHECK(!dos_finish_drivers() && !module_live && device->calls[DOS_DEV_FINISH]==finished);
    CHECK(!dos_init(disk,arena)); memset(device_probes,0,sizeof(device_probes));
    block_probe=(BlockProbe){.disk=&other,.generation=1,.flags=IO_DISK_PRESENT,.units=1};
    CHECK(!dos_load_driver("RAMDRV.SYS","")); DosDriveInfo drive; CHECK(!dos_drive_info(3,&drive));
    CriticalProbe probe={.action=DOS_CRITICAL_FAIL,.disk=source};
    DosCriticalHandler handler={critical_probe,&probe}; CHECK(!dos_critical_handler(&handler,NULL));
    u32 parent=dos_pid(),child; unsigned h;
    CHECK(!dos_task_create(&child) && !dos_task_select(child)); CHECK(!dos_open("QDEV",2,0,&h));
    CHECK(!dos_task_select(parent)); dos_volume.faulted=1; device->close_error=DE_NOTREADY;
    CHECK(dos_task_destroy(child)==DE_IO && !device->live && dos_task_select(child)==DE_BLOCK);
    CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.locus==DOS_LOCUS_DISK && error.drive==2);
    CHECK(error.generation==1 && error.operation==UINT32_MAX && !probe.calls);
    device->close_error=0;
    /* A later physical error with the same numeric code must neither replace
     * the first cause nor invoke another critical handler during cleanup. */
    other.fail_flush=1; unsigned flushes=other.flushes;
    DosRegs r={.ax=0x0d00}; CHECK(call(&r)==DE_IO && !probe.calls && other.flushes==flushes+1);
    CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.drive==2 && error.locus==DOS_LOCUS_DISK);
    CHECK(error.operation==UINT32_MAX && error.sector==UINT64_MAX);
    dos_volume.faulted=0; source->fail_flush=1;
    r=(DosRegs){.ax=0x0d00}; CHECK(call(&r)==DE_CRITICAL && probe.calls==1);
    CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.drive==2 && error.operation==DOS_CRITICAL_FLUSH);
    source->fail_flush=other.fail_flush=0;
    /* EXEC cleanup releases a child-owned device and its image allocation
     * without changing the cause returned by the image loader. */
    u32 largest,before; CHECK(!arena_check(&dos_arena,&before)); exec_probe=cleanup_exec;
    r=(DosRegs){.ax=0x4b00,.dx=(uintptr_t)"HELLO.EFI",.bx=(uintptr_t)""}; CHECK(call(&r)==DE_IO);
    exec_probe=NULL; CHECK(!device->live && !arena_check(&dos_arena,&largest) && largest==before);
    CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.locus==DOS_LOCUS_DISK && error.drive==2);
    CHECK(!dos_task_create(&child) && !dos_task_select(child)); CHECK(!dos_open("QDEV",2,0,&h));
    CHECK(!dos_task_select(parent)); device->flush_error=DE_NOTREADY; device->close_error=DE_IO;
    CHECK(dos_task_destroy(child)==DE_NOTREADY && !device->live);
    CHECK(!dos_extended_error(&error) && error.error==DE_NOTREADY && error.locus==DOS_LOCUS_DEVICE);
    device->flush_error=device->close_error=0; device->finish_error=DE_NOTREADY;
    block_probe.finish_error=DE_IO;
    CHECK(dos_finish_drivers()==DE_IO && !module_live && !module_pages);
    CHECK(!dos_extended_error(&error) && error.error==DE_IO && error.locus==DOS_LOCUS_DISK && error.drive==UINT32_MAX);
    CHECK(!dos_init(disk,arena)); block_probe.finish_error=0;
    CHECK(!dos_load_driver("RAMDRV.SYS",""));
    CHECK(dos_finish_drivers()==DE_NOTREADY && !module_live && !module_pages);
    CHECK(!dos_extended_error(&error) && error.error==DE_NOTREADY && error.locus==DOS_LOCUS_DEVICE);
    CHECK(dos_exec("HELLO.EFI","")==DE_FORMAT);
    CHECK(!dos_extended_error(&error) && error.error==DE_FORMAT && error.locus==DOS_LOCUS_UNKNOWN);
    CHECK(!dos_finish_drivers()); driver_io=NULL; free(other.data); free(arena);
    puts("PASS cleanup errors: first cause, disk/device origin, critical precedence, task release, EXEC and driver unload");
}
#define NLS_CHECK CHECK
#define NLS_CALL call
#include "nls_cases.h"
#include "maint_cases.h"
static void test_nls(Disk *disk,MemoryDisk *md) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    CHECK(!nls_test_suite());
    DosConfig config; config_defaults(&config); CHECK(!config.country);
    CHECK(!parse_config(&config,"COUNTRY=81,932") && config.country==81 && config.code_page==932);
    CHECK(!strcmp(config.country_file,"C:\\COUNTRY.SYS"));
    DosConfig saved=config;
    const char *bad[]={"COUNTRY=","COUNTRY=0","COUNTRY=65535","COUNTRY=99999999999999999999",
        "COUNTRY=81,65535","COUNTRY=81,932junk","COUNTRY=81 932","COUNTRY=81,932,TOO-LONG-NAME.SYS"};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        CHECK(parse_config(&config,bad[i])!=0 && !memcmp(&config,&saved,sizeof(config)));
    }
    CHECK(!parse_config(&config,"country 351 , , \"C:\\COUNTRY.SYS\"") && config.country==351 && !config.code_page);
    CHECK(!dos_config_country(config.country,config.code_page,config.country_file));
    DosCountryInfo info,before; CHECK(!nls_test_info(65535,65535,&before) && before.country==351 && before.code_page==850);
    DosRegs r={.ax=0x6601}; CHECK(!call(&r) && r.bx==850 && r.dx==850);
    unsigned h; u32 written;
    CHECK(!dos_open("BADNLS.SYS",2,1,&h) && !dos_write(h,"bad country",11,&written) && !dos_close(h));
    CHECK(dos_config_country(1,437,"BADNLS.SYS")==DE_DATA);
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    CHECK(dos_config_country(1,999,"COUNTRY.SYS")==DE_NOFILE);
    CHECK(dos_config_country(1,437,"ABSENT.SYS")==DE_NOFILE);
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    md->fail_read=1;
    CHECK(dos_config_country(1,437,"COUNTRY.SYS")==DE_IO); md->fail_read=0;
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    md->fail_flush=1;
    CHECK(dos_config_country(1,437,"COUNTRY.SYS")==DE_IO); md->fail_flush=0;
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    CHECK(!dos_open("COUNTRY.SYS",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h));
    CHECK(dos_config_country(1,437,"COUNTRY.SYS")==DE_SHARE);
    CHECK(!dos_close(h));
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    /* A valid external database must supply its own data, not merely select
     * an identically numbered built-in profile. Copies outlive later loads. */
    u8 *custom=malloc(65537); CHECK(custom!=NULL);
    CHECK(!dos_open("COUNTRY.SYS",0,0,&h));
    u32 length; CHECK(!dos_read(h,custom,65536,&length) && length>100 && length<65536 && !dos_close(h));
    NlsDatabase db; NlsCountry country;
    CHECK(!nls_database_open(&db,custom,length) && !nls_database_country(&db,1,437,&country));
    custom[country.info.data-custom+6]='Q'; custom[country.upper.data-custom+1]='X';
    CHECK(!dos_open("CUSTOM.SYS",2,1,&h));
    CHECK(!dos_write(h,custom,length,&written) && written==length && !dos_close(h));
    CHECK(!dos_config_country(1,437,"CUSTOM.SYS"));
    DosCountryInfo copied;
    CHECK(!nls_test_info(65535,65535,&copied) && copied.currency[0]=='Q');
    r=(DosRegs){.ax=0x6520,.dx=0x81}; CHECK(!call(&r) && r.dx=='X');
    CHECK(!dos_config_country(351,850,"COUNTRY.SYS") && copied.currency[0]=='Q');
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    /* A structurally valid database with an oversized tail is rejected
     * before publication, and the source file can immediately be removed. */
    memset(custom+length,0,65537-length);
    CHECK(!dos_open("CUSTOM.SYS",2,1,&h));
    CHECK(!dos_write(h,custom,65537,&written) && written==65537 && !dos_close(h));
    CHECK(dos_config_country(1,437,"CUSTOM.SYS")==DE_DATA);
    CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&before,sizeof(info)));
    CHECK(!dos_remove("CUSTOM.SYS",0)); free(custom);
    /* Keep a valid file handle across switches: its identity is not re-folded. */
    CHECK(!dos_open("NLSKEEP.DAT",0,0,&h));
    CHECK(!dos_config_country(81,932,"COUNTRY.SYS"));
    r=(DosRegs){.ax=0x6300}; CHECK(!call(&r) && r.cx==6);
    const u8 *lead=(const u8 *)(uintptr_t)r.si;
    CHECK(lead[0]==0x81 && lead[1]==0x9f);
    u8 data[16]; CHECK(!dos_read(h,data,sizeof(data),&written) && written==8 && !memcmp(data,"NLS \x81\\\xe5""a",8));
    CHECK(!dos_close(h));
    u32 parent=dos_pid(),child; CHECK(!dos_task_create(&child) && !dos_task_select(child));
    CHECK(!nls_test_info(65535,65535,&info) && info.country==81 && info.code_page==932);
    CHECK(!nls_test_page(437) && !dos_task_select(parent));
    r=(DosRegs){.ax=0x6300}; CHECK(!call(&r) && r.si==(uintptr_t)lead && r.cx==2 && !lead[0] && !lead[1]);
    CHECK(!nls_test_info(65535,65535,&info) && info.country==81 && info.code_page==437);
    CHECK(!dos_task_destroy(child));
    r=(DosRegs){.ax=0x6601}; CHECK(!call(&r) && r.bx==437 && r.dx==932);
    CHECK(!dos_config_country(1,437,"COUNTRY.SYS") && !dos_remove("BADNLS.SYS",0));
    free(arena);
    puts("PASS NLS: country/code-page APIs, table bounds, DBCS files/FCBs, task-global state and atomic database loading");
}
static void test_temp_verify(Disk *disk,MemoryDisk *md) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    char a[DOS_PATH_MAX]="",b[DOS_PATH_MAX]="C:",c[DOS_PATH_MAX]="TMPWORK",d[DOS_PATH_MAX]="TMPWORK\\";
    unsigned ha,hb,hc,hd; u32 n;
    CHECK(!dos_mkdir("TMPWORK"));
    DosRegs r={.ax=0x5a00,.dx=(uintptr_t)a}; CHECK(!call(&r)); ha=r.ax;
    CHECK(strlen(a)==8); for(unsigned i=0;i<8;i++) CHECK((a[i]>='0' && a[i]<='9') || (a[i]>='A' && a[i]<='F'));
    r=(DosRegs){.ax=0x5a00,.cx=FA_HIDDEN,.dx=(uintptr_t)b}; CHECK(!call(&r)); hb=r.ax;
    CHECK(strlen(b)==11 && !memcmp(b,"C:\\",3) && strcmp(a,b+3));
    r=(DosRegs){.ax=0x5a00,.dx=(uintptr_t)c}; CHECK(!call(&r)); hc=r.ax;
    CHECK(strlen(c)==16 && !memcmp(c,"TMPWORK\\",8));
    r=(DosRegs){.ax=0x5a00,.dx=(uintptr_t)d}; CHECK(!call(&r)); hd=r.ax;
    CHECK(strlen(d)==16 && strcmp(c,d));
    CHECK(!dos_write(hc,"pipe",4,&n) && n==4); u32 pos; CHECK(!dos_seek(hc,0,0,&pos));
    char text[8]; CHECK(!dos_read(hc,text,sizeof(text),&n) && n==4 && !memcmp(text,"pipe",4));
    u8 attr; CHECK(!dos_attribute(b,0,&attr) && attr==(FA_HIDDEN|FA_ARCHIVE));
    CHECK(!dos_close(ha) && !dos_close(hb) && !dos_close(hc) && !dos_close(hd));
    CHECK(!dos_remove(a,0) && !dos_remove(b,0) && !dos_remove(c,0) && !dos_remove(d,0));
    char bad[DOS_PATH_MAX]="TMPWORK"; r=(DosRegs){.ax=0x5a00,.cx=FA_DIR,.dx=(uintptr_t)bad};
    CHECK(call(&r)==DE_ACCESS && !strcmp(bad,"TMPWORK"));
    strcopy(bad,sizeof(bad),"ABSENT\\"); r=(DosRegs){.ax=0x5a00,.dx=(uintptr_t)bad};
    CHECK(call(&r)==DE_PATH && !strcmp(bad,"ABSENT\\"));
    char full[DOS_PATH_MAX]; memset(full,'A',DOS_PATH_MAX-5); full[DOS_PATH_MAX-5]=0;
    r=(DosRegs){.ax=0x5a00,.dx=(uintptr_t)full}; CHECK(call(&r)==DE_PATH);
    /* A DBCS trail byte 5Ch is part of the name, not its separator. */
    CHECK(!dos_config_country(81,932,"COUNTRY.SYS"));
    CHECK(!dos_mkdir("TMPWORK\\\x83\x5c"));
    char kanji[DOS_PATH_MAX]="TMPWORK\\\x83\x5c"; r=(DosRegs){.ax=0x5a00,.dx=(uintptr_t)kanji};
    CHECK(!call(&r)); CHECK(strlen(kanji)==19 && kanji[10]=='\\' && !dos_close(r.ax) && !dos_remove(kanji,0));
    CHECK(!dos_remove("TMPWORK\\\x83\x5c",1));
    CHECK(!dos_config_country(1,437,"COUNTRY.SYS"));
    /* VERIFY is global and reads back each committed sector. */
    r=(DosRegs){.ax=0x5400}; CHECK(!call(&r) && r.ax==0);
    r=(DosRegs){.ax=0x2e03}; CHECK(!call(&r));
    r=(DosRegs){.ax=0x5400}; CHECK(!call(&r) && r.ax==1);
    u32 parent=dos_pid(),child; CHECK(!dos_task_create(&child) && !dos_task_select(child));
    r=(DosRegs){.ax=0x5400}; CHECK(!call(&r) && r.ax==1); CHECK(!dos_task_select(parent) && !dos_task_destroy(child));
    unsigned h; u8 block[512]; memset(block,'v',sizeof(block));
    CHECK(!dos_open("VERIFY.DAT",2,1,&h) && !dos_write(h,block,sizeof(block),&n) && n==512);
    CHECK(!dos_write(h,block,sizeof(block),&n) && n==512);
    md->corrupt_write_at=md->writes+1; CHECK(dos_write(h,block,sizeof(block),&n)==DE_IO && !n);
    md->corrupt_write_at=0; CHECK(!dos_seek(h,0,2,&pos) && pos==1024);
    CHECK(!dos_close(h));
    r=(DosRegs){.ax=0x2e00}; CHECK(!call(&r)); r=(DosRegs){.ax=0x5400}; CHECK(!call(&r) && r.ax==0);
    CHECK(!dos_open("VERIFY.DAT",2,0,&h)); CHECK(!dos_seek(h,0,2,&pos));
    md->corrupt_write_at=md->writes+1;
    CHECK(!dos_write(h,block,sizeof(block),&n) && n==512); md->corrupt_write_at=0;
    u8 back[512]; CHECK(!dos_seek(h,1024,0,&pos) && !dos_read(h,back,sizeof(back),&n) && n==512 && memcmp(back,block,512));
    CHECK(!dos_close(h) && !dos_remove("VERIFY.DAT",0));
    CHECK(!dos_remove("TMPWORK",1)); free(arena);
}

/* A read-only in-memory tree served through the redirector interface. */
typedef struct {const char *path,*data; u8 attr; u16 dir;} MemEntry;
static const MemEntry mem_tree[]={
    {"\\",NULL,FA_DIR,1},{"\\README.TXT","WINDOWS SETUP CD\r\n",FA_RDONLY,0},
    {"\\WIN",NULL,FA_DIR|FA_RDONLY,2},{"\\WIN\\SETUP.EXE","setup program image",FA_RDONLY,0},
    {"\\WIN\\A.TXT","alpha",FA_RDONLY,0},{"\\WIN\\HIDDEN.SYS","x",FA_RDONLY|FA_HIDDEN,0},{"\\EMPTY",NULL,FA_DIR,3},
};
static u32 mem_generation=1; static int mem_open;
static int mem_find(const char *path) {
    for(unsigned i=0;i<ARRAY_SIZE(mem_tree);i++) if(!strcmp(mem_tree[i].path,path)) return (int)i;
    return -1;
}
static const char *mem_leaf(const char *path) {const char *leaf=path; for(;*path;path++) if(*path=='\\') leaf=path+1; return leaf;}
static void mem_fill(int i,DosRedirEntry *e) {
    const MemEntry *m=&mem_tree[i]; const char *leaf=mem_leaf(m->path);
    memset(e,0,sizeof(*e)); e->attributes=m->attr; e->directory=m->dir; e->size=m->data?(u32)strlen(m->data):0;
    e->date=((2026-1980)<<9)|(10<<5)|1; e->time=(12<<11)|(34<<5)|28; strcopy(e->name,sizeof(e->name),leaf);
}
static int mem_stat(void *c,const char *path,DosRedirEntry *e) {(void)c; int i=mem_find(path); if(i<0) return DE_NOFILE; mem_fill(i,e); return 0;}
static int mem_open_file(void *c,const char *path,u64 *file,DosRedirEntry *e) {
    int r=mem_stat(c,path,e); if(r) return r;
    *file=(u64)mem_find(path)+100; mem_open++; return 0;
}
static int mem_read(void *c,u64 file,u32 offset,void *buffer,u32 count,u32 *done) {
    (void)c; const char *d=mem_tree[file-100].data; u32 size=(u32)strlen(d);
    *done=offset<size?MIN(count,size-offset):0; memcpy(buffer,d+offset,*done); return 0;
}
static int mem_close(void *c,u64 file) {(void)c; (void)file; CHECK(mem_open>0); mem_open--; return 0;}
static int mem_next(void *c,u16 dir,u32 *index,DosRedirEntry *e) {
    (void)c;
    if(dir==1 && *index==0) {memset(e,0,sizeof(*e)); e->attributes=FA_VOLUME; strcopy(e->name,sizeof(e->name),"WIN30"); *index=1; return 0;}
    for(u32 i=*index?*index:1;i<ARRAY_SIZE(mem_tree);i++) {
        char parent[64]; strcopy(parent,sizeof(parent),mem_tree[i].path);
        char *last=parent+(mem_leaf(parent)-parent)-1; if(last==parent) last++; *last=0;
        int p=mem_find(parent); if(p<0 || mem_tree[p].dir!=dir || i==0) continue;
        mem_fill((int)i,e); *index=i+1; return 0;
    }
    *index=ARRAY_SIZE(mem_tree); return DE_NOMORE;
}
static int mem_space(void *c,u32 *clusters,u32 *free,u32 *bytes) {(void)c; *clusters=100; *free=0; *bytes=2048; return 0;}
static u32 mem_gen(void *c) {(void)c; return mem_generation;}
static const DosRedirector mem_redirector={sizeof(DosRedirector),0,NULL,mem_stat,mem_open_file,mem_read,mem_close,mem_next,mem_space,mem_gen};
static void test_redirector(Disk *disk) {
    void *arena=aligned_alloc(16,DOS_MEMORY_BYTES); CHECK(arena!=NULL); CHECK(!dos_init(disk,arena));
    u32 drive=UINT32_MAX; DosRedirector bad=mem_redirector; bad.next=NULL;
    CHECK(dos_redirect(&drive,&bad)==DE_FUNCTION);
    CHECK(!dos_redirect(&drive,&mem_redirector) && drive==3);
    u32 other=3; CHECK(dos_redirect(&other,&mem_redirector)==DE_ACCESS);
    other=2; CHECK(dos_redirect(&other,&mem_redirector)==DE_ACCESS);
    /* Reading, seeking, times; the drive is read-only. */
    unsigned h; u32 n,pos; char buf[64]; u16 date,time;
    CHECK(!dos_open("d:readme.txt",0,0,&h)); CHECK(!dos_read(h,buf,7,&n) && n==7 && !memcmp(buf,"WINDOWS",7));
    CHECK(!dos_seek(h,-2,2,&pos) && pos==16); CHECK(!dos_read(h,buf,10,&n) && n==2 && !memcmp(buf,"\r\n",2));
    CHECK(!dos_read(h,buf,10,&n) && !n);
    CHECK(!dos_file_time(h,0,&date,&time) && date==(((2026-1980)<<9)|(10<<5)|1));
    CHECK(dos_file_time(h,1,&date,&time)==DE_ACCESS && dos_write(h,"x",1,&n)==DE_ACCESS);
    DosRegs r={.ax=0x440a,.bx=h}; CHECK(!call(&r) && r.dx==(0x8000|3));
    CHECK(!dos_close(h) && !mem_open);
    CHECK(dos_open("D:\\README.TXT",2,0,&h)==DE_ACCESS && !mem_open);
    unsigned result; CHECK(dos_open_ex("D:\\NEW.TXT",2,0,0x12,&h,&result)==DE_ACCESS);
    CHECK(dos_open_ex("D:\\README.TXT",0,0,0x10,&h,&result)==DE_EXISTS && !mem_open);
    CHECK(dos_open("D:\\WIN",0,0,&h)==DE_ACCESS && dos_open("D:\\NONE.TXT",0,0,&h)==DE_NOFILE);
    CHECK(dos_mkdir("D:\\NEW")==DE_ACCESS && dos_remove("D:\\README.TXT",0)==DE_ACCESS);
    CHECK(dos_rename("D:\\README.TXT","D:\\X.TXT")==DE_ACCESS);
    u8 attr=0; CHECK(!dos_attribute("D:\\WIN\\HIDDEN.SYS",0,&attr) && attr==(FA_RDONLY|FA_HIDDEN));
    CHECK(dos_attribute("D:\\README.TXT",1,&attr)==DE_ACCESS);
    /* Directories, searches and the label. */
    CHECK(!dos_select_drive(3) && !dos_chdir("WIN")); char cwd[DOS_PATH_MAX]; CHECK(!dos_drive_cwd(3,cwd) && !strcmp(cwd,"\\WIN"));
    CHECK(!dos_open("a.txt",0,0,&h) && !dos_read(h,buf,5,&n) && n==5 && !dos_close(h));
    CHECK(dos_chdir("A.TXT")==DE_PATH && dos_chdir("\\NONE")==DE_PATH);
    DosFind find; char names[256]=""; 
    for(int e=dos_find_first("*.*",FA_DIR,&find);!e;e=dos_find_next(&find)) {strappend(names,sizeof(names),find.name); strappend(names,sizeof(names),";");}
    CHECK(!strcmp(names,"SETUP.EXE;A.TXT;"));
    CHECK(!dos_find_first("D:\\WIN\\*.*",FA_HIDDEN,&find) && dos_find_next(&find)==0 && dos_find_next(&find)==0 && !strcmp(find.name,"HIDDEN.SYS"));
    CHECK(!dos_find_first("D:\\*.*",FA_DIR,&find) && !strcmp(find.name,"README.TXT") && !dos_find_next(&find) && !strcmp(find.name,"WIN") && find.attr&FA_DIR);
    CHECK(!dos_find_first("D:\\*.*",FA_VOLUME,&find) && !strcmp(find.name,"WIN30") && find.attr==FA_VOLUME);
    {   /* COMMAND.COM reads labels with an extended-FCB search. */
        u8 dta[64],fcb[44]={0xff,0,0,0,0,0,FA_VOLUME,4}; memset(fcb+8,'?',11);
        DosRegs t={.ax=0x1a00,.dx=(uintptr_t)dta}; CHECK(!call(&t));
        t=(DosRegs){.ax=0x1100,.dx=(uintptr_t)fcb}; dos_int21(&t);
        CHECK(!(t.ax&255) && !memcmp(dta+8,"WIN30      ",11) && dta[8+11]==FA_VOLUME);
        u8 plain[37]={4}; memcpy(plain+1,"A       TXT",11); /* in D:'s current directory, \WIN */
        t=(DosRegs){.ax=0x1100,.dx=(uintptr_t)plain}; dos_int21(&t); CHECK(!(t.ax&255) && !memcmp(dta+1,"A       TXT",11));
        t=(DosRegs){.ax=0x1300,.dx=(uintptr_t)plain}; dos_int21(&t); CHECK((t.ax&255)==0xff);
        t=(DosRegs){.ax=0x0f00,.dx=(uintptr_t)plain}; dos_int21(&t); CHECK((t.ax&255)==0xff && !mem_open);
    }
    CHECK(dos_find_first("D:\\EMPTY\\*.*",0,&find)==DE_NOMORE && dos_find_first("D:\\NONE\\*.*",0,&find)==DE_PATH);
    CHECK(!dos_find_first("D:\\WIN\\S*.E?E",0,&find) && !strcmp(find.name,"SETUP.EXE") && find.size==19);
    /* Space, IOCTL, FAT-only calls. */
    DosDriveInfo info; CHECK(!dos_drive_info(3,&info) && info.flags==(DOS_DRIVE_PRESENT|DOS_DRIVE_REMOVABLE|DOS_DRIVE_READONLY|DOS_DRIVE_REMOTE));
    CHECK(info.sectors_per_cluster==4 && info.total_clusters==100 && !info.free_clusters);
    r=(DosRegs){.ax=0x3600,.dx=4}; dos_int21(&r); CHECK(r.ax==4 && r.bx==0 && r.cx==512 && r.dx==100);
    r=(DosRegs){.ax=0x4409,.bx=4}; CHECK(!call(&r) && r.dx==0x1000);
    r=(DosRegs){.ax=0x4408,.bx=4}; CHECK(call(&r)==DE_REMOTE);
    u8 sector[512]; u32 count; CHECK(dos_disk_read(3,0,1,sector,&count)==DE_REMOTE);
    DosInfo q; CHECK(!dos_query(&q) && !q.fat_bits);
    r=(DosRegs){.ax=0x0e00,.dx=3}; dos_int21(&r); CHECK(r.ax==4);
    /* A new disc invalidates handles and directories; none is "not ready". */
    CHECK(!dos_open("A.TXT",0,0,&h)); mem_generation=2;
    CHECK(dos_read(h,buf,1,&n)==DE_CHANGED); CHECK(!dos_drive_cwd(3,cwd) && !strcmp(cwd,"\\"));
    CHECK(dos_unredirect(3)==DE_ACCESS); CHECK(!dos_close(h) && !mem_open);
    mem_generation=0; CHECK(dos_open("D:\\README.TXT",0,0,&h)==DE_NOTREADY); mem_generation=3;
    CHECK(!dos_unredirect(3) && dos_unredirect(3)==DE_DRIVE && dos_open("D:\\README.TXT",0,0,&h)==DE_DRIVE);
    char letter[3]; CHECK(!dos_drive_cwd(2,cwd)); (void)letter;
    /* A task's redirection ends with it unless it stays resident. */
    u32 self=dos_pid(),task; CHECK(!dos_task_create(&task) && !dos_task_select(task));
    drive=UINT32_MAX; CHECK(!dos_redirect(&drive,&mem_redirector) && drive==3);
    CHECK(!dos_open("D:\\README.TXT",0,0,&h));
    CHECK(!dos_task_select(self) && !dos_task_destroy(task) && !mem_open && dos_select_drive(3)==DE_DRIVE);
    CHECK(!dos_select_drive(2));
    free(arena);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3); FILE *fp=fopen(argv[1],"rb"); CHECK(fp!=NULL);
    CHECK(!fseek(fp,0,SEEK_END)); long length=ftell(fp); CHECK(length>0); rewind(fp);
    u8 *image=malloc(length); CHECK(image!=NULL); CHECK(fread(image,1,length,fp)==(size_t)length); fclose(fp);
    u32 start=rd32(image+454); MemoryDisk memory={.data=image+(u64)start*512,.bytes=(size_t)length-(u64)start*512};
    Disk disk={&memory,read_block,write_block,flush_block,memory.bytes/512,0};
    const char *only=getenv("DOS_TEST_ONLY");
    if(only && !strcmp(only,"fcb")) test_fcb(&disk,&memory);
    else if(only && !strcmp(only,"nls")) test_nls(&disk,&memory);
    else if(only && !strcmp(only,"codepage")) test_codepage(&disk);
    else if(only && !strcmp(only,"ansi")) test_ansi(&disk);
    else if(only && !strcmp(only,"print")) test_print(&disk);
    else if(only && !strcmp(only,"keyb")) test_keyb(&disk);
    else if(only && !strcmp(only,"redirector")) test_redirector(&disk);
    else {
    test_arena(); test_fat(&disk,&memory); test_kernel(&disk); test_tasks(&disk);
    test_environment(&disk); test_config(&disk); test_handle_limits(&disk);
    test_sharing(&disk); test_locks(&disk); test_file_api(&disk,&memory);
    test_transactions(&disk,&memory); test_moves(&disk); test_drives(&disk,&memory); test_console(&disk); test_codepage(&disk); test_keyb(&disk); test_ansi(&disk);
    test_print(&disk);
    test_critical(&disk,&memory);
    test_critical_media(&disk,&memory);
    test_devices(&disk);
    test_ports(&disk);
    test_serial_ports(&disk);
    test_ramdisk(&disk);
    test_block_drivers(&disk,&memory);
    test_cleanup_errors(&disk,&memory);
    test_clock(&disk);
    test_fcb(&disk,&memory);
    test_nls(&disk,&memory);
    test_maintenance(&disk,&memory);
    test_resident(&disk);
    test_redirector(&disk);
    test_temp_verify(&disk,&memory);
    }
    CHECK(!fat_pages);
    if(argc==3) {
        fp=fopen(argv[2],"wb"); CHECK(fp!=NULL);
        CHECK(fwrite(image,1,length,fp)==(size_t)length); CHECK(!fclose(fp));
    }
    free(image); printf("PASS host: %u assertions (FAT, tasks, environment, CONFIG.SYS, sharing, locks, file API, reentry)\n",checks); return 0;
}
