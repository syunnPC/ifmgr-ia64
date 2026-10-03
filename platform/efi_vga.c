/* SPDX-License-Identifier: GPL-2.0-or-later
 * VGA text console (IO_CAP_VGA_TEXT) for direct screen/port access.
 * A PCI VGA adapter decoding legacy ports and memory is programmed to mode
 * 3: 80x25 cells, 9x16 dots, 16 colors and an 8x16 CP437 font in plane 2.
 * Disable Bochs VBE first so it does not hide VGA output. Restore saved VGA
 * registers before GOP SetMode: ati-vga retains them in graphics mode and
 * they still control legacy memory writes. Ports use the PCI root bridge;
 * legacy memory uses uncached physical addresses.
 *
 * An 80x25 shadow of firmware console output supplies the initial screen
 * and, for an 80x25 firmware console, its cursor. Subsequent console text
 * uses the displayed page's hardware cursor and existing cell attributes,
 * unless a claimant handles it. CR/BS/TAB/BEL are supported; LF starts a
 * new line. Read geometry from the adapter to follow guest mode changes.
 */
#include "efi_vga.h"
#include "vga_font.h"
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
static int present;

#define COLS_MAX 132
#define ROWS_MAX 60
static void outb(u16 port,u8 v) {root->Io.Write(root,EfiPciIoWidthUint8,port,1,&v);}
static u8 inb(u16 port) {u8 v=0xff; root->Io.Read(root,EfiPciIoWidthUint8,port,1,&v); return v;}
static void outw(u16 port,u16 v) {root->Io.Write(root,EfiPciIoWidthUint16,port,1,&v);}
static u16 inw(u16 port) {u16 v=0xffff; root->Io.Read(root,EfiPciIoWidthUint16,port,1,&v); return v;}
static void indexed(u16 port,u8 index,u8 value) {outb(port,index); outb((u16)(port+1),value);}
static volatile u8 *legacy(u32 address) {return (volatile u8 *)(uintptr_t)(0x8000000000000000ULL|address);}
static u32 config(UINTN bus,UINTN dev,UINTN fn,UINTN reg) {
    u32 v=0xffffffffU;
    if(EFI_ERROR(root->Pci.Read(root,EfiPciIoWidthUint32,EFI_PCI_ADDRESS(bus,dev,fn)+reg,1,&v))) return 0xffffffffU;
    return v;
}
/* A VGA-compatible function (class 03h, subclass 00h, interface 00h) with
 * I/O and memory decoding on. */
static int find_vga(void) {
    for(UINTN bus=0;bus<8;bus++) for(UINTN dev=0;dev<32;dev++) {
        UINTN functions=(config(bus,dev,0,0x0c)>>16&0x80)?8:1;
        for(UINTN fn=0;fn<functions;fn++) {
            u32 id=config(bus,dev,fn,0);
            if((id&0xffff)==0xffff) continue;
            if(config(bus,dev,fn,0x08)>>8==0x030000 && (config(bus,dev,fn,0x04)&3)==3) return 1;
        }
    }
    return 0;
}
/* The Bochs VBE extension: index 1CEh, data at 1CFh (or 1D0h where odd
 * ports cannot be decoded); its identification reads B0Cxh. */
static void vbe_off(void) {
    static const u16 data[2]={0x1cf,0x1d0};
    for(unsigned i=0;i<2;i++) {
        outw(0x1ce,0);
        if((inw(data[i])&0xfff0)!=0xb0c0) continue;
        outw(0x1ce,4); outw(data[i],0); /* VBE_DISPI_INDEX_ENABLE: disabled */
        return;
    }
}

/* IBM VGA mode 3. */
static const u8 seq[5]={0x03,0x00,0x03,0x00,0x02};
static const u8 crtc[25]={0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x00,
                          0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff};
