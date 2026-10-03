/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: the smaller system modules Win16 programs import, as far as
 * this system has their devices.
 *   SHELL     the registration database (kept for the session), ShellExecute
 *             and FindExecutable through WIN.INI's [Extensions], ShellAbout;
 *             dropped files never arrive
 *   MMSYSTEM  timeGetTime and timeSetEvent (run when the task takes its
 *             messages); there are no wave, MIDI, joystick or MCI devices
 *   LZEXPAND  files COMPRESS made, expanded whole; others as they are
 *   VER       no version resources
 *   WIN87EM   the coprocessor is always there
 *   KEYBOARD, SOUND (no voices), SYSTEM, TOOLHELP (timers, heap info)
 */
#include "api.h"

/* --- SHELL: the registration database ---------------------------------------- */
/* Keys are paths under HKEY_CLASSES_ROOT (1); a handle is an entry's
 * index plus 0x100. ERROR_SUCCESS 0, ERROR_BADKEY 2, ERROR_OUTOFMEMORY 6. */
#define KEYS 256
static struct {BOOL used; char path[128],value[128];} keys[KEYS];
static BOOL key_path(DWORD key,LPCSTR sub,char *out) {
    out[0]=0;
    if(key>=0x100 && key-0x100<KEYS && keys[key-0x100].used) lstrcpy(out,keys[key-0x100].path);
    else if(key!=1) return FALSE;
    if(sub && *sub) {
        if(lstrlen(out)+lstrlen(sub)+2>128) return FALSE;
        if(out[0]) lstrcat(out,"\\");
        lstrcat(out,sub);
    }
    return TRUE;
}
static int find_key(LPCSTR path) {
    int i;
    for(i=0;i<KEYS;i++) if(keys[i].used && !lstrcmpi(keys[i].path,path)) return i;
    return -1;
}
static int make_key(LPCSTR path) {
    int i=find_key(path); char parent[128]; int n;
    if(i>=0 || !path[0]) return i;
    lstrcpy(parent,path);
    for(n=lstrlen(parent);n>0 && parent[n-1]!='\\';n--) {}
    if(n>1) {parent[n-1]=0; if(make_key(parent)<0) return -1;}
    for(i=0;i<KEYS && keys[i].used;i++) {}
    if(i==KEYS) return -1;
    keys[i].used=TRUE; lstrcpy(keys[i].path,path); keys[i].value[0]=0;
    return i;
}
static DWORD open_key(Args16 *a,BOOL create) {
    char path[128]; int i; BYTE *out=(BYTE *)PTR(a->a[2]);
    if(!out || !key_path(a->a[0],(LPCSTR)PTR(a->a[1]),path)) return 2;
    if(!path[0]) {put32(out,1); return 0;}
    i=create?make_key(path):find_key(path);
    if(i<0) return create?6:2;
    put32(out,(DWORD)i+0x100);
    return 0;
}
DWORD W16_RegOpenKey(Args16 *a) {return open_key(a,FALSE);}
DWORD W16_RegCreateKey(Args16 *a) {return open_key(a,TRUE);}
DWORD W16_RegCloseKey(Args16 *a) {(void)a; return 0;}
DWORD W16_RegDeleteKey(Args16 *a) {
    char path[128]; int i,n;
    if(!key_path(a->a[0],(LPCSTR)PTR(a->a[1]),path) || !path[0] || find_key(path)<0) return 2;
    n=lstrlen(path);
    for(i=0;i<KEYS;i++) if(keys[i].used && !lstrcmpi(keys[i].path,path)) keys[i].used=FALSE;
    for(i=0;i<KEYS;i++) if(keys[i].used && lstrlen(keys[i].path)>n && keys[i].path[n]=='\\') {
        char head[128]; lstrcpyn(head,keys[i].path,n+1);
        if(!lstrcmpi(head,path)) keys[i].used=FALSE;
    }
    return 0;
}
/* RegSetValue(key, subkey, REG_SZ, data, size) */
DWORD W16_RegSetValue(Args16 *a) {
    char path[128]; int i; LPCSTR data=(LPCSTR)PTR(a->a[3]);
    if(a->a[2]!=1 || !data || !key_path(a->a[0],(LPCSTR)PTR(a->a[1]),path) || !path[0]) return 2;
    if((i=make_key(path))<0) return 6;
    lstrcpyn(keys[i].value,data,sizeof(keys[i].value));
    return 0;
}
/* RegQueryValue(key, subkey, buffer, size in and out) */
DWORD W16_RegQueryValue(Args16 *a) {
    char path[128]; int i; char *out=(char *)PTR(a->a[2]); BYTE *size=(BYTE *)PTR(a->a[3]); DWORD room;
    if(!key_path(a->a[0],(LPCSTR)PTR(a->a[1]),path) || (i=find_key(path))<0) return 2;
    room=size?get32(size):0;
    if(out && room) lstrcpyn(out,keys[i].value,(int)room);
    if(size) put32(size,(DWORD)lstrlen(keys[i].value)+1);
    return 0;
}
/* RegEnumKey(key, index, buffer, size): the index-th subkey's name. */
DWORD W16_RegEnumKey(Args16 *a) {
    char path[128]; int i,n; DWORD index=a->a[1]; char *out=(char *)PTR(a->a[2]);
    if(!key_path(a->a[0],NULL,path)) return 2;
    n=lstrlen(path);
    for(i=0;i<KEYS;i++) {
        LPCSTR rest; int k;
        if(!keys[i].used) continue;
        if(n) {
            char head[128];
            if(lstrlen(keys[i].path)<=n || keys[i].path[n]!='\\') continue;
            lstrcpyn(head,keys[i].path,n+1);
            if(lstrcmpi(head,path)) continue;
            rest=keys[i].path+n+1;
        } else rest=keys[i].path;
        for(k=0;rest[k] && rest[k]!='\\';k++) {}
        if(rest[k]) continue;
        if(!index--) {if(out) lstrcpyn(out,rest,(int)a->a[3]); return 0;}
    }
    return 259; /* ERROR_NO_MORE_ITEMS */
}

