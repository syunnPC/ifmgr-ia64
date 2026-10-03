/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows 3.0 NE loader. Keep the file resident for resources; its NE
 * header selector is the module handle. Load all segments as fixed/preloaded
 * and apply internal and named/ordinal import relocations. Floating-point
 * fixups are unnecessary because a coprocessor is always available.
 * The automatic data segment contains static data, stack, then a local
 * heap that may grow to 64 KiB.
 *
 * DLLs share references from importing modules and LoadLibrary. Patch each
 * export's "push ds; pop ax" prolog to "mov ax, DGROUP". LibEntry runs when
 * the first loading task can execute; WEP runs on the last release.
 * Self-loading programs are unsupported.
 */
#include "wow.h"
#define MODULES 32
static Module16 modules[MODULES];
static int load(LPCSTR path,BOOL library,Module16 **out);

/* Name of module reference n (1-based) as a C string. */
static void import_name(Module16 *m,WORD n,char *out) {
    const BYTE *h=m->image+m->ne;
    const BYTE *ref=m->image+m->ne+get16(h+0x28)+(n-1)*2;
    const BYTE *s=m->image+m->ne+get16(h+0x2a)+get16(ref);
    unsigned len=s[0]>8?8:s[0],i;
    for(i=0;i<len;i++) out[i]=(char)s[1+i];
    out[len]=0;
}
/* A movable entry point (ordinal) as segment number and offset. */
static BOOL entry_point(Module16 *m,WORD ordinal,WORD *segment,WORD *offset) {
    const BYTE *h=m->image+m->ne,*p=m->image+m->ne+get16(h+4),*end=p+get16(h+6);
    WORD n=1;
    while(p<end && p[0]) {
        BYTE count=p[0],type=p[1]; unsigned i;
        p+=2;
        if(!type) {n=(WORD)(n+count); continue;}
        for(i=0;i<count;i++,n++) {
            if(type==0xff) {if(n==ordinal) {*segment=p[3]; *offset=get16(p+4); return TRUE;} p+=6;}
            else {if(n==ordinal) {*segment=type; *offset=get16(p+1); return TRUE;} p+=3;}
        }
    }
    return FALSE;
}
/* --- libraries ---------------------------------------------------------- */
Module16 *NeFromName(LPCSTR name) {
    unsigned i;
    for(i=0;i<MODULES;i++) if(modules[i].used && !lstrcmpi(modules[i].name,name)) return &modules[i];
    return NULL;
}
Module16 *NeFromCode(WORD sel) {
    unsigned i,k;
    sel|=7;
    for(i=0;i<MODULES;i++) if(modules[i].used) for(k=0;k<modules[i].segments;k++) if(modules[i].seg[k].sel==sel) return &modules[i];
    return NULL;
}
BOOL NeEntry(Module16 *m,WORD ordinal,DWORD *address) {
    WORD segment,offset;
    if(!ordinal || !entry_point(m,ordinal,&segment,&offset) || !segment || segment>m->segments) return FALSE;
    *address=(DWORD)m->seg[segment-1].sel<<16|offset;
    return TRUE;
}
/* Whether n characters are a name, without case. */
static BOOL same_name(const BYTE *s,unsigned n,LPCSTR name) {
    unsigned i;
    if((int)n!=lstrlen(name)) return FALSE;
    for(i=0;i<n;i++) {char a=(char)s[i],b=name[i]; if(a>='a' && a<='z') a=(char)(a-32); if(b>='a' && b<='z') b=(char)(b-32); if(a!=b) return FALSE;}
    return TRUE;
}
/* A name in a names table (length, name, ordinal) without case. */
static WORD table_ordinal(const BYTE *p,const BYTE *end,LPCSTR name) {
    while(p<end && p[0]) {
        if(same_name(p+1,p[0],name)) return get16(p+1+p[0]);
        p+=1+p[0]+2;
    }
    return 0;
}
WORD NeOrdinal(Module16 *m,LPCSTR name) {
    const BYTE *h=m->image+m->ne; DWORD at=get32(h+0x2c),size=get16(h+0x20); WORD o;
    if(!name || !name[0]) return 0;
    if((o=table_ordinal(h+get16(h+0x26),h+get16(h+0x28),name))!=0) return o;
    if(at && at<m->image_size) {if(size>m->image_size-at) size=m->image_size-at; o=table_ordinal(m->image+at,m->image+at+size,name);}
    return o;
}
/* A library's file: next to the module that wants it, in the Windows and
 * system directories, then along PATH and in the current directory. */