static const u8 gfx[9]={0x00,0x00,0x00,0x00,0x00,0x10,0x0e,0x00,0xff};
static const u8 attr[21]={0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
                          0x0c,0x00,0x0f,0x08,0x00};
static void load_font(void) {
    indexed(0x3c4,0,0x01); indexed(0x3c4,2,0x04); indexed(0x3c4,4,0x07); indexed(0x3c4,0,0x03);
    indexed(0x3ce,4,0x02); indexed(0x3ce,5,0x00); indexed(0x3ce,6,0x00);
    for(unsigned c=0;c<256;c++) for(unsigned row=0;row<32;row++) *legacy(0xa0000+c*32+row)=row<16?vga_font16[c][row]:0;
    indexed(0x3c4,0,0x01); indexed(0x3c4,2,seq[2]); indexed(0x3c4,4,seq[4]); indexed(0x3c4,0,0x03);
    indexed(0x3ce,4,gfx[4]); indexed(0x3ce,5,gfx[5]); indexed(0x3ce,6,gfx[6]);
}
/* The adapter's state before mode 3. */
static struct {u8 misc,seq[5],crtc[25],gfx[9],attr[21],dac[64*3];} before;
static u16 crtc_port(u8 misc) {return misc&1?0x3d4:0x3b4;}
static void attr_write(u8 misc,u8 index,u8 value) {(void)inb((u16)(crtc_port(misc)+6)); outb(0x3c0,index); outb(0x3c0,value);}
static void keep_state(void) {
    before.misc=inb(0x3cc);
    u16 crtc_index=crtc_port(before.misc);
    for(u8 i=0;i<5;i++) {outb(0x3c4,i); before.seq[i]=inb(0x3c5);}
    for(u8 i=0;i<25;i++) {outb(crtc_index,i); before.crtc[i]=inb((u16)(crtc_index+1));}
    for(u8 i=0;i<9;i++) {outb(0x3ce,i); before.gfx[i]=inb(0x3cf);}
    for(u8 i=0;i<21;i++) {(void)inb((u16)(crtc_index+6)); outb(0x3c0,i); before.attr[i]=inb(0x3c1);}
    outb(0x3c7,0);
    for(unsigned i=0;i<sizeof before.dac;i++) before.dac[i]=inb(0x3c9);
}
static void restore_state(void) {
    u16 crtc_index=crtc_port(before.misc);
    (void)inb(0x3da); outb(0x3c0,0x00);
    indexed(0x3c4,0,0x01);
    outb(0x3c2,before.misc);
    for(u8 i=1;i<5;i++) indexed(0x3c4,i,before.seq[i]);
    indexed(0x3c4,0,before.seq[0]);
    indexed(crtc_index,0x11,before.crtc[0x11]&0x7f);
    for(u8 i=0;i<25;i++) indexed(crtc_index,i,i==0x11?before.crtc[i]&0x7f:before.crtc[i]);
    indexed(crtc_index,0x11,before.crtc[0x11]);
    for(u8 i=0;i<9;i++) indexed(0x3ce,i,before.gfx[i]);
    for(u8 i=0;i<21;i++) attr_write(before.misc,i,before.attr[i]);
    outb(0x3c8,0);
    for(unsigned i=0;i<sizeof before.dac;i++) outb(0x3c9,before.dac[i]);
    (void)inb((u16)(crtc_index+6)); outb(0x3c0,0x20);
}
static u8 cp437(u32 u) {
    if(u<0x7f) return (u8)u;
    for(unsigned c=1;c<256;c++) if(vga_unicode[c]==u) return (u8)c;
    return '?';
}

