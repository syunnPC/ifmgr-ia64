/* SPDX-License-Identifier: GPL-2.0-or-later
 * PE32+ (IA-64) loader and dynamic linker; see pe.h. Every RVA, size and
 * string from an image is bounds-checked, so a malformed file fails cleanly.
 */
#include "pe.h"
#define MZ 0x5a4d
#define PE_SIGNATURE 0x4550
#define MACHINE_IA64 0x200
#define MAGIC_PE32_PLUS 0x20b
#define FILE_RELOCS_STRIPPED 0x0001
#define FILE_EXECUTABLE 0x0002
#define FILE_DLL 0x2000
#define DIR_EXPORT 0
#define DIR_IMPORT 1
#define DIR_RESOURCE 2
#define DIR_RELOC 5
#define DIR_TLS 9
#define REL_ABSOLUTE 0
#define REL_IA64_IMM64 9
#define REL_DIR64 10
#define FORWARD_DEPTH 8

static u64 rd64(const void *p) {return (u64)rd32(p)|(u64)rd32((const u8 *)p+4)<<32;}
static void wr64(void *p,u64 v) {wr32(p,(u32)v); wr32((u8 *)p+4,(u32)(v>>32));}
static void note(PeLoader *l,const char *module,const char *symbol) {
    strcopy(l->error_module,sizeof(l->error_module),module?module:"");
    strcopy(l->error_symbol,sizeof(l->error_symbol),symbol?symbol:"");
}
/* A range inside the mapped image, or NULL. */
static u8 *at(const PeModule *m,u64 rva,u64 bytes) {
    if(rva>m->size || bytes>m->size-rva) return NULL;
    return m->base+rva;
}
/* A NUL-terminated string inside the image, or NULL. */
static const char *string_at(const PeModule *m,u64 rva) {
    const u8 *s=at(m,rva,1); if(!s) return NULL;
    for(u64 n=rva;n<m->size;n++) if(!m->base[n]) return (const char *)s;
    return NULL;
}

/* --- IA-64 movl (MLX template) immediates ------------------------------ */
#define SLOT_MASK ((1ULL<<41)-1)
static int mlx(u64 lo) {u32 t=(u32)(lo&0x1e); return t==4;} /* templates 4/5 */
int pe_movl_get(const u8 bundle[16],u64 *value) {
    u64 lo=rd64(bundle),hi=rd64(bundle+8);
    u64 l=((lo>>46)|(hi<<18))&SLOT_MASK,x=(hi>>23)&SLOT_MASK;
    if(!mlx(lo) || ((x>>37)&15)!=6) return DE_FORMAT;
    *value=((x>>13)&0x7f)|((x>>27)&0x1ff)<<7|((x>>22)&0x1f)<<16|((x>>21)&1)<<21|l<<22|((x>>36)&1)<<63;
    return 0;
}
int pe_movl_set(u8 bundle[16],u64 value) {
    u64 lo=rd64(bundle),hi=rd64(bundle+8),x=(hi>>23)&SLOT_MASK;
    if(!mlx(lo) || ((x>>37)&15)!=6) return DE_FORMAT;
    x&=~((1ULL<<36)|(0x1ffULL<<27)|(0x1fULL<<22)|(1ULL<<21)|(0x7fULL<<13));
    x|=((value>>63)&1)<<36|((value>>7)&0x1ff)<<27|((value>>16)&0x1f)<<22|((value>>21)&1)<<21|(value&0x7f)<<13;
    u64 l=(value>>22)&SLOT_MASK;
    lo=(lo&((1ULL<<46)-1))|l<<46;
    hi=(l>>18)|x<<23;
    wr64(bundle,lo); wr64(bundle+8,hi); return 0;
}

