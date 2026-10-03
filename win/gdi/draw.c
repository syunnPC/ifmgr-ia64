/* SPDX-License-Identifier: GPL-2.0-or-later
 * Drawing: pixels, lines, rectangles, ellipses, rounded rectangles, arcs,
 * chords and pies, polygons, flood fills, and regions: painting them and
 * making them from the same shapes. Shapes are filled row by row: the
 * outline is the shape minus the shape shrunk by the pen width, the interior
 * is the shrunk shape. Coordinates go through the DC's mapping mode (map.c)
 * first; the rest works in device pixels.
 */
#include "gdip.h"

/* The 16 binary raster operations on pen (p) and destination (d). */
static DWORD rop2(int code,DWORD p,DWORD d) {
    DWORD r;
    switch(code) {
    case R2_BLACK: r=0; break;
    case R2_NOTMERGEPEN: r=~(p|d); break;
    case R2_MASKNOTPEN: r=~p&d; break;
    case R2_NOTCOPYPEN: r=~p; break;
    case R2_MASKPENNOT: r=p&~d; break;
    case R2_NOT: r=~d; break;
    case R2_XORPEN: r=p^d; break;
    case R2_NOTMASKPEN: r=~(p&d); break;
    case R2_MASKPEN: r=p&d; break;
    case R2_NOTXORPEN: r=~(p^d); break;
    case R2_NOP: r=d; break;
    case R2_MERGENOTPEN: r=~p|d; break;
    case R2_MERGEPENNOT: r=p|~d; break;
    case R2_MERGEPEN: r=p|d; break;
    case R2_WHITE: r=0xffffff; break;
    default: r=p; break;
    }
    return r&0xffffff;
}
void fill_solid(DC *dc,const RECT *area,DWORD value,int code) {
    int i,x,y; RECT c; Surface *s=dc->surface;
    if(s->mono) value=to_mono(value);
    for(i=0;i<dc->eff.count;i++) if(r_intersect(&c,area,&dc->eff.rects[i])) {
        for(y=c.top;y<c.bottom;y++) {
            DWORD *row=s->bits+(ULONG_PTR)y*s->stride;
            if(code==R2_COPYPEN) for(x=c.left;x<c.right;x++) row[x]=value;
            else for(x=c.left;x<c.right;x++) row[x]=rop2(code,value,row[x]);
        }
        if(dc->display) mark(&c);
    }
}
/* The brush's pixel at a device position; ~0 means leave the destination. */
DWORD brush_pixel(const Brush *b,const DC *dc,int x,int y) {
    DWORD p;
    if(b->style==BS_SOLID) return dc_pixel(dc,b->color);
    p=b->pattern[((y-dc->origin.y-dc->s.brush_origin.y)&7)*8+((x-dc->origin.x-dc->s.brush_origin.x)&7)];
    if(b->style==BS_HATCHED) return p?dc_pixel(dc,b->color):dc->s.bk_mode==OPAQUE?dc_pixel(dc,dc->s.bk):0xffffffffU;
    if(b->mono) return p?dc_pixel(dc,dc->s.bk):dc_pixel(dc,dc->s.text);
    return p;
}
static void fill_device(DC *dc,const RECT *area,const Brush *b,int code) {
    int i,x,y; RECT c; Surface *s=dc->surface;
    if(!b || b->style==BS_NULL) return;
    if(b->style==BS_SOLID) {fill_solid(dc,area,dc_pixel(dc,b->color),code); return;}
    for(i=0;i<dc->eff.count;i++) if(r_intersect(&c,area,&dc->eff.rects[i])) {
        for(y=c.top;y<c.bottom;y++) {
            DWORD *row=s->bits+(ULONG_PTR)y*s->stride;
            for(x=c.left;x<c.right;x++) {
                DWORD p=brush_pixel(b,dc,x,y);
                if(p!=0xffffffffU) row[x]=rop2(code,s->mono?to_mono(p):p,row[x]);
            }
        }
        if(dc->display) mark(&c);
    }
}
static void plot(DC *dc,int x,int y,DWORD value,int code) {
    RECT r; r_set(&r,x,y,x+1,y+1); fill_solid(dc,&r,value,code);
}
COLORREF WINAPI SetPixel(HDC h,int x,int y,COLORREF c) {
    DC *dc=dc_of(h);
    if(!dc) return (COLORREF)-1;
    if(dc->meta) return mf_words(dc,META_SETPIXEL,4,(int)LOWORD(c),(int)HIWORD(c),y,x)?c:(COLORREF)-1;
    plot(dc,dev_x(dc,x),dev_y(dc,y),dc_pixel(dc,c),R2_COPYPEN);
    return color(dc_pixel(dc,c));
}
COLORREF WINAPI GetPixel(HDC h,int x,int y) {
    DC *dc=dc_of(h);
    if(!dc) return (COLORREF)-1;
    x=dev_x(dc,x); y=dev_y(dc,y);
    if(!rl_contains(&dc->eff,x,y)) return (COLORREF)-1;
    return color(dc->surface->bits[(ULONG_PTR)y*dc->surface->stride+x]);
}

