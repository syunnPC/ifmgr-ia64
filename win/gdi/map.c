/* SPDX-License-Identifier: GPL-2.0-or-later
 * Mapping modes: a DC's window (logical) and viewport (device) origins and
 * extents, the functions that set them (Windows 3.0's, which return the old
 * values packed in a DWORD, and the Win32 Ex forms), and the conversions the
 * drawing functions use. The metric and English modes take the device's
 * dots per inch (96 for the display), with y growing upward.
 */
#include "gdip.h"

void map_reset(DCState *s) {
    s->map_mode=MM_TEXT;
    s->window_org.x=s->window_org.y=s->viewport_org.x=s->viewport_org.y=0;
    s->window_ext.x=s->window_ext.y=s->viewport_ext.x=s->viewport_ext.y=1;
}
/* v * num / den, rounded half away from zero. */
static int scale(LONGLONG v,LONG num,LONG den) {
    LONGLONG n=v*num,d=den;
    if(!d) return (int)v;
    if(d<0) {n=-n; d=-d;}
    return (int)(n>=0?(n+d/2)/d:-((-n+d/2)/d));
}
int dev_x(const DC *dc,int x) {
    const DCState *s=&dc->s; int v=x-s->window_org.x;
    if(s->viewport_ext.x!=s->window_ext.x) v=scale(v,s->viewport_ext.x,s->window_ext.x);
    return v+s->viewport_org.x+dc->origin.x;
}
int dev_y(const DC *dc,int y) {
    const DCState *s=&dc->s; int v=y-s->window_org.y;
    if(s->viewport_ext.y!=s->window_ext.y) v=scale(v,s->viewport_ext.y,s->window_ext.y);
    return v+s->viewport_org.y+dc->origin.y;
}
void dev_point(const DC *dc,POINT *p) {p->x=dev_x(dc,p->x); p->y=dev_y(dc,p->y);}
void dev_rect(const DC *dc,RECT *r) {
    int l=dev_x(dc,r->left),t=dev_y(dc,r->top),rt=dev_x(dc,r->right),b=dev_y(dc,r->bottom);
    r_set(r,min(l,rt),min(t,b),max(l,rt),max(t,b));
}
int dev_w(const DC *dc,int w) {
    int v=dc->s.viewport_ext.x==dc->s.window_ext.x?w:scale(w,dc->s.viewport_ext.x,dc->s.window_ext.x);
    return v<0?-v:v;
}
int dev_h(const DC *dc,int h) {
    int v=dc->s.viewport_ext.y==dc->s.window_ext.y?h:scale(h,dc->s.viewport_ext.y,dc->s.window_ext.y);
    return v<0?-v:v;
}
int log_x(const DC *dc,int x) {
    const DCState *s=&dc->s; int v=x-dc->origin.x-s->viewport_org.x;
    if(s->viewport_ext.x!=s->window_ext.x) v=scale(v,s->window_ext.x,s->viewport_ext.x);
    return v+s->window_org.x;
}
int log_y(const DC *dc,int y) {
    const DCState *s=&dc->s; int v=y-dc->origin.y-s->viewport_org.y;
    if(s->viewport_ext.y!=s->window_ext.y) v=scale(v,s->window_ext.y,s->viewport_ext.y);
    return v+s->window_org.y;
}
int log_w(const DC *dc,int w) {
    int v=dc->s.viewport_ext.x==dc->s.window_ext.x?w:scale(w,dc->s.window_ext.x,dc->s.viewport_ext.x);
    return v<0?-v:v;
}
int log_h(const DC *dc,int h) {
    int v=dc->s.viewport_ext.y==dc->s.window_ext.y?h:scale(h,dc->s.window_ext.y,dc->s.viewport_ext.y);
    return v<0?-v:v;
}

/* --- mapping modes ------------------------------------------------------------- */
static BOOL scalable(const DCState *s) {return s->map_mode==MM_ISOTROPIC || s->map_mode==MM_ANISOTROPIC;}
/* MM_ISOTROPIC keeps one unit the same size along both axes: the viewport
 * extent along the axis with the larger scale shrinks. */
