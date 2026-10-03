/* SPDX-License-Identifier: GPL-2.0-or-later
 * Cooperative fibers for native IA-64 code: each fiber owns a memory stack
 * and a register-stack backing store. Switches happen only at explicit calls
 * (for DOS work, at InDOS == 0); there is no preemption.
 */
#ifndef DOS_FIBER_H
#define DOS_FIBER_H
#include "base.h"
/* Preserved state of the IA-64 software conventions. Offsets are fixed for
 * sdk/fiber_ia64.S: sp gp r4-r7 unat(spill,user) b0-b5 pfs lc fpsr pr rsc
 * bspstore rnat, then f2-f5 and f16-f31 in spill format. */
typedef struct {
    u64 sp,gp,r4,r5,r6,r7,unat_spill,unat_user;
    u64 b0,b1,b2,b3,b4,b5;
    u64 pfs,lc,fpsr,pr,rsc,bspstore,rnat,reserved;
    _Alignas(16) u8 fp[20][16];
} FiberContext;
_Static_assert(sizeof(FiberContext)==496,"fiber context layout");
/* Saves the caller into from and resumes to. Returns when someone switches
 * back to from. from and to must differ. */
void fiber_switch(FiberContext *from,const FiberContext *to);
/* entry(arg) runs on the new stacks at the first switch and must not return.
 * Both areas must stay valid while the fiber can run; sizes are bytes. */
int fiber_prepare(FiberContext *,void *stack,size_t stack_bytes,
                  void *backing,size_t backing_bytes,void (*entry)(void *),void *arg);
#endif
