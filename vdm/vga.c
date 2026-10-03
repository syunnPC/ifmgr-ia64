/* SPDX-License-Identifier: GPL-2.0-or-later
 * INT 10h VGA BIOS. IO_DISPLAY_VGA_TEXT makes the adapter the DOS
 * console; guests can access its memory and ports directly. Exit preserves
 * text mode, or restores mode 3 after graphics mode.
 *
 * IBM register tables supply modes 0-3/7 (400 lines, 8x16 cells), CGA 4-6,
 * EGA 0Dh/0Eh/10h and VGA 11h-13h with standard palettes. BIOS data tracks
 * mode, pages, cursors and character height. Graphics text uses INT 43h's
 * font, or INT 1Fh for CGA characters 80h-FFh. The 8x8/8x14/8x16 fonts and
 * functionality table occupy conventional memory below the arena, exposed
 * through those vectors, AX=1130h and AH=1Bh.
 *
 * IO.SYS display_console routes DOS and native-child output to the VGA.
 * Text uses light grey until ANSI.SYS supplies an attribute via display_text;
 * LF starts a new line. BIOS teletype output also reaches serial.
 */
#include "vdm.h"
#include "vdm_fonts.h"
#define BDA 0x400U

enum {TEXT,MONO,CGA4,CGA2,PLANAR,LINEAR};
enum {DAC_EGA,DAC_CGA,DAC_MONO,DAC_256};
typedef struct {
    u8 mode,kind,dac,misc,seq[4],crtc[25],attr[20],gfx[9];
    u8 cols,rows,height,pages,mode_control,palette;
    u16 width,lines,page_size;
} Mode;
#define TEXT40 {0x2d,0x27,0x28,0x90,0x2b,0xa0,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x14,0x1f,0x96,0xb9,0xa3,0xff}
#define TEXT80 {0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff}
#define EGA_PALETTE 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f
#define CGA_PALETTE 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17
#define TEXT_ATTR {EGA_PALETTE,0x0c,0x00,0x0f,0x08}
#define TEXT_GFX {0x00,0x00,0x00,0x00,0x00,0x10,0x0e,0x00,0xff}
#define PLANAR_GFX {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff}
#define CRTC_200 0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x28,0x00,0x96,0xb9
#define CRTC_480 0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0xea,0x8c,0xdf,0x28,0x00,0xe7,0x04
static const Mode modes[]={
    {0x00,TEXT,DAC_EGA,0x67,{0x08,0x03,0x00,0x02},TEXT40,TEXT_ATTR,TEXT_GFX,40,25,16,8,0x2c,0x30,360,400,0x0800},
    {0x01,TEXT,DAC_EGA,0x67,{0x08,0x03,0x00,0x02},TEXT40,TEXT_ATTR,TEXT_GFX,40,25,16,8,0x28,0x30,360,400,0x0800},
    {0x02,TEXT,DAC_EGA,0x67,{0x00,0x03,0x00,0x02},TEXT80,TEXT_ATTR,TEXT_GFX,80,25,16,8,0x2d,0x30,720,400,0x1000},
    {0x03,TEXT,DAC_EGA,0x67,{0x00,0x03,0x00,0x02},TEXT80,TEXT_ATTR,TEXT_GFX,80,25,16,8,0x29,0x30,720,400,0x1000},
    {0x04,CGA4,DAC_CGA,0x63,{0x09,0x03,0x00,0x02},
     {0x2d,0x27,0x28,0x90,0x2b,0x80,0xbf,0x1f,0x00,0xc1,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x14,0x00,0x96,0xb9,0xa2,0xff},
     {0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x03,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x30,0x0f,0x00,0xff},40,25,8,1,0x2a,0x30,320,200,0x4000},
    {0x05,CGA4,DAC_CGA,0x63,{0x09,0x03,0x00,0x02},
     {0x2d,0x27,0x28,0x90,0x2b,0x80,0xbf,0x1f,0x00,0xc1,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x14,0x00,0x96,0xb9,0xa2,0xff},
     {0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x01,0x00,0x03,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x30,0x0f,0x00,0xff},40,25,8,1,0x2e,0x30,320,200,0x4000},
    {0x06,CGA2,DAC_CGA,0x63,{0x01,0x01,0x00,0x06},
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0xc1,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x28,0x00,0x96,0xb9,0xc2,0xff},
     {0x00,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x01,0x00,0x01,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x00,0x0d,0x00,0xff},80,25,8,1,0x1e,0x3f,640,200,0x4000},
    {0x07,MONO,DAC_MONO,0x66,{0x00,0x03,0x00,0x02},
     {0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x28,0x0f,0x96,0xb9,0xa3,0xff},
     {0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x10,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x0e,0x00,0x0f,0x08},
     {0x00,0x00,0x00,0x00,0x00,0x10,0x0a,0x00,0xff},80,25,16,8,0x29,0x30,720,400,0x1000},
    {0x0d,PLANAR,DAC_CGA,0x63,{0x09,0x0f,0x00,0x06},
     {0x2d,0x27,0x28,0x90,0x2b,0x80,0xbf,0x1f,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x14,0x00,0x96,0xb9,0xe3,0xff},
     {CGA_PALETTE,0x01,0x00,0x0f,0x00},PLANAR_GFX,40,25,8,8,0x29,0x30,320,200,0x2000},
    {0x0e,PLANAR,DAC_CGA,0x63,{0x01,0x0f,0x00,0x06},{CRTC_200,0xe3,0xff},
     {CGA_PALETTE,0x01,0x00,0x0f,0x00},PLANAR_GFX,80,25,8,4,0x29,0x30,640,200,0x4000},
    {0x10,PLANAR,DAC_EGA,0xa3,{0x01,0x0f,0x00,0x06},
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x83,0x85,0x5d,0x28,0x0f,0x63,0xba,0xe3,0xff},
     {EGA_PALETTE,0x01,0x00,0x0f,0x00},PLANAR_GFX,80,25,14,2,0x29,0x30,640,350,0x8000},
    {0x11,PLANAR,DAC_EGA,0xe3,{0x01,0x0f,0x00,0x06},{CRTC_480,0xc3,0xff},
     {0x00,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x01,0x00,0x0f,0x00},
     PLANAR_GFX,80,30,16,1,0x29,0x30,640,480,0xa000},
    {0x12,PLANAR,DAC_EGA,0xe3,{0x01,0x0f,0x00,0x06},{CRTC_480,0xe3,0xff},
     {EGA_PALETTE,0x01,0x00,0x0f,0x00},PLANAR_GFX,80,30,16,1,0x29,0x30,640,480,0xa000},
    {0x13,LINEAR,DAC_256,0x63,{0x01,0x0f,0x00,0x0e},
     {0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x9c,0x8e,0x8f,0x28,0x40,0x96,0xb9,0xa3,0xff},
     {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x41,0x00,0x0f,0x00},
     {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0f,0xff},40,25,8,1,0x29,0x30,320,200,0xfa00},
};

