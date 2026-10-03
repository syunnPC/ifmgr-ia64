/* SPDX-License-Identifier: GPL-2.0-or-later
 * The C run-time library (win/crt): general utilities, with Microsoft C's
 * additions that Windows 3.0 programs use. */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fff
#define _MAX_PATH 260
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256
typedef struct {int quot,rem;} div_t;
typedef struct {long quot,rem;} ldiv_t;
void *malloc(size_t);
void *calloc(size_t,size_t);
void *realloc(void *,size_t);
void free(void *);
double atof(const char *);
int atoi(const char *);
long atol(const char *);
double strtod(const char *,char **);
long strtol(const char *,char **,int);
unsigned long strtoul(const char *,char **,int);
char *itoa(int,char *,int);
char *ltoa(long,char *,int);
char *ultoa(unsigned long,char *,int);
#define _itoa itoa
#define _ltoa ltoa
#define _ultoa ultoa
char *gcvt(double,int,char *);
char *ecvt(double,int,int *,int *);
char *fcvt(double,int,int *,int *);
#define _gcvt gcvt
#define _ecvt ecvt
#define _fcvt fcvt
int rand(void);
void srand(unsigned);
int abs(int);
long labs(long);
div_t div(int,int);
ldiv_t ldiv(long,long);
void qsort(void *,size_t,size_t,int (*)(const void *,const void *));
void *bsearch(const void *,const void *,size_t,size_t,int (*)(const void *,const void *));
char *getenv(const char *);
int putenv(const char *);
#define _putenv putenv
int system(const char *);
void exit(int);
void _exit(int);
void abort(void);
int atexit(void (*)(void));
void _splitpath(const char *,char *,char *,char *,char *);
void _makepath(char *,const char *,const char *,const char *,const char *);
char *_fullpath(char *,const char *,size_t);
unsigned int _rotl(unsigned int,int);
unsigned int _rotr(unsigned int,int);
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#define __max max
#define __min min
#endif
