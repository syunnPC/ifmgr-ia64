/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: INT 21h (and DOS3Call) for Win16 programs, which the C run-time
 * libraries call directly. Far pointers are selector:offset; the calls run
 * in the task's own DOS context with native pointers, so handles are the
 * task's DOS handles. Directory searches keep their native record here,
 * indexed from the program's DTA, which gets the DOS layout.
 */
#include "api.h"
#define SEARCHES 16
static struct {BYTE record[44]; BOOL used;} searches[SEARCHES];
static unsigned next_search;

static DWORD far_ptr(Task16 *t,int seg,int reg) {return (DWORD)Seg16(t,seg)<<16|Reg16(t,reg);}
static wh_u64 native(Task16 *t,int seg,int reg) {return (wh_u64)(ULONG_PTR)Lin16(far_ptr(t,seg,reg));}
static int call(WhRegs *r) {wh_int21(r); return (r->flags&1)?(int)r->ax:0;}
static void result(Task16 *t,int e) {
    if(e) {t->cpu.eflags|=1; SetReg16(t,AX,(WORD)e);} else t->cpu.eflags&=~(wh_u64)1;
}
static void search(Task16 *t,BOOL first) {
    BYTE *d=(BYTE *)Lin16(t->dta); unsigned slot; WhRegs r; int e;
    const BYTE *f;
    if(!d) {result(t,18); return;}
    if(first) {slot=next_search++%SEARCHES; memset(searches[slot].record,0,44);}
    else {
        if(d[1]!='W' || d[2]!='O' || d[3]!='W' || d[0]>=SEARCHES) {result(t,18); return;}
        slot=d[0];
    }
    memset(&r,0,sizeof(r)); r.ax=0x1a01; r.dx=(wh_u64)(ULONG_PTR)searches[slot].record; r.cx=44; call(&r);
    memset(&r,0,sizeof(r)); r.ax=first?0x4e00:0x4f00; r.cx=Reg16(t,CX); r.dx=first?native(t,3,DX):0;
    e=call(&r);
    if(e) {result(t,e); return;}
    /* Native record: attr 18, time 20, date 22, size 24, name 28. */
    f=searches[slot].record;
    memset(d,0,43); d[0]=(BYTE)slot; d[1]='W'; d[2]='O'; d[3]='W';
    d[21]=f[18]; memcpy(d+22,f+20,4); memcpy(d+26,f+24,4); memcpy(d+30,f+28,13); d[42]=0;
    result(t,0);
}
void Int21(Task16 *t) {
    BYTE ah=(BYTE)(Reg16(t,AX)>>8),al=(BYTE)Reg16(t,AX); WhRegs r; int e;
    memset(&r,0,sizeof(r));
    r.ax=Reg16(t,AX); r.bx=Reg16(t,BX); r.cx=Reg16(t,CX); r.dx=Reg16(t,DX);
    switch(ah) {
    case 0x4c: TaskEnd16(t,al); return;
    case 0x30: SetReg16(t,AX,4); SetReg16(t,BX,0xff00); SetReg16(t,CX,0); return;
    case 0x0e: r.dx=(BYTE)Reg16(t,DX); call(&r); SetReg16(t,AX,(WORD)((Reg16(t,AX)&0xff00)|(BYTE)r.ax)); return;
    case 0x19: call(&r); SetReg16(t,AX,(WORD)((Reg16(t,AX)&0xff00)|(BYTE)r.ax)); return;
    case 0x1a: t->dta=far_ptr(t,3,DX); return;
    case 0x2f: SetSeg16(t,0,HIWORD(t->dta)); SetReg16(t,BX,LOWORD(t->dta)); return;
    case 0x2a: case 0x2c: call(&r); SetReg16(t,AX,(WORD)r.ax); SetReg16(t,CX,(WORD)r.cx); SetReg16(t,DX,(WORD)r.dx); return;
    case 0x2b: case 0x2d: call(&r); SetReg16(t,AX,(WORD)((Reg16(t,AX)&0xff00)|(BYTE)r.ax)); return;
    case 0x36: {
        r.dx=(BYTE)Reg16(t,DX); wh_int21(&r);
        if((r.flags&1) || (WORD)r.ax==0xffff) {SetReg16(t,AX,0xffff); return;}
        SetReg16(t,AX,(WORD)r.ax); SetReg16(t,BX,r.bx>0xffff?0xffff:(WORD)r.bx);
        SetReg16(t,CX,(WORD)r.cx); SetReg16(t,DX,r.dx>0xffff?0xffff:(WORD)r.dx);
        return;
    }
    case 0x39: case 0x3a: case 0x3b: case 0x41: r.dx=native(t,3,DX); result(t,call(&r)); return;
    case 0x3c: case 0x3d: case 0x5a: case 0x5b:
        r.dx=native(t,3,DX); e=call(&r); if(!e) SetReg16(t,AX,(WORD)r.ax); result(t,e); return;
    case 0x6c:
        r.si=native(t,3,SI); r.bx=Reg16(t,BX); e=call(&r);
        if(!e) {SetReg16(t,AX,(WORD)r.ax); SetReg16(t,CX,(WORD)r.cx);}
        result(t,e); return;
    case 0x3e: case 0x45: case 0x68: e=call(&r); if(!e && ah==0x45) SetReg16(t,AX,(WORD)r.ax); result(t,e); return;
    case 0x46: e=call(&r); result(t,e); return;
    case 0x3f: case 0x40:
        r.dx=native(t,3,DX); e=call(&r); SetReg16(t,AX,(WORD)r.ax); result(t,e); return;
    case 0x42:
        r.dx=(wh_u64)(wh_i64)(LONG)((DWORD)Reg16(t,CX)<<16|Reg16(t,DX)); e=call(&r);
        if(!e) {SetReg16(t,AX,(WORD)r.ax); SetReg16(t,DX,(WORD)(r.ax>>16));}
        result(t,e); return;
    case 0x43: r.dx=native(t,3,DX); e=call(&r); if(!e) SetReg16(t,CX,(WORD)r.cx); result(t,e); return;
    case 0x44:
        if(al<=1 || al==6 || al==7 || al==8 || al==9 || al==0x0a) {
            e=call(&r);
            if(!e) {
                if(al==0 || al==9 || al==0x0a) SetReg16(t,DX,(WORD)r.dx);
                else if(al==6 || al==7) SetReg16(t,AX,(WORD)((Reg16(t,AX)&0xff00)|(BYTE)r.ax));
                else if(al==8) SetReg16(t,AX,(WORD)r.ax);
            }
            result(t,e);
        } else result(t,1);
        return;
    case 0x47: {
        char cwd[128]; r.si=(wh_u64)(ULONG_PTR)cwd; e=call(&r);
        if(!e) lstrcpyn((char *)Lin16(far_ptr(t,3,SI)),cwd,64);
        result(t,e); return;
    }
    case 0x4e: search(t,TRUE); return;
    case 0x4f: search(t,FALSE); return;
    case 0x56: r.dx=native(t,3,DX); r.di=native(t,0,DI); result(t,call(&r)); return;
    case 0x57: e=call(&r); if(!e && al==0) {SetReg16(t,CX,(WORD)r.cx); SetReg16(t,DX,(WORD)r.dx);} result(t,e); return;
    case 0x59: r.bx=0; call(&r); SetReg16(t,AX,(WORD)r.ax); SetReg16(t,BX,(WORD)r.bx); SetReg16(t,CX,(WORD)r.cx); return;
    case 0x62: case 0x51: SetReg16(t,BX,t->psp); return;
    case 0x2e: case 0x54: call(&r); if(ah==0x54) SetReg16(t,AX,(WORD)((Reg16(t,AX)&0xff00)|(BYTE)r.ax)); return;
    default: result(t,1); return;
    }
}
