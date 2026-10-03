/* SPDX-License-Identifier: GPL-2.0-or-later
 * The string functions win/sdk/crt.c does not have. C89. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#pragma function(strset)
char *strncat(char *d,const char *s,size_t n) {
    char *p=d+strlen(d);
    while(n-- && *s) *p++=*s++;
    *p=0; return d;
}
int strcoll(const char *a,const char *b) {return strcmp(a,b);}
size_t strxfrm(char *d,const char *s,size_t n) {
    size_t len=strlen(s);
    if(n) {strncpy(d,s,n); if(len>=n) d[n-1]=0;}
    return len;
}
size_t strspn(const char *s,const char *set) {size_t n=0; while(s[n] && strchr(set,s[n])) n++; return n;}
size_t strcspn(const char *s,const char *set) {size_t n=0; while(s[n] && !strchr(set,s[n])) n++; return n;}
char *strpbrk(const char *s,const char *set) {for(;*s;s++) if(strchr(set,*s)) return (char *)s; return NULL;}
char *strstr(const char *s,const char *t) {
    size_t n=strlen(t);
    if(!n) return (char *)s;
    for(;*s;s++) if(*s==*t && !strncmp(s,t,n)) return (char *)s;
    return NULL;
}
char *strtok(char *s,const char *set) {
    static char *next;
    char *start;
    if(!s) s=next;
    if(!s) return NULL;
    s+=strspn(s,set);
    if(!*s) {next=NULL; return NULL;}
    start=s; s+=strcspn(s,set);
    if(*s) {*s=0; next=s+1;} else next=NULL;
    return start;
}
char *strerror(int e) {
    switch(e) {
    case 0: return "No error";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file number";
    case ENOMEM: return "Not enough memory";
    case EACCES: return "Permission denied";
    case EEXIST: return "File exists";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOSPC: return "No space left on device";
    case EDOM: return "Math argument";
    case ERANGE: return "Result too large";
    default: return "Unknown error";
    }
}
void *memccpy(void *d,const void *s,int c,size_t n) {
    unsigned char *p=(unsigned char *)d; const unsigned char *q=(const unsigned char *)s;
    while(n--) {if((*p++=*q++)==(unsigned char)c) return p;}
    return NULL;
}
int memicmp(const void *a,const void *b,size_t n) {
    const unsigned char *p=(const unsigned char *)a,*q=(const unsigned char *)b;
    for(;n;n--,p++,q++) {int d=tolower(*p)-tolower(*q); if(d) return d;}
    return 0;
}
int stricmp(const char *a,const char *b) {
    for(;;a++,b++) {int d=tolower((unsigned char)*a)-tolower((unsigned char)*b); if(d || !*a) return d;}
}
int strnicmp(const char *a,const char *b,size_t n) {
    for(;n;n--,a++,b++) {int d=tolower((unsigned char)*a)-tolower((unsigned char)*b); if(d || !*a) return d;}
    return 0;
}
char *strdup(const char *s) {char *d=(char *)malloc(strlen(s)+1); if(d) strcpy(d,s); return d;}
char *strupr(char *s) {char *p; for(p=s;*p;p++) *p=(char)toupper((unsigned char)*p); return s;}
char *strlwr(char *s) {char *p; for(p=s;*p;p++) *p=(char)tolower((unsigned char)*p); return s;}
char *strrev(char *s) {
    size_t i=0,j=strlen(s);
    while(j>i+1) {char t=s[i]; s[i]=s[j-1]; s[j-1]=t; i++; j--;}
    return s;
}
char *strset(char *s,int c) {char *p; for(p=s;*p;p++) *p=(char)c; return s;}
char *strnset(char *s,int c,size_t n) {char *p; for(p=s;*p && n;p++,n--) *p=(char)c; return s;}
