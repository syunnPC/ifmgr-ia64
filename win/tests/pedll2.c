/* SPDX-License-Identifier: GPL-2.0-or-later
 * PEDLL2.DLL: the other half of an import cycle with PEDLL.DLL.
 */
#define PEDLL2_BUILD
#include "wintest.h"
int pedll2_value(void) {return 1234;}
int pedll2_sum(int a) {return pedll_add(a,0);}
int DllMain(void *instance,unsigned reason,void *reserved) {
    (void)instance; (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) host_trace("PEDLL2 attach");
    else if(reason==DLL_PROCESS_DETACH) host_trace("PEDLL2 detach");
    return 1;
}
