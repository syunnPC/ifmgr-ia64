/* SPDX-License-Identifier: GPL-2.0-or-later
 * KERNEL.DLL: tasks, modules, resources, strings and version.
 * A task is started by WinExec: the application image is loaded, a WINHOST
 * task runs its entry (the SDK start-up code), which gets its arguments from
 * InitTask and ends with ExitProcess. Ending a task signals USER, frees the
 * task's memory and unloads the image.
 */
#include "kernel.h"
#define TASKS 32
/* A task: a program's, or a DOS program's (dos: its path, command tail,
 * start-up directory, WH_DOS_ flags and title). */
typedef struct {
    BOOL used,win16,dos; HINSTANCE instance; wh_u32 id; int show; char command[256],path[128];
    char dir[65],title[32]; DWORD dos_flags;
} TDB;
static TDB tdbs[TASKS];
static TASKSIGNALPROC signal_proc;

static TDB *current_tdb(void) {
    void **slots=wh_task_slots(0);
    return slots?(TDB *)slots[0]:NULL;
}
static void end_task(TDB *t) {
    HINSTANCE instance=t->instance;
    if(signal_proc) signal_proc((HTASK)t,SG_EXIT);
    AtomsTaskEnded(t);
    MemoryFreeTask(t->id);
    t->used=FALSE;
    if(instance) wh_free_library(instance);
}
/* A Windows 3.0 program (NE format) runs in WOW.DLL on the processor's
 * IA-32 instruction set; WOW stays loaded once used. */
typedef int (WINAPI *WIN16MAIN)(LPCSTR,LPCSTR,int);
static int run_win16(TDB *t) {
    static void *wow; WIN16MAIN entry; int e;
    if(!wow) wow=wh_load_library("WOW");
    entry=wow?(WIN16MAIN)wh_proc_address(wow,"Win16Main"):NULL;
    if(!entry) return 2;
    e=entry(t->path,t->command,t->show);
    if(e) {
        char line[160];
        if(e==0x100) wsprintf(line,"%s needs a processor with the IA-32 instruction set.",t->path);
        else wsprintf(line,"Cannot run %s (error %d).",t->path,e);
        MessageBox(NULL,line,"Interface Manager",MB_OK|MB_ICONHAND);
    }
    return e;
}
/* A DOS program, full screen; while it is set aside its icon waits for the
 * user to switch back to it. */
static int run_dos(TDB *t) {
    int e=wh_dos_session_ex(t->path,t->command,t->dir[0]?t->dir:NULL,t->dos_flags);
    while(e==WH_DOS_AWAY) {
        DosAway(t->title);
        e=wh_dos_resume();
    }
    if(e) {
        char line[200];
        wsprintf(line,"Cannot run %s (error %d).",(LPSTR)t->path,e);
        MessageBox(NULL,line,"Interface Manager",MB_OK|MB_ICONHAND);
    }
    return e;
}
static int task_start(void *arg) {
    TDB *t=(TDB *)arg; void **slots=wh_task_slots(0); const void *entry; int code=1;
    slots[0]=t;
    if(t->dos) code=run_dos(t);
    else if(t->win16) code=run_win16(t);
    else {
        entry=wh_entry(t->instance);
        if(entry) code=((int (*)(void))entry)();
    }
    end_task(t);
    return code;
}
static BOOL ne_program(LPCSTR path) {
    HFILE h=_lopen(path,OF_READ); BYTE head[64],sig[2]; BOOL ne=FALSE;
    if(h==HFILE_ERROR) return FALSE;
    if(_lread(h,head,64)==64 && head[0]=='M' && head[1]=='Z') {
        DWORD at=*(const DWORD *)(head+60);
        if(_llseek(h,(LONG)at,0)==(LONG)at && _lread(h,sig,2)==2 && sig[0]=='N' && sig[1]=='E') ne=TRUE;
    }
    _lclose(h);
    return ne;
}
/* The PE subsystem of a program file: 2 for Windows programs; DOS programs
 * here are EFI applications (10). -1 when it is not a PE image. */