/* --- names ---------------------------------------------------------------- */
static const char *base_name(const char *path) {
    const char *b=path;
    for(const char *p=path;*p;p++) if(*p=='\\' || *p=='/' || *p==':') b=p+1;
    return b;
}
/* Upper-case base name; ".DLL" is added when there is no extension. */
static int module_name(const char *path,char out[PE_NAME_MAX],int add_dll) {
    const char *b=base_name(path); size_t n=strlen(b);
    if(!n || n>=PE_NAME_MAX-4) return DE_PATH;
    for(size_t i=0;i<=n;i++) out[i]=upper(b[i]);
    if(add_dll && !strchr(out,'.')) strappend(out,PE_NAME_MAX,".DLL");
    return 0;
}
/* Upper-case module name with aliases applied. */
static int canonical(const PeLoader *l,const char *name,char out[PE_NAME_MAX],int add_dll) {
    int e=module_name(name,out,add_dll); if(e) return e;
    for(u32 i=0;i<l->alias_count;i++) if(!strcmp(out,l->alias[i][0])) {strcopy(out,PE_NAME_MAX,l->alias[i][1]); break;}
    return 0;
}
int pe_alias(PeLoader *l,const char *alias,const char *target) {
    char a[PE_NAME_MAX],t[PE_NAME_MAX];
    if(module_name(alias,a,1) || module_name(target,t,1)) return DE_PATH;
    if(l->alias_count==PE_ALIASES_MAX) return DE_NOMEM;
    strcopy(l->alias[l->alias_count][0],PE_NAME_MAX,a); strcopy(l->alias[l->alias_count][1],PE_NAME_MAX,t);
    l->alias_count++; return 0;
}
PeModule *pe_from_base(PeLoader *l,const void *base) {
    for(unsigned i=0;i<PE_MODULES_MAX;i++) {
        PeModule *m=&l->modules[i];
        if(m->state==PE_READY && m->base && m->base==base) return m;
    }
    return NULL;
}
PeModule *pe_find(PeLoader *l,const char *name) {
    char want[PE_NAME_MAX]; if(!name || canonical(l,name,want,1)) return NULL;
    for(unsigned i=0;i<PE_MODULES_MAX;i++) {
        PeModule *m=&l->modules[i];
        if(m->state!=PE_FREE && m->state!=PE_UNLOADING && (m->flags&PE_FLAG_DLL) && !strcmp(m->name,want)) return m;
    }
    return NULL;
}
static PeModule *new_module(PeLoader *l) {
    for(unsigned i=0;i<PE_MODULES_MAX;i++) if(l->modules[i].state==PE_FREE) {
        memset(&l->modules[i],0,sizeof(l->modules[i])); return &l->modules[i];
    }
    return NULL;
}

