/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host tests for the PE dynamic linker (win/host/pe.c) with WDK-built
 * fixtures: mapping, relocation, import/export resolution, DllMain order,
 * unwinding, reference counts, resources and malformed images. IA-64 code
 * cannot run here, so DllMain calls are recorded instead of executed.
 *   pe-test <directory of petest.exe, pedll.dll, ...>
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pe.h"
static unsigned checks;
void con_write(const void *p,size_t n) {fwrite(p,1,n,stdout);}
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"%s:%d: CHECK(%s)\n",__FILE__,__LINE__,#x); exit(1);}} while(0)

static const char *fixture_dir;
typedef struct {char name[16]; unsigned char *data; u32 size;} File;
static File files[16];
static unsigned file_count;
static void load_fixture(const char *name) {
    char path[512]; snprintf(path,sizeof(path),"%s/%s",fixture_dir,name);
    FILE *f=fopen(path,"rb"); if(!f) {perror(path); exit(1);}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    File *x=&files[file_count++]; snprintf(x->name,sizeof(x->name),"%s",name);
    x->data=malloc((size_t)n); x->size=(u32)n;
    CHECK(fread(x->data,1,(size_t)n,f)==(size_t)n); fclose(f);
}
static File *fixture(const char *name) {
    for(unsigned i=0;i<file_count;i++) if(!strcasecmp(files[i].name,name)) return &files[i];
    return NULL;
}

/* Environment: memory accounting, a C:\WINTEST directory, recorded DllMain. */
static unsigned allocations,file_reads,flushes;
static char requests[16][DOS_PATH_MAX];
static unsigned request_count;
static File *override; /* replaces the next file read */
static PeLoader loader;
static char trace[1024];
static const char *refuse; /* module whose DllMain fails to attach */
static int env_alloc(void *ctx,u64 bytes,void **out) {
    (void)ctx; size_t n=(bytes+4095)&~(u64)4095;
    *out=aligned_alloc(4096,n); if(!*out) return DE_NOMEM;
    memset(*out,0x5a,n); allocations++; return 0;
}
static void env_free(void *ctx,void *memory,u64 bytes) {(void)ctx; (void)bytes; CHECK(allocations); allocations--; free(memory);}
static int env_read(void *ctx,const char *path,void **data,u32 *size) {
    (void)ctx;
    if(request_count<16) snprintf(requests[request_count++],DOS_PATH_MAX,"%s",path);
    const char *prefix="C:\\WINTEST\\";
    if(strncasecmp(path,prefix,strlen(prefix))) return DE_PATH;
    File *f=fixture(path+strlen(prefix)); if(!f) return DE_NOFILE;
    if(override) {f=override; override=NULL;}
    *data=malloc(f->size); memcpy(*data,f->data,f->size); *size=f->size;
    file_reads++; return 0;
}
static void env_free_file(void *ctx,void *data,u32 size) {(void)ctx; (void)size; CHECK(file_reads); file_reads--; free(data);}
static void env_flush(void *ctx,void *memory,u64 bytes) {(void)ctx; CHECK(memory && bytes); flushes++;}
static PeModule *owner(const void *p) {
    for(unsigned i=0;i<PE_MODULES_MAX;i++) {
        PeModule *m=&loader.modules[i];
        if(m->state!=PE_FREE && m->base && (const u8 *)p>=m->base && (const u8 *)p<m->base+m->size) return m;
    }
    return NULL;
}
static int fuzzing;
static int env_dll_main(void *ctx,const void *descriptor,void *instance,u32 reason) {
    (void)ctx; PeModule *m=owner(descriptor); CHECK(m && instance==m->base);
    /* The descriptor's code and gp were relocated into the image. */
    u64 code,gp; memcpy(&code,descriptor,8); memcpy(&gp,(const u8 *)descriptor+8,8);
    if(fuzzing) return 1;
    CHECK(owner((const void *)(uintptr_t)code)==m && gp>=(uintptr_t)m->base);
    char line[64]; snprintf(line,sizeof(line),"%s %s;",m->name,reason==PE_DLL_PROCESS_ATTACH?"attach":"detach");
    strcat(trace,line);
    return !(refuse && reason==PE_DLL_PROCESS_ATTACH && !strcmp(m->name,refuse));
}
static int host_value=42;
static void host_function(void) {}
static const PeExport host_exports[]={
    {"host_trace",1,host_function},{"host_load",2,host_function},{"host_proc",3,host_function},
    {"host_proc_ordinal",4,host_function},{"host_free",5,host_function},{"host_module",6,host_function},
    {"host_resource",7,host_function},{"host_value",8,&host_value},
};
static void reset(void) {
    PeEnv env={NULL,env_alloc,env_free,env_read,env_free_file,env_flush,env_dll_main,"C:\\WINTEST"};
    pe_init(&loader,&env); trace[0]=0; request_count=0; refuse=NULL;
    CHECK(!pe_builtin(&loader,"hosttest",host_exports,ARRAY_SIZE(host_exports),NULL));
    CHECK(pe_builtin(&loader,"HOSTTEST.DLL",host_exports,1,NULL)==DE_EXISTS);
}
static unsigned live_modules(void) {
    unsigned n=0; for(unsigned i=0;i<PE_MODULES_MAX;i++) if(loader.modules[i].state!=PE_FREE) n++;
    return n;
}

