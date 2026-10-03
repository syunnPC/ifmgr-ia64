/* SPDX-License-Identifier: GPL-2.0-or-later
 * PETEST.EXE: exercises the PE dynamic linker from WDK-built code.
 */
#include "wintest.h"
typedef int (*Mul)(int,int);
static unsigned failures;
static void check(const char *what,int ok) {
    if(!ok) {host_trace(what); failures++;}
}
int petest_main(void) {
    void *dyn,*self; Mul mul; const char *text; unsigned size;
    host_trace("PETEST start");
    check("FAIL LLP64",sizeof(long)==4 && sizeof(void *)==8 && sizeof(__int64)==8);
    check("FAIL import by name",pedll_add(2,3)==5);
    check("FAIL import by ordinal",pedll_twice(21)==42);
    check("FAIL data import",pedll_counter==7);
    check("FAIL forwarder",pedll_forward()==1234);
    check("FAIL built-in data",host_value==42);
    dyn=host_load("PEDYN");
    check("FAIL LoadLibrary",dyn!=0);
    if(dyn) {
        mul=(Mul)host_proc(dyn,"pedyn_mul");
        check("FAIL GetProcAddress",mul!=0 && mul(6,7)==42);
        check("FAIL GetProcAddress by ordinal",host_proc_ordinal(dyn,1)==(const void *)mul);
        check("FAIL missing procedure",host_proc(dyn,"nothing")==0);
        check("FAIL second LoadLibrary",host_load("pedyn.dll")==dyn && host_free(dyn)==0);
        check("FAIL FreeLibrary",host_free(dyn)==0);
    }
    check("FAIL missing DLL",host_load("NOSUCH")==0);
    check("FAIL refused DllMain",host_load("PEFAIL")==0);
    self=host_module(0);
    text=(const char *)host_resource(self,10,0,1,0,&size);
    check("FAIL RCDATA",text!=0 && size>=5 && text[0]=='h' && text[4]=='o');
    text=(const char *)host_resource(self,0,"TEXT",0,"greeting",&size);
    check("FAIL named resource",text!=0 && size>=14 && text[0]=='n' && text[13]=='e');
    check("FAIL GetModuleHandle",host_module("pedll")!=0 && host_module("PEDYN")==0);
    host_trace(failures?"PETEST failed":"PETEST passed");
    return failures?100+(int)failures:0;
}
