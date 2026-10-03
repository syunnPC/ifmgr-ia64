/* SPDX-License-Identifier: GPL-2.0-or-later
 * Time: seconds since 1970 UTC. The machine's clock is local time, and the
 * time zone is TZ's as Microsoft C reads it, tzn[+|-]hh[:mm[:ss]][dzn]
 * (hours west of UTC, a daylight saving name when there is one), with
 * Microsoft C's own default, PST8PDT, when TZ is not set. Daylight saving
 * time follows the United States' rules, as Microsoft C's does. C89. */
#include <windows.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
long timezone=8*3600L;
int daylight=1;
static char standard_name[10]="PST",daylight_name[10]="PDT";
char *tzname[2]={standard_name,daylight_name};
static int zone_read;
static int name(const char **s,char *out) {
    int n=0;
    while(n<9 && isalpha((unsigned char)**s)) out[n++]=*(*s)++;
    out[n]=0;
    return n;
}
static long number(const char **s) {long v=0; while(isdigit((unsigned char)**s)) v=v*10+*(*s)++-'0'; return v;}
void tzset(void) {
    const char *s=getenv("TZ"); long sign=1,h,m=0,sec=0;
    zone_read=1;
    if(!s || !*s) s="PST8PDT";
    name(&s,standard_name);
    if(*s=='+' || *s=='-') {if(*s=='-') sign=-1; s++;}
    h=number(&s);
    if(*s==':') {s++; m=number(&s); if(*s==':') {s++; sec=number(&s);}}
    timezone=sign*(h*3600L+m*60L+sec);
    daylight=name(&s,daylight_name)!=0;
}
static void zone(void) {if(!zone_read) tzset();}
static const int month_days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
static int leap(int year) {return (year%4==0 && year%100!=0) || year%400==0;}
/* Days from 1970-01-01 to a date (year from 1900, month 0-12). */
static long days(int year,int month,int day) {
    long d=0; int y,m;
    year+=1900;
    for(y=1970;y<year;y++) d+=leap(y)?366:365;
    for(y=year;y<1970;y++) d-=leap(y)?366:365;
    for(m=0;m<month;m++) d+=month_days[m]+(m==1 && leap(year));
    return d+day-1;
}
static int weekday(long d) {return (int)((d%7+11)%7);} /* 1970-01-01 was a Thursday */
/* A month's nth Sunday, or its last (0), as a day from 1970. */
static long sunday(int year,int month,int nth) {
    long d;
    if(!nth) {d=days(year,month+1,1)-1; return d-weekday(d);}
    d=days(year,month,1);
    return d+(7-weekday(d))%7+7*(nth-1);
}
/* Seconds of a broken-down time, and back, with no zone. */
static long seconds_of(const struct tm *t) {
    return (days(t->tm_year,t->tm_mon,1)+t->tm_mday-1)*86400L+t->tm_hour*3600L+t->tm_min*60L+t->tm_sec;
}
static struct tm shared; /* gmtime's and localtime's, as in Microsoft C */
static struct tm *breakdown(long s) {
    struct tm *t=&shared; long d; int y=1970,m=0;
    d=s/86400; s%=86400; if(s<0) {s+=86400; d--;}
    t->tm_hour=(int)(s/3600); t->tm_min=(int)(s/60%60); t->tm_sec=(int)(s%60);
    t->tm_wday=weekday(d);
    while(d>=(leap(y)?366:365)) {d-=leap(y)?366:365; y++;}
    while(d<0) {y--; d+=leap(y)?366:365;}
    t->tm_year=y-1900; t->tm_yday=(int)d;
    while(d>=month_days[m]+(m==1 && leap(y))) {d-=month_days[m]+(m==1 && leap(y)); m++;}
    t->tm_mon=m; t->tm_mday=(int)d+1; t->tm_isdst=0;
    return t;
}
/* Whether local standard time s is in daylight saving time: from 2:00 on
 * the first Sunday of April (the last before 1987, the second of March
 * from 2007) to 2:00 daylight time on the last Sunday of October (the
 * first of November from 2007). */
static int in_daylight(long s) {
    int year=breakdown(s)->tm_year; long start,end;
    if(year>=107) {start=sunday(year,2,2); end=sunday(year,10,1);}
    else if(year>=87) {start=sunday(year,3,1); end=sunday(year,9,0);}
    else {start=sunday(year,3,0); end=sunday(year,9,0);}
    return s>=start*86400L+2*3600L && s<end*86400L+3600L;
}
struct tm *gmtime(const time_t *when) {return breakdown(*when);}
struct tm *localtime(const time_t *when) {
    long s; int dst; struct tm *t;
    zone(); s=*when-timezone;
    dst=daylight && in_daylight(s);
    t=breakdown(dst?s+3600L:s); t->tm_isdst=dst;
    return t;
}
/* Local time to UTC: tm_isdst says whether it is daylight time, or when
 * negative, the rules say. The fields come back normalised. */