/* --- lines ------------------------------------------------------------------------ */
static const BYTE dashes[5][8]={{0},{18,6,0},{3,3,0},{9,6,3,6,0},{9,3,3,3,3,3,0}};
typedef struct {DC *dc; Pen *pen; DWORD value; int code,phase,width;} LineState;
/* A pen's width in pixels: a width of 1 or less is one pixel, wider pens
 * are logical units along x. */
static int pen_width(const DC *dc,const Pen *p) {
    int w;
    if(!p || p->style==PS_NULL) return 0;
    if(p->width<=1) return 1;
    w=dev_w(dc,p->width);
    return w<1?1:w;
}
static void stamp(LineState *l,int x,int y) {
    int w=l->width;
    if(l->pen->style>=PS_DASH && l->pen->style<=PS_DASHDOTDOT && w==1) {
        const BYTE *d=dashes[l->pen->style]; int total=0,i,at,on=1;
        for(i=0;d[i];i++) total+=d[i];
        at=l->phase++%total;
        for(i=0;d[i];i++) {if(at<d[i]) break; at-=d[i]; on=!on;}
        if(!on) {if(l->dc->s.bk_mode==OPAQUE) plot(l->dc,x,y,dc_pixel(l->dc,l->dc->s.bk),l->code); return;}
    }
    if(w<=1) plot(l->dc,x,y,l->value,l->code);
    else {RECT r; r_set(&r,x-w/2,y-w/2,x-w/2+w,y-w/2+w); fill_solid(l->dc,&r,l->value,l->code);}
}
/* Bresenham, from (x0,y0) up to but excluding (x1,y1), device coordinates. */
static void line(LineState *l,int x0,int y0,int x1,int y1) {
    int dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y0-y1:y1-y0,sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    while(x0!=x1 || y0!=y1) {
        int e2=2*err;
        stamp(l,x0,y0);
        if(e2>=dy) {err+=dy; x0+=sx;}
        if(e2<=dx) {err+=dx; y0+=sy;}
    }
}
static BOOL pen_state(DC *dc,LineState *l) {
    l->dc=dc; l->pen=(Pen *)object(dc->s.pen,OBJ_PEN); l->code=dc->s.rop2; l->phase=0;
    if(!l->pen || l->pen->style==PS_NULL) return FALSE;
    l->value=dc_pixel(dc,l->pen->color); l->width=pen_width(dc,l->pen); return TRUE;
}
DWORD WINAPI MoveTo(HDC h,int x,int y) {
    DC *dc=dc_of(h); DWORD old;
    if(!dc) return 0;
    if(dc->meta) mf_words(dc,META_MOVETO,2,y,x);
    old=MAKELONG(dc->s.pos.x,dc->s.pos.y); dc->s.pos.x=x; dc->s.pos.y=y;
    return old;
}
BOOL WINAPI MoveToEx(HDC h,int x,int y,LPPOINT old) {
    DC *dc=dc_of(h);
    if(!dc) return FALSE;
    if(dc->meta) mf_words(dc,META_MOVETO,2,y,x);
    if(old) *old=dc->s.pos;
    dc->s.pos.x=x; dc->s.pos.y=y; return TRUE;
}
BOOL WINAPI LineTo(HDC h,int x,int y) {
    DC *dc=dc_of(h); LineState l;
    if(!dc) return FALSE;
    if(dc->meta) {dc->s.pos.x=x; dc->s.pos.y=y; return mf_words(dc,META_LINETO,2,y,x);}
    if(pen_state(dc,&l)) line(&l,dev_x(dc,dc->s.pos.x),dev_y(dc,dc->s.pos.y),dev_x(dc,x),dev_y(dc,y));
    dc->s.pos.x=x; dc->s.pos.y=y;
    return TRUE;
}
BOOL WINAPI Polyline(HDC h,const POINT FAR *p,int n) {
    DC *dc=dc_of(h); LineState l; int i;
    if(!dc || !p || n<2) return FALSE;
    if(dc->meta) return mf_points(dc,META_POLYLINE,p,n);
    if(pen_state(dc,&l)) {
        for(i=1;i<n;i++) line(&l,dev_x(dc,p[i-1].x),dev_y(dc,p[i-1].y),dev_x(dc,p[i].x),dev_y(dc,p[i].y));
        stamp(&l,dev_x(dc,p[n-1].x),dev_y(dc,p[n-1].y));
    }
    return TRUE;
}