/* --- SHELL: programs and files --------------------------------------------------- */
static BOOL program(LPCSTR file) {
    int n=lstrlen(file);
    return n>4 && (!lstrcmpi(file+n-4,".EXE") || !lstrcmpi(file+n-4,".COM") || !lstrcmpi(file+n-4,".BAT") || !lstrcmpi(file+n-4,".PIF"));
}
/* The program for a document: WIN.INI's [Extensions] "txt=notepad.exe ^.txt". */
static BOOL association(LPCSTR file,char *program_out,int size) {
    LPCSTR dot=NULL,p; char line[128]; int i;
    for(p=file;*p;p++) {if(*p=='.') dot=p; else if(*p=='\\') dot=NULL;}
    if(!dot || !GetProfileString("Extensions",dot+1,"",line,sizeof(line)) || !line[0]) return FALSE;
    for(i=0;line[i] && line[i]!=' ' && i<size-1;i++) program_out[i]=line[i];
    program_out[i]=0;
    return TRUE;
}
/* ShellExecute(hwnd, operation, file, parameters, directory, show) */
DWORD W16_ShellExecute(Args16 *a) {
    LPCSTR file=(LPCSTR)PTR(a->a[2]),params=(LPCSTR)PTR(a->a[3]),dir=(LPCSTR)PTR(a->a[4]); char line[260],prog[80]; UINT r;
    if(!file) return 2;
    if(dir && *dir) SetCurrentDirectory(dir);
    if(program(file)) {lstrcpyn(line,file,sizeof(line)); if(params && *params && lstrlen(line)+lstrlen(params)+2<(int)sizeof(line)) {lstrcat(line," "); lstrcat(line,params);}}
    else if(association(file,prog,sizeof(prog)) && lstrlen(prog)+lstrlen(file)+2<(int)sizeof(line)) wsprintf(line,"%s %s",(LPSTR)prog,(LPSTR)file);
    else return 31; /* SE_ERR_NOASSOC */
    r=WinExec(line,(UINT)(short)a->a[5]);
    return r;
}
DWORD W16_FindExecutable(Args16 *a) {
    LPCSTR file=(LPCSTR)PTR(a->a[0]); char *out=(char *)PTR(a->a[2]);
    if(!file || !out) return 2;
    out[0]=0;
    if(program(file)) {lstrcpyn(out,file,128); return 33;}
    return association(file,out,128)?33:31;
}
DWORD W16_ShellAbout(Args16 *a) {
    LPCSTR app=(LPCSTR)PTR(a->a[1]),other=(LPCSTR)PTR(a->a[2]); char title[96],text[300];
    wsprintf(title,"About %s",(LPSTR)(app?app:"Interface Manager"));
    wsprintf(text,"%s\n\nInterface Manager 3.0\nfor IA-64\n\n%s",(LPSTR)(app?app:"Interface Manager"),(LPSTR)(other?other:""));
    MessageBox(HWND32(a->a[0]),text,title,MB_OK|MB_ICONINFORMATION);
    return 1;
}
DWORD W16_DragAcceptFiles(Args16 *a) {(void)a; return 0;}
DWORD W16_DragQueryFile(Args16 *a) {char *out=(char *)PTR(a->a[2]); if(out && a->a[3]) out[0]=0; return 0;}
DWORD W16_DragFinish(Args16 *a) {(void)a; return 0;}
DWORD W16_DragQueryPoint(Args16 *a) {(void)a; return 0;}
DWORD W16_ExtractIcon(Args16 *a) {(void)a; return 0;}
DWORD W16_DoEnvironmentSubst(Args16 *a) {LPCSTR s=(LPCSTR)PTR(a->a[0]); return MAKELONG(s?lstrlen(s):0,1);}

