/* SPDX-License-Identifier: GPL-2.0-or-later
 * VDM execution, interrupts and DOS processes.
 *
 * MSDOS.SYS invokes VDM.EXE with the program path and command tail. V86 code
 * runs at CPL 3, IOPL 3: CLI/STI/PUSHF/POPF execute directly; port I/O traps
 * because CFLG.io is clear. Default INT stubs call native services. Hooked
 * vectors receive FLAGS/CS/IP with IF and TF cleared; chaining to a stub
 * returns through emulated IRET with the service's result flags. IRET, HLT,
 * SMSW and MOV SS traps are emulated. Faults reach hooked vectors (0, 1,
 * 3-6; exception 16 maps to INT 75h) or terminate the program.
 *
 * Each run is limited to PIT counter 0's period. Due ticks call hooked
 * INT 8, or INT 1Ch through the default handler, when IF is set and IRQ 0
 * is unmasked and not in service. Every stop updates BIOS time at 40:6Ch.
 *
 * AH=4B00h children share the VDM; parent registers are saved until exit.
 * Each PSP's job file table maps handles to the native task, isolating
 * redirections and closes.
 */
#include "runtime.h"
#include "vdm.h"
IoIa32Context cpu;
const IoServices *io;
u16 cur_psp,return_code;
u32 dta;
unsigned alloc_strategy;
u16 mem_top=0xa000;
int break_pending;
static int finished,pending_exit,child_pending;
static u8 exit_code,exit_kind,final_code;
static u16 exit_keep;
typedef struct {u16 psp,cs,ip,ss,sp;} Image;
static Image child;
typedef struct {u16 psp; u32 dta; IoIa32Context parent;} Process;
static Process procs[MAX_DEPTH];
static unsigned depth;
/* Conventional memory as it was before this machine started: the firmware
 * and another, suspended VDM may have state there. */
static void *saved_low;
#define SAVED_PAGES (0xa0000U/4096U)

/* Virtual-8086 segments: base sel*16, 64 KiB, read/write data, DPL 3. */
u64 real_desc(u16 sel) {return (u64)sel<<4|0xffffULL<<32|3ULL<<52|1ULL<<56|3ULL<<57|1ULL<<59;}
u16 sreg(unsigned s) {unsigned shift; return (u16)(*selector_word(s,&shift)>>shift);}
void set_sreg(unsigned s,u16 v) {
    unsigned shift; u64 *w=selector_word(s,&shift);
    *w=(*w&~(0xffffULL<<shift))|(u64)v<<shift;
    *descriptor_of(s)=real_desc(v);
}
void push16(u16 v) {u16 sp=(u16)(rw(ESP)-2); ww(ESP,sp); poke16(((u32)sreg(SR_SS)<<4)+sp,v);}
u16 pop16(void) {u16 sp=rw(ESP); u16 v=peek16(((u32)sreg(SR_SS)<<4)+sp); ww(ESP,(u16)(sp+2)); return v;}
static u32 pop32(void) {u32 low=pop16(); return low|(u32)pop16()<<16;}
u32 ivt(unsigned n) {return peek32(n*4);}
void set_ivt(unsigned n,u32 v) {poke32(n*4,v);}
u32 stub(unsigned n) {return STUB_SEG<<16|(n*2);}
void jump(u16 cs,u16 ip) {set_sreg(SR_CS,cs); cpu.eip=ip;}

static void enter_interrupt(unsigned n) {
    push16((u16)cpu.eflags); push16(sreg(SR_CS)); push16((u16)cpu.eip);
    cpu.eflags&=~(u64)(FL_IF|FL_TF);
    u32 v=ivt(n); jump((u16)(v>>16),(u16)v);
}
/* Returns from an interrupt; a service stub keeps its result flags. */
static void iret16(int keep_flags) {
    u16 ip=pop16(),cs=pop16(),flags=pop16();
    if(keep_flags) flags=(u16)((flags&~FL_ARITH)|(cpu.eflags&FL_ARITH));
    jump(cs,ip); cpu.eflags=(cpu.eflags&~0xffffULL)|flags|EFLAGS_V86;
}
static void iret32(void) {
    u32 ip=pop32(),cs=pop32(),flags=pop32();
    jump((u16)cs,(u16)ip); cpu.eip=ip; cpu.eflags=(flags&0x3f7fd7U)|EFLAGS_V86;
}

/* Memory control blocks: 'M' or 'Z', owner PSP (0 free, 8 DOS), size in
 * paragraphs, program name at 8. */
