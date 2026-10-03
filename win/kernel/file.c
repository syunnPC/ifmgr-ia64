/* SPDX-License-Identifier: GPL-2.0-or-later
 * KERNEL's files: the _l file calls, profiles (WIN.INI and private .INI
 * files), the Windows and system directories, OpenFile, temporary files and
 * drives, and the Win32 file, directory and time functions this port adds.
 * All of them are INT 21h calls in the task's DOS context.
 */
#include "kernel.h"

/* DOS's find record, as in include/dos_api.h. */
typedef struct {
    DWORD index; WORD dir; BYTE mask[11],search_attr;
    BYTE attr,cookie_low; WORD time,date; DWORD size; char name[13]; BYTE cookie_high[3];
} DosFindRecord;
typedef struct {DosFindRecord f; char pad[84];} FindHandle;
static char windows_dir[128],system_dir[128];
static DWORD last_error;

static BOOL dos(WhRegs *r) {wh_int21(r); if(r->flags&1) {last_error=(DWORD)r->ax; return FALSE;} return TRUE;}
static void regs(WhRegs *r,WORD ax) {memset(r,0,sizeof(*r)); r->ax=ax;}
DWORD WINAPI GetLastError(void) {return last_error;}
static BOOL has_dir(LPCSTR p) {for(;*p;p++) if(*p=='\\' || *p==':' || *p=='/') return TRUE; return FALSE;}
static void join(char *out,int size,LPCSTR dir,LPCSTR name) {
    int n;
    lstrcpyn(out,dir,size); n=lstrlen(out);
    if(n && out[n-1]!='\\' && n+1<size) {out[n++]='\\'; out[n]=0;}
    lstrcpyn(out+n,name,size-n);
}

/* --- the _l file calls ---------------------------------------------------------- */
HFILE WINAPI _lopen(LPCSTR path,int mode) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x3d00|(mode&0xff); r.dx=(wh_u64)(ULONG_PTR)path;
    return dos(&r)?(HFILE)r.ax:HFILE_ERROR;
}
HFILE WINAPI _lcreat(LPCSTR path,int attributes) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x3c00; r.cx=(wh_u64)(attributes&0x27); r.dx=(wh_u64)(ULONG_PTR)path;
    return dos(&r)?(HFILE)r.ax:HFILE_ERROR;
}
HFILE WINAPI _lclose(HFILE h) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x3e00; r.bx=(wh_u64)h;
    return dos(&r)?0:HFILE_ERROR;
}
UINT WINAPI _lread(HFILE h,void FAR *buffer,UINT count) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x3f00; r.bx=(wh_u64)h; r.cx=count; r.dx=(wh_u64)(ULONG_PTR)buffer;
    return dos(&r)?(UINT)r.ax:(UINT)HFILE_ERROR;
}
UINT WINAPI _lwrite(HFILE h,const void FAR *buffer,UINT count) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x4000; r.bx=(wh_u64)h; r.cx=count; r.dx=(wh_u64)(ULONG_PTR)buffer;
    return dos(&r)?(UINT)r.ax:(UINT)HFILE_ERROR;
}
LONG WINAPI _llseek(HFILE h,LONG offset,int origin) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x4200|(origin&3); r.bx=(wh_u64)h; r.dx=(wh_u64)(wh_i64)offset;
    return dos(&r)?(LONG)r.ax:HFILE_ERROR;
}

