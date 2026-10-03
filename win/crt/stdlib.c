/* SPDX-License-Identifier: GPL-2.0-or-later
 * General utilities: conversions, random numbers, sorting, the
 * environment, ending the program, paths. C89. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <winstart.h>
#include "crtp.h"
#pragma function(abs,labs,_rotl,_rotr)
int errno;
int abs(int n) {return n<0?-n:n;}
long labs(long n) {return n<0?-n:n;}
div_t div(int a,int b) {div_t r; r.quot=a/b; r.rem=a%b; return r;}
ldiv_t ldiv(long a,long b) {ldiv_t r; r.quot=a/b; r.rem=a%b; return r;}
unsigned int _rotl(unsigned int v,int n) {n&=31; return n?(v<<n)|(v>>(32-n)):v;}
unsigned int _rotr(unsigned int v,int n) {n&=31; return n?(v>>n)|(v<<(32-n)):v;}

static unsigned long seed=1;
int rand(void) {seed=seed*1103515245UL+12345UL; return (int)((seed>>16)&RAND_MAX);}
void srand(unsigned s) {seed=s;}

/* An unsigned number in a base (0: by its prefix), its sign before it. */
static unsigned __int64 parse(const char *s,char **end,int base,int *negative,int *overflow,unsigned __int64 limit) {
    const char *p=s; unsigned __int64 v=0; int any=0;
    *negative=0; *overflow=0;
    while(isspace((unsigned char)*p)) p++;
    if(*p=='-' || *p=='+') *negative=*p++=='-';
    if((!base || base==16) && p[0]=='0' && (p[1]=='x' || p[1]=='X') && isxdigit((unsigned char)p[2])) {base=16; p+=2;}
    else if(!base) base=*p=='0'?8:10;
    for(;;p++) {
        int d=isdigit((unsigned char)*p)?*p-'0':isalpha((unsigned char)*p)?toupper((unsigned char)*p)-'A'+10:99;
        if(d>=base) break;
        any=1;
        if(v>(limit-(unsigned)d)/(unsigned)base) *overflow=1; else v=v*(unsigned)base+(unsigned)d;
    }
    if(end) *end=(char *)(any?p:s);
    return v;
}
long strtol(const char *s,char **end,int base) {
    int neg,over; unsigned __int64 v=parse(s,end,base,&neg,&over,(unsigned __int64)LONG_MAX+1);
    if(over || (!neg && v>LONG_MAX)) {errno=ERANGE; return neg?LONG_MIN:LONG_MAX;}
    return neg?(long)(0-v):(long)v;
}
unsigned long strtoul(const char *s,char **end,int base) {
    int neg,over; unsigned __int64 v=parse(s,end,base,&neg,&over,ULONG_MAX);
    if(over) {errno=ERANGE; return ULONG_MAX;}
    return neg?(unsigned long)(0-v):(unsigned long)v;
}
int atoi(const char *s) {return (int)strtol(s,NULL,10);}
long atol(const char *s) {return strtol(s,NULL,10);}
double atof(const char *s) {return strtod(s,NULL);}
static char *utoa(unsigned long v,char *s,int radix,int negative) {
    char t[34]; int n=0; char *p=s;
    if(radix<2 || radix>36) {*s=0; return s;}
    do {int d=(int)(v%(unsigned)radix); t[n++]=(char)(d<10?'0'+d:'a'+d-10); v/=(unsigned)radix;} while(v);
    if(negative) *p++='-';
    while(n) *p++=t[--n];
    *p=0; return s;
}
char *itoa(int v,char *s,int radix) {return radix==10 && v<0?utoa((unsigned long)-(long)v,s,10,1):utoa((unsigned)v,s,radix,0);}
char *ltoa(long v,char *s,int radix) {return radix==10 && v<0?utoa(0-(unsigned long)v,s,10,1):utoa((unsigned long)v,s,radix,0);}
char *ultoa(unsigned long v,char *s,int radix) {return utoa(v,s,radix,0);}

/* Shell sort: no recursion, no memory. */
void qsort(void *base,size_t n,size_t size,int (*cmp)(const void *,const void *)) {
    char *a=(char *)base; size_t gap,i,j,k;
    for(gap=n/2;gap;gap/=2) for(i=gap;i<n;i++) for(j=i;j>=gap && cmp(a+(j-gap)*size,a+j*size)>0;j-=gap)
        for(k=0;k<size;k++) {char t=a[(j-gap)*size+k]; a[(j-gap)*size+k]=a[j*size+k]; a[j*size+k]=t;}
}
void *bsearch(const void *key,const void *base,size_t n,size_t size,int (*cmp)(const void *,const void *)) {
    size_t lo=0,hi=n;
    while(lo<hi) {
        size_t mid=(lo+hi)/2; const char *e=(const char *)base+mid*size; int c=cmp(key,e);
        if(!c) return (void *)e;
        if(c<0) hi=mid; else lo=mid+1;
    }
    return NULL;
}

/* The environment: the task's DOS environment, with putenv's changes in
 * an array of our own. */
