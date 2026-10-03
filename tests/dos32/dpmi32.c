/* SPDX-License-Identifier: GPL-2.0-or-later
 * DPMI32: a 32-bit DOS program in the flat model for the DOS extenders
 * Open Watcom binds (DOS/32A, PMODE/W, CauseWay, DOS/4GW), each a DPMI
 * client of VDM's host. It writes through the C library (INT 21h, which the
 * extender carries to real mode), uses several MiB, writes and reads a
 * file, asks DPMI for its version, calls real-mode DOS through 0300h and
 * waits for the BIOS tick count to move. Each check prints "DPMI32: <name>
 * ok" or "FAILED"; the exit code is the number of failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <i86.h>

typedef struct {
    unsigned long edi,esi,ebp,reserved,ebx,edx,ecx,eax;
    unsigned short flags,es,ds,fs,gs,ip,cs,sp,ss;
} RealRegs;

static int failures;
static void check(const char *name,int good) {
    printf("DPMI32: %s %s\n",name,good?"ok":"FAILED");
    if(!good) failures++;
}

int main(void) {
    union REGS r; struct SREGS s; RealRegs rm;
    unsigned char *p,*q; unsigned long i,n=4ul<<20; int good; FILE *f;
    volatile unsigned long *ticks=(volatile unsigned long *)0x46c; unsigned long start,spins;

    memset(&r,0,sizeof r); r.w.ax=0x0400; int386(0x31,&r,&r);
    check("DPMI version",!r.x.cflag && r.h.ah==0 && r.h.al==0x5a);

    p=(unsigned char *)malloc(n); good=p!=NULL;
    if(p) {
        for(i=0;i<n;i++) p[i]=(unsigned char)(i*7+(i>>12));
        for(i=0;i<n && good;i++) good=p[i]==(unsigned char)(i*7+(i>>12));
    }
    check("4 MiB of memory",good);

    good=0;
    if(p && (f=fopen("DPMI32.TMP","wb"))!=NULL) {
        good=fwrite(p,1,100000,f)==100000; good&=fclose(f)==0;
        q=(unsigned char *)malloc(100000);
        if(good && q && (f=fopen("DPMI32.TMP","rb"))!=NULL) {
            good=fread(q,1,100000,f)==100000 && !memcmp(p,q,100000); fclose(f);
        } else good=0;
        free(q); remove("DPMI32.TMP");
    }
    check("file write and read",good);
    free(p);

    /* INT 21h AH=30h in real mode through the host. */
    memset(&rm,0,sizeof rm); rm.eax=0x3000;
    segread(&s); memset(&r,0,sizeof r);
    r.x.eax=0x0300; r.x.ebx=0x21; r.x.ecx=0; r.x.edi=(unsigned)&rm;
    int386x(0x31,&r,&r,&s);
    check("real-mode call",!r.x.cflag && (rm.eax&0xff)==4);

    start=*ticks;
    for(spins=0;*ticks-start<2 && spins<400000000ul;spins++) {}
    check("timer ticks",*ticks-start>=2);
    return failures;
}