/* --- mapping -------------------------------------------------------------- */
typedef struct {u32 rva,size;} Dir;
static int map_image(PeLoader *l,PeModule *m,const u8 *file,u32 size) {
    if(size<64 || rd16(file)!=MZ) return DE_FORMAT;
    u32 pe=rd32(file+60);
    if(pe>size || size-pe<24+112 || rd32(file+pe)!=PE_SIGNATURE) return DE_FORMAT;
    const u8 *fh=file+pe+4,*oh=fh+20;
    u32 sections=rd16(fh+2),optional=rd16(fh+16); m->characteristics=rd16(fh+18);
    if(rd16(fh)!=MACHINE_IA64 || optional<112 || optional>size-pe-24 || rd16(oh)!=MAGIC_PE32_PLUS ||
       !(m->characteristics&FILE_EXECUTABLE)) return DE_FORMAT;
    u64 preferred=rd64(oh+24); u32 align=rd32(oh+32),file_align=rd32(oh+36);
    u32 image_size=rd32(oh+56),header_size=rd32(oh+60),count=rd32(oh+108);
    m->subsystem=rd16(oh+68);
    if(!align || (align&(align-1)) || align<4096 || !file_align || (file_align&(file_align-1)) || file_align>align) return DE_FORMAT;
    if(!image_size || image_size>0x40000000 || header_size>image_size || header_size>size) return DE_FORMAT;
    if(count>(optional-112)/8) return DE_FORMAT;
    if(m->subsystem!=2 && m->subsystem!=3) return DE_FORMAT; /* Windows GUI or console */
    Dir dir[16]; memset(dir,0,sizeof(dir));
    for(u32 i=0;i<count && i<16;i++) dir[i]=(Dir){rd32(oh+112+i*8),rd32(oh+116+i*8)};
    if(dir[DIR_TLS].rva || dir[DIR_TLS].size) return DE_FORMAT; /* thread-local storage is not supported */
    u64 sections_at=(u64)pe+24+optional;
    if(sections_at+(u64)sections*40>header_size || !sections || sections>96) return DE_FORMAT;
    /* Over-allocate so the base honours SectionAlignment. */
    m->memory_bytes=(u64)image_size+align;
    int e=l->env.alloc(l->env.ctx,m->memory_bytes,&m->memory); if(e) {m->memory=NULL; return e;}
    m->base=(u8 *)(((uintptr_t)m->memory+align-1)&~(uintptr_t)(align-1)); m->size=image_size;
    memset(m->memory,0,m->memory_bytes);
    memcpy(m->base,file,header_size);
    for(u32 i=0;i<sections;i++) {
        const u8 *s=file+sections_at+i*40;
        u32 vsize=rd32(s+8),va=rd32(s+12),raw=rd32(s+16),ptr=rd32(s+20);
        u32 span=vsize?vsize:raw;
        if(va<header_size && span) return DE_FORMAT;
        if(!at(m,va,span)) return DE_FORMAT;
        u32 copy=MIN(raw,span);
        if(copy && (ptr>size || copy>size-ptr)) return DE_FORMAT;
        if(copy) memcpy(m->base+va,file+ptr,copy);
    }
    /* Base relocations. */
    u64 delta=(u64)(uintptr_t)m->base-preferred;
    if(delta && ((m->characteristics&FILE_RELOCS_STRIPPED) || !dir[DIR_RELOC].size)) return DE_FORMAT;
    if(dir[DIR_RELOC].size) {
        const u8 *r=at(m,dir[DIR_RELOC].rva,dir[DIR_RELOC].size); if(!r) return DE_FORMAT;
        for(u32 off=0;off<dir[DIR_RELOC].size;) {
            if(dir[DIR_RELOC].size-off<8) return DE_FORMAT;
            u32 page=rd32(r+off),block=rd32(r+off+4);
            if(block<8 || (block&1) || block>dir[DIR_RELOC].size-off) return DE_FORMAT;
            for(u32 k=8;k+2<=block;k+=2) {
                u16 entry=rd16(r+off+k); u32 type=entry>>12; u64 rva=(u64)page+(entry&0xfff);
                if(type==REL_ABSOLUTE) continue;
                if(type==REL_DIR64) {
                    u8 *p=at(m,rva,8); if(!p) return DE_FORMAT;
                    wr64(p,rd64(p)+delta);
                } else if(type==REL_IA64_IMM64) {
                    u8 *p=at(m,rva&~15ULL,16); u64 v; if(!p) return DE_FORMAT;
                    if(pe_movl_get(p,&v) || pe_movl_set(p,v+delta)) return DE_FORMAT;
                } else return DE_FORMAT;
            }
            off+=block;
        }
    }
    u32 entry=rd32(oh+16);
    if(entry) {if(!at(m,entry,16)) return DE_FORMAT; m->entry=m->base+entry;}
    m->export_rva=dir[DIR_EXPORT].rva; m->export_size=dir[DIR_EXPORT].size;
    m->resource_rva=dir[DIR_RESOURCE].rva; m->resource_size=dir[DIR_RESOURCE].size;
    if(m->characteristics&FILE_DLL) m->flags|=PE_FLAG_DLL;
    m->state=PE_MAPPED;
    return 0;
}
static void release_memory(PeLoader *l,PeModule *m) {
    if(m->memory) l->env.free(l->env.ctx,m->memory,m->memory_bytes);
    memset(m,0,sizeof(*m));
}
static int read_candidate(PeLoader *l,const char *dir,const char *name,char *path,void **data,u32 *size) {
    path[0]=0;
    if(dir && *dir) {
        if(strcopy(path,DOS_PATH_MAX,dir)) return DE_PATH;
        size_t n=strlen(path);
        if(path[n-1]!='\\' && path[n-1]!=':' && strappend(path,DOS_PATH_MAX,"\\")) return DE_PATH;
    }
    if(strappend(path,DOS_PATH_MAX,name)) return DE_PATH;
    return l->env.read_file(l->env.ctx,path,data,size);
}
static int same_dir(const char *a,const char *b) {
    if(!a || !b) return 0;
    size_t n=strlen(a),k=strlen(b);
    if(n && a[n-1]=='\\') n--;
    if(k && b[k-1]=='\\') k--;
    if(n!=k) return 0;
    for(size_t i=0;i<n;i++) if(upper(a[i])!=upper(b[i])) return 0;
    return 1;
}
/* Map a file (not yet linked) and add it to the group. */
static int map_file(PeLoader *l,const char *request,const char *dir,int exe,PeModule **out) {
    char name[PE_NAME_MAX],path[DOS_PATH_MAX],file_name[PE_NAME_MAX];
    int e=exe?module_name(request,name,0):canonical(l,request,name,1); if(e) {note(l,request,NULL); return e;}
    /* The name to open keeps an explicit path. */
    int has_path=base_name(request)!=request;
    strcopy(file_name,sizeof(file_name),name);
    void *data=NULL; u32 size=0; e=DE_NOFILE;
    if(has_path) {
        if(strcopy(path,sizeof(path),request)) e=DE_PATH;
        else {
            if(!exe && !strchr(base_name(path),'.')) strappend(path,sizeof(path),".DLL");
            e=l->env.read_file(l->env.ctx,path,&data,&size);
        }
    } else {
        const char *dirs[3]={dir,l->env.system_dir,""};
        for(unsigned i=0;i<3 && (e==DE_NOFILE || e==DE_PATH);i++) {
            if(i<2 && (!dirs[i] || !*dirs[i])) continue;
            if(i==1 && same_dir(dirs[0],dirs[1])) continue;
            e=read_candidate(l,dirs[i],file_name,path,&data,&size);
        }
        if(e==DE_PATH) e=DE_NOFILE; /* not in any searched directory */
    }
    if(e) {note(l,name,NULL); return e;}
    PeModule *m=new_module(l);
    if(!m || l->group_count==PE_MODULES_MAX) {l->env.free_file(l->env.ctx,data,size); note(l,name,NULL); return DE_NOMEM;}
    strcopy(m->name,sizeof(m->name),name);
    strcopy(m->dir,sizeof(m->dir),path); m->dir[base_name(m->dir)-m->dir]=0;
    e=map_image(l,m,data,size);
    l->env.free_file(l->env.ctx,data,size);
    if(!e && !exe && !(m->flags&PE_FLAG_DLL)) e=DE_FORMAT;
    if(!e && exe && (m->flags&PE_FLAG_DLL)) e=DE_FORMAT;
    if(e) {note(l,name,NULL); release_memory(l,m); return e;}
    m->refs=1;
    l->group[l->group_count++]=m; *out=m; return 0;
}