static u64 screen;
static int screen_state; /* 1 while the program has the adapter, -1 while it is set aside */
static int quiet; /* console text the BIOS has drawn already */
static const Mode *cur;
static unsigned rows,height; /* text rows and character height now */

/* --- the adapter ------------------------------------------------------------ */
static u8 in(u16 port) {u8 v=0xff; io->vga_port(io->context,screen,port,&v,0); return v;}
static void out(u16 port,u8 v) {io->vga_port(io->context,screen,port,&v,1);}
static void seq(u8 index,u8 v) {out(0x3c4,index); out(0x3c5,v);}
static void gc(u8 index,u8 v) {out(0x3ce,index); out(0x3cf,v);}
static u16 crtc_port(void) {return peek16(BDA+0x63)==0x3b4?0x3b4:0x3d4;}
static void crtc(u8 index,u8 v) {u16 p=crtc_port(); out(p,index); out((u16)(p+1),v);}
static u8 crtc_in(u8 index) {u16 p=crtc_port(); out(p,index); return in((u16)(p+1));}
/* Attribute controller registers; the screen is blanked while a palette
 * register is addressed and turned on again after. */
static void attr_out(u8 index,u8 v) {(void)in((u16)(crtc_port()+6)); out(0x3c0,index); out(0x3c0,v); out(0x3c0,0x20);}
static u8 attr_in(u8 index) {
    u16 status=(u16)(crtc_port()+6);
    (void)in(status); out(0x3c0,index); u8 v=in(0x3c1); (void)in(status); out(0x3c0,0x20);
    return v;
}
static volatile u8 *vmem(u32 linear) {return (volatile u8 *)(uintptr_t)(0x8000000000000000ULL|linear);}
static void dac(u8 r,u8 g,u8 b) {out(0x3c9,r); out(0x3c9,g); out(0x3c9,b);}
static void load_dac(unsigned kind) {
    static const u8 grey[16]={0,5,8,11,14,17,20,24,28,32,36,40,45,50,56,63};
    static const u8 levels[9][5]={{0,16,31,47,63},{31,39,47,55,63},{45,49,54,58,63},{0,7,14,21,28},{14,17,21,24,28},
                                  {20,22,24,26,28},{0,4,8,12,16},{8,10,12,14,16},{11,12,13,15,16}};
    static const u8 hue[24][3]={{0,0,4},{1,0,4},{2,0,4},{3,0,4},{4,0,4},{4,0,3},{4,0,2},{4,0,1},{4,0,0},{4,1,0},{4,2,0},{4,3,0},
                                {4,4,0},{3,4,0},{2,4,0},{1,4,0},{0,4,0},{0,4,1},{0,4,2},{0,4,3},{0,4,4},{0,3,4},{0,2,4},{0,1,4}};
    out(0x3c6,0xff); out(0x3c8,0);
    if(kind==DAC_256) {
        for(unsigned c=0;c<16;c++) {
            u8 hi=c&8?0x15:0;
            dac((u8)((c&4?0x2a:0)+hi),(u8)(c==6?0x15:(c&2?0x2a:0)+hi),(u8)((c&1?0x2a:0)+hi));
        }
        for(unsigned c=0;c<16;c++) dac(grey[c],grey[c],grey[c]);
        for(unsigned g=0;g<9;g++) for(unsigned h=0;h<24;h++) dac(levels[g][hue[h][0]],levels[g][hue[h][1]],levels[g][hue[h][2]]);
        for(unsigned c=248;c<256;c++) dac(0,0,0);
        return;
    }
    for(unsigned c=0;c<64;c++) {
        if(kind==DAC_EGA) {
            dac((u8)((c&4?0x2a:0)|(c&0x20?0x15:0)),(u8)((c&2?0x2a:0)|(c&0x10?0x15:0)),(u8)((c&1?0x2a:0)|(c&0x08?0x15:0)));
        } else if(kind==DAC_CGA) {
            u8 hi=c&0x10?0x15:0;
            dac((u8)((c&4?0x2a:0)+hi),(u8)((c&0x17)==6?0x15:(c&2?0x2a:0)+hi),(u8)((c&1?0x2a:0)+hi));
        } else {
            u8 v=(u8)((c&8?0x2a:0)+(c&0x10?0x15:0)); dac(v,v,v);
        }
    }
}
/* Characters into the character generator (plane 2): count from first,
 * bytes_per character rows each, in 32-byte slots of block 0. */
static void load_font(const u8 *font,unsigned bytes,unsigned first,unsigned count) {
    seq(0,0x01); seq(2,0x04); seq(4,0x07); seq(0,0x03);
    gc(4,0x02); gc(5,0x00); gc(6,0x04);
    for(unsigned c=0;c<count && first+c<256;c++)
        for(unsigned r=0;r<32;r++) *vmem(0xa0000+(first+c)*32+r)=r<bytes?font[c*bytes+r]:0;
    seq(0,0x01); seq(2,cur->seq[1]); seq(4,cur->seq[3]); seq(0,0x03);
    gc(4,cur->gfx[4]); gc(5,cur->gfx[5]); gc(6,cur->gfx[6]);
}

/* --- fonts in conventional memory ---------------------------------------- */
static u32 font_vector(unsigned bytes) {
    u16 off=bytes==8?FONT8_OFF:bytes==14?FONT14_OFF:FONT16_OFF;
    return (u32)VIDEO_SEG<<16|off;
}
/* Modes 0-7, 0Dh, 0Eh and 10h-13h; 400 scan lines in text modes; font
 * loading, palettes, the DAC, cursor emulation, blinking and the display
 * combination code. */