/* --- filled shapes ---------------------------------------------------------------------- */
typedef struct {int kind; double l,t,r,b,cw,ch;} Shape; /* 0 rectangle, 1 ellipse, 2 rounded */
/* The pixel columns [*x0,*x1) of row y (pixel centres) inside the shape. */
static BOOL span(const Shape *s,int y,int *x0,int *x1) {
    double cy=y+0.5,half,inset=0;
    if(s->r<=s->l || s->b<=s->t || cy<s->t || cy>=s->b) return FALSE;
    if(s->kind==1) {
        double rx=(s->r-s->l)/2,ry=(s->b-s->t)/2,mx=s->l+rx,my=s->t+ry,d=(cy-my)/ry;
        if(d*d>=1) return FALSE;
        {double v=1-d*d,root=0; int k; root=v; for(k=0;k<20;k++) root=(root+v/root)/2; half=rx*root;}
        *x0=(int)(mx-half+0.5); *x1=(int)(mx+half+0.5);
        return *x1>*x0;
    }
    if(s->kind==2 && s->cw>0 && s->ch>0) {
        double rx=s->cw/2,ry=s->ch/2,d=0;
        if(cy<s->t+ry) d=(s->t+ry-cy)/ry;
        else if(cy>s->b-ry) d=(cy-(s->b-ry))/ry;
        if(d>0) {double v=1-d*d,root=v; int k; if(v<=0) return FALSE; for(k=0;k<20;k++) root=(root+v/root)/2; inset=rx-rx*root;}
    }
    *x0=(int)(s->l+inset+0.5); *x1=(int)(s->r-inset+0.5);
    return *x1>*x0;
}
static void shape(DC *dc,Shape outer) {
    Pen *pen=(Pen *)object(dc->s.pen,OBJ_PEN); Brush *brush=(Brush *)object(dc->s.brush,OBJ_BRUSH);
    int w=pen_width(dc,pen),y,top,bottom; DWORD ink=pen?dc_pixel(dc,pen->color):0;
    Shape inner=outer;
    inner.l+=w; inner.t+=w; inner.r-=w; inner.b-=w;
    if(inner.kind==2) {inner.cw=max(0,outer.cw-2*w); inner.ch=max(0,outer.ch-2*w);}
    top=(int)outer.t; bottom=(int)(outer.b+0.999);
    for(y=top;y<bottom;y++) {
        int a,b,c,d; BOOL in=span(&inner,y,&c,&d); RECT r;
        if(!span(&outer,y,&a,&b)) continue;
        if(in) {
            r_set(&r,c,y,d,y+1); fill_device(dc,&r,brush,dc->s.rop2);
            if(w) {
                r_set(&r,a,y,c,y+1); fill_solid(dc,&r,ink,dc->s.rop2);
                r_set(&r,d,y,b,y+1); fill_solid(dc,&r,ink,dc->s.rop2);
            }
        } else if(w) {r_set(&r,a,y,b,y+1); fill_solid(dc,&r,ink,dc->s.rop2);}
        else {r_set(&r,a,y,b,y+1); fill_device(dc,&r,brush,dc->s.rop2);}
    }
}
static Shape shape_of(int kind,const RECT *r) {
    Shape s; s.kind=kind; s.l=r->left; s.t=r->top; s.r=r->right; s.b=r->bottom; s.cw=s.ch=0;
    return s;
}
static Shape box(DC *dc,int kind,int l,int t,int r,int b) {
    RECT rc; r_set(&rc,l,t,r,b); dev_rect(dc,&rc);
    return shape_of(kind,&rc);
}
BOOL WINAPI Rectangle(HDC h,int l,int t,int r,int b) {
    DC *dc=dc_of(h); if(!dc) return FALSE;
    if(dc->meta) return mf_words(dc,META_RECTANGLE,4,b,r,t,l);
    shape(dc,box(dc,0,l,t,r,b)); return TRUE;
}
BOOL WINAPI Ellipse(HDC h,int l,int t,int r,int b) {
    DC *dc=dc_of(h); if(!dc) return FALSE;
    if(dc->meta) return mf_words(dc,META_ELLIPSE,4,b,r,t,l);
    shape(dc,box(dc,1,l,t,r,b)); return TRUE;
}
BOOL WINAPI RoundRect(HDC h,int l,int t,int r,int b,int cw,int ch) {
    DC *dc=dc_of(h); Shape s;
    if(!dc) return FALSE;
    if(dc->meta) return mf_words(dc,META_ROUNDRECT,6,ch,cw,b,r,t,l);
    s=box(dc,2,l,t,r,b); s.cw=min(dev_w(dc,cw),(int)(s.r-s.l)); s.ch=min(dev_h(dc,ch),(int)(s.b-s.t));
    shape(dc,s); return TRUE;
}
/* --- polygons ------------------------------------------------------------------------ */
/* The spans of row y (pixel centres) inside polygons of device points, by
 * the alternate or winding rule, as [left,right) pairs; returns how many
 * numbers went to spans. */