/* --- exports and imports -------------------------------------------------- */
static int add_import(PeModule *m,PeModule *target) {
    for(u32 i=0;i<m->import_count;i++) if(m->imports[i]==target) return 0;
    if(m->import_count==PE_IMPORTS_MAX) return DE_NOMEM;
    m->imports[m->import_count++]=target; target->refs++; return 0;
}
/* The module an import or forwarder names: loaded, in this group, or mapped now. */
static int need(PeLoader *l,PeModule *from,const char *name,PeModule **out) {
    PeModule *t=pe_find(l,name);
    if(!t && l->lookup_only) {note(l,name,NULL); return DE_NOFILE;}
    if(!t) {int e=map_file(l,name,from->dir,0,&t); if(e) return e; t->refs=0;}
    if(t==from) {*out=t; return 0;}
    int e=add_import(from,t); if(e) {note(l,from->name,NULL); return e;}
    *out=t; return 0;
}
static int lookup(PeLoader *l,PeModule *from,PeModule *m,const char *name,u32 ordinal,unsigned depth,const void **out);
static int forward(PeLoader *l,PeModule *from,const char *text,unsigned depth,const void **out) {
    const char *dot=strchr(text,'.'); char module[PE_NAME_MAX];
    if(!dot || dot==text || (size_t)(dot-text)>=PE_NAME_MAX-4 || depth>=FORWARD_DEPTH) return DE_FORMAT;
    memcpy(module,text,(size_t)(dot-text)); module[dot-text]=0;
    PeModule *t; int e=need(l,from,module,&t); if(e) return e;
    if(dot[1]=='#') {
        u32 ordinal=0; for(const char *p=dot+2;*p;p++) {if(*p<'0' || *p>'9' || ordinal>65535) return DE_FORMAT; ordinal=ordinal*10+(u32)(*p-'0');}
        return lookup(l,from,t,NULL,ordinal,depth+1,out);
    }
    return lookup(l,from,t,dot+1,0,depth+1,out);
}
/* name, or ordinal when name is NULL. from receives references to forwarder targets. */
static int lookup(PeLoader *l,PeModule *from,PeModule *m,const char *name,u32 ordinal,unsigned depth,const void **out) {
    *out=NULL;
    if(m->flags&PE_FLAG_BUILTIN) {
        for(u32 i=0;i<m->export_count;i++) {
            const PeExport *x=&m->exports[i];
            if(name?x->name && !strcmp(x->name,name):x->ordinal==ordinal) {*out=x->address; return 0;}
        }
        return DE_NOFILE;
    }
    const u8 *d=at(m,m->export_rva,40); if(!d || m->export_size<40) return DE_NOFILE;
    u32 base=rd32(d+16),functions=rd32(d+20),names=rd32(d+24);
    const u8 *eat=at(m,rd32(d+28),(u64)functions*4),*npt=at(m,rd32(d+32),(u64)names*4),*ot=at(m,rd32(d+36),(u64)names*2);
    if(!eat || (names && (!npt || !ot))) return DE_FORMAT;
    u32 index=UINT32_MAX;
    if(name) {
        /* Name pointers are sorted; binary search. */
        u32 lo=0,hi=names;
        while(lo<hi) {
            u32 mid=lo+(hi-lo)/2; const char *s=string_at(m,rd32(npt+mid*4)); if(!s) return DE_FORMAT;
            int c=strcmp(name,s);
            if(!c) {index=rd16(ot+mid*2); break;}
            if(c<0) hi=mid; else lo=mid+1;
        }
    } else if(ordinal>=base && ordinal-base<functions) index=ordinal-base;
    if(index>=functions) return DE_NOFILE;
    u32 rva=rd32(eat+index*4); if(!rva) return DE_NOFILE;
    if(rva>=m->export_rva && rva-m->export_rva<m->export_size) {
        const char *text=string_at(m,rva); if(!text) return DE_FORMAT;
        return forward(l,from,text,depth,out);
    }
    if(!at(m,rva,1)) return DE_FORMAT;
    *out=m->base+rva; return 0;
}
static void ordinal_text(char out[16],u32 ordinal) {
    char digits[8]; unsigned n=0;
    do digits[n++]=(char)('0'+ordinal%10); while((ordinal/=10) && n<7);
    out[0]='#'; for(unsigned i=0;i<n;i++) out[1+i]=digits[n-1-i];
    out[1+n]=0;
}
static int link_imports(PeLoader *l,PeModule *m) {
    const u8 *oh=m->base+rd32(m->base+60)+24;
    u32 count=rd32(oh+108); if(count<=DIR_IMPORT) return 0;
    u32 rva=rd32(oh+112+DIR_IMPORT*8),bytes=rd32(oh+116+DIR_IMPORT*8);
    if(!rva && !bytes) return 0;
    for(u32 off=0;;off+=20) {
        const u8 *desc=at(m,(u64)rva+off,20); if(!desc) {note(l,m->name,NULL); return DE_FORMAT;}
        u32 names_rva=rd32(desc),name_rva=rd32(desc+12),iat_rva=rd32(desc+16);
        if(!names_rva && !name_rva && !iat_rva) return 0;
        const char *dll=string_at(m,name_rva); if(!dll || !iat_rva) {note(l,m->name,NULL); return DE_FORMAT;}
        PeModule *t; int e=need(l,m,dll,&t); if(e) return e;
        u32 thunks=names_rva?names_rva:iat_rva;
        for(u64 i=0;;i++) {
            const u8 *thunk=at(m,thunks+i*8,8); u8 *slot=at(m,iat_rva+i*8,8);
            if(!thunk || !slot) {note(l,m->name,NULL); return DE_FORMAT;}
            u64 v=rd64(thunk); if(!v) break;
            const void *address; char symbol[64];
            if(v>>63) {
                u32 ordinal=(u32)(v&0xffff);
                e=lookup(l,m,t,NULL,ordinal,0,&address);
                ordinal_text(symbol,ordinal);
            } else {
                const char *s=v>>32?NULL:string_at(m,(u32)v+2);
                if(!s) {note(l,m->name,NULL); return DE_FORMAT;}
                e=lookup(l,m,t,s,0,0,&address); strcopy(symbol,sizeof(symbol),s);
            }
            if(e) {note(l,t->name,symbol); return e;}
            wr64(slot,(u64)(uintptr_t)address);
        }
    }
}