static int subsystem(LPCSTR path) {
    HFILE h=_lopen(path,OF_READ); BYTE head[64],pe[96]; int sub=-1;
    if(h==HFILE_ERROR) return -1;
    if(_lread(h,head,64)==64 && head[0]=='M' && head[1]=='Z') {
        DWORD at=*(const DWORD *)(head+60);
        if(_llseek(h,(LONG)at,0)==(LONG)at && _lread(h,pe,sizeof(pe))==sizeof(pe) && !memcmp(pe,"PE\0\0",4))
            sub=*(const WORD *)(pe+24+68);
    }
    _lclose(h);
    return sub;
}
static BOOL has_extension(LPCSTR name,LPCSTR ext) {
    int n=lstrlen(name),k=lstrlen(ext);
    return n>k && !lstrcmpi(name+n-k,ext);
}
/* A PIF (program information file) as standard mode reads it: the title,
 * program, its parameters and start-up directory (OEM strings in fixed
 * fields, padded with NULs or spaces), whether the screen comes back to
 * Interface Manager at once when the program ends (bit 4 at 0x63) and
 * whether switching away is prevented (bit 2); and from the extension
 * WINDOWS 286 3.0, in the chain of headers after MICROSOFT PIFEX at 171h,
 * the shortcut keys the program reserves (its flags' bits 0, 1 and 4:
 * Alt+Tab, Alt+Esc and Ctrl+Esc). */