/* --- the text console as the screen of a PC -------------------------------- */
#define SHADOW_COLS 80
#define SHADOW_ROWS 25
static u16 shadow[SHADOW_ROWS][SHADOW_COLS]; /* character and attribute */
static unsigned shadow_row,shadow_col;
static int shadow_wrap; /* the last column is written; the next character starts a line */
static SIMPLE_TEXT_OUTPUT_INTERFACE *console;
static int console_matches; /* the console is 80x25 too */
static void shadow_line(void) {
    shadow_col=0; shadow_wrap=0;
    if(++shadow_row<SHADOW_ROWS) return;
    shadow_row=SHADOW_ROWS-1;
    memmove(shadow[0],shadow[1],sizeof shadow-sizeof shadow[0]);
    for(unsigned col=0;col<SHADOW_COLS;col++) shadow[SHADOW_ROWS-1][col]=0x0720;
}
/* A character in attr; without wrapping the last column takes the next. */
static void shadow_put(u32 u,u8 attr,int nowrap) {
    if(shadow_wrap && !nowrap) shadow_line();
    shadow[shadow_row][shadow_col]=(u16)(attr<<8|cp437(u));
    if(shadow_col+1>=SHADOW_COLS) shadow_wrap=1; else shadow_col++;
}
void efi_vga_shadow_clear(void) {
    for(unsigned row=0;row<SHADOW_ROWS;row++) for(unsigned col=0;col<SHADOW_COLS;col++) shadow[row][col]=0x0720;
    shadow_row=shadow_col=0; shadow_wrap=0;
}
void efi_vga_shadow(const void *text,size_t n,int wide,u8 attr,int nowrap) {
    if(!present) return;
    for(size_t i=0;i<n;i++) {
        u32 u=wide?((const u16 *)text)[i]:((const u8 *)text)[i];
        switch(u) {
        case 0: case 7: break;
        case 8: if(shadow_wrap) shadow_wrap=0; else if(shadow_col) shadow_col--; break;
        case 9: do shadow_put(' ',attr,nowrap); while(shadow_col&7 && !shadow_wrap); break;
        case 10: shadow_line(); break;
        case 13: shadow_col=0; shadow_wrap=0; break;
        default: shadow_put(u,attr,nowrap);
        }
    }
    if(console_matches) {
        unsigned col=(unsigned)console->Mode->CursorColumn,row=(unsigned)console->Mode->CursorRow;
        if(col<SHADOW_COLS && row<SHADOW_ROWS && (col!=shadow_col || row!=shadow_row)) {shadow_col=col; shadow_row=row; shadow_wrap=0;}
    }
}
void efi_vga_shadow_locate(unsigned col,unsigned row) {
    if(col<SHADOW_COLS && row<SHADOW_ROWS) {shadow_col=col; shadow_row=row; shadow_wrap=0;}
}
void efi_vga_shadow_erase(unsigned col,unsigned row,unsigned cells,u8 attr) {
    for(unsigned pos=row*SHADOW_COLS+col;cells-- && pos<SHADOW_COLS*SHADOW_ROWS;pos++)
        shadow[pos/SHADOW_COLS][pos%SHADOW_COLS]=(u16)(attr<<8|' ');
}
/* The shadow kept for a program set aside, and shown again: drawn on the
 * firmware console a row at a time in runs of one attribute (its trailing
 * blanks left out, and the last cell, which would scroll the screen), the
 * cursor where it was. */
