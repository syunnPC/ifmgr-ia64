/* SPDX-License-Identifier: GPL-2.0-or-later
 * XMS 3.0, enabled by HIMEM.SYS (DOS_INSTALLED_XMS).
 * INT 2Fh 4300h detects it; 4310h returns TRAP_SEG:XMS_ENTRY. The entry's
 * short jump and three NOPs allow hooks before its location-specific INT 67h.
 *
 * Extended blocks use alloc_pages_range below 4 GiB so IA-32 can access
 * locked addresses. XMS and DPMI share EXTENDED_LIMIT_KB (16 MiB).
 * IO_CAP_HMA exposes 64 KiB at 100000h. A20 is always enabled; disabling
 * returns 94h. Upper memory blocks are unavailable.
 */
#include "vdm.h"
#define LOW (0x110000ULL)
#define HIGH (0x100000000ULL)
typedef struct {void *memory; u32 kb,pages; u8 locks,used;} Block;
static Block *blocks; static unsigned handles,hma_min_kb;
static int hma_used;
static u32 total_kb(void) {u32 t=0; for(unsigned i=0;i<handles;i++) if(blocks[i].used) t+=blocks[i].kb; return t;}
u32 xms_used_kb(void) {return blocks?total_kb():0;}
int xms_owns(u32 linear,u32 length) {
    for(unsigned i=0;blocks && i<handles;i++) if(blocks[i].used && blocks[i].memory) {
        u32 b=(u32)(uintptr_t)blocks[i].memory;
        if(linear>=b && (u64)linear+length<=(u64)b+blocks[i].pages*4096ULL) return 1;
    }
    return 0;
}
/* Free kilobytes: what IO.SYS has below 4 GiB, within what VDM's programs
 * may have in all (with DPMI's). */
static void free_kb(u32 *largest,u32 *total) {
    u64 pages=0,run=0; u32 used=total_kb()+dpmi_used_kb(),room=used<EXTENDED_LIMIT_KB?EXTENDED_LIMIT_KB-used:0;
    if(io->range_free(io->context,LOW,HIGH,&pages,&run)) pages=run=0;
    u64 l=run*4,t=pages*4;
    *largest=(u32)(l<room?l:room); *total=(u32)(t<room?t:room);
}
static int valid(u16 h) {return h && h<=handles && blocks[h-1].used;}
static int grow(Block *b,u32 kb) {
    u32 pages=(kb+3)/4; void *memory=NULL;
    if(pages && io->alloc_pages_range(io->context,pages,LOW,HIGH,&memory)) return 0;
    if(memory && b->memory) memcpy(memory,b->memory,(b->kb<kb?b->kb:kb)*1024ULL);
    if(b->memory) io->free_pages(io->context,b->memory,b->pages);
    b->memory=memory; b->pages=pages; b->kb=kb; return 1;
}
static void answer(u16 ax,u8 bl) {ww(EAX,ax); wl(EBX,bl);}
static void error(u8 bl) {answer(0,bl);}
/* A place of a move: a block and offset, or (handle 0) seg:off in the
 * program's own memory. */
static u8 *place(u16 h,u32 offset,u32 length) {
    if(!h) {
        u32 at=((offset>>16)<<4)+(offset&0xffff);
        if(at+(u64)length>0xa0000 || (at+length>(u32)HOLE_START<<4 && at<(u32)HOLE_END<<4)) return NULL;
        return LINEAR(at);
    }
    if(!valid(h) || offset+(u64)length>blocks[h-1].kb*1024ULL) return NULL;
    return (u8 *)blocks[h-1].memory+offset;
}
/* 0Bh: the length, then the source's handle and offset, then the
 * destination's, at DS:SI. */