/* --- load groups ---------------------------------------------------------- */
static int in_group(PeLoader *l,const PeModule *m) {
    for(u32 i=0;i<l->group_count;i++) if(l->group[i]==m) return 1;
    return 0;
}
/* Undo a failed group: detach what attached (newest first), drop references
 * it took on older modules, then free it. */
static void unwind_group(PeLoader *l) {
    for(;;) {
        PeModule *last=NULL;
        for(u32 i=0;i<l->group_count;i++) {
            PeModule *m=l->group[i];
            if((m->flags&PE_FLAG_INITIALIZED) && (!last || m->order>last->order)) last=m;
        }
        if(!last) break;
        last->flags&=~PE_FLAG_INITIALIZED;
        if((last->flags&PE_FLAG_DLL) && last->entry)
            l->env.dll_main(l->env.ctx,last->entry,last->base,PE_DLL_PROCESS_DETACH);
    }
    for(u32 i=0;i<l->group_count;i++) {
        PeModule *m=l->group[i];
        for(u32 k=0;k<m->import_count;k++) if(!in_group(l,m->imports[k])) m->imports[k]->refs--;
    }
    for(u32 i=0;i<l->group_count;i++) release_memory(l,l->group[i]);
    l->group_count=0;
}
/* Dependencies first; a module already being visited breaks a cycle. */
static int initialize(PeLoader *l,PeModule *m) {
    if(!in_group(l,m) || (m->flags&PE_FLAG_INITIALIZED)) return 0;
    for(u32 i=0;i<l->visit_count;i++) if(l->visiting[i]==m) return 0;
    l->visiting[l->visit_count++]=m;
    for(u32 i=0;i<m->import_count;i++) {int e=initialize(l,m->imports[i]); if(e) return e;}
    l->visit_count--;
    m->order=++l->next_order;
    m->flags|=PE_FLAG_INITIALIZED;
    if((m->flags&PE_FLAG_DLL) && m->entry && !l->env.dll_main(l->env.ctx,m->entry,m->base,PE_DLL_PROCESS_ATTACH)) {
        m->flags&=~PE_FLAG_INITIALIZED; note(l,m->name,"DllMain"); return DE_ACCESS;
    }
    return 0;
}
static int load_group(PeLoader *l,PeModule *first) {
    int e=0;
    for(u32 i=0;i<l->group_count && !e;i++) {e=link_imports(l,l->group[i]); if(!e) l->group[i]->state=PE_LINKED;}
    if(e) {unwind_group(l); return e;}
    for(u32 i=0;i<l->group_count;i++) l->env.flush_code(l->env.ctx,l->group[i]->base,l->group[i]->size);
    l->visit_count=0; e=initialize(l,first);
    for(u32 i=0;i<l->group_count && !e;i++) {l->visit_count=0; e=initialize(l,l->group[i]);}
    l->visit_count=0;
    if(e) {unwind_group(l); return e;}
    for(u32 i=0;i<l->group_count;i++) l->group[i]->state=PE_READY;
    l->group_count=0; return 0;
}