static int find_library(LPCSTR name,LPCSTR near_path,Module16 **out) {
    char path[160]; int e=2,i,n; LPCSTR env;
    BOOL has_dir=FALSE,has_ext=FALSE;
    for(i=0;name[i];i++) {if(name[i]=='\\' || name[i]==':') {has_dir=TRUE; has_ext=FALSE;} else if(name[i]=='.') has_ext=TRUE;}
    if(has_dir) return load(name,TRUE,out);
    for(i=0;i<5 && e==2;i++) {
        path[0]=0;
        if(i==0 && near_path) {lstrcpyn(path,near_path,sizeof(path)); for(n=lstrlen(path);n>0 && path[n-1]!='\\' && path[n-1]!=':';n--) {} path[n]=0;}
        else if(i==1) GetWindowsDirectory(path,sizeof(path)-20);
        else if(i==2) GetSystemDirectory(path,sizeof(path)-20);
        else if(i==3) {
            for(env=GetDOSEnvironment();env && *env;env+=lstrlen(env)+1) if(!memcmp(env,"PATH=",5)) break;
            if(env && *env) {
                LPCSTR p=env+5;
                while(*p && e==2) {
                    for(n=0;p[n] && p[n]!=';' && n<120;n++) path[n]=p[n];
                    path[n]=0; p+=n; if(*p==';') p++;
                    if(n && path[n-1]!='\\') lstrcat(path,"\\");
                    lstrcat(path,name); if(!has_ext) lstrcat(path,".DLL");
                    if(n) e=load(path,TRUE,out);
                }
            }
            continue;
        }
        n=lstrlen(path);
        if(n && path[n-1]!='\\' && path[n-1]!=':') lstrcat(path,"\\");
        if(i==0 && !near_path) continue;
        lstrcat(path,name); if(!has_ext) lstrcat(path,".DLL");
        e=load(path,TRUE,out);
    }
    return e;
}
/* A module name from a file name: the base name, at most eight characters. */
void NeModuleName(LPCSTR name,char *base) {
    unsigned n,start=0;
    for(n=0;name[n];n++) if(name[n]=='\\' || name[n]==':') start=n+1;
    for(n=0;name[start+n] && name[start+n]!='.' && n<8;n++) base[n]=name[start+n];
    base[n]=0;
}
/* A library by module or file name, loaded or held once more. */
int NeLoadLibrary(LPCSTR name,LPCSTR near_path,Module16 **out) {
    char base[9]; Module16 *m; int e;
    NeModuleName(name,base);
    if((m=NeFromName(base))!=NULL) {
        if(!m->library) return 11;
        m->usage++; *out=m; return 0;
    }
    e=find_library(name,near_path,&m);
    if(e) return e;
    m->usage=1; *out=m;
    return 0;
}
static void release_imports(Module16 *m) {
    WORD i,n=m->import_count;
    m->import_count=0;
    for(i=0;i<n;i++) NeRelease(m->imports[i]);
}
void NeRelease(Module16 *m) {
    if(!m || !m->used || !m->library || --m->usage) return;
    if(m->initialized) {Task16 *t=CurrentTask16(); if(t) CallWep16(t,m);}
    User16TaskEnded(m); LocalTaskEnded16(m);
    release_imports(m);
    NeFree(m);
}
void NeReleaseImports(Module16 *m) {if(m && m->used) release_imports(m);}
/* LibEntry for the libraries m imports that have not run it, their own
 * libraries first; FALSE when one fails. */
BOOL NeInitLibraries(Task16 *t,Module16 *m) {
    WORD i;
    for(i=0;i<m->import_count;i++) {
        Module16 *lib=m->imports[i];
        if(lib->initialized) continue;
        lib->initialized=TRUE;
        if(!NeInitLibraries(t,lib) || !CallLibEntry16(t,lib)) return FALSE;
    }
    return TRUE;
}
/* The library a relocation names, held by m once. */
static Module16 *import_library(Module16 *m,LPCSTR name) {
    Module16 *lib; WORD i; int e;
    for(i=0;i<m->import_count;i++) if(!lstrcmpi(m->imports[i]->name,name)) return m->imports[i];
    if(m->import_count==sizeof(m->imports)/sizeof(m->imports[0])) return NULL;
    if((e=NeLoadLibrary(name,m->path,&lib))!=0) return NULL;
    m->imports[m->import_count++]=lib;
    return lib;
}
/* A library's exported entry points load DS from AX: make that the
 * library's data segment. */