/* --- directories ---------------------------------------------------------------- */
void FileInit(HINSTANCE kernel) {
    char path[128]; int i,cut=-1;
    if(wh_module_file_name(kernel,path,sizeof(path))) lstrcpy(path,"C:\\WINDOWS\\SYSTEM\\KERNEL.DLL");
    for(i=0;path[i];i++) if(path[i]=='\\') cut=i;
    if(cut>0) path[cut]=0;
    lstrcpy(system_dir,path);
    for(i=0,cut=-1;path[i];i++) if(path[i]=='\\') cut=i;
    if(cut>2) path[cut]=0; else if(cut==2) path[3]=0;
    lstrcpy(windows_dir,path);
}
UINT WINAPI GetWindowsDirectory(LPSTR out,UINT size) {
    if(out && size) lstrcpyn(out,windows_dir,(int)size);
    return (UINT)lstrlen(windows_dir);
}
UINT WINAPI GetSystemDirectory(LPSTR out,UINT size) {
    if(out && size) lstrcpyn(out,system_dir,(int)size);
    return (UINT)lstrlen(system_dir);
}
DWORD WINAPI GetCurrentDirectory(DWORD size,LPSTR out) {
    WhRegs r; char path[128];
    regs(&r,0x1900); wh_int21(&r);
    path[0]=(char)('A'+(r.ax&0xff)); path[1]=':'; path[2]='\\';
    regs(&r,0x4700); r.dx=0; r.si=(wh_u64)(ULONG_PTR)(path+3);
    if(!dos(&r)) path[3]=0;
    if(out && size) lstrcpyn(out,path,(int)size);
    return (DWORD)lstrlen(path);
}
BOOL WINAPI SetCurrentDirectory(LPCSTR path) {
    WhRegs r;
    if(!path || !path[0]) return FALSE;
    if(path[1]==':') {
        regs(&r,0x0e00); r.dx=(wh_u64)((path[0]|0x20)-'a'); wh_int21(&r);
        regs(&r,0x1900); wh_int21(&r);
        if((int)(r.ax&0xff)!=(path[0]|0x20)-'a') {last_error=15; return FALSE;}
        if(!path[2]) return TRUE;
    }
    regs(&r,0x3b00); r.dx=(wh_u64)(ULONG_PTR)path;
    return dos(&r);
}
BOOL WINAPI CreateDirectory(LPCSTR path,void FAR *security) {WhRegs r; (void)security; regs(&r,0x3900); r.dx=(wh_u64)(ULONG_PTR)path; return dos(&r);}
BOOL WINAPI RemoveDirectory(LPCSTR path) {WhRegs r; regs(&r,0x3a00); r.dx=(wh_u64)(ULONG_PTR)path; return dos(&r);}
BOOL WINAPI DeleteFile(LPCSTR path) {WhRegs r; regs(&r,0x4100); r.dx=(wh_u64)(ULONG_PTR)path; return dos(&r);}
BOOL WINAPI MoveFile(LPCSTR from,LPCSTR to) {WhRegs r; regs(&r,0x5600); r.dx=(wh_u64)(ULONG_PTR)from; r.di=(wh_u64)(ULONG_PTR)to; return dos(&r);}
DWORD WINAPI GetFileAttributes(LPCSTR path) {
    WhRegs r; regs(&r,0x4300); r.dx=(wh_u64)(ULONG_PTR)path;
    if(!dos(&r)) return INVALID_FILE_ATTRIBUTES;
    return (DWORD)(r.cx&0x3f)?(DWORD)(r.cx&0x3f):FILE_ATTRIBUTE_NORMAL;
}
BOOL WINAPI SetFileAttributes(LPCSTR path,DWORD attrs) {
    WhRegs r; regs(&r,0x4301); r.dx=(wh_u64)(ULONG_PTR)path; r.cx=attrs&0x27;
    return dos(&r);
}
BOOL WINAPI CopyFile(LPCSTR from,LPCSTR to,BOOL fail_if_exists) {
    HFILE in,out; BYTE *buffer; UINT n; BOOL ok=TRUE; WhRegs r; WORD date=0,time=0;
    if(fail_if_exists && GetFileAttributes(to)!=INVALID_FILE_ATTRIBUTES) {last_error=80; return FALSE;}
    if((in=_lopen(from,OF_READ))==HFILE_ERROR) return FALSE;
    if((out=_lcreat(to,0))==HFILE_ERROR) {_lclose(in); return FALSE;}
    buffer=(BYTE *)GlobalAlloc(GPTR,32768);
    if(!buffer) ok=FALSE;
    while(ok && (n=_lread(in,buffer,32768))!=0) {
        if(n==(UINT)HFILE_ERROR || _lwrite(out,buffer,n)!=n) ok=FALSE;
        if(n<32768) break;
    }
    regs(&r,0x5700); r.bx=(wh_u64)in; if(dos(&r)) {date=(WORD)r.dx; time=(WORD)r.cx;}
    if(ok && date) {regs(&r,0x5701); r.bx=(wh_u64)out; r.dx=date; r.cx=time; dos(&r);}
    if(buffer) GlobalFree(buffer);
    _lclose(in); _lclose(out);
    if(!ok) DeleteFile(to);
    return ok;
}
BOOL WINAPI GetDiskFreeSpace(LPCSTR root,LPDWORD spc,LPDWORD bps,LPDWORD free_clusters,LPDWORD total) {
    WhRegs r; regs(&r,0x3600); r.dx=root && root[0] && root[1]==':'?(wh_u64)((root[0]|0x20)-'a'+1):0;
    wh_int21(&r);
    if((r.ax&0xffff)==0xffff) {last_error=15; return FALSE;}
    if(spc) *spc=(DWORD)r.ax;
    if(bps) *bps=(DWORD)r.cx;
    if(free_clusters) *free_clusters=(DWORD)r.bx;
    if(total) *total=(DWORD)r.dx;
    return TRUE;
}
static BOOL drive_valid(int d) {
    char spec[4]; BYTE fcb[44]; WhRegs r;
    spec[0]=(char)('A'+d); spec[1]=':'; spec[2]=0;
    regs(&r,0x2900); r.si=(wh_u64)(ULONG_PTR)spec; r.di=(wh_u64)(ULONG_PTR)fcb;
    wh_int21(&r);
    return (r.ax&0xff)!=0xff;
}
DWORD WINAPI GetLogicalDrives(void) {
    DWORD mask=0; int d;
    for(d=0;d<26;d++) if(drive_valid(d)) mask|=1u<<d;
    return mask;
}
UINT WINAPI GetDriveType(int drive) {
    WhRegs r;
    if(drive<0 || drive>=26 || !drive_valid(drive)) return 0;
    regs(&r,0x4409); r.bx=(wh_u64)(drive+1);
    if(dos(&r) && (r.dx&0x1000)) return DRIVE_REMOTE;
    regs(&r,0x4408); r.bx=(wh_u64)(drive+1);
    if(!dos(&r)) return last_error==50?DRIVE_REMOTE:DRIVE_FIXED;
    return (r.ax&0xff)?DRIVE_FIXED:DRIVE_REMOVABLE;
}
BYTE WINAPI GetTempDrive(char hint) {(void)hint; return 'C';}