/* --- MMSYSTEM ------------------------------------------------------------------- */
DWORD W16_mmsystemGetVersion(Args16 *a) {(void)a; return 0x0101;}
DWORD W16_sndPlaySound(Args16 *a) {(void)a; MessageBeep(0); return 1;}
DWORD W16_NoDevices(Args16 *a) {(void)a; return 0;}
#define MMSYSERR_NODRIVER 6
DWORD W16_NoDriver(Args16 *a) {(void)a; return MMSYSERR_NODRIVER;}
DWORD W16_timeGetTime(Args16 *a) {(void)a; return GetTickCount();}
/* MMTIME: type (TIME_MS 1), then the milliseconds. */
DWORD W16_timeGetSystemTime(Args16 *a) {
    BYTE *t=(BYTE *)PTR(a->a[0]);
    if(t && a->a[1]>=6) {put16(t,1); put32(t+2,GetTickCount());}
    return 0;
}
/* TIMECAPS: minimum and maximum period. */
DWORD W16_timeGetDevCaps(Args16 *a) {
    BYTE *c=(BYTE *)PTR(a->a[0]);
    if(!c || a->a[1]<4) return 97; /* TIMERR_STRUCT */
    put16(c,10); put16(c+2,65535);
    return 0;
}
DWORD W16_timeBeginPeriod(Args16 *a) {(void)a; return 0;}
/* timeSetEvent(delay, resolution, procedure, user data, flags): the
 * procedure (id, message, user, 0, 0) runs from the task's messages, on a
 * window-less native timer; TIME_PERIODIC (1) repeats. */
