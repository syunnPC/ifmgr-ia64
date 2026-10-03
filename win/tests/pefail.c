/* SPDX-License-Identifier: GPL-2.0-or-later
 * PEFAIL.DLL: its DllMain refuses to attach, so loading it fails.
 */
#include "wintest.h"
int pefail_unused(void) {return 0;}
int DllMain(void *instance,unsigned reason,void *reserved) {
    (void)instance; (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) host_trace("PEFAIL attach refused");
    return reason!=DLL_PROCESS_ATTACH;
}
