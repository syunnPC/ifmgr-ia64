/* SPDX-License-Identifier: GPL-2.0-or-later
 * Streams over KERNEL's files (win/crt/stdio.c). A Windows program has no
 * console: stdout and stderr go to OutputDebugString a line at a time, and
 * stdin reads as at its end. Text streams turn CR LF into LF when read and
 * LF into CR LF when written. */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
#define BUFSIZ 512
#define FILENAME_MAX 260
#define FOPEN_MAX 20
#define L_tmpnam 13
#define TMP_MAX 32767
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 0x40
#define _IONBF 4
typedef struct _iobuf FILE;
typedef long fpos_t;
extern FILE _iob[];
#define stdin (&_iob[0])
#define stdout (&_iob[1])
#define stderr (&_iob[2])
FILE *fopen(const char *,const char *);
FILE *freopen(const char *,const char *,FILE *);
FILE *fdopen(int,const char *);
#define _fdopen fdopen
int fclose(FILE *);
int fflush(FILE *);
int fcloseall(void);
#define _fcloseall fcloseall
int flushall(void);
#define _flushall flushall
void setbuf(FILE *,char *);
int setvbuf(FILE *,char *,int,size_t);
size_t fread(void *,size_t,size_t,FILE *);
size_t fwrite(const void *,size_t,size_t,FILE *);
int fgetc(FILE *);
int fputc(int,FILE *);
int getc(FILE *);
int putc(int,FILE *);
int getchar(void);
int putchar(int);
int ungetc(int,FILE *);
char *fgets(char *,int,FILE *);
int fputs(const char *,FILE *);
char *gets(char *);
int puts(const char *);
int fseek(FILE *,long,int);
long ftell(FILE *);
void rewind(FILE *);
int fgetpos(FILE *,fpos_t *);
int fsetpos(FILE *,const fpos_t *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fileno(FILE *);
#define _fileno fileno
int printf(const char *,...);
int fprintf(FILE *,const char *,...);
int sprintf(char *,const char *,...);
int _snprintf(char *,size_t,const char *,...);
int vprintf(const char *,va_list);
int vfprintf(FILE *,const char *,va_list);
int vsprintf(char *,const char *,va_list);
int _vsnprintf(char *,size_t,const char *,va_list);
int scanf(const char *,...);
int fscanf(FILE *,const char *,...);
int sscanf(const char *,const char *,...);
int remove(const char *);
int rename(const char *,const char *);
char *tmpnam(char *);
FILE *tmpfile(void);
void perror(const char *);
#endif