#define CROSSINGS 128
static int poly_spans(const POINT *p,const int *counts,int polys,int mode,int y,int *spans,int room) {
    double xs[CROSSINGS],cy=y+0.5; int dirs[CROSSINGS],count=0,j,k,n=0,base=0,poly;
    for(poly=0;poly<polys;base+=counts[poly++]) {
        int i,c=counts[poly];
        for(i=0;i<c && count<CROSSINGS;i++) {
            const POINT *a=&p[base+i],*b=&p[base+(i+1)%c];
            if((a->y<=cy && b->y>cy) || (b->y<=cy && a->y>cy)) {
                xs[count]=a->x+(cy-a->y)*(double)(b->x-a->x)/(double)(b->y-a->y);
                dirs[count++]=b->y>a->y?1:-1;
            }
        }
    }
    for(j=1;j<count;j++) for(k=j;k>0 && xs[k-1]>xs[k];k--) {
        double t=xs[k]; int d=dirs[k]; xs[k]=xs[k-1]; dirs[k]=dirs[k-1]; xs[k-1]=t; dirs[k-1]=d;
    }
    for(j=0;j<count && n+2<=room;) {
        int wind=0,start=j,l,r;
        if(mode==WINDING) {do wind+=dirs[j++]; while(j<count && wind);}
        else j+=2;
        if(j>count) break;
        l=(int)(xs[start]+0.5); r=(int)(xs[j-1]+0.5);
        if(l>=r) continue;
        if(n && spans[n-1]>=l) {if(r>spans[n-1]) spans[n-1]=r; continue;}
        spans[n++]=l; spans[n++]=r;
    }
    return n;
}
static void poly_bounds(const POINT *p,int total,int *top,int *bottom) {
    int i; *top=*bottom=total?p[0].y:0;
    for(i=1;i<total;i++) {*top=min(*top,(int)p[i].y); *bottom=max(*bottom,(int)p[i].y);}
}
/* Logical points to device points, in a buffer that may be the one given. */
static POINT *device_points(DC *dc,const POINT *p,int total,POINT *small,int room) {
    POINT *d=total<=room?small:(POINT *)gdi_alloc((DWORD)total*sizeof(POINT)); int i;
    if(d) for(i=0;i<total;i++) {d[i]=p[i]; dev_point(dc,&d[i]);}
    return d;
}
static BOOL polys(HDC h,const POINT *p,const int *counts,int n) {
    DC *dc=dc_of(h); Brush *brush; POINT small[64],*d; int total=0,i,y,top,bottom,spans[CROSSINGS];
    if(!dc || !p || !counts || n<1) return FALSE;
    for(i=0;i<n;i++) {if(counts[i]<2) return FALSE; total+=counts[i];}
    if(!(d=device_points(dc,p,total,small,64))) return FALSE;
    brush=(Brush *)object(dc->s.brush,OBJ_BRUSH);
    poly_bounds(d,total,&top,&bottom);
    if(brush && brush->style!=BS_NULL) for(y=top;y<bottom;y++) {
        int k=poly_spans(d,counts,n,dc->s.poly_fill,y,spans,CROSSINGS);
        for(i=0;i<k;i+=2) {RECT r; r_set(&r,spans[i],y,spans[i+1],y+1); fill_device(dc,&r,brush,dc->s.rop2);}
    }
    {
        LineState l; int base=0,poly;
        if(pen_state(dc,&l)) for(poly=0;poly<n;base+=counts[poly++])
            for(i=0;i<counts[poly];i++) {
                const POINT *a=&d[base+i],*b=&d[base+(i+1)%counts[poly]];
                line(&l,a->x,a->y,b->x,b->y);
            }
    }
    if(d!=small) gdi_free(d);
    return TRUE;
}
BOOL WINAPI Polygon(HDC h,const POINT FAR *p,int n) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return p && n>0 && mf_points(dc,META_POLYGON,p,n);
    return polys(h,p,&n,1);
}
BOOL WINAPI PolyPolygon(HDC h,const POINT FAR *p,const int FAR *counts,int n) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return p && counts && n>0 && mf_poly_polygon(dc,p,counts,n);
    return polys(h,p,counts,n);
}

