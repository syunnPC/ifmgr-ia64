/* SPDX-License-Identifier: GPL-2.0-or-later
 * DDE lParam packing: WM_DDE_ACK, WM_DDE_ADVISE, WM_DDE_DATA and
 * WM_DDE_POKE carry a block (shareable global memory) holding their two
 * values and a mark; the other messages carry two words, WM_DDE_EXECUTE
 * its handle.
 */
#include "user.h"
#include <dde.h>
#define BLOCK_MARK 0x50454444U /* "DDEP" */
static BOOL packed(UINT msg) {return msg==WM_DDE_ACK || msg==WM_DDE_ADVISE || msg==WM_DDE_DATA || msg==WM_DDE_POKE;}
LPARAM WINAPI PackDDElParam(UINT msg,UINT_PTR lo,UINT_PTR hi) {
    HGLOBAL h; UINT_PTR *p;
    if(msg==WM_DDE_EXECUTE) return (LPARAM)hi;
    if(!packed(msg)) return (LPARAM)MAKELONG(lo,hi);
    if(!(h=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,3*sizeof(UINT_PTR))) || !(p=(UINT_PTR *)GlobalLock(h))) return 0;
    p[0]=lo; p[1]=hi; p[2]=BLOCK_MARK; GlobalUnlock(h);
    return (LPARAM)h;
}
/* Whether lParam is a block PackDDElParam made: not WM_DDE_INITIATE's
 * acknowledgement (two atoms), nor the two words a Win16 source packs. */
BOOL DDEBlock(LPARAM lp) {
    UINT_PTR *p; BOOL yes;
    if(!lp || (ULONG_PTR)lp>0xffff || GlobalSize((HGLOBAL)lp)!=3*sizeof(UINT_PTR) || !(p=(UINT_PTR *)GlobalLock((HGLOBAL)lp))) return FALSE;
    yes=p[2]==BLOCK_MARK;
    GlobalUnlock((HGLOBAL)lp);
    return yes;
}
BOOL WINAPI UnpackDDElParam(UINT msg,LPARAM lp,PUINT_PTR lo,PUINT_PTR hi) {
    UINT_PTR *p;
    if(msg==WM_DDE_EXECUTE) {if(lo) *lo=0; if(hi) *hi=(UINT_PTR)lp; return TRUE;}
    if(!packed(msg)) {if(lo) *lo=LOWORD(lp); if(hi) *hi=HIWORD(lp); return TRUE;}
    if(!lp || !(p=(UINT_PTR *)GlobalLock((HGLOBAL)lp))) {if(lo) *lo=0; if(hi) *hi=0; return FALSE;}
    if(lo) *lo=p[0];
    if(hi) *hi=p[1];
    GlobalUnlock((HGLOBAL)lp);
    return TRUE;
}
BOOL WINAPI FreeDDElParam(UINT msg,LPARAM lp) {
    if(!packed(msg) || !lp) return TRUE;
    return GlobalFree((HGLOBAL)lp)==NULL;
}
LPARAM WINAPI ReuseDDElParam(LPARAM lp,UINT in,UINT out,UINT_PTR lo,UINT_PTR hi) {
    FreeDDElParam(in,lp);
    return PackDDElParam(out,lo,hi);
}
