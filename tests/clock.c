/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "../platform/efi_clock.h"
#include "dos_client.h"
static unsigned checks,reads,writes;
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"FAIL clock:%u: %s\n",__LINE__,#x); exit(1);}} while(0)
static IoServices io;
static EFI_TIME rtc;
static EFI_STATUS read_error,write_error;
static int recursive;
static void reentry(void) {
    if(!recursive) return;
    IoDateTime t={2000,1,1,0,0,0,0};
    CHECK(io.clock_get(NULL,&t)==DE_BUSY);
    CHECK(io.clock_set(NULL,&t,IO_CLOCK_DATE|IO_CLOCK_TIME)==DE_BUSY);
}
static EFI_STATUS EFIAPI get_time(EFI_TIME *out,EFI_TIME_CAPABILITIES *cap) {
    CHECK(out && !cap); reads++; reentry();
    if(read_error) return read_error;
    *out=rtc; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_time(EFI_TIME *t) {
    writes++; reentry(); CHECK(!t->Pad1 && !t->Pad2);
    if(write_error) return write_error;
    rtc=*t; return EFI_SUCCESS;
}
static unsigned client_calls,native_calls,date_calls,rollovers;
static int client_error,bad_field;
static const char *tail(void) {return "";}
static void client_int21(DosRegs *r) {
    client_calls++; r->flags=client_error?1:0;
    if(client_error) {r->ax=client_error; return;}
    switch(r->ax>>8) {
    case 0x2a:
        r->cx=2000;
        r->dx=rollovers==2?0x301+(date_calls%2):rollovers && !date_calls?0x21d:0x301;
        date_calls++; r->ax=3; break;
    case 0x2c: r->cx=1; r->dx=(2<<8)|34; r->ax=0; break;
    case 0x2b: CHECK(r->cx==2000 && r->dx==0x21d); r->ax=bad_field?255:0; break;
    case 0x2d: CHECK(r->cx==0x172f && r->dx==0x3b63); r->ax=bad_field?255:0; break;
    default: CHECK(0);
    }
}
static int native_time(DosDateTime *t) {
    native_calls++;
    if(client_error) return client_error;
    *t=(DosDateTime){2026,9,29,1,2,3,45,2}; return 0;
}
int main(void) {
    EFI_RUNTIME_SERVICES rt={.GetTime=get_time,.SetTime=set_time};
    efi_clock_init(&rt,&io); CHECK(io.capabilities&IO_CAP_CLOCK);
    rtc=(EFI_TIME){.Year=2000,.Month=2,.Day=29,.Hour=12,.Minute=34,.Second=56,
        .Nanosecond=780000000,.TimeZone=330,.Daylight=2,.Pad1=1,.Pad2=2};
    IoDateTime t; recursive=1;
    CHECK(!io.clock_get(NULL,&t) && t.year==2000 && t.nanosecond==780000000);
    unsigned old[7]; CHECK(!io.datetime(NULL,old) && old[0]==2000 && old[6]==2);
    t=(IoDateTime){2099,12,31,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    CHECK(!io.clock_set(NULL,&t,IO_CLOCK_DATE));
    CHECK(rtc.Year==2099 && rtc.Month==12 && rtc.Day==31 && rtc.Hour==12 && rtc.Minute==34 && rtc.Second==56);
    CHECK(rtc.Nanosecond==780000000 && rtc.TimeZone==330 && rtc.Daylight==2);
    t=(IoDateTime){UINT32_MAX,UINT32_MAX,UINT32_MAX,0,1,2,340000000};
    CHECK(!io.clock_set(NULL,&t,IO_CLOCK_TIME));
    CHECK(rtc.Year==2099 && rtc.Month==12 && rtc.Day==31 && rtc.Hour==0 && rtc.Minute==1 && rtc.Second==2);
    CHECK(rtc.Nanosecond==340000000 && rtc.TimeZone==330 && rtc.Daylight==2);
    t=(IoDateTime){2000,2,29,23,59,59,990000000};
    CHECK(!io.clock_set(NULL,&t,IO_CLOCK_DATE|IO_CLOCK_TIME));
    CHECK(rtc.Year==2000 && rtc.Day==29 && rtc.Hour==23 && rtc.Nanosecond==990000000);
    recursive=0; unsigned old_reads=reads,old_writes=writes;
    CHECK(io.clock_set(NULL,NULL,3)==DE_FUNCTION && io.clock_set(NULL,&t,0)==DE_FUNCTION);
    CHECK(io.clock_set(NULL,&t,4)==DE_FUNCTION && io.clock_get(NULL,NULL)==DE_FUNCTION);
    t.year=2100; CHECK(io.clock_set(NULL,&t,IO_CLOCK_DATE)==DE_FUNCTION);
    t.year=2000; t.nanosecond=1000000000; CHECK(io.clock_set(NULL,&t,IO_CLOCK_TIME)==DE_FUNCTION);
    t.nanosecond=0; t.hour=24; CHECK(io.clock_set(NULL,&t,IO_CLOCK_TIME)==DE_FUNCTION);
    CHECK(reads==old_reads && writes==old_writes);
    t.hour=0; EFI_TIME before=rtc;
    read_error=EFI_DEVICE_ERROR; CHECK(io.clock_set(NULL,&t,3)==DE_IO && writes==old_writes);
    IoDateTime untouched=t; CHECK(io.clock_get(NULL,&t)==DE_IO && !memcmp(&t,&untouched,sizeof(t)));
    read_error=0; write_error=EFI_DEVICE_ERROR;
    CHECK(io.clock_set(NULL,&t,3)==DE_IO && !memcmp(&rtc,&before,sizeof(rtc)));
    write_error=EFI_UNSUPPORTED; CHECK(io.clock_set(NULL,&t,3)==DE_FUNCTION); write_error=0;
    rtc.Year=2100; CHECK(io.clock_get(NULL,&t)==DE_IO);
    CHECK(!io.clock_set(NULL,&t,IO_CLOCK_DATE) && rtc.Year==2000 && rtc.Day==29); rtc=before;
    rtc.Nanosecond=1000000000; CHECK(io.clock_get(NULL,&t)==DE_IO);
    CHECK(!io.clock_set(NULL,&t,IO_CLOCK_TIME) && !rtc.Nanosecond); rtc=before;
    rtc.TimeZone=1441; CHECK(io.clock_set(NULL,&t,3)==DE_IO); rtc=before;
    rtc.Daylight=4; CHECK(io.datetime(NULL,old)==DE_IO); rtc=before;
    rt.SetTime=NULL; efi_clock_init(&rt,&io);
    CHECK(!(io.capabilities&IO_CAP_CLOCK) && io.clock_set(NULL,&t,3)==DE_FUNCTION);
    CHECK(!io.clock_get(NULL,&t)); efi_clock_init(NULL,&io); CHECK(io.clock_get(NULL,&t)==DE_FUNCTION);
    DosApi api={.version=DOS_ABI_VERSION,.size=sizeof(api),.int21=client_int21,.command_tail=tail,.datetime=native_time};
    CHECK(!dos_client_bind(&api)); DosDateTime now;
    CHECK(!dos_get_datetime(&now) && now.hundredth==45 && native_calls==1 && !client_calls);
    CHECK(!dos_datetime(old) && old[0]==2026 && old[3]==1 && old[6]==2);
    api.size=offsetof(DosApi,datetime); rollovers=1;
    CHECK(!dos_get_datetime(&now) && now.year==2000 && now.month==3 && now.day==1 && now.hundredth==34);
    CHECK(date_calls==4 && client_calls==6);
    DosDateTime saved=now; rollovers=2; date_calls=0;
    CHECK(dos_get_datetime(&now)==DE_BUSY && !memcmp(&now,&saved,sizeof(now)));
    client_error=DE_IO; CHECK(dos_get_datetime(&now)==DE_IO && !memcmp(&now,&saved,sizeof(now))); client_error=0;
    CHECK(!dos_set_date(2000,2,29) && !dos_set_time(23,47,59,99));
    bad_field=1; CHECK(dos_set_date(2000,2,29)==DE_FUNCTION && dos_set_time(23,47,59,99)==DE_FUNCTION); bad_field=0;
    unsigned calls=client_calls;
    CHECK(dos_set_date(2100,1,1)==DE_FUNCTION && dos_set_date(2000,258,29)==DE_FUNCTION);
    CHECK(dos_set_time(256,0,0,0)==DE_FUNCTION && dos_set_time(0,0,0,100)==DE_FUNCTION);
    CHECK(dos_get_datetime(NULL)==DE_FUNCTION && dos_datetime(NULL)==DE_FUNCTION && client_calls==calls);
    printf("PASS clock backend/SDK: %u assertions (EFI preservation, failures, reentry, legacy ABI and midnight sampling)\n",checks);
    return 0;
}