static u32 mcb(u16 s) {return (u32)s<<4;}
static u8 mtype(u16 s) {return *LINEAR(mcb(s));}
static u16 mowner(u16 s) {return peek16(mcb(s)+1);}
static u16 msize(u16 s) {return peek16(mcb(s)+3);}
static void set_mcb(u16 s,u8 type,u16 owner,u16 size) {*LINEAR(mcb(s))=type; poke16(mcb(s)+1,owner); poke16(mcb(s)+3,size);}
static int arena_ok(u16 s) {return mtype(s)=='M' || mtype(s)=='Z';}
static void merge_free(void) {
    for(u16 s=FIRST_MCB;arena_ok(s) && mtype(s)=='M';) {
        u16 n=(u16)(s+msize(s)+1);
        if(!mowner(s) && !mowner(n) && arena_ok(n)) {set_mcb(s,mtype(n),0,(u16)(msize(s)+msize(n)+1)); continue;}
        s=n;
    }
}
int mem_alloc(u16 paras,u16 owner,u16 *seg,u16 *largest) {
    merge_free();
    u16 pick=0,big=0;
    for(u16 s=FIRST_MCB;;s=(u16)(s+msize(s)+1)) {
        if(!arena_ok(s)) return DE_ARENA;
        if(!mowner(s)) {
            if(msize(s)>big) big=msize(s);
            if(msize(s)>=paras) {
                unsigned fit=alloc_strategy&3;
                if(!pick || (fit==1 && msize(s)<msize(pick)) || fit==2) pick=s;
            }
        }
        if(mtype(s)=='Z') break;
    }
    if(!pick) {*largest=big; return DE_NOMEM;}
    u16 size=msize(pick);
    if(size>paras) {
        u8 type=mtype(pick);
        if((alloc_strategy&3)==2) {
            /* Last fit takes the top of the block. */
            u16 top=(u16)(pick+size-paras);
            set_mcb(pick,'M',0,(u16)(size-paras-1)); set_mcb(top,type,owner,paras);
            memset(LINEAR(mcb(top)+5),0,11); *seg=(u16)(top+1); return 0;
        }
        set_mcb(pick,'M',owner,paras);
        set_mcb((u16)(pick+paras+1),type,0,(u16)(size-paras-1));
    } else poke16(mcb(pick)+1,owner);
    memset(LINEAR(mcb(pick)+5),0,11);
    *seg=(u16)(pick+1); return 0;
}
static int block_mcb(u16 seg,u16 *m) {
    for(u16 s=FIRST_MCB;arena_ok(s);s=(u16)(s+msize(s)+1)) {
        if(s+1==seg) {*m=s; return 0;}
        if(mtype(s)=='Z') break;
    }
    return DE_BLOCK;
}
int mem_free(u16 seg) {
    u16 m; if(block_mcb(seg,&m) || !mowner(m) || mowner(m)==8) return DE_BLOCK;
    poke16(mcb(m)+1,0); return 0;
}
int mem_resize(u16 seg,u16 paras,u16 *largest) {
    u16 m; if(block_mcb(seg,&m) || !mowner(m) || mowner(m)==8) return DE_BLOCK;
    merge_free();
    u16 size=msize(m);
    if(paras<=size) {
        if(paras<size) {
            u8 type=mtype(m); set_mcb(m,'M',mowner(m),paras);
            set_mcb((u16)(m+paras+1),type,0,(u16)(size-paras-1)); merge_free();
        }
        return 0;
    }
    u16 n=(u16)(m+size+1);
    u32 room=size;
    if(mtype(m)=='M' && arena_ok(n) && !mowner(n)) room+=msize(n)+1U;
    if(room<paras) {*largest=(u16)room; return DE_NOMEM;}
    u8 type=mtype(n);
    u32 rest=room-paras;
    if(rest) {set_mcb(m,'M',mowner(m),paras); set_mcb((u16)(m+paras+1),type,0,(u16)(rest-1));}
    else set_mcb(m,type,mowner(m),paras);
    return 0;
}
void mem_free_owner(u16 psp) {
    for(u16 s=FIRST_MCB;arena_ok(s);s=(u16)(s+msize(s)+1)) {
        if(mowner(s)==psp) poke16(mcb(s)+1,0);
        if(mtype(s)=='Z') break;
    }
    merge_free();
}
static void mem_init(void) {
    set_mcb(FIRST_MCB,'M',0,(u16)(HOLE_START-1-FIRST_MCB-1));
    set_mcb(HOLE_START-1,'M',8,HOLE_END-HOLE_START);
    memcpy(LINEAR(mcb(HOLE_START-1)+8),"SC",2);
    set_mcb(HOLE_END,'Z',0,(u16)(MEM_TOP-HOLE_END-1));
}

/* Handles. */
u8 *jft(u16 psp) {u32 p=peek32(((u32)psp<<4)+0x34); return LIN(p>>16,p);}
u16 jft_size(u16 psp) {return peek16(((u32)psp<<4)+0x32);}
int handle_native(u16 h,unsigned *native) {
    if(h>=jft_size(cur_psp) || jft(cur_psp)[h]==0xff) return DE_HANDLE;
    *native=jft(cur_psp)[h]; return 0;
}
int handle_new(unsigned native,u16 *h) {
    u8 *t=jft(cur_psp); u16 n=jft_size(cur_psp);
    for(u16 i=0;i<n;i++) if(t[i]==0xff) {t[i]=(u8)native; *h=i; return 0;}
    dos_close(native); return DE_HANDLES;
}
/* Native handles 0-4 of this task follow the running program's standard
 * handles, so native console calls and native children see redirections.
 * They are never closed: the table's own duplicates keep the files open. */
void sync_std(void) {
    u8 *t=jft(cur_psp);
    for(unsigned i=0;i<5 && i<jft_size(cur_psp);i++) if(t[i]!=0xff && t[i]!=i) dos_dup2(t[i],i);
}
static void close_handles(u16 psp) {
    u8 *t=jft(psp); u16 n=jft_size(psp);
    for(u16 i=0;i<n;i++) if(t[i]!=0xff) {dos_close(t[i]); emm_closed(t[i]); t[i]=0xff;}
}

/* A program or overlay to load is part of EXEC: APPEND finds it only
 * with /X, as for 4B00h and 4B03h. */
static int open_file(const char *path,unsigned *h) {
    DosAppend a={.size=sizeof(a)}; u32 was=0;
    int masked=!dos_append(NULL,&a) && !(a.flags&DOS_APPEND_X) && !dos_append_task(0,&was);
    int e=dos_open(path,DOS_OPEN_READ,0,h);
    if(masked) dos_append_task(was,NULL);
    return e;
}
static int read_at(unsigned h,u32 offset,void *buffer,u32 bytes) {
    u32 position,got=0;
    int e=dos_seek(h,offset,0,&position);
    if(!e) e=dos_read(h,buffer,bytes,&got);
    return e?e:got==bytes?0:DE_FORMAT;
}
/* An MZ header's signature (or ZM, which DOS takes too). */
static int mz(const u8 *hdr) {return (hdr[0]=='M' && hdr[1]=='Z') || (hdr[0]=='Z' && hdr[1]=='M');}
/* An MZ image loaded at segment load: each word its relocation table
 * names gets add. */