static void fix_isotropic(DCState *s) {
    double x,y;
    if(s->map_mode!=MM_ISOTROPIC || !s->window_ext.x || !s->window_ext.y) return;
    x=(double)s->viewport_ext.x/s->window_ext.x; y=(double)s->viewport_ext.y/s->window_ext.y;
    if(x<0) x=-x;
    if(y<0) y=-y;
    if(x>y) {
        int sign=s->viewport_ext.x>=0?1:-1;
        s->viewport_ext.x=(int)(s->viewport_ext.x*y/x+sign*0.5);
        if(!s->viewport_ext.x) s->viewport_ext.x=sign;
    } else if(y>x) {
        int sign=s->viewport_ext.y>=0?1:-1;
        s->viewport_ext.y=(int)(s->viewport_ext.y*x/y+sign*0.5);
        if(!s->viewport_ext.y) s->viewport_ext.y=sign;
    }
}
int WINAPI SetMapMode(HDC h,int mode) {
    DC *dc=dc_of(h); DCState *s; int old,units;
    if(!dc || mode<MM_TEXT || mode>MM_ANISOTROPIC) return 0;
    s=&dc->s; old=s->map_mode;
    if(dc->meta) {mf_words(dc,META_SETMAPMODE,1,mode); s->map_mode=mode; return old;}
    if(mode==old && scalable(s)) return old;
    switch(mode) {
    case MM_LOMETRIC: case MM_ISOTROPIC: units=254; break;
    case MM_HIMETRIC: units=2540; break;
    case MM_LOENGLISH: units=100; break;
    case MM_HIENGLISH: units=1000; break;
    case MM_TWIPS: units=1440; break;
    default: units=0;
    }
    if(mode==MM_TEXT) {s->window_ext.x=s->window_ext.y=s->viewport_ext.x=s->viewport_ext.y=1;}
    else if(units) {s->window_ext.x=s->window_ext.y=units; s->viewport_ext.x=dc->dpi; s->viewport_ext.y=-dc->dpi;}
    s->map_mode=mode;
    return old;
}
int WINAPI GetMapMode(HDC h) {DC *dc=dc_of(h); return dc?dc->s.map_mode:0;}

/* Each value is a point in the DC's state; the setters return the old one. */
static BOOL set_point(HDC h,POINT *(*field)(DCState *),int x,int y,BOOL offset,LPPOINT old,WORD function) {
    DC *dc=dc_of(h); POINT *p;
    if(!dc) return FALSE;
    p=field(&dc->s);
    if(old) *old=*p;
    if(dc->meta) return mf_words(dc,function,2,y,x);
    if(offset) {p->x+=x; p->y+=y;} else {p->x=x; p->y=y;}
    return TRUE;
}
static BOOL set_extent(HDC h,POINT *(*field)(DCState *),int x,int y,LPSIZE old,WORD function) {
    DC *dc=dc_of(h); POINT *p;
    if(!dc) return FALSE;
    p=field(&dc->s);
    if(old) {old->cx=p->x; old->cy=p->y;}
    if(dc->meta) return mf_words(dc,function,2,y,x);
    if(!scalable(&dc->s)) return TRUE;
    if(!x || !y) return FALSE;
    p->x=x; p->y=y; fix_isotropic(&dc->s);
    return TRUE;
}
static BOOL get_point(HDC h,POINT *(*field)(DCState *),LPPOINT out) {
    DC *dc=dc_of(h);
    if(!dc || !out) return FALSE;
    *out=*field(&dc->s); return TRUE;
}
static POINT *window_org(DCState *s) {return &s->window_org;}
static POINT *window_ext(DCState *s) {return &s->window_ext;}
static POINT *viewport_org(DCState *s) {return &s->viewport_org;}
static POINT *viewport_ext(DCState *s) {return &s->viewport_ext;}
static DWORD packed(BOOL ok,LONG x,LONG y) {return ok?MAKELONG(x,y):0;}

