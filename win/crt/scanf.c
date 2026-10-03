/* SPDX-License-Identifier: GPL-2.0-or-later
 * scanf's conversions: d i u o x X c s [ e f g E G n p %, * to skip one,
 * a width, the sizes h l L I64 and the far/near F N. Returns how many were
 * assigned, EOF when the input ends before the first. C89. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include "crtp.h"
static int next(_Source *s,int *count) {int c=s->get(s->context); if(c!=EOF) (*count)++; return c;}
static void back(_Source *s,int c,int *count) {if(c!=EOF) {s->unget(s->context,c); (*count)--;}}
int _scan(_Source *s,const char *f,va_list ap) {
    int assigned=0,count=0,c,ended=0;
    for(;*f;f++) {
        int skip=0,width=0,size=0;
        if(isspace((unsigned char)*f)) {
            do c=next(s,&count); while(isspace(c));
            back(s,c,&count); continue;
        }
        if(*f!='%' || f[1]=='%') {
            if(*f=='%') f++;
            c=next(s,&count);
            if(c!=(unsigned char)*f) {back(s,c,&count); if(c==EOF) ended=1; break;}
            continue;
        }
        f++;
        if(*f=='*') {skip=1; f++;}
        while(isdigit((unsigned char)*f)) width=width*10+(*f++-'0');
        for(;;f++) {
            if(*f=='h') size='h'; else if(*f=='l') size=size=='l'?'q':'l'; else if(*f=='L') size='L';
            else if(*f=='F' || *f=='N') {}
            else if(f[0]=='I' && f[1]=='6' && f[2]=='4') {size='q'; f+=2;}
            else break;
        }
        if(*f=='n') {if(!skip) *va_arg(ap,int *)=count; continue;}
        if(*f!='c' && *f!='[') {
            do c=next(s,&count); while(isspace(c));
            back(s,c,&count);
        }
        if(*f=='c') {
            char *out=skip?NULL:va_arg(ap,char *); int i;
            if(!width) width=1;
            for(i=0;i<width;i++) {c=next(s,&count); if(c==EOF) break; if(out) out[i]=(char)c;}
            if(!i) {ended=1; break;}
            if(!skip) assigned++;
            continue;
        }
        if(*f=='s' || *f=='[') {
            char set[256],*out=skip?NULL:va_arg(ap,char *); int i=0,negate=0,k;
            memset(set,0,sizeof(set));
            if(*f=='[') {
                f++;
                if(*f=='^') {negate=1; f++;}
                if(*f==']') {set[']']=1; f++;}
                for(;*f && *f!=']';f++) {
                    if(f[1]=='-' && f[2] && f[2]!=']') {for(k=(unsigned char)f[0];k<=(unsigned char)f[2];k++) set[k]=1; f+=2;}
                    else set[(unsigned char)*f]=1;
                }
                if(negate) for(k=0;k<256;k++) set[k]=(char)!set[k];
            } else {for(k=0;k<256;k++) set[k]=(char)!isspace(k);}
            if(!width) width=0x7fffffff;
            while(i<width) {c=next(s,&count); if(c==EOF || !set[c]) {back(s,c,&count); break;} if(out) out[i]=(char)c; i++;}
            if(!i) {ended=c==EOF; break;}
            if(out) {out[i]=0; assigned++;}
            continue;
        }
        if(strchr("diuoxXp",*f)) {
            char t[72]; int i=0,base=*f=='d'||*f=='u'?10:*f=='o'?8:*f=='i'?0:16; __int64 v; char *end;
            if(!width || width>70) width=70;
            c=next(s,&count);
            if(c=='-' || c=='+') {t[i++]=(char)c; c=next(s,&count);}
            if(c=='0' && i<width && (base==0 || base==16)) {
                t[i++]=(char)c; c=next(s,&count);
                if((c=='x' || c=='X') && i<width) {t[i++]=(char)c; c=next(s,&count); if(!base) base=16;}
                else if(!base) base=8;
            }
            if(!base) base=10;
            while(i<width && c!=EOF && (base==16?isxdigit(c):base==8?c>='0'&&c<='7':isdigit(c))) {t[i++]=(char)c; c=next(s,&count);}
            back(s,c,&count); t[i]=0;
            if(!i || (i==1 && (t[0]=='-' || t[0]=='+'))) {ended=c==EOF; break;}
            v=(__int64)strtoul(t[0]=='-'||t[0]=='+'?t+1:t,&end,base);
            if(t[0]=='-') v=-v;
            if(!skip) {
                if(*f=='p') *va_arg(ap,void **)=(void *)(size_t)v;
                else if(size=='h') *va_arg(ap,short *)=(short)v;
                else if(size=='q') *va_arg(ap,__int64 *)=v;
                else *va_arg(ap,int *)=(int)v;
                assigned++;
            }
            continue;
        }
        if(strchr("efgEG",*f)) {
            char t[72]; int i=0; double v;
            if(!width || width>70) width=70;
            c=next(s,&count);
            if(c=='-' || c=='+') {t[i++]=(char)c; c=next(s,&count);}
            while(i<width && isdigit(c)) {t[i++]=(char)c; c=next(s,&count);}
            if(i<width && c=='.') {t[i++]=(char)c; c=next(s,&count); while(i<width && isdigit(c)) {t[i++]=(char)c; c=next(s,&count);}}
            if(i<width && (c=='e' || c=='E')) {
                t[i++]=(char)c; c=next(s,&count);
                if(i<width && (c=='-' || c=='+')) {t[i++]=(char)c; c=next(s,&count);}
                while(i<width && isdigit(c)) {t[i++]=(char)c; c=next(s,&count);}
            }
            back(s,c,&count); t[i]=0;
            if(!i) {ended=c==EOF; break;}
            v=strtod(t,NULL);
            if(!skip) {
                if(size=='l' || size=='L') *va_arg(ap,double *)=v; else *va_arg(ap,float *)=(float)v;
                assigned++;
            }
            continue;
        }
        break;
    }
    return !assigned && ended?EOF:assigned;
}
typedef struct {const char *p;} Text;
static int from_text(void *context) {Text *t=(Text *)context; return *t->p?(unsigned char)*t->p++:EOF;}
static void back_to_text(void *context,int c) {Text *t=(Text *)context; (void)c; t->p--;}
int sscanf(const char *in,const char *format,...) {
    _Source s; Text t; va_list ap; int n;
    t.p=in; s.get=from_text; s.unget=back_to_text; s.context=&t;
    va_start(ap,format); n=_scan(&s,format,ap); va_end(ap); return n;
}