static const u8 functionality[16]={0xff,0x60,0x0f,0,0,0,0,0x04,8,2,0x7d,0x0c,0,0,0,0};
void vga_init(void) {
    memcpy(LIN(VIDEO_SEG,FONT8_OFF),vga_font8,sizeof vga_font8);
    memcpy(LIN(VIDEO_SEG,FONT14_OFF),vga_font14,sizeof vga_font14);
    memcpy(LIN(VIDEO_SEG,FONT16_OFF),vga_font16,sizeof vga_font16);
    memcpy(LIN(VIDEO_SEG,FUNCTIONALITY_OFF),functionality,sizeof functionality);
    poke16(((u32)VIDEO_SEG<<4)+NO_ALTERNATES_OFF,0);
    set_ivt(0x1f,font_vector(8)+128*8);
    set_ivt(0x43,font_vector(8));
    /* GRAFTABL's code page (DosApi installed) for the CGA modes' characters
     * 80h-FFh, as its table would be resident under DOS 4. */
    {
        u32 page=0;
        if(!dos_installed(DOS_INSTALLED_GRAFTABL,NULL,&page))
            for(unsigned i=0;i<ARRAY_SIZE(vga_graftabl_pages);i++) if(vga_graftabl_pages[i]==page) {
                memcpy(LIN(VIDEO_SEG,GRAFTABL_OFF),vga_graftabl[i],sizeof vga_graftabl[i]);
                set_ivt(0x1f,(u32)VIDEO_SEG<<16|GRAFTABL_OFF);
            }
    }
    screen_state=0; cur=NULL;
}
/* The CGA modes take characters 80h-FFh from INT 1Fh. */
static const u8 *glyph(u8 c) {
    int upper=(cur->kind==CGA4 || cur->kind==CGA2) && c>=128;
    u32 v=ivt(upper?0x1f:0x43);
    return LIN(v>>16,(u16)v+(upper?c-128u:c)*height);
}

/* --- pages and the cursor ------------------------------------------------- */
static int graphics(void) {return cur->kind!=TEXT && cur->kind!=MONO;}
static unsigned cols(void) {return cur->cols;}
static u8 page(void) {return *LINEAR(BDA+0x62)&7;}
static u32 page_base(u8 p) {return (u32)p*peek16(BDA+0x4c);}
static unsigned pages(void) {
    if(graphics()) return cur->pages;
    unsigned n=0x8000U/peek16(BDA+0x4c);
    return n>8?8:n;
}
static u8 cur_row(u8 p) {return *LINEAR(BDA+0x51+(p&7)*2);}
static u8 cur_col(u8 p) {return *LINEAR(BDA+0x50+(p&7)*2);}
static void set_cursor(u8 p,u8 row,u8 col) {
    p&=7; *LINEAR(BDA+0x50+p*2)=col; *LINEAR(BDA+0x51+p*2)=row;
    if(p!=page() || graphics()) return;
    u16 pos=(u16)(page_base(p)/2+row*cols()+col);
    crtc(0x0e,(u8)(pos>>8)); crtc(0x0f,(u8)pos);
}
static void select_page(u8 p) {
    if(p>=pages()) return;
    *LINEAR(BDA+0x62)=p; poke16(BDA+0x4e,(u16)page_base(p));
    unsigned shift=cur->kind==PLANAR?0:cur->kind==LINEAR?2:1;
    u16 start=(u16)(page_base(p)>>shift);
    crtc(0x0c,(u8)(start>>8)); crtc(0x0d,(u8)start);
    set_cursor(p,cur_row(p),cur_col(p));
}
/* A cursor shape for 8-line characters is scaled to the cell unless the
 * program turned that off (AX=1201h BL=34h). */
static u8 cursor_line(u8 v) {return v<4?(u8)(v*height/8):(u8)(height-(8u-v)-1u);}
static void cursor_shape(u16 shape) {
    u8 start=(u8)(shape>>8),end=(u8)(shape&0x1f);
    poke16(BDA+0x60,shape);
    if(graphics()) return;
    if((start&0x60)==0x20) {crtc(0x0a,0x20); return;}
    start&=0x1f;
    if(!(*LINEAR(BDA+0x87)&1) && height>8 && start<8 && end<8) {start=cursor_line(start); end=cursor_line(end);}
    crtc(0x0a,start); crtc(0x0b,end);
}

/* --- characters ------------------------------------------------------------- */
static u32 text_base(void) {return cur->kind==MONO?0xb0000:0xb8000;}
static volatile u8 *cell(u8 p,unsigned row,unsigned col) {return vmem(text_base()+((page_base(p)+(row*cols()+col)*2)&0x7fff));}
/* Bytes of one scan line of a graphics page, and where it is. */
static unsigned line_bytes(void) {return cur->kind==LINEAR?cur->width:cur->kind==CGA4?cur->width/4u:cur->width/8u;}
static volatile u8 *scan(u8 p,unsigned y,unsigned offset) {
    if(cur->kind==CGA4 || cur->kind==CGA2) return vmem(0xb8000+(y&1)*0x2000+(y>>1)*line_bytes()+offset);
    return vmem(0xa0000+((page_base(p)+y*line_bytes()+offset)&0xffff));
}
/* A character in a graphics mode in color (bit 7: exclusive or, except
 * with 256 colors), its background 0 otherwise. */
static void draw_char(u8 p,unsigned row,unsigned col,u8 c,u8 color) {
    const u8 *g=glyph(c); int xor=(color&0x80) && cur->kind!=LINEAR;
    unsigned y=row*height;
    switch(cur->kind) {
    case LINEAR:
        for(unsigned r=0;r<height;r++) {
            volatile u8 *d=scan(p,y+r,col*8);
            for(unsigned b=0;b<8;b++) d[b]=g[r]&0x80>>b?color:0;
        }
        break;
    case PLANAR:
        gc(1,0); gc(3,xor?0x18:0); gc(5,0); gc(8,0xff);
        for(u8 plane=0;plane<4;plane++) {
            seq(2,(u8)(1<<plane));
            for(unsigned r=0;r<height;r++) {
                volatile u8 *d=scan(p,y+r,col); u8 bits=color>>plane&1?g[r]:0;
                if(xor) (void)*d;
                *d=bits;
            }
        }
        seq(2,0x0f); gc(3,0);
        break;
    case CGA4:
        for(unsigned r=0;r<height;r++) {
            volatile u8 *d=scan(p,y+r,col*2); u16 w=0;
            for(unsigned b=0;b<8;b++) if(g[r]&0x80>>b) w|=(u16)((color&3)<<(14-2*b));
            if(xor) {d[0]^=(u8)(w>>8); d[1]^=(u8)w;} else {d[0]=(u8)(w>>8); d[1]=(u8)w;}
        }
        break;
    case CGA2:
        for(unsigned r=0;r<height;r++) {
            volatile u8 *d=scan(p,y+r,col); u8 bits=color&1?g[r]:0;
            if(xor) *d^=bits; else *d=bits;
        }
        break;
    }
}
/* A character at a position: the attribute (text) or color (graphics);
 * attr<0 keeps a text cell's attribute. */