/* --- arcs, chords and pies ------------------------------------------------------------- */
/* The ellipse of a bounding box, and the directions from its centre to the
 * start and end points (y up), between which the arc runs counterclockwise
 * on the device. */
typedef struct {Shape box; double cx,cy,sx,sy,ex,ey; BOOL full;} ArcShape;
static double cross(double ax,double ay,double bx,double by) {return ax*by-ay*bx;}
static BOOL in_sweep(const ArcShape *a,double x,double y) {
    double se=cross(a->sx,a->sy,a->ex,a->ey);
    if(a->full) return TRUE;
    if(se>0 || (se==0 && a->sx*a->ex+a->sy*a->ey<0)) return cross(a->sx,a->sy,x,y)>=0 && cross(x,y,a->ex,a->ey)>=0;
    return cross(a->sx,a->sy,x,y)>=0 || cross(x,y,a->ex,a->ey)>=0;
}
static double root(double v) {double r=v; int k; if(v<=0) return 0; for(k=0;k<30;k++) r=(r+v/r)/2; return r;}
/* Where the direction (x,y) from the centre meets the ellipse, in device pixels. */
static void on_ellipse(const ArcShape *a,double x,double y,int *px,int *py) {
    double rx=(a->box.r-a->box.l)/2,ry=(a->box.b-a->box.t)/2,k=0,q;
    if(rx>0 && ry>0 && (x || y)) {q=(x/rx)*(x/rx)+(y/ry)*(y/ry); k=1/root(q);}
    *px=(int)(a->cx+x*k+(x*k>=0?0.5:-0.5)); *py=(int)(a->cy-y*k+(-y*k>=0?0.5:-0.5));
}
static BOOL arc_shape(DC *dc,ArcShape *a,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    a->box=box(dc,1,l,t,r,b);
    if(a->box.r<=a->box.l || a->box.b<=a->box.t) return FALSE;
    a->cx=(a->box.l+a->box.r)/2; a->cy=(a->box.t+a->box.b)/2;
    a->sx=dev_x(dc,xs)-a->cx; a->sy=a->cy-dev_y(dc,ys); a->ex=dev_x(dc,xe)-a->cx; a->ey=a->cy-dev_y(dc,ye);
    a->full=cross(a->sx,a->sy,a->ex,a->ey)==0 && a->sx*a->ex+a->sy*a->ey>0;
    return TRUE;
}
/* The outline: pixels of the ellipse with a neighbour outside, in the sweep. */
static void arc_outline(LineState *l,const ArcShape *a) {
    int y,x,top=(int)a->box.t,bottom=(int)(a->box.b+0.999);
    for(y=top;y<bottom;y++) {
        int x0,x1,u0,u1,d0,d1; BOOL up,down;
        if(!span(&a->box,y,&x0,&x1)) continue;
        up=span(&a->box,y-1,&u0,&u1); down=span(&a->box,y+1,&d0,&d1);
        for(x=x0;x<x1;x++) {
            BOOL inside=x>x0 && x<x1-1 && up && down && x>=u0 && x<u1 && x>=d0 && x<d1;
            if(!inside && in_sweep(a,x+0.5-a->cx,a->cy-(y+0.5))) stamp(l,x,y);
        }
    }
}
enum {ARC,CHORD,PIE};
static BOOL arc_figure(HDC h,int kind,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    DC *dc=dc_of(h); ArcShape a; LineState ls; Brush *brush; int y,x,sx,sy,ex,ey;
    if(!dc) return FALSE;
    if(!arc_shape(dc,&a,l,t,r,b,xs,ys,xe,ye)) return TRUE;
    on_ellipse(&a,a.sx,a.sy,&sx,&sy); on_ellipse(&a,a.ex,a.ey,&ex,&ey);
    brush=(Brush *)object(dc->s.brush,OBJ_BRUSH);
    if(kind!=ARC && brush && brush->style!=BS_NULL) {
        int top=(int)a.box.t,bottom=(int)(a.box.b+0.999);
        for(y=top;y<bottom;y++) {
            int x0,x1,run=-1;
            if(!span(&a.box,y,&x0,&x1)) continue;
            for(x=x0;x<=x1;x++) {
                BOOL in=FALSE;
                if(x<x1) {
                    double px=x+0.5-a.cx,py=a.cy-(y+0.5);
                    in=a.full || (kind==PIE?in_sweep(&a,px,py):cross(ex-sx,sy-ey,x+0.5-sx,sy-(y+0.5))<=0);
                }
                if(in && run<0) run=x;
                else if(!in && run>=0) {RECT rc; r_set(&rc,run,y,x,y+1); fill_device(dc,&rc,brush,dc->s.rop2); run=-1;}
            }
        }
    }
    if(pen_state(dc,&ls)) {
        arc_outline(&ls,&a);
        if(kind==PIE && !a.full) {
            int cx=(int)a.cx,cy=(int)a.cy;
            line(&ls,cx,cy,sx,sy); line(&ls,cx,cy,ex,ey);
        } else if(kind==CHORD && !a.full) line(&ls,sx,sy,ex,ey);
    }
    return TRUE;
}
static BOOL arc_record(HDC h,WORD function,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    return mf_words(dc_of(h),function,8,ye,xe,ys,xs,b,r,t,l);
}
BOOL WINAPI Arc(HDC h,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    if(META_DC(dc_of(h))) return arc_record(h,META_ARC,l,t,r,b,xs,ys,xe,ye);
    return arc_figure(h,ARC,l,t,r,b,xs,ys,xe,ye);
}
BOOL WINAPI Chord(HDC h,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    if(META_DC(dc_of(h))) return arc_record(h,META_CHORD,l,t,r,b,xs,ys,xe,ye);
    return arc_figure(h,CHORD,l,t,r,b,xs,ys,xe,ye);
}
BOOL WINAPI Pie(HDC h,int l,int t,int r,int b,int xs,int ys,int xe,int ye) {
    if(META_DC(dc_of(h))) return arc_record(h,META_PIE,l,t,r,b,xs,ys,xe,ye);
    return arc_figure(h,PIE,l,t,r,b,xs,ys,xe,ye);
}

