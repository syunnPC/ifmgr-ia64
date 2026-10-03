/* SPDX-License-Identifier: GPL-2.0-or-later
 * LIM EMS 4.0, enabled by EMM386.SYS (DOS_INSTALLED_EMS).
 * INT 67h dispatches at TRAP_SEG:EMS_ENTRY after the EMMXXXX0 device header.
 * The page frame occupies 9000h-9FFFh: four 16 KiB pages below 640 KiB,
 * because Itanium has no memory at C0000h-EFFFFh.
 *
 * Without paging, unmapping saves frame contents to native logical-page
 * storage and mapping copies them back. Mapping one logical page into two
 * frame pages creates independent copies.
 */
#include "vdm.h"
#define PAGE 0x4000U
#define PHYSICAL 4
#define FRAME_SEG 0x9000U
#define MAX_HANDLES 255
typedef struct {u8 **pages; u16 count,list; u8 used; char name[8];} Handle; /* list: pages of the page list */
typedef struct {u16 handle,page;} Mapping; /* handle 0xffff: none */
static Handle handles[MAX_HANDLES];
static Mapping map[PHYSICAL],saved[MAX_HANDLES][PHYSICAL]; static u8 has_saved[MAX_HANDLES];
static unsigned total_pages,free_pages;
static int present,os_disabled; static u32 access_key;
static u8 *frame(unsigned p) {return LINEAR((FRAME_SEG<<4)+p*PAGE);}

static void status(u8 ah) {wh(EAX,ah);}
static u8 *new_page(void) {
    void *p; if(io->alloc_pages(io->context,PAGE/4096,&p)) return NULL;
    memset(p,0,PAGE); return p;
}
/* The physical page's contents back to its logical page, and none mapped. */
static void unmap(unsigned p) {
    if(map[p].handle!=0xffff) memcpy(handles[map[p].handle].pages[map[p].page],frame(p),PAGE);
    map[p].handle=0xffff;
}
static void map_page(unsigned p,u16 h,u16 page) {
    if(map[p].handle==h && map[p].page==page) return;
    unmap(p);
    memcpy(frame(p),handles[h].pages[page],PAGE);
    map[p]=(Mapping){h,page};
}
static void flush_all(void) {for(unsigned p=0;p<PHYSICAL;p++) if(map[p].handle!=0xffff) memcpy(handles[map[p].handle].pages[map[p].page],frame(p),PAGE);}
static void reload_all(void) {for(unsigned p=0;p<PHYSICAL;p++) if(map[p].handle!=0xffff) memcpy(frame(p),handles[map[p].handle].pages[map[p].page],PAGE);}
static int valid(u16 h) {return h<MAX_HANDLES && handles[h].used;}
/* Pages for a handle, from the pool: none taken on failure. */
static int resize(u16 h,u16 count) {
    Handle *x=&handles[h];
    if(count>x->count) {
        if((unsigned)(count-x->count)>free_pages) return 0x88;
        u8 **list; void *mem; u16 pages=(u16)((count*sizeof(u8 *)+4095)/4096);
        if(io->alloc_pages(io->context,pages,&mem)) return 0x80;
        list=mem; for(u16 i=0;i<x->count;i++) list[i]=x->pages[i];
        for(u16 i=x->count;i<count;i++) if(!(list[i]=new_page())) {
            while(i>x->count) io->free_pages(io->context,list[--i],PAGE/4096);
            io->free_pages(io->context,list,pages); return 0x80;
        }
        if(x->pages) io->free_pages(io->context,x->pages,x->list);
        free_pages-=count-x->count; x->pages=list; x->list=pages; x->count=count;
    } else {
        for(unsigned p=0;p<PHYSICAL;p++) if(map[p].handle==h && map[p].page>=count) map[p].handle=0xffff;
        for(u16 i=count;i<x->count;i++) io->free_pages(io->context,x->pages[i],PAGE/4096);
        free_pages+=x->count-count; x->count=count;
    }
    return 0;
}
static int new_handle(u16 count,u16 *out) {
    unsigned h; for(h=1;h<MAX_HANDLES && handles[h].used;h++) {}
    if(h==MAX_HANDLES) return 0x85;
    if(count>total_pages) return 0x87;
    if(count>free_pages) return 0x88;
    handles[h]=(Handle){0}; handles[h].used=1;
    int e=resize((u16)h,count); if(e) {handles[h].used=0; return e;}
    *out=(u16)h; return 0;
}
static void free_handle(u16 h) {
    resize(h,0);
    if(handles[h].pages) io->free_pages(io->context,handles[h].pages,handles[h].list);
    if(h) handles[h]=(Handle){0}; else {handles[0].pages=NULL; handles[0].count=handles[0].list=0;}
    has_saved[h]=0;
}
/* A physical page from its number or (by segment) its segment. */
static int physical(u16 value,int by_segment,unsigned *p) {
    if(!by_segment) {if(value>=PHYSICAL) return 0x8b; *p=value; return 0;}
    if(value<FRAME_SEG || value>=FRAME_SEG+PHYSICAL*0x400 || (value-FRAME_SEG)%0x400) return 0x8b;
    *p=(value-FRAME_SEG)/0x400; return 0;
}
static int map_one(u16 h,u16 logical,unsigned p) {
    if(logical==0xffff) {unmap(p); return 0;}
    if(logical>=handles[h].count) return 0x8a;
    map_page(p,h,logical); return 0;
}
/* 4Eh/4Fh contexts: each physical page's segment, handle and logical page. */
static void get_context(u32 to,const u16 *segments,unsigned n) {
    for(unsigned i=0;i<n;i++) {
        unsigned p=(segments[i]-FRAME_SEG)/0x400;
        poke16(to+i*6,segments[i]); poke16(to+i*6+2,map[p].handle); poke16(to+i*6+4,map[p].page);
    }
}
static int set_context(u32 from,unsigned n) {
    for(unsigned i=0;i<n;i++) {
        u16 seg=peek16(from+i*6),h=peek16(from+i*6+2),page=peek16(from+i*6+4); unsigned p;
        if(physical(seg,1,&p)) return 0xa3;
        if(h==0xffff) unmap(p);
        else if(!valid(h) || page>=handles[h].count) return 0xa3;
        else map_page(p,h,page);
    }
    return 0;
}
static const u16 all_segments[PHYSICAL]={FRAME_SEG,FRAME_SEG+0x400,FRAME_SEG+0x800,FRAME_SEG+0xc00};
/* 57h: one side of a move, conventional memory (seg:off) or expanded
 * (handle, logical page and offset), from its 7 bytes at at. */