typedef struct {char title[31],program[64],params[65],dir[65]; BOOL close,noswitch; WORD keys;} Pif;
#define PIF_BASE 0x171
#define PIF_HEADER 0x16
static void pif_string(char *out,const BYTE *p,int n) {
    int k=0;
    while(k<n && p[k]) {out[k]=(char)p[k]; k++;}
    while(k && out[k-1]==' ') k--;
    out[k]=0;
}
static WORD pif_word(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static BOOL read_pif(LPCSTR path,Pif *pif) {
    BYTE b[0x400]; HFILE h=_lopen(path,OF_READ); UINT n; WORD at; int hops;
    if(h==HFILE_ERROR) return FALSE;
    n=_lread(h,b,sizeof(b));
    _lclose(h);
    if(n==HFILE_ERROR || n<PIF_BASE) return FALSE;
    pif_string(pif->title,b+0x02,30);
    pif_string(pif->program,b+0x24,63); pif_string(pif->dir,b+0x65,64); pif_string(pif->params,b+0xa5,64);
    pif->close=(b[0x63]&0x10)!=0; pif->noswitch=(b[0x63]&0x04)!=0; pif->keys=0;
    if(n<PIF_BASE+PIF_HEADER || memcmp(b+PIF_BASE,"MICROSOFT PIFEX",16)) return TRUE;
    for(at=PIF_BASE,hops=0;at!=0xffff && (UINT)at+PIF_HEADER<=n && hops<64;at=pif_word(b+at+16),hops++) {
        WORD data=pif_word(b+at+18),length=pif_word(b+at+20);
        if(!memcmp(b+at,"WINDOWS 286 3.0",16) && length>=6 && (UINT)data+6<=n) {pif->keys=pif_word(b+data+4); break;}
    }
    return TRUE;
}
/* The PIF for a program without one of its own: NAME.PIF beside it or along
 * the search, else _DEFAULT.PIF. */
static BOOL find_pif(LPCSTR path,Pif *pif) {
    char name[132]; OFSTRUCT of; int n=lstrlen(path),k;
    for(k=n;k>0 && path[k-1]!='.' && path[k-1]!='\\' && path[k-1]!=':';k--) {}
    if(k>0 && path[k-1]=='.' && k+3<(int)sizeof(name)) {
        lstrcpyn(name,path,k+1); lstrcat(name,"PIF");
        if(read_pif(name,pif)) return TRUE;
        for(n=k-1;n>0 && path[n-1]!='\\' && path[n-1]!=':';n--) {}
        lstrcpyn(name,path+n,k-n+1); lstrcat(name,"PIF");
        if(OpenFile(name,&of,OF_EXIST)!=HFILE_ERROR && read_pif(of.szPathName,pif)) return TRUE;
    }
    return OpenFile("_DEFAULT.PIF",&of,OF_EXIST)!=HFILE_ERROR && read_pif(of.szPathName,pif);
}
/* A DOS program (or a batch file, through COMMAND.COM) runs full screen in
 * a task of its own while Interface Manager waits, as in Windows 3.0's
 * standard mode: in the start-up directory of its PIF, waiting for a key at
 * the end unless the PIF closes the window on exit, and with the PIF's
 * title (else the program's name) when it is set aside. */
static UINT dos_program(LPCSTR path,LPCSTR tail,const Pif *given) {
    char comspec[128]; Pif pif; TDB *t=NULL; unsigned i; int k,n;
    for(i=0;i<TASKS && !t;i++) if(!tdbs[i].used) t=&tdbs[i];
    if(!t) return 0;
    memset(t,0,sizeof(*t)); t->dos=TRUE;
    if(given) pif=*given;
    if(given || find_pif(path,&pif)) {
        lstrcpyn(t->dir,pif.dir,sizeof(t->dir)); lstrcpyn(t->title,pif.title,sizeof(t->title));
        if(!pif.close) t->dos_flags|=WH_DOS_PAUSE;
        if(pif.noswitch) t->dos_flags|=WH_DOS_NOSWITCH;
        if(pif.keys&0x0001) t->dos_flags|=WH_DOS_KEEP_ALT_TAB;
        if(pif.keys&0x0002) t->dos_flags|=WH_DOS_KEEP_ALT_ESC;
        if(pif.keys&0x0010) t->dos_flags|=WH_DOS_KEEP_CTRL_ESC;
    }
    if(!t->title[0]) {
        for(n=lstrlen(path);n>0 && path[n-1]!='\\' && path[n-1]!=':';n--) {}
        for(k=0;path[n+k] && path[n+k]!='.' && k<(int)sizeof(t->title)-1;k++) t->title[k]=path[n+k];
        t->title[k]=0; AnsiUpper(t->title);
    }
    if(has_extension(path,".BAT")) {
        if(wh_env_get("COMSPEC",comspec,sizeof(comspec)) || !comspec[0]) lstrcpy(comspec,"C:\\COMMAND.COM");
        lstrcpyn(t->path,comspec,sizeof(t->path));
        lstrcpy(t->command,"/C "); lstrcat(t->command,path);
        if(tail[0] && lstrlen(t->command)+lstrlen(tail)+2<(int)sizeof(t->command)) {lstrcat(t->command," "); lstrcat(t->command,tail);}
    } else {
        lstrcpyn(t->path,path,sizeof(t->path));
        lstrcpyn(t->command,tail,sizeof(t->command));
    }
    t->used=TRUE;
    t->id=wh_task_create(task_start,t);
    if(!t->id) {t->used=FALSE; return 0;}
    return 33;
}
/* A PIF run: its program (found along the search), with its parameters, or
 * the command line's when it has none or asks for them with "?". */
static UINT pif_program(LPCSTR path,LPCSTR tail) {
    Pif pif; OFSTRUCT of;
    if(!read_pif(path,&pif) || !pif.program[0]) return 2;
    if(OpenFile(pif.program,&of,OF_EXIST)==HFILE_ERROR) return 2;
    return dos_program(of.szPathName,pif.params[0] && lstrcmp(pif.params,"?")?pif.params:tail,&pif);
}
UINT WINAPI WinExec(LPCSTR command,UINT show) {
    char path[128]; unsigned n=0; TDB *t=NULL; unsigned i; OFSTRUCT of; BOOL win16=FALSE;
    if(!command) return 2;
    while(*command==' ') command++;
    while(*command && *command!=' ' && n+1<sizeof(path)) path[n++]=*command++;
    path[n]=0;
    while(*command==' ') command++;
    if(!n) return 2;
    {
        /* Found along the usual search, a program that is not for Windows runs under DOS. */
        char name[132]; LPCSTR base=path; unsigned k; BOOL dot=FALSE;
        for(k=0;path[k];k++) if(path[k]=='\\' || path[k]==':') base=path+k+1;
        for(k=0;base[k];k++) if(base[k]=='.') dot=TRUE;
        lstrcpy(name,path);
        if(!dot) lstrcat(name,".EXE");
        if(!lstrcmpi(base,"COMMAND.COM") && OpenFile(name,&of,OF_EXIST)==HFILE_ERROR &&
           !wh_env_get("COMSPEC",of.szPathName,sizeof(of.szPathName)) && of.szPathName[0]) return dos_program(of.szPathName,command,NULL);
        if(OpenFile(name,&of,OF_EXIST)!=HFILE_ERROR) {
            if(has_extension(of.szPathName,".PIF")) return pif_program(of.szPathName,command);
            win16=ne_program(of.szPathName);
            if(!win16 && (has_extension(of.szPathName,".BAT") || subsystem(of.szPathName)!=2)) return dos_program(of.szPathName,command,NULL);
            lstrcpyn(path,of.szPathName,sizeof(path));
        }
    }
    for(i=0;i<TASKS && !t;i++) if(!tdbs[i].used) t=&tdbs[i];
    if(!t) return 0;
    memset(t,0,sizeof(*t));
    if(win16) {t->win16=TRUE; lstrcpyn(t->path,path,sizeof(t->path));}
    else {
        t->instance=(HINSTANCE)wh_load_application(path);
        if(!t->instance) return 2;
    }
    t->used=TRUE; t->show=(int)show;
    lstrcpy(t->command,lstrlen(command)<(int)sizeof(t->command)?command:"");
    t->id=wh_task_create(task_start,t);
    if(!t->id) {t->used=FALSE; if(t->instance) wh_free_library(t->instance); return 0;}
    return 33;
}
BOOL WINAPI InitTask(LPWINSTARTINFO info) {
    TDB *t=current_tdb();
    if(!t || !info) return FALSE;
    info->hInstance=t->instance; info->lpCmdLine=t->command; info->nCmdShow=t->show;
    return TRUE;
}
void WINAPI ExitProcess(UINT code) {
    TDB *t=current_tdb();
    if(t) end_task(t);
    wh_task_exit((int)code);
}
/* End Windows: every other task first (as if it had ended), then this one. */
void WINAPI ExitKernel(int code) {
    TDB *me=current_tdb(); unsigned i;
    for(i=0;i<TASKS;i++) if(tdbs[i].used && &tdbs[i]!=me) {
        wh_u32 id=tdbs[i].id;
        end_task(&tdbs[i]);
        wh_task_kill(id);
    }
    ExitProcess((UINT)code);
}
DWORD WINAPI GetTaskId(HTASK task) {
    TDB *t=(TDB *)task; unsigned i;
    for(i=0;i<TASKS;i++) if(&tdbs[i]==t && t->used) return t->id;
    return 0;
}
UINT WINAPI GetNumTasks(void) {unsigned i,n=0; for(i=0;i<TASKS;i++) n+=tdbs[i].used; return n;}
BOOL WINAPI IsTask(HTASK task) {return GetTaskId(task)!=0;}
int WINAPI GetModuleUsage(HINSTANCE h) {unsigned i; int n=0; for(i=0;i<TASKS;i++) if(tdbs[i].used && tdbs[i].instance==h) n++; return n;}
FARPROC WINAPI MakeProcInstance(FARPROC proc,HINSTANCE h) {(void)h; return proc;}
void WINAPI FreeProcInstance(FARPROC proc) {(void)proc;}
UINT WINAPI SetErrorMode(UINT mode) {static UINT current; UINT old=current; current=mode; return old;}
void WINAPI FatalAppExit(UINT action,LPCSTR message) {
    (void)action;
    OutputDebugString(message?message:"FatalAppExit");
    ExitProcess(1);
}
void WINAPI DebugBreak(void) {OutputDebugString("DebugBreak");}
DWORD WINAPI GetFreeSpace(UINT flags) {(void)flags; return 16UL*1024*1024;}
/* WIN.COM's environment as NAME=value strings ending with an empty one. */
LPSTR WINAPI GetDOSEnvironment(void) {
    static char block[4096]; unsigned n=0,i;
    if(block[0]) return block;
    for(i=0;n+2<sizeof(block);i++) {
        char entry[512]; unsigned len;
        if(wh_env_list(i,entry,sizeof(entry))) break;
        len=(unsigned)lstrlen(entry);
        if(n+len+2>=sizeof(block)) break;
        memcpy(block+n,entry,len+1); n+=len+1;
    }
    block[n]=0;
    return block;
}
void WINAPI FatalExit(int code) {
    char text[24]="FatalExit ",digits[12]; int n=0,i=10; unsigned v=(unsigned)code;
    if(code<0) {text[i++]='-'; v=0u-v;}
    do digits[n++]=(char)('0'+v%10); while(v/=10);
    while(n) text[i++]=digits[--n];
    text[i]=0; wh_trace(text);
    ExitProcess((UINT)code);
}
TASKSIGNALPROC WINAPI SetTaskSignalProc(HTASK task,TASKSIGNALPROC proc) {
    TASKSIGNALPROC old=signal_proc; (void)task; signal_proc=proc; return old;
}
HTASK WINAPI GetCurrentTask(void) {return (HTASK)current_tdb();}
void WINAPI Yield(void) {wh_yield();}
void WINAPI OutputDebugString(LPCSTR text) {
    char line[256]; int n=0;
    if(!text) return;
    while(text[n] && n<(int)sizeof(line)-1) {line[n]=text[n]; n++;}
    while(n && (line[n-1]=='\n' || line[n-1]=='\r')) n--;
    line[n]=0; wh_trace(line);
}
/* Windows 3.0 on DOS 4.00. */
DWORD WINAPI GetVersion(void) {return 0x04000003UL;}
DWORD WINAPI GetWinFlags(void) {return WF_PMODE|WF_ENHANCED|WF_80x87;}

/* --- modules and resources ----------------------------------------------- */
static HINSTANCE instance_or_current(HINSTANCE h) {
    TDB *t;
    if(h) return h;
    t=current_tdb(); return t?t->instance:NULL;
}
static const char *base_of(const char *path) {
    const char *b=path;
    for(;*path;path++) if(*path=='\\' || *path==':' || *path=='/') b=path+1;
    return b;
}
/* Module names compare without extension: "HELLOWIN" names HELLOWIN.EXE. */
static BOOL same_module(const char *file,const char *name) {
    const char *a=base_of(file),*b=base_of(name);
    for(;*a && *a!='.' && *b && *b!='.';a++,b++) if((*a|0x20)!=(*b|0x20)) return FALSE;
    return (!*a || *a=='.') && (!*b || *b=='.');
}
HMODULE WINAPI GetModuleHandle(LPCSTR name) {
    void *m; unsigned i; char file[128];
    if(!name) return instance_or_current(NULL);
    if(IS_INTRESOURCE(name)) return NULL;
    m=wh_module_handle(name);
    if(m) return (HMODULE)m;
    for(i=0;i<TASKS;i++) if(tdbs[i].used && !wh_module_file_name(tdbs[i].instance,file,sizeof(file)) && same_module(file,name))
        return tdbs[i].instance;
    return NULL;
}
int WINAPI GetModuleFileName(HINSTANCE h,LPSTR buffer,int size) {
    if(!buffer || size<=0 || wh_module_file_name(instance_or_current(h),buffer,(wh_u32)size)) return 0;
    return lstrlen(buffer);
}
HINSTANCE WINAPI LoadLibrary(LPCSTR name) {return name?(HINSTANCE)wh_load_library(name):NULL;}
void WINAPI FreeLibrary(HINSTANCE h) {wh_free_library(h);}
FARPROC WINAPI GetProcAddress(HINSTANCE h,LPCSTR name) {
    return (FARPROC)wh_proc_address(instance_or_current(h),name);
}
/* HRSRC is the image's IMAGE_RESOURCE_DATA_ENTRY, as in Win32. */
HRSRC WINAPI FindResource(HINSTANCE h,LPCSTR name,LPCSTR type) {
    return (HRSRC)wh_find_resource(instance_or_current(h),type,name);
}
HGLOBAL WINAPI LoadResource(HINSTANCE h,HRSRC resource) {
    h=instance_or_current(h);
    if(!h || !resource) return NULL;
    return (HGLOBAL)((BYTE *)h+((const DWORD *)resource)[0]);
}
void FAR *WINAPI LockResource(HGLOBAL h) {return h;}
BOOL WINAPI FreeResource(HGLOBAL h) {(void)h; return FALSE;}
DWORD WINAPI SizeofResource(HINSTANCE h,HRSRC resource) {(void)h; return resource?((const DWORD *)resource)[1]:0;}

/* --- strings ------------------------------------------------------------- */
/* a*b/c rounded half away from zero; -1 for a zero divisor or an overflow. */
int WINAPI MulDiv(int a,int b,int c) {
    LONGLONG n=(LONGLONG)a*b,d=c,q;
    if(!c) return -1;
    if(d<0) {n=-n; d=-d;}
    q=n>=0?(n+d/2)/d:-((-n+d/2)/d);
    return q>0x7fffffff || q<-0x7fffffff-1?-1:(int)q;
}
int WINAPI lstrlen(LPCSTR s) {return s?(int)strlen(s):0;}
LPSTR WINAPI lstrcpy(LPSTR d,LPCSTR s) {return d && s?strcpy(d,s):NULL;}
LPSTR WINAPI lstrcat(LPSTR d,LPCSTR s) {return d && s?strcat(d,s):NULL;}
LPSTR WINAPI lstrcpyn(LPSTR d,LPCSTR s,int n) {
    int i;
    if(!d || n<=0) return d;
    for(i=0;i<n-1 && s && s[i];i++) d[i]=s[i];
    d[i]=0;
    return d;
}
int WINAPI lstrcmp(LPCSTR a,LPCSTR b) {return strcmp(a?a:"",b?b:"");}
static int lower(int c) {
    c&=0xff;
    return (c>='A' && c<='Z') || (c>=0xc0 && c<=0xde && c!=0xd7)?c+0x20:c;
}
int WINAPI lstrcmpi(LPCSTR a,LPCSTR b) {
    if(!a) a="";
    if(!b) b="";
    for(;*a && lower(*a)==lower(*b);a++,b++) {}
    return lower(*a)-lower(*b);
}

int WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if(reason==1) {MemoryInit(); memset(tdbs,0,sizeof(tdbs)); signal_proc=NULL; FileInit(instance);}
    else if(reason==0) MemoryShutdown();
    return TRUE;
}

