/* SPDX-License-Identifier: GPL-2.0-or-later
 * PELOAD [path]: loads a WDK-built application with the PE dynamic linker
 * (win/host/pe.c) and calls its entry, int entry(void). The built-in module
 * HOSTTEST.DLL gives test images tracing and LoadLibrary-style calls.
 * The default path is C:\WINTEST\PETEST.EXE; dependents are searched in the
 * application's directory and C:\WINTEST.
 */
#include "env.h"
static PeLoader loader;
static PeModule *exe;
static const IoServices *io;

static void host_trace(const char *text) {print("PETRACE: %s\n",text);}
static void *host_load(const char *name) {
    PeModule *m; int e=pe_load(&loader,name,exe?exe->dir:NULL,&m);
    if(e) print("PELOAD: LoadLibrary %s failed (%u) at %s %s\n",name,(unsigned long long)e,loader.error_module,loader.error_symbol);
    return e?NULL:m;
}
static const void *host_proc(void *m,const char *name) {return pe_valid(&loader,m)?pe_proc(&loader,m,name):NULL;}
static const void *host_proc_ordinal(void *m,unsigned ordinal) {return pe_valid(&loader,m)?pe_proc_ordinal(&loader,m,ordinal):NULL;}
static int host_free(void *m) {return pe_free(&loader,m);}
static void *host_module(const char *name) {return name?(void *)pe_find(&loader,name):(void *)exe;}
static const void *host_resource(void *m,unsigned type_id,const char *type,unsigned name_id,const char *name,unsigned *size) {
    const void *data; u32 bytes;
    if(!pe_valid(&loader,m) || pe_resource(m,(PeResId){type_id,type},(PeResId){name_id,name},0,&data,&bytes)) return NULL;
    *size=bytes; return data;
}
static int host_value=42;
static const PeExport host_exports[]={
    {"host_trace",1,host_trace},{"host_load",2,host_load},{"host_proc",3,host_proc},
    {"host_proc_ordinal",4,host_proc_ordinal},{"host_free",5,host_free},{"host_module",6,host_module},
    {"host_resource",7,host_resource},{"host_value",8,&host_value},
};

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    io=dos_io_services();
    if(!io || !io->alloc_pages) {print("PELOAD: no page services\n"); return 1;}
    char path[DOS_PATH_MAX]; const char *tail=app_dos->command_tail();
    while(*tail==' ') tail++;
    strcopy(path,sizeof(path),*tail?tail:"C:\\WINTEST\\PETEST.EXE");
    for(char *p=path;*p;p++) if(*p==' ') {*p=0; break;}
    PeEnv env; host_env(&env,io,"C:\\WINTEST");
    pe_init(&loader,&env);
    if(pe_builtin(&loader,"HOSTTEST",host_exports,ARRAY_SIZE(host_exports),NULL)) {print("PELOAD: built-in failed\n"); return 1;}
    int e=pe_load_exe(&loader,path,&exe), result=1;
    if(e) print("PELOAD: %s failed (%u) at %s %s\n",path,(unsigned long long)e,loader.error_module,loader.error_symbol);
    else {
        result=((int (*)(void))(uintptr_t)exe->entry)();
        print("PELOAD: exit %u\n",(unsigned long long)result);
        e=pe_free(&loader,exe); exe=NULL;
        unsigned left=0; for(unsigned i=0;i<PE_MODULES_MAX;i++) if(loader.modules[i].state!=PE_FREE) left++;
        print("PELOAD: free %u, %u modules remain\n",(unsigned long long)e,(unsigned long long)left);
    }
    pe_shutdown(&loader);
    print("PELOAD: %u pages and %u files outstanding\n",(unsigned long long)host_pages_out,(unsigned long long)host_files_out);
    return result || host_pages_out || host_files_out?1:0;
}