static int relocate(unsigned h,const u8 *hdr,u16 load,u16 add) {
    u16 count=hdr[6]|hdr[7]<<8,table=hdr[24]|hdr[25]<<8; u8 entry[4]; int e=0;
    for(u16 i=0;i<count && !e;i++) {
        e=read_at(h,table+(u32)i*4,entry,4); if(e) break;
        u32 at=((u32)(u16)(load+(entry[2]|entry[3]<<8))<<4)+(entry[0]|entry[1]<<8);
        poke16(at,(u16)(peek16(at)+add));
    }
    return e;
}
/* Native images go to native EXEC; everything else with an MZ header, and
 * any .COM file, runs here (an MZ stub prints its own message). */
int is_dos16(const char *path) {
    unsigned h; if(open_file(path,&h)) return 0;
    u8 head[64]; u32 size=0;
    int dos16=0;
    if(!dos_seek(h,0,2,&size) && size>=2 && !read_at(h,0,head,size<64?size:64)) {
        if(mz(head)) {
            dos16=1;
            u32 pe=size>=64?head[60]|head[61]<<8|(u32)head[62]<<16|(u32)head[63]<<24:0;
            u8 sig[6];
            if(pe && pe<=size-6 && !read_at(h,pe,sig,6) && !memcmp(sig,"PE\0\0",4) && (sig[4]|sig[5]<<8)==0x200) dos16=0;
        } else {
            const char *dot=NULL;
            for(const char *p=path;*p;p++) if(*p=='.') dot=p; else if(*p=='\\') dot=NULL;
            dos16=dot && !stricmp(dot,".COM");
        }
    }
    dos_close(h); return dos16;
}

/* Environment: strings, an empty string, a count of 1 and the program path. */
static int build_environment(u16 source,const char *path,u16 *seg) {
    static char block[DOS_ENV_CAPACITY+DOS_PATH_MAX+8];
    u32 n=0;
    if(source) {
        const u8 *s=LIN(source,0);
        while(n<DOS_ENV_CAPACITY-1 && s[n]) {
            while(n<DOS_ENV_CAPACITY-1 && s[n]) {block[n]=(char)s[n]; n++;}
            block[n++]=0;
        }
        if(!n) block[n++]=0;
    } else {
        char entry[DOS_ENV_CAPACITY];
        for(u32 i=0;!dos_env_list(i,entry,sizeof(entry));i++) {
            u32 len=(u32)strlen(entry)+1;
            if(n+len>DOS_ENV_CAPACITY) break;
            memcpy(block+n,entry,len); n+=len;
        }
        if(!n) block[n++]=0;
    }
    block[n++]=0; block[n++]=1; block[n++]=0;
    u32 len=(u32)strlen(path)+1; memcpy(block+n,path,len); n+=len;
    u16 largest; int e=mem_alloc((u16)((n+15)/16),8,seg,&largest); if(e) return e;
    memcpy(LIN(*seg,0),block,n); return 0;
}
static void set_name(u16 psp,const char *path) {
    const char *name=path; for(const char *p=path;*p;p++) if(*p=='\\' || *p==':') name=p+1;
    u8 *field=LINEAR(mcb((u16)(psp-1))+8); memset(field,0,8);
    for(unsigned i=0;i<8 && name[i] && name[i]!='.';i++) field[i]=(u8)name[i];
}
static void parse_fcb(u8 *fcb,const char **text) {
    DosFcb temp; memset(&temp,0,sizeof(temp)); unsigned status;
    dos_fcb_parse(text,&temp,1,&status);
    memcpy(fcb,temp.bytes,12);
}
/* tail: DOS form, count byte and text (a CR is added). */
static void build_psp(u16 psp,u16 parent,u16 env,u16 end,const u8 *tail) {
    u32 p=(u32)psp<<4; u8 *b=LINEAR(p);
    memset(b,0,256);
    b[0]=0xcd; b[1]=0x20; poke16(p+2,end);
    poke32(p+0x0a,ivt(0x22)); poke32(p+0x0e,ivt(0x23)); poke32(p+0x12,ivt(0x24));
    poke16(p+0x16,parent); memset(b+0x18,0xff,20);
    poke16(p+0x2c,env); poke16(p+0x32,20); poke32(p+0x34,(u32)psp<<16|0x18); poke32(p+0x38,0xffffffffU);
    b[0x50]=0xcd; b[0x51]=0x21; b[0x52]=0xcb;
    unsigned len=tail[0]>126?126:tail[0];
    b[0x80]=(u8)len; memcpy(b+0x81,tail+1,len); b[0x81+len]=13;
    char text[128]; memcpy(text,tail+1,len); text[len]=0;
    const char *t=text;
    parse_fcb(b+0x5c,&t); parse_fcb(b+0x6c,&t);
}
/* Each program gets its own duplicates of the inherited handles. */
static void inherit_handles(u16 psp,u16 parent) {
    u8 *t=jft(psp);
    for(unsigned i=0;i<20;i++) {
        unsigned from;
        if(parent) {if(i>=jft_size(parent) || jft(parent)[i]==0xff) continue; from=jft(parent)[i];}
        else if(i<5) from=i; else continue;
        unsigned h; if(!dos_dup(from,&h)) t[i]=(u8)h;
    }
}

