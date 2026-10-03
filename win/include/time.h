/* SPDX-License-Identifier: GPL-2.0-or-later
 * Time: time_t counts seconds since 1970 UTC. The machine's clock is local
 * time in TZ's zone (tzset), PST8PDT when TZ is not set, as in Microsoft C. */
#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef long time_t;
#endif
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
#define CLK_TCK CLOCKS_PER_SEC
struct tm {int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday,tm_yday,tm_isdst;};
time_t time(time_t *);
clock_t clock(void);
double difftime(time_t,time_t);
time_t mktime(struct tm *);
struct tm *localtime(const time_t *);
struct tm *gmtime(const time_t *);
char *asctime(const struct tm *);
char *ctime(const time_t *);
size_t strftime(char *,size_t,const char *,const struct tm *);
extern long timezone;
extern int daylight;
extern char *tzname[2];
void tzset(void);
#define _timezone timezone
#define _daylight daylight
#define _tzname tzname
#define _tzset tzset
#endif