/* --- finding files -------------------------------------------------------------------- */
static void set_dta(FindHandle *h) {WhRegs r; regs(&r,0x1a01); r.dx=(wh_u64)(ULONG_PTR)&h->f; r.cx=sizeof(h->f); wh_int21(&r);}
static void fill(const DosFindRecord *f,LPWIN32_FIND_DATA d) {
    memset(d,0,sizeof(*d));
    d->dwFileAttributes=f->attr?f->attr:FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeLow=f->size;
    d->ftLastWriteTime.dwLowDateTime=f->time|((DWORD)f->date<<16);
    lstrcpyn(d->cFileName,f->name,sizeof(d->cFileName)); lstrcpyn(d->cAlternateFileName,f->name,sizeof(d->cAlternateFileName));
}
HANDLE WINAPI FindFirstFile(LPCSTR pattern,LPWIN32_FIND_DATA d) {
    FindHandle *h; WhRegs r;
    if(!pattern || !d || !(h=(FindHandle *)GlobalAlloc(GPTR,sizeof(FindHandle)))) return INVALID_HANDLE_VALUE;
    set_dta(h);
    regs(&r,0x4e00); r.cx=0x37; r.dx=(wh_u64)(ULONG_PTR)pattern;
    if(!dos(&r)) {GlobalFree(h); return INVALID_HANDLE_VALUE;}
    fill(&h->f,d);
    return (HANDLE)h;
}
BOOL WINAPI FindNextFile(HANDLE handle,LPWIN32_FIND_DATA d) {
    FindHandle *h=(FindHandle *)handle; WhRegs r;
    if(!h || handle==INVALID_HANDLE_VALUE || !d) return FALSE;
    set_dta(h);
    regs(&r,0x4f00);
    if(!dos(&r)) return FALSE;
    fill(&h->f,d);
    return TRUE;
}
BOOL WINAPI FindClose(HANDLE h) {if(!h || h==INVALID_HANDLE_VALUE) return FALSE; GlobalFree(h); return TRUE;}
BOOL WINAPI FileTimeToDosDateTime(const FILETIME FAR *t,LPWORD date,LPWORD time) {
    if(!t) return FALSE;
    if(date) *date=(WORD)(t->dwLowDateTime>>16);
    if(time) *time=(WORD)t->dwLowDateTime;
    return TRUE;
}

