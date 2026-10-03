/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos.h"
#include "device.h"
#include "clock.h"
#include "calendar.h"
int dos_clock_read(IoDateTime *out) {
    if(!out) return DE_FUNCTION;
    const IoServices *io=platform_io_services();
    if(!io || io->version!=IO_ABI_VERSION) return DE_FUNCTION;
    IoDateTime t={0}; int e;
    if(io->size>=offsetof(IoServices,clock_get)+sizeof(io->clock_get) && io->clock_get)
        e=io->clock_get(io->context,&t);
    else {
        if(io->size<offsetof(IoServices,datetime)+sizeof(io->datetime) || !io->datetime) return DE_FUNCTION;
        unsigned old[7]={0}; e=io->datetime(io->context,old);
        if(!e) t=(IoDateTime){old[0],old[1],old[2],old[3],old[4],old[5],0};
    }
    if(e) return e<0?DE_IO:e;
    if(t.year<1900 || !calendar_date(t.year,t.month,t.day) ||
       !calendar_time(t.hour,t.minute,t.second,t.nanosecond)) return DE_IO;
    *out=t; return 0;
}
int dos_clock_write(const IoDateTime *t,u32 fields) {
    if(!t || !fields || (fields&~(IO_CLOCK_DATE|IO_CLOCK_TIME))) return DE_FUNCTION;
    if((fields&IO_CLOCK_DATE) && (t->year<1900 || !calendar_date(t->year,t->month,t->day))) return DE_FUNCTION;
    if((fields&IO_CLOCK_TIME) && !calendar_time(t->hour,t->minute,t->second,t->nanosecond)) return DE_FUNCTION;
    const IoServices *io=platform_io_services();
    if(!io || io->version!=IO_ABI_VERSION || io->size<offsetof(IoServices,clock_set)+sizeof(io->clock_set) || !io->clock_set)
        return DE_FUNCTION;
    int e=io->clock_set(io->context,t,fields); return e<0?DE_IO:e;
}
void platform_fat_time(u16 *date,u16 *time) {
    IoDateTime t;
    if(dos_clock_read(&t) || t.year<1980 || t.year>2107) {*date=0x21; *time=0; return;}
    *date=((t.year-1980)<<9)|(t.month<<5)|t.day;
    *time=(t.hour<<11)|(t.minute<<5)|(t.second/2);
}
