/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static u16 packed_time(const DosDateTime *t) {return (t->hour<<11)|(t->minute<<5)|(t->second/2);}
static void verify_file(void) {
    unsigned h; int e=dos_open("C:\\CLOCK.TST",0,0,&h); check("timestamp-reopen",!e);
    if(e) return;
    u16 date=0,time=0; e=dos_file_time(h,0,&date,&time);
    check("timestamp-persisted",!e && date==((20<<9)|(2<<5)|29) && (time>>11)==12 && ((time>>5)&63)>=34 && ((time>>5)&63)<=35);
    char bytes[5]; u32 n; check("file-data-persisted",!dos_read(h,bytes,sizeof(bytes),&n) && n==5 && !memcmp(bytes,"clock",5));
    check("timestamp-close",!dos_close(h));
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    if(!stricmp(app_dos->command_tail(),"verify")) {verify_file(); goto finish;}
    const IoServices *io=dos_io_services();
    check("native-clock-interface",app_dos->size>=offsetof(DosApi,datetime)+sizeof(app_dos->datetime) &&
        app_dos->datetime && (app_dos->capabilities&DOS_CAP_DATETIME) && io &&
        io->size>=offsetof(IoServices,clock_set)+sizeof(io->clock_set) && io->clock_get && io->clock_set);
    if(failed) goto finish;
    IoDateTime saved; int e=io->clock_get(io->context,&saved); check("save-clock",!e); if(e) goto finish;
    u64 ticks=io->ticks_ms?io->ticks_ms(io->context):0;
    check("set-time",!dos_set_time(12,34,56,78));
    check("set-leap-date",!dos_set_date(2000,2,29));
    DosDateTime now; e=dos_get_datetime(&now);
    check("single-clock-snapshot",!e && now.year==2000 && now.month==2 && now.day==29 && now.weekday==2 &&
        now.hour==12 && now.minute>=34 && now.minute<=35 && now.hundredth<100);
    DosRegs r={.ax=0x2a00}; check("get-date-registers",!dos_call(&r) && r.cx==2000 && r.dx==0x21d && r.ax==2);
    r=(DosRegs){.ax=0x2c00}; check("get-time-registers",!dos_call(&r) && (r.cx>>8)==12 && (r.dx&255)<100 && !r.ax);
    const DosRegs invalid[]={
        {.ax=0x2b00,.cx=2001,.dx=0x21d},{.ax=0x2b00,.cx=2100,.dx=0x101},
        {.ax=0x2b00,.cx=1979,.dx=0x101},{.ax=0x2b00,.cx=2000,.dx=0x41f},
        {.ax=0x2b00,.cx=2000,.dx=0x100},{.ax=0x2b00,.cx=(1ULL<<32)+2000,.dx=0x101},
        {.ax=0x2d00,.cx=24<<8},{.ax=0x2d00,.cx=60},{.ax=0x2d00,.dx=60<<8},
        {.ax=0x2d00,.dx=100},{.ax=0x2d00,.dx=1ULL<<32}};
    for(unsigned i=0;i<ARRAY_SIZE(invalid);i++) {r=invalid[i]; check("invalid-fields-AL-FF",!dos_call(&r) && r.ax==255);}
    check("SDK-no-truncation",dos_set_date(2000,258,29)==DE_FUNCTION && dos_set_time(256,0,0,0)==DE_FUNCTION);
    e=dos_get_datetime(&now); check("invalid-fields-preserve-clock",!e && now.year==2000 && now.day==29 && now.hour==12);
    unsigned h; e=dos_open("C:\\CLOCK.TST",2,1,&h); check("timestamp-create",!e);
    if(!e) {
        DosDateTime before,after; u32 n; u16 date=0,time=0;
        int before_error=dos_get_datetime(&before);
        check("timestamp-write",!dos_write(h,"clock",5,&n) && n==5);
        int stamp=dos_file_time(h,0,&date,&time),after_error=dos_get_datetime(&after);
        check("FAT-clock-source",!stamp && !before_error && !after_error && date==((20<<9)|(2<<5)|29) &&
            time>=packed_time(&before) && time<=packed_time(&after));
        check("timestamp-commit",!dos_close(h));
    }
    u32 child=0; DosInfo parent;
    e=dos_query(&parent); if(!e) e=app_dos->task_create(&child);
    check("clock-task-create",!e);
    if(!e) {
        e=app_dos->task_select(child); check("clock-task-select",!e);
        if(!e) check("clock-task-shared",!dos_get_datetime(&now) && now.year==2000 && now.day==29);
        check("clock-parent-select",!app_dos->task_select(parent.pid));
        check("clock-task-release",!app_dos->task_destroy(child));
    }
    check("first-DOS-date",!dos_set_date(1980,1,1) && !dos_get_datetime(&now) && now.year==1980 && now.weekday==2);
    check("last-DOS-date",!dos_set_date(2099,12,31) && !dos_get_datetime(&now) && now.year==2099 && now.weekday==4);
    if(io->ticks_ms) check("wall-time-keeps-monotonic-ticks",io->ticks_ms(io->context)>=ticks);
    check("restore-clock",!io->clock_set(io->context,&saved,IO_CLOCK_DATE|IO_CLOCK_TIME));
    e=dos_get_datetime(&now); check("restored-date",!e && now.year==saved.year && now.month==saved.month && now.day==saved.day);
    verify_file();
finish:
    print("TIMETEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