/* --- time ------------------------------------------------------------------------------- */
void WINAPI GetLocalTime(LPSYSTEMTIME t) {
    WhRegs r;
    if(!t) return;
    memset(t,0,sizeof(*t));
    regs(&r,0x2a00); wh_int21(&r);
    t->wYear=(WORD)r.cx; t->wMonth=(WORD)(r.dx>>8&0xff); t->wDay=(WORD)(r.dx&0xff); t->wDayOfWeek=(WORD)(r.ax&0xff);
    regs(&r,0x2c00); wh_int21(&r);
    t->wHour=(WORD)(r.cx>>8&0xff); t->wMinute=(WORD)(r.cx&0xff); t->wSecond=(WORD)(r.dx>>8&0xff); t->wMilliseconds=(WORD)((r.dx&0xff)*10);
}
BOOL WINAPI SetLocalTime(const SYSTEMTIME FAR *t) {
    WhRegs r;
    if(!t) return FALSE;
    regs(&r,0x2b00); r.cx=t->wYear; r.dx=(wh_u64)((t->wMonth<<8)|t->wDay); wh_int21(&r);
    if(r.ax&0xff) return FALSE;
    regs(&r,0x2d00); r.cx=(wh_u64)((t->wHour<<8)|t->wMinute); r.dx=(wh_u64)((t->wSecond<<8)|(t->wMilliseconds/10)); wh_int21(&r);
    return !(r.ax&0xff);
}

/* --- OpenFile and temporary files ------------------------------------------------------------- */
static BOOL canonical(LPCSTR in,char *out) {
    WhRegs r; regs(&r,0x6000); r.si=(wh_u64)(ULONG_PTR)in; r.di=(wh_u64)(ULONG_PTR)out;
    if(!dos(&r)) {lstrcpyn(out,in,OFS_MAXPATHNAME); return FALSE;}
    return TRUE;
}
/* A file without a directory is looked for in the current, Windows and
 * system directories, then along the PATH. */
