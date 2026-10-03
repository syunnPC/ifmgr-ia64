/* SPDX-License-Identifier: GPL-2.0-or-later
 * Low-level files over KERNEL's (_lopen and the like): a handle is an HFILE.
 * As Microsoft C's, files open in text mode unless O_BINARY (or _fmode)
 * says otherwise: reads drop the CR of CR LF and end at ^Z, writes put CR
 * before LF. Also stat, the directory functions and Microsoft C's _dos_*
 * calls. C89. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <dirent.h>
#include <dos.h>
#include <time.h>
#include <sys/stat.h>
#include "crtp.h"
#define HANDLES 256
int _fmode=O_TEXT;
static unsigned char text[HANDLES],append[HANDLES];
static int _fd_text(int fd) {return fd>=0 && fd<HANDLES && text[fd];}
static int error_from(DWORD e) {return e==2 || e==3?ENOENT:e==4?EMFILE:e==5?EACCES:e==80?EEXIST:EINVAL;}
static int fail(int e) {errno=e; return -1;}
int open(const char *path,int flag,...) {
    HFILE h; int access=(flag&3)==O_WRONLY?OF_WRITE:(flag&3)==O_RDWR?OF_READWRITE:OF_READ;
    DWORD attr=GetFileAttributes(path);
    if(attr!=0xffffffffU && (attr&FILE_ATTRIBUTE_DIRECTORY)) return fail(EACCES);
    if((flag&O_CREAT) && (flag&O_EXCL) && attr!=0xffffffffU) return fail(EEXIST);
    if((flag&O_CREAT) && (attr==0xffffffffU || (flag&O_TRUNC))) {
        h=_lcreat(path,0);
        if(h!=HFILE_ERROR && access!=OF_READWRITE) {_lclose(h); h=_lopen(path,access);}
    } else {
        h=_lopen(path,access);
        if(h!=HFILE_ERROR && (flag&O_TRUNC)) {_lclose(h); h=_lcreat(path,0); if(h!=HFILE_ERROR && access!=OF_READWRITE) {_lclose(h); h=_lopen(path,access);}}
    }
    if(h==HFILE_ERROR) return fail(error_from(GetLastError()));
    if(h<0 || h>=HANDLES) {_lclose(h); return fail(EMFILE);}
    text[h]=(unsigned char)(flag&O_BINARY?0:flag&O_TEXT?1:_fmode!=O_BINARY);
    append[h]=(unsigned char)((flag&O_APPEND)!=0);
    return h;
}
int creat(const char *path,int mode) {(void)mode; return open(path,O_CREAT|O_TRUNC|O_RDWR);}
int close(int fd) {
    if(fd<0 || fd>=HANDLES) return fail(EBADF);
    text[fd]=append[fd]=0;
    return _lclose(fd)==HFILE_ERROR?fail(EBADF):0;
}
static int raw_read(int fd,void *b,unsigned n) {
    UINT got=_lread(fd,b,n);
    return got==(UINT)HFILE_ERROR?fail(EBADF):(int)got;
}
int read(int fd,void *b,unsigned n) {
    char *p=(char *)b; int got,i,j;
    if(!_fd_text(fd)) return raw_read(fd,b,n);
    got=raw_read(fd,b,n); if(got<=0) return got;
    for(i=j=0;i<got;i++) {
        if(p[i]==26) {_llseek(fd,(LONG)(i-got),1); break;}
        if(p[i]=='\r') {
            char c;
            if(i+1<got) {if(p[i+1]=='\n') continue;}
            else if(raw_read(fd,&c,1)==1) {_llseek(fd,-1,1); if(c=='\n') continue;}
        }
        p[j++]=p[i];
    }
    return j;
}
static int raw_write(int fd,const void *b,unsigned n) {
    UINT done;
    if(fd>=0 && fd<HANDLES && append[fd]) _llseek(fd,0,2);
    done=_lwrite(fd,b,n);
    if(done==(UINT)HFILE_ERROR) return fail(EBADF);
    if(done<n) return fail(ENOSPC);
    return (int)done;
}
int write(int fd,const void *b,unsigned n) {
    const char *p=(const char *)b; char out[512]; unsigned i,k=0;
    if(!_fd_text(fd)) return raw_write(fd,b,n);
    for(i=0;i<n;i++) {
        if(p[i]=='\n') out[k++]='\r';
        out[k++]=p[i];
        if(k>=sizeof(out)-1) {if(raw_write(fd,out,k)<0) return -1; k=0;}
    }
    if(k && raw_write(fd,out,k)<0) return -1;
    return (int)n;
}
long lseek(int fd,long offset,int whence) {
    LONG r=_llseek(fd,offset,whence);
    return r==-1?(long)fail(EINVAL):r;
}
long tell(int fd) {return lseek(fd,0,SEEK_CUR);}
long filelength(int fd) {
    long here=tell(fd),end;
    if(here<0) return -1;
    end=lseek(fd,0,SEEK_END); lseek(fd,here,SEEK_SET); return end;
}
int eof(int fd) {long here=tell(fd),end=filelength(fd); return here<0 || end<0?-1:here>=end;}
int setmode(int fd,int mode) {
    int old;
    if(fd<0 || fd>=HANDLES) return fail(EBADF);
    old=text[fd]?O_TEXT:O_BINARY; text[fd]=(unsigned char)(mode==O_TEXT); return old;
}
int isatty(int fd) {(void)fd; return 0;}
int chsize(int fd,long size) {
    long here=tell(fd);
    if(here<0 || lseek(fd,size,SEEK_SET)<0) return -1;
    if(_lwrite(fd,"",0)==(UINT)HFILE_ERROR) return fail(EACCES);
    lseek(fd,here,SEEK_SET); return 0;
}
int access(const char *path,int mode) {
    DWORD a=GetFileAttributes(path);
    if(a==0xffffffffU) return fail(ENOENT);
    if((mode&2) && (a&1)) return fail(EACCES);
    return 0;
}
int unlink(const char *path) {return DeleteFile(path)?0:fail(error_from(GetLastError()));}
int remove(const char *path) {return unlink(path);}
int rename(const char *from,const char *to) {return MoveFile(from,to)?0:fail(error_from(GetLastError()));}

/* A DOS date and time, local, as seconds since 1970 UTC. */
static time_t _dos_time(unsigned date,unsigned time) {
    struct tm t;
    t.tm_year=(int)((date>>9)&127)+80; t.tm_mon=(int)((date>>5)&15)-1; t.tm_mday=(int)(date&31);
    t.tm_hour=(int)((time>>11)&31); t.tm_min=(int)((time>>5)&63); t.tm_sec=(int)((time&31)*2); t.tm_isdst=-1;
    return mktime(&t);
}
int stat(const char *path,struct stat *st) {
    WIN32_FIND_DATA f; HANDLE h; size_t n=strlen(path);
    memset(st,0,sizeof(*st));
    if((n==3 && path[1]==':' && (path[2]=='\\' || path[2]=='/')) || (n==1 && (path[0]=='\\' || path[0]=='/'))) {
        if(GetFileAttributes(path)==0xffffffffU) return fail(ENOENT);
        st->st_mode=S_IFDIR|S_IREAD|S_IWRITE|S_IEXEC; return 0;
    }
    h=FindFirstFile(path,&f);
    if(h==INVALID_HANDLE_VALUE) return fail(ENOENT);
    FindClose(h);
    st->st_mode=(unsigned short)((f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY?S_IFDIR|S_IEXEC:S_IFREG)|S_IREAD|(f.dwFileAttributes&1?0:S_IWRITE));
    st->st_size=(long)f.nFileSizeLow; st->st_nlink=1;
    st->st_mtime=st->st_atime=st->st_ctime=_dos_time(f.ftLastWriteTime.dwLowDateTime>>16,f.ftLastWriteTime.dwLowDateTime&0xffff);
    if(path[0] && path[1]==':') st->st_dev=st->st_rdev=(short)((path[0]|0x20)-'a'); else st->st_dev=st->st_rdev=(short)(_getdrive()-1);
    return 0;
}
int fstat(int fd,struct stat *st) {
    memset(st,0,sizeof(*st));
    st->st_size=filelength(fd);
    if(st->st_size<0) return -1;
    st->st_mode=S_IFREG|S_IREAD|S_IWRITE; st->st_nlink=1;
    return 0;
}

