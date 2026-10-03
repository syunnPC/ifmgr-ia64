/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: KERNEL functions for Win16 programs (INT 21h is int21.c's).
 */
#include "api.h"

/* InitTask: the instance data at the start of DGROUP gets the stack
 * limits; AX 1, BX the command line in the PSP (ES), CX the stack limit,
 * DX the show command, SI the previous instance, DI this one. A zero word
 * is left pushed on the caller's stack, as Windows does. */
DWORD W16_InitTask(Args16 *a) {
    Task16 *t=a->task; WORD ds=Seg16(t,3),sp=Reg16(t,SP),stackmin=(WORD)(sp+4),top;
    BYTE *instance=(BYTE *)SelPointer(ds,0),*psp=(BYTE *)SelPointer(t->psp,0);
    WORD bx=0x81;
    top=(WORD)((stackmin>Reg16(t,BX)?stackmin-Reg16(t,BX):0)+150);
    put16(instance+0x0a,top); put16(instance+0x0c,stackmin); put16(instance+0x0e,stackmin);
    while(bx<0x81+psp[0x80] && (psp[bx]==' ' || psp[bx]=='\t')) bx++;
    if(Reg16(t,CX)) LocalInit16(ds,0,0);
    Return16(t,0);
    sp=(WORD)(Reg16(t,SP)-2); SetReg16(t,SP,sp);
    put16((BYTE *)SelPointer(Seg16(t,2),sp),0);
    SetReg16(t,AX,1); SetReg16(t,BX,bx); SetReg16(t,CX,top); SetReg16(t,DX,(WORD)t->show);
    SetReg16(t,SI,0); SetReg16(t,DI,t->module->instance); SetSeg16(t,0,t->psp);
    return 0;
}
DWORD W16_DOS3Call(Args16 *a) {Int21(a->task); Return16(a->task,0); return 0;}
DWORD W16_GetVersion(Args16 *a) {(void)a; return 0x04000003UL;} /* Windows 3.0 on DOS 4.00 */
/* Protected mode on a 386 with a coprocessor, in enhanced mode. */
DWORD W16_GetWinFlags(Args16 *a) {(void)a; return 0x0425;}
DWORD W16_WaitEvent(Args16 *a) {(void)a; wh_yield(); return 0;}
DWORD W16_GetModuleFileName(Args16 *a) {
    Module16 *m=NeFromHandle((WORD)a->a[0]); char *out=(char *)PTR(a->a[1]); int size=(int)(short)a->a[2];
    if(!m) m=a->task->module;
    if(!out || size<=0) return 0;
    lstrcpyn(out,m->path,size);
    return (DWORD)lstrlen(out);
}



/* --- modules and the rest ------------------------------------------------- */
DWORD W16_GetModuleHandle(Args16 *a) {
    Task16 *t=a->task; DWORD raw=a->raw[0]; LPCSTR name=(LPCSTR)Lin16(raw); char base[9]; Module16 *m;
    if(!HIWORD(raw)) {m=NeFromHandle(LOWORD(raw)); return m?m->handle:0;}
    if(!name) return 0;
    NeModuleName(name,base);
    if(t->module && !lstrcmpi(base,t->module->name)) return t->module->handle;
    if((m=NeFromName(base))!=NULL) return m->handle;
    return ThunkModuleHandle(base);
}
DWORD W16_GetProcAddress(Args16 *a) {
    LPCSTR module=ThunkModuleName((WORD)a->a[0]); DWORD raw=a->raw[1],address; WORD ordinal; Module16 *m;
    if(module) {
        ordinal=HIWORD(raw)?ThunkOrdinal(module,(LPCSTR)Lin16(raw)):LOWORD(raw);
        return ordinal && ThunkImplemented(module,ordinal)?ThunkAddress(module,ordinal):0;
    }
    if(!(m=NeFromHandle((WORD)a->a[0]))) return 0;
    ordinal=HIWORD(raw)?NeOrdinal(m,(LPCSTR)Lin16(raw)):LOWORD(raw);
    return NeEntry(m,ordinal,&address)?address:0;
}
/* LoadLibrary gives the library's instance handle, or an error below 32. */
DWORD W16_LoadLibrary(Args16 *a) {
    Task16 *t=a->task; LPCSTR name=(LPCSTR)PTR(a->a[0]); char base[9]; WORD h; Module16 *m; unsigned i; int e;
    if(!name) return 2;
    NeModuleName(name,base);
    if((h=ThunkModuleHandle(base))!=0) return h;
    for(i=0;i<sizeof(t->loaded)/sizeof(t->loaded[0]) && t->loaded[i];i++) {}
    if(i==sizeof(t->loaded)/sizeof(t->loaded[0])) return 8;
    if((e=NeLoadLibrary(name,t->module->path,&m))!=0) return (DWORD)e;
    t->loaded[i]=m;
    if(!m->initialized) {
        m->initialized=TRUE;
        if(!NeInitLibraries(t,m) || !CallLibEntry16(t,m)) {t->loaded[i]=NULL; NeRelease(m); return 20;}
    }
    return m->instance;
}
DWORD W16_FreeLibrary(Args16 *a) {
    Task16 *t=a->task; Module16 *m=NeFromHandle((WORD)a->a[0]); unsigned i;
    if(!m || !m->library) return 0;
    for(i=0;i<sizeof(t->loaded)/sizeof(t->loaded[0]);i++) if(t->loaded[i]==m) {t->loaded[i]=NULL; NeRelease(m); break;}
    return 0;
}
DWORD W16_FreeModule(Args16 *a) {Module16 *m=NeFromHandle((WORD)a->a[0]); if(m && m->library) W16_FreeLibrary(a); return m!=NULL;}
/* A library's procedures load their own data segment; a program's get a
 * thunk that sets AX to the instance's. */
DWORD W16_MakeProcInstance(Args16 *a) {
    Module16 *code=NeFromCode(HIWORD(a->raw[0])),*m=NeFromHandle((WORD)a->a[1]);
    if(!a->raw[0] || (code && code->library)) return a->raw[0];
    if(!m) m=a->task->module;
    return InstanceThunk16(a->raw[0],m->instance);
}
DWORD W16_FreeProcInstance(Args16 *a) {(void)a; return 0;}
DWORD W16_GetModuleUsage(Args16 *a) {Module16 *m=NeFromHandle((WORD)a->a[0]); return m && m->library?m->usage:1;}
DWORD W16_GetCurrentTask(Args16 *a) {return a->task->psp;}
DWORD W16_IsDBCSLeadByte(Args16 *a) {(void)a; return 0;}
/* The environment: a copy of WIN.COM's, in a segment named by PSP:2Ch. */
DWORD W16_GetDOSEnvironment(Args16 *a) {return MAKELONG(0,a->task->environment);}
WORD Environment16(void) {
    LPCSTR env=GetDOSEnvironment(); DWORD n=0; BYTE *p; WORD sel;
    if(!env) return 0;
    while(env[n] || env[n+1]) n++;
    n+=2;
    if(!(p=(BYTE *)Alloc16(n+16))) return 0;
    memcpy(p,env,n);
    sel=SelAlloc(SelLinear(p),n+15,SEL_DATA);
    if(!sel) Free16(p,n+16);
    return sel;
}
/* Catch and Throw: CATCHBUF holds IP, CS, SP, BP, SI, DI, DS and SS as they
 * are after Catch returns; Throw goes back there with AX set. It works
 * within one call depth: a Throw out of a procedure called back by native
 * code cannot unwind the native frames. */
DWORD W16_Catch(Args16 *a) {
    Task16 *t=a->task; WORD sp=Reg16(t,SP),ss=Seg16(t,2),*b=(WORD *)Lin16((DWORD)Peek16(ss,(WORD)(sp+6))<<16|Peek16(ss,(WORD)(sp+4)));
    if(b) {
        b[0]=Peek16(ss,sp); b[1]=Peek16(ss,(WORD)(sp+2)); b[2]=(WORD)(sp+8); b[3]=Reg16(t,BP);
        b[4]=Reg16(t,SI); b[5]=Reg16(t,DI); b[6]=Seg16(t,3); b[7]=ss; b[8]=(WORD)t->depth;
    }
    SetReg16(t,AX,0); Return16(t,4); return 0;
}
DWORD W16_Throw(Args16 *a) {
    Task16 *t=a->task; WORD sp=Reg16(t,SP),ss=Seg16(t,2),value=Peek16(ss,(WORD)(sp+4));
    const WORD *b=(const WORD *)Lin16((DWORD)Peek16(ss,(WORD)(sp+8))<<16|Peek16(ss,(WORD)(sp+6)));
    if(!b || b[8]!=(WORD)t->depth) {Fault16(t,"Throw to another call depth"); return 0;}
    SetSeg16(t,2,b[7]); SetReg16(t,SP,b[2]); SetReg16(t,BP,b[3]); SetReg16(t,SI,b[4]); SetReg16(t,DI,b[5]);
    SetSeg16(t,3,b[6]); SetSeg16(t,1,b[1]); t->cpu.eip=b[0]; SetReg16(t,AX,value);
    return 0;
}

/* --- resources ------------------------------------------------------------ */
/* A resource handle (HRSRC) is an entry of this table times four;
 * LoadResource copies the data into a global block, kept (and counted) until
 * FreeResource. A name or type "#n" is the number n. */
#define RESOURCES 256
static struct {Module16 *module; const BYTE *data; DWORD size; WORD block,loads;} resources[RESOURCES];
static LPCSTR resource_name(LPCSTR s) {
    if(HIWORD((ULONG_PTR)s) && s[0]=='#') {
        UINT n=0; LPCSTR p=s+1;
        while(*p>='0' && *p<='9') n=n*10+(UINT)(*p++-'0');
        if(!*p) return MAKEINTRESOURCE(n);
    }
    return s;
}
static int resource_index(WORD h) {
    unsigned i=(unsigned)(h/4);
    return h%4 || !i || i>RESOURCES || !resources[i-1].data?-1:(int)(i-1);
}
void Resources16Freed(Module16 *m) {
    unsigned i;
    for(i=0;i<RESOURCES;i++) if(resources[i].module==m) {
        if(resources[i].block) GlobalFree16(resources[i].block);
        memset(&resources[i],0,sizeof(resources[i]));
    }
}
static Module16 *module_or_task(Args16 *a,WORD h) {Module16 *m=NeFromHandle(h); return m?m:a->task->module;}
DWORD W16_FindResource(Args16 *a) {
    Module16 *m=module_or_task(a,(WORD)a->a[0]); DWORD size; const BYTE *p; unsigned i,free_slot=RESOURCES;
    p=NeResource(m,resource_name((LPCSTR)PTR(a->a[2])),resource_name((LPCSTR)PTR(a->a[1])),&size);
    if(!p) return 0;
    for(i=0;i<RESOURCES;i++) {
        if(resources[i].data==p) return (i+1)*4;
        if(!resources[i].data && free_slot==RESOURCES) free_slot=i;
    }
    if(free_slot==RESOURCES) return 0;
    resources[free_slot].module=m; resources[free_slot].data=p; resources[free_slot].size=size;
    return (free_slot+1)*4;
}
DWORD W16_SizeofResource(Args16 *a) {int i=resource_index((WORD)a->a[1]); return i<0?0:resources[i].size;}
DWORD W16_LoadResource(Args16 *a) {
    int i=resource_index((WORD)a->a[1]); BYTE *p;
    if(i<0) return 0;
    if(resources[i].block && SelValid(resources[i].block)) {resources[i].loads++; return resources[i].block;}
    /* The block belongs to the module, not the task. */
    if(!(resources[i].block=GlobalAlloc16(NULL,0,resources[i].size))) return 0;
    p=(BYTE *)SelPointer(resources[i].block,0);
    memcpy(p,resources[i].data,resources[i].size);
    resources[i].loads=1;
    return resources[i].block;
}
DWORD W16_LockResource(Args16 *a) {WORD h=(WORD)a->a[0]; return h && SelValid(h)?MAKELONG(0,h|7):0;}
DWORD W16_FreeResource(Args16 *a) {
    unsigned i; WORD h=(WORD)(a->a[0]|7);
    for(i=0;i<RESOURCES;i++) if(resources[i].block==h) {
        if(!--resources[i].loads) {GlobalFree16(h); resources[i].block=0;}
        return 0;
    }
    return h;
}
/* AccessResource: the module's file, open at the resource's data. */
DWORD W16_AccessResource(Args16 *a) {
    int i=resource_index((WORD)a->a[1]); HFILE f;
    if(i<0) return (DWORD)-1;
    f=_lopen(resources[i].module->path,OF_READ);
    if(f!=HFILE_ERROR) _llseek(f,(LONG)(resources[i].data-resources[i].module->image),0);
    return (WORD)f;
}
DWORD W16_AllocResource(Args16 *a) {
    int i=resource_index((WORD)a->a[1]);
    return i<0?0:GlobalAlloc16(a->task,0,a->a[2]>resources[i].size?a->a[2]:resources[i].size);
}
DWORD W16_SetResourceHandler(Args16 *a) {(void)a; return 0;}

/* --- atoms ------------------------------------------------------------------ */
/* KERNEL's: a task's own table, and the one all tasks share (GlobalAddAtom),
 * native programs too, so DDE can name things across. */
DWORD W16_InitAtomTable(Args16 *a) {(void)a; return 1;}
DWORD W16_AddAtom(Args16 *a) {return AddAtom((LPCSTR)PTR(a->a[0]));}
DWORD W16_FindAtom(Args16 *a) {return FindAtom((LPCSTR)PTR(a->a[0]));}
DWORD W16_DeleteAtom(Args16 *a) {return DeleteAtom((ATOM)a->a[0]);}
DWORD W16_GetAtomName(Args16 *a) {return GetAtomName((ATOM)a->a[0],(LPSTR)PTR(a->a[1]),(int)a->a[2]);}
DWORD W16_GetAtomHandle(Args16 *a) {return a->a[0];}
DWORD W16_GlobalAddAtom(Args16 *a) {return GlobalAddAtom((LPCSTR)PTR(a->a[0]));}
DWORD W16_GlobalFindAtom(Args16 *a) {return GlobalFindAtom((LPCSTR)PTR(a->a[0]));}
DWORD W16_GlobalDeleteAtom(Args16 *a) {return GlobalDeleteAtom((ATOM)a->a[0]);}
DWORD W16_GlobalGetAtomName(Args16 *a) {return GlobalGetAtomName((ATOM)a->a[0],(LPSTR)PTR(a->a[1]),(int)a->a[2]);}

/* --- more of KERNEL ---------------------------------------------------------- */
/* GetInstanceData: bytes of another instance's data segment, at the same
 * offset of the caller's. */
DWORD W16_GetInstanceData(Args16 *a) {
    Module16 *m=NeFromHandle((WORD)a->a[0]); WORD offset=(WORD)a->a[1],count=(WORD)a->a[2],ds=Seg16(a->task,3);
    if(!m || !SelValid(m->instance) || (DWORD)offset+count>SelLimit(m->instance)+1 || (DWORD)offset+count>SelLimit(ds)+1) return 0;
    memmove(SelPointer(ds,offset),SelPointer(m->instance,offset),count);
    return count;
}
DWORD W16_IsTask(Args16 *a) {return Task16Of((WORD)a->a[0])!=NULL;}
DWORD W16_GetCurrentPDB(Args16 *a) {return a->task->psp;}
DWORD W16_GetTaskDS(Args16 *a) {return a->task->module->instance;}
DWORD W16_GetExePtr(Args16 *a) {
    Task16 *t=Task16Of((WORD)a->a[0]); Module16 *m=t?t->module:NeFromHandle((WORD)a->a[0]);
    if(!m) m=NeFromCode((WORD)a->a[0]);
    return m?m->handle:0;
}
DWORD W16_GetCodeHandle(Args16 *a) {return HIWORD(a->raw[0]);}
DWORD W16_GetExpWinVer(Args16 *a) {(void)a; return 0x300;}
DWORD W16_SetHandleCount(Args16 *a) {return a->a[0]<255?a->a[0]:255;}
DWORD W16_DirectedYield(Args16 *a) {(void)a; wh_yield(); return 0;}
DWORD W16_GlobalWire(Args16 *a) {return SelValid((WORD)a->a[0])?MAKELONG(0,(WORD)a->a[0]|7):0;}
DWORD W16_GlobalUnWire(Args16 *a) {(void)a; return 1;}
DWORD W16_GlobalFix(Args16 *a) {(void)a; return 0;}
DWORD W16_GlobalUnfix(Args16 *a) {(void)a; return 1;}
DWORD W16_GlobalNotify(Args16 *a) {(void)a; return 0;}
DWORD W16_LocalShrink(Args16 *a) {(void)a; return 0;}
/* Plenty free in every heap: the heaps here are not shared by modules. */
DWORD W16_GetHeapSpaces(Args16 *a) {(void)a; return MAKELONG(0xe000,0xffff);}
DWORD W16_hmemcpy(Args16 *a) {if(a->a[0] && a->a[1]) memmove(PTR(a->a[0]),PTR(a->a[1]),a->a[2]); return 0;}
/* LoadModule: WinExec with the parameter block's command line and show
 * command. */
DWORD W16_LoadModule(Args16 *a) {
    LPCSTR name=(LPCSTR)PTR(a->a[0]); const BYTE *block=(const BYTE *)PTR(a->a[1]); char line[200]; UINT show=SW_SHOWNORMAL;
    const BYTE *tail,*how;
    if(!name) return 2;
    lstrcpyn(line,name,sizeof(line));
    if(block) {
        tail=(const BYTE *)Lin16(get32(block+2)); how=(const BYTE *)Lin16(get32(block+6));
        if(tail && tail[0]) {
            int n=lstrlen(line); unsigned k;
            if(n<(int)sizeof(line)-2) {line[n++]=' '; for(k=0;k<tail[0] && n<(int)sizeof(line)-1 && tail[1+k]!=13;k++) line[n++]=(char)tail[1+k]; line[n]=0;}
        }
        if(how && get16(how)==2) show=get16(how+2);
    }
    return WinExec(line,show);
}
/* Pointer checks: the selector, its limit, and for writing a data segment. */
static BOOL bad_range(DWORD segptr,DWORD count,BOOL write) {
    WORD sel=HIWORD(segptr),off=LOWORD(segptr);
    if(!sel || !SelValid(sel)) return TRUE;
    if(count && (DWORD)off+count-1>SelLimit(sel)) return TRUE;
    if(write && SelIsCode(sel)) return TRUE;
    return FALSE;
}
DWORD W16_IsBadReadPtr(Args16 *a) {return bad_range(a->raw[0],a->a[1],FALSE);}
DWORD W16_IsBadWritePtr(Args16 *a) {return bad_range(a->raw[0],a->a[1],TRUE);}
DWORD W16_IsBadHugeReadPtr(Args16 *a) {return bad_range(a->raw[0],a->a[1]>0xffff?1:a->a[1],FALSE);}
DWORD W16_IsBadHugeWritePtr(Args16 *a) {return bad_range(a->raw[0],a->a[1]>0xffff?1:a->a[1],TRUE);}
DWORD W16_IsBadCodePtr(Args16 *a) {return bad_range(a->raw[0],1,FALSE) || !SelIsCode(HIWORD(a->raw[0]));}
DWORD W16_IsBadStringPtr(Args16 *a) {
    DWORD p=a->raw[0]; WORD max=(WORD)a->a[1],i; const char *s;
    if(bad_range(p,1,FALSE)) return TRUE;
    s=(const char *)Lin16(p);
    for(i=0;i<max && (DWORD)LOWORD(p)+i<=SelLimit(HIWORD(p));i++) if(!s[i]) return FALSE;
    return i<max;
}
