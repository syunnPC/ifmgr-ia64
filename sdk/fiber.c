/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fiber.h"
void fiber_trampoline(void);
u64 fiber_rsc(void);
/* The IA-64 calling convention's default FPSR: IEEE rounding, traps off. */
#define FIBER_FPSR 0x0009804c0270033fULL
int fiber_prepare(FiberContext *c,void *stack,size_t stack_bytes,
                  void *backing,size_t backing_bytes,void (*entry)(void *),void *arg) {
    if(!c || !stack || !backing || !entry || stack_bytes<4096 || backing_bytes<4096) return DE_FUNCTION;
    memset(c,0,sizeof(*c));
    /* IA-64 function pointers address {entry, gp} descriptors. */
    const u64 *trampoline=(const u64 *)(uintptr_t)fiber_trampoline;
    c->sp=((uintptr_t)stack+stack_bytes-16)&~(u64)15; /* 16-byte scratch area */
    c->bspstore=((uintptr_t)backing+7)&~(u64)7;     /* grows upward */
    c->b0=trampoline[0]; c->gp=trampoline[1];
    c->r4=(uintptr_t)arg; c->r5=(uintptr_t)entry;
    c->rsc=(fiber_rsc()&~(0x3fffULL<<16))|3; /* eager mode, loadrs 0 */
    c->fpsr=FIBER_FPSR; c->pr=1;
    return 0;
}