static void move(void) {
    u32 m=((u32)sreg(SR_DS)<<4)+rw(ESI);
    u32 length=peek32(m),so=peek32(m+6),dof=peek32(m+12); u16 sh=peek16(m+4),dh=peek16(m+10);
    if(length&1) {error(0xa7); return;}
    if(sh && !valid(sh)) {error(0xa3); return;}
    if(dh && !valid(dh)) {error(0xa5); return;}
    u8 *from=place(sh,so,length),*to=place(dh,dof,length);
    if(!from) {error(0xa4); return;}
    if(!to) {error(0xa6); return;}
    if(length) memmove(to,from,length);
    answer(1,0);
}
void xms_call(void) {
    u8 ah=rh(EAX); u16 dx=rw(EDX);
    if(!blocks) {error(0x80); return;}
    switch(ah) {
    case 0x00: ww(EAX,0x0300); ww(EBX,0x0300); ww(EDX,(io->capabilities&IO_CAP_HMA)?1:0); return;
    case 0x01:
        if(!(io->capabilities&IO_CAP_HMA)) error(0x90);
        else if(hma_used) error(0x91);
        else if(dx!=0xffff && dx<hma_min_kb*1024U) error(0x92);
        else {hma_used=1; answer(1,0);}
        return;
    case 0x02:
        if(!(io->capabilities&IO_CAP_HMA)) error(0x90);
        else if(!hma_used) error(0x93);
        else {hma_used=0; answer(1,0);}
        return;
    case 0x03: answer(1,0); return;
    case 0x05: answer(1,0); return;
    case 0x04: error(0x94); return;
    case 0x06: error(0x94); return;
    case 0x07: answer(1,0); return;
    case 0x08: case 0x88: {
        u32 largest,total; free_kb(&largest,&total);
        if(ah==0x08) {ww(EAX,(u16)(largest>0xffff?0xffff:largest)); ww(EDX,(u16)(total>0xffff?0xffff:total));}
        /* ECX: the highest address a block may end at, IO.SYS's pages being anywhere below 4 GiB. */
        else {cpu.gr[EAX]=largest; cpu.gr[EDX]=total; cpu.gr[ECX]=0xffffffffU;}
        wl(EBX,largest?0:0xa0);
        return;
    }
    case 0x09: case 0x89: {
        u32 kb=ah==0x09?dx:(u32)cpu.gr[EDX],largest,total; unsigned i;
        for(i=0;i<handles && blocks[i].used;i++) {}
        if(i==handles) {error(0xa1); return;}
        free_kb(&largest,&total);
        if(kb>largest) {error(0xa0); return;}
        blocks[i]=(Block){0}; blocks[i].used=1;
        if(!grow(&blocks[i],kb)) {blocks[i].used=0; error(0xa0); return;}
        answer(1,0); ww(EDX,(u16)(i+1));
        return;
    }
    case 0x0a:
        if(!valid(dx)) error(0xa2);
        else if(blocks[dx-1].locks) error(0xab);
        else {Block *b=&blocks[dx-1]; if(b->memory) io->free_pages(io->context,b->memory,b->pages); *b=(Block){0}; answer(1,0);}
        return;
    case 0x0b: move(); return;
    case 0x0c:
        if(!valid(dx)) {error(0xa2); return;}
        if(blocks[dx-1].locks==255) {error(0xac); return;}
        blocks[dx-1].locks++;
        {u32 a=(u32)(uintptr_t)blocks[dx-1].memory; answer(1,0); ww(EDX,(u16)(a>>16)); ww(EBX,(u16)a);}
        return;
    case 0x0d:
        if(!valid(dx)) error(0xa2);
        else if(!blocks[dx-1].locks) error(0xaa);
        else {blocks[dx-1].locks--; answer(1,0);}
        return;
    case 0x0e: case 0x8e: {
        if(!valid(dx)) {error(0xa2); return;}
        unsigned freeh=0; for(unsigned i=0;i<handles;i++) if(!blocks[i].used) freeh++;
        ww(EAX,1); wh(EBX,blocks[dx-1].locks);
        if(ah==0x0e) {wl(EBX,(u8)(freeh>255?255:freeh)); ww(EDX,(u16)(blocks[dx-1].kb>0xffff?0xffff:blocks[dx-1].kb));}
        else {ww(ECX,(u16)freeh); cpu.gr[EDX]=blocks[dx-1].kb;}
        return;
    }
    case 0x0f: case 0x8f: {
        u32 kb=ah==0x0f?rw(EBX):(u32)cpu.gr[EBX],largest,total;
        if(!valid(dx)) {error(0xa2); return;}
        Block *b=&blocks[dx-1];
        if(b->locks) {error(0xab); return;}
        free_kb(&largest,&total);
        if(kb>b->kb && kb-b->kb>total) {error(0xa0); return;}
        if(!grow(b,kb)) {error(0xa0); return;}
        answer(1,0);
        return;
    }
    case 0x10: case 0x11: case 0x12: error(0x80); return;
    default: error(0x80);
    }
}
/* At the machine's start: the driver there or not. */
void xms_init(void) {
    u32 v=0;
    if(dos_installed(DOS_INSTALLED_XMS,NULL,&v) || !(v&DOS_XMS_ON) ||
       io->size<offsetof(IoServices,range_free)+sizeof(io->range_free) || !io->alloc_pages_range) return;
    handles=(v>>16)&0xffff; if(!handles) handles=32;
    hma_min_kb=(v>>8)&0xff;
    void *table;
    if(io->alloc_pages(io->context,(u32)((handles*sizeof(Block)+4095)/4096),&table)) {handles=0; return;}
    blocks=table; memset(blocks,0,handles*sizeof(Block));
}
int xms_present(void) {return blocks!=NULL;}
/* At the machine's end: every block back to IO.SYS. */
void xms_close(void) {
    if(!blocks) return;
    for(unsigned i=0;i<handles;i++) if(blocks[i].used && blocks[i].memory) io->free_pages(io->context,blocks[i].memory,blocks[i].pages);
    io->free_pages(io->context,blocks,(u32)((handles*sizeof(Block)+4095)/4096));
    blocks=NULL; handles=0;
}
