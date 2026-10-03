/* SPDX-License-Identifier: GPL-2.0-or-later
 * Streams: a buffer over a low-level handle (io.c). Text streams translate
 * in the buffer (CR LF to LF, ^Z as the end, LF to CR LF), so ftell and
 * fseek count the file's own bytes. stdout and stderr collect a line for
 * OutputDebugString; stdin is at its end. C89. */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include "crtp.h"
FILE _iob[FOPEN_MAX]={
    {-1,F_USED|F_READ|F_EOF,EOF},
    {-1,F_USED|F_WRITE|F_DEBUG,EOF},
    {-1,F_USED|F_WRITE|F_DEBUG,EOF}};
static void init(FILE *f) {if(!f->buf) {f->buf=f->own; f->size=BUFSIZ;}}
/* A line of debug output, at its LF or when the buffer is full. */
static void debug_out(FILE *f) {
    char line[BUFSIZ+1];
    if(!f->pos) return;
    memcpy(line,f->buf,f->pos); line[f->pos]=0; f->pos=0;
    OutputDebugString(line);
}
static int flush_write(FILE *f) {
    unsigned done=0;
    if(!(f->flags&F_DIRTY)) return 0;
    f->flags&=~F_DIRTY;
    if(f->flags&F_DEBUG) {debug_out(f); return 0;}
    while(done<f->pos) {
        int n=write(f->fd,f->buf+done,f->pos-done);
        if(n<=0) {f->flags|=F_ERR; f->pos=0; return EOF;}
        done+=(unsigned)n;
    }
    f->pos=0; return 0;
}
/* Before writing after reading: the file position back to where reading is. */
static void drop_read(FILE *f) {
    if(f->len>f->pos && f->fd>=0) lseek(f->fd,-(long)(f->len-f->pos),SEEK_CUR);
    f->pos=f->len=0; f->ungot=EOF;
}
static int fill(FILE *f) {
    int n;
    if(f->flags&F_EOF || f->fd<0) {f->flags|=F_EOF; return EOF;}
    init(f);
    n=read(f->fd,f->buf,f->size);
    if(n<0) {f->flags|=F_ERR; return EOF;}
    if(!n) {f->flags|=F_EOF; return EOF;}
    f->pos=0; f->len=(unsigned)n; return 0;
}
static int raw_getc(FILE *f) {
    if(f->pos>=f->len && fill(f)) return EOF;
    return (unsigned char)f->buf[f->pos++];
}
int fgetc(FILE *f) {
    int c;
    if(!(f->flags&F_READ)) {f->flags|=F_ERR; return EOF;}
    if(f->flags&F_DIRTY) flush_write(f);
    if(f->ungot!=EOF) {c=f->ungot; f->ungot=EOF; return c;}
    c=raw_getc(f);
    if(f->flags&F_TEXT) {
        if(c==26) {f->flags|=F_EOF; f->pos--; return EOF;}
        if(c=='\r') {
            int d=raw_getc(f);
            if(d=='\n') return '\n';
            if(d!=EOF) f->pos--;
        }
    }
    return c;
}
int getc(FILE *f) {return fgetc(f);}
int getchar(void) {return fgetc(stdin);}
int ungetc(int c,FILE *f) {
    if(c==EOF || f->ungot!=EOF) return EOF;
    f->ungot=(unsigned char)c; f->flags&=~F_EOF; return (unsigned char)c;
}
static int put_raw(FILE *f,char c) {
    init(f);
    if(f->pos>=f->size && flush_write(f)) return EOF;
    f->buf[f->pos++]=c; f->flags|=F_DIRTY;
    return 0;
}
int fputc(int c,FILE *f) {
    if(!(f->flags&F_WRITE)) {f->flags|=F_ERR; return EOF;}
    if(!(f->flags&F_DIRTY) && f->len) drop_read(f);
    if((f->flags&F_TEXT) && c=='\n' && put_raw(f,'\r')) return EOF;
    if(put_raw(f,(char)c)) return EOF;
    if((f->flags&F_NOBUF) || (c=='\n' && (f->flags&(F_LINE|F_DEBUG)))) {if(flush_write(f)) return EOF;}
    return (unsigned char)c;
}
int putc(int c,FILE *f) {return fputc(c,f);}
int putchar(int c) {return fputc(c,stdout);}
static FILE *slot(void) {
    int i;
    for(i=3;i<FOPEN_MAX;i++) if(!(_iob[i].flags&F_USED)) {memset(&_iob[i],0,sizeof(FILE)); _iob[i].ungot=EOF; return &_iob[i];}
    errno=EMFILE; return NULL;
}
/* fopen's mode: r w a, then + and b or t in any order. */
static int mode_flags(const char *mode,int *oflag) {
    int flags,o; const char *m;
    switch(mode[0]) {
    case 'r': flags=F_READ; o=O_RDONLY; break;
    case 'w': flags=F_WRITE; o=O_WRONLY|O_CREAT|O_TRUNC; break;
    case 'a': flags=F_WRITE; o=O_WRONLY|O_CREAT|O_APPEND; break;
    default: errno=EINVAL; return -1;
    }
    o|=O_TEXT;
    for(m=mode+1;*m;m++) {
        if(*m=='+') {flags|=F_READ|F_WRITE; o=(o&~(O_WRONLY|O_RDONLY))|O_RDWR;}
        else if(*m=='b') o=(o&~O_TEXT)|O_BINARY;
        else if(*m=='t') o=(o&~O_BINARY)|O_TEXT;
    }
    if(o&O_TEXT) flags|=F_TEXT;
    *oflag=o; return flags;
}
static FILE *attach(FILE *f,int fd,int flags) {
    f->fd=fd; f->flags=flags|F_USED; f->ungot=EOF; f->buf=NULL; f->pos=f->len=0;
    setmode(fd,O_BINARY); /* translated here, not again below */
    return f;
}
FILE *fopen(const char *path,const char *mode) {
    int o,flags=mode_flags(mode,&o),fd; FILE *f;
    if(flags<0 || !(f=slot())) return NULL;
    fd=open(path,o); if(fd<0) return NULL;
    return attach(f,fd,flags);
}
FILE *fdopen(int fd,const char *mode) {
    int o,flags=mode_flags(mode,&o); FILE *f;
    if(flags<0 || !(f=slot())) return NULL;
    return attach(f,fd,flags);
}
int fflush(FILE *f) {
    if(!f) {_crt_flush_all(); return 0;}
    if(f->flags&F_DIRTY) return flush_write(f);
    if(f->len) drop_read(f);
    return 0;
}
int fclose(FILE *f) {
    int r;
    if(!(f->flags&F_USED)) return EOF;
    r=fflush(f);
    if(f->fd>=0 && close(f->fd)) r=EOF;
    if(f->buf && f->buf!=f->own) free(f->buf);
    f->flags=0; f->fd=-1; f->buf=NULL;
    return r;
}
FILE *freopen(const char *path,const char *mode,FILE *f) {
    int o,flags=mode_flags(mode,&o),fd;
    if(f->flags&F_USED) fclose(f);
    if(flags<0) return NULL;
    fd=open(path,o); if(fd<0) return NULL;
    return attach(f,fd,flags);
}
int fcloseall(void) {int i,n=0; for(i=3;i<FOPEN_MAX;i++) if(_iob[i].flags&F_USED) {fclose(&_iob[i]); n++;} return n;}
void _crt_flush_all(void) {int i; for(i=0;i<FOPEN_MAX;i++) if(_iob[i].flags&F_USED) fflush(&_iob[i]);}
int flushall(void) {_crt_flush_all(); return FOPEN_MAX;}
int setvbuf(FILE *f,char *buf,int mode,size_t size) {
    if(f->pos || f->len) return EOF;
    f->flags&=~(F_NOBUF|F_LINE);
    if(mode==_IONBF) {f->flags|=F_NOBUF; f->buf=f->own; f->size=1; return 0;}
    if(mode==_IOLBF) f->flags|=F_LINE;
    if(buf && size>1) {f->buf=buf; f->size=(unsigned)size;}
    return 0;
}
void setbuf(FILE *f,char *buf) {setvbuf(f,buf,buf?_IOFBF:_IONBF,BUFSIZ);}
size_t fread(void *b,size_t size,size_t n,FILE *f) {
    char *p=(char *)b; size_t i,total=size*n;
    if(!size || !n) return 0;
    for(i=0;i<total;i++) {int c=fgetc(f); if(c==EOF) break; p[i]=(char)c;}
    return i/size;
}
size_t fwrite(const void *b,size_t size,size_t n,FILE *f) {
    const char *p=(const char *)b; size_t i,total=size*n;
    if(!size || !n) return 0;
    for(i=0;i<total;i++) if(fputc((unsigned char)p[i],f)==EOF) break;
    return i/size;
}
char *fgets(char *s,int n,FILE *f) {
    int i=0,c=EOF;
    if(n<=0) return NULL;
    while(i<n-1 && (c=fgetc(f))!=EOF) {s[i++]=(char)c; if(c=='\n') break;}
    if(!i && c==EOF) return NULL;
    s[i]=0; return s;
}
char *gets(char *s) {
    int i=0,c;
    while((c=fgetc(stdin))!=EOF && c!='\n') s[i++]=(char)c;
    if(!i && c==EOF) return NULL;
    s[i]=0; return s;
}
int fputs(const char *s,FILE *f) {for(;*s;s++) if(fputc((unsigned char)*s,f)==EOF) return EOF; return 0;}
int puts(const char *s) {return fputs(s,stdout)==EOF || fputc('\n',stdout)==EOF?EOF:0;}
long ftell(FILE *f) {
    long here;
    if(f->fd<0) {errno=EBADF; return -1;}
    here=lseek(f->fd,0,SEEK_CUR);
    if(here<0) return -1;
    if(f->flags&F_DIRTY) return here+(long)f->pos;
    return here-(long)(f->len-f->pos)-(f->ungot!=EOF?1:0);
}
int fseek(FILE *f,long offset,int whence) {
    if(fflush(f)) return -1; /* SEEK_CUR: fflush put the file position where reading was */
    if(f->fd<0 || lseek(f->fd,offset,whence)<0) return -1;
    f->flags&=~F_EOF; f->ungot=EOF; f->pos=f->len=0;
    return 0;
}
void rewind(FILE *f) {fseek(f,0,SEEK_SET); f->flags&=~F_ERR;}
int fgetpos(FILE *f,fpos_t *p) {*p=ftell(f); return *p<0?-1:0;}
int fsetpos(FILE *f,const fpos_t *p) {return fseek(f,*p,SEEK_SET);}
int feof(FILE *f) {return (f->flags&F_EOF)!=0 && f->ungot==EOF && f->pos>=f->len;}
int ferror(FILE *f) {return (f->flags&F_ERR)!=0;}
void clearerr(FILE *f) {f->flags&=~(F_ERR|F_EOF);}
int fileno(FILE *f) {return f->fd;}
static int to_stream(void *context,const char *p,unsigned n) {
    FILE *f=(FILE *)context;
    while(n--) if(fputc((unsigned char)*p++,f)==EOF) return -1;
    return 0;
}
int vfprintf(FILE *f,const char *format,va_list ap) {_Sink s; s.put=to_stream; s.context=f; return _format(&s,format,ap);}
int vprintf(const char *format,va_list ap) {return vfprintf(stdout,format,ap);}
int fprintf(FILE *f,const char *format,...) {va_list ap; int n; va_start(ap,format); n=vfprintf(f,format,ap); va_end(ap); return n;}
int printf(const char *format,...) {va_list ap; int n; va_start(ap,format); n=vfprintf(stdout,format,ap); va_end(ap); return n;}
static int from_stream(void *context) {return fgetc((FILE *)context);}
static void back_to_stream(void *context,int c) {ungetc(c,(FILE *)context);}
int fscanf(FILE *f,const char *format,...) {
    _Source s; va_list ap; int n;
    s.get=from_stream; s.unget=back_to_stream; s.context=f;
    va_start(ap,format); n=_scan(&s,format,ap); va_end(ap); return n;
}
int scanf(const char *format,...) {
    _Source s; va_list ap; int n;
    s.get=from_stream; s.unget=back_to_stream; s.context=stdin;
    va_start(ap,format); n=_scan(&s,format,ap); va_end(ap); return n;
}
char *tmpnam(char *s) {
    static char name[L_tmpnam+260]; static unsigned n;
    char *out=s?s:name;
    sprintf(out,"TMP%05u.$$$",++n);
    return out;
}
FILE *tmpfile(void) {return fopen(tmpnam(NULL),"w+b");}
void perror(const char *s) {
    if(s && *s) fprintf(stderr,"%s: ",s);
    fprintf(stderr,"%s\n",strerror(errno));
}