static int load_image(const char *path,const u8 *tail,u16 env_source,u16 parent,Image *im) {
    char full[DOS_PATH_MAX]; int e=dos_canonical(path,full); if(e) return e;
    unsigned h; e=open_file(full,&h); if(e) return e;
    u8 hdr[28]; u32 size=0; memset(hdr,0,sizeof(hdr));
    e=dos_seek(h,0,2,&size);
    if(!e && size>=2) e=read_at(h,0,hdr,size<28?size:28);
    int exe=!e && mz(hdr);
    u32 load_bytes=0,offset=0; u16 need=0,want=0;
    if(!e && exe) {
        u16 last=hdr[2]|hdr[3]<<8,pages=hdr[4]|hdr[5]<<8,header=hdr[8]|hdr[9]<<8;
        u32 image=(u32)pages*512-(last?512-last:0);
        offset=(u32)header*16;
        if(size<28 || image<=offset) e=DE_FORMAT;
        else {
            if(image>size) image=size;
            load_bytes=image-offset;
            u32 base=0x10+(load_bytes+15)/16;
            u32 lo=base+(hdr[10]|hdr[11]<<8),hi=base+(hdr[12]|hdr[13]<<8);
            if(lo>0xffff) e=DE_NOMEM;
            need=(u16)lo; want=hi>0xffff?0xffff:(u16)hi;
        }
    } else if(!e) {
        if(size>0xff00) e=DE_FORMAT;
        load_bytes=size; need=(u16)((0x200+size+15)/16); want=0xffff;
    }
    u16 env=0,psp=0,largest;
    if(!e) e=build_environment(env_source,full,&env);
    if(!e) {
        e=mem_alloc(want,8,&psp,&largest);
        if(e==DE_NOMEM && largest>=need) e=mem_alloc(largest,8,&psp,&largest);
        if(e) {mem_free(env); env=0;}
    }
    if(e) {dos_close(h); return e;}
    u16 m=(u16)(psp-1); u16 paras=msize(m);
    poke16(mcb(m)+1,psp); poke16(mcb((u16)(env-1))+1,psp); set_name(psp,full);
    /* The load module follows the PSP; a .COM image starts at PSP:100h. */
    u16 load=(u16)(psp+0x10);
    e=read_at(h,offset,LIN(load,0),load_bytes);
    if(!e && exe) e=relocate(h,hdr,load,load);
    dos_close(h);
    if(e) {mem_free_owner(psp); return e;}
    build_psp(psp,parent?parent:psp,env,(u16)(psp+paras),tail);
    inherit_handles(psp,parent);
    im->psp=psp;
    if(exe) {
        im->cs=(u16)(load+(hdr[22]|hdr[23]<<8)); im->ip=hdr[20]|hdr[21]<<8;
        im->ss=(u16)(load+(hdr[14]|hdr[15]<<8)); im->sp=hdr[16]|hdr[17]<<8;
    } else {
        u32 bytes=(u32)paras*16;
        im->cs=im->ss=psp; im->ip=0x100; im->sp=bytes>=0x10000?0xfffe:(u16)(bytes-2);
        poke16(((u32)psp<<4)+im->sp,0);
    }
    return 0;
}

static void reset_cpu(void) {
    memset(&cpu,0,sizeof(cpu));
    cpu.cpl=3; cpu.cflg=0x31; /* CR0: PE, ET, NE */
    cpu.fsr=0x55550000ULL; cpu.fcr=0x37f|0x1f80ULL<<32; cpu.eflags=FL_IF|EFLAGS_V86;
    for(unsigned s=SR_ES;s<=SR_GS;s++) set_sreg(s,0);
}
static void start(const Image *im) {
    reset_cpu();
    set_sreg(SR_CS,im->cs); set_sreg(SR_SS,im->ss); set_sreg(SR_DS,im->psp); set_sreg(SR_ES,im->psp);
    cpu.eip=im->ip; ww(ESP,im->sp);
    cur_psp=im->psp; dta=((u32)im->psp<<4)+0x80;
}
/* The native DTA follows the program's, for FCB transfers. */
void sync_dta(void) {
    DosRegs r={.ax=0x1a01,.dx=(u64)dta,.cx=0xa0000-dta};
    app_dos->int21(&r);
}

int exec_program(const char *path,u32 block) {
    if(depth+1>=MAX_DEPTH) return DE_NOMEM;
    u16 env=peek16(block);
    u32 t=peek32(block+2),f1=peek32(block+6),f2=peek32(block+10);
    u8 tail[128]; const u8 *src=LIN(t>>16,t);
    tail[0]=src[0]>126?126:src[0]; memcpy(tail+1,src+1,tail[0]);
    int e=load_image(path,tail,env?env:peek16(((u32)cur_psp<<4)+0x2c),cur_psp,&child);
    if(e) return e;
    u8 *p=LIN(child.psp,0);
    if(f1>>16 || (u16)f1) memcpy(p+0x5c,LIN(f1>>16,f1),16);
    if(f2>>16 || (u16)f2) memcpy(p+0x6c,LIN(f2>>16,f2),16);
    poke32(((u32)child.psp<<4)+0x0a,(u32)sreg(SR_CS)<<16|(u16)cpu.eip);
    child_pending=1; return 0;
}
static void start_child(void) {
    child_pending=0;
    Process *p=&procs[depth+1];
    clear_cf();
    p->parent=cpu; p->psp=child.psp; p->dta=((u32)child.psp<<4)+0x80;
    procs[depth].dta=dta;
    depth++;
    start(&child); sync_dta(); sync_std();
}
int load_overlay(const char *path,u32 block) {
    u16 seg=peek16(block),factor=peek16(block+2);
    char full[DOS_PATH_MAX]; int e=dos_canonical(path,full); if(e) return e;
    unsigned h; e=open_file(full,&h); if(e) return e;
    u8 hdr[28]; u32 size=0; memset(hdr,0,sizeof(hdr));
    e=dos_seek(h,0,2,&size);
    if(!e && size>=2) e=read_at(h,0,hdr,size<28?size:28);
    if(!e && mz(hdr)) {
        u16 last=hdr[2]|hdr[3]<<8,pages=hdr[4]|hdr[5]<<8,header=hdr[8]|hdr[9]<<8;
        u32 image=(u32)pages*512-(last?512-last:0),offset=(u32)header*16;
        if(image>size) image=size;
        e=image<=offset?DE_FORMAT:read_at(h,offset,LIN(seg,0),image-offset);
        if(!e) e=relocate(h,hdr,seg,factor);
    } else if(!e) e=read_at(h,0,LIN(seg,0),size);
    dos_close(h); return e;
}