time_t mktime(struct tm *t) {
    long s; int dst;
    zone();
    while(t->tm_mon<0) {t->tm_mon+=12; t->tm_year--;}
    while(t->tm_mon>11) {t->tm_mon-=12; t->tm_year++;}
    s=seconds_of(t);
    dst=daylight && (t->tm_isdst>0 || (t->tm_isdst<0 && in_daylight(s)));
    s+=timezone-(dst?3600L:0L);
    *t=*localtime(&s);
    return (time_t)s;
}
time_t time(time_t *out) {
    SYSTEMTIME st; struct tm t; time_t s;
    GetLocalTime(&st);
    t.tm_year=st.wYear-1900; t.tm_mon=st.wMonth-1; t.tm_mday=st.wDay;
    t.tm_hour=st.wHour; t.tm_min=st.wMinute; t.tm_sec=st.wSecond; t.tm_isdst=-1;
    s=mktime(&t);
    if(out) *out=s;
    return s;
}
clock_t clock(void) {
    static DWORD start; static int started;
    if(!started) {start=GetTickCount(); started=1;}
    return (clock_t)(GetTickCount()-start);
}
double difftime(time_t a,time_t b) {return (double)a-(double)b;}
static const char *const day_names[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *const month_names[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
char *asctime(const struct tm *t) {
    static char s[26];
    sprintf(s,"%.3s %.3s %2d %02d:%02d:%02d %d\n",day_names[t->tm_wday%7],month_names[t->tm_mon%12],t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,t->tm_year+1900);
    return s;
}
char *ctime(const time_t *when) {return asctime(localtime(when));}
size_t strftime(char *out,size_t max,const char *f,const struct tm *t) {
    size_t n=0; char item[64];
    for(;*f;f++) {
        size_t k;
        if(*f!='%') {item[0]=*f; item[1]=0;}
        else switch(*++f) {
        case 'a': sprintf(item,"%.3s",day_names[t->tm_wday%7]); break;
        case 'A': strcpy(item,day_names[t->tm_wday%7]); break;
        case 'b': sprintf(item,"%.3s",month_names[t->tm_mon%12]); break;
        case 'B': strcpy(item,month_names[t->tm_mon%12]); break;
        case 'c': sprintf(item,"%02d/%02d/%02d %02d:%02d:%02d",t->tm_mon+1,t->tm_mday,t->tm_year%100,t->tm_hour,t->tm_min,t->tm_sec); break;
        case 'd': sprintf(item,"%02d",t->tm_mday); break;
        case 'H': sprintf(item,"%02d",t->tm_hour); break;
        case 'I': sprintf(item,"%02d",t->tm_hour%12?t->tm_hour%12:12); break;
        case 'j': sprintf(item,"%03d",t->tm_yday+1); break;
        case 'm': sprintf(item,"%02d",t->tm_mon+1); break;
        case 'M': sprintf(item,"%02d",t->tm_min); break;
        case 'p': strcpy(item,t->tm_hour<12?"AM":"PM"); break;
        case 'S': sprintf(item,"%02d",t->tm_sec); break;
        case 'U': sprintf(item,"%02d",(t->tm_yday+7-t->tm_wday)/7); break;
        case 'w': sprintf(item,"%d",t->tm_wday); break;
        case 'W': sprintf(item,"%02d",(t->tm_yday+7-(t->tm_wday+6)%7)/7); break;
        case 'x': sprintf(item,"%02d/%02d/%02d",t->tm_mon+1,t->tm_mday,t->tm_year%100); break;
        case 'X': sprintf(item,"%02d:%02d:%02d",t->tm_hour,t->tm_min,t->tm_sec); break;
        case 'y': sprintf(item,"%02d",t->tm_year%100); break;
        case 'Y': sprintf(item,"%d",t->tm_year+1900); break;
        case 'Z': zone(); strcpy(item,tzname[t->tm_isdst>0]); break;
        case '%': strcpy(item,"%"); break;
        default: if(!*f) f--; item[0]=0;
        }
        k=strlen(item);
        if(n+k>=max) return 0;
        memcpy(out+n,item,k); n+=k;
    }
    out[n]=0; return n;
}