static BOOL search(LPCSTR name,char *found) {
    char path[512],candidate[260]; int i,start;
    if(GetFileAttributes(name)!=INVALID_FILE_ATTRIBUTES) {canonical(name,found); return TRUE;}
    if(has_dir(name)) return FALSE;
    join(candidate,sizeof(candidate),windows_dir,name);
    if(GetFileAttributes(candidate)!=INVALID_FILE_ATTRIBUTES) {lstrcpyn(found,candidate,OFS_MAXPATHNAME); return TRUE;}
    join(candidate,sizeof(candidate),system_dir,name);
    if(GetFileAttributes(candidate)!=INVALID_FILE_ATTRIBUTES) {lstrcpyn(found,candidate,OFS_MAXPATHNAME); return TRUE;}
    if(wh_env_get("PATH",path,sizeof(path))) return FALSE;
    for(i=0,start=0;;i++) if(path[i]==';' || !path[i]) {
        char c=path[i]; path[i]=0;
        if(i>start) {join(candidate,sizeof(candidate),path+start,name); if(GetFileAttributes(candidate)!=INVALID_FILE_ATTRIBUTES) {lstrcpyn(found,candidate,OFS_MAXPATHNAME); return TRUE;}}
        if(!c) break;
        start=i+1;
    }
    return FALSE;
}
HFILE WINAPI OpenFile(LPCSTR name,LPOFSTRUCT ofs,UINT style) {
    char path[OFS_MAXPATHNAME]; HFILE h;
    if(!ofs) return HFILE_ERROR;
    if(style&OF_REOPEN) lstrcpyn(path,ofs->szPathName,sizeof(path));
    else if(!name) return HFILE_ERROR;
    else if(style&(OF_CREATE|OF_PARSE)) canonical(name,path);
    else if(!search(name,path)) {
        memset(ofs,0,sizeof(*ofs)); ofs->cBytes=sizeof(*ofs); ofs->nErrCode=2; canonical(name,ofs->szPathName);
        return HFILE_ERROR;
    }
    memset(ofs,0,sizeof(*ofs)); ofs->cBytes=sizeof(*ofs); lstrcpyn(ofs->szPathName,path,sizeof(ofs->szPathName));
    ofs->fFixedDisk=(BYTE)(GetDriveType((path[0]|0x20)-'a')!=DRIVE_REMOVABLE);
    if(style&OF_PARSE) return 0;
    if(style&OF_DELETE) {if(DeleteFile(path)) return 1; ofs->nErrCode=(WORD)last_error; return HFILE_ERROR;}
    if(style&OF_CREATE) h=_lcreat(path,0);
    else h=_lopen(path,(int)(style&0x73));
    if(h==HFILE_ERROR) {ofs->nErrCode=(WORD)last_error; return HFILE_ERROR;}
    if(style&OF_EXIST) {_lclose(h); return 1;}
    return h;
}
int WINAPI GetTempFileName(BYTE drive,LPCSTR prefix,UINT unique,LPSTR out) {
    char dir[260]; UINT n=unique?unique:(UINT)(wh_ticks()&0xffff); int tries;
    if(!out) return 0;
    if((drive&TF_FORCEDRIVE) || wh_env_get("TEMP",dir,sizeof(dir)) || !dir[0]) {dir[0]=(char)(drive&0x7f?drive&0x7f:'C'); dir[1]=':'; dir[2]='\\'; dir[3]=0;}
    for(tries=0;tries<1000;tries++,n=(n+1)&0xffff) {
        char name[16]; HFILE h;
        if(!n) n=1;
        {   /* ~PRE + four hex digits + .TMP */
            static const char hex[]="0123456789ABCDEF"; int k=0,i;
            name[k++]='~';
            for(i=0;prefix && prefix[i] && i<3;i++) name[k++]=prefix[i];
            for(i=3;i>=0;i--) name[k++]=hex[(n>>(i*4))&15];
            lstrcpy(name+k,".TMP");
        }
        join(out,144,dir,name);
        if(unique) return (int)n;
        if(GetFileAttributes(out)!=INVALID_FILE_ATTRIBUTES) continue;
        if((h=_lcreat(out,0))!=HFILE_ERROR) {_lclose(h); return (int)n;}
    }
    return 0;
}