void request_exit(u8 code,u8 kind,u16 keep) {pending_exit=1; exit_code=code; exit_kind=kind; exit_keep=keep;}
void terminate(u8 code,u8 kind,u16 keep) {
    u16 psp=cur_psp; u32 p=(u32)psp<<4;
    dpmi_terminate(psp);
    print_release(psp);
    set_ivt(0x22,peek32(p+0x0a)); set_ivt(0x23,peek32(p+0x0e)); set_ivt(0x24,peek32(p+0x12));
    if(keep) {
        u16 largest; mem_resize(psp,keep<6?6:keep,&largest);
        kind=3;
    } else {close_handles(psp); mem_free_owner(psp);}
    return_code=(u16)(code|kind<<8);
    if(!depth) {finished=1; final_code=code; return;}
    cpu=procs[depth].parent; depth--;
    cur_psp=procs[depth].psp; dta=procs[depth].dta; sync_dta(); sync_std();
}

/* Ctrl-C: INT 23h when hooked, otherwise the program ends. */
static void check_break(void) {
    if(!break_pending || !(cpu.eflags&0x20000)) return; /* in protected mode, when real mode comes */
    break_pending=0;
    if(dpmi_real_interrupt(0x23)) return;
    if(ivt(0x23)!=stub(0x23)) enter_interrupt(0x23);
    else {console_out((const u8 *)"^C\r\n",4); terminate(0,1,0);}
}
/* At a timer stop: Ctrl-C first in the keyboard queue is a break, also
 * for a program that makes no calls. */
static void poll_break(void) {
    IoEvent k;
    if(io->console_key(io->context,&k,IO_KEY_PEEK) || k.type!=IO_EVENT_KEY || k.unicode!=3 || (k.flags&IO_KEY_RELEASE)) return;
    io->console_key(io->context,&k,0);
    break_pending=1;
    check_break();
}
static int from_stub(unsigned n) {
    return sreg(SR_CS)==STUB_SEG && cpu.eip==n*2+2;
}
static void service(unsigned n) {
    switch(n) {
    case 0x20: request_exit(0,0,0); break;
    case 0x21: int21(); break;
    case 0x22: request_exit(0,0,0); break;
    case 0x23: request_exit(0,1,0); break;
    case 0x24: wl(EAX,3); break; /* fail */
    case 0x27: request_exit(0,0,(u16)((rw(EDX)+15U)>>4)); break;
    default: bios_int(n);
    }
}
static void software_interrupt(unsigned n) {
    /* The trap of an entry the program called (vdm.h's TRAP_SEG). */
    if(sreg(SR_CS)==TRAP_SEG) {
        if(n==0x67 && cpu.eip==EMS_ENTRY+2 && ems_present()) {ems_call(); return;}
        if(n==0x67 && cpu.eip==XMS_ENTRY+7 && xms_present()) {xms_call(); return;}
        if(n==0x67 && cpu.eip==EMS_RETURN+2 && ems_present()) {ems_return(); return;}
        if(n==0x67 && cpu.eip==DPMI_SWITCH+2) {dpmi_switch(); return;}
        if(n==0x67 && cpu.eip==DPMI_RETURN+2) {dpmi_return(); return;}
        if(n==0x67 && cpu.eip==DPMI_RAW+2 && dpmi_active()) {dpmi_raw_to_pm(); return;}
        if(n==0x67 && cpu.eip>=DPMI_CALLBACKS+2 && cpu.eip<DPMI_CALLBACKS+2+64 && !(cpu.eip&1)) {dpmi_callback((unsigned)(cpu.eip-DPMI_CALLBACKS-2)/2); return;}
    }
    int stubbed=from_stub(n);
    if(!stubbed && ivt(n)!=stub(n)) {enter_interrupt(n); return;}
    service(n);
    if(pending_exit) {pending_exit=0; terminate(exit_code,exit_kind,exit_keep); return;}
    if(stubbed) iret16(1);
    /* A hooked INT 8 chained to the default handler: its end of interrupt,
     * and INT 1Ch. */
    if(stubbed && n==8) {irq0_in_service=0; if(!dpmi_real_interrupt(0x1c) && ivt(0x1c)!=stub(0x1c)) enter_interrupt(0x1c);}
    if(child_pending) start_child();
    check_break();
}
static const char hex[]="0123456789ABCDEF";
static void fault(const char *what) {
    char text[120]; unsigned n=0;
    const char *head="VDM: ";
    while(*head) text[n++]=*head++;
    while(*what && n<80) text[n++]=*what++;
    const char *at=" at ";
    while(*at) text[n++]=*at++;
    u16 cs=sreg(SR_CS),ip=(u16)cpu.eip;
    for(int i=12;i>=0;i-=4) text[n++]=hex[(cs>>i)&15];
    text[n++]=':';
    for(int i=12;i>=0;i-=4) text[n++]=hex[(ip>>i)&15];
    text[n++]='\r'; text[n++]='\n';
    console_out((const u8 *)text,n);
    terminate(255,0,0);
}
static void exception(unsigned v) {
    switch(v) {
    case 1: case 3:
        if(ivt(v)!=stub(v)) enter_interrupt(v); else if(v==1) cpu.eflags&=~(u64)FL_TF;
        return;
    case 4: if(ivt(4)!=stub(4)) enter_interrupt(4); return;
    case 0:
        if(ivt(0)!=stub(0)) {enter_interrupt(0); return;}
        console_out((const u8 *)"Divide overflow\r\n",17); terminate(255,0,0); return;
    case 5: case 6:
        if(ivt(v)!=stub(v)) {enter_interrupt(v); return;}
        fault(v==5?"bound range exceeded":"invalid instruction"); return;
    case 16:
        if(ivt(0x75)!=stub(0x75)) {enter_interrupt(0x75); return;}
        fault("floating-point error"); return;
    case 13: if(emulate_privileged()) return; fault("general protection fault"); return;
    case 12: fault("stack segment limit exceeded"); return;
    case 17: fault("alignment check"); return;
    default: {
        static char text[]="processor exception 00h";
        text[20]=hex[v>>4&15]; text[21]=hex[v&15];
        fault(text);
    }
    }
}
/* SMSW at CS:IP, which the processor leaves to VDM in virtual-8086 mode:
 * CR0's low word (PE set, as under any V86 monitor) to a register, or to
 * memory through a 16-bit or, after 67h, a 32-bit address. */
