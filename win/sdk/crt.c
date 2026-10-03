/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compiler support for WDK-built (cl, IA-64) modules, which link with
 * /NODEFAULTLIB: no Microsoft runtime library is used. C89.
 */
#include <string.h>
#pragma function(memset,memcpy,memcmp,strlen,strcpy,strcat,strcmp)

/* Marks floating-point use; the CRT normally defines it. */
int _fltused=0x9875;

void *memset(void *d,int c,size_t n) {
    unsigned char *p=(unsigned char *)d;
    while(n--) *p++=(unsigned char)c;
    return d;
}
void *memcpy(void *d,const void *s,size_t n) {
    unsigned char *p=(unsigned char *)d; const unsigned char *q=(const unsigned char *)s;
    while(n--) *p++=*q++;
    return d;
}
void *memmove(void *d,const void *s,size_t n) {
    unsigned char *p=(unsigned char *)d; const unsigned char *q=(const unsigned char *)s;
    if(p<q) while(n--) *p++=*q++;
    else while(n--) p[n]=q[n];
    return d;
}
int memcmp(const void *a,const void *b,size_t n) {
    const unsigned char *p=(const unsigned char *)a,*q=(const unsigned char *)b;
    for(;n;n--,p++,q++) if(*p!=*q) return *p<*q?-1:1;
    return 0;
}

void *memchr(const void *p,int c,size_t n) {
    const unsigned char *q=(const unsigned char *)p;
    for(;n;n--,q++) if(*q==(unsigned char)c) return (void *)q;
    return 0;
}
size_t strlen(const char *s) {const char *p=s; while(*p) p++; return (size_t)(p-s);}
char *strcpy(char *d,const char *s) {char *p=d; while((*p++=*s++)!=0) {} return d;}
char *strncpy(char *d,const char *s,size_t n) {
    char *p=d;
    for(;n && *s;n--) *p++=*s++;
    for(;n;n--) *p++=0;
    return d;
}
char *strcat(char *d,const char *s) {strcpy(d+strlen(d),s); return d;}
int strcmp(const char *a,const char *b) {
    for(;*a && *a==*b;a++,b++) {}
    return (unsigned char)*a-(unsigned char)*b;
}
int strncmp(const char *a,const char *b,size_t n) {
    for(;n && *a && *a==*b;n--,a++,b++) {}
    return n?(unsigned char)*a-(unsigned char)*b:0;
}
char *strchr(const char *s,int c) {
    for(;;s++) {if(*s==(char)c) return (char *)s; if(!*s) return 0;}
}
char *strrchr(const char *s,int c) {
    const char *found=0;
    for(;;s++) {if(*s==(char)c) found=s; if(!*s) return (char *)found;}
}

/* /GS stack cookies. There is no SEH dispatcher, so the handler that
 * unwind data names is never called; a damaged cookie stops here. */
unsigned __int64 __security_cookie=0x00002b992ddfa232;
void __security_check_cookie(unsigned __int64 cookie) {
    if(cookie!=__security_cookie) for(;;) __debugbreak();
}
int __GSHandlerCheck(void *record,void *frame,void *context,void *dispatch) {
    (void)record; (void)frame; (void)context; (void)dispatch;
    return 1; /* ExceptionContinueSearch */
}
