/* SPDX-License-Identifier: GPL-2.0-or-later
 * PE loader test modules (WDK cl, C89). HOSTTEST.DLL is a built-in module
 * provided by PELOAD.EFI.
 */
#ifndef WINTEST_H
#define WINTEST_H
#define IMPORT __declspec(dllimport)
IMPORT void host_trace(const char *text);
IMPORT void *host_load(const char *name);
IMPORT const void *host_proc(void *module,const char *name);
IMPORT const void *host_proc_ordinal(void *module,unsigned ordinal);
IMPORT int host_free(void *module);
IMPORT void *host_module(const char *name);
IMPORT const void *host_resource(void *module,unsigned type_id,const char *type,
                                 unsigned name_id,const char *name,unsigned *size);
IMPORT extern int host_value;
#ifndef PEDLL_BUILD
IMPORT int pedll_add(int a,int b);
IMPORT int pedll_twice(int a); /* exported by ordinal only */
IMPORT extern int pedll_counter;
IMPORT int pedll_forward(void); /* forwarded to PEDLL2.pedll2_value */
#endif
#ifndef PEDLL2_BUILD
IMPORT int pedll2_value(void);
#endif
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#endif