/* The IAT slot a module filled for a symbol of one of its imports. */
static u64 rd64(const void *p) {u64 v; memcpy(&v,p,8); return v;}
static void wr64(void *p,u64 v) {memcpy(p,&v,8);}
static u64 slot(const PeModule *m,const char *dll,const char *symbol,u32 ordinal) {
    const u8 *oh=m->base+rd32(m->base+60)+24; u32 rva=rd32(oh+112+8);
    for(const u8 *d=m->base+rva;rd32(d+12);d+=20) {
        if(strcasecmp((const char *)m->base+rd32(d+12),dll)) continue;
        const u8 *names=m->base+rd32(d),*iat=m->base+rd32(d+16);
        for(unsigned i=0;rd64(names+i*8);i++) {
            u64 v=rd64(names+i*8);
            if(v>>63?(!symbol && (v&0xffff)==ordinal):(symbol && !strcmp((const char *)m->base+(u32)v+2,symbol))) return rd64(iat+i*8);
        }
    }
    return 0;
}

static void test_movl(void) {
    /* movl r8=0x923456789abcdef0 and movl r9=0x1000 from GNU as. */
    static const u8 reference[32]={
        0x04,0x00,0x00,0x00,0x01,0x80,0x9a,0x78,0x56,0x34,0x12,0x00,0x01,0x97,0xf7,0x6e,
        0x05,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x01,0x00,0x80,0x60};
    u8 b[32]; memcpy(b,reference,32); u64 v;
    CHECK(!pe_movl_get(b,&v) && v==0x923456789abcdef0ULL);
    CHECK(!pe_movl_get(b+16,&v) && v==0x1000);
    /* With the other's immediate, the bundles differ only in the template
     * stop bit and the target register (slot 2 bits 6-12). */
    static const u8 allowed[16]={0x01,0,0,0,0,0,0,0,0,0,0,0xe0,0x0f,0,0,0};
    CHECK(!pe_movl_set(b+16,0x923456789abcdef0ULL) && !pe_movl_set(b,0x1000));
    for(unsigned i=0;i<16;i++) CHECK(!((b[16+i]^reference[i])&~allowed[i]) && !((b[i]^reference[16+i])&~allowed[i]));
    CHECK(!pe_movl_get(b,&v) && v==0x1000);
    for(u64 x=1;x;x<<=1) {CHECK(!pe_movl_set(b,x) && !pe_movl_get(b,&v) && v==x);}
    u8 bad[16]; memcpy(bad,reference,16); bad[0]=0x08; /* MMI template */
    CHECK(pe_movl_get(bad,&v)==DE_FORMAT && pe_movl_set(bad,1)==DE_FORMAT);
}