typedef struct {int expanded; u16 handle; u32 at;} Side;
static int side(u32 at,u32 length,Side *s) {
    u8 type=*LINEAR(at); u16 h=peek16(at+1),offset=peek16(at+3),segpage=peek16(at+5);
    if(type==0) {
        s->expanded=0; s->at=((u32)segpage<<4)+offset;
        return s->at+(u64)length>0xa0000?0xa2:0;
    }
    if(type!=1) return 0x98;
    if(!valid(h)) return 0x83;
    if(offset>=PAGE) return 0x95;
    if(segpage>=handles[h].count || segpage*(u64)PAGE+offset+length>handles[h].count*(u64)PAGE) return 0x8a;
    s->expanded=1; s->handle=h; s->at=segpage*PAGE+offset; return 0;
}
/* A byte of a side; expanded memory is its logical pages. */
static u8 *byte_at(const Side *s,u32 i) {
    if(!s->expanded) return LINEAR(s->at+i);
    u32 a=s->at+i; return handles[s->handle].pages[a/PAGE]+a%PAGE;
}
static int frame_overlap(const Side *s,u32 length) {
    return !s->expanded && s->at<(FRAME_SEG<<4)+PHYSICAL*PAGE && s->at+length>(FRAME_SEG<<4);
}
static void move_region(int exchange) {
    u32 m=((u32)sreg(SR_DS)<<4)+rw(ESI),length=peek32(m); Side src,dst; int e; u8 result=0;
    if(length>0x100000) {status(0x96); return;}
    if((e=side(m+4,length,&src)) || (e=side(m+11,length,&dst))) {status((u8)e); return;}
    int same=src.expanded==dst.expanded && (!src.expanded || src.handle==dst.handle);
    if(same && src.at<dst.at+length && dst.at<src.at+length) {
        if(exchange) {status(src.expanded?0x97:0x94); return;}
        result=0x92;
    }
    /* The frame's pages and their logical pages agree during the move. */
    flush_all();
    if(!exchange && result && src.at<dst.at) for(u32 i=length;i--;) *byte_at(&dst,i)=*byte_at(&src,i);
    else for(u32 i=0;i<length;i++) {
        u8 *f=byte_at(&src,i),*t=byte_at(&dst,i);
        if(exchange) {u8 x=*t; *t=*f; *f=x;} else *t=*f;
    }
    /* What the frame took goes to its logical pages, what logical pages
     * took to the frame. */
    if(frame_overlap(&dst,length) || (exchange && frame_overlap(&src,length))) flush_all();
    if(dst.expanded || (exchange && src.expanded)) reload_all();
    status(result);
}
/* 55h and 56h: the page map changed for a far jump or call; a call
 * returns through TRAP_SEG:EMS_RETURN, which maps the structure's old page
 * map (LIM 4.0 function 23: its length at +9, its address at +10). */
