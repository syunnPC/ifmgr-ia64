/* SPDX-License-Identifier: GPL-2.0-or-later
 * Startup screen remains until USER paints the desktop. Title/version
 * use Liberation Serif text rendered by tools/mksplash.py (splash_text.h);
 * credits use Wine's MS Sans Serif raster font and wrap at sentence boundaries
 * where possible.
 */
#include "splash.h"
#include "splash_text.h"
#include "font_sans13.h"

/* The credits, the screen's bottom lines. */
static const char *const credits[]={
    "Copyright (C) 2026 syunnPC; The source code is available at https://github.com/syunnPC/ifmgr-ia64",
    "This project is available under the GNU General Public License version 2 or later. "
    "This Program comes with ABSOLUTELY NO WARRANTY.",
};
#define GROUND 0x5454a8u /* blue-violet */
#define SHADOW 0x808080u
#define TEXT 0xffffffu

typedef struct {IoPixel *pixels; int width,height;} Canvas;
static IoPixel pixel(u32 rgb) {return (IoPixel){(u8)rgb,(u8)(rgb>>8),(u8)(rgb>>16),0};}
static void fill(Canvas *c,int x,int y,int w,int h,u32 rgb) {
    IoPixel p=pixel(rgb);
    for(int j=y<0?0:y;j<y+h && j<c->height;j++)
        for(int i=x<0?0:x;i<x+w && i<c->width;i++) c->pixels[j*c->width+i]=p;
}
/* A line of text from its runs of coverage (0 to 15), blended in. */
static void coverage(Canvas *c,const u8 *runs,u32 count,int w,int x,int y,u32 rgb) {
    int at=0;
    for(u32 r=0;r<count;r++) {
        int level=runs[r]>>4,n=(runs[r]&15)+1;
        for(;n--;at++) {
            int px=x+at%w,py=y+at/w;
            if(!level || px<0 || py<0 || px>=c->width || py>=c->height) continue;
            IoPixel *p=&c->pixels[py*c->width+px];
            p->red=(u8)((p->red*(15-level)+(rgb>>16&255)*level)/15);
            p->green=(u8)((p->green*(15-level)+(rgb>>8&255)*level)/15);
            p->blue=(u8)((p->blue*(15-level)+(rgb&255)*level)/15);
        }
    }
}
static void shadowed(Canvas *c,const u8 *runs,u32 count,int w,int x,int y,int offset) {
    coverage(c,runs,count,w,x+offset,y+offset,SHADOW);
    coverage(c,runs,count,w,x,y,TEXT);
}
static int text_width(const char *s,int n) {int w=0; while(n--) w+=font_sans13_widths[(u8)*s++]; return w;}
/* How much of s goes on a line of the width: all of it, else up to the
 * last sentence's end that fits, else up to the last word that does. */
static int fitting(const char *s,int width) {
    int n=0,w=0,sentence=0,word=0;
    for(;s[n];n++) {
        if((w+=font_sans13_widths[(u8)s[n]])>width) break;
        if(s[n]==' ') {word=n; if(n && s[n-1]=='.') sentence=n;}
    }
    return !s[n]?n:sentence?sentence:word?word:n?n:1;
}
static void text(Canvas *c,const char *s,int n,int x,int y,u32 rgb) {
    IoPixel p=pixel(rgb);
    for(;n--;s++) {
        u8 ch=(u8)*s; int w=font_sans13_widths[ch];
        for(int r=0;r<FONT_SANS13_HEIGHT;r++) for(int k=0;k<w;k++)
            if(font_sans13_bits[ch][r]&(0x80000000u>>k) && x+k>=0 && x+k<c->width && y+r>=0 && y+r<c->height)
                c->pixels[(y+r)*c->width+x+k]=p;
        x+=w;
    }
}
void splash_show(const IoServices *io) {
    IoDisplay d; void *memory; Canvas c;
    if(io->display_info(io->context,&d) || !d.width || !d.height) return;
    u32 pages=(u32)(((u64)d.width*d.height*sizeof(IoPixel)+4095)/4096);
    if(io->alloc_pages(io->context,pages,&memory)) return;
    c=(Canvas){memory,(int)d.width,(int)d.height};
    int w=c.width,h=c.height,line=FONT_SANS13_HEIGHT+5;
    fill(&c,0,0,w,h,GROUND);
    int title_y=h*31/100,version_y=title_y+TITLE_HEIGHT+h/60;
    shadowed(&c,title_runs,sizeof(title_runs),TITLE_WIDTH,(w-TITLE_WIDTH)/2,title_y,4);
    shadowed(&c,version_runs,sizeof(version_runs),VERSION_WIDTH,(w-VERSION_WIDTH)/2,version_y,2);
    /* The credits' lines (counted first), each centred, ending a twentieth of the height above the bottom. */
    int lines=0,n=(int)(sizeof(credits)/sizeof(credits[0]));
    for(int pass=0;pass<2;pass++) {
        int y=h-h/20-lines*line;
        for(int i=0;i<n;i++) for(const char *s=credits[i];*s;) {
            int k=fitting(s,w-16);
            if(pass) {text(&c,s,k,(w-text_width(s,k))/2,y,TEXT); y+=line;} else lines++;
            for(s+=k;*s==' ';s++) {}
        }
    }
    io->display_blt(io->context,c.pixels,0,0,d.width,d.height,d.width,0);
    io->free_pages(io->context,memory,pages);
}
