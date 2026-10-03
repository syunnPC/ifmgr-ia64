/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Shared pieces of the file utilities; see util.h. The parser and extended
 * error texts are v4.0 MESSAGES/USA-MS.MSG's (classes PARSE and EXTEND).
 */
#include "util.h"

static void stderr_sink(const void *p,size_t n) {u32 done; dos_write(2,p,(u32)n,&done);}
void to_stderr(int on) {print_sink=on?stderr_sink:NULL;}
void append_files_only(void) {
    u32 was; if(!dos_append_task(~DOS_APPEND_X,&was)) dos_append_task(was&~DOS_APPEND_X,NULL);
}

static void message(const char *text,const char *arg) {
    to_stderr(1);
    if(arg && *arg) print("%s - %s\n",text,arg); else print("%s\n",text);
    to_stderr(0);
}
void parse_error(unsigned which,const char *arg) {
    static const char *const text[]={"","Too many parameters","Required parameter missing","Invalid switch",
        "Invalid keyword","","Parameter value not in allowed range","Parameter value not allowed",
        "Parameter value not allowed","Parameter format not correct","Invalid parameter","Invalid parameter combination"};
    message(which<ARRAY_SIZE(text) && text[which][0]?text[which]:"Invalid parameter",arg);
}
void extended_error(int e,const char *arg) {
    static const char *const text[]={"","Invalid function","File not found","Path not found","Too many open files",
        "Access denied","Invalid handle","Memory control blocks destroyed","Insufficient memory",
        "Invalid memory block address","Invalid Environment","Invalid format","Invalid function parameter",
        "Invalid data","","Invalid drive specification","Attempt to remove current directory","Not same device",
        "No more files","Write protect error","Invalid unit","Not ready","Invalid device request","Data error",
        "Invalid device request parameters","Seek error","Invalid media type","Sector not found",
        "Printer out of paper error","Write fault error","Read fault error","General failure","Sharing violation",
        "Lock violation","Invalid disk change"};
    message(e>0 && (unsigned)e<ARRAY_SIZE(text) && text[e][0]?text[e]:dos_error(e),arg);
}

static int separator(char c) {return c==' ' || c=='\t' || c==',' || c==';' || c=='=';}
const char *next_arg(const char *p,char *out,unsigned cap) {
    unsigned n=0;
    while(separator(*p)) p++;
    if(!*p) return NULL;
    do {if(n+1<cap) out[n++]=*p; p++;} while(*p && !separator(*p) && *p!='/');
    out[n]=0;
    return p;
}

static int wild(const char *s) {for(;*s;s++) if(*s=='*' || *s=='?') return 1; return 0;}
static int end_slash(char *dir) {
    size_t n=strlen(dir);
    return n && dir[n-1]=='\\'?0:strappend(dir,DOS_PATH_MAX,"\\");
}
int file_spec(const char *spec,char dir[DOS_PATH_MAX],char name[13],int dir_ok) {
    char head[DOS_PATH_MAX]; const char *last=spec; size_t n; int e;
    for(const char *p=spec;*p;p++) if(*p=='\\' || *p=='/' || (p==spec+1 && *p==':')) last=p+1;
    n=(size_t)(last-spec);
    if(n>=sizeof(head) || strlen(last)>12) return DE_PATH;
    memcpy(head,spec,n); head[n]=0;
    if(!strcmp(last,".") || !strcmp(last,"..")) {
        e=dos_full_path(spec,dir); if(!e) e=end_slash(dir);
        if(!e) strcopy(name,13,"*.*");
        return e;
    }
    e=dos_full_path(n?head:".",dir); if(!e) e=end_slash(dir);
    if(e) return e;
    strcopy(name,13,*last?last:"*.*");
    for(char *p=name;*p;p++) *p=(char)upper(*p);
    if(dir_ok && *last && !wild(last)) {
        char full[DOS_PATH_MAX]; u8 attr=0;
        if(!strcopy(full,sizeof(full),dir) && !strappend(full,sizeof(full),name) &&
           !dos_attribute(full,0,&attr) && (attr&FA_DIR)) {
            e=strcopy(dir,DOS_PATH_MAX,full); if(!e) e=end_slash(dir);
            strcopy(name,13,"*.*");
        }
    }
    return e;
}

