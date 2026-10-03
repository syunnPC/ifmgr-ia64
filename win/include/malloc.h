/* SPDX-License-Identifier: GPL-2.0-or-later
 * The heap, with Windows 3.0's near and far forms as the one heap. */
#ifndef _MALLOC_H
#define _MALLOC_H
#include <stdlib.h>
size_t _msize(void *);
void *_expand(void *,size_t);
/* Heap checks. */
#define _HEAPEMPTY (-1)
#define _HEAPOK (-2)
#define _HEAPBADBEGIN (-3)
#define _HEAPBADNODE (-4)
#define _HEAPEND (-5)
#define _HEAPBADPTR (-6)
#define _USEDENTRY 0
#define _FREEENTRY 1
typedef struct _heapinfo {int *_pentry; size_t _size; int _useflag;} _HEAPINFO;
int _heapchk(void);
int _heapset(unsigned);
int _heapmin(void);
int _heapwalk(_HEAPINFO *);
#define _fheapchk _heapchk
#define _nheapchk _heapchk
#define _fheapset _heapset
#define _nheapset _heapset
#define _fheapmin _heapmin
#define _nheapmin _heapmin
#define _fheapwalk _heapwalk
#define _nheapwalk _heapwalk
#define _fmalloc malloc
#define _nmalloc malloc
#define _fcalloc calloc
#define _ncalloc calloc
#define _frealloc realloc
#define _nrealloc realloc
#define _ffree free
#define _nfree free
#define _fmsize _msize
#define _nmsize _msize
#define _fexpand _expand
#define _nexpand _expand
#define halloc(n,s) calloc((size_t)(n),(s))
#define hfree free
#define alloca _alloca
void *_alloca(size_t);
#endif