static void patch_prologs(Module16 *m) {
    const BYTE *h=m->image+m->ne,*p=m->image+m->ne+get16(h+4),*end=p+get16(h+6);
    if(!m->autodata) return;
    while(p<end && p[0]) {
        BYTE count=p[0],type=p[1]; unsigned i;
        p+=2;
        if(!type) continue;
        for(i=0;i<count;i++,p+=type==0xff?6:3) {
            BYTE flags=p[0],segment=type==0xff?p[3]:type; WORD offset=type==0xff?get16(p+4):get16(p+1); BYTE *code;
            if(!(flags&1) || !segment || segment>m->segments || m->seg[segment-1].flags&1 || (DWORD)offset+3>m->seg[segment-1].size) continue;
            code=m->seg[segment-1].memory+offset;
            if((code[0]==0x1e && code[1]==0x58 && code[2]==0x90) || (code[0]==0x8c && code[1]==0xd8 && code[2]==0x90)) {
                code[0]=0xb8; put16(code+1,m->instance);
            }
        }
    }
}

static int relocate(Module16 *m,unsigned index,const BYTE *rel,DWORD available) {
    Segment *s=&m->seg[index];
    WORD count,i;
    if(available<2) return 11;
    count=get16(rel); rel+=2;
    if((DWORD)count*8+2>available) return 11;
    for(i=0;i<count;i++,rel+=8) {
        BYTE source=rel[0],kind=rel[1]&3;
        BOOL additive=(rel[1]&4)!=0;
        WORD offset=get16(rel+2),target_sel=0,target_off=0;
        unsigned guard=0;
        if(kind==0) {
            WORD segment=rel[4],at=get16(rel+6);
            if(segment==0xff && !entry_point(m,at,&segment,&at)) return 11;
            if(!segment || segment>m->segments) return 11;
            target_sel=m->seg[segment-1].sel; target_off=at;
        } else if(kind==1 || kind==2) {
            char module[9],text[64]; WORD ordinal=get16(rel+6); DWORD thunk=0; Module16 *lib=NULL;
            import_name(m,get16(rel+4),module); text[0]=0;
            if(kind==2) {
                const BYTE *name=m->image+m->ne+get16(m->image+m->ne+0x2a)+ordinal;
                unsigned len=name[0]<63?name[0]:63,k;
                for(k=0;k<len;k++) text[k]=(char)name[1+k];
                text[len]=0;
            }
            if(ThunkModuleHandle(module)) thunk=ThunkAddress(module,kind==2?ThunkOrdinal(module,text):ordinal);
            else if((lib=import_library(m,module))!=NULL && !NeEntry(lib,kind==2?NeOrdinal(lib,text):ordinal,&thunk)) thunk=0;
            if(!thunk) {
                char line[100];
                if(ThunkModuleHandle(module) || lib) wsprintf(line,"WOW: %s needs %s.%s, which is not available",m->name,module,kind==2?text:"(ordinal)");
                else wsprintf(line,"WOW: %s needs module %s, which is not available",m->name,module);
                wh_trace(line);
                return 2;
            }
            target_sel=HIWORD(thunk); target_off=LOWORD(thunk);
        } else continue; /* floating-point fixup */
        for(;;) {
            BYTE *at;
            WORD next;
            if((DWORD)offset+(source==3?4:source==0?1:2)>s->size || ++guard>0x8000) return 11;
            at=s->memory+offset; next=get16(at);
            switch(source) {
            case 0: at[0]=(BYTE)(additive?at[0]+target_off:target_off); break;
            case 2: put16(at,(WORD)(additive?get16(at)+target_sel:target_sel)); break;
            case 3:
                if(additive) put16(at,(WORD)(get16(at)+target_off)); else put16(at,target_off);
                put16(at+2,target_sel); break;
            case 5: put16(at,(WORD)(additive?get16(at)+target_off:target_off)); break;
            default: return 11;
            }
            if(additive || next==0xffff) break;
            offset=next;
        }
    }
    return 0;
}