char *getcwd(char *buf,int size) {
    char path[_MAX_PATH]; DWORD n=GetCurrentDirectory(sizeof(path),path);
    if(!n || n>=sizeof(path)) {errno=ENOENT; return NULL;}
    if(!buf) {if(size<(int)n+1) size=(int)n+1; buf=(char *)malloc((size_t)size); if(!buf) return NULL;}
    if((int)n+1>size) {errno=ERANGE; return NULL;}
    return strcpy(buf,path);
}
int chdir(const char *path) {return SetCurrentDirectory(path)?0:fail(ENOENT);}
int mkdir(const char *path) {return CreateDirectory(path,NULL)?0:fail(error_from(GetLastError()));}
int rmdir(const char *path) {return RemoveDirectory(path)?0:fail(error_from(GetLastError()));}
int _getdrive(void) {char path[_MAX_PATH]; return GetCurrentDirectory(sizeof(path),path)?(path[0]|0x20)-'a'+1:0;}
int _chdrive(int drive) {
    char path[3]; path[0]=(char)('A'+drive-1); path[1]=':'; path[2]=0;
    return SetCurrentDirectory(path)?0:-1;
}

/* Microsoft C's _dos_* calls. A search keeps its KERNEL handle and the
 * attributes asked for in find_t's reserved bytes. */
