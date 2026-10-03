/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos.h"
#include "config.h"
static char *strip(char *p) {
    while(*p==' ' || *p=='\t') p++;
    size_t n=strlen(p);
    while(n && (p[n-1]==' ' || p[n-1]=='\t')) p[--n]=0;
    return p;
}
static int country_line(DosConfig *c,char *value) {
    u16 numbers[2]={0,0}; char full[DOS_PATH_MAX];
    for(unsigned i=0;i<2;i++) {
        value=strip(value); unsigned n=0; char *start=value;
        while(*value>='0' && *value<='9') {
            n=n*10+(*value++-'0'); if(n>=65535) return DE_FUNCTION;
        }
        if(!i && (value==start || !n)) return DE_FUNCTION;
        numbers[i]=n; value=strip(value);
        if(!*value) break;
        if(*value++!=',') return DE_FUNCTION;
    }
    value=strip(value);
    if(!*value) value="C:\\COUNTRY.SYS";
    else if(*value=='"') {
        value++; size_t n=strlen(value);
        if(!n || value[n-1]!='"') return DE_PATH;
        value[n-1]=0;
    }
    int e=dos_canonical(value,full); if(e) return e;
    c->country=numbers[0]; c->code_page=numbers[1];
    strcopy(c->country_file,sizeof(c->country_file),full); return 0;
}
void config_defaults(DosConfig *c) {
    memset(c,0,sizeof(*c)); c->files=DOS_DEFAULT_FILES;
    strcopy(c->shell,sizeof(c->shell),"C:\\COMMAND.COM");
    strcopy(c->tail,sizeof(c->tail),"/P");
}
int config_line(DosConfig *c,char *line) {
    char *key=strip(line); if(!*key || *key==';') return 0;
    char *value=key;
    while(*value && *value!='=' && *value!=' ' && *value!='\t' && *value!=';') value++;
    if(*value) *value++=0;
    if(!stricmp(key,"REM")) return 0;
    value=strip(value); if(*value=='=') value=strip(value+1);
    if(!stricmp(key,"COUNTRY")) return country_line(c,value);
    if(!stricmp(key,"BREAK")) {
        if(!stricmp(value,"ON")) c->break_check=1;
        else if(!stricmp(value,"OFF")) c->break_check=0;
        else return DE_FUNCTION;
        return 0;
    }
    if(!stricmp(key,"FILES")) {
        unsigned n=0; char *p=value;
        if(!*p) return DE_FUNCTION;
        while(*p>='0' && *p<='9') {
            n=n*10+(*p++-'0'); if(n>DOS_MAX_FILES) return DE_FUNCTION;
        }
        if(*p || n<8) return DE_FUNCTION;
        c->files=n; return 0;
    }
    if(!stricmp(key,"SHELL") || !stricmp(key,"DEVICE") || !stricmp(key,"INSTALL")) {
        int install=!stricmp(key,"INSTALL"),device=install || !stricmp(key,"DEVICE");
        if(device && c->device_count==DOS_CONFIG_DEVICES) return DE_NOMEM;
        char *name=value,*tail=value;
        if(*name=='"') {
            name=++tail; while(*tail && *tail!='"') tail++;
            if(!*tail) return DE_PATH;
            *tail++=0;
            if(*tail && *tail!=' ' && *tail!='\t') return DE_PATH;
        } else {
            while(*tail && *tail!=' ' && *tail!='\t') tail++;
            if(*tail) *tail++=0;
        }
        tail=strip(tail); if(!*name || strlen(tail)>=sizeof(c->tail)) return DE_ENV;
        char full[DOS_PATH_MAX];
        int e=dos_canonical(name,full); if(e) return e;
        if(device) {
            DosConfigDevice *d=&c->devices[c->device_count++];
            strcopy(d->path,sizeof(d->path),full); strcopy(d->tail,sizeof(d->tail),tail); d->line=0; d->install=install;
        } else {strcopy(c->shell,sizeof(c->shell),full); strcopy(c->tail,sizeof(c->tail),tail);}
        return 0;
    }
    return DE_FUNCTION;
}
static int country_directive(const char *p) {
    while(*p==' ' || *p=='\t') p++;
    const char *key="COUNTRY";
    while(*key) if(upper(*p++)!=*key++) return 0;
    return !*p || *p=='=' || *p==' ' || *p=='\t' || *p==';';
}
static void apply_line(DosConfig *c,char *line,unsigned number,int bad,int pass) {
    int country=country_directive(line);
    if(country!=(pass==0)) return;
    unsigned previous=c->device_count;
    u16 old_country=c->country,old_page=c->code_page; char old_path[DOS_PATH_MAX];
    memcpy(old_path,c->country_file,sizeof(old_path));
    int e=bad?DE_ENV:config_line(c,line);
    if(!e && country) e=dos_config_country(c->country,c->code_page,c->country_file);
    if(e && country) {
        c->country=old_country; c->code_page=old_page;
        memcpy(c->country_file,old_path,sizeof(old_path));
    }
    if(!e && c->device_count>previous) c->devices[previous].line=number;
    if(e) {
        c->warnings++;
        print("CONFIG.SYS line %u: invalid or unsupported directive (%u)\n",
              (unsigned long long)number,(unsigned long long)e);
    }
}
int config_load(DosConfig *c) {
    config_defaults(c);
    unsigned h; int e=dos_open("C:\\CONFIG.SYS",0,0,&h);
    if(e==DE_NOFILE) return 0;
    if(e) return e;
    char line[512]; u8 buffer[512];
    /* Select the final country before canonicalizing SHELL/DEVICE paths,
     * regardless of directive order. The second pass skips COUNTRY lines. */
    for(int pass=0;pass<2 && !e;pass++) {
        unsigned number=1,n=0,total=0; int bad=0,cr=0,eof=0;
        if(pass) {u32 position; e=dos_seek(h,0,0,&position);}
        while(!e && !eof) {
            u32 got; e=dos_read(h,buffer,sizeof(buffer),&got); if(e || !got) break;
            total+=got; if(total>65536) {e=DE_ENV; break;}
            for(u32 i=0;i<got;i++) {
                u8 ch=buffer[i];
                if(ch==26) {eof=1; break;}
                if(ch=='\n' && cr) {cr=0; continue;}
                cr=ch=='\r';
                if(ch=='\r' || ch=='\n') {
                    line[n]=0; apply_line(c,line,number++,bad,pass); n=0; bad=0;
                } else if(!ch || n+1>=sizeof(line)) bad=1;
                else line[n++]=ch;
            }
        }
        if(!e && (n || bad)) {line[n]=0; apply_line(c,line,number,bad,pass);}
    }
    if(e) dos_preserve_error(e);
    int close=dos_close(h); return e?e:close;
}