void map_name(const char *pattern,const char *name,char out[13]) {
    char fields[2][9]={{0},{0}},from[2][9]={{0},{0}}; unsigned n=0;
    for(unsigned k=0,f=0;name[k] && f<2;k++) {if(name[k]=='.') {f++; n=0; continue;} if(n<8) from[f][n++]=name[k];}
    n=0;
    for(unsigned k=0,f=0;pattern[k] && f<2;k++) {
        unsigned limit=f?3:8;
        if(pattern[k]=='.') {f++; n=0; continue;}
        if(pattern[k]=='*') {while(n<limit && from[f][n]) {fields[f][n]=from[f][n]; n++;} while(pattern[k+1] && pattern[k+1]!='.') k++; continue;}
        if(n<limit) {fields[f][n]=pattern[k]=='?'?from[f][n]:pattern[k]; n++;}
    }
    strcopy(out,13,fields[0]);
    if(fields[1][0]) {strappend(out,13,"."); strappend(out,13,fields[1]);}
}

static int walk_entries(char *path,const char *pattern,u8 attrs,WalkFn fn,void *ctx) {
    size_t base=strlen(path); DosFind f; int e,r;
    e=strappend(path,DOS_PATH_MAX,pattern); if(e) {path[base]=0; return e;}
    r=dos_find_first(path,attrs,&f); path[base]=0;
    for(;!r;r=dos_find_next(&f)) {
        if(f.name[0]=='.' || (f.attr&FA_VOLUME) || (f.attr&~attrs&(FA_DIR|FA_HIDDEN|FA_SYSTEM))) continue;
        e=fn(ctx,path,&f); if(e) return e;
    }
    return r==DE_NOMORE || r==DE_NOFILE?0:r;
}
static int walk_dir(char *path,const char *pattern,u8 attrs,int recurse,int first,WalkFn fn,void *ctx);
static int walk_subdirs(char *path,const char *pattern,u8 attrs,int first,WalkFn fn,void *ctx) {
    size_t base=strlen(path); DosFind f; int e,r;
    e=strappend(path,DOS_PATH_MAX,"*.*"); if(e) {path[base]=0; return e;}
    r=dos_find_first(path,FA_DIR|FA_HIDDEN|FA_SYSTEM,&f); path[base]=0;
    for(;!r;r=dos_find_next(&f)) {
        if(!(f.attr&FA_DIR) || f.name[0]=='.') continue;
        e=strappend(path,DOS_PATH_MAX,f.name); if(!e) e=strappend(path,DOS_PATH_MAX,"\\");
        if(!e) e=walk_dir(path,pattern,attrs,1,first,fn,ctx);
        path[base]=0;
        if(e) return e;
    }
    return r==DE_NOMORE || r==DE_NOFILE?0:r;
}
static int walk_dir(char *path,const char *pattern,u8 attrs,int recurse,int first,WalkFn fn,void *ctx) {
    int e=0;
    if(recurse && first) e=walk_subdirs(path,pattern,attrs,first,fn,ctx);
    if(!e) e=walk_entries(path,pattern,attrs,fn,ctx);
    if(!e && recurse && !first) e=walk_subdirs(path,pattern,attrs,first,fn,ctx);
    return e;
}
int walk(const char *dir,const char *pattern,u8 attrs,int recurse,int subdirs_first,WalkFn fn,void *ctx) {
    char path[DOS_PATH_MAX]; int e=strcopy(path,sizeof(path),dir);
    return e?e:walk_dir(path,pattern,attrs,recurse,subdirs_first,fn,ctx);
}