/* LineDDA: the points of a line, without the last, to a callback. */
void WINAPI LineDDA(int x0,int y0,int x1,int y1,LINEDDAPROC proc,LPARAM lp) {
    int dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y0-y1:y1-y0,sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    if(!proc) return;
    while(x0!=x1 || y0!=y1) {
        int e2=2*err;
        proc(x0,y0,lp);
        if(e2>=dy) {err+=dy; x0+=sx;}
        if(e2<=dx) {err+=dx; y0+=sy;}
    }
}

/* --- flood fill ------------------------------------------------------------------------ */
/* Scan lines from a seed: pixels unlike the border color (FLOODFILLBORDER) or
 * like the surface color (FLOODFILLSURFACE) inside the clipping area. */
typedef struct {DC *dc; DWORD target; UINT mode; BYTE *seen; int width,height;} Flood;
static BOOL floodable(const Flood *f,int x,int y) {
    ULONG_PTR i=(ULONG_PTR)y*f->width+x; DWORD v;
    if(x<0 || y<0 || x>=f->width || y>=f->height || f->seen[i/8]&(1<<(i%8))) return FALSE;
    if(!rl_contains(&f->dc->eff,x,y)) return FALSE;
    v=f->dc->surface->bits[(ULONG_PTR)y*f->dc->surface->stride+x];
    return f->mode==FLOODFILLSURFACE?v==f->target:v!=f->target;
}
static void see(Flood *f,int x,int y) {ULONG_PTR i=(ULONG_PTR)y*f->width+x; f->seen[i/8]|=(BYTE)(1<<(i%8));}
BOOL WINAPI ExtFloodFill(HDC h,int x,int y,COLORREF c,UINT mode) {
    DC *dc=dc_of(h); Flood f; Brush *brush; POINT *stack; int top=0,room=4096;
    if(!dc) return FALSE;
    if(dc->meta) return mf_words(dc,META_EXTFLOODFILL,5,(int)mode,(int)LOWORD(c),(int)HIWORD(c),y,x);
    brush=(Brush *)object(dc->s.brush,OBJ_BRUSH);
    f.dc=dc; f.mode=mode; f.width=dc->surface->width; f.height=dc->surface->height;
    f.target=dc_pixel(dc,c); if(dc->surface->mono) f.target=to_mono(f.target);
    x=dev_x(dc,x); y=dev_y(dc,y);
    f.seen=(BYTE *)gdi_alloc(((DWORD)f.width*(DWORD)f.height+7)/8);
    stack=(POINT *)gdi_alloc((DWORD)room*sizeof(POINT));
    if(!f.seen || !stack || !brush || !floodable(&f,x,y)) {gdi_free(f.seen); gdi_free(stack); return FALSE;}
    stack[top].x=x; stack[top++].y=y;
    while(top) {
        int l,r,row,k; POINT p=stack[--top]; RECT rc;
        if(!floodable(&f,p.x,p.y)) continue;
        for(l=p.x;l>0 && floodable(&f,l-1,p.y);l--);
        for(r=p.x;r+1<f.width && floodable(&f,r+1,p.y);r++);
        for(k=l;k<=r;k++) see(&f,k,p.y);
        r_set(&rc,l,p.y,r+1,p.y+1); fill_device(dc,&rc,brush,R2_COPYPEN);
        for(row=p.y-1;row<=p.y+1;row+=2) {
            BOOL in=FALSE;
            for(k=l;k<=r;k++) {
                BOOL ok=floodable(&f,k,row);
                if(ok && !in) {
                    if(top==room) {
                        POINT *more=(POINT *)gdi_alloc((DWORD)room*2*sizeof(POINT));
                        if(!more) break;
                        memcpy(more,stack,(size_t)room*sizeof(POINT)); gdi_free(stack); stack=more; room*=2;
                    }
                    stack[top].x=k; stack[top++].y=row;
                }
                in=ok;
            }
        }
    }
    gdi_free(f.seen); gdi_free(stack);
    return TRUE;
}
BOOL WINAPI FloodFill(HDC h,int x,int y,COLORREF c) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return mf_words(dc,META_FLOODFILL,4,(int)LOWORD(c),(int)HIWORD(c),y,x);
    return ExtFloodFill(h,x,y,c,FLOODFILLBORDER);
}

