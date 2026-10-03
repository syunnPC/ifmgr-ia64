/* SPDX-License-Identifier: GPL-2.0-or-later
 * printf's formatting, as Microsoft C's: flags - + space # 0, width and
 * precision (also *), sizes h l L I64 ll and the far/near F N of Windows 3.0
 * programs (ignored), conversions d i u o x X c s p n e E f g G %; exponents
 * have three digits. C89. */
#include <string.h>
#include <stdarg.h>
#include "crtp.h"
#include "fp.h"
#define FLOAT_DIGITS 360
static int out(_Sink *s,const char *p,unsigned n) {
    if(!n) return 0;
    if(s->put(s->context,p,n)<0) return -1;
    s->count+=n; return 0;
}
static int pad(_Sink *s,char c,int n) {
    char b[32]; memset(b,c,sizeof(b));
    while(n>0) {int k=n>32?32:n; if(out(s,b,(unsigned)k)) return -1; n-=k;}
    return 0;
}
/* A converted field: sign or prefix, zeros after it, the body, padded to width. */
static int field(_Sink *s,const char *prefix,int zeros,const char *body,int len,int width,int left,int zero_pad) {
    int plen=(int)strlen(prefix),total=plen+zeros+len;
    if(zero_pad && !left && width>total) {zeros+=width-total; total=width;}
    if(!left && pad(s,' ',width-total)) return -1;
    if(out(s,prefix,(unsigned)plen) || pad(s,'0',zeros) || out(s,body,(unsigned)len)) return -1;
    if(left && pad(s,' ',width-total)) return -1;
    return 0;
}
static int number(_Sink *s,unsigned __int64 v,int negative,int base,int upper,int width,int precision,int left,int zero,int plus,int space,int alt) {
    char t[24],prefix[3]; int n=0; const char *dig=upper?"0123456789ABCDEF":"0123456789abcdef";
    while(v) {t[sizeof(t)-1-n++]=dig[v%(unsigned)base]; v/=(unsigned)base;}
    if(!n && precision) t[sizeof(t)-1-n++]='0';
    prefix[0]=0;
    if(negative) strcpy(prefix,"-"); else if(plus) strcpy(prefix,"+"); else if(space) strcpy(prefix," ");
    if(alt && base==16 && n && !(n==1 && t[sizeof(t)-1]=='0')) strcpy(prefix,upper?"0X":"0x");
    if(alt && base==8 && t[sizeof(t)-n]!='0') {if(precision<=n) precision=n+1;}
    return field(s,prefix,precision>n?precision-n:0,t+sizeof(t)-n,n,width,left,zero && precision<0);
}
/* e, f or g of a finite double into body; returns its length. */
static int floating(double x,char conv,int precision,int alt,char *body) {
    char d[FLOAT_DIGITS]; int point,len,n=0,i,exp10;
    char c=(char)(conv|0x20);
    if(precision<0) precision=6;
    if(c=='g') {
        if(!precision) precision=1;
        _fp_digits(x,FP_SIGNIFICANT,precision,d,&point);
        exp10=point-1;
        if(exp10<-4 || exp10>=precision) {c='e'; precision--;}
        else {c='f'; precision=precision-1-exp10;}
        if(precision>FLOAT_DIGITS-40) precision=FLOAT_DIGITS-40;
        n=floating(x,c=='e'?(char)(conv=='G'?'E':'e'):'f',precision,1,body);
        if(!alt && strchr(body,'.')) {
            /* No trailing zeros nor a lone point, before any exponent. */
            char *e=strpbrk(body,"eE"),tail[8]; int end=e?(int)(e-body):n;
            tail[0]=0; if(e) strcpy(tail,e);
            while(end>0 && body[end-1]=='0') end--;
            if(end>0 && body[end-1]=='.') end--;
            strcpy(body+end,tail); n=(int)strlen(body);
        }
        return n;
    }
    if(precision>FLOAT_DIGITS-40) precision=FLOAT_DIGITS-40;
    if(c=='e') {
        len=_fp_digits(x,FP_SIGNIFICANT,precision+1,d,&point);
        exp10=x==0?0:point-1;
        body[n++]=d[0];
        if(precision || alt) body[n++]='.';
        for(i=1;i<=precision;i++) body[n++]=i<len?d[i]:'0';
        body[n++]=conv=='E'?'E':'e';
        body[n++]=(char)(exp10<0?'-':'+'); if(exp10<0) exp10=-exp10;
        body[n++]=(char)('0'+exp10/100); body[n++]=(char)('0'+exp10/10%10); body[n++]=(char)('0'+exp10%10);
        body[n]=0; return n;
    }
    len=_fp_digits(x,FP_FIXED,precision,d,&point);
    if(point<=0) body[n++]='0';
    for(i=0;i<point;i++) body[n++]=i<len?d[i]:'0';
    if(precision || alt) body[n++]='.';
    for(i=0;i<precision;i++) {int k=point+i; body[n++]=k>=0 && k<len?d[k]:'0';}
    body[n]=0; return n;
}
int _format(_Sink *s,const char *f,va_list ap) {
    char body[FLOAT_DIGITS+40];
    s->count=0;
    for(;*f;f++) {
        int left=0,plus=0,space=0,alt=0,zero=0,width=0,precision=-1,size=0;
        const char *start=f;
        if(*f!='%') {
            const char *e=strchr(f,'%'); unsigned n=e?(unsigned)(e-f):(unsigned)strlen(f);
            if(out(s,f,n)) return -1;
            f+=n-1; continue;
        }
        for(f++;;f++) {
            if(*f=='-') left=1; else if(*f=='+') plus=1; else if(*f==' ') space=1;
            else if(*f=='#') alt=1; else if(*f=='0') zero=1; else break;
        }
        if(*f=='*') {width=va_arg(ap,int); if(width<0) {left=1; width=-width;} f++;}
        else while(*f>='0' && *f<='9') width=width*10+(*f++-'0');
        if(*f=='.') {
            f++; precision=0;
            if(*f=='*') {precision=va_arg(ap,int); f++;}
            else while(*f>='0' && *f<='9') precision=precision*10+(*f++-'0');
        }
        for(;;f++) {
            if(*f=='h') size='h'; else if(*f=='l') size=size=='l'?'q':'l'; else if(*f=='L') size='L';
            else if(*f=='F' || *f=='N') {} /* far and near: one kind of pointer */
            else if(f[0]=='I' && f[1]=='6' && f[2]=='4') {size='q'; f+=2;}
            else if(f[0]=='I' && f[1]=='3' && f[2]=='2') {size='l'; f+=2;}
            else if(*f=='I') size='p';
            else break;
        }
        switch(*f) {
        case 'd': case 'i': {
            __int64 v=size=='q'||size=='p'?va_arg(ap,__int64):size=='h'?(short)va_arg(ap,int):(__int64)va_arg(ap,int);
            if(number(s,v<0?(unsigned __int64)0-(unsigned __int64)v:(unsigned __int64)v,v<0,10,0,width,precision,left,zero,plus,space,0)) return -1;
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned __int64 v=size=='q'||size=='p'?va_arg(ap,unsigned __int64):size=='h'?(unsigned short)va_arg(ap,unsigned):(unsigned __int64)va_arg(ap,unsigned);
            if(number(s,v,0,*f=='u'?10:*f=='o'?8:16,*f=='X',width,precision,left,zero,0,0,alt)) return -1;
            break;
        }
        case 'p': {
            void *p=va_arg(ap,void *);
            if(number(s,(unsigned __int64)(size_t)p,0,16,1,width,16,left,0,0,0,0)) return -1;
            break;
        }
        case 'c': {char c=(char)va_arg(ap,int); if(field(s,"",0,&c,1,width,left,0)) return -1; break;}
        case 's': {
            const char *t=va_arg(ap,const char *); int n=0;
            if(!t) t="(null)";
            while(t[n] && (precision<0 || n<precision)) n++;
            if(field(s,"",0,t,n,width,left,0)) return -1;
            break;
        }
        case 'n': {int *p=va_arg(ap,int *); if(p) *p=(int)s->count; break;}
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double x=va_arg(ap,double); const char *sign=x<0?"-":plus?"+":space?" ":""; int n,k;
            k=_fp_class(x);
            if(k!=FP_FINITE) {
                strcpy(body,k==FP_INF?"1.#INF":"1.#QNAN");
                if(field(s,sign,0,body,(int)strlen(body),width,left,0)) return -1;
                break;
            }
            n=floating(x<0?-x:x,*f,precision,alt,body);
            if(field(s,sign,0,body,n,width,left,zero)) return -1;
            break;
        }
        case '%': if(out(s,"%",1)) return -1; break;
        default: /* not a conversion: printed as it is */
            if(!*f) f--;
            if(out(s,start,(unsigned)(f-start+1))) return -1;
        }
    }
    return (int)s->count;
}