static void put_char(u8 p,unsigned row,unsigned col,u8 c,int attr) {
    if(graphics()) {draw_char(p,row,col,c,attr<0?7:(u8)attr); return;}
    volatile u8 *d=cell(p,row,col); d[0]=c;
    if(attr>=0) d[1]=(u8)attr;
}

static void fill_scan(u8 p,unsigned y,unsigned from,unsigned bytes,u8 color) {
    u8 v=cur->kind==CGA4?(u8)((color&3)*0x55):cur->kind==CGA2?(color&1?0xff:0):color;
    volatile u8 *d=scan(p,y,from);
    for(unsigned i=0;i<bytes;i++) d[i]=v;
}
/* Rows scrolled up (or down) in a window of page p; 0 rows clears it.
 * New rows get attr (text) or color (graphics). */
static void scroll(u8 p,unsigned lines,int down,u8 attr,u8 top,u8 left,u8 bottom,u8 right) {
    if(bottom>=rows) bottom=(u8)(rows-1);
    if(right>=cols()) right=(u8)(cols()-1);
    if(top>bottom || left>right) return;
    unsigned span=bottom-top+1u;
    if(!lines || lines>span) lines=span;
    unsigned unit=cur->kind==LINEAR?8:cur->kind==CGA4?2:1,from=left*unit,bytes=(right-left+1u)*unit;
    if(graphics() && cur->kind==PLANAR) {gc(1,0x0f); gc(0,attr&0x0f); gc(3,0); gc(8,0xff);}
    for(unsigned i=0;i<span;i++) {
        unsigned row=down?bottom-i:top+i,src=down?row-lines:row+lines;
        int keep=i<span-lines;
        if(!graphics()) {
            for(unsigned col=left;col<=right;col++) {
                volatile u8 *d=cell(p,row,col);
                if(keep) {volatile u8 *s=cell(p,src,col); d[0]=s[0]; d[1]=s[1];} else {d[0]=' '; d[1]=attr;}
            }
            continue;
        }
        for(unsigned r=0;r<height;r++) {
            if(!keep) {
                if(cur->kind==PLANAR) {gc(5,0); volatile u8 *d=scan(p,row*height+r,from); for(unsigned b=0;b<bytes;b++) {(void)d[b]; d[b]=0;}}
                else fill_scan(p,row*height+r,from,bytes,attr);
                continue;
            }
            volatile u8 *d=scan(p,row*height+r,from),*s=scan(p,src*height+r,from);
            if(cur->kind==PLANAR) gc(5,1); /* the latches carry all four planes */
            for(unsigned b=0;b<bytes;b++) {u8 v=s[b]; d[b]=v;}
        }
    }
    if(graphics() && cur->kind==PLANAR) {gc(5,0); gc(1,0); gc(0,0);}
}
static void clear_page(u8 p,u8 attr) {scroll(p,0,0,graphics()?0:attr,0,0,(u8)(rows-1),(u8)(cols()-1));}
/* A character as the teletype writes it (BEL, BS, LF and CR act), with
 * attr or color, or keeping the cell's (attr<0); the page scrolls at its
 * end. */
static void advance(u8 p,u8 *row,u8 *col,u8 c,int attr) {
    unsigned r=*row<rows?*row:rows-1,k=*col<cols()?*col:cols()-1;
    switch(c) {
    case 7: break;
    case 8: if(k) k--; break;
    case 10: r++; break;
    case 13: k=0; break;
    default: put_char(p,r,k++,c,attr);
    }
    if(k>=cols()) {k=0; r++;}
    if(r>=rows) {r=rows-1; scroll(p,1,0,graphics()?0:cell(p,r,k)[1],0,0,(u8)(rows-1),(u8)(cols()-1));}
    *row=(u8)r; *col=(u8)k;
}
static void tty(u8 c,int attr) {
    u8 p=page(),row=cur_row(p),col=cur_col(p);
    advance(p,&row,&col,c,graphics()?attr:-1);
    set_cursor(p,row,col);
}
static void text_at(u8 p,u8 c,int attr,u16 count) {
    unsigned pos=cur_row(p)*cols()+cur_col(p);
    for(u16 i=0;i<count && pos<cols()*rows;i++,pos++) put_char(p,pos/cols(),pos%cols(),c,attr);
}

/* --- DOS console text ------------------------------------------------------- */
static u8 cp437(u16 u) {
    if(u<0x80) return (u8)u;
    for(unsigned c=0x80;c<256;c++) if(vga_unicode[c]==u) return (u8)c;
    for(unsigned c=1;c<0x20;c++) if(vga_unicode[c]==u) return (u8)c;
    return u==0x2302?0x7f:'?';
}
/* ANSI.SYS's attribute for DOS console text (-1 before it sets one), and
 * IO_TEXT_NOWRAP. */
static int text_attr=-1;
static u32 text_flags;
/* A character of DOS console text: as the teletype writes it, or in the
 * attribute, scrolling filling with it, and kept in the last column
 * without wrapping, as ANSI.SYS writes. */
static void console_char(u8 c) {
    if(text_attr<0) {tty(c,7); return;}
    u8 p=page(),a=(u8)text_attr;
    unsigned r=cur_row(p)<rows?cur_row(p):rows-1,k=cur_col(p)<cols()?cur_col(p):cols()-1;
    switch(c) {
    case 7: break;
    case 8: if(k) k--; break;
    case 10: r++; break;
    case 13: k=0; break;
    default:
        put_char(p,r,k,c,a);
        if(k+1<cols()) k++;
        else if(!(text_flags&IO_TEXT_NOWRAP)) {k=0; r++;}
    }
    if(r>=rows) {r=rows-1; scroll(p,1,0,graphics()?0:a,0,0,(u8)(rows-1),(u8)(cols()-1));}
    set_cursor(p,(u8)r,(u8)k);
}
static void console_draw(void *context,const u16 *text,size_t n) {
    (void)context;
    if(!cur || quiet) return;
    if(!text) {
        clear_page(page(),text_attr>=0?(u8)text_attr:0x07); set_cursor(page(),0,0); return;
    }
    for(size_t i=0;i<n;i++) {
        u8 c=cp437(text[i]);
        if(c==10) {console_char(13); console_char(10);}
        else if(c==9) {u8 k; do {k=cur_col(page()); console_char(' ');} while(cur_col(page())%8 && cur_col(page())!=k);}
        else console_char(c);
    }
}
/* The screen as ANSI.SYS's (display_text): the active page's cursor, cells
 * blanked as INT 10h's scroll blanks them (in color 0 in graphics modes),
 * modes as INT 10h sets them. */