typedef struct {HANDLE h; unsigned attrib;} Search;
static unsigned found(struct find_t *f,const WIN32_FIND_DATA *d) {
    size_t n=strlen(d->cAlternateFileName[0]?d->cAlternateFileName:d->cFileName);
    f->attrib=(char)d->dwFileAttributes; f->size=(long)d->nFileSizeLow;
    f->wr_time=(unsigned short)(d->ftLastWriteTime.dwLowDateTime&0xffff);
    f->wr_date=(unsigned short)(d->ftLastWriteTime.dwLowDateTime>>16);
    if(n>12) n=12;
    memcpy(f->name,d->cAlternateFileName[0]?d->cAlternateFileName:d->cFileName,n); f->name[n]=0;
    return 0;
}
static int wanted(const WIN32_FIND_DATA *d,unsigned attrib) {return !(d->dwFileAttributes&(_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_VOLID)&~attrib);}
unsigned _dos_findfirst(const char *path,unsigned attrib,struct find_t *f) {
    WIN32_FIND_DATA d; Search s;
    s.h=FindFirstFile(path,&d); s.attrib=attrib;
    if(s.h==INVALID_HANDLE_VALUE) {errno=ENOENT; return 18;}
    while(!wanted(&d,attrib)) if(!FindNextFile(s.h,&d)) {FindClose(s.h); errno=ENOENT; return 18;}
    memcpy(f->reserved,&s,sizeof(s));
    return found(f,&d);
}
unsigned _dos_findnext(struct find_t *f) {
    WIN32_FIND_DATA d; Search s;
    memcpy(&s,f->reserved,sizeof(s));
    do {if(!FindNextFile(s.h,&d)) {FindClose(s.h); errno=ENOENT; return 18;}} while(!wanted(&d,s.attrib));
    return found(f,&d);
}
/* Open Watcom's opendir: a directory's entries (hidden, system and
 * directories among them), or a pattern's. */