#define MM_TIMERS 16
static struct {UINT_PTR id; DWORD proc,user; BOOL periodic; Task16 *task;} mm_timers[MM_TIMERS];
static void CALLBACK mm_timer(HWND h,UINT msg,UINT_PTR id,DWORD time) {
    static const BYTE sizes[5]={2,2,4,4,4}; unsigned i; DWORD args[5];
    (void)h; (void)msg; (void)time;
    for(i=0;i<MM_TIMERS;i++) if(mm_timers[i].id==id && mm_timers[i].proc) {
        DWORD proc=mm_timers[i].proc; Task16 *t=mm_timers[i].task;
        if(!mm_timers[i].periodic) {KillTimer(NULL,id); mm_timers[i].proc=0;}
        args[0]=i+1; args[1]=0; args[2]=mm_timers[i].user; args[3]=0; args[4]=0;
        if(t==CurrentTask16()) Call16(t,proc,5,args,sizes);
        return;
    }
}
DWORD W16_timeSetEvent(Args16 *a) {
    unsigned i; UINT_PTR id;
    for(i=0;i<MM_TIMERS && mm_timers[i].proc;i++) {}
    if(i==MM_TIMERS || !a->a[2]) return 0;
    if(!(id=SetTimer(NULL,0,(UINT)(a->a[0]?a->a[0]:1),mm_timer))) return 0;
    mm_timers[i].id=id; mm_timers[i].proc=a->a[2]; mm_timers[i].user=a->a[3]; mm_timers[i].periodic=(a->a[4]&1)!=0;
    mm_timers[i].task=a->task;
    return i+1;
}
DWORD W16_timeKillEvent(Args16 *a) {
    unsigned i=(unsigned)a->a[0]-1;
    if(i>=MM_TIMERS || !mm_timers[i].proc) return 97;
    KillTimer(NULL,mm_timers[i].id); mm_timers[i].proc=0;
    return 0;
}
void System16TaskEnded(Task16 *t) {
    unsigned i;
    for(i=0;i<MM_TIMERS;i++) if(mm_timers[i].proc && mm_timers[i].task==t) {KillTimer(NULL,mm_timers[i].id); mm_timers[i].proc=0;}
}
#define MCIERR_DEVICE_NOT_INSTALLED 0x0122
DWORD W16_mciSendCommand(Args16 *a) {(void)a; return MCIERR_DEVICE_NOT_INSTALLED;}
DWORD W16_mciSendString(Args16 *a) {char *out=(char *)PTR(a->a[1]); if(out && a->a[2]) out[0]=0; return MCIERR_DEVICE_NOT_INSTALLED;}
DWORD W16_mciGetErrorString(Args16 *a) {
    char *out=(char *)PTR(a->a[1]);
    if(!out || !a->a[2]) return 0;
    lstrcpyn(out,"There is no multimedia device.",(int)a->a[2]);
    return 1;
}

/* --- LZEXPAND ------------------------------------------------------------------- */
/* Files that COMPRESS made (SZDD: the signature, 'A', the name's last
 * character, the expanded size, then LZSS: a 4096-byte window of spaces
 * written from 4080, flag bytes read from bit 0, a set bit a literal byte,
 * a clear one two bytes with a 12-bit window position and a length of 3 to
 * 18) are expanded whole by LZInit; their LZ handles (0x400 and up) read and
 * seek in the expanded data. Other files stay DOS handles. LZOpenFile also
 * tries the compressed name (the last character an underscore). */
#define LZ_HANDLES 16
#define LZ_BIAS 0x400
#define LZERROR_BADINHANDLE (-1)
#define LZERROR_READ (-3)
#define LZERROR_WRITE (-4)
#define LZERROR_GLOBALLOC (-5)
#define LZERROR_BADVALUE (-7)
static struct {HFILE file; HGLOBAL data; BYTE *bytes; DWORD size,pos;} lz[LZ_HANDLES];
static int lz_slot(DWORD h) {h=(WORD)h; return h>=LZ_BIAS && h<LZ_BIAS+LZ_HANDLES && lz[h-LZ_BIAS].bytes?(int)(h-LZ_BIAS):-1;}
/* A compressed file's expanded size and its name's last character; the
 * file is left after the header, or where it was when it is not one. */
