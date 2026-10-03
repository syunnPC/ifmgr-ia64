/* SPDX-License-Identifier: GPL-2.0-or-later
 * The C run-time library's own declarations. */
#ifndef CRTP_H
#define CRTP_H
#include <windows.h>
#include <stdio.h>
/* A stream: a KERNEL file handle and its buffer. */
struct _iobuf {
    int fd,flags,ungot;
    char *buf; unsigned size,pos,len; /* the buffer, where in it, how much was read */
    char own[BUFSIZ];
};
#define F_READ 1
#define F_WRITE 2
#define F_TEXT 4
#define F_EOF 8
#define F_ERR 16
#define F_DEBUG 64 /* stdout or stderr: OutputDebugString */
#define F_DIRTY 256 /* the buffer holds what was written */
#define F_USED 512
#define F_NOBUF 1024
#define F_LINE 2048
extern void _crt_flush_all(void);
/* The formatting engine: output through a sink, which takes a run of characters. */
typedef struct {int (*put)(void *,const char *,unsigned); void *context; long count;} _Sink;
int _format(_Sink *,const char *,va_list);
/* Input for scanf: a character at a time, one put back. */
typedef struct {int (*get)(void *); void (*unget)(void *,int); void *context;} _Source;
int _scan(_Source *,const char *,va_list);
#endif
