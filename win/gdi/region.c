/* SPDX-License-Identifier: GPL-2.0-or-later
 * Rectangle lists for regions and clipping: disjoint rectangles kept
 * disjoint by every operation (subtracting before adding).
 */
#include "gdip.h"

void r_set(RECT *r,int l,int t,int rt,int b) {r->left=l; r->top=t; r->right=rt; r->bottom=b;}
BOOL r_empty(const RECT *r) {return r->left>=r->right || r->top>=r->bottom;}
BOOL r_intersect(RECT *d,const RECT *a,const RECT *b) {
    RECT r; r_set(&r,max(a->left,b->left),max(a->top,b->top),min(a->right,b->right),min(a->bottom,b->bottom));
    if(r_empty(&r)) {r_set(d,0,0,0,0); return FALSE;}
    *d=r; return TRUE;
}
void r_union(RECT *d,const RECT *a,const RECT *b) {
    if(r_empty(a)) {*d=*b; return;}
    if(r_empty(b)) {*d=*a; return;}
    r_set(d,min(a->left,b->left),min(a->top,b->top),max(a->right,b->right),max(a->bottom,b->bottom));
}
void rl_free(RectList *l) {if(l->rects) gdi_free(l->rects); l->rects=NULL; l->count=l->capacity=0;}
static BOOL reserve(RectList *l,int n) {
    RECT *r;
    if(n<=l->capacity) return TRUE;
    n=n<8?8:n*2;
    r=(RECT *)gdi_alloc((DWORD)(n*sizeof(RECT)));
    if(!r) return FALSE;
    if(l->count) memcpy(r,l->rects,sizeof(RECT)*(unsigned)l->count);
    if(l->rects) gdi_free(l->rects);
    l->rects=r; l->capacity=n; return TRUE;
}
static BOOL push(RectList *l,const RECT *r) {
    if(r_empty(r)) return TRUE;
    if(!reserve(l,l->count+1)) return FALSE;
    l->rects[l->count++]=*r; return TRUE;
}
BOOL rl_set(RectList *l,const RECT *r) {l->count=0; return push(l,r);}
BOOL rl_copy(RectList *d,const RectList *s) {
    if(d==s) return TRUE;
    d->count=0;
    if(!reserve(d,s->count)) return FALSE;
    if(s->count) memcpy(d->rects,s->rects,sizeof(RECT)*(unsigned)s->count);
    d->count=s->count; return TRUE;
}
BOOL rl_subtract(RectList *l,const RECT *cut) {
    RectList out; int i,k; BOOL ok=TRUE;
    memset(&out,0,sizeof(out));
    for(i=0;i<l->count && ok;i++) {
        RECT a=l->rects[i],c,piece[4];
        if(!r_intersect(&c,&a,cut)) {ok=push(&out,&a); continue;}
        r_set(&piece[0],a.left,a.top,a.right,c.top);
        r_set(&piece[1],a.left,c.bottom,a.right,a.bottom);
        r_set(&piece[2],a.left,c.top,c.left,c.bottom);
        r_set(&piece[3],c.right,c.top,a.right,c.bottom);
        for(k=0;k<4 && ok;k++) ok=push(&out,&piece[k]);
    }
    if(ok) ok=rl_copy(l,&out);
    rl_free(&out); return ok;
}
BOOL rl_add(RectList *l,const RECT *r) {
    RectList add; int i; BOOL ok=TRUE;
    memset(&add,0,sizeof(add));
    if(!rl_set(&add,r)) return FALSE;
    for(i=0;i<l->count && ok;i++) ok=rl_subtract(&add,&l->rects[i]);
    for(i=0;i<add.count && ok;i++) ok=push(l,&add.rects[i]);
    rl_free(&add); return ok;
}
BOOL rl_intersect(RectList *out,const RectList *a,const RectList *b) {
    RectList r; int i,j; BOOL ok=TRUE;
    memset(&r,0,sizeof(r));
    for(i=0;i<a->count && ok;i++) for(j=0;j<b->count && ok;j++) {
        RECT c; if(r_intersect(&c,&a->rects[i],&b->rects[j])) ok=push(&r,&c);
    }
    if(ok) ok=rl_copy(out,&r);
    rl_free(&r); return ok;
}
BOOL rl_combine(RectList *out,const RectList *a,const RectList *b,int mode) {
    RectList r; int i; BOOL ok=TRUE;
    memset(&r,0,sizeof(r));
    switch(mode) {
    case RGN_AND: ok=rl_intersect(&r,a,b); break;
    case RGN_COPY: ok=rl_copy(&r,a); break;
    case RGN_OR: ok=rl_copy(&r,a); for(i=0;i<b->count && ok;i++) ok=rl_add(&r,&b->rects[i]); break;
    case RGN_DIFF: ok=rl_copy(&r,a); for(i=0;i<b->count && ok;i++) ok=rl_subtract(&r,&b->rects[i]); break;
    case RGN_XOR: {
        RectList both; memset(&both,0,sizeof(both));
        ok=rl_intersect(&both,a,b);
        if(ok) ok=rl_copy(&r,a);
        for(i=0;i<b->count && ok;i++) ok=rl_add(&r,&b->rects[i]);
        for(i=0;i<both.count && ok;i++) ok=rl_subtract(&r,&both.rects[i]);
        rl_free(&both); break;
    }
    default: ok=FALSE;
    }
    if(ok) ok=rl_copy(out,&r);
    rl_free(&r); return ok;
}
void rows_begin(RowBuilder *b,RectList *l) {b->list=l; b->first=b->count=0;}
/* A row with the same spans as the one directly above extends it. */
BOOL rows_add(RowBuilder *b,int y,const int *spans,int n) {
    RectList *l=b->list; int i,k=0;
    for(i=0;i+1<n;i+=2) if(spans[i]<spans[i+1]) k++;
    if(k && k==b->count && l->rects[b->first].bottom==y) {
        int j=0;
        for(i=0;i+1<n;i+=2) if(spans[i]<spans[i+1]) {
            const RECT *r=&l->rects[b->first+j++];
            if(r->left!=spans[i] || r->right!=spans[i+1]) break;
        }
        if(i+1>=n) {for(j=0;j<k;j++) l->rects[b->first+j].bottom=y+1; return TRUE;}
    }
    b->first=l->count; b->count=0;
    for(i=0;i+1<n;i+=2) if(spans[i]<spans[i+1]) {
        RECT r; r_set(&r,spans[i],y,spans[i+1],y+1);
        if(!push(l,&r)) return FALSE;
        b->count++;
    }
    return TRUE;
}
void rl_offset(RectList *l,int dx,int dy) {
    int i;
    for(i=0;i<l->count;i++) {l->rects[i].left+=dx; l->rects[i].right+=dx; l->rects[i].top+=dy; l->rects[i].bottom+=dy;}
}
void rl_box(const RectList *l,RECT *box) {
    int i; r_set(box,0,0,0,0);
    for(i=0;i<l->count;i++) r_union(box,box,&l->rects[i]);
}
BOOL rl_contains(const RectList *l,int x,int y) {
    int i;
    for(i=0;i<l->count;i++) if(x>=l->rects[i].left && x<l->rects[i].right && y>=l->rects[i].top && y<l->rects[i].bottom) return TRUE;
    return FALSE;
}
