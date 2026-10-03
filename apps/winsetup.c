/* SPDX-License-Identifier: GPL-2.0-or-later
 * SETUP (on the Interface Manager CD): installs Interface Manager on a DOS system.
 *   SETUP [/Q] [drive:\directory]
 * Finds the disc by its SETUP.INF, copies its WINDOWS tree to the target
 * (default C:\WINDOWS), copies its WIN.INI and writes SYSTEM.INI when they
 * are absent, and adds the directory to the PATH in C:\AUTOEXEC.BAT. /Q
 * asks nothing.
 */
#include "runtime.h"
#define SIGNATURE "[Interface Manager 3.0 Setup]"
#define CHUNK 32768
static char source[DOS_PATH_MAX],target[DOS_PATH_MAX];
static unsigned files_copied;
static u8 *buffer;

static int yes(const char *question) {
    print("%s (Y/N)? ",question);
    for(;;) {
        DosRegs r={.ax=0x0800}; if(dos_call(&r)) return 0;
        char c=upper((char)r.ax);
        if(c=='Y' || c=='N') {print("%c\n",c); return c=='Y';}
    }
}
static int read_file(const char *path,char *out,u32 capacity,u32 *size) {
    unsigned h; int e=dos_open(path,DOS_OPEN_READ,0,&h); if(e) return e;
    e=dos_read(h,out,capacity-1,size); dos_close(h);
    if(!e) out[*size]=0;
    return e;
}
static int write_file(const char *path,const void *data,u32 size) {
    unsigned h,result; u32 done;
    int e=dos_open_ex(path,DOS_OPEN_WRITE,0,0x12,&h,&result); if(e) return e;
    e=dos_write(h,data,size,&done); if(!e && done!=size) e=DE_FULL;
    int c=dos_close(h); return e?e:c;
}
static int contains(const char *text,const char *word) {
    size_t n=strlen(word);
    for(;*text;text++) {
        size_t i=0; while(i<n && upper(text[i])==upper(word[i])) i++;
        if(i==n) return 1;
    }
    return 0;
}
/* The first drive whose root has a Windows SETUP.INF. */
static int find_source(void) {
    static char inf[512];
    for(unsigned d=0;d<26;d++) {
        char path[16]="X:\\SETUP.INF"; u32 size; path[0]=(char)('A'+d);
        if(read_file(path,inf,sizeof(inf),&size) || !contains(inf,SIGNATURE)) continue;
        strcopy(source,sizeof(source),"X:\\WINDOWS"); source[0]=(char)('A'+d);
        return 0;
    }
    return DE_NOFILE;
}
static int copy_file(const char *from,const char *to) {
    unsigned in,out,result; u32 got,done; int e;
    e=dos_open(from,DOS_OPEN_READ,0,&in); if(e) return e;
    e=dos_open_ex(to,DOS_OPEN_WRITE,0,0x12,&out,&result);
    if(e) {dos_close(in); return e;}
    do {
        e=dos_read(in,buffer,CHUNK,&got);
        if(!e && got) {e=dos_write(out,buffer,got,&done); if(!e && done!=got) e=DE_FULL;}
    } while(!e && got==CHUNK);
    dos_close(in);
    int c=dos_close(out); if(!e) e=c;
    if(!e) files_copied++;
    return e;
}
static int join(char *out,const char *dir,const char *name) {
    if(strcopy(out,DOS_PATH_MAX,dir)) return DE_PATH;
    if(strappend(out,DOS_PATH_MAX,"\\") || strappend(out,DOS_PATH_MAX,name)) return DE_PATH;
    return 0;
}
static int copy_tree(const char *from,const char *to) {
    char pattern[DOS_PATH_MAX],a[DOS_PATH_MAX],b[DOS_PATH_MAX]; DosFind find;
    int e=dos_mkdir(to);
    if(e && e!=DE_ACCESS && e!=DE_EXISTS) return e;
    e=join(pattern,from,"*.*"); if(e) return e;
    for(e=dos_find_first(pattern,FA_DIR,&find);!e;e=dos_find_next(&find)) {
        if(find.name[0]=='.') continue;
        if(join(a,from,find.name) || join(b,to,find.name)) return DE_PATH;
        if(find.attr&FA_DIR) {int x=copy_tree(a,b); if(x) return x;}
        else {
            print("  %s\n",b);
            int x=copy_file(a,b); if(x) {print("SETUP: cannot copy %s (%s)\n",a,dos_error(x)); return x;}
        }
    }
    return e==DE_NOMORE?0:e;
}
static int write_ini(const char *name,const char *text) {
    char path[DOS_PATH_MAX]; u8 attr; int e=join(path,target,name); if(e) return e;
    if(!dos_attribute(path,0,&attr)) return 0; /* keep the user's settings */
    return write_file(path,text,(u32)strlen(text));
}
/* WIN.INI comes from the root of the disc (the default printer and ports
 * among its settings), unless the target has one. */