BOOL WINAPI SetWindowOrgEx(HDC h,int x,int y,LPPOINT old) {return set_point(h,window_org,x,y,FALSE,old,META_SETWINDOWORG);}
BOOL WINAPI SetViewportOrgEx(HDC h,int x,int y,LPPOINT old) {return set_point(h,viewport_org,x,y,FALSE,old,META_SETVIEWPORTORG);}
BOOL WINAPI OffsetWindowOrgEx(HDC h,int x,int y,LPPOINT old) {return set_point(h,window_org,x,y,TRUE,old,META_OFFSETWINDOWORG);}
BOOL WINAPI OffsetViewportOrgEx(HDC h,int x,int y,LPPOINT old) {return set_point(h,viewport_org,x,y,TRUE,old,META_OFFSETVIEWPORTORG);}
BOOL WINAPI SetWindowExtEx(HDC h,int x,int y,LPSIZE old) {return set_extent(h,window_ext,x,y,old,META_SETWINDOWEXT);}
BOOL WINAPI SetViewportExtEx(HDC h,int x,int y,LPSIZE old) {return set_extent(h,viewport_ext,x,y,old,META_SETVIEWPORTEXT);}
BOOL WINAPI GetWindowOrgEx(HDC h,LPPOINT out) {return get_point(h,window_org,out);}
BOOL WINAPI GetViewportOrgEx(HDC h,LPPOINT out) {return get_point(h,viewport_org,out);}
BOOL WINAPI GetWindowExtEx(HDC h,LPSIZE out) {
    POINT p; if(!out || !get_point(h,window_ext,&p)) return FALSE;
    out->cx=p.x; out->cy=p.y; return TRUE;
}
BOOL WINAPI GetViewportExtEx(HDC h,LPSIZE out) {
    POINT p; if(!out || !get_point(h,viewport_ext,&p)) return FALSE;
    out->cx=p.x; out->cy=p.y; return TRUE;
}
static BOOL scale_extent(HDC h,POINT *(*field)(DCState *),int xn,int xd,int yn,int yd,LPSIZE old,WORD function) {
    DC *dc=dc_of(h); POINT *p;
    if(!dc) return FALSE;
    p=field(&dc->s);
    if(old) {old->cx=p->x; old->cy=p->y;}
    if(dc->meta) return mf_words(dc,function,4,yd,yn,xd,xn);
    if(!scalable(&dc->s)) return TRUE;
    if(!xd || !yd) return FALSE;
    if(!scale(p->x,xn,xd) || !scale(p->y,yn,yd)) return FALSE;
    p->x=scale(p->x,xn,xd); p->y=scale(p->y,yn,yd); fix_isotropic(&dc->s);
    return TRUE;
}
BOOL WINAPI ScaleWindowExtEx(HDC h,int xn,int xd,int yn,int yd,LPSIZE old) {return scale_extent(h,window_ext,xn,xd,yn,yd,old,META_SCALEWINDOWEXT);}
BOOL WINAPI ScaleViewportExtEx(HDC h,int xn,int xd,int yn,int yd,LPSIZE old) {return scale_extent(h,viewport_ext,xn,xd,yn,yd,old,META_SCALEVIEWPORTEXT);}

DWORD WINAPI SetWindowOrg(HDC h,int x,int y) {POINT o; return packed(SetWindowOrgEx(h,x,y,&o),o.x,o.y);}
DWORD WINAPI SetViewportOrg(HDC h,int x,int y) {POINT o; return packed(SetViewportOrgEx(h,x,y,&o),o.x,o.y);}
DWORD WINAPI OffsetWindowOrg(HDC h,int x,int y) {POINT o; return packed(OffsetWindowOrgEx(h,x,y,&o),o.x,o.y);}
DWORD WINAPI OffsetViewportOrg(HDC h,int x,int y) {POINT o; return packed(OffsetViewportOrgEx(h,x,y,&o),o.x,o.y);}
DWORD WINAPI SetWindowExt(HDC h,int x,int y) {SIZE o; return packed(SetWindowExtEx(h,x,y,&o),o.cx,o.cy);}
DWORD WINAPI SetViewportExt(HDC h,int x,int y) {SIZE o; return packed(SetViewportExtEx(h,x,y,&o),o.cx,o.cy);}
DWORD WINAPI ScaleWindowExt(HDC h,int xn,int xd,int yn,int yd) {SIZE o; return packed(ScaleWindowExtEx(h,xn,xd,yn,yd,&o),o.cx,o.cy);}
DWORD WINAPI ScaleViewportExt(HDC h,int xn,int xd,int yn,int yd) {SIZE o; return packed(ScaleViewportExtEx(h,xn,xd,yn,yd,&o),o.cx,o.cy);}
DWORD WINAPI GetWindowOrg(HDC h) {POINT p; return packed(GetWindowOrgEx(h,&p),p.x,p.y);}
DWORD WINAPI GetViewportOrg(HDC h) {POINT p; return packed(GetViewportOrgEx(h,&p),p.x,p.y);}
DWORD WINAPI GetWindowExt(HDC h) {SIZE s; return packed(GetWindowExtEx(h,&s),s.cx,s.cy);}
DWORD WINAPI GetViewportExt(HDC h) {SIZE s; return packed(GetViewportExtEx(h,&s),s.cx,s.cy);}

/* Device points here are relative to the DC, as the application sees them. */
BOOL WINAPI LPtoDP(HDC h,LPPOINT p,int n) {
    DC *dc=dc_of(h); int i;
    if(!dc || (!p && n)) return FALSE;
    for(i=0;i<n;i++) {p[i].x=dev_x(dc,p[i].x)-dc->origin.x; p[i].y=dev_y(dc,p[i].y)-dc->origin.y;}
    return TRUE;
}
BOOL WINAPI DPtoLP(HDC h,LPPOINT p,int n) {
    DC *dc=dc_of(h); int i;
    if(!dc || (!p && n)) return FALSE;
    for(i=0;i<n;i++) {p[i].x=log_x(dc,p[i].x+dc->origin.x); p[i].y=log_y(dc,p[i].y+dc->origin.y);}
    return TRUE;
}
BOOL WINAPI GetCurrentPositionEx(HDC h,LPPOINT out) {
    DC *dc=dc_of(h);
    if(!dc || !out) return FALSE;
    *out=dc->s.pos; return TRUE;
}
