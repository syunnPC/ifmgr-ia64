/* SPDX-License-Identifier: GPL-2.0-or-later
 * Mathematical functions in double precision: arguments reduced (by powers
 * of two, multiples of ln 2 or of pi/2 split in three parts) and short
 * series, good to about 1e-15 relative. Domain errors give a NaN and EDOM,
 * overflow HUGE_VAL and ERANGE, as Microsoft C's. C89. */
#include <math.h>
#include <errno.h>
#include <string.h>
#pragma function(acos,asin,atan,atan2,cos,cosh,exp,fabs,floor,fmod,log,log10,pow,sin,sinh,sqrt,tan,tanh)
#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define INV_LN2 1.44269504088896338700e+00
#define PIO2_1 1.57079632673412561417e+00
#define PIO2_2 6.07710050650619224932e-11
#define PIO2_3 2.02226624879595063154e-21
#define PI 3.14159265358979323846
#define PIO2 1.57079632679489661923
static double from_bits(unsigned __int64 b) {double x; memcpy(&x,&b,8); return x;}
static unsigned __int64 to_bits(double x) {unsigned __int64 b; memcpy(&b,&x,8); return b;}
const _HUGE_T _HUGE_U={0x7ff0000000000000ui64};
static double huge(void) {return from_bits(0x7ff0000000000000ui64);}
static double nan(void) {return from_bits(0xfff8000000000000ui64);}
static double domain(void) {errno=EDOM; return nan();}
double fabs(double x) {return from_bits(to_bits(x)&0x7fffffffffffffffui64);}
double ldexp(double x,int n) {
    unsigned __int64 b=to_bits(x); int e=(int)((b>>52)&0x7ff);
    if(x==0 || e==0x7ff) return x;
    while(n>1000) {x*=from_bits(0x7e70000000000000ui64); n-=1000;} /* 2^1000 */
    while(n<-1000) {x*=from_bits(0x0170000000000000ui64); n+=1000;} /* 2^-1000 */
    while(n>0) {int k=n>60?60:n; x*=(double)(1ui64<<k); n-=k;}
    while(n<0) {int k=-n>60?60:-n; x/=(double)(1ui64<<k); n+=k;}
    if(fabs(x)==huge()) errno=ERANGE;
    return x;
}
double frexp(double x,int *exp) {
    unsigned __int64 b=to_bits(x); int e=(int)((b>>52)&0x7ff),extra=0;
    *exp=0;
    if(x==0 || e==0x7ff) return x;
    if(!e) {x*=(double)(1ui64<<54); b=to_bits(x); e=(int)((b>>52)&0x7ff); extra=-54;}
    *exp=e-1022+extra;
    return from_bits((b&0x800fffffffffffffui64)|0x3fe0000000000000ui64);
}
double floor(double x) {
    double t;
    if(!(fabs(x)<4503599627370496.0)) return x; /* 2^52 and above: whole */
    t=(double)(__int64)x;
    return t>x?t-1:t;
}
double ceil(double x) {double t=floor(x); return t<x?t+1:t;}
double modf(double x,double *whole) {
    double t=x<0?ceil(x):floor(x);
    *whole=t; return x-t;
}
double fmod(double x,double y) {
    double r=fabs(x),a=fabs(y);
    if(a==0) return domain();
    while(r>=a) {
        int er,ea; double t;
        frexp(r,&er); frexp(a,&ea);
        t=ldexp(a,er-ea);
        if(t>r) t/=2;
        r-=t;
    }
    return x<0?-r:r;
}
double sqrt(double x) {
    double y; int e,i;
    if(x<0) return domain();
    if(x==0 || x==huge()) return x;
    y=frexp(x,&e); /* x=y*2^e, y in [0.5,1) */
    if(e&1) {y*=2; e--;}
    y=ldexp((y+1)/2,e/2); /* within a factor of two */
    for(i=0;i<6;i++) y=0.5*(y+x/y);
    return y;
}
double hypot(double x,double y) {
    double a=fabs(x),b=fabs(y),t;
    if(a<b) {t=a; a=b; b=t;}
    if(a==0) return 0;
    t=b/a; return a*sqrt(1+t*t);
}
double cabs(struct _complex z) {return hypot(z.x,z.y);}
double exp(double x) {
    double k,r,p,term; int i,n;
    if(x>709.782712893384) {errno=ERANGE; return huge();}
    if(x<-745.1332191019412) return 0;
    k=floor(x*INV_LN2+0.5); n=(int)k;
    r=(x-k*LN2_HI)-k*LN2_LO;
    p=1; term=1;
    for(i=1;i<=18;i++) {term*=r/i; p+=term;}
    return ldexp(p,n);
}
double log(double x) {
    double m,s,s2,sum,term; int e,i;
    if(x<0) return domain();
    if(x==0) {errno=ERANGE; return -huge();}
    if(x==huge()) return x;
    m=frexp(x,&e);
    if(m<0.70710678118654752440) {m*=2; e--;}
    s=(m-1)/(m+1); s2=s*s; sum=0; term=s;
    for(i=1;i<=41;i+=2) {sum+=term/i; term*=s2;}
    return e*LN2_HI+(e*LN2_LO+2*sum);
}
double log10(double x) {return log(x)*0.43429448190325182765;}
double pow(double x,double y) {
    double r; int negative=0;
    if(y==0) return 1;
    if(x==0) {if(y<0) {errno=ERANGE; return huge();} return 0;}
    if(x<0) {
        if(floor(y)!=y) return domain();
        negative=fmod(y,2)!=0; x=-x;
    }
    if(floor(y)==y && fabs(y)<2147483648.0) {
        /* Whole powers by squaring. */
        long n=(long)fabs(y); double b=x;
        r=1;
        while(n) {if(n&1) r*=b; b*=b; n>>=1;}
        if(y<0) r=1/r;
    } else r=exp(y*log(x));
    if(r==huge()) errno=ERANGE;
    return negative?-r:r;
}
/* x reduced to r in [-pi/4,pi/4] and the quadrant. */
static double reduce(double x,int *quadrant) {
    double k=floor(x/PIO2+0.5);
    *quadrant=(int)fmod(k,4); if(*quadrant<0) *quadrant+=4;
    return ((x-k*PIO2_1)-k*PIO2_2)-k*PIO2_3;
}
static double sin_series(double r) {double r2=r*r,term=r,sum=r; int i; for(i=3;i<=25;i+=2) {term*=-r2/((i-1)*i); sum+=term;} return sum;}
static double cos_series(double r) {double r2=r*r,term=1,sum=1; int i; for(i=2;i<=24;i+=2) {term*=-r2/((i-1)*i); sum+=term;} return sum;}
double sin(double x) {
    int q; double r;
    if(fabs(x)==huge()) return domain();
    r=reduce(x,&q);
    switch(q) {case 0: return sin_series(r); case 1: return cos_series(r); case 2: return -sin_series(r); default: return -cos_series(r);}
}
double cos(double x) {
    int q; double r;
    if(fabs(x)==huge()) return domain();
    r=reduce(x,&q);
    switch(q) {case 0: return cos_series(r); case 1: return -sin_series(r); case 2: return -cos_series(r); default: return sin_series(r);}
}
double tan(double x) {
    int q; double r,s,c;
    if(fabs(x)==huge()) return domain();
    r=reduce(x,&q); s=sin_series(r); c=cos_series(r);
    return q&1?-c/s:s/c;
}
double atan(double x) {
    double a=fabs(x),base=0,s,s2,term,sum; int i,invert=0;
    if(a>1) {a=1/a; invert=1;}
    if(a>0.26794919243112270647) {a=(a*1.73205080756887729353-1)/(1.73205080756887729353+a); base=PI/6;}
    s=a; s2=s*s; term=s; sum=0;
    for(i=1;i<=41;i+=2) {sum+=term/i; term*=-s2;}
    sum+=base;
    if(invert) sum=PIO2-sum;
    return x<0?-sum:sum;
}
double atan2(double y,double x) {
    if(x==0) {if(y==0) return 0; return y>0?PIO2:-PIO2;}
    if(x>0) return atan(y/x);
    return y<0?atan(y/x)-PI:atan(y/x)+PI;
}
double asin(double x) {if(fabs(x)>1) return domain(); return atan2(x,sqrt((1-x)*(1+x)));}
double acos(double x) {if(fabs(x)>1) return domain(); return atan2(sqrt((1-x)*(1+x)),x);}
double sinh(double x) {
    double a=fabs(x),r;
    if(a<0.5) {double a2=a*a,term=a; int i; r=a; for(i=3;i<=21;i+=2) {term*=a2/((i-1)*i); r+=term;}}
    else {double e=exp(a); r=(e-1/e)/2;}
    return x<0?-r:r;
}
double cosh(double x) {double e=exp(fabs(x)); return (e+1/e)/2;}
double tanh(double x) {
    double a=fabs(x),r;
    if(a>22) r=1;
    else if(a<0.5) r=sinh(a)/cosh(a);
    else {double e=exp(2*a); r=(e-1)/(e+1);}
    return x<0?-r:r;
}