static int text_query(void *context,IoTextScreen *s) {
    (void)context;
    if(!cur) return DE_ACCESS;
    u8 p=page();
    *s=(IoTextScreen){.size=sizeof(*s),.columns=cols(),.rows=rows,.column=cur_col(p),.row=cur_row(p),
        .attribute=text_attr>=0?(u32)text_attr:0x07,.flags=graphics() || (attr_in(0x10)&8)?0:IO_TEXT_INTENSITY};
    return 0;
}
static int text_locate(void *context,u32 column,u32 row) {
    (void)context;
    if(!cur || column>=cols() || row>=rows) return DE_FUNCTION;
    set_cursor(page(),(u8)row,(u8)column); return 0;
}
static int text_attribute(void *context,u32 attribute,u32 flags) {
    (void)context; text_attr=(int)(attribute&0xff); text_flags=flags; return 0;
}
static int text_erase(void *context,u32 column,u32 row,u32 cells) {
    (void)context;
    if(!cur || column>=cols() || row>=rows) return DE_FUNCTION;
    u8 p=page(),a=(u8)(graphics()?0:text_attr>=0?text_attr:0x07);
    unsigned total=cols()*rows,pos=row*cols()+column,end=cells<total-pos?pos+cells:total;
    if(!pos && end==total) {clear_page(p,a); set_cursor(p,0,0); return 0;}
    while(pos<end) {
        unsigned r=pos/cols(),last=end<(r+1)*cols()?(end-1)%cols():cols()-1;
        scroll(p,0,0,a,(u8)r,(u8)(pos%cols()),(u8)r,(u8)last);
        pos=(r+1)*cols();
    }
    return 0;
}
static int set_mode(u8,int);
static int text_mode(void *context,u32 mode) {(void)context; return mode<256 && set_mode((u8)mode,0)?0:DE_FUNCTION;}
static const IoTextOps text_ops={sizeof(IoTextOps),0,text_query,text_locate,text_attribute,text_erase,text_mode};
/* DOS console text drawn here, ANSI.SYS's work too where IO.SYS has it. */
static void console_here(void) {
    io->display_console(io->context,screen,console_draw,NULL);
    if(io->size>=offsetof(IoServices,display_text)+sizeof(io->display_text) && io->display_text)
        io->display_text(io->context,screen,&text_ops,NULL);
}

/* --- modes ------------------------------------------------------------------ */
static const Mode *find(u8 m) {
    for(unsigned i=0;i<sizeof modes/sizeof modes[0];i++) if(modes[i].mode==m) return &modes[i];
    return NULL;
}
/* Character rows of h lines: as many as the display has lines for. */
static void set_rows(unsigned h) {
    height=h; rows=cur->lines/h;
    if(!rows) rows=1;
    *LINEAR(BDA+0x84)=(u8)(rows-1); poke16(BDA+0x85,(u16)h);
}
/* Mode m with its registers, palette, font and BIOS data; the memory is
 * cleared unless keep. */
static int set_mode(u8 m,int keep) {
    const Mode *d=find(m);
    if(!d) return 0;
    (void)in(0x3da); (void)in(0x3ba); out(0x3c0,0x00);
    seq(0,0x01); out(0x3c2,d->misc);
    for(u8 i=0;i<4;i++) seq((u8)(i+1),d->seq[i]);
    seq(0,0x03);
    cur=d; poke16(BDA+0x63,d->misc&1?0x3d4:0x3b4);
    crtc(0x11,d->crtc[0x11]&0x7f);
    for(u8 i=0;i<25;i++) crtc(i,i==0x11?d->crtc[i]&0x7f:d->crtc[i]);
    crtc(0x11,d->crtc[0x11]);
    for(u8 i=0;i<9;i++) gc(i,d->gfx[i]);
    u16 status=(u16)(crtc_port()+6);
    (void)in(status);
    for(u8 i=0;i<20;i++) {out(0x3c0,i); out(0x3c0,d->attr[i]);}
    out(0x3c0,0x14); out(0x3c0,0x00);
    load_dac(d->dac);
    poke16(BDA+0x4c,d->page_size);
    if(!graphics()) load_font(&vga_font16[0][0],16,0,256);
    set_rows(graphics()?d->height:16);
    if(!keep) {
        if(!graphics()) for(u32 i=0;i<0x8000;i+=2) {*vmem(text_base()+i)=' '; *vmem(text_base()+i+1)=0x07;}
        else if(d->kind==CGA4 || d->kind==CGA2) for(u32 i=0;i<0x8000;i++) *vmem(0xb8000+i)=0;
        else for(u32 i=0;i<0x10000;i++) *vmem(0xa0000+i)=0;
    }
    *LINEAR(BDA+0x49)=m; poke16(BDA+0x4a,d->cols); poke16(BDA+0x4e,0);
    for(unsigned i=0;i<16;i++) *LINEAR(BDA+0x50+i)=0;
    *LINEAR(BDA+0x62)=0; *LINEAR(BDA+0x65)=d->mode_control; *LINEAR(BDA+0x66)=d->palette;
    *LINEAR(BDA+0x87)=(u8)((*LINEAR(BDA+0x87)&0x7f)|(keep?0x80:0));
    poke16(BDA+0x10,(u16)((peek16(BDA+0x10)&~0x30)|(d->kind==MONO?0x30:0x20)));
    set_ivt(0x43,font_vector(graphics()?d->height:8));
    poke16(BDA+0x60,d->kind==MONO?0x0b0c:0x0607); /* the tables' cursor lines */
    select_page(0);
    (void)in(status); out(0x3c0,0x20);
    return 1;
}
/* A text font of h rows (AX=11x0h-11x4h); with recalculate, the rows
 * follow it, as do the cursor and the page size. */
static void text_font(const u8 *font,unsigned h,unsigned first,unsigned count,int recalculate) {
    if(graphics() || !h || h>32) return;
    load_font(font,h,first,count);
    if(!recalculate) return;
    crtc(0x09,(u8)((crtc_in(0x09)&0xe0)|(h-1)));
    set_rows(h);
    unsigned lines=rows*h-1,overflow=crtc_in(0x07)&~0x42u;
    crtc(0x12,(u8)lines); crtc(0x07,(u8)(overflow|(lines>>7&2)|(lines>>3&0x40)));
    u8 start=(u8)(h-(h>8?3:2));
    crtc(0x0a,start); crtc(0x0b,(u8)(start+1)); poke16(BDA+0x60,0x0607);
    poke16(BDA+0x4c,(u16)((cols()*rows*2+0x7ff)&~0x7ffu));
    select_page(0);
}