static int smsw(void) {
    u16 cs=sreg(SR_CS),ip=(u16)cpu.eip; unsigned n=0,seg=SR_DS,size32=0,addr32=0,overridden=0;
    u8 op;
    for(;;) {
        op=*LIN(cs,(u16)(ip+n));
        if(op==0x66) size32=1; else if(op==0x67) addr32=1;
        else if(op==0x26 || op==0x2e || op==0x36 || op==0x3e) {seg=op==0x26?SR_ES:op==0x2e?SR_CS:op==0x36?SR_SS:SR_DS; overridden=1;}
        else if(op==0x64 || op==0x65) {seg=op==0x64?SR_FS:SR_GS; overridden=1;}
        else if(op!=0xf0 && op!=0xf2 && op!=0xf3) break;
        if(++n>14) return 0;
    }
    u8 modrm=*LIN(cs,(u16)(ip+n+2));
    if(op!=0x0f || *LIN(cs,(u16)(ip+n+1))!=0x01 || ((modrm>>3)&7)!=4) return 0;
    n+=3;
    unsigned mod=modrm>>6,rm=modrm&7;
    if(mod==3) {
        if(size32) cpu.gr[rm]=(u32)cpu.cflg; else ww(rm,(u16)cpu.cflg);
        cpu.eip=(u16)(ip+n); return 1;
    }
    u32 offset=0;
    if(!addr32) {
        static const signed char base[8]={EBX,EBX,EBP,EBP,-1,-1,EBP,EBX},index[8]={ESI,EDI,ESI,EDI,ESI,EDI,-1,-1};
        if(mod==0 && rm==6) {offset=*LIN(cs,(u16)(ip+n))|(u32)*LIN(cs,(u16)(ip+n+1))<<8; n+=2;}
        else {
            if(base[rm]>=0) offset+=rw((unsigned)base[rm]);
            if(index[rm]>=0) offset+=rw((unsigned)index[rm]);
            if(base[rm]==EBP && !overridden) seg=SR_SS;
        }
        if(mod==1) {offset+=(u32)(int)(signed char)*LIN(cs,(u16)(ip+n)); n++;}
        else if(mod==2) {offset+=*LIN(cs,(u16)(ip+n))|(u32)*LIN(cs,(u16)(ip+n+1))<<8; n+=2;}
        offset&=0xffff;
    } else {
        unsigned b=rm;
        if(rm==4) {
            u8 sib=*LIN(cs,(u16)(ip+n)); n++; b=sib&7;
            unsigned x=(sib>>3)&7;
            if(x!=4) offset+=(u32)cpu.gr[x]<<(sib>>6);
        }
        if(b==5 && mod==0) {
            for(unsigned i=0;i<4;i++) offset|=(u32)*LIN(cs,(u16)(ip+n+i))<<(i*8);
            n+=4;
        } else {
            offset+=(u32)cpu.gr[b];
            if((b==EBP || b==ESP) && !overridden) seg=SR_SS;
        }
        if(mod==1) {offset+=(u32)(int)(signed char)*LIN(cs,(u16)(ip+n)); n++;}
        else if(mod==2) {u32 d=0; for(unsigned i=0;i<4;i++) d|=(u32)*LIN(cs,(u16)(ip+n+i))<<(i*8); offset+=d; n+=4;}
        if(offset>0xfffe) return 0;
    }
    poke16(((u32)sreg(seg)<<4)+offset,(u16)cpu.cflg);
    cpu.eip=(u16)(ip+n); return 1;
}
static void intercept(void) {
    if(cpu.vector==2) return; /* MOV SS, POPF: resume after it */
    if(cpu.vector==0) {
        unsigned prefixes=(cpu.code>>12)&15; u8 op=(u8)cpu.iim;
        if(op==0xcf) {if(cpu.code&2) iret32(); else iret16(0); return;}
        if(op==0xf4) {io->stall_us(io->context,1000); cpu.eip=(u16)(cpu.eip+prefixes+1); return;}
        if(smsw()) return;
    }
    fault("unsupported instruction");
}

/* --- the timer interrupt --------------------------------------------------- */
static u64 next_tick_us;
static unsigned pending_ticks;
static void timer_due(void) {
    u64 now=io->ticks_ms(io->context)*1000ULL; u32 period=timer_period_us();
    if(!next_tick_us || next_tick_us>now+65536000ULL) next_tick_us=now+period;
    while(now>=next_tick_us && pending_ticks<32) {pending_ticks++; next_tick_us+=period;}
    if(now>=next_tick_us) next_tick_us=now+period; /* behind: drop the rest */
}
static void timer_deliver(void) {
    if(!pending_ticks || !(cpu.eflags&FL_IF) || irq0_masked() || irq0_in_service) return;
    pending_ticks--;
    /* A DPMI program's protected-mode handlers take them first. */
    if(dpmi_real_interrupt(8) || ivt(8)!=stub(8)) {irq0_in_service=1; if(cpu.eflags&0x20000) enter_interrupt(8);}
    else if(!dpmi_real_interrupt(0x1c) && ivt(0x1c)!=stub(0x1c)) enter_interrupt(0x1c);
}
static u32 run_limit_ms(void) {
    u32 ms=(timer_period_us()+999)/1000;
    return ms<1?1:ms>55?55:ms;
}