/* --- profiles -------------------------------------------------------------------------------- */
typedef struct {char *text; DWORD len;} Ini;
static void ini_path(LPCSTR file,char *out) {
    if(has_dir(file)) lstrcpyn(out,file,260);
    else join(out,260,windows_dir,file);
}
static BOOL ini_read(LPCSTR file,Ini *ini) {
    char path[260]; HFILE h; LONG size;
    ini->text=NULL; ini->len=0;
    ini_path(file,path);
    if((h=_lopen(path,OF_READ))==HFILE_ERROR) return FALSE;
    size=_llseek(h,0,2); _llseek(h,0,0);
    if(size<0 || size>1024*1024 || !(ini->text=(char *)GlobalAlloc(GPTR,(DWORD)size+1))) {_lclose(h); return FALSE;}
    ini->len=_lread(h,ini->text,(UINT)size);
    if(ini->len==(DWORD)HFILE_ERROR) ini->len=0;
    ini->text[ini->len]=0;
    _lclose(h);
    return TRUE;
}
static BOOL ini_write(LPCSTR file,const char *text,DWORD len) {
    char path[260]; HFILE h; BOOL ok;
    ini_path(file,path);
    if((h=_lcreat(path,0))==HFILE_ERROR) return FALSE;
    ok=_lwrite(h,text,(UINT)len)==(UINT)len;
    _lclose(h);
    return ok;
}
/* Walk the lines: each call gives one line's bounds and kind. */
typedef struct {const char *p,*end; const char *line,*line_end,*name,*name_end,*value,*value_end; int kind;} Line; /* 1 section, 2 key */
static BOOL next_line(Line *l) {
    const char *s,*e;
    if(l->p>=l->end) return FALSE;
    l->line=l->p;
    for(e=l->p;e<l->end && *e!='\n';e++) {}
    l->line_end=e; l->p=e<l->end?e+1:e;
    for(s=l->line;s<e && (*s==' ' || *s=='\t');s++) {}
    while(e>s && (e[-1]=='\r' || e[-1]==' ' || e[-1]=='\t')) e--;
    l->kind=0;
    if(s<e && *s=='[') {
        const char *c=s+1; while(c<e && *c!=']') c++;
        l->name=s+1; l->name_end=c; l->kind=1;
    } else if(s<e && *s!=';') {
        const char *eq=s; while(eq<e && *eq!='=') eq++;
        if(eq<e) {
            const char *ne=eq,*v=eq+1;
            while(ne>s && (ne[-1]==' ' || ne[-1]=='\t')) ne--;
            while(v<e && (*v==' ' || *v=='\t')) v++;
            l->name=s; l->name_end=ne; l->value=v; l->value_end=e; l->kind=2;
        }
    }
    return TRUE;
}
static BOOL same(const char *a,const char *a_end,LPCSTR b) {
    int n=lstrlen(b);
    if(a_end-a!=n) return FALSE;
    for(;a<a_end;a++,b++) {char x=*a,y=*b; if(x>='a' && x<='z') x-=32; if(y>='a' && y<='z') y-=32; if(x!=y) return FALSE;}
    return TRUE;
}
static int copy_out(LPSTR out,int size,const char *s,int n) {
    if(size<=0) return 0;
    if(n>size-1) n=size-1;
    memcpy(out,s,(size_t)n); out[n]=0;
    return n;
}
int WINAPI GetPrivateProfileString(LPCSTR section,LPCSTR key,LPCSTR def,LPSTR out,int size,LPCSTR file) {
    Ini ini; Line l; BOOL in=FALSE; int n=0;
    if(!out || size<=0) return 0;
    out[0]=0;
    if(!file) file="WIN.INI";
    if(ini_read(file,&ini)) {
        l.p=ini.text; l.end=ini.text+ini.len;
        while(next_line(&l)) {
            if(l.kind==1) {in=section && same(l.name,l.name_end,section); continue;}
            if(l.kind!=2 || !in) continue;
            if(!key) {
                /* Every key name, each followed by a zero, then another zero. */
                int k=(int)(l.name_end-l.name);
                if(n+k+2>size) break;
                memcpy(out+n,l.name,(size_t)k); n+=k; out[n++]=0; out[n]=0;
                continue;
            }
            if(same(l.name,l.name_end,key)) {n=copy_out(out,size,l.value,(int)(l.value_end-l.value)); GlobalFree(ini.text); return n;}
        }
        if(!section) {
            /* Every section name. */
            l.p=ini.text; l.end=ini.text+ini.len; n=0;
            while(next_line(&l)) if(l.kind==1) {
                int k=(int)(l.name_end-l.name);
                if(n+k+2>size) break;
                memcpy(out+n,l.name,(size_t)k); n+=k; out[n++]=0; out[n]=0;
            }
        }
        GlobalFree(ini.text);
        if(!key || !section) return n;
    }
    if(!key || !section) return 0;
    return copy_out(out,size,def?def:"",def?lstrlen(def):0);
}
UINT WINAPI GetPrivateProfileInt(LPCSTR section,LPCSTR key,int def,LPCSTR file) {
    char text[32]; const char *p=text; int v=0,neg=0;
    if(!GetPrivateProfileString(section,key,"",text,sizeof(text),file)) return (UINT)def;
    if(*p=='-') {neg=1; p++;}
    if(*p<'0' || *p>'9') return (UINT)def;
    while(*p>='0' && *p<='9') v=v*10+(*p++-'0');
    return (UINT)(neg?-v:v);
}
/* Rewrite the file with the key set (or removed, or the section removed). */
BOOL WINAPI WritePrivateProfileString(LPCSTR section,LPCSTR key,LPCSTR value,LPCSTR file) {
    Ini ini; Line l; char *out; DWORD n=0,cap; BOOL in=FALSE,done=FALSE,found=FALSE,ok; const char *insert_at=NULL;
    if(!section) return FALSE;
    if(!file) file="WIN.INI";
    if(!ini_read(file,&ini)) {ini.text=NULL; ini.len=0;}
    cap=ini.len+(DWORD)lstrlen(section)+(key?(DWORD)lstrlen(key):0)+(value?(DWORD)lstrlen(value):0)+64;
    if(!(out=(char *)GlobalAlloc(GPTR,cap))) {if(ini.text) GlobalFree(ini.text); return FALSE;}
#define EMIT(s,len) do {memcpy(out+n,(s),(size_t)(len)); n+=(DWORD)(len);} while(0)
#define EMIT_KEY() do {EMIT(key,lstrlen(key)); EMIT("=",1); EMIT(value,lstrlen(value)); EMIT("\r\n",2);} while(0)
    /* The section's end: after its last non-blank line. */
    l.p=ini.text?ini.text:""; l.end=l.p+ini.len;
    while(next_line(&l)) {
        if(l.kind==1) {in=same(l.name,l.name_end,section); if(in) {found=TRUE; insert_at=l.p;} continue;}
        if(in && l.kind) insert_at=l.p;
    }
    in=FALSE;
    l.p=ini.text?ini.text:""; l.end=l.p+ini.len;
    while(next_line(&l)) {
        BOOL keep=TRUE;
        if(l.kind==1) in=same(l.name,l.name_end,section);
        if(in && !key) keep=FALSE;
        else if(in && l.kind==2 && key && same(l.name,l.name_end,key)) {
            keep=FALSE;
            if(value && !done) EMIT_KEY();
            done=TRUE;
        }
        if(keep) {EMIT(l.line,l.p-l.line); if(l.p==l.end && (l.p==l.line || l.p[-1]!='\n')) EMIT("\r\n",2);}
        if(key && value && !done && l.p==insert_at) {EMIT_KEY(); done=TRUE;}
    }
    if(key && value && !done && !found) {
        if(n) EMIT("\r\n",2);
        EMIT("[",1); EMIT(section,lstrlen(section)); EMIT("]\r\n",3);
        EMIT_KEY();
    }
#undef EMIT_KEY
#undef EMIT
    ok=ini_write(file,out,n);
    GlobalFree(out); if(ini.text) GlobalFree(ini.text);
    return ok;
}
int WINAPI GetProfileString(LPCSTR section,LPCSTR key,LPCSTR def,LPSTR out,int size) {return GetPrivateProfileString(section,key,def,out,size,"WIN.INI");}
int WINAPI GetProfileInt(LPCSTR section,LPCSTR key,int def) {return (int)GetPrivateProfileInt(section,key,def,"WIN.INI");}
BOOL WINAPI WriteProfileString(LPCSTR section,LPCSTR key,LPCSTR value) {return WritePrivateProfileString(section,key,value,"WIN.INI");}
