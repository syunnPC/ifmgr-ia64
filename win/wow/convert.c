/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: conversions shared by the generated and the hand-written
 * functions. RECT16, POINT16 and SIZE16 hold signed WORDs.
 */
#include "api.h"
BOOL RectIn16(DWORD p,RECT *r) {
    const BYTE *i=(const BYTE *)PTR(p);
    if(!i) return FALSE;
    r->left=(short)get16(i); r->top=(short)get16(i+2); r->right=(short)get16(i+4); r->bottom=(short)get16(i+6);
    return TRUE;
}
void RectOut16(DWORD p,const RECT *r) {
    BYTE *o=(BYTE *)PTR(p);
    if(o) {put16(o,(WORD)r->left); put16(o+2,(WORD)r->top); put16(o+4,(WORD)r->right); put16(o+6,(WORD)r->bottom);}
}
BOOL PointIn16(DWORD p,POINT *pt) {
    const BYTE *i=(const BYTE *)PTR(p);
    if(!i) return FALSE;
    pt->x=(short)get16(i); pt->y=(short)get16(i+2);
    return TRUE;
}
void PointOut16(DWORD p,const POINT *pt) {BYTE *o=(BYTE *)PTR(p); if(o) {put16(o,(WORD)pt->x); put16(o+2,(WORD)pt->y);}}
void SizeOut16(DWORD p,const SIZE *s) {BYTE *o=(BYTE *)PTR(p); if(o) {put16(o,(WORD)s->cx); put16(o+2,(WORD)s->cy);}}
HBRUSH Brush32(WORD h) {return h<0x100?(HBRUSH)(ULONG_PTR)h:(HBRUSH)HGDI32(h);}
HINSTANCE Instance32(WORD h) {
    Module16 *m;
    if(!h) return NULL;
    m=NeFromHandle(h);
    return m?INSTANCE32(m->handle):NULL;
}
