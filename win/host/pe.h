/* SPDX-License-Identifier: GPL-2.0-or-later
 * PE32+ (IA-64) module loader and dynamic linker for the Windows layer.
 *
 * Modules are built by the WDK's IA-64 cl/link. In these images an exported
 * function, an IAT slot and AddressOfEntryPoint all hold the address of a
 * function descriptor {entry, gp}, which is also what a GCC function pointer
 * is, so code from both compilers calls through the same values. Images must
 * be relocatable (link /FIXED:NO): preferred bases are far above guest RAM.
 *
 * DLLs are shared by every task and counted: one reference per explicit load
 * and one per importing module. Static loads link a whole group first (so
 * cyclic imports resolve), then run DllMain in dependency order; any failure
 * unwinds the group. Modules in an import cycle stay until pe_shutdown.
 * Built-in modules are export tables supplied by the host.
 */
#ifndef WIN_PE_H
#define WIN_PE_H
#include "base.h"
#define PE_NAME_MAX 32
#define PE_MODULES_MAX 64
#define PE_IMPORTS_MAX 32
#define PE_ALIASES_MAX 16
#define PE_DLL_PROCESS_DETACH 0U
#define PE_DLL_PROCESS_ATTACH 1U
typedef struct {const char *name; u16 ordinal; const void *address;} PeExport;
typedef struct {
    void *ctx;
    /* Zeroed memory, at least 4 KiB aligned. */
    int (*alloc)(void *ctx,u64 bytes,void **out);
    void (*free)(void *ctx,void *memory,u64 bytes);
    /* A whole file; DE_NOFILE or DE_PATH when it does not exist. */
    int (*read_file)(void *ctx,const char *path,void **data,u32 *size);
    void (*free_file)(void *ctx,void *data,u32 size);
    /* Make freshly written code visible to instruction fetch. */
    void (*flush_code)(void *ctx,void *memory,u64 bytes);
    /* DllMain(instance, reason, NULL) through a descriptor; zero is failure. */
    int (*dll_main)(void *ctx,const void *descriptor,void *instance,u32 reason);
    /* Searched after the importer's directory; may be NULL. */
    const char *system_dir;
} PeEnv;
enum {PE_FREE,PE_MAPPED,PE_LINKED,PE_READY,PE_UNLOADING};
#define PE_FLAG_DLL 1U
#define PE_FLAG_BUILTIN 2U
#define PE_FLAG_INITIALIZED 4U
typedef struct PeModule {
    u32 state,flags,refs,order;
    char name[PE_NAME_MAX];       /* upper-case base name, e.g. USER32.DLL */
    char dir[DOS_PATH_MAX];       /* directory it was loaded from, with '\' */
    u8 *base; u64 size;           /* mapped image */
    void *memory; u64 memory_bytes;
    const void *entry;            /* descriptor, or NULL */
    u16 subsystem,characteristics;
    u32 export_rva,export_size,resource_rva,resource_size;
    const PeExport *exports; u32 export_count; /* built-in modules */
    struct PeModule *imports[PE_IMPORTS_MAX];
    u32 import_count;
} PeModule;
typedef struct {
    PeEnv env;
    PeModule modules[PE_MODULES_MAX];
    PeModule *group[PE_MODULES_MAX]; /* modules of the load in progress */
    PeModule *visiting[PE_MODULES_MAX]; /* DllMain ordering */
    u32 group_count,visit_count,next_order;
    int lookup_only; /* GetProcAddress: forwarders must name loaded modules */
    /* Other names of a module, e.g. KERNEL32.DLL for KERNEL.DLL. */
    char alias[PE_ALIASES_MAX][2][PE_NAME_MAX];
    u32 alias_count;
    /* What failed last: a module name and, for imports, the symbol. */
    char error_module[PE_NAME_MAX],error_symbol[64];
} PeLoader;
/* Resource type or name: a string when name is set, else an integer ID. */
typedef struct {u32 id; const char *name;} PeResId;

void pe_init(PeLoader *,const PeEnv *);
int pe_builtin(PeLoader *,const char *name,const PeExport *,u32 count,PeModule **);
/* LoadLibrary: a path, or a name searched in dir (may be NULL), the system
 * directory and the current directory. A name without '.' gets ".DLL". */
int pe_load(PeLoader *,const char *name,const char *dir,PeModule **);
/* An application image: never shared; its entry descriptor is pe->entry. */
int pe_load_exe(PeLoader *,const char *path,PeModule **);
int pe_free(PeLoader *,PeModule *);
/* Detach and release everything, last initialized first. */
void pe_shutdown(PeLoader *);
PeModule *pe_find(PeLoader *,const char *name);
/* Names resolve to target for imports, loads and lookups. */
int pe_alias(PeLoader *,const char *alias,const char *target);
/* The module whose image starts at base (HINSTANCE/HMODULE), or NULL. */
PeModule *pe_from_base(PeLoader *,const void *base);
/* A handle from untrusted code: a loaded module of this loader. */
int pe_valid(const PeLoader *,const PeModule *);
const void *pe_proc(PeLoader *,PeModule *,const char *name);
const void *pe_proc_ordinal(PeLoader *,PeModule *,u32 ordinal);
int pe_resource(const PeModule *,PeResId type,PeResId name,u16 language,const void **data,u32 *size);
/* The 16-byte IMAGE_RESOURCE_DATA_ENTRY {RVA, size, code page, reserved}
 * of a resource; its data was checked to lie inside the image. */
const u8 *pe_resource_entry(const PeModule *,PeResId type,PeResId name,u16 language);
/* IA-64 movl immediate in a 16-byte MLX bundle (exported for tests). */
int pe_movl_get(const u8 bundle[16],u64 *value);
int pe_movl_set(u8 bundle[16],u64 value);
#endif