static void test_application(void) {
    reset();
    PeModule *exe,*dll,*dll2,*host=pe_find(&loader,"HostTest");
    CHECK(host && (host->flags&PE_FLAG_BUILTIN));
    CHECK(!pe_load_exe(&loader,"C:\\WINTEST\\PETEST.EXE",&exe) && exe && !(exe->flags&PE_FLAG_DLL));
    /* Dependencies attach first; the PEDLL/PEDLL2 cycle is broken at PEDLL. */
    CHECK(!strcmp(trace,"PEDLL2.DLL attach;PEDLL.DLL attach;"));
    CHECK((dll=pe_find(&loader,"pedll")) && (dll2=pe_find(&loader,"PEDLL2.DLL")) && !pe_find(&loader,"PETEST.EXE"));
    CHECK(live_modules()==4 && flushes==3 && !file_reads);
    CHECK(!strcmp(exe->dir,"C:\\WINTEST\\") && exe->subsystem==2);
    CHECK(((uintptr_t)exe->base&0x1fff)==0); /* SectionAlignment 0x2000 */
    CHECK(owner(exe->entry)==exe && owner((const void *)(uintptr_t)rd64(exe->entry))==exe);
    /* Each import slot holds the export: a descriptor, data or a built-in. */
    const void *add=pe_proc(&loader,dll,"pedll_add"),*twice=pe_proc_ordinal(&loader,dll,5);
    const void *counter=pe_proc(&loader,dll,"pedll_counter"),*value=pe_proc(&loader,dll2,"pedll2_value");
    CHECK(owner(add)==dll && owner(twice)==dll && owner(counter)==dll && owner(value)==dll2);
    CHECK(!pe_proc(&loader,dll,"pedll_twice")); /* NONAME */
    CHECK(slot(exe,"PEDLL.DLL","pedll_add",0)==(uintptr_t)add);
    CHECK(slot(exe,"PEDLL.DLL",NULL,5)==(uintptr_t)twice);
    CHECK(slot(exe,"PEDLL.DLL","pedll_counter",0)==(uintptr_t)counter);
    CHECK(slot(exe,"PEDLL.DLL","pedll_forward",0)==(uintptr_t)value);
    CHECK(pe_proc(&loader,dll,"pedll_forward")==value);
    CHECK(slot(exe,"HOSTTEST.DLL","host_value",0)==(uintptr_t)&host_value);
    CHECK(slot(exe,"HOSTTEST.DLL","host_trace",0)==(uintptr_t)host_function);
    CHECK(slot(dll,"PEDLL2.DLL","pedll2_value",0)==(uintptr_t)value);
    CHECK(slot(dll2,"PEDLL.DLL","pedll_add",0)==(uintptr_t)add);
    CHECK(pe_proc(&loader,host,"host_value")==&host_value && pe_proc_ordinal(&loader,host,8)==&host_value);
    /* References: PETEST and PEDLL2 hold PEDLL; PEDLL and the forwarder hold PEDLL2. */
    CHECK(dll->refs==2 && dll2->refs==2 && exe->refs==1);
    /* Resources: RCDATA 1, TEXT "GREETING" (any case) and string 17. */
    const void *data; u32 size;
    CHECK(!pe_resource(exe,(PeResId){10,NULL},(PeResId){1,NULL},0,&data,&size) && size>=5 && !memcmp(data,"hello",5));
    CHECK(!pe_resource(exe,(PeResId){0,"text"},(PeResId){0,"Greeting"},0x409,&data,&size) && !memcmp(data,"named resource",14));
    CHECK(!pe_resource(exe,(PeResId){6,NULL},(PeResId){2,NULL},0,&data,&size));
    const u8 *s=data; CHECK(rd16(s)==0 && rd16(s+2)==9 && rd16(s+4)=='s' && rd16(s+20)=='n');
    CHECK(pe_resource(exe,(PeResId){10,NULL},(PeResId){2,NULL},0,&data,&size)==DE_NOFILE && !data);
    CHECK(pe_resource(dll,(PeResId){10,NULL},(PeResId){1,NULL},0,&data,&size)==DE_NOFILE);
    CHECK(pe_resource(host,(PeResId){10,NULL},(PeResId){1,NULL},0,&data,&size)==DE_FUNCTION);
    /* LoadLibrary/FreeLibrary: one attach, one detach, memory returned. */
    unsigned before=allocations; PeModule *dyn,*again; trace[0]=0;
    CHECK(!pe_load(&loader,"pedyn",exe->dir,&dyn) && !pe_load(&loader,"PEDYN.DLL",NULL,&again) && dyn==again && dyn->refs==2);
    CHECK(!strcmp(trace,"PEDYN.DLL attach;") && allocations==before+1);
    CHECK(!pe_free(&loader,dyn) && !strcmp(trace,"PEDYN.DLL attach;") && !pe_free(&loader,dyn));
    CHECK(!strcmp(trace,"PEDYN.DLL attach;PEDYN.DLL detach;") && allocations==before && pe_free(&loader,dyn)==DE_HANDLE);
    /* Search order: the importer's directory, the system directory, then the current one. */
    request_count=0;
    CHECK(pe_load(&loader,"NOSUCH",exe->dir,&dyn)==DE_NOFILE && !dyn && !strcmp(loader.error_module,"NOSUCH.DLL"));
    CHECK(request_count==2 && !strcmp(requests[0],"C:\\WINTEST\\NOSUCH.DLL") && !strcmp(requests[1],"NOSUCH.DLL"));
    request_count=0;
    CHECK(!pe_load(&loader,"C:\\WINTEST\\pedyn",NULL,&dyn) && request_count==1 && !strcmp(requests[0],"C:\\WINTEST\\pedyn.DLL"));
    CHECK(!pe_free(&loader,dyn) && allocations==before);
    /* A refused DllMain unwinds the load; the DLL never gets a detach. */
    trace[0]=0; refuse="PEFAIL.DLL"; u32 host_refs=host->refs;
    CHECK(pe_load(&loader,"PEFAIL",NULL,&dyn)==DE_ACCESS && !strcmp(loader.error_module,"PEFAIL.DLL"));
    CHECK(!strcmp(trace,"PEFAIL.DLL attach;") && allocations==before && !pe_find(&loader,"PEFAIL") && host->refs==host_refs);
    refuse=NULL;
    /* A missing import while PEDLL is loaded leaves its count alone. */
    CHECK(pe_load_exe(&loader,"C:\\WINTEST\\PEBAD.EXE",&dyn)==DE_NOFILE);
    CHECK(!strcmp(loader.error_module,"PEDLL.DLL") && !strcmp(loader.error_symbol,"pedll_missing"));
    CHECK(dll->refs==2 && allocations==before);
    /* Aliases name the same module; HINSTANCE is the image base. */
    CHECK(!pe_alias(&loader,"PEDLL32","pedll") && pe_find(&loader,"pedll32.dll")==dll);
    CHECK(!pe_load(&loader,"PEDLL32",NULL,&dyn) && dyn==dll && dll->refs==3 && !pe_free(&loader,dyn));
    CHECK(pe_from_base(&loader,dll->base)==dll && pe_from_base(&loader,exe->base)==exe && !pe_from_base(&loader,dll->base+1));
    CHECK(!pe_from_base(&loader,NULL));
    /* Handles from guests. */
    CHECK(!pe_valid(&loader,NULL) && !pe_valid(&loader,(PeModule *)((u8 *)exe+1)) && pe_valid(&loader,exe));
    CHECK(pe_free(&loader,(PeModule *)&loader)==DE_HANDLE);
    /* The application ends; the cycle stays until shutdown, newest first. */
    trace[0]=0;
    CHECK(!pe_free(&loader,exe) && !trace[0] && dll->refs==1 && dll2->refs==1 && live_modules()==3);
    pe_shutdown(&loader);
    CHECK(!strcmp(trace,"PEDLL.DLL detach;PEDLL2.DLL detach;") && !allocations && !file_reads && !live_modules());
}

