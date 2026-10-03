/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: the local descriptor table shared by all Win16 tasks, kept in
 * the IA-32 memory format that the processor reads when a program loads a
 * segment register. Index 0 is never handed out. Segments are 16-bit; a
 * limit above 1 MiB uses page granularity.
 */
#include "wow.h"
#define ENTRIES 8192U
static BYTE *ldt,*gdt;
static BYTE used[ENTRIES];
wh_u64 ldt_descriptor,gdt_descriptor;

void *Alloc16(DWORD bytes) {
    void *p;
    if(!bytes) bytes=1;
    p=wh_alloc_low(bytes);
    if(p && ((ULONG_PTR)p>>32 || ((ULONG_PTR)p+bytes)>>32)) {wh_free(p,bytes); p=NULL;}
    return p;
}
void Free16(void *p,DWORD bytes) {if(p) wh_free(p,bytes?bytes:1);}
DWORD SelLinear(void *p) {return (ULONG_PTR)p>>32?0:(DWORD)(ULONG_PTR)p;}

BOOL LdtInit(void) {
    if(ldt) return TRUE;
    ldt=(BYTE *)Alloc16(ENTRIES*8); gdt=(BYTE *)Alloc16(4096);
    if(!ldt || !gdt) return FALSE;
    /* Register format: base, limit, type 2 (LDT) or none, present. */
    ldt_descriptor=SelLinear(ldt)|(wh_u64)(ENTRIES*8-1)<<32|(wh_u64)0x82<<52;
    gdt_descriptor=SelLinear(gdt)|(wh_u64)15<<32|(wh_u64)0x80<<52;
    used[0]=1;
    return TRUE;
}
void LdtShutdown(void) {
    Free16(ldt,ENTRIES*8); Free16(gdt,4096);
    ldt=gdt=NULL; memset(used,0,sizeof(used));
}
static BYTE *entry(WORD sel) {return ldt+(sel&~7U);}
static BOOL index_ok(WORD sel) {return (sel&7)==7 && (sel>>3)<ENTRIES && used[sel>>3];}
BOOL SelValid(WORD sel) {return ldt && index_ok(sel);}
void SelSet(WORD sel,DWORD base,DWORD limit,BYTE access) {
    BYTE *e=entry(sel),flags=0;
    if(limit>0xfffff) {limit>>=12; flags|=0x80;}
    e[0]=(BYTE)limit; e[1]=(BYTE)(limit>>8);
    e[2]=(BYTE)base; e[3]=(BYTE)(base>>8); e[4]=(BYTE)(base>>16);
    e[5]=access; e[6]=(BYTE)(flags|((limit>>16)&15)); e[7]=(BYTE)(base>>24);
}
WORD SelAlloc(DWORD base,DWORD limit,BYTE access) {
    unsigned i;
    if(!ldt) return 0;
    for(i=1;i<ENTRIES;i++) if(!used[i]) {
        WORD sel=(WORD)(i*8+7);
        used[i]=1; SelSet(sel,base,limit,access); return sel;
    }
    return 0;
}
static unsigned tiles(DWORD size) {return size?(unsigned)((size+0xffff)>>16):1;}
void SelSetTiled(WORD sel,DWORD base,DWORD size,BYTE access) {
    unsigned i,n=tiles(size);
    for(i=0;i<n;i++) {
        DWORD left=size-i*0x10000;
        SelSet((WORD)(sel+i*8),base+i*0x10000,(left>0x10000?0x10000:left?left:1)-1,access);
    }
}
WORD SelAllocTiled(DWORD base,DWORD size,BYTE access) {
    unsigned i,k,n=tiles(size);
    if(!ldt) return 0;
    for(i=1;i+n<=ENTRIES;i++) {
        for(k=0;k<n && !used[i+k];k++) {}
        if(k==n) {
            WORD sel=(WORD)(i*8+7);
            for(k=0;k<n;k++) used[i+k]=1;
            SelSetTiled(sel,base,size,access);
            return sel;
        }
        i+=k;
    }
    return 0;
}
void SelFreeTiled(WORD sel,DWORD size) {
    unsigned i,n=tiles(size);
    for(i=0;i<n;i++) SelFree((WORD)(sel+i*8));
}
void SelFree(WORD sel) {
    if(!SelValid(sel)) return;
    memset(entry(sel),0,8); used[sel>>3]=0;
}
DWORD SelBase(WORD sel) {
    const BYTE *e=entry(sel);
    return e[2]|(DWORD)e[3]<<8|(DWORD)e[4]<<16|(DWORD)e[7]<<24;
}
DWORD SelLimit(WORD sel) {
    const BYTE *e=entry(sel); DWORD limit=e[0]|(DWORD)e[1]<<8|(DWORD)(e[6]&15)<<16;
    return e[6]&0x80?limit<<12|0xfff:limit;
}
/* The register format keeps the access byte at 52-59 and G, D/B, L, AVL
 * at 60-63, in the memory format's order. */
wh_u64 SelDescriptor(WORD sel) {
    const BYTE *e;
    if(!SelValid(sel)) return 0;
    e=entry(sel);
    return SelBase(sel)|(wh_u64)(e[0]|(DWORD)e[1]<<8|(DWORD)(e[6]&15)<<16)<<32|
           (wh_u64)e[5]<<52|(wh_u64)(e[6]>>4)<<60;
}
BOOL SelIsCode(WORD sel) {return SelValid(sel) && (entry(sel)[5]&8)!=0;}
void *SelPointer(WORD sel,WORD offset) {return (void *)(ULONG_PTR)(SelBase(sel)+offset);}
void *Lin16(DWORD segptr) {
    WORD sel=HIWORD(segptr);
    if(!segptr || !SelValid(sel)) return NULL;
    return SelPointer(sel,LOWORD(segptr));
}
WORD Peek16(WORD sel,WORD off) {return get16((const BYTE *)SelPointer(sel,off));}
