/* SPDX-License-Identifier: GPL-2.0-or-later
 * .FON files are code-free NE modules with FONT (8) resources.
 * FNT headers are 118 bytes (2.0) or 148 bytes (3.0), holding dimensions,
 * metrics, style, charset, family, character range and face-name offset.
 * Character entries contain widths and glyph offsets (2-byte in 2.0,
 * 4-byte in 3.0). Glyphs use 8-pixel columns, each stored top to bottom.
 */
#include <windows.h>
#include <string.h>
#include "fontfile.h"
#define MAX_HEIGHT 255
#define MAX_WIDTH 128

static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}

/* One FNT's raster font into *f; FALSE when it is not a readable one. */
static BOOL read_fnt(const BYTE *p,DWORD n,FileFont *f,void *(*alloc)(DWORD)) {
    WORD version; int first,last,fallback,c,table,entry,height,words,widest=0; DWORD face;
    if(n<118) return FALSE;
    version=get16(p);
    if((version!=0x200 && version!=0x300) || (get16(p+66)&1)) return FALSE;
    table=version==0x200?118:148; entry=version==0x200?4:6;
    first=p[95]; last=p[96]; height=get16(p+88);
    if(last<first || !height || height>MAX_HEIGHT || (DWORD)table+(DWORD)(last-first+2)*entry>n) return FALSE;
    for(c=first;c<=last;c++) widest=max(widest,(int)get16(p+table+(c-first)*entry));
    if(widest>MAX_WIDTH) return FALSE;
    words=widest?(widest+31)/32:1;
    memset(f,0,sizeof(*f));
    if(!(f->bits=(unsigned int *)alloc((DWORD)256*height*words*sizeof(unsigned int)))) return FALSE;
    f->points=get16(p+68); f->ascent=get16(p+74); f->leading=get16(p+76);
    f->italic=p[80]!=0; f->bold=get16(p+83)>=FW_SEMIBOLD; f->charset=p[85];
    f->proportional=!get16(p+86) || (p[90]&1);
    f->family=(BYTE)((p[90]&0xf0)|(f->proportional?VARIABLE_PITCH:FIXED_PITCH));
    f->height=height; f->avg=get16(p+91); f->max=widest; f->words=words;
    face=get32(p+105);
    if(face<n) {DWORD i; for(i=0;i<sizeof(f->name)-1 && face+i<n && p[face+i];i++) f->name[i]=(char)p[face+i];}
    for(c=first;c<=last;c++) {
        const BYTE *e=p+table+(c-first)*entry; int width=get16(e),col,row,bit;
        DWORD at=version==0x200?get16(e+2):get32(e+2);
        unsigned int *glyph=f->bits+(DWORD)c*height*words;
        f->widths[c]=(unsigned char)width;
        if(at+(DWORD)((width+7)/8)*height>n) continue;
        for(col=0;col<(width+7)/8;col++) for(row=0;row<height;row++) {
            BYTE b=p[at+(DWORD)col*height+row];
            for(bit=0;bit<8 && b;bit++) if(b&(0x80>>bit)) {
                int x=col*8+bit;
                if(x<width) glyph[row*words+x/32]|=0x80000000U>>(x%32);
            }
        }
    }
    /* The characters it lacks are its default one. */
    fallback=first+p[97];
    if(fallback>last) fallback=first;
    for(c=0;c<256;c++) if(c<first || c>last) {
        f->widths[c]=f->widths[fallback];
        memcpy(f->bits+(DWORD)c*height*words,f->bits+(DWORD)fallback*height*words,(size_t)height*words*sizeof(unsigned int));
    }
    if(!f->avg) f->avg=f->widths['x']?f->widths['x']:widest;
    return TRUE;
}
int ReadFontFile(const BYTE *data,DWORD size,FileFont *fonts,int max,void *(*alloc)(DWORD)) {
    DWORD ne,table,at; WORD shift; int count=0;
    if(size>=2 && (get16(data)==0x200 || get16(data)==0x300)) return max>0 && read_fnt(data,size,&fonts[0],alloc)?1:0;
    if(size<0x40 || data[0]!='M' || data[1]!='Z') return 0;
    ne=get32(data+0x3c);
    if(ne>size-0x40 || data[ne]!='N' || data[ne+1]!='E') return 0;
    table=ne+get16(data+ne+0x24);
    if(table+2>size) return 0;
    shift=get16(data+table);
    if(shift>16) return 0;
    for(at=table+2;at+8<=size && get16(data+at);) {
        WORD type=get16(data+at),n=get16(data+at+2),i;
        at+=8;
        for(i=0;i<n && at+12<=size;i++,at+=12) {
            DWORD offset=(DWORD)get16(data+at)<<shift,length=(DWORD)get16(data+at+2)<<shift;
            if(type!=0x8008 || offset>=size || count>=max) continue;
            if(read_fnt(data+offset,min(length,size-offset),&fonts[count],alloc)) count++;
        }
    }
    return count;
}