#define ENV_MAX 64
static char *added[ENV_MAX];
static int name_is(const char *entry,const char *name,size_t n) {return !strnicmp(entry,name,n) && entry[n]=='=';}
char *getenv(const char *name) {
    size_t n=strlen(name); int i; const char *e;
    for(i=0;i<ENV_MAX;i++) if(added[i] && name_is(added[i],name,n)) return added[i][n+1]?added[i]+n+1:NULL;
    for(e=GetDOSEnvironment();e && *e;e+=strlen(e)+1) if(name_is(e,name,n)) return (char *)e+n+1;
    return NULL;
}
int putenv(const char *entry) {
    const char *eq=strchr(entry,'='); size_t n; int i,free_slot=-1;
    if(!eq) return -1;
    n=(size_t)(eq-entry);
    for(i=0;i<ENV_MAX;i++) {
        if(added[i] && name_is(added[i],entry,n)) {free(added[i]); added[i]=strdup(entry); return added[i]?0:-1;}
        if(!added[i] && free_slot<0) free_slot=i;
    }
    if(free_slot<0) return -1;
    added[free_slot]=strdup(entry); return added[free_slot]?0:-1;
}
int system(const char *command) {
    char line[260];
    if(!command) return 1;
    if(strlen(command)+10>sizeof(line)) return -1;
    strcpy(line,"COMMAND /C "); strcat(line,command);
    return WinExec(line,SW_SHOWNORMAL)<32?-1:0;
}

#define EXITS 32
static void (*exits[EXITS])(void);
static int exit_count;
int atexit(void (*f)(void)) {if(exit_count==EXITS) return -1; exits[exit_count++]=f; return 0;}
/* The program's end, from exit or WinMainCRTStartup: atexit's functions
 * in reverse, then the streams flushed. */
void _crt_term(void) {
    while(exit_count) exits[--exit_count]();
    _crt_flush_all();
}
void exit(int code) {_crt_term(); ExitProcess((UINT)code);}
void _exit(int code) {ExitProcess((UINT)code);}
void abort(void) {FatalAppExit(0,"Abnormal program termination");}
void _assert(const char *expression,const char *file,unsigned line) {
    char text[300];
    wsprintf(text,"Assertion failed: %.120s, file %.120s, line %u",expression,file,line);
    FatalAppExit(0,text);
}

/* Paths: drive, directory, name and extension. */
static void part(char *out,const char *from,const char *to,size_t max) {
    size_t n;
    if(!out) return;
    n=(size_t)(to-from); if(n>=max) n=max-1;
    memcpy(out,from,n); out[n]=0;
}
void _splitpath(const char *path,char *drive,char *dir,char *name,char *ext) {
    const char *p=path,*slash=NULL,*dot=NULL,*s;
    if(p[0] && p[1]==':') {part(drive,p,p+2,_MAX_DRIVE); p+=2;} else part(drive,p,p,_MAX_DRIVE);
    for(s=p;*s;s++) if(*s=='\\' || *s=='/') slash=s;
    part(dir,p,slash?slash+1:p,_MAX_DIR);
    s=slash?slash+1:p;
    for(dot=NULL;*s;s++) if(*s=='.') dot=s;
    s=slash?slash+1:p;
    part(name,s,dot?dot:s+strlen(s),_MAX_FNAME);
    part(ext,dot?dot:s+strlen(s),s+strlen(s),_MAX_EXT);
}
void _makepath(char *path,const char *drive,const char *dir,const char *name,const char *ext) {
    *path=0;
    if(drive && *drive) {strncat(path,drive,1); strcat(path,":");}
    if(dir && *dir) {strcat(path,dir); if(dir[strlen(dir)-1]!='\\' && dir[strlen(dir)-1]!='/') strcat(path,"\\");}
    if(name) strcat(path,name);
    if(ext && *ext) {if(*ext!='.') strcat(path,"."); strcat(path,ext);}
}
char *_fullpath(char *out,const char *path,size_t size) {
    char full[_MAX_PATH],*p;
    if(path[0] && path[1]==':' && (path[2]=='\\' || path[2]=='/')) {if(strlen(path)>=sizeof(full)) return NULL; strcpy(full,path);}
    else {
        DWORD n=GetCurrentDirectory(sizeof(full),full);
        if(!n || n>=sizeof(full)) return NULL;
        if(path[0]=='\\' || path[0]=='/') full[2]=0;
        else if(path[0] && path[1]==':') {full[0]=path[0]; full[2]=0; path+=2; if(*path) strcat(full,"\\");}
        else if(full[strlen(full)-1]!='\\') strcat(full,"\\");
        if(strlen(full)+strlen(path)>=sizeof(full)) return NULL;
        strcat(full,path);
    }
    for(p=full;*p;p++) if(*p=='/') *p='\\';
    if(!out) {out=(char *)malloc(strlen(full)+1); if(!out) return NULL; size=strlen(full)+1;}
    if(strlen(full)>=size) return NULL;
    return strcpy(out,full);
}