static int break_handler(void *context) {(void)context; break_pending=1; return DOS_BREAK_CANCEL;}

static void init_low_memory(void) {
    memset(LINEAR(0),0,0x10000); memset(LIN(HOLE_END,0),0,(u32)(MEM_TOP-HOLE_END)<<4);
    /* The vectors a PC's BIOS and DOS set lead to VDM's stubs; the rest all
     * lead to one IRET, as a BIOS's unused vectors do: programs look for
     * unused vectors as equal neighbours (DOS/4GW, below BEh) and for a
     * mouse driver or an EMM as a vector that is more than an IRET. */
    for(unsigned n=0;n<256;n++) {
        int unused=n==0x33 || (n>=0x34 && n<=0x3f) || (n>=0x50 && n<=0x6f) || n>=0x78;
        set_ivt(n,unused?DOS_DATA_SEG<<16|UNUSED_IRET_OFF:stub(n)); u8 *s=LINEAR(STUB_LINEAR+n*2); s[0]=0xcd; s[1]=(u8)n;
    }
    u32 d=DOS_DATA_SEG<<4;
    *LINEAR(d+CASEMAP_OFF)=0xcb; *LINEAR(d+UNUSED_IRET_OFF)=0xcf;
    /* The entries the drivers give: the EMM device header with its name,
     * INT 67h's, XMS's and the return from 56h's call. */
    u8 *t=LINEAR(TRAP_SEG<<4);
    poke32(TRAP_SEG<<4,0xffffffffU); poke16((TRAP_SEG<<4)+4,0xc000);
    poke16((TRAP_SEG<<4)+6,XMS_ENTRY+7); poke16((TRAP_SEG<<4)+8,XMS_ENTRY+7); /* the RETF there */
    memcpy(t+10,"EMMXXXX0",8);
    t[EMS_ENTRY]=0xcd; t[EMS_ENTRY+1]=0x67; t[EMS_ENTRY+2]=0xcf;
    static const u8 xms[8]={0xeb,0x03,0x90,0x90,0x90,0xcd,0x67,0xcb}; memcpy(t+XMS_ENTRY,xms,8);
    t[EMS_RETURN]=0xcd; t[EMS_RETURN+1]=0x67; t[EMS_RETURN+2]=0xcf;
    /* DPMI's entries. */
    static const u8 dpmi_traps[]={DPMI_SWITCH,DPMI_RETURN,DPMI_RAW};
    for(unsigned i=0;i<sizeof dpmi_traps;i++) {t[dpmi_traps[i]]=0xcd; t[dpmi_traps[i]+1]=0x67;}
    t[DPMI_SAVE]=0xcb;
    for(unsigned i=0;i<32;i++) {t[DPMI_CALLBACKS+i*2]=0xcd; t[DPMI_CALLBACKS+i*2+1]=0x67;}
    if(ems_present()) set_ivt(0x67,TRAP_SEG<<16|EMS_ENTRY);
    poke16(d+SYSVARS_OFF-2,FIRST_MCB);
    mem_init();
    bios_init();
}
/* Conventional memory, but the firmware's hole, to a buffer or back. */
static void copy_low(u8 *buffer,int restore) {
    if(!buffer) return;
    for(u32 a=0;a<0xa0000;a+=0x1000) {
        if(a>=(u32)HOLE_START<<4 && a<(u32)HOLE_END<<4) continue;
        if(restore) memcpy(LINEAR(a),buffer+a,0x1000); else memcpy(buffer+a,LINEAR(a),0x1000);
    }
}
static void save_low(int restore) {copy_low(saved_low,restore);}
/* The high memory area IO.SYS holds for VDMs (IO_CAP_HMA): each machine
 * keeps its own contents, as it keeps conventional memory's. */
#define HMA_PAGES 16U
static u8 *saved_hma;
static void copy_hma(u8 *buffer,int restore) {
    if(!buffer || !(io->capabilities&IO_CAP_HMA)) return;
    if(restore) memcpy(LINEAR(0x100000),buffer,HMA_PAGES*4096); else memcpy(buffer,LINEAR(0x100000),HMA_PAGES*4096);
}

/* Switching away (MSDOS.SYS's switch hook, Alt+Tab and the like, from INT
 * 16h here or a DOS key read): the program's conventional memory and the
 * adapter put aside, what was below 640 KiB before put back, and the hook
 * before VDM's run (Interface Manager's, or an outer VDM's); then all of it
 * taken up again as the program goes on. */
#define AWAY_PLANE_PAGES (4*VGA_PLANE/4096)
static DosSwitchHook outer_switch; static int switch_hooked;
static u8 *away_low,*away_planes,*away_hma;
static int vdm_switch(void *context,u32 key) {
    (void)context;
    if(!outer_switch.hook || !saved_low || !away_low || !away_planes) return 0;
    if(key&DOS_SWITCH_QUERY) return outer_switch.hook(outer_switch.context,key);
    int had=vga_away(away_planes);
    copy_low(away_low,0); save_low(1); copy_hma(away_hma,0); copy_hma(saved_hma,1);
    int taken=outer_switch.hook(outer_switch.context,key);
    copy_hma(saved_hma,0); copy_hma(away_hma,1); save_low(0); copy_low(away_low,1);
    if(had) vga_back(away_planes);
    return taken;
}
/* Pages above the first MiB, which a VDM taking up conventional memory
 * again (an outer one, after this one) leaves alone. */