#define SHADOW_SAVE (sizeof shadow+4)
int efi_vga_shadow_save(void *out,u32 size) {
    if(!present || size<SHADOW_SAVE) return DE_FUNCTION;
    u8 *p=out; memcpy(p,shadow,sizeof shadow); p+=sizeof shadow;
    p[0]=(u8)shadow_row; p[1]=(u8)shadow_col; p[2]=(u8)shadow_wrap; p[3]=0;
    return 0;
}
int efi_vga_shadow_restore(const void *in,u32 size) {
    if(!present || !console || size<SHADOW_SAVE) return DE_FUNCTION;
    const u8 *p=in; memcpy(shadow,p,sizeof shadow); p+=sizeof shadow;
    shadow_row=p[0]<SHADOW_ROWS?p[0]:SHADOW_ROWS-1; shadow_col=p[1]<SHADOW_COLS?p[1]:0; shadow_wrap=p[2]!=0;
    INT32 was=console->Mode->Attribute;
    console->SetAttribute(console,0x07); console->ClearScreen(console);
    for(unsigned row=0;row<SHADOW_ROWS;row++) {
        unsigned n=SHADOW_COLS;
        while(n && shadow[row][n-1]==0x0720) n--;
        if(row==SHADOW_ROWS-1 && n==SHADOW_COLS) n--;
        for(unsigned c=0;c<n;) {
            CHAR16 line[SHADOW_COLS+1]; unsigned k=0; u8 attr=(u8)(shadow[row][c]>>8);
            console->SetCursorPosition(console,c,row);
            while(c<n && (u8)(shadow[row][c]>>8)==attr) {u8 b=(u8)shadow[row][c++]; line[k++]=b?(CHAR16)vga_unicode[b]:' ';}
            line[k]=0;
            console->SetAttribute(console,attr&0x7f);
            console->OutputString(console,line);
        }
    }
    console->SetAttribute(console,(UINTN)was);
    console->SetCursorPosition(console,shadow_col,shadow_row);
    return 0;
}
/* The copy on page 0 with the cursor after it. */
static void show_console(void) {
    if(shadow_wrap) shadow_line();
    for(u32 i=0;i<SHADOW_COLS*SHADOW_ROWS*2;i+=2) {
        u16 c=shadow[i/2/SHADOW_COLS][i/2%SHADOW_COLS];
        *legacy(0xb8000+i)=(u8)c; *legacy(0xb8000+i+1)=(u8)(c>>8);
    }
    unsigned pos=shadow_row*SHADOW_COLS+shadow_col;
    indexed(0x3d4,0x0e,(u8)(pos>>8)); indexed(0x3d4,0x0f,(u8)pos);
}

/* Mode 3, the screen blank. */
static void mode3(void) {
    (void)inb(0x3da); outb(0x3c0,0x00); /* screen off while the registers change */
    indexed(0x3c4,0,0x01);
    outb(0x3c2,0x67);
    for(unsigned i=1;i<5;i++) indexed(0x3c4,(u8)i,seq[i]);
    indexed(0x3c4,0,0x03);
    indexed(0x3d4,0x11,crtc[0x11]&0x7f); /* unprotect CR0-CR7 */
    for(unsigned i=0;i<25;i++) indexed(0x3d4,(u8)i,i==0x11?crtc[i]&0x7f:crtc[i]);
    indexed(0x3d4,0x11,crtc[0x11]);
    for(unsigned i=0;i<9;i++) indexed(0x3ce,(u8)i,gfx[i]);
    (void)inb(0x3da);
    for(unsigned i=0;i<21;i++) {outb(0x3c0,(u8)i); outb(0x3c0,attr[i]);}
    /* The EGA colors in DAC registers 0-63. */
    outb(0x3c6,0xff); outb(0x3c8,0);
    for(unsigned c=0;c<64;c++) {
        outb(0x3c9,(u8)((c&4?0x2a:0)|(c&0x20?0x15:0)));
        outb(0x3c9,(u8)((c&2?0x2a:0)|(c&0x10?0x15:0)));
        outb(0x3c9,(u8)((c&1?0x2a:0)|(c&0x08?0x15:0)));
    }
    load_font();
    for(u32 i=0;i<0x8000;i+=2) {*legacy(0xb8000+i)=' '; *legacy(0xb8000+i+1)=0x07;}
    indexed(0x3d4,0x0e,0); indexed(0x3d4,0x0f,0);
    (void)inb(0x3da); outb(0x3c0,0x20); /* screen on */
}
int efi_vga_text(void) {
    if(!present) return DE_FUNCTION;
    keep_state();
    vbe_off();
    mode3();
    show_console();
    return 0;
}
/* --- console text --------------------------------------------------------- */
typedef struct {u16 crtc; u32 base,mask; unsigned cols,rows,start;} Text;
static u8 crtc_in(const Text *t,u8 index) {outb(t->crtc,index); return inb((u16)(t->crtc+1));}
static unsigned crtc_word(const Text *t,u8 index) {return (unsigned)crtc_in(t,index)<<8|crtc_in(t,(u8)(index+1));}
/* The text screen as the adapter shows it: columns from the horizontal
 * display end, rows from the vertical display end and the character height,
 * the memory by the graphics controller's map; 0 in a graphics mode. */