static void test_failures(void) {
    /* A missing import found while mapping: both new DLLs are unwound unattached. */
    reset();
    PeModule *m;
    CHECK(pe_load_exe(&loader,"C:\\WINTEST\\PEBAD.EXE",&m)==DE_NOFILE && !m && !trace[0]);
    CHECK(!allocations && live_modules()==1);
    CHECK(pe_load_exe(&loader,"C:\\WINTEST\\PESTRIP.EXE",&m)==DE_FORMAT && !allocations);
    CHECK(pe_load_exe(&loader,"C:\\WINTEST\\PEDLL.DLL",&m)==DE_FORMAT && !allocations);  /* not an application */
    CHECK(pe_load(&loader,"C:\\WINTEST\\PETEST.EXE",NULL,&m)==DE_FORMAT && !allocations); /* not a DLL */
    CHECK(pe_load(&loader,"",NULL,&m)==DE_FUNCTION && pe_load(&loader,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",NULL,&m)==DE_PATH);
    pe_shutdown(&loader); CHECK(!allocations && !file_reads);
}

/* File offset of an RVA in a fixture. */
static u32 file_offset(const File *f,u32 rva) {
    u32 pe=rd32(f->data+60),count=rd16(f->data+pe+6),optional=rd16(f->data+pe+20);
    const u8 *s=f->data+pe+24+optional;
    for(u32 i=0;i<count;i++,s+=40) if(rva>=rd32(s+12) && rva<rd32(s+12)+rd32(s+16)) return rva-rd32(s+12)+rd32(s+20);
    return 0;
}
static int load_variant(File *variant,PeModule **m) {
    override=variant; int e=pe_load(&loader,"PEDYN",NULL,m); override=NULL;
    return e;
}
static void test_malformed(void) {
    File *dyn=fixture("pedyn.dll"); File v; PeModule *m;
    u32 pe=rd32(dyn->data+60); const u8 *oh=dyn->data+pe+24;
    unsigned char *copy=malloc(dyn->size);
    #define VARIANT(edit,expect) do { \
        memcpy(copy,dyn->data,dyn->size); v=(File){"pedyn.dll",copy,dyn->size}; edit; \
        reset(); int e_=load_variant(&v,&m); CHECK(e_==(expect)); \
        if(!e_) CHECK(!pe_free(&loader,m)); \
        pe_shutdown(&loader); CHECK(!allocations && !file_reads); } while(0)
    VARIANT((void)0,0);
    VARIANT(v.size=100,DE_FORMAT);
    VARIANT(v.size=pe+24+100,DE_FORMAT);
    VARIANT(copy[0]='X',DE_FORMAT);
    VARIANT(wr16(copy+pe+4,0x8664),DE_FORMAT);           /* x64 */
    VARIANT(wr16(copy+pe+24,0x10b),DE_FORMAT);           /* PE32 */
    VARIANT(wr32(copy+pe+24+32,0x1800),DE_FORMAT);       /* SectionAlignment not a power of two */
    VARIANT(wr32(copy+pe+24+56,0x80000000),DE_FORMAT);   /* SizeOfImage */
    VARIANT(wr32(copy+pe+24+108,1000),DE_FORMAT);        /* NumberOfRvaAndSizes */
    VARIANT(wr16(copy+pe+24+68,10),DE_FORMAT);           /* EFI application */
    VARIANT(wr32(copy+pe+24+112+9*8,0x1000),DE_FORMAT);  /* TLS directory */
    u32 sec=pe+24+rd16(dyn->data+pe+20);
    VARIANT(wr32(copy+sec+20,0x7fffff00),DE_FORMAT);     /* raw data past the end of the file */
    VARIANT(wr32(copy+sec+12,0x7fffff00),DE_FORMAT);     /* section past SizeOfImage */
    u32 reloc=file_offset(dyn,rd32(oh+112+5*8));
    VARIANT(wr32(copy+reloc+4,6),DE_FORMAT);             /* relocation block size */
    VARIANT(wr16(copy+reloc+8,0x5000),DE_FORMAT);        /* unknown relocation type */
    VARIANT(wr32(copy+reloc,0x7ffff000),DE_FORMAT);      /* relocation outside the image */
    u32 import=file_offset(dyn,rd32(oh+112+1*8));
    VARIANT(wr32(copy+import+12,0x7fffffff),DE_FORMAT);  /* DLL name outside the image */
    u32 thunk=file_offset(dyn,rd32(dyn->data+import));
    VARIANT(wr32(copy+file_offset(dyn,rd32(copy+thunk))+2,0x5f5f5f5f),DE_NOFILE); /* unknown name */
    VARIANT(wr64(copy+thunk,0x8000000000000063ULL),DE_NOFILE); /* unknown ordinal */
    /* A broken export directory only affects lookups. */
    memcpy(copy,dyn->data,dyn->size); v=(File){"pedyn.dll",copy,dyn->size};
    u32 exports=file_offset(dyn,rd32(oh+112));
    wr32(copy+exports+28,0x7ffffff0); reset();
    CHECK(!load_variant(&v,&m) && !pe_proc(&loader,m,"pedyn_mul") && !pe_proc_ordinal(&loader,m,1));
    pe_shutdown(&loader); CHECK(!allocations);
    /* Random damage to headers and directories: clean failure or a usable module. */
    srand(1); fuzzing=1;
    unsigned loaded=0;
    for(unsigned round=0;round<3000;round++) {
        memcpy(copy,dyn->data,dyn->size); v=(File){"pedyn.dll",copy,dyn->size};
        unsigned flips=1+(unsigned)rand()%4;
        for(unsigned k=0;k<flips;k++) {
            u32 at=(u32)rand()%(round&1?0x400:dyn->size);
            copy[at]^=(u8)(1u<<(rand()%8));
        }
        reset(); int e=load_variant(&v,&m);
        if(!e) {loaded++; pe_proc(&loader,m,"pedyn_mul"); pe_resource(m,(PeResId){10,NULL},(PeResId){1,NULL},0,&(const void *){0},&(u32){0}); CHECK(!pe_free(&loader,m));}
        pe_shutdown(&loader); CHECK(!allocations && !file_reads);
    }
    fuzzing=0;
    CHECK(loaded>0 && loaded<3000);
    free(copy);
}
int main(int argc,char **argv) {
    if(argc!=2) {fprintf(stderr,"usage: %s fixture-directory\n",argv[0]); return 2;}
    fixture_dir=argv[1];
    static const char *names[]={"petest.exe","pedll.dll","pedll2.dll","pedyn.dll","pefail.dll","pebad.exe","pestrip.exe"};
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) load_fixture(names[i]);
    test_movl();
    test_application();
    test_failures();
    test_malformed();
    printf("PASS PE loader: %u assertions (WDK fixtures, relocation, imports by name/ordinal/data/forwarder, cycles, DllMain order and unwinding, resources, malformed images)\n",checks);
    for(unsigned i=0;i<file_count;i++) free(files[i].data);
    return 0;
}