/* --- regions from shapes ---------------------------------------------------------------- */
static HRGN shape_region(const Shape *s) {
    RectList l; RowBuilder b; int y,top=(int)s->t,bottom=(int)(s->b+0.999),x[2];
    memset(&l,0,sizeof(l)); rows_begin(&b,&l);
    for(y=top;y<bottom;y++) if(span(s,y,&x[0],&x[1]) && !rows_add(&b,y,x,2)) {rl_free(&l); return NULL;}
    return new_region(&l);
}
HRGN WINAPI CreateEllipticRgn(int l,int t,int r,int b) {
    RECT rc; Shape s; r_set(&rc,min(l,r),min(t,b),max(l,r),max(t,b)); s=shape_of(1,&rc);
    return shape_region(&s);
}
HRGN WINAPI CreateEllipticRgnIndirect(const RECT FAR *r) {return r?CreateEllipticRgn(r->left,r->top,r->right,r->bottom):NULL;}
HRGN WINAPI CreateRoundRectRgn(int l,int t,int r,int b,int cw,int ch) {
    RECT rc; Shape s; r_set(&rc,min(l,r),min(t,b),max(l,r),max(t,b)); s=shape_of(2,&rc);
    s.cw=min(cw,(int)(s.r-s.l)); s.ch=min(ch,(int)(s.b-s.t));
    return shape_region(&s);
}
HRGN WINAPI CreatePolyPolygonRgn(const POINT FAR *p,const int FAR *counts,int n,int mode) {
    RectList l; RowBuilder b; int total=0,i,y,top,bottom,spans[CROSSINGS];
    if(!p || !counts || n<1) return NULL;
    for(i=0;i<n;i++) {if(counts[i]<2) return NULL; total+=counts[i];}
    memset(&l,0,sizeof(l)); rows_begin(&b,&l);
    poly_bounds(p,total,&top,&bottom);
    for(y=top;y<bottom;y++) {
        int k=poly_spans(p,counts,n,mode,y,spans,CROSSINGS);
        if(k && !rows_add(&b,y,spans,k)) {rl_free(&l); return NULL;}
    }
    return new_region(&l);
}
HRGN WINAPI CreatePolygonRgn(const POINT FAR *p,int n,int mode) {return CreatePolyPolygonRgn(p,&n,1,mode);}