static int text_shape(Text *t) {
    u8 misc=inb(0x3cc); outb(0x3ce,6); u8 map=inb(0x3cf);
    if(map&1) return 0;
    t->crtc=crtc_port(misc);
    switch(map>>2&3) {
    case 2: t->base=0xb0000; t->mask=0x7fff; break;
    case 3: t->base=0xb8000; t->mask=0x7fff; break;
    default: t->base=0xa0000; t->mask=map>>2&3?0xffff:0x1ffff; break;
    }
    u8 overflow=crtc_in(t,0x07),scan=crtc_in(t,0x09);
    unsigned lines=(crtc_in(t,0x12)|(overflow&2)<<7|(overflow&0x40)<<3)+1u,height=((scan&0x1fu)+1u)<<(scan>>7);
    t->cols=crtc_in(t,0x01)+1u; t->rows=lines/height;
    t->start=crtc_word(t,0x0c);
    return t->cols>=8 && t->cols<=COLS_MAX && t->rows>=1 && t->rows<=ROWS_MAX;
}
/* The cursor within the page (on its last row when below it). */
static unsigned cursor(const Text *t) {
    unsigned pos=(crtc_word(t,0x0e)-t->start)&0xffff;
    return pos<t->cols*t->rows?pos:(t->rows-1)*t->cols+pos%t->cols;
}
static void set_cursor(const Text *t,unsigned pos) {
    pos+=t->start; outb(t->crtc,0x0e); outb((u16)(t->crtc+1),(u8)(pos>>8)); outb(t->crtc,0x0f); outb((u16)(t->crtc+1),(u8)pos);
}
static volatile u8 *cell(const Text *t,unsigned pos) {return legacy(t->base+((t->start+pos)*2&t->mask));}
/* A character, with attr or keeping the cell's (attr<0). */
static void put(const Text *t,unsigned pos,u8 c,int attr) {
    volatile u8 *d=cell(t,pos); d[0]=c;
    if(attr>=0) d[1]=(u8)attr;
}
static void scroll(const Text *t,u8 attr) {
    unsigned last=(t->rows-1)*t->cols;
    for(unsigned pos=0;pos<last;pos++) {volatile u8 *d=cell(t,pos),*s=cell(t,pos+t->cols); d[0]=s[0]; d[1]=s[1];}
    for(unsigned col=0;col<t->cols;col++) {volatile u8 *d=cell(t,last+col); d[0]=' '; d[1]=attr;}
}
void efi_vga_console(const void *text,size_t n,int wide,int attr,int nowrap) {
    Text t;
    if(!text_shape(&t)) return;
    unsigned pos=cursor(&t),row=pos/t.cols,col=pos%t.cols;
    for(size_t i=0;i<n;i++) {
        u32 u=wide?((const u16 *)text)[i]:((const u8 *)text)[i];
        switch(u) {
        case 7: continue;
        case 8: if(col) col--; continue;
        case 9: do put(&t,row*t.cols+col++,' ',attr); while(col%8 && col<t.cols); break;
        case 10: row++; col=0; break;
        case 13: col=0; continue;
        default: put(&t,row*t.cols+col++,wide?cp437(u):(u8)u,attr);
        }
        if(col>=t.cols) {if(nowrap) col=t.cols-1; else {col=0; row++;}}
        if(row>=t.rows) {row=t.rows-1; scroll(&t,attr>=0?(u8)attr:cell(&t,row*t.cols)[1]);}
    }
    set_cursor(&t,row*t.cols+col);
}
void efi_vga_clear(u8 attr) {
    Text t;
    if(!text_shape(&t)) return;
    for(unsigned pos=0;pos<t.cols*t.rows;pos++) put(&t,pos,' ',attr);
    set_cursor(&t,0);
}
void efi_vga_erase(unsigned col,unsigned row,unsigned cells,u8 attr) {
    Text t;
    if(!text_shape(&t)) return;
    for(unsigned pos=row*t.cols+col;cells-- && pos<t.cols*t.rows;pos++) put(&t,pos,' ',attr);
}
/* Attribute bit 7 a bright background rather than blinking (AR10 bit 3 clear). */
int efi_vga_intensity(void) {
    if(!present) return 0;
    u16 status=(u16)(crtc_port(inb(0x3cc))+6);
    (void)inb(status); outb(0x3c0,0x30); u8 mode=inb(0x3c1); (void)inb(status);
    return !(mode&8);
}
/* The text screen's size, and the cursor, for a console interface. */
int efi_vga_size(unsigned *cols,unsigned *rows) {
    Text t;
    if(!text_shape(&t)) return 0;
    *cols=t.cols; *rows=t.rows; return 1;
}
int efi_vga_where(unsigned *col,unsigned *row) {
    Text t;
    if(!text_shape(&t)) return 0;
    unsigned pos=cursor(&t); *col=pos%t.cols; *row=pos/t.cols; return 1;
}
void efi_vga_locate(unsigned col,unsigned row) {
    Text t;
    if(text_shape(&t) && col<t.cols && row<t.rows) set_cursor(&t,row*t.cols+col);
}
/* The cursor's start line: bit 5 hides it. */
void efi_vga_show_cursor(int on) {
    Text t;
    if(!text_shape(&t)) return;
    u8 start=crtc_in(&t,0x0a);
    outb(t.crtc,0x0a); outb((u16)(t.crtc+1),(u8)(on?start&~0x20:start|0x20));
}