typedef struct {char *p; size_t room;} Buffer;
static int to_buffer(void *context,const char *p,unsigned n) {
    Buffer *b=(Buffer *)context; unsigned k=n;
    if(b->room!=(size_t)-1) {if(k>b->room) k=(unsigned)b->room; b->room-=k;}
    memcpy(b->p,p,k); b->p+=k; return 0;
}
int vsprintf(char *out,const char *f,va_list ap) {
    Buffer b; _Sink s; int n;
    b.p=out; b.room=(size_t)-1; s.put=to_buffer; s.context=&b;
    n=_format(&s,f,ap); *b.p=0; return n;
}
int _vsnprintf(char *out,size_t size,const char *f,va_list ap) {
    Buffer b; _Sink s; int n;
    b.p=out; b.room=size; s.put=to_buffer; s.context=&b;
    n=_format(&s,f,ap);
    if(n<0 || (size_t)n>=size) {if((size_t)n<size) *b.p=0; return (size_t)n==size?n:-1;}
    *b.p=0; return n;
}
int sprintf(char *out,const char *f,...) {va_list ap; int n; va_start(ap,f); n=vsprintf(out,f,ap); va_end(ap); return n;}
int _snprintf(char *out,size_t size,const char *f,...) {va_list ap; int n; va_start(ap,f); n=_vsnprintf(out,size,f,ap); va_end(ap); return n;}
