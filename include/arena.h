/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOS_ARENA_H
#define DOS_ARENA_H
#include "base.h"
typedef struct {u8 *base; u32 paragraphs; u8 method;} Arena;
void arena_init(Arena *, void *, u32);
int arena_check(Arena *, u32 *);
int arena_alloc(Arena *, u32, u32, void **, u32 *);
int arena_free(Arena *, void *, u32);
int arena_resize(Arena *, void *, u32, u32, u32 *);
int arena_free_process(Arena *, u32);
int arena_block(Arena *, u32, u32 *, u32 *, u32 *);
#endif
