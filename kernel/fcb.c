/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native FCB services, following DOS 4 FCB/FCBIO/FCBIO2 record semantics.
 * Reserved FCB bytes hold a task-owned token, not a pointer or disk location.
 */
#include "fcb.h"
#include "nls.h"
typedef struct {u64 token; u32 pid; unsigned ref; FcbInfo info;} Binding;
static Binding bindings[DOS_MAX_FILES];
static u64 next_token;
#define SEARCH_MAGIC 0x31424346U
static u64 token(const u8 *f) {return rd32(f+24)|((u64)rd32(f+28)<<32);}
static void set_token(u8 *f,u64 value) {wr32(f+24,value); wr32(f+28,value>>32);}
static Binding *binding(const u8 *f) {
    u64 value=token(f); if(!value) return NULL;
    for(unsigned i=0;i<ARRAY_SIZE(bindings);i++) if(bindings[i].token==value) return &bindings[i];
    return NULL;
}
void fcb_reset(void) {memset(bindings,0,sizeof(bindings)); next_token=0x4643000000000001ULL;}
int fcb_reap(u32 pid) {
    int error=0;
    for(unsigned i=0;i<ARRAY_SIZE(bindings);i++) if(bindings[i].token && bindings[i].pid==pid) {
        int e=fcb_ref_close(bindings[i].ref,pid,NULL,NULL);
        if(e && !error) error=dos_preserve_error(e);
        memset(&bindings[i],0,sizeof(bindings[i]));
    }
    return error;
}
static unsigned drive_number(const u8 *f) {return f[0]?(unsigned)f[0]-1:dos_current_drive();}
static void save_info(u8 *f,Binding *b,const FcbInfo *info) {
    b->info=*info; wr32(f+16,info->size); wr16(f+20,info->date); wr16(f+22,info->time);
}
static int close_fcb(u8 *f) {
    Binding *b=binding(f);
    if(!b) return 0;
    if(b->pid!=dos_pid()) return DE_HANDLE;
    FcbInfo wanted=b->info;
    wanted.size=rd32(f+16); wanted.date=rd16(f+20); wanted.time=rd16(f+22);
    int e=fcb_ref_close(b->ref,b->pid,&b->info,&wanted);
    memset(b,0,sizeof(*b)); set_token(f,0); return e;
}
/* Convert a fixed 8.3 field without accepting paths, embedded padding or
 * aliases that could name a different file when converted back to a path. */