/* A text mode for the console: mode 3 again when a program left a
 * graphics mode. */
void efi_vga_console_mode(void) {
    Text t;
    if(!text_shape(&t)) mode3();
}
void efi_vga_leave(void) {restore_state();}

/* A program's access to the VGA's ports while it has the text screen. */
int efi_vga_port(u32 port,u8 *value,int write) {
    if(!present || port<0x3b0 || port>0x3df || !value) return DE_FUNCTION;
    if(write) outb((u16)port,*value); else *value=inb((u16)port);
    return 0;
}
int efi_vga_init(EFI_SYSTEM_TABLE *st) {
    EFI_GUID guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles=NULL; UINTN count=0;
    root=NULL; present=0;
    if(EFI_ERROR(st->BootServices->LocateHandleBuffer(ByProtocol,&guid,NULL,&count,&handles))) return 0;
    if(count==1) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *p=NULL;
        if(!EFI_ERROR(st->BootServices->HandleProtocol(handles[0],&guid,(void **)&p)) && p && !p->SegmentNumber &&
           p->Io.Read && p->Io.Write && p->Pci.Read) root=p;
    }
    st->BootServices->FreePool(handles);
    present=root && find_vga();
    console=st->ConOut;
    UINTN cols=0,rows=0;
    console_matches=!EFI_ERROR(console->QueryMode(console,(UINTN)console->Mode->Mode,&cols,&rows)) &&
                    cols==SHADOW_COLS && rows==SHADOW_ROWS;
    efi_vga_shadow_clear();
    if(console_matches) {shadow_col=(unsigned)console->Mode->CursorColumn; shadow_row=(unsigned)console->Mode->CursorRow;}
    return present;
}