int copy_file(const char *from,const char *to) {
    static u8 *buffer; unsigned in,out,result; u32 got,done; u16 date,time; int e,c;
    if(!buffer && dos_alloc(32768/16,(void **)&buffer)) {buffer=NULL; return DE_NOMEM;}
    e=dos_open(from,0,0,&in); if(e) return e;
    e=dos_open_ex(to,1,FA_ARCHIVE,0x12,&out,&result); if(e) {dos_close(in); return e;}
    while(!(e=dos_read(in,buffer,32768,&got)) && got) {
        e=dos_write(out,buffer,got,&done); if(!e && done!=got) e=DE_FULL;
        if(e) break;
    }
    if(!e) e=dos_file_time(in,0,&date,&time);
    if(!e) e=dos_file_time(out,1,&date,&time);
    dos_close(in); c=dos_close(out); if(!e) e=c;
    if(e) dos_remove(to,0);
    return e;
}
int make_dirs(const char *dir) {
    char path[DOS_PATH_MAX]; size_t n; u8 attr; int e=strcopy(path,sizeof(path),dir);
    if(e) return e;
    n=strlen(path); if(n>3 && path[n-1]=='\\') path[--n]=0;
    if(n<=3 || (!dos_attribute(path,0,&attr) && (attr&FA_DIR))) return 0;
    char *last=path+n; while(last>path+2 && *last!='\\') last--;
    if(last>path+2) {*last=0; e=make_dirs(path); *last='\\'; if(e) return e;}
    return dos_mkdir(path);
}

void lines_open(LineReader *r,unsigned handle) {r->handle=handle; r->at=r->end=0; r->eof=0;}
int lines_next(LineReader *r,char *out,unsigned cap,unsigned *length) {
    unsigned n=0; int any=0;
    for(;;) {
        if(r->at==r->end) {
            u32 got=0;
            if(r->eof || dos_read(r->handle,r->data,sizeof(r->data),&got) || !got) {r->eof=1; break;}
            r->at=0; r->end=got;
        }
        u8 c=r->data[r->at++];
        /* Ctrl+Z ends text, as for DOS's own filters. */
        if(c==0x1a) {r->eof=1; r->at=r->end; break;}
        any=1;
        if(c=='\n') break;
        if(n+1<cap) out[n++]=(char)c;
    }
    if(n && out[n-1]=='\r') n--;
    if(cap) out[n]=0;
    if(length) *length=n;
    return any;
}
int parse_date(const char *s,u16 *out) {
    DosCountryInfo info; unsigned part[3]={0,0,0},n=0,digits=0;
    if(dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,&info)) info.date_order=0;
    for(;;s++) {
        if(*s>='0' && *s<='9') {part[n]=part[n]*10+(unsigned)(*s-'0'); if(++digits>4) return 0; continue;}
        if(!digits) return 0;
        if(!*s) break;
        if((*s!='-' && *s!='/' && *s!='.') || n==2) return 0;
        n++; digits=0;
    }
    if(n!=2) return 0;
    unsigned month=part[0],day=part[1],year=part[2];
    if(info.date_order==1) {day=part[0]; month=part[1];}
    else if(info.date_order==2) {year=part[0]; month=part[1]; day=part[2];}
    if(year<80) year+=2000; else if(year<100) year+=1900;
    if(year<1980 || year>2107 || !month || month>12 || !day || day>31) return 0;
    *out=(u16)(((year-1980)<<9)|(month<<5)|day);
    return 1;
}
int parse_time(const char *s,u16 *out) {
    DosCountryInfo info; unsigned part[3]={0,0,0},n=0,digits=0;
    if(dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,&info)) info.time_separator[0]=':';
    for(;;s++) {
        if(*s>='0' && *s<='9') {part[n]=part[n]*10+(unsigned)(*s-'0'); if(++digits>2) return 0; continue;}
        if(!digits) return 0;
        if(!*s) break;
        if((*s!=':' && *s!=(char)info.time_separator[0]) || n==2) return 0;
        n++; digits=0;
    }
    if(!n || part[0]>23 || part[1]>59 || part[2]>59) return 0;
    *out=(u16)((part[0]<<11)|(part[1]<<5)|(part[2]/2));
    return 1;
}
