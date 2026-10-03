/* SPDX-License-Identifier: GPL-2.0-or-later
 * WinMainCRTStartup: the entry point of applications built with this SDK
 * (link /ENTRY:WinMainCRTStartup). C89.
 */
#include <windows.h>
#include <winstart.h>
void _crt_term(void); /* win/crt: atexit's functions, the streams flushed */
int WinMainCRTStartup(void) {
    WINSTARTINFO info; int code=1;
    if(InitTask(&info) && InitApp(info.hInstance)) code=WinMain(info.hInstance,NULL,info.lpCmdLine,info.nCmdShow);
    _crt_term();
    ExitProcess((UINT)code);
    return code;
}
