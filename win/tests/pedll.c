/* SPDX-License-Identifier: GPL-2.0-or-later
 * PEDLL.DLL: name, ordinal-only, data and forwarded exports. It imports
 * PEDLL2.DLL, which imports it back.
 */
#define PEDLL_BUILD
#include "wintest.h"
int pedll_counter;
int pedll_add(int a,int b) {return a+b+pedll2_value()-1234;}
int pedll_twice(int a) {return 2*a;}
int DllMain(void *instance,unsigned reason,void *reserved) {
    (void)instance; (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) {host_trace("PEDLL attach"); pedll_counter=7;}
    else if(reason==DLL_PROCESS_DETACH) host_trace("PEDLL detach");
    return 1;
}