Module16 *NeFromHandle(WORD h) {
    unsigned i;
    for(i=0;i<MODULES;i++) if(modules[i].used && h && (modules[i].instance==h || modules[i].handle==h)) return &modules[i];
    return NULL;
}
void NeFree(Module16 *m) {
    unsigned i;
    if(!m || !m->used) return;
    Resources16Freed(m);
    for(i=0;i<m->segments;i++) {SelFree(m->seg[i].sel); Free16(m->seg[i].memory,m->seg[i].size);}
    SelFree(m->handle);
    Free16(m->image,m->image_size);
    memset(m,0,sizeof(*m));
}

int NeLoad(LPCSTR path,Module16 **out) {return load(path,FALSE,out);}
static int load(LPCSTR path,BOOL library,Module16 **out) {
    Module16 *m=NULL; HFILE f; LONG size; const BYTE *h; unsigned i,shift; int e=0;
    for(i=0;i<MODULES && !m;i++) if(!modules[i].used) m=&modules[i];
    if(!m) return 8; /* every slot in use: out of memory, as LoadModule says */
    memset(m,0,sizeof(*m));
    f=_lopen(path,OF_READ);
    if(f==HFILE_ERROR) return 2;
    size=_llseek(f,0,2);
    if(size<0x40 || size>0x400000 || _llseek(f,0,0)!=0) {_lclose(f); return 11;}
    m->image_size=(DWORD)size; m->image=(BYTE *)Alloc16(m->image_size);
    if(!m->image) {_lclose(f); return 8;}
    if(_lread(f,m->image,(UINT)size)!=(UINT)size) e=11;
    _lclose(f);
    m->used=TRUE; lstrcpyn(m->path,path,sizeof(m->path));
    if(e) {NeFree(m); return e;}
    m->ne=get32(m->image+0x3c);
    if(get16(m->image)!=0x5a4d || m->ne>m->image_size-0x40 || get16(m->image+m->ne)!=0x454e) {NeFree(m); return 11;}
    h=m->image+m->ne;
    m->flags=get16(h+0x0c); m->autodata=get16(h+0x0e); m->heap=get16(h+0x10); m->stack=get16(h+0x12);
    m->ip=get16(h+0x14); m->cs=get16(h+0x16); m->sp=get16(h+0x18); m->ss=get16(h+0x1a);
    m->segments=get16(h+0x1c); shift=get16(h+0x32); if(!shift) shift=9;
    {
        const BYTE *name=m->image+m->ne+get16(h+0x26); unsigned len=name[0]>8?8:name[0];
        memcpy(m->name,name+1,len); m->name[len]=0;
    }
    m->library=(m->flags&0x8000)!=0;
    if(m->library!=library) {NeFree(m); return 11;}
    if(m->flags&0x0800 || !m->segments || m->segments>MAX_SEGMENTS || m->cs>m->segments || (!library && (!m->cs || !m->ss)) ||
       m->ss>m->segments || (m->autodata && m->autodata>m->segments)) {NeFree(m); return 11;}
    for(i=0;i<m->segments && !e;i++) {
        const BYTE *t=h+get16(h+0x22)+i*8;
        DWORD file=(DWORD)get16(t)<<shift,length=get16(t+2),memory=get16(t+6);
        Segment *s=&m->seg[i];
        if(!length && get16(t)) length=0x10000;
        if(!memory) memory=0x10000;
        if(memory<length) memory=length;
        if(i+1==m->autodata) {m->heap_start=memory+m->stack; if(m->heap_start>0xff00) m->heap_start=0xff00; memory=0x10000;}
        s->flags=get16(t+4); s->size=memory;
        if(get16(t) && file+length>m->image_size) {e=11; break;}
        s->memory=(BYTE *)Alloc16(memory);
        if(!s->memory) {e=8; break;}
        if(get16(t)) memcpy(s->memory,m->image+file,length);
        s->sel=SelAlloc(SelLinear(s->memory),memory-1,(BYTE)(s->flags&1?SEL_DATA:SEL_CODE));
        if(!s->sel) e=8;
    }
    for(i=0;i<m->segments && !e;i++) {
        const BYTE *t=h+get16(h+0x22)+i*8;
        DWORD file=(DWORD)get16(t)<<shift,length=get16(t+2);
        if(!length && get16(t)) length=0x10000;
        if((get16(t+4)&0x100) && get16(t)) e=relocate(m,i,m->image+file+length,file+length<=m->image_size?m->image_size-file-length:0);
    }
    if(!e) {
        DWORD limit=m->image_size-m->ne-1;
        m->handle=SelAlloc(SelLinear(m->image+m->ne),limit>0xffff?0xffff:limit,SEL_DATA);
        if(!m->handle) e=8;
    }
    if(e) {release_imports(m); NeFree(m); return e;}
    m->instance=m->autodata?m->seg[m->autodata-1].sel:m->handle;
    if(library) {patch_prologs(m); *out=m; return 0;}
    if(!m->sp) {
        const BYTE *t=h+get16(h+0x22)+(m->ss-1)*8; DWORD top=get16(t+6);
        if(!top) top=0x10000;
        top+=m->stack; if(top>0xfffe) top=0xfffe;
        m->sp=(WORD)(top&~1U);
    }
    *out=m; return 0;
}