/* --- public API ------------------------------------------------------------ */
void pe_init(PeLoader *l,const PeEnv *env) {memset(l,0,sizeof(*l)); l->env=*env;}
int pe_builtin(PeLoader *l,const char *name,const PeExport *exports,u32 count,PeModule **out) {
    char want[PE_NAME_MAX]; int e=module_name(name,want,1); if(e) return e;
    if(pe_find(l,want)) return DE_EXISTS;
    PeModule *m=new_module(l); if(!m) return DE_NOMEM;
    strcopy(m->name,sizeof(m->name),want);
    m->flags=PE_FLAG_DLL|PE_FLAG_BUILTIN|PE_FLAG_INITIALIZED; m->state=PE_READY; m->refs=1;
    m->exports=exports; m->export_count=count; m->order=++l->next_order;
    if(out) *out=m;
    return 0;
}
int pe_load(PeLoader *l,const char *name,const char *dir,PeModule **out) {
    *out=NULL; note(l,NULL,NULL);
    if(!name || !*name || l->group_count) return DE_FUNCTION;
    PeModule *m=pe_find(l,name);
    if(m) {m->refs++; *out=m; return 0;}
    int e=map_file(l,name,dir,0,&m); if(e) {l->group_count=0; return e;}
    e=load_group(l,m); if(!e) *out=m;
    return e;
}
int pe_load_exe(PeLoader *l,const char *path,PeModule **out) {
    *out=NULL; note(l,NULL,NULL);
    if(!path || !*path || l->group_count) return DE_FUNCTION;
    PeModule *m; int e=map_file(l,path,NULL,1,&m); if(e) {l->group_count=0; return e;}
    e=load_group(l,m); if(!e) *out=m;
    return e;
}
static void release(PeLoader *l,PeModule *m) {
    m->state=PE_UNLOADING;
    if((m->flags&(PE_FLAG_DLL|PE_FLAG_INITIALIZED|PE_FLAG_BUILTIN))==(PE_FLAG_DLL|PE_FLAG_INITIALIZED) && m->entry)
        l->env.dll_main(l->env.ctx,m->entry,m->base,PE_DLL_PROCESS_DETACH);
    for(u32 i=0;i<m->import_count;i++) {
        PeModule *t=m->imports[i];
        if(t->state!=PE_UNLOADING && t->refs && !--t->refs) release(l,t);
    }
    release_memory(l,m);
}
int pe_valid(const PeLoader *l,const PeModule *m) {
    uintptr_t off=(uintptr_t)m-(uintptr_t)l->modules;
    if(!m || off>=sizeof(l->modules) || off%sizeof(*m)) return 0;
    return m->state==PE_READY && m->refs;
}
int pe_free(PeLoader *l,PeModule *m) {
    if(!pe_valid(l,m)) return DE_HANDLE;
    if(!--m->refs) release(l,m);
    return 0;
}
void pe_shutdown(PeLoader *l) {
    for(;;) {
        PeModule *last=NULL;
        for(unsigned i=0;i<PE_MODULES_MAX;i++) {
            PeModule *m=&l->modules[i];
            if(m->state!=PE_FREE && (!last || m->order>last->order)) last=m;
        }
        if(!last) return;
        /* Imports are released by their own turn, newest first. */
        last->import_count=0;
        if(last->flags&PE_FLAG_BUILTIN) memset(last,0,sizeof(*last)); else release(l,last);
    }
}
/* Forwarders resolve against loaded modules only, and m keeps a reference
 * to each module a forwarder led to. */
