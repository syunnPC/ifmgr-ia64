/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_CALENDAR_H
#define DOS_CALENDAR_H
#include "base.h"
static inline int calendar_date(unsigned year,unsigned month,unsigned day) {
    static const u8 days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(!year || year>9999 || !month || month>12 || !day) return 0;
    unsigned leap=!(year%4) && ((year%100) || !(year%400));
    return day<=(unsigned)days[month-1]+(month==2 && leap);
}
static inline int calendar_time(unsigned hour,unsigned minute,unsigned second,unsigned nanosecond) {
    return hour<24 && minute<60 && second<60 && nanosecond<1000000000;
}
/* Sunday is zero; the date must already have passed validation. */
static inline unsigned calendar_weekday(unsigned year,unsigned month,unsigned day) {
    static const u8 offset[]={0,3,2,5,0,3,5,1,4,6,2,4};
    year-=month<3;
    return (year+year/4-year/100+year/400+offset[month-1]+day)%7;
}
#endif