static int alloc_high(u32 pages,void **out) {
    void *low[8]; int n=0,e;
    for(;;) {
        e=io->alloc_pages(io->context,pages,out);
        if(e || (uintptr_t)*out>=0x100000 || n==8) break;
        low[n++]=*out;
    }
    if(!e && (uintptr_t)*out<0x100000) {io->free_pages(io->context,*out,pages); *out=NULL; e=DE_NOMEM;}
    while(n) io->free_pages(io->context,low[--n],pages);
    return e;
}
static void switch_begin(void) {
    if(app_dos->size<offsetof(DosApi,switch_away)+sizeof(app_dos->switch_away) || !saved_low) return;
    app_dos->switch_hook(NULL,&outer_switch);
    if(!outer_switch.hook) return;
    if(alloc_high(SAVED_PAGES,(void **)&away_low)) {away_low=NULL; return;}
    if(alloc_high(AWAY_PLANE_PAGES,(void **)&away_planes)) {io->free_pages(io->context,away_low,SAVED_PAGES); away_low=away_planes=NULL; return;}
    if((io->capabilities&IO_CAP_HMA) && alloc_high(HMA_PAGES,(void **)&away_hma)) away_hma=NULL;
    DosSwitchHook mine={vdm_switch,NULL};
    app_dos->switch_hook(&mine,NULL); switch_hooked=1;
}
static void switch_end(void) {
    if(switch_hooked) app_dos->switch_hook(&outer_switch,NULL);
    if(away_low) io->free_pages(io->context,away_low,SAVED_PAGES);
    if(away_planes) io->free_pages(io->context,away_planes,AWAY_PLANE_PAGES);
    if(away_hma) io->free_pages(io->context,away_hma,HMA_PAGES);
    away_low=away_planes=away_hma=NULL; switch_hooked=0;
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    io=dos_io_services();
    if(!io || io->size<offsetof(IoServices,ia32_run)+sizeof(io->ia32_run) || !(io->capabilities&IO_CAP_IA32)) {
        print("VDM: 16-bit programs need a processor with the IA-32 instruction set\n");
        return (EFI_STATUS)255;
    }
    /* The tail is the program path, then the program's own command tail. */
    const char *tail=app_dos->command_tail(); if(!tail) tail="";
    while(*tail==' ') tail++;
    char path[DOS_PATH_MAX]; unsigned n=0;
    while(tail[n] && tail[n]!=' ' && n<sizeof(path)-1) {path[n]=tail[n]; n++;}
    path[n]=0; tail+=n; if(*tail==' ') tail++;
    if(!n) {print("VDM: no program\n"); return (EFI_STATUS)255;}
    u8 args[128]; unsigned len=0;
    if(*tail && *tail!=' ' && *tail!='\t') args[1+len++]=' ';
    while(*tail && len<126) args[1+len++]=(u8)*tail++;
    args[0]=(u8)len;
    if(io->alloc_pages(io->context,SAVED_PAGES,&saved_low)) saved_low=NULL;
    save_low(0);
    if((io->capabilities&IO_CAP_HMA) && io->alloc_pages(io->context,HMA_PAGES,(void **)&saved_hma)) saved_hma=NULL;
    copy_hma(saved_hma,0);
    dos_set_handle_count(255);
    xms_init(); ems_init();
    if(ems_present()) mem_top=ems_frame();
    init_low_memory();
    vga_open();
    switch_begin();
    DosBreakHandler handler={break_handler,NULL},previous;
    int have_handler=!dos_break_handler(&handler,&previous);
    Image first;
    int e=load_image(path,args,0,0,&first);
    if(e) print("VDM: cannot load %s (%s)\n",path,dos_error(e));
    else {
        start(&first); procs[0].psp=first.psp; sync_dta(); sync_std();
        /* After MOV SS or POP SS (the processor's system flag intercept)
         * no interrupt comes until the next instruction has run: it loads
         * the stack pointer that goes with the new stack. */
        int shadow=0; u64 shadow_cs=0; u32 shadow_eip=0;
        while(!finished) {
            bios_ticks();
            cpu.timer_ms=run_limit_ms();
            if(io->ia32_run(io->context,&cpu)) {print("VDM: IA-32 execution failed\n"); final_code=255; break;}
            if(!(cpu.eflags&0x20000)) {
                /* Protected mode: a DPMI program's. */
                if(cpu.exit!=IO_IA32_TIMER) dpmi_exit(); else poll_break();
                if(pending_exit) {pending_exit=0; terminate(exit_code,exit_kind,exit_keep);}
                else if(child_pending) start_child();
            }
            else if(cpu.exit==IO_IA32_INTERRUPT) software_interrupt(cpu.vector);
            else if(cpu.exit==IO_IA32_EXCEPTION) exception(cpu.vector);
            else if(cpu.exit!=IO_IA32_TIMER) intercept();
            else poll_break();
            if(pending_exit) {pending_exit=0; terminate(exit_code,exit_kind,exit_keep);}
            if(cpu.exit==IO_IA32_INTERCEPT && cpu.vector==2) {shadow=1; shadow_cs=cpu.cs; shadow_eip=(u32)cpu.eip;}
            else if(shadow && (cpu.cs!=shadow_cs || (u32)cpu.eip!=shadow_eip)) shadow=0;
            if(!finished) {
                timer_due();
                if(shadow) {}
                else if(cpu.eflags&0x20000) timer_deliver();
                else if(pending_ticks && dpmi_timer()) pending_ticks--;
            }
        }
    }
    switch_end();
    dpmi_close(); xms_close(); ems_close();
    video_close();
    if(have_handler) dos_break_handler(&previous,NULL);
    save_low(1); copy_hma(saved_hma,1);
    if(saved_low) io->free_pages(io->context,saved_low,SAVED_PAGES);
    if(saved_hma) io->free_pages(io->context,saved_hma,HMA_PAGES);
    DosRegs r={.ax=0x4c00|(e?255:final_code)}; app_dos->int21(&r);
    return EFI_SUCCESS;
}