static const void *proc(PeLoader *l,PeModule *m,const char *name,u32 ordinal) {
    const void *p; if(!m || m->state!=PE_READY || l->group_count) return NULL;
    l->lookup_only=1; int e=lookup(l,m,m,name,ordinal,0,&p); l->lookup_only=0;
    return e?NULL:p;
}
const void *pe_proc(PeLoader *l,PeModule *m,const char *name) {return name?proc(l,m,name,0):NULL;}
const void *pe_proc_ordinal(PeLoader *l,PeModule *m,u32 ordinal) {return proc(l,m,NULL,ordinal);}

/* --- resources ------------------------------------------------------------ */
static int find_resource(const PeModule *,PeResId,PeResId,u16,const u8 **);
/* One level of the resource tree: the entry for id, or a name compared
 * case-insensitively with the UTF-16 directory string. language 0 takes the
 * first entry. Returns the entry's OffsetToData word, or UINT32_MAX. */
static u32 resource_entry(const PeModule *m,const u8 *root,u32 offset,PeResId want,int any) {
    const u8 *dir=root+offset; if(offset>m->resource_size || m->resource_size-offset<16) return UINT32_MAX;
    u32 named=rd16(dir+12),ids=rd16(dir+14);
    if((u64)(named+ids)*8>m->resource_size-offset-16) return UINT32_MAX;
    for(u32 i=0;i<named+ids;i++) {
        const u8 *e=dir+16+i*8; u32 key=rd32(e),value=rd32(e+4);
        if(any) return value;
        if(want.name) {
            if(!(key>>31)) continue;
            u32 s=key&0x7fffffff; if(s>m->resource_size || m->resource_size-s<2) continue;
            u32 n=rd16(root+s); if((u64)n*2>m->resource_size-s-2 || n!=strlen(want.name)) continue;
            u32 k=0; while(k<n && rd16(root+s+2+k*2)<0x80 && upper((char)rd16(root+s+2+k*2))==upper(want.name[k])) k++;
            if(k==n) return value;
        } else if(!(key>>31) && key==want.id) return value;
    }
    return UINT32_MAX;
}
int pe_resource(const PeModule *m,PeResId type,PeResId name,u16 language,const void **data,u32 *size) {
    *data=NULL; *size=0;
    if(!m || (m->flags&PE_FLAG_BUILTIN)) return DE_FUNCTION;
    const u8 *entry; int e=find_resource(m,type,name,language,&entry); if(e) return e;
    *data=m->base+rd32(entry); *size=rd32(entry+4); return 0;
}
const u8 *pe_resource_entry(const PeModule *m,PeResId type,PeResId name,u16 language) {
    const u8 *entry;
    if(!m || (m->flags&PE_FLAG_BUILTIN) || find_resource(m,type,name,language,&entry)) return NULL;
    return entry;
}
static int find_resource(const PeModule *m,PeResId type,PeResId name,u16 language,const u8 **out) {
    const u8 *root=at(m,m->resource_rva,m->resource_size); if(!root || m->resource_size<16) return DE_NOFILE;
    u32 v=resource_entry(m,root,0,type,0);
    if(v==UINT32_MAX || !(v>>31)) return DE_NOFILE;
    v=resource_entry(m,root,v&0x7fffffff,name,0);
    if(v==UINT32_MAX || !(v>>31)) return DE_NOFILE;
    u32 dir=v&0x7fffffff;
    v=language?resource_entry(m,root,dir,(PeResId){language,NULL},0):UINT32_MAX;
    if(v==UINT32_MAX) v=resource_entry(m,root,dir,(PeResId){0,NULL},1); /* first language */
    if(v==UINT32_MAX || (v>>31)) return DE_NOFILE;
    if(v>m->resource_size || m->resource_size-v<16) return DE_FORMAT;
    if(!at(m,rd32(root+v),rd32(root+v+4))) return DE_FORMAT;
    *out=root+v; return 0;
}