#define CALL_DEPTH 8
typedef struct {u16 handle; u32 old_map; u8 old_count,by_segment;} Call;
static Call calls[CALL_DEPTH]; static unsigned call_depth;
static int map_list(u16 h,u32 at,unsigned n,int by_segment) {
    unsigned p; int e;
    for(unsigned i=0;i<n;i++) {
        u16 logical=peek16(at+i*4),where=peek16(at+i*4+2);
        if((e=physical(where,by_segment,&p)) || (e=map_one(h,logical,p))) return e;
    }
    return 0;
}
static void alter_map(int call) {
    u8 al=rl(EAX); u16 h=rw(EDX); u32 m=((u32)sreg(SR_DS)<<4)+rw(ESI); int e;
    if(call && al==2) {ww(EBX,10); status(0); return;}
    if(al>1) {status(0x8f); return;}
    if(!valid(h)) {status(0x83); return;}
    if(call && call_depth==CALL_DEPTH) {status(0x80); return;}
    u32 target=peek32(m),new_map=peek32(m+5),old_map=peek32(m+10);
    if(call) calls[call_depth]=(Call){h,((old_map>>16)<<4)+(u16)old_map,*LINEAR(m+9),al};
    if((e=map_list(h,((new_map>>16)<<4)+(u16)new_map,*LINEAR(m+4),al))) {status((u8)e); return;}
    /* The interrupt's frame (the caller's IP, CS and flags) is on the
     * stack: a jump returns to the target instead; a call puts above it
     * a return to EMS_RETURN and a frame for the target. */
    u32 ss=(u32)sreg(SR_SS)<<4; u16 sp=rw(ESP),flags=peek16(ss+(u16)(sp+4));
    if(call) {
        sp=(u16)(sp-10); ww(ESP,sp);
        poke16(ss+(u16)(sp+4),flags); poke16(ss+(u16)(sp+6),EMS_RETURN); poke16(ss+(u16)(sp+8),TRAP_SEG);
        call_depth++;
    }
    poke16(ss+sp,(u16)target); poke16(ss+(u16)(sp+2),(u16)(target>>16));
    status(0);
}
/* A call made by 56h returned: the old page map it named. */
void ems_return(void) {
    if(!call_depth) {status(0x80); return;}
    const Call *c=&calls[--call_depth];
    if(!valid(c->handle)) {status(0x83); return;}
    status((u8)map_list(c->handle,c->old_map,c->old_count,c->by_segment));
}
void ems_call(void) {
    u8 ah=rh(EAX),al=rl(EAX); u16 dx=rw(EDX),bx=rw(EBX); int e; unsigned p;
    if(!present) {status(0x80); return;}
    switch(ah) {
    case 0x40: status(0); return;
    case 0x41: ww(EBX,FRAME_SEG); status(0); return;
    case 0x42: ww(EBX,(u16)free_pages); ww(EDX,(u16)total_pages); status(0); return;
    case 0x43: {
        u16 h; if(!bx) {status(0x89); return;}
        e=new_handle(bx,&h); if(e) {status((u8)e); return;}
        ww(EDX,h); status(0); return;
    }
    case 0x44:
        if(!valid(dx)) {status(0x83); return;}
        if(al>=PHYSICAL) {status(0x8b); return;}
        status((u8)map_one(dx,bx,al)); return;
    case 0x45:
        if(!valid(dx)) {status(0x83); return;}
        if(has_saved[dx]) {status(0x86); return;}
        free_handle(dx); status(0); return;
    case 0x46: wl(EAX,0x40); status(0); return;
    case 0x47:
        if(!valid(dx)) {status(0x83); return;}
        if(has_saved[dx]) {status(0x8d); return;}
        memcpy(saved[dx],map,sizeof(map)); has_saved[dx]=1; status(0); return;
    case 0x48:
        if(!valid(dx)) {status(0x83); return;}
        if(!has_saved[dx]) {status(0x8e); return;}
        for(p=0;p<PHYSICAL;p++) {if(saved[dx][p].handle==0xffff) unmap(p); else if(valid(saved[dx][p].handle)) map_page(p,saved[dx][p].handle,saved[dx][p].page);}
        has_saved[dx]=0; status(0); return;
    case 0x4b: {
        unsigned n=0; for(unsigned h=0;h<MAX_HANDLES;h++) n+=handles[h].used;
        ww(EBX,(u16)n); status(0); return;
    }
    case 0x4c: if(!valid(dx)) {status(0x83); return;} ww(EBX,handles[dx].count); status(0); return;
    case 0x4d: {
        u32 to=((u32)sreg(SR_ES)<<4)+rw(EDI); unsigned n=0;
        for(unsigned h=0;h<MAX_HANDLES;h++) if(handles[h].used) {poke16(to+n*4,(u16)h); poke16(to+n*4+2,handles[h].count); n++;}
        ww(EBX,(u16)n); status(0); return;
    }
    case 0x4e: {
        u32 to=((u32)sreg(SR_ES)<<4)+rw(EDI),from=((u32)sreg(SR_DS)<<4)+rw(ESI);
        if(al==3) {wl(EAX,PHYSICAL*6); status(0); return;}
        if(al>3) {status(0x8f); return;}
        if(al==0 || al==2) get_context(to,all_segments,PHYSICAL);
        if(al==1 || al==2) {e=set_context(from,PHYSICAL); if(e) {status((u8)e); return;}}
        status(0); return;
    }
    case 0x4f: {
        u32 from=((u32)sreg(SR_DS)<<4)+rw(ESI),to=((u32)sreg(SR_ES)<<4)+rw(EDI);
        if(al==2) {if(bx>PHYSICAL) {status(0x8b); return;} wl(EAX,(u8)(2+bx*6)); status(0); return;}
        if(al==0) {
            u16 n=peek16(from),segments[PHYSICAL];
            if(n>PHYSICAL) {status(0x8b); return;}
            for(u16 i=0;i<n;i++) {segments[i]=peek16(from+2+i*2); if(physical(segments[i],1,&p)) {status(0x8b); return;}}
            poke16(to,n); get_context(to+2,segments,n); status(0); return;
        }
        if(al==1) {u16 n=peek16(from); if(n>PHYSICAL) {status(0xa3); return;} status((u8)set_context(from+2,n)); return;}
        status(0x8f); return;
    }
    case 0x50: {
        u32 from=((u32)sreg(SR_DS)<<4)+rw(ESI); u16 n=rw(ECX);
        if(al>1) {status(0x8f); return;}
        if(!valid(dx)) {status(0x83); return;}
        status((u8)map_list(dx,from,n,al==1)); return;
    }
    case 0x51:
        if(!valid(dx)) {status(0x83); return;}
        if(bx>total_pages) {status(0x87); return;}
        e=resize(dx,bx); if(e) {status((u8)e); return;}
        ww(EBX,handles[dx].count); status(0); return;
    case 0x52:
        if(al!=2 && !valid(dx)) {status(0x83); return;}
        if(al==0 || al==2) {wl(EAX,0); status(0); return;}
        if(al==1) {status(rl(EBX)?0x91:0); return;}
        status(0x8f); return;
    case 0x53: {
        if(!valid(dx)) {status(0x83); return;}
        if(al==0) {memcpy(LIN(sreg(SR_ES),rw(EDI)),handles[dx].name,8); status(0); return;}
        if(al==1) {
            const u8 *name=LIN(sreg(SR_DS),rw(ESI)); int blank=1;
            for(unsigned i=0;i<8;i++) if(name[i]) blank=0;
            if(!blank) for(unsigned h=0;h<MAX_HANDLES;h++) if(handles[h].used && h!=dx && !memcmp(handles[h].name,name,8)) {status(0xa1); return;}
            memcpy(handles[dx].name,name,8); status(0); return;
        }
        status(0x8f); return;
    }
    case 0x54: {
        if(al==0) {
            u32 to=((u32)sreg(SR_ES)<<4)+rw(EDI); unsigned n=0;
            for(unsigned h=0;h<MAX_HANDLES;h++) if(handles[h].used) {poke16(to+n*10,(u16)h); memcpy(LINEAR(to+n*10+2),handles[h].name,8); n++;}
            wl(EAX,(u8)n); status(0); return;
        }
        if(al==1) {
            const u8 *name=LIN(sreg(SR_DS),rw(ESI)); int blank=1;
            for(unsigned i=0;i<8;i++) if(name[i]) blank=0;
            if(blank) {status(0xa1); return;}
            for(unsigned h=0;h<MAX_HANDLES;h++) if(handles[h].used && !memcmp(handles[h].name,name,8)) {ww(EDX,(u16)h); status(0); return;}
            status(0xa0); return;
        }
        if(al==2) {ww(EBX,MAX_HANDLES); status(0); return;}
        status(0x8f); return;
    }
    case 0x55: alter_map(0); return;
    case 0x56: alter_map(1); return;
    case 0x57: if(al>1) {status(0x8f); return;} move_region(al==1); return;
    case 0x58:
        if(al==0) {
            u32 to=((u32)sreg(SR_ES)<<4)+rw(EDI);
            for(unsigned i=0;i<PHYSICAL;i++) {poke16(to+i*4,all_segments[i]); poke16(to+i*4+2,(u16)i);}
        } else if(al!=1) {status(0x8f); return;}
        ww(ECX,PHYSICAL); status(0); return;
    case 0x59:
        if(os_disabled) {status(0xa4); return;}
        if(al==0) {
            u32 to=((u32)sreg(SR_ES)<<4)+rw(EDI);
            poke16(to,PAGE/16); poke16(to+2,0); poke16(to+4,PHYSICAL*6); poke16(to+6,0); poke16(to+8,0);
            status(0); return;
        }
        if(al==1) {ww(EBX,(u16)free_pages); ww(EDX,(u16)total_pages); status(0); return;}
        status(0x8f); return;
    case 0x5a: {
        u16 h; if(al>1) {status(0x8f); return;}
        e=new_handle(bx,&h); if(e) {status((u8)e); return;}
        ww(EDX,h); status(0); return;
    }
    case 0x5b:
        if(os_disabled) {status(0xa4); return;}
        switch(al) {
        case 0: wl(EBX,0); set_sreg(SR_ES,0); ww(EDI,0); status(0); return;
        case 1: if(rl(EBX)) {status(0x9c); return;} status(0); return;
        case 2: ww(EDX,PHYSICAL*6); status(0); return;
        case 3: case 5: wl(EBX,0); status(0); return;
        case 4: case 6: case 7: case 8: status(rl(EBX)?0x9c:0); return;
        default: status(0x8f); return;
        }
    case 0x5c: status(0); return;
    case 0x5d: {
        u32 key=(u32)rw(EBX)<<16|rw(ECX);
        if(al>2) {status(0x8f); return;}
        if(access_key && key!=access_key) {status(0xa4); return;}
        if(!access_key) {access_key=0x4d454d53U^(u32)io->ticks_ms(io->context); if(!access_key) access_key=1; ww(EBX,(u16)(access_key>>16)); ww(ECX,(u16)access_key);}
        if(al==0) os_disabled=0; else if(al==1) os_disabled=1; else {os_disabled=0; access_key=0;}
        status(0); return;
    }
    default: status(0x84); return;
    }
}
/* At the machine's start: the pool, handle 0 with no pages, nothing
 * mapped, the frame taken from conventional memory. */
void ems_init(void) {
    u32 v=0;
    if(dos_installed(DOS_INSTALLED_EMS,NULL,&v) || !(v&0xffff)) return;
    total_pages=free_pages=(v&0xffff)/16;
    for(unsigned p=0;p<PHYSICAL;p++) map[p].handle=0xffff;
    handles[0].used=1; present=1;
}
int ems_present(void) {return present;}
/* "EMMXXXX0" as a file name, which opens the driver's device. */
int ems_device(const char *path) {
    const char *name=path;
    for(const char *p=path;*p;p++) if(*p=='\\' || *p=='/' || *p==':') name=p+1;
    for(unsigned i=0;i<8;i++) if(upper(name[i])!="EMMXXXX0"[i]) return 0;
    return present && (!name[8] || name[8]=='.');
}
u16 ems_frame(void) {return present?FRAME_SEG:0;}
void ems_close(void) {
    if(!present) return;
    for(unsigned h=0;h<MAX_HANDLES;h++) if(handles[h].used) free_handle((u16)h);
    present=0;
}