/* A resource in the file image: type and name are integers (below 0x10000)
 * or strings, compared without case as the table stores them. */
static BOOL match(const BYTE *table,WORD id,LPCSTR want) {
    if(!HIWORD((ULONG_PTR)want)) return id==(0x8000|LOWORD((ULONG_PTR)want));
    return !(id&0x8000) && same_name(table+id+1,table[id],want);
}
static const BYTE *lookup(Module16 *m,LPCSTR type,LPCSTR name,DWORD *size) {
    const BYTE *h=m->image+m->ne,*table=m->image+m->ne+get16(h+0x24),*p;
    unsigned shift;
    if(get16(h+0x24)==get16(h+0x26)) return NULL;
    shift=get16(table); p=table+2;
    while(get16(p)) {
        WORD count=get16(p+2); BOOL wanted=match(table,get16(p),type); unsigned i;
        p+=8;
        for(i=0;i<count;i++,p+=12) if(wanted && match(table,get16(p+6),name)) {
            DWORD at=(DWORD)get16(p)<<shift,bytes=(DWORD)get16(p+2)<<shift;
            if(at>=m->image_size) return NULL;
            if(bytes>m->image_size-at) bytes=m->image_size-at;
            *size=bytes; return m->image+at;
        }
    }
    return NULL;
}
/* Windows 3.0's resource compiler numbered named resources and listed their
 * names in name tables (type 15), which Windows 3.0 looks in: entries of a
 * length, the type (0x8000 with a name), the number, the type's name and
 * the resource's. */
static BOOL by_name(Module16 *m,LPCSTR *type,LPCSTR *name) {
    DWORD size,at,len; const BYTE *t; WORD n;
    for(n=1;(t=lookup(m,MAKEINTRESOURCE(15),MAKEINTRESOURCE(n),&size))!=NULL;n++)
        for(at=0;at+6<size && (len=get16(t+at))>=6 && len<=size-at;at+=len) {
            const BYTE *e=t+at,*tname=e+6,*rname,*end=e+len; WORD kind=get16(e+2),id=get16(e+4);
            for(rname=tname;rname<end && *rname;rname++) {}
            if(++rname>=end) continue;
            if(kind&0x8000) {if(!HIWORD((ULONG_PTR)*type) || !same_name(tname,(unsigned)(rname-1-tname),*type)) continue;}
            else if(HIWORD((ULONG_PTR)*type) || LOWORD((ULONG_PTR)*type)!=kind) continue;
            if(HIWORD((ULONG_PTR)*name)) {
                const BYTE *z=rname; while(z<end && *z) z++;
                if(z==end || !same_name(rname,(unsigned)(z-rname),*name)) continue;
            } else if((id&0x7fff)!=LOWORD((ULONG_PTR)*name)) continue;
            *type=MAKEINTRESOURCE(kind&0x7fff); *name=MAKEINTRESOURCE(id&0x7fff);
            return TRUE;
        }
    return FALSE;
}
const BYTE *NeResource(Module16 *m,LPCSTR type,LPCSTR name,DWORD *size) {
    const BYTE *r=lookup(m,type,name,size);
    if(!r && (HIWORD((ULONG_PTR)type) || HIWORD((ULONG_PTR)name)) && by_name(m,&type,&name)) r=lookup(m,type,name,size);
    return r;
}