static int copy_ini(const char *name) {
    char from[DOS_PATH_MAX],to[DOS_PATH_MAX]; u8 attr; int e=join(to,target,name); if(e) return e;
    if(!dos_attribute(to,0,&attr)) return 0; /* keep the user's settings */
    strcopy(from,sizeof(from),"X:"); from[0]=source[0];
    if((e=join(from,from,name))!=0) return e;
    return copy_file(from,to);
}
/* Add the Windows directory to the PATH statement, or add one. */
static int update_autoexec(void) {
    static char text[8192],out[8192+DOS_PATH_MAX+16];
    u32 size=0; int e=read_file("C:\\AUTOEXEC.BAT",text,sizeof(text),&size);
    if(e && e!=DE_NOFILE) return e;
    if(e) text[0]=0;
    if(contains(text,target)) return 0;
    out[0]=0; int done=0; char *line=text;
    while(*line) {
        char *end=line; while(*end && *end!='\n') end++;
        int has_newline=*end=='\n'; *end=0;
        size_t length=strlen(line); if(length && line[length-1]=='\r') line[--length]=0;
        strappend(out,sizeof(out),line);
        char *p=line; while(*p==' ') p++;
        if(!done && upper(p[0])=='P' && upper(p[1])=='A' && upper(p[2])=='T' && upper(p[3])=='H' && (p[4]==' ' || p[4]=='=')) {
            strappend(out,sizeof(out),";"); strappend(out,sizeof(out),target); done=1;
        }
        strappend(out,sizeof(out),"\r\n");
        line=has_newline?end+1:end;
    }
    if(!done) {strappend(out,sizeof(out),"PATH "); strappend(out,sizeof(out),target); strappend(out,sizeof(out),";%PATH%\r\n");}
    return write_file("C:\\AUTOEXEC.BAT",out,(u32)strlen(out));
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char args[128],*p=args; int quiet=0; strcopy(args,sizeof(args),app_dos->command_tail());
    strcopy(target,sizeof(target),"C:\\WINDOWS");
    for(;;) {
        while(*p==' ') p++;
        if(!*p) break;
        char *word=p; while(*p && *p!=' ') p++;
        if(*p) *p++=0;
        if(word[0]=='/' && upper(word[1])=='Q' && !word[2]) quiet=1;
        else if(word[1]==':' && word[2]=='\\' && word[3]) {for(char *c=word;*c;c++) *c=upper(*c); strcopy(target,sizeof(target),word);}
        else {print("usage: SETUP [/Q] [drive:\\directory]\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    }
    print("\nInterface Manager 3.0 for IA-64 Setup\n\n");
    if(find_source()) {print("SETUP: the Interface Manager disc was not found\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    char check[DOS_PATH_MAX]; u8 attr; join(check,target,"WIN.COM");
    print("Interface Manager will be installed in %s from %s.\n",target,source);
    if(!quiet && !yes(dos_attribute(check,0,&attr)?"Continue":"Interface Manager is already there. Replace it")) {
        print("Setup was cancelled.\n"); dos_set_errorlevel(2); return EFI_SUCCESS;
    }
    if(dos_alloc(CHUNK/16,(void **)&buffer)) {print("SETUP: not enough memory\n"); dos_set_errorlevel(1); return EFI_SUCCESS;}
    print("Copying files...\n");
    int e=copy_tree(source,target);
    if(!e) e=copy_ini("WIN.INI");
    if(!e) e=write_ini("SYSTEM.INI","[boot]\r\nshell=progman.exe\r\n");
    if(!e) e=update_autoexec();
    dos_free(buffer);
    if(e) {print("SETUP: installation failed (%s)\n",dos_error(e)); dos_set_errorlevel(1); return EFI_SUCCESS;}
    print("\n%u files copied. Interface Manager is installed in %s.\n",(unsigned long long)files_copied,target);
    print("Restart DOS to use the new PATH, then type WIN to start Interface Manager.\n");
    return EFI_SUCCESS;
}