/* --- INT 10h -------------------------------------------------------------- */
static void palette(u8 al) {
    u8 *table=LIN(sreg(SR_ES),rw(EDX));
    switch(al) {
    case 0x00: attr_out(rl(EBX)&15,rh(EBX)); break;
    case 0x01: attr_out(0x11,rh(EBX)); break;
    case 0x02: for(u8 i=0;i<16;i++) attr_out(i,table[i]); attr_out(0x11,table[16]); break;
    case 0x03: {
        u8 m=attr_in(0x10); m=rl(EBX)?(u8)(m|8):(u8)(m&~8); attr_out(0x10,m);
        *LINEAR(BDA+0x65)=rl(EBX)?(u8)(*LINEAR(BDA+0x65)|0x20):(u8)(*LINEAR(BDA+0x65)&~0x20);
        break;
    }
    case 0x07: wh(EBX,attr_in(rl(EBX)&15)); break;
    case 0x08: wh(EBX,attr_in(0x11)); break;
    case 0x09: for(u8 i=0;i<16;i++) table[i]=attr_in(i); table[16]=attr_in(0x11); break;
    case 0x10: out(0x3c8,rl(EBX)); out(0x3c9,rh(EDX)); out(0x3c9,rh(ECX)); out(0x3c9,rl(ECX)); break;
    case 0x12:
        out(0x3c8,rl(EBX));
        for(u32 i=0;i<rw(ECX)*3U && i<768;i++) out(0x3c9,table[i]);
        break;
    case 0x13:
        if(rl(EBX)==0) {u8 m=attr_in(0x10); attr_out(0x10,(u8)(rh(EBX)?m|0x80:m&0x7f));}
        else attr_out(0x14,(u8)(attr_in(0x10)&0x80?rh(EBX)&0x0f:(rh(EBX)&3)<<2));
        break;
    case 0x15: out(0x3c7,rl(EBX)); wh(EDX,in(0x3c9)); wh(ECX,in(0x3c9)); wl(ECX,in(0x3c9)); break;
    case 0x17:
        out(0x3c7,rl(EBX));
        for(u32 i=0;i<rw(ECX)*3U && i<768;i++) table[i]=in(0x3c9);
        break;
    case 0x18: out(0x3c6,rl(EBX)); break;
    case 0x19: wl(EBX,in(0x3c6)); break;
    case 0x1a: {u8 m=attr_in(0x10); wl(EBX,m>>7); wh(EBX,attr_in(0x14)>>(m&0x80?0:2)&(m&0x80?0x0f:3)); break;}
    default: break;
    }
}
/* AH=0Bh: the background or border, and the CGA palette in modes 4 and 5. */
static void cga_palette(void) {
    u8 *bda=LINEAR(BDA+0x66),v=rl(EBX);
    if(rh(EBX)==0) *bda=(u8)((*bda&0xe0)|(v&0x1f)); else *bda=(u8)((*bda&~0x20)|(v&1?0x20:0));
    u8 back=(u8)((*bda&7)|(*bda&8?0x10:0)),bright=*bda&0x10?0x10:0,set=*bda&0x20?1:0;
    if(cur->kind==CGA4) {
        attr_out(0,back);
        for(u8 i=1;i<4;i++) attr_out(i,(u8)(i*2+set+bright));
    } else if(cur->kind==CGA2) attr_out(0,back);
    else attr_out(0x11,back);
}
static void pixel(int write) {
    u16 x=rw(ECX),y=rw(EDX); u8 p=rh(EBX),color=rl(EAX),v=0;
    if(x>=cur->width || y>=cur->lines) return;
    if(!graphics()) return;
    if(p>=cur->pages) p=0;
    switch(cur->kind) {
    case LINEAR: if(write) *scan(p,y,x)=color; else v=*scan(p,y,x); break;
    case PLANAR: {
        volatile u8 *d=scan(p,y,x/8u); u8 bit=(u8)(0x80>>(x&7));
        if(write) {gc(8,bit); gc(5,2); gc(3,color&0x80?0x18:0); (void)*d; *d=color&0x0f; gc(8,0xff); gc(5,0); gc(3,0);}
        else {for(u8 plane=0;plane<4;plane++) {gc(4,plane); if(*d&bit) v|=(u8)(1<<plane);} gc(4,0);}
        break;
    }
    case CGA4: {
        volatile u8 *d=scan(p,y,x/4u); unsigned shift=(3-(x&3))*2u;
        if(!write) v=(u8)(*d>>shift&3);
        else if(color&0x80) *d^=(u8)((color&3)<<shift);
        else *d=(u8)((*d&~(3u<<shift))|(color&3u)<<shift);
        break;
    }
    case CGA2: {
        volatile u8 *d=scan(p,y,x/8u); u8 bit=(u8)(0x80>>(x&7));
        if(!write) v=*d&bit?1:0;
        else if(color&0x80) {if(color&1) *d^=bit;}
        else *d=color&1?(u8)(*d|bit):(u8)(*d&~bit);
        break;
    }
    }
    if(!write) wl(EAX,v);
}
static void fonts(u8 al) {
    u8 *user=LIN(sreg(SR_ES),rw(EBP));
    switch(al) {
    case 0x00: case 0x10: text_font(user,rh(EBX),rw(EDX),rw(ECX),al==0x10); break;
    case 0x01: case 0x11: text_font(&vga_font14[0][0],14,0,256,al==0x11); break;
    case 0x02: case 0x12: text_font(&vga_font8[0][0],8,0,256,al==0x12); break;
    case 0x04: case 0x14: text_font(&vga_font16[0][0],16,0,256,al==0x14); break;
    case 0x03: seq(3,rl(EBX)); break;
    case 0x20: set_ivt(0x1f,(u32)sreg(SR_ES)<<16|rw(EBP)); break;
    case 0x21: case 0x22: case 0x23: case 0x24: {
        static const u8 heights[]={0,14,8,16};
        unsigned h=al==0x21?rw(ECX):heights[al-0x21];
        set_ivt(0x43,al==0x21?(u32)sreg(SR_ES)<<16|rw(EBP):font_vector(h));
        if(graphics() && h && h<=32) set_rows(h);
        break;
    }
    case 0x30: {
        u32 v;
        switch(rh(EBX)) {
        case 0: v=ivt(0x1f); break;
        case 1: v=ivt(0x43); break;
        case 2: v=font_vector(14); break;
        case 3: v=font_vector(8); break;
        case 4: v=font_vector(8)+128*8; break;
        case 6: v=font_vector(16); break;
        default: v=(u32)VIDEO_SEG<<16|NO_ALTERNATES_OFF; /* no 9-dot alternates */
        }
        set_sreg(SR_ES,(u16)(v>>16)); ww(EBP,(u16)v); ww(ECX,(u16)height); wl(EDX,(u8)(rows-1));
        break;
    }
    default: break;
    }
}
/* AH=1Bh: the state of the video system, 64 bytes at ES:DI. */
static void state(void) {
    u8 *s=LIN(sreg(SR_ES),rw(EDI));
    memset(s,0,64);
    poke16(((u32)sreg(SR_ES)<<4)+(u16)(rw(EDI)),FUNCTIONALITY_OFF);
    poke16(((u32)sreg(SR_ES)<<4)+(u16)(rw(EDI)+2),VIDEO_SEG);
    memcpy(s+4,LINEAR(BDA+0x49),30); /* mode through the CRTC port, 3x8h and 3x9h */
    s[34]=(u8)rows; s[35]=(u8)height; s[36]=0;
    s[37]=cur->kind==MONO?7:8;
    u16 colors=cur->kind==LINEAR?256:cur->kind==CGA4?4:cur->kind==CGA2?2:cur->kind==MONO?0:16;
    s[39]=(u8)colors; s[40]=(u8)(colors>>8); s[41]=(u8)pages();
    s[42]=cur->lines==200?0:cur->lines==350?1:cur->lines==400?2:3;
    s[45]=(u8)((*LINEAR(BDA+0x65)&0x20?0x20:0)|(*LINEAR(BDA+0x87)&1?0:0x10));
    s[49]=3; /* 256 KiB */
    wl(EAX,0x1b);
}
static void int10(void) {
    u8 ah=rh(EAX),al=rl(EAX),p=rh(EBX)&7;
    if(p>=pages()) p=0;
    switch(ah) {
    case 0x00: set_mode(al&0x7f,al&0x80); break;
    case 0x01: cursor_shape(rw(ECX)); break;
    case 0x02: set_cursor(p,rh(EDX),rl(EDX)); break;
    case 0x03: wh(EDX,cur_row(p)); wl(EDX,cur_col(p)); ww(ECX,peek16(BDA+0x60)); break;
    case 0x05: select_page(al); break;
    case 0x06: case 0x07: scroll(page(),al,ah==7,rh(EBX),rh(ECX),rl(ECX),rh(EDX),rl(EDX)); break;
    case 0x08:
        if(graphics()) ww(EAX,0); /* characters are not read back from pixels */
        else {volatile u8 *c=cell(p,cur_row(p)%rows,cur_col(p)%cols()); wl(EAX,c[0]); wh(EAX,c[1]);}
        break;
    case 0x09: case 0x0a: text_at(graphics()?page():p,al,ah==9 || graphics()?rl(EBX):-1,rw(ECX)); break;
    case 0x0b: cga_palette(); break;
    case 0x0c: case 0x0d: pixel(ah==0x0c); break;
    case 0x0e: vga_tty(al,rl(EBX)); break;
    case 0x0f: wl(EAX,(u8)(*LINEAR(BDA+0x49)|(*LINEAR(BDA+0x87)&0x80))); wh(EAX,(u8)cols()); wh(EBX,page()); break;
    case 0x10: palette(al); break;
    case 0x11: fonts(al); break;
    case 0x12:
        switch(rl(EBX)) {
        case 0x10: wh(EBX,cur->kind==MONO); wl(EBX,3); ww(ECX,0x0009); break;
        case 0x34: *LINEAR(BDA+0x87)=al?(u8)(*LINEAR(BDA+0x87)|1):(u8)(*LINEAR(BDA+0x87)&~1); wl(EAX,0x12); break;
        case 0x20: case 0x30: case 0x31: case 0x32: case 0x33: case 0x35: case 0x36: wl(EAX,0x12); break;
        default: break;
        }
        break;
    case 0x13: {
        u8 *s=LIN(sreg(SR_ES),rw(EBP)),row=rh(EDX),col=rl(EDX); int attrs=al&2;
        for(u16 i=0;i<rw(ECX);i++) advance(p,&row,&col,s[attrs?i*2:i],attrs?s[i*2+1]:rl(EBX));
        if(al&1) set_cursor(p,row,col);
        break;
    }
    case 0x1a: if(al==0) {wl(EAX,0x1a); ww(EBX,cur->kind==MONO?0x0007:0x0008);} else if(al==1) wl(EAX,0x1a); break;
    case 0x1b: if(!rw(EBX)) state(); break;
    default: break; /* no VBE (AH=4Fh) and no state save */
    }
}