static void entry(DIR *d,const WIN32_FIND_DATA *f) {
    struct find_t t;
    found(&t,f);
    d->d_attr=t.attrib; d->d_time=t.wr_time; d->d_date=t.wr_date; d->d_size=t.size; strcpy(d->d_name,t.name);
}
static int restart(DIR *d) {
    WIN32_FIND_DATA f;
    d->d_search=FindFirstFile(d->d_pattern,&f);
    if(d->d_search==INVALID_HANDLE_VALUE) {d->d_search=NULL; return 0;}
    d->d_first=1; entry(d,&f);
    return 1;
}
DIR *opendir(const char *path) {
    DIR *d=(DIR *)malloc(sizeof(DIR)); size_t n=strlen(path); DWORD a=GetFileAttributes(path);
    if(!d || n+5>sizeof(d->d_pattern)) {free(d); errno=ENOENT; return NULL;}
    strcpy(d->d_pattern,path);
    if(!strpbrk(path,"*?")) {
        if(n && strchr("\\/:",path[n-1])) strcat(d->d_pattern,"*.*");
        else if(a!=0xffffffff && (a&FILE_ATTRIBUTE_DIRECTORY)) strcat(d->d_pattern,"\\*.*");
    }
    if(!restart(d)) {free(d); errno=ENOENT; return NULL;}
    return d;
}
struct dirent *readdir(DIR *d) {
    WIN32_FIND_DATA f;
    if(!d || !d->d_search) return NULL;
    if(d->d_first) {d->d_first=0; return d;}
    if(!FindNextFile(d->d_search,&f)) return NULL;
    entry(d,&f);
    return d;
}
int closedir(DIR *d) {
    if(!d) return -1;
    if(d->d_search) FindClose(d->d_search);
    free(d);
    return 0;
}
void rewinddir(DIR *d) {if(d->d_search) FindClose(d->d_search); restart(d);}
unsigned _dos_open(const char *path,unsigned mode,int *fd) {
    HFILE h=_lopen(path,(int)(mode&0x73));
    if(h==HFILE_ERROR) return (unsigned)GetLastError();
    *fd=h; return 0;
}
unsigned _dos_creat(const char *path,unsigned attrib,int *fd) {
    HFILE h=_lcreat(path,(int)attrib);
    if(h==HFILE_ERROR) return (unsigned)GetLastError();
    *fd=h; return 0;
}
unsigned _dos_close(int fd) {return _lclose(fd)==HFILE_ERROR?6:0;}
unsigned _dos_read(int fd,void *b,unsigned n,unsigned *got) {UINT r=_lread(fd,b,n); if(r==(UINT)HFILE_ERROR) return 6; *got=r; return 0;}
unsigned _dos_write(int fd,const void *b,unsigned n,unsigned *done) {UINT r=_lwrite(fd,b,n); if(r==(UINT)HFILE_ERROR) return 6; *done=r; return 0;}
void _dos_getdate(struct _dosdate_t *d) {SYSTEMTIME t; GetLocalTime(&t); d->day=(unsigned char)t.wDay; d->month=(unsigned char)t.wMonth; d->year=t.wYear; d->dayofweek=(unsigned char)t.wDayOfWeek;}
void _dos_gettime(struct _dostime_t *d) {SYSTEMTIME t; GetLocalTime(&t); d->hour=(unsigned char)t.wHour; d->minute=(unsigned char)t.wMinute; d->second=(unsigned char)t.wSecond; d->hsecond=(unsigned char)(t.wMilliseconds/10);}
unsigned _dos_getdiskfree(unsigned drive,struct _diskfree_t *d) {
    char root[4]; DWORD spc,bps,free_clusters,total;
    if(!drive) drive=(unsigned)_getdrive();
    root[0]=(char)('A'+drive-1); root[1]=':'; root[2]='\\'; root[3]=0;
    if(!GetDiskFreeSpace(root,&spc,&bps,&free_clusters,&total)) return 15;
    d->total_clusters=total; d->avail_clusters=free_clusters; d->sectors_per_cluster=spc; d->bytes_per_sector=bps;
    return 0;
}
void _dos_getdrive(unsigned *drive) {*drive=(unsigned)_getdrive();}
void _dos_setdrive(unsigned drive,unsigned *count) {_chdrive((int)drive); *count=26;}
unsigned _dos_getfileattr(const char *path,unsigned *attrib) {
    DWORD a=GetFileAttributes(path);
    if(a==0xffffffffU) return 2;
    *attrib=(unsigned)a; return 0;
}
unsigned _dos_setfileattr(const char *path,unsigned attrib) {return SetFileAttributes(path,attrib)?0:2;}