/* --- atoms ------------------------------------------------------------------- */
/* Integer atoms are below 0xC000 ("#n" or MAKEINTATOM); string atoms are
 * 0xC000 plus an index, in each task's own table (AddAtom) or the one all
 * tasks share (GlobalAddAtom, which WOW's programs use too), counted and
 * compared without case. A task's atoms go with it. */
#define ATOMS 512
static struct {BOOL used; WORD count; TDB *task; char name[256];} atoms[ATOMS];
static WORD integer_atom(LPCSTR s) {
    UINT n=0;
    if(!HIWORD((ULONG_PTR)s)) return LOWORD((ULONG_PTR)s);
    if(s[0]!='#') return 0;
    for(s++;*s>='0' && *s<='9';s++) n=n*10+(UINT)(*s-'0');
    return (WORD)(n && n<0xc000 && !*s?n:0);
}
static ATOM find_atom(TDB *t,LPCSTR name) {
    WORD i=integer_atom(name);
    if(i || !name) return i;
    for(i=0;i<ATOMS;i++) if(atoms[i].used && atoms[i].task==t && !lstrcmpi(atoms[i].name,name)) return (ATOM)(0xc000+i);
    return 0;
}
static ATOM add_atom(TDB *t,LPCSTR name) {
    ATOM a=find_atom(t,name); WORD i;
    if(a>=0xc000) {atoms[a-0xc000].count++; return a;}
    if(a || !name || !*name || lstrlen(name)>=(int)sizeof(atoms[0].name)) return a;
    for(i=0;i<ATOMS && atoms[i].used;i++) {}
    if(i==ATOMS) return 0;
    atoms[i].used=TRUE; atoms[i].count=1; atoms[i].task=t; lstrcpy(atoms[i].name,name);
    return (ATOM)(0xc000+i);
}
static ATOM delete_atom(TDB *t,ATOM a) {
    if(a<0xc000) return 0;
    if(a-0xc000>=ATOMS || !atoms[a-0xc000].used || atoms[a-0xc000].task!=t) return a;
    if(!--atoms[a-0xc000].count) atoms[a-0xc000].used=FALSE;
    return 0;
}
static UINT atom_name(TDB *t,ATOM a,LPSTR out,int size) {
    char text[16];
    if(!out || size<=0) return 0;
    if(a<0xc000) {if(!a) return 0; wsprintf(text,"#%u",(UINT)a); lstrcpyn(out,text,size); return (UINT)lstrlen(out);}
    if(a-0xc000>=ATOMS || !atoms[a-0xc000].used || atoms[a-0xc000].task!=t) return 0;
    lstrcpyn(out,atoms[a-0xc000].name,size);
    return (UINT)lstrlen(out);
}
void AtomsTaskEnded(void *task) {
    unsigned i;
    for(i=0;i<ATOMS;i++) if(atoms[i].used && atoms[i].task==(TDB *)task) atoms[i].used=FALSE;
}
BOOL WINAPI InitAtomTable(int size) {(void)size; return TRUE;}
ATOM WINAPI AddAtom(LPCSTR name) {return add_atom(current_tdb(),name);}
ATOM WINAPI FindAtom(LPCSTR name) {return find_atom(current_tdb(),name);}
ATOM WINAPI DeleteAtom(ATOM a) {return delete_atom(current_tdb(),a);}
UINT WINAPI GetAtomName(ATOM a,LPSTR out,int size) {return atom_name(current_tdb(),a,out,size);}
ATOM WINAPI GlobalAddAtom(LPCSTR name) {return add_atom(NULL,name);}
ATOM WINAPI GlobalFindAtom(LPCSTR name) {return find_atom(NULL,name);}
ATOM WINAPI GlobalDeleteAtom(ATOM a) {return delete_atom(NULL,a);}
UINT WINAPI GlobalGetAtomName(ATOM a,LPSTR out,int size) {return atom_name(NULL,a,out,size);}