/* --- regions ------------------------------------------------------------------------ */
static BOOL region_fill(HDC h,HRGN rgn,const Brush *b,int code) {
    DC *dc=dc_of(h); Region *g=(Region *)object(rgn,OBJ_REGION); int i;
    if(!dc || !g) return FALSE;
    for(i=0;i<g->r.count;i++) {
        RECT r=g->r.rects[i];
        dev_rect(dc,&r);
        if(b) fill_device(dc,&r,b,code); else fill_solid(dc,&r,0,code);
    }
    return TRUE;
}
BOOL WINAPI FillRgn(HDC h,HRGN rgn,HBRUSH brush) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return mf_region(dc,META_FILLREGION,rgn,brush,0,0);
    return region_fill(h,rgn,(Brush *)object(brush,OBJ_BRUSH),R2_COPYPEN);
}
BOOL WINAPI PaintRgn(HDC h,HRGN rgn) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return mf_region(dc,META_PAINTREGION,rgn,NULL,0,0);
    return dc && region_fill(h,rgn,(Brush *)object(dc->s.brush,OBJ_BRUSH),dc->s.rop2);
}
BOOL WINAPI InvertRgn(HDC h,HRGN rgn) {
    DC *dc=dc_of(h);
    if(META_DC(dc)) return mf_region(dc,META_INVERTREGION,rgn,NULL,0,0);
    return region_fill(h,rgn,NULL,R2_NOT);
}
/* Each rectangle of the region is framed. */
BOOL WINAPI FrameRgn(HDC h,HRGN rgn,HBRUSH brush,int w,int hh) {
    DC *dc=dc_of(h); Region *g=(Region *)object(rgn,OBJ_REGION); Brush *b=(Brush *)object(brush,OBJ_BRUSH); int i;
    if(!dc || !g || !b) return FALSE;
    if(dc->meta) return mf_region(dc,META_FRAMEREGION,rgn,brush,w,hh);
    for(i=0;i<g->r.count;i++) {
        RECT o=g->r.rects[i],e[4]; int k,fw=dev_w(dc,w),fh=dev_h(dc,hh);
        dev_rect(dc,&o);
        r_set(&e[0],o.left,o.top,o.right,o.top+fh); r_set(&e[1],o.left,o.bottom-fh,o.right,o.bottom);
        r_set(&e[2],o.left,o.top,o.left+fw,o.bottom); r_set(&e[3],o.right-fw,o.top,o.right,o.bottom);
        for(k=0;k<4;k++) fill_device(dc,&e[k],b,R2_COPYPEN);
    }
    return TRUE;
}