static BOOL szdd_header(HFILE f,DWORD *size,char *last) {
    static const BYTE magic[8]={'S','Z','D','D',0x88,0xf0,0x27,0x33}; BYTE h[14]; LONG at=_llseek(f,0,1);
    _llseek(f,0,0);
    if(_lread(f,h,14)!=14 || memcmp(h,magic,8) || h[8]!='A') {_llseek(f,at,0); return FALSE;}
    if(last) *last=(char)h[9];
    *size=get32(h+10);
    return TRUE;
}
typedef struct {HFILE f; BYTE buffer[4096]; UINT avail,at;} LzReader;
static int lz_byte(LzReader *r) {
    if(r->at==r->avail) {
        UINT n=_lread(r->f,r->buffer,sizeof(r->buffer));
        if(!n || n==(UINT)HFILE_ERROR) return -1;
        r->avail=n; r->at=0;
    }
    return r->buffer[r->at++];
}
static DWORD lz_expand(HFILE f,BYTE *out,DWORD size) {
    static BYTE window[4096]; static LzReader r; unsigned pos=4096-16; DWORD done=0; int flags=0,count=0,c;
    memset(window,' ',sizeof(window)); r.f=f; r.avail=r.at=0;
    while(done<size) {
        if(!count) {if((flags=lz_byte(&r))<0) break; count=8;}
        count--;
        if(flags&1) {
            if((c=lz_byte(&r))<0) break;
            out[done++]=window[pos]=(BYTE)c; pos=(pos+1)&4095;
        } else {
            int lo=lz_byte(&r),hi=lz_byte(&r),from,len,i;
            if(lo<0 || hi<0) break;
            from=lo|(hi&0xf0)<<4; len=(hi&0x0f)+3;
            for(i=0;i<len && done<size;i++) {BYTE b=window[(from+i)&4095]; out[done++]=window[pos]=b; pos=(pos+1)&4095;}
        }
        flags>>=1;
    }
    return done;
}
static DWORD lz_init(HFILE f) {
    DWORD size; int i; HGLOBAL data; BYTE *bytes;
    if((short)f<0) return (DWORD)(int)LZERROR_BADINHANDLE;
    if(!szdd_header(f,&size,NULL)) return (WORD)f;
    for(i=0;i<LZ_HANDLES && lz[i].bytes;i++) {}
    if(i==LZ_HANDLES || !(data=GlobalAlloc(GMEM_FIXED,size?size:1)) || !(bytes=(BYTE *)GlobalLock(data))) return (DWORD)(int)LZERROR_GLOBALLOC;
    if(lz_expand(f,bytes,size)!=size) {GlobalFree(data); return (DWORD)(int)LZERROR_READ;}
    lz[i].file=f; lz[i].data=data; lz[i].bytes=bytes; lz[i].size=size; lz[i].pos=0;
    return LZ_BIAS+i;
}
/* LZOpenFile(name, OFSTRUCT, style): OpenFile's, LZInit's for reading. */
DWORD W16_LZOpenFile(Args16 *a) {
    OFSTRUCT of; BYTE *o=(BYTE *)PTR(a->a[1]); LPCSTR name=(LPCSTR)PTR(a->a[0]); UINT style=(UINT)a->a[2];
    HFILE h=OpenFile(name,&of,style);
    if(h==HFILE_ERROR && name && !(style&(OF_CREATE|OF_DELETE))) {
        char compressed[144]; int n; LPCSTR dot;
        lstrcpyn(compressed,name,sizeof(compressed)-1); n=lstrlen(compressed);
        for(dot=compressed+n;dot>compressed && *dot!='.' && *dot!='\\';dot--) {}
        if(*dot=='.' && compressed+n-dot==4) compressed[n-1]='_'; else {compressed[n]='_'; compressed[n+1]=0;}
        h=OpenFile(compressed,&of,style);
    }
    if(o) {memset(o,0,136); o[0]=136; o[1]=of.fFixedDisk; put16(o+2,of.nErrCode); lstrcpyn((char *)o+8,of.szPathName,128);}
    if(h==HFILE_ERROR) return (WORD)(short)LZERROR_BADINHANDLE;
    if((style&3)==OF_READ && !(style&(OF_EXIST|OF_PARSE))) return (WORD)lz_init(h);
    return (WORD)h;
}
DWORD W16_LZInit(Args16 *a) {return (WORD)lz_init((HFILE)(short)a->a[0]);}
DWORD W16_LZStart(Args16 *a) {(void)a; return 1;}
DWORD W16_LZDone(Args16 *a) {(void)a; return 0;}
DWORD W16_LZRead(Args16 *a) {
    int i=lz_slot(a->a[0]); BYTE *out=(BYTE *)PTR(a->a[1]); DWORD n=(WORD)a->a[2];
    if(i<0) return (WORD)_lread((HFILE)(short)a->a[0],out,(UINT)n);
    if(n>lz[i].size-lz[i].pos) n=lz[i].size-lz[i].pos;
    if(out) memcpy(out,lz[i].bytes+lz[i].pos,n);
    lz[i].pos+=n;
    return n;
}
DWORD W16_LZSeek(Args16 *a) {
    int i=lz_slot(a->a[0]); LONG to=(LONG)a->a[1];
    if(i<0) return (DWORD)_llseek((HFILE)(short)a->a[0],to,(int)a->a[2]);
    if(a->a[2]==1) to+=(LONG)lz[i].pos; else if(a->a[2]==2) to+=(LONG)lz[i].size;
    if(to<0 || (DWORD)to>lz[i].size) return (DWORD)LZERROR_BADVALUE;
    lz[i].pos=(DWORD)to;
    return (DWORD)to;
}
DWORD W16_LZClose(Args16 *a) {
    int i=lz_slot(a->a[0]);
    if(i<0) {_lclose((HFILE)(short)a->a[0]); return 0;}
    _lclose(lz[i].file); GlobalFree(lz[i].data); memset(&lz[i],0,sizeof(lz[i]));
    return 0;
}
/* LZCopy(source, destination): the bytes written, expanded when the source is compressed. */
DWORD W16_LZCopy(Args16 *a) {
    static char buffer[4096]; DWORD from=a->a[0],total=0,h; HFILE to=(HFILE)(short)a->a[1]; UINT n; int i;
    if(lz_slot(from)<0 && (h=lz_init((HFILE)(short)from))!=(WORD)from) {
        if((LONG)(int)h<0) return h;
        from=h;
    }
    if((i=lz_slot(from))>=0) {
        DWORD left=lz[i].size-lz[i].pos;
        while(left) {
            n=(UINT)(left<sizeof(buffer)?left:sizeof(buffer));
            if(_lwrite(to,(LPCSTR)lz[i].bytes+lz[i].pos,n)!=n) return (DWORD)LZERROR_WRITE;
            lz[i].pos+=n; left-=n; total+=n;
        }
        return total;
    }
    while((n=_lread((HFILE)(short)from,buffer,sizeof(buffer)))!=0 && n!=(UINT)HFILE_ERROR) {
        if(_lwrite(to,buffer,n)!=n) return (DWORD)LZERROR_WRITE;
        total+=n;
    }
    return total;
}
/* The name a compressed file expands to: its last character restored. */
DWORD W16_GetExpandedName(Args16 *a) {
    LPCSTR from=(LPCSTR)PTR(a->a[0]); char *to=(char *)PTR(a->a[1]); OFSTRUCT of; HFILE f; DWORD size; char last; int n;
    if(!from || !to) return (DWORD)LZERROR_BADVALUE;
    lstrcpy(to,from);
    if((f=OpenFile(from,&of,OF_READ))==HFILE_ERROR) return 1;
    if(szdd_header(f,&size,&last) && last && (n=lstrlen(to))>0 && to[n-1]=='_') to[n-1]=last;
    _lclose(f);
    return 1;
}

