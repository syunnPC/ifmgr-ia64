/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base.h"
void *memcpy(void *d, const void *s, size_t n) {
    u8 *p=d; const u8 *q=s; while(n--) *p++=*q++; return d;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *p=d; const u8 *q=s;
    if ((uintptr_t)p < (uintptr_t)q) return memcpy(d,s,n);
    while(n) { --n; p[n]=q[n]; } return d;
}
void *memset(void *d, int v, size_t n) {
    u8 *p=d; while(n--) *p++=(u8)v; return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *p=a,*q=b; while(n--) { if(*p!=*q) return *p-*q; p++; q++; } return 0;
}
size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
char upper(char c) { return c>='a' && c<='z' ? c-32 : c; }
int strcmp(const char *a,const char *b) {
    while(*a && *a==*b) { a++; b++; } return (u8)*a-(u8)*b;
}
int stricmp(const char *a,const char *b) {
    while(*a && upper(*a)==upper(*b)) { a++; b++; } return (u8)upper(*a)-(u8)upper(*b);
}
char *strchr(const char *s,int c) {
    do { if(*s==c) return (char *)s; } while(*s++); return NULL;
}
int strcopy(char *d,size_t cap,const char *s) {
    size_t n=strlen(s); if(n>=cap) return DE_PATH; memcpy(d,s,n+1); return 0;
}
int strappend(char *d,size_t cap,const char *s) {
    size_t n=strlen(d); if(n>=cap) return DE_PATH; return strcopy(d+n,cap-n,s);
}
u16 rd16(const void *p) { const u8 *b=p; return b[0]|((u16)b[1]<<8); }
u32 rd32(const void *p) { const u8 *b=p; return rd16(b)|((u32)rd16(b+2)<<16); }
void wr16(void *p,u16 v) { u8 *b=p; b[0]=v; b[1]=v>>8; }
void wr32(void *p,u32 v) { u8 *b=p; wr16(b,v); wr16(b+2,v>>16); }
void (*print_sink)(const void *,size_t);
static void print_write(const void *data,size_t size) {
    if(print_sink) print_sink(data,size); else con_write(data,size);
}
static void number(u64 n,unsigned base,unsigned width,char pad) {
    char b[32]; unsigned i=0; do { b[i++]="0123456789ABCDEF"[n%base]; n/=base; } while(n);
    while(width>i) { print_write(&pad,1); width--; }
    while(i) print_write(&b[--i],1);
}
/* All numeric arguments are unsigned long long; no libc or varargs ABI shim. */
void print(const char *fmt,...) {
    va_list ap; va_start(ap,fmt);
    while(*fmt) {
        if(*fmt!='%') { print_write(fmt++,1); continue; }
        fmt++; unsigned w=0; char pad=' ';
        if(*fmt=='0') {pad='0'; fmt++;}
        while(*fmt>='0'&&*fmt<='9') w=w*10+(*fmt++-'0');
        char c=*fmt; if(!c) break; fmt++;
        if(c=='s') { const char *s=va_arg(ap,const char *); print_write(s,strlen(s)); }
        else if(c=='c') {char t=(char)va_arg(ap,int); print_write(&t,1);}
        else if(c=='u'||c=='x') number(va_arg(ap,unsigned long long),c=='x'?16:10,w,pad);
        else print_write(&c,1);
    }
    va_end(ap);
}
const char *dos_error(int e) {
    switch(e) {
    case 0: return "Success"; case DE_FUNCTION: return "Invalid function";
    case DE_NOFILE: return "File not found"; case DE_PATH: return "Invalid path";
    case DE_HANDLES: return "Too many open files"; case DE_ACCESS: return "Access denied";
    case DE_HANDLE: return "Invalid handle"; case DE_ARENA: return "Arena damaged";
    case DE_NOMEM: return "Insufficient memory"; case DE_BLOCK: return "Invalid memory block";
    case DE_ENV: return "Invalid or full environment";
    case DE_MODE: return "Invalid access mode";
    case DE_DATA: return "Invalid data";
    case DE_FORMAT: return "Unsupported executable format (IA-64 PE32+ required)";
    case DE_DRIVE: return "Invalid drive"; case DE_CURRENT: return "Current directory";
    case DE_NOTSAME: return "Not the same drive"; case DE_CHANGED: return "Media changed";
    case DE_NOMORE: return "No more files"; case DE_READONLY: return "Write protected";
    case DE_SEEK: return "Invalid seek"; case DE_FULL: return "Disk full";
    case DE_SHARE: return "Sharing violation"; case DE_LOCK: return "Lock violation";
    case DE_LOCKS: return "Sharing buffer exceeded";
    case DE_EOF: return "End of file"; case DE_BREAK: return "Interrupted";
    case DE_CRITICAL: return "Critical error handler failure";
    case DE_EXISTS: return "File already exists"; default: return "Disk I/O error";
    case DE_BUSY: return "DOS is busy"; case DE_NOTREADY: return "Not ready";
    }
}
