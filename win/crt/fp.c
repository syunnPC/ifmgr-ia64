/* SPDX-License-Identifier: GPL-2.0-or-later
 * Decimal conversion of doubles: digits for printf, ecvt, fcvt and gcvt,
 * and strtod. The value is scaled into [1,10) by exact powers of ten and
 * rounded once into a 17-digit integer, so results are good to about 15
 * significant digits. C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include "fp.h"
static unsigned __int64 bits(double x) {unsigned __int64 b; memcpy(&b,&x,8); return b;}
int _fp_class(double x) {
    unsigned __int64 b=bits(x);
    if(((b>>52)&0x7ff)!=0x7ff) return FP_FINITE;
    return b&0xfffffffffffffui64?FP_NAN:FP_INF;
}
/* 17 significant digits of x>0 and the power of ten of the first. */
static void digits17(double x,char *d,int *exp10) {
    int e=0,i; unsigned __int64 m;
    while(x>=1e16) {x/=1e16; e+=16;}
    while(x>=10) {x/=10; e++;}
    while(x<1e-16) {x*=1e16; e-=16;}
    while(x<1) {x*=10; e--;}
    m=(unsigned __int64)(x*1e16+0.5);
    if(m>=100000000000000000ui64) {m/=10; e++;}
    for(i=16;i>=0;i--) {d[i]=(char)('0'+(int)(m%10)); m/=10;}
    *exp10=e;
}
/* x's digits into buf: n significant ones (mode FP_SIGNIFICANT) or those
 * down to the n-th after the point (FP_FIXED), rounded half up; *point is
 * where the decimal point goes, counted from the first digit. Returns how
 * many digits (an empty result, all rounded away, is "0" with *point 1). */
int _fp_digits(double x,int mode,int n,char *buf,int *point) {
    char d[17]; int e,want,i;
    if(x<0) x=-x;
    if(x==0) {
        want=mode==FP_FIXED?n+1:(n<1?1:n);
        if(want<1) want=1;
        memset(buf,'0',(size_t)want); buf[want]=0; *point=1; return want;
    }
    digits17(x,d,&e);
    want=mode==FP_FIXED?e+1+n:n;
    if(want<0) {buf[0]='0'; buf[1]=0; *point=1; return 1;}
    if(want==0) {
        /* Everything below the last place: it rounds to 0 or to one unit there. */
        if(d[0]>='5') {buf[0]='1'; buf[1]=0; *point=e+2; return 1;}
        buf[0]='0'; buf[1]=0; *point=1; return 1;
    }
    for(i=0;i<want;i++) buf[i]=i<17?d[i]:'0';
    buf[want]=0;
    if(want<17 && d[want]>='5') {
        for(i=want-1;i>=0;i--) {if(buf[i]=='9') buf[i]='0'; else {buf[i]++; break;}}
        if(i<0) {
            memmove(buf+1,buf,(size_t)want); buf[0]='1'; e++;
            if(mode==FP_FIXED) want++; buf[want]=0;
        }
    }
    *point=e+1; return want;
}
double strtod(const char *s,char **end) {
    const char *p=s; int negative=0,exp10=0,any=0,count=0; unsigned __int64 m=0; double v;
    while(isspace((unsigned char)*p)) p++;
    if(*p=='-' || *p=='+') negative=*p++=='-';
    for(;isdigit((unsigned char)*p);p++,any=1) {if(count<19) {m=m*10+(unsigned)(*p-'0'); if(m) count++;} else exp10++;}
    if(*p=='.') for(p++;isdigit((unsigned char)*p);p++,any=1) {if(count<19) {m=m*10+(unsigned)(*p-'0'); if(m) count++; exp10--;}}
    if(!any) {if(end) *end=(char *)s; return 0;}
    if(*p=='e' || *p=='E') {
        const char *q=p+1; int sign=1,x=0,digits=0;
        if(*q=='-' || *q=='+') sign=*q++=='-'?-1:1;
        for(;isdigit((unsigned char)*q);q++,digits=1) if(x<10000) x=x*10+(*q-'0');
        if(digits) {exp10+=sign*x; p=q;}
    }
    if(end) *end=(char *)p;
    v=m>>63?(double)(__int64)(m>>1)*2+(double)(__int64)(m&1):(double)(__int64)m;
    if(v!=0) {
        while(exp10>=16) {if(v>1.7976931348623158e+292) {errno=ERANGE; v=HUGE_VAL; exp10=0; break;} v*=1e16; exp10-=16;}
        while(exp10>0) {v*=10; exp10--;}
        while(exp10<=-16) {v/=1e16; exp10+=16;}
        while(exp10<0) {v/=10; exp10++;}
        if(v==0) errno=ERANGE;
    }
    return negative?-v:v;
}
static char cvt_buffer[350];
char *ecvt(double x,int n,int *point,int *sign) {
    *sign=x<0; if(n>17) n=17; if(n<1) n=1;
    _fp_digits(x,FP_SIGNIFICANT,n,cvt_buffer,point);
    return cvt_buffer;
}
char *fcvt(double x,int n,int *point,int *sign) {
    *sign=x<0; if(n>300) n=300;
    _fp_digits(x,FP_FIXED,n,cvt_buffer,point);
    return cvt_buffer;
}
char *gcvt(double x,int n,char *out) {
    sprintf(out,"%.*g",n,x);
    return out;
}