static int raw_name(const u8 in[11],u8 out[11],int pattern) {
    for(unsigned start=0;start<11;start+=8) {
        unsigned end=start?11:8; int padding=0;
        for(unsigned i=start;i<end;i++) {
            u8 c=!i && in[i]==5?0xe5:in[i];
            if(nls_lead(c)) {
                if(padding || i+1==end || !in[i+1]) return DE_PATH;
                out[i]=c; i++; out[i]=in[i]; continue;
            }
            c=nls_upper(c,1);
            if(pattern && c=='*') {memset(out+i,'?',end-i); break;}
            if(c==' ') padding=1;
            else if(padding || (c<32 && !(i==0 && c==5)) || strchr(".\"*+,/:;<=>[\\]|",c) || (!pattern && c=='?')) return DE_PATH;
            out[i]=c;
        }
    }
    if(out[0]==' ') return DE_PATH;
    if(out[0]==0xe5) out[0]=5;
    return 0;
}
static int raw_label(const u8 in[11],u8 out[11],int pattern) {
    int nonblank=0;
    for(unsigned i=0;i<11;i++) {
        u8 c=!i && in[i]==5?0xe5:in[i];
        if(nls_lead(c)) {
            if(i+1==11 || !in[i+1]) return DE_PATH;
            out[i]=c; i++; out[i]=in[i]; nonblank=1; continue;
        }
        c=nls_upper(c,1);
        if(pattern && c=='*') {memset(out+i,'?',11-i); return 0;}
        if((c<32 && !(i==0 && c==5)) || strchr(".\"*+,/:;<=>[\\]|",c) || (!pattern && c=='?')) return DE_PATH;
        out[i]=c; if(c!=' ') nonblank=1;
    }
    if(out[0]==0xe5) out[0]=5;
    return nonblank?0:DE_PATH;
}
static int fcb_path(const u8 *f,char path[15]) {
    u8 raw[11]; int e=raw_name(f+1,raw,0); if(e) return e;
    unsigned drive=drive_number(f); if(drive>=DOS_DRIVES) return DE_DRIVE;
    Node n={0}; memcpy(n.raw,raw,11);
    path[0]='A'+drive; path[1]=':'; fat_name(&n,path+2);
    if((u8)path[2]==5) path[2]=(char)0xe5;
    return 0;
}
static int open_fcb(u8 *f,u8 attr,int create) {
    Binding *old=binding(f);
    if(old) {int e=close_fcb(f); if(e) return e;}
    if(!create && (attr&FA_VOLUME)) return DE_ACCESS;
    if(create && (attr&FA_VOLUME)) {
        if(attr&~0x2fU) return DE_ACCESS;
        u8 name[11]; int e=raw_label(f+1,name,0); if(e) return e;
        FcbInfo info; e=fcb_label_create(drive_number(f),name,&info); if(e) return e;
        f[0]=info.drive+1; wr16(f+12,0); wr16(f+14,128); wr32(f+16,0);
        wr16(f+20,info.date); wr16(f+22,info.time); set_token(f,0); return 0;
    }
    Binding *b=NULL;
    for(unsigned i=0;i<ARRAY_SIZE(bindings);i++) if(!bindings[i].token) {b=&bindings[i]; break;}
    if(!b || !next_token) return DE_HANDLES;
    char path[15]; int e=fcb_path(f,path); if(e) return e;
    FcbInfo info; unsigned ref; e=fcb_ref_open(path,attr,create,&ref,&info); if(e) return e;
    *b=(Binding){.token=next_token++,.pid=dos_pid(),.ref=ref};
    set_token(f,b->token); save_info(f,b,&info);
    if(!info.device) f[0]=info.drive+1;
    wr16(f+12,0); wr16(f+14,128); return 0;
}
static u32 record_size(u8 *f,int store) {
    u32 size=rd16(f+14); if(!size) {size=128; if(store) wr16(f+14,128);}
    return size;
}
static u32 random_record(const u8 *f,u32 size) {
    u32 record=rd32(f+33); return size<64?record:record&0xffffff;
}
static void set_random(u8 *f,u32 record,u32 size) {
    wr16(f+33,record); f[35]=record>>16; if(size<64) f[36]=record>>24;
}
static void set_extent(u8 *f,u32 record) {wr16(f+12,record>>7); f[32]=record&127;}
static int transfer(DosRegs *r,u8 *f,unsigned fn,void *dta,u32 capacity) {
    int write=fn==0x15 || fn==0x22 || fn==0x28;
    int random=fn==0x21 || fn==0x22 || fn==0x27 || fn==0x28;
    int block=fn==0x27 || fn==0x28;
    u64 requested=block?r->cx:1; if(block) r->cx=0;
    r->ax=1;
    Binding *b=binding(f); if(!b || b->pid!=dos_pid()) return DE_HANDLE;
    if(requested>UINT32_MAX) return DE_FUNCTION;
    u32 size=record_size(f,1);
    u32 record=random?random_record(f,size):(u32)rd16(f+12)*128+(f[32]&127);
    if(random && !block) set_extent(f,record);
    u64 offset=(u64)record*size; if(offset>UINT32_MAX) return DE_SEEK;
    u32 count=MIN(requested,capacity/size);
    r->ax=count<requested?2:0;
    if(requested && !count) return 0;
    if((u64)count*size>UINT32_MAX-offset) {r->ax=1; return DE_FULL;}
    u32 bytes=0; FcbInfo info=b->info;
    int e=fcb_ref_io(b->ref,write,offset,dta,count*size,&bytes,&info);
    save_info(f,b,&info);
    u32 whole=bytes/size,tail=bytes%size,advanced=whole+!!tail;
    if(e || whole<count) r->ax=1;
    if(!write && tail) {memset((u8 *)dta+bytes,0,size-tail); r->ax=3;}
    if(!random || block) set_extent(f,record+advanced);
    if(block) {set_random(f,record+advanced,size); r->cx=whole+(!write && tail);}
    return e;
}
static void store_search(u8 *f,const DosFind *find) {
    wr32(f+12,find->index); wr16(f+16,find->dir); f[18]=find->search_attr;
    f[19]=find->cookie_low; memcpy(f+20,find->cookie_high,3);
    wr32(f+23,dos_pid()); wr32(f+27,SEARCH_MAGIC);
}
static int load_search(const u8 *f,DosFind *find) {
    if(rd32(f+27)!=SEARCH_MAGIC || rd32(f+23)!=dos_pid()) return DE_NOMORE;
    memset(find,0,sizeof(*find)); find->index=rd32(f+12); find->dir=rd16(f+16);
    find->search_attr=f[18]; find->cookie_low=f[19]; memcpy(find->cookie_high,f+20,3);
    return (find->search_attr&0x1e)==FA_VOLUME?raw_label(f+1,find->mask,1):raw_name(f+1,find->mask,1);
}
static int search(u8 *f,u8 attr,int extended,int first,void *dta,u32 capacity) {
    if(binding(f)) return DE_ACCESS;
    if(capacity<(extended?40U:33U)) return DE_NOMEM;
    DosFind find={0}; Node n; u8 mask[11]; int e;
    if(first) {e=(attr&0x1e)==FA_VOLUME?raw_label(f+1,mask,1):raw_name(f+1,mask,1); if(e) return e;}
    else {e=load_search(f,&find); if(e) return e;}
    e=fcb_find(drive_number(f),attr,mask,&find,&n,first);
    store_search(f,&find); if(e) return e;
    u8 result[40]={0},*normal=result;
    if(extended) {result[0]=255; result[6]=find.search_attr; normal+=7;}
    normal[0]=(find.index>>27)+1; memcpy(normal+1,n.raw,32);
    memcpy(dta,result,extended?40:33); return 0;
}
static int change(u8 *f,u8 attr,int rename) {
    int (*convert)(const u8 *,u8 *,int)=(attr&0x1e)==FA_VOLUME?raw_label:raw_name;
    u8 mask[11],replacement[11]; int e=convert(f+1,mask,1); if(e) return e;
    if(rename) {e=convert(f+17,replacement,1); if(e) return e;}
    DosFind find={0}; Node n;
    e=fcb_find(drive_number(f),attr,mask,&find,&n,1); if(e) return e;
    do {
        u8 name[11],checked[11];
        if(rename) {
            for(unsigned i=0;i<11;i++) {
                u8 c=!i && replacement[i]==5?0xe5:replacement[i];
                name[i]=replacement[i]=='?'?n.raw[i]:replacement[i];
                if(nls_lead(c) && i+1<(i<8?8:11)) {i++; name[i]=replacement[i];}
            }
            e=convert(name,checked,0); if(e) return e;
        }
        e=fcb_mutate(&find,&n,rename?checked:NULL); if(e) return e;
        e=fcb_find(0,0,NULL,&find,&n,0);
    } while(!e);
    return e==DE_NOMORE?0:e;
}
static int terminator(u8 c) {return c<=32 || strchr(".:;,=+/\"[]\\<>|",c)!=NULL;}
static const u8 *parse_part(const u8 *p,u8 *out,unsigned width,unsigned *status,int force) {
    if(terminator(*p) && !force) return p;
    memset(out,' ',width); unsigned used=0;
    while(!terminator(*p)) {
        if(nls_lead(*p)) {
            if(!p[1]) {*status=256; return p;}
            if(used+2<=width) {out[used++]=p[0]; out[used++]=p[1];}
            else used=width;
            p+=2; continue;
        }
        u8 c=nls_upper(*p++,1);
        if(used==width) continue;
        if(c=='*') {memset(out+used,'?',width-used); used=width; if(*status!=255) *status=1;}
        else {out[used++]=c; if(c=='?' && *status!=255) *status=1;}
    }
    return p;
}
static int parse(DosRegs *r) {
    const u8 *p=(const u8 *)(uintptr_t)r->si; u8 *f=(u8 *)(uintptr_t)r->di;
    unsigned flags=r->ax&15,status=0; r->ax=255;
    if(!p || !f) return DE_FUNCTION;
    if(!(flags&2)) f[0]=0;
    if(!(flags&4)) memset(f+1,' ',8);
    if(!(flags&8)) memset(f+9,' ',3);
    wr16(f+12,0); wr16(f+14,0);
    while(*p==' ' || *p=='\t') p++;
    if((flags&1) && *p && strchr(":<>|+=;,",*p)) {
        p++; while(*p==' ' || *p=='\t') p++;
    }
    if(!terminator(*p) && !nls_lead(*p) && p[1]==':') {
        unsigned drive=(unsigned)(upper(*p)-'A');
        f[0]=(u8)(drive+1); if(!fcb_drive_exists(drive)) status=255;
        p+=2;
    }
    p=parse_part(p,f+1,8,&status,0);
    if(*p=='.') p=parse_part(p+1,f+9,3,&status,1);
    r->si=(uintptr_t)p; r->ax=status>255?255:status;
    return status==256?DE_PATH:status==255?DE_DRIVE:0;
}
int fcb_call(DosRegs *r,void *dta,u32 capacity) {
    unsigned fn=(r->ax>>8)&255;
    if(fn==0x29) return parse(r);
    u8 *f=(u8 *)(uintptr_t)r->dx; r->ax=255;
    if(!f) {if(fn==0x27 || fn==0x28) r->cx=0; return DE_FUNCTION;}
    int extended=f[0]==255; u8 attr=extended?f[6]:0;
    if(extended) f+=7;
    int e;
    switch(fn) {
    case 0x0f: case 0x16: e=open_fcb(f,attr,fn==0x16); break;
    case 0x10: e=close_fcb(f); break;
    case 0x11: case 0x12: e=search(f,attr,extended,fn==0x11,dta,capacity); break;
    case 0x13: case 0x17: e=change(f,attr,fn==0x17); break;
    case 0x14: case 0x15: case 0x21: case 0x22: case 0x27: case 0x28:
        return transfer(r,f,fn,dta,capacity);
    case 0x23: {
        char path[15]; FcbInfo info; e=fcb_path(f,path);
        if(!e) e=fcb_path_info(path,attr,&info);
        if(!e) {u32 size=record_size(f,0); set_random(f,((u64)info.size+size-1)/size,size);}
        break;
    }
    case 0x24: set_random(f,(u32)rd16(f+12)*128+(f[32]&127),rd16(f+14)); e=0; break;
    default: e=DE_FUNCTION;
    }
    r->ax=e?255:0; return e;
}
