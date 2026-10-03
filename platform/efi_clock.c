/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "efi_clock.h"
#include "calendar.h"
static EFI_RUNTIME_SERVICES *runtime;
static int busy;
static int status(EFI_STATUS e) {
    if(!EFI_ERROR(e)) return 0;
    if(e==EFI_UNSUPPORTED) return DE_FUNCTION;
    return DE_IO;
}
static int valid(const EFI_TIME *t) {
    return t->Year>=1900 && calendar_date(t->Year,t->Month,t->Day) &&
        calendar_time(t->Hour,t->Minute,t->Second,t->Nanosecond) &&
        (t->TimeZone==2047 || (t->TimeZone>=-1440 && t->TimeZone<=1440)) && !(t->Daylight&~3U);
}
static int clock_get(void *context,IoDateTime *out) {
    (void)context;
    if(!out) return DE_FUNCTION;
    if(busy) return DE_BUSY;
    if(!runtime || !runtime->GetTime) return DE_FUNCTION;
    EFI_TIME t={0}; busy=1;
    int e=status(runtime->GetTime(&t,NULL)); busy=0;
    if(e) return e;
    if(!valid(&t)) return DE_IO;
    *out=(IoDateTime){t.Year,t.Month,t.Day,t.Hour,t.Minute,t.Second,t.Nanosecond}; return 0;
}
static int clock_set(void *context,const IoDateTime *input,u32 fields) {
    (void)context;
    if(!input || !fields || (fields&~(IO_CLOCK_DATE|IO_CLOCK_TIME))) return DE_FUNCTION;
    if((fields&IO_CLOCK_DATE) && (input->year<1900 || !calendar_date(input->year,input->month,input->day))) return DE_FUNCTION;
    if((fields&IO_CLOCK_TIME) && !calendar_time(input->hour,input->minute,input->second,input->nanosecond)) return DE_FUNCTION;
    if(busy) return DE_BUSY;
    if(!runtime || !runtime->GetTime || !runtime->SetTime) return DE_FUNCTION;
    /* Copy before entering firmware, then preserve the unselected fields and
     * timezone metadata from one GetTime call. No read/modify/write in DOS. */
    IoDateTime next=*input; EFI_TIME t={0}; busy=1;
    int e=status(runtime->GetTime(&t,NULL));
    if(!e) {
        if(fields&IO_CLOCK_DATE) {t.Year=next.year; t.Month=next.month; t.Day=next.day;}
        if(fields&IO_CLOCK_TIME) {t.Hour=next.hour; t.Minute=next.minute; t.Second=next.second; t.Nanosecond=next.nanosecond;}
        t.Pad1=t.Pad2=0;
        /* A valid replacement may repair an invalid selected field. Any bad
         * field that would be preserved still prevents the firmware write. */
        e=valid(&t)?status(runtime->SetTime(&t)):DE_IO;
    }
    busy=0; return e;
}
static int datetime(void *context,unsigned out[7]) {
    if(!out) return DE_FUNCTION;
    IoDateTime t; int e=clock_get(context,&t); if(e) return e;
    out[0]=t.year; out[1]=t.month; out[2]=t.day;
    out[3]=t.hour; out[4]=t.minute; out[5]=t.second;
    out[6]=calendar_weekday(t.year,t.month,t.day); return 0;
}
void efi_clock_init(EFI_RUNTIME_SERVICES *services,IoServices *io) {
    runtime=services; busy=0;
    io->datetime=datetime; io->clock_get=clock_get; io->clock_set=clock_set;
    io->capabilities&=~IO_CAP_CLOCK;
    if(runtime && runtime->GetTime && runtime->SetTime) io->capabilities|=IO_CAP_CLOCK;
}