/* --- the screen ------------------------------------------------------------ */
/* The character height and rows the adapter shows. */
static unsigned shown_height(void) {u8 scan=crtc_in(0x09); return ((scan&0x1fu)+1u)<<(scan>>7);}
static unsigned shown_rows(unsigned h) {
    u8 overflow=crtc_in(0x07);
    unsigned n=((crtc_in(0x12)|(overflow&2u)<<7|(overflow&0x40u)<<3)+1u)/h;
    return n && n<=60?n:cur->rows;
}
/* The displayed page and its cursor as the adapter has them. */
static void follow_cursor(void) {
    u16 size=peek16(BDA+0x4c),start=(u16)(crtc_in(0x0c)<<8|crtc_in(0x0d));
    u8 p=start*2u%size==0 && start*2u/size<8?(u8)(start*2u/size):0;
    u16 pos=(u16)((crtc_in(0x0e)<<8|crtc_in(0x0f))-p*size/2u);
    *LINEAR(BDA+0x62)=p; poke16(BDA+0x4e,(u16)(p*size));
    if(pos<cols()*rows) {*LINEAR(BDA+0x50+p*2)=(u8)(pos%cols()); *LINEAR(BDA+0x51+p*2)=(u8)(pos/cols());}
}
/* A text mode the adapter is in, as the BIOS data area would still
 * describe it after the program that set it: 40 or 80 columns, color or
 * monochrome, the character height, the page and the cursor; the adapter is
 * left alone, palette and font included. */
