/* SPDX-License-Identifier: GPL-2.0-or-later
 * PEDYN.DLL: loaded and freed at run time.
 */
#include "wintest.h"
int pedyn_mul(int a,int b) {return a*b;}
int DllMain(void *instance,unsigned reason,void *reserved) {
    (void)instance; (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) host_trace("PEDYN attach");
    else if(reason==DLL_PROCESS_DETACH) host_trace("PEDYN detach");
    return 1;
}