/* --- VER ------------------------------------------------------------------------ */
DWORD W16_GetFileVersionInfoSize(Args16 *a) {BYTE *h=(BYTE *)PTR(a->a[1]); if(h) put32(h,0); return 0;}
DWORD W16_NoVersion(Args16 *a) {(void)a; return 0;}
DWORD W16_VerLanguageName(Args16 *a) {
    char *out=(char *)PTR(a->a[1]);
    if(!out || !a->a[2]) return 0;
    lstrcpyn(out,"English (United States)",(int)a->a[2]);
    return (DWORD)lstrlen(out);
}

/* --- WIN87EM ---------------------------------------------------------------------- */
/* __fpMath (a register function): BX selects. Install and initialize
 * leave AX 0; the status calls give 0; the rest need nothing. */
DWORD W16_fpMath(Args16 *a) {
    Task16 *t=a->task; WORD op=Reg16(t,BX);
    if(op==0 || op==5 || op==8 || op==10) SetReg16(t,AX,0);
    if(op==7) {SetReg16(t,AX,0); SetReg16(t,DX,0);}
    Return16(t,0);
    return 0;
}
DWORD W16_WinEm87Info(Args16 *a) {(void)a; return 0;}

/* --- KEYBOARD, SOUND, SYSTEM ------------------------------------------------------- */
DWORD W16_AnsiToOem16(Args16 *a) {AnsiToOem((LPCSTR)PTR(a->a[0]),(LPSTR)PTR(a->a[1])); return 1;}
DWORD W16_OemToAnsi16(Args16 *a) {OemToAnsi((LPCSTR)PTR(a->a[0]),(LPSTR)PTR(a->a[1])); return 1;}
DWORD W16_GetKeyboardType(Args16 *a) {return a->a[0]==0?4:a->a[0]==2?12:0;} /* enhanced, 12 function keys */
DWORD W16_GetKBCodePage(Args16 *a) {(void)a; return 437;}
DWORD W16_OpenSound(Args16 *a) {(void)a; return (WORD)S_SERDVNA;}
DWORD W16_NoSound(Args16 *a) {(void)a; return 0;}
DWORD W16_DoBeep(Args16 *a) {(void)a; MessageBeep(0); return 0;}
DWORD W16_GetSystemMSecCount(Args16 *a) {(void)a; return GetTickCount();}
DWORD W16_InquireSystem(Args16 *a) {return a->a[0]==0?MAKELONG(55,1):a->a[0]==1?MAKELONG(3,0):0;} /* timer period, drive count */

/* --- TOOLHELP ---------------------------------------------------------------------- */
/* TIMERINFO: size, milliseconds since Windows started, in this VM. */
DWORD W16_TimerCount(Args16 *a) {
    BYTE *t=(BYTE *)PTR(a->a[0]); DWORD now=GetTickCount();
    if(!t) return 0;
    put32(t+4,now); put32(t+8,now);
    return 1;
}
/* SYSHEAPINFO: size, USER's and GDI's free percentages and segments. */
DWORD W16_SystemHeapInfo(Args16 *a) {
    BYTE *h=(BYTE *)PTR(a->a[0]);
    if(!h) return 0;
    put16(h+4,90); put16(h+6,90); put16(h+8,ThunkModuleHandle("USER")); put16(h+10,ThunkModuleHandle("GDI"));
    return 1;
}
DWORD W16_NoToolHelp(Args16 *a) {(void)a; return 0;}
DWORD W16_NotifyRegister(Args16 *a) {(void)a; return 1;}