static void adopt(u16 port) {
    poke16(BDA+0x63,port);
    unsigned width=crtc_in(0x01)+1u;
    cur=find(port==0x3b4?7:width<=40?1:3);
    set_rows(shown_height());
    rows=shown_rows(height);
    *LINEAR(BDA+0x84)=(u8)(rows-1);
    *LINEAR(BDA+0x49)=cur->mode; poke16(BDA+0x4a,(u16)cols()); poke16(BDA+0x4c,(u16)((cols()*rows*2+0x7ff)&~0x7ffu));
    for(unsigned i=0;i<16;i++) *LINEAR(BDA+0x50+i)=0;
    follow_cursor();
    u8 start_line=crtc_in(0x0a),end_line=crtc_in(0x0b);
    poke16(BDA+0x60,start_line==height-3 && end_line==height-2?(u16)(cur->kind==MONO?0x0b0c:0x0607):(u16)(start_line<<8|end_line));
    *LINEAR(BDA+0x65)=(u8)((cur->mode_control&~0x20)|(attr_in(0x10)&8?0x20:0)); *LINEAR(BDA+0x66)=cur->palette;
    *LINEAR(BDA+0x87)=0x60;
    poke16(BDA+0x10,(u16)((peek16(BDA+0x10)&~0x30)|(cur->kind==MONO?0x30:0x20)));
    set_ivt(0x43,font_vector(8));
}
/* The adapter as IO.SYS gives it: the console's text screen, or a graphics
 * screen the VDM that started this one's parent left, which is cleared for
 * mode 3. */
void vga_open(void) {
    screen_state=0;
    if(io->size<offsetof(IoServices,display_console)+sizeof(io->display_console) || !(io->capabilities&IO_CAP_VGA_TEXT) ||
       io->display_claim(io->context,IO_DISPLAY_VGA_TEXT,&screen)) return;
    screen_state=1;
    u16 port=in(0x3cc)&1?0x3d4:0x3b4;
    out(0x3ce,6);
    if(in(0x3cf)&1) set_mode(3,0); else adopt(port);
    console_here();
}
/* After a native child, which may have run 16-bit programs of its own on
 * the screen: the BIOS data follows the adapter again, only the page and
 * cursor while the text mode is the same. */
void vga_resume(void) {
    if(screen_state<=0) return;
    u16 port=in(0x3cc)&1?0x3d4:0x3b4;
    out(0x3ce,6);
    if(in(0x3cf)&1) {if(!graphics()) set_mode(3,0); return;}
    if(graphics() || port!=crtc_port() || crtc_in(0x01)+1u!=cols() || shown_height()!=height || shown_rows(height)!=rows) adopt(port);
    else follow_cursor();
}
int vga_int10(void) {
    if(screen_state<=0) return 0;
    int10();
    return 1;
}
/* The teletype, in color in graphics modes, also on the serial console. */
int vga_tty(u8 c,u8 color) {
    if(screen_state<=0) return 0;
    tty(c,color);
    quiet=1; io->console_write(io->context,&c,1); quiet=0;
    return 1;
}
int video_port(u16 port,u8 *value,int write) {
    if(screen_state<=0) return 0;
    return !io->vga_port(io->context,screen,port,value,write);
}
/* The adapter set aside while the program is away (Alt+Tab): its
 * registers, the DAC and the four planes kept (the planes in 4*VGA_PLANE
 * bytes of the caller's), the claim and the console text given back; then
 * claimed again, as IO.SYS gives it, and put back as it was. */
typedef struct {u8 misc,seq[5],crtc[25],gfx[9],attr[21],pel,dac[768];} Adapter;
static Adapter kept;
int vga_away(u8 *planes) {
    Adapter *a=&kept; u16 c=crtc_port();
    if(screen_state<=0) return 0;
    a->misc=in(0x3cc);
    for(u8 i=0;i<5;i++) {out(0x3c4,i); a->seq[i]=in(0x3c5);}
    for(u8 i=0;i<25;i++) {out(c,i); a->crtc[i]=in((u16)(c+1));}
    for(u8 i=0;i<9;i++) {out(0x3ce,i); a->gfx[i]=in(0x3cf);}
    for(u8 i=0;i<21;i++) a->attr[i]=attr_in(i);
    a->pel=in(0x3c6);
    out(0x3c7,0); for(unsigned i=0;i<sizeof a->dac;i++) a->dac[i]=in(0x3c9);
    /* Each plane through planar reads of A0000h-AFFFFh. */
    seq(4,0x06); gc(5,0x00); gc(6,0x05);
    for(u8 p=0;p<4;p++) {gc(4,p); for(u32 i=0;i<VGA_PLANE;i++) planes[p*VGA_PLANE+i]=*vmem(0xa0000+i);}
    io->display_console(io->context,screen,NULL,NULL);
    io->display_release(io->context,screen);
    screen_state=-1;
    return 1;
}
void vga_back(const u8 *planes) {
    const Adapter *a=&kept; u16 c=a->misc&1?0x3d4:0x3b4;
    if(screen_state!=-1) return;
    screen_state=0;
    if(io->display_claim(io->context,IO_DISPLAY_VGA_TEXT,&screen)) return;
    screen_state=1;
    /* The planes by planar writes, one plane enabled at a time. */
    seq(4,0x06); gc(5,0x00); gc(6,0x05); gc(1,0x00); gc(3,0x00); gc(8,0xff);
    for(u8 p=0;p<4;p++) {seq(2,(u8)(1u<<p)); for(u32 i=0;i<VGA_PLANE;i++) *vmem(0xa0000+i)=planes[p*VGA_PLANE+i];}
    /* The registers, the sequencer held in reset and the CRTC unprotected meanwhile. */
    out(0x3c2,a->misc);
    seq(0,0x01); for(u8 i=1;i<5;i++) seq(i,a->seq[i]); seq(0,a->seq[0]);
    out(c,0x11); out((u16)(c+1),a->crtc[0x11]&0x7f);
    for(u8 i=0;i<25;i++) {out(c,i); out((u16)(c+1),i==0x11?a->crtc[i]&0x7f:a->crtc[i]);}
    out(c,0x11); out((u16)(c+1),a->crtc[0x11]);
    for(u8 i=0;i<9;i++) gc(i,a->gfx[i]);
    (void)in((u16)(c+6)); for(u8 i=0;i<21;i++) {out(0x3c0,i); out(0x3c0,a->attr[i]);}
    (void)in((u16)(c+6)); out(0x3c0,0x20);
    out(0x3c6,a->pel);
    out(0x3c8,0); for(unsigned i=0;i<sizeof a->dac;i++) out(0x3c9,a->dac[i]);
    console_here();
}
void vga_close(void) {
    if(screen_state>0) {
        io->display_console(io->context,screen,NULL,NULL);
        io->display_release(io->context,screen);
    }
    screen_state=0; cur=NULL;
}
