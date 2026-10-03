/* SPDX-License-Identifier: GPL-2.0-or-later
 * Resources for USER: strings, bitmaps, icons and cursors (from resources,
 * created, or USER's own), drawing icons, the cursor shape, and icons taken
 * from other programs' files. Resource strings are UTF-16 (rc.exe's Win32
 * format); characters outside code page 1252's first 256 become '?'.
 */
#include "user.h"

typedef struct {
    BOOL used,cursor,shared; int w,h; POINT hot;
    HBITMAP mask,image;           /* AND mask (1 bit) and image (color, or 1 bit for cursors) */
    BYTE and_bits[GDI_CURSOR_SIZE*GDI_CURSOR_SIZE/8],xor_bits[GDI_CURSOR_SIZE*GDI_CURSOR_SIZE/8];
    HINSTANCE instance; LPCSTR name; /* where a shared one came from */
    Queue *owner;                    /* the task that made it */
} IconObj;
static IconObj icons[ICONS];
static HCURSOR current_cursor;

int WideToAnsi(const WORD *s,char *out,int size) {
    int n=0;
    if(size<=0) return 0;
    while(*s && n<size-1) {out[n++]=(char)(*s<0x100?*s:'?'); s++;}
    out[n]=0; return n;
}
const WORD *SkipWide(const WORD *s) {while(*s) s++; return s+1;}
const void *Resource(HINSTANCE instance,LPCSTR type,LPCSTR name,DWORD *size) {
    HRSRC r; HGLOBAL g;
    if(!(r=FindResource(instance,name,type))) return NULL;
    if(size) *size=SizeofResource(instance,r);
    g=LoadResource(instance,r);
    return g?LockResource(g):NULL;
}

/* --- strings and bitmaps ------------------------------------------------------- */
int WINAPI LoadString(HINSTANCE instance,UINT id,LPSTR out,int size) {
    DWORD bytes; const WORD *p=(const WORD *)Resource(instance,RT_STRING,MAKEINTRESOURCE((id>>4)+1),&bytes); UINT i; int n;
    if(!out || size<=0) return 0;
    out[0]=0;
    if(!p) return 0;
    for(i=0;i<(id&15);i++) p+=1+*p;
    n=min((int)*p,size-1);
    for(i=0;i<(UINT)n;i++) out[i]=(char)(p[1+i]<0x100?p[1+i]:'?');
    out[n]=0;
    return n;
}
HBITMAP WINAPI LoadBitmap(HINSTANCE instance,LPCSTR name) {
    DWORD size; const BITMAPINFOHEADER *h=(const BITMAPINFOHEADER *)Resource(instance,RT_BITMAP,name,&size);
    HDC dc; HBITMAP b; DWORD colors,header;
    if(!h) return NULL;
    if(h->biSize==sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREHEADER *c=(const BITMAPCOREHEADER *)h;
        colors=c->bcBitCount<=8?1u<<c->bcBitCount:0; header=c->bcSize+colors*3;
    } else {
        colors=h->biBitCount<=8?(h->biClrUsed?h->biClrUsed:1u<<h->biBitCount):0; header=h->biSize+colors*4;
    }
    dc=GetDC(NULL);
    b=CreateDIBitmap(dc,h,CBM_INIT,(const BYTE *)h+header,(const BITMAPINFO *)h,DIB_RGB_COLORS);
    ReleaseDC(NULL,dc);
    return b;
}

/* --- icon objects --------------------------------------------------------------- */
static IconObj *icon_of(HICON h) {
    ULONG_PTR v=(ULONG_PTR)h;
    if(v<HICON_BASE || (v-HICON_BASE)%4 || (v-HICON_BASE)/4>=ICONS) return NULL;
    return icons[(v-HICON_BASE)/4].used?&icons[(v-HICON_BASE)/4]:NULL;
}
static HICON new_icon(IconObj **out) {
    int i;
    for(i=0;i<ICONS;i++) if(!icons[i].used) {
        memset(&icons[i],0,sizeof(icons[i])); icons[i].used=TRUE; icons[i].owner=CurrentQueue(); *out=&icons[i];
        return (HICON)(ULONG_PTR)(HICON_BASE+i*4);
    }
    return NULL;
}
/* An icon or cursor from top-down 1-bit AND rows and 32-bit image rows. */
static HICON make_icon(BOOL cursor,int w,int h,int hx,int hy,const BYTE *and_rows,int and_stride,const DWORD *pixels) {
    IconObj *o; HICON hi=new_icon(&o); HDC dc,mem; int x,y;
    if(!hi) return NULL;
    o->cursor=cursor; o->w=w; o->h=h; o->hot.x=hx; o->hot.y=hy;
    o->mask=CreateBitmap(w,h,1,1,NULL);
    dc=GetDC(NULL); o->image=CreateCompatibleBitmap(dc,w,h); ReleaseDC(NULL,dc);
    mem=CreateCompatibleDC(NULL);
    SelectObject(mem,o->mask);
    for(y=0;y<h;y++) for(x=0;x<w;x++) SetPixel(mem,x,y,and_rows[y*and_stride+x/8]&(0x80>>(x%8))?RGB(255,255,255):RGB(0,0,0));
    SelectObject(mem,o->image);
    for(y=0;y<h;y++) for(x=0;x<w;x++) {DWORD v=pixels[y*w+x]; SetPixel(mem,x,y,RGB(v>>16&0xff,v>>8&0xff,v&0xff));}
    DeleteDC(mem);
    /* The cursor masks (32x32) for GDI's software cursor. */
    memset(o->and_bits,0xff,sizeof(o->and_bits)); memset(o->xor_bits,0,sizeof(o->xor_bits));
    for(y=0;y<h && y<GDI_CURSOR_SIZE;y++) for(x=0;x<w && x<GDI_CURSOR_SIZE;x++) {
        int bit=y*GDI_CURSOR_SIZE+x; BOOL a=(and_rows[y*and_stride+x/8]&(0x80>>(x%8)))!=0; DWORD v=pixels[y*w+x]&0xffffff;
        if(!a) o->and_bits[bit/8]&=(BYTE)~(0x80>>(bit%8));
        if(v) o->xor_bits[bit/8]|=(BYTE)(0x80>>(bit%8));
    }
    return hi;
}
/* An icon from ASCII art: K black, W white, R red, Y yellow, B blue, G gray,
 * D dark gray, C cyan, N dark blue, '.' white (a cursor's inside), ' '
 * transparent; scaled 'scale' times. */
static HICON art_icon(BOOL cursor,const char *const *rows,int scale,int hx,int hy) {
    static BYTE and_rows[32*4]; static DWORD pixels[32*32]; int x,y,i;
    memset(and_rows,0xff,sizeof(and_rows)); memset(pixels,0,sizeof(pixels));
    for(y=0;rows[y] && y*scale<32;y++) for(x=0;rows[y][x] && x*scale<32;x++) {
        DWORD v=0; BOOL opaque=TRUE;
        switch(rows[y][x]) {
        case 'K': v=0; break; case 'W': v=0xffffff; break; case 'R': v=0xff0000; break; case 'Y': v=0xffff00; break;
        case 'B': v=0x0000ff; break; case 'G': v=0xc0c0c0; break; case 'D': v=0x808080; break; case 'C': v=0x00ffff; break;
        case 'N': v=0x000080; break;
        case '.': v=0xffffff; break;
        default: opaque=FALSE;
        }
        for(i=0;i<scale*scale;i++) {
            int px=x*scale+i%scale,py=y*scale+i/scale;
            if(opaque) and_rows[py*4+px/8]&=(BYTE)~(0x80>>(px%8));
            pixels[py*32+px]=v;
        }
    }
    return make_icon(cursor,32,32,hx,hy,and_rows,4,pixels);
}
static const char *const arrow_art[]={
    "K","KK","K.K","K..K","K...K","K....K","K.....K","K......K","K.......K","K........K",
    "K.....KKKKK","K..K..K","K.K K..K","KK  K..K","K    K..K","     K..K","      K..K","      KK",NULL};
static const char *const ibeam_art[]={
    "KKK KKK","   K","   K","   K","   K","   K","   K","   K","   K","   K","   K","   K","   K","   K","KKK KKK",NULL};
static const char *const wait_art[]={
    "KKKKKKKKKKKKK","KKKKKKKKKKKKK"," K.........K"," K.........K","  K.K.K.K.K","   K.K.K.K","    K.K.K","     K.K","      K",
    "     K.K","    K...K","   K..K..K","  K..K.K..K"," K..K.K.K..K"," K.K.K.K.K.K","KKKKKKKKKKKKK","KKKKKKKKKKKKK",NULL};
static const char *const cross_art[]={
    "       K","       K","       K","       K","       K","       K","       K","KKKKKKK KKKKKKK","       K","       K","       K","       K","       K","       K","       K",NULL};
static const char *const uparrow_art[]={
    "    K","   K.K","  K...K"," K.....K","K.......K","KKKK.KKKK","   K.K","   K.K","   K.K","   K.K","   K.K","   K.K","   KKK",NULL};
static const char *const size_art[]={
    "       K","      K.K","     K...K","    KKK.KKK","   K  K.K  K","  K.K K.K K.K"," K..KKK.KKK..K","K.............K",
    " K..KKK.KKK..K","  K.K K.K K.K","   K  K.K  K","    KKK.KKK","     K...K","      K.K","       K",NULL};
static const char *const we_art[]={
    "   K       K","  KK       KK"," K.KKKKKKKKK.K","K.............K"," K.KKKKKKKKK.K","  KK       KK","   K       K",NULL};
static const char *const ns_art[]={
    "   K","  K.K"," K...K","KKK.KKK","  K.K","  K.K","  K.K","  K.K","  K.K","KKK.KKK"," K...K","  K.K","   K",NULL};
static const char *const nwse_art[]={
    "KKKKKK","K....K","K...K","K....K","K.KK..K","KK  K..K","     K..K","      K..K  KK","       K..KK.K","        K....K","         K...K","        K....K","        KKKKKK",NULL};
static const char *const nesw_art[]={
    "        KKKKKK","        K....K","         K...K","        K....K","       K..KK.K","      K..K  KK","     K..K","KK  K..K","K.KK..K","K....K","K...K","K....K","KKKKKK",NULL};
static const char *const iconbox_art[]={
    "KKKKKKKKKKKKKKKK","K..............K","K..............K","K..............K","K..............K","K..............K","K..............K",
    "K..............K","K..............K","K..............K","K..............K","K..............K","K..............K","K..............K","KKKKKKKKKKKKKKKK",NULL};
/* 16x16 icons shown at 32x32. */
static const char *const app_art[]={
    "KKKKKKKKKKKKKKKK","KNNNNNNNNNNNNNNK","KNNNNNNNNNNNNNNK","KKKKKKKKKKKKKKKK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK",
    "KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWWWWWWWWWWK","KKKKKKKKKKKKKKKK",NULL};
static const char *const hand_art[]={
    "     KKKKKK","   KKRRRRRRKK","  KRRRRRRRRRRK"," KRRRWRRRRWRRRK"," KRRWWWRRWWWRRK","KRRRRWWWWWWRRRRK","KRRRRRWWWWRRRRRK","KRRRRRWWWWRRRRRK",
    "KRRRRWWWWWWRRRRK","KRRRWWWRRWWWRRRK"," KRRRWRRRRWRRRK"," KRRRRRRRRRRRRK","  KRRRRRRRRRRK","   KKRRRRRRKK","     KKKKKK",NULL};
static const char *const question_art[]={
    "  KKKKKKKKKKKK"," KWWWWWWWWWWWWK","KWWWWWKKKKWWWWWK","KWWWWKKWWKKWWWWK","KWWWWWWWWKKWWWWK","KWWWWWWWKKWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWKKWWWWWWK",
    "KWWWWWWWWWWWWWWK","KWWWWWWKKWWWWWWK"," KWWWWWWWWWWWWK","  KKKKWWWKKKKK","      KWWK","      KWK","      KK",NULL};
static const char *const exclamation_art[]={
    "  KKKKKKKKKKKK"," KWWWWWWWWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWKKWWWWWWK","KWWWWWWWWWWWWWWK",
    "KWWWWWWKKWWWWWWK","KWWWWWWWWWWWWWWK"," KWWWWWWWWWWWWK","  KKKKWWWKKKKK","      KWWK","      KWK","      KK",NULL};
static const char *const asterisk_art[]={
    "  KKKKKKKKKKKK"," KWWWWWWWWWWWWK","KWWWWWWBBWWWWWWK","KWWWWWWWWWWWWWWK","KWWWWWBBBWWWWWWK","KWWWWWWBBWWWWWWK","KWWWWWWBBWWWWWWK","KWWWWWWBBWWWWWWK",
    "KWWWWWBBBBWWWWWK","KWWWWWWWWWWWWWWK"," KWWWWWWWWWWWWK","  KKKKWWWKKKKK","      KWWK","      KWK","      KK",NULL};
static struct {LPCSTR id; HICON h;} stock_cursors[11],stock_icons[5];
/* The system's, as a system class's icon is: no task's, so that it stays
 * when the task that first asked for it ends. */
HICON ArtIcon(const char *const *rows,int scale) {
    HICON h=art_icon(FALSE,rows,scale,0,0); IconObj *o=icon_of(h);
    if(o) {o->owner=NULL; o->shared=TRUE; GdiSetOwner(o->mask,NULL); GdiSetOwner(o->image,NULL);}
    return h;
}
HCURSOR StockCursor(LPCSTR id) {
    int i;
    for(i=0;i<11;i++) if(stock_cursors[i].id==id) return stock_cursors[i].h;
    return stock_cursors[0].h;
}
static HICON StockIcon(LPCSTR id) {
    int i;
    for(i=0;i<5;i++) if(stock_icons[i].id==id) return stock_icons[i].h;
    return stock_icons[0].h;
}
void ResourceInit(void) {
    static const char *const *const cursor_art[11]={arrow_art,ibeam_art,wait_art,cross_art,uparrow_art,size_art,iconbox_art,nwse_art,nesw_art,we_art,ns_art};
    static const int hot[11][2]={{0,0},{3,7},{6,8},{7,7},{4,0},{7,7},{8,7},{6,6},{6,6},{7,3},{3,6}};
    static const int ids[11]={32512,32513,32514,32515,32516,32640,32641,32642,32643,32644,32645};
    static const char *const *const icon_art[5]={app_art,hand_art,question_art,exclamation_art,asterisk_art};
    int i;
    memset(icons,0,sizeof(icons));
    for(i=0;i<11;i++) {
        stock_cursors[i].id=MAKEINTRESOURCE(ids[i]);
        stock_cursors[i].h=art_icon(TRUE,cursor_art[i],1,hot[i][0],hot[i][1]);
        if(icon_of(stock_cursors[i].h)) icon_of(stock_cursors[i].h)->shared=TRUE;
    }
    for(i=0;i<5;i++) {
        stock_icons[i].id=MAKEINTRESOURCE(32512+i);
        stock_icons[i].h=art_icon(FALSE,icon_art[i],2,16,16);
        if(icon_of(stock_icons[i].h)) icon_of(stock_icons[i].h)->shared=TRUE;
    }
    current_cursor=NULL;
    SetCursor(stock_cursors[0].h);
}
/* A task's icons and cursors go when it ends; GDI frees their bitmaps. */
void ResourceTaskEnded(Queue *q) {
    int i;
    for(i=0;i<ICONS;i++) if(icons[i].used && (icons[i].owner==q || (icons[i].shared && q->instance && icons[i].instance==q->instance))) {
        if((HICON)(ULONG_PTR)(HICON_BASE+i*4)==current_cursor) SetCursor(StockCursor(IDC_ARROW));
        DeleteObject(icons[i].mask); DeleteObject(icons[i].image);
        icons[i].used=FALSE;
    }
}
void ResourceShutdown(void) {
    int i;
    for(i=0;i<ICONS;i++) if(icons[i].used) {DeleteObject(icons[i].mask); DeleteObject(icons[i].image); icons[i].used=FALSE;}
}

/* --- icons and cursors from resources ------------------------------------------------- */
/* An icon or cursor image: BITMAPINFOHEADER (double height), colors, XOR rows, AND rows. */
static HICON from_dib(BOOL cursor,const BYTE *data,DWORD size,int hx,int hy) {
    const BITMAPINFOHEADER *h=(const BITMAPINFOHEADER *)data; int w,ht,bpp,colors,stride,and_stride,x,y; const BYTE *xor_bits,*and_bits;
    static BYTE and_rows[64*8]; static DWORD pixels[64*64]; const RGBQUAD *pal;
    if(size<sizeof(*h) || h->biSize<sizeof(*h)) return NULL;
    w=(int)h->biWidth; ht=(int)h->biHeight/2; bpp=h->biBitCount;
    if(w<=0 || ht<=0 || w>64 || ht>64 || (bpp!=1 && bpp!=4 && bpp!=8 && bpp!=24 && bpp!=32)) return NULL;
    colors=bpp<=8?(h->biClrUsed?(int)h->biClrUsed:1<<bpp):0;
    pal=(const RGBQUAD *)(data+h->biSize);
    stride=((w*bpp+31)/32)*4; and_stride=((w+31)/32)*4;
    xor_bits=data+h->biSize+colors*4; and_bits=xor_bits+stride*ht;
    if((DWORD)(and_bits+and_stride*ht-data)>size) return NULL;
    for(y=0;y<ht;y++) {
        const BYTE *row=xor_bits+(ht-1-y)*stride,*arow=and_bits+(ht-1-y)*and_stride;
        for(x=0;x<(w+7)/8;x++) and_rows[y*8+x]=arow[x];
        for(x=0;x<w;x++) {
            DWORD v; int i;
            switch(bpp) {
            case 1: i=row[x/8]>>(7-x%8)&1; v=i<colors?(DWORD)pal[i].rgbRed<<16|(DWORD)pal[i].rgbGreen<<8|pal[i].rgbBlue:0; break;
            case 4: i=x&1?row[x/2]&15:row[x/2]>>4; v=i<colors?(DWORD)pal[i].rgbRed<<16|(DWORD)pal[i].rgbGreen<<8|pal[i].rgbBlue:0; break;
            case 8: i=row[x]; v=i<colors?(DWORD)pal[i].rgbRed<<16|(DWORD)pal[i].rgbGreen<<8|pal[i].rgbBlue:0; break;
            case 24: v=(DWORD)row[x*3+2]<<16|(DWORD)row[x*3+1]<<8|row[x*3]; break;
            default: v=((const DWORD *)row)[x]&0xffffff;
            }
            pixels[y*w+x]=v;
        }
    }
    return make_icon(cursor,w,ht,hx,hy,and_rows,8,pixels);
}
/* Pick from a group: 32x32 with the most colors up to 256. */
static int pick(const BYTE *dir,BOOL cursor) {
    int count=((const WORD *)dir)[2],i,best=-1,best_score=-1;
    for(i=0;i<count;i++) {
        const BYTE *e=dir+6+i*14; int w,h,bpp,score;
        if(cursor) {w=((const WORD *)e)[0]; h=((const WORD *)e)[1]/2; bpp=((const WORD *)e)[3];}
        else {w=e[0]?e[0]:256; h=e[1]?e[1]:256; bpp=((const WORD *)(e+6))[0]; if(!bpp) bpp=e[2]==16?4:e[2]==2?1:8;}
        score=(w==32 && h==32?1000:w<=64 && h<=64?500:0)+(bpp<=8?bpp*10:bpp);
        if(score>best_score) {best_score=score; best=i;}
    }
    return best<0?-1:((const WORD *)(dir+6+best*14+12))[0];
}
static HICON load_group(HINSTANCE instance,LPCSTR name,BOOL cursor) {
    DWORD size; const BYTE *dir=(const BYTE *)Resource(instance,cursor?RT_GROUP_CURSOR:RT_GROUP_ICON,name,&size),*data; int id; HICON h; int i;
    if(!dir) return NULL;
    for(i=0;i<ICONS;i++) if(icons[i].used && icons[i].shared && icons[i].instance==instance && icons[i].name==name && IS_INTRESOURCE(name) && icons[i].cursor==cursor)
        return (HICON)(ULONG_PTR)(HICON_BASE+i*4);
    if((id=pick(dir,cursor))<0) return NULL;
    if(!(data=(const BYTE *)Resource(instance,cursor?RT_CURSOR:RT_ICON,MAKEINTRESOURCE(id),&size))) return NULL;
    h=cursor?from_dib(TRUE,data+4,size-4,((const WORD *)data)[0],((const WORD *)data)[1]):from_dib(FALSE,data,size,16,16);
    if(h && IS_INTRESOURCE(name)) {IconObj *o=icon_of(h); o->shared=TRUE; o->instance=instance; o->name=name;}
    return h;
}
HICON WINAPI CreateIconFromResource(const BYTE FAR *bits,DWORD size,BOOL icon,DWORD version) {
    (void)version;
    if(!bits) return NULL;
    if(icon) return from_dib(FALSE,bits,size,16,16);
    return size>4?from_dib(TRUE,bits+4,size-4,((const WORD *)bits)[0],((const WORD *)bits)[1]):NULL;
}
int WINAPI LookupIconIdFromDirectory(const BYTE FAR *dir,BOOL icon) {return dir?pick(dir,!icon):0;}
HICON WINAPI LoadIcon(HINSTANCE instance,LPCSTR name) {
    if(!instance) return StockIcon(name);
    return load_group(instance,name,FALSE);
}
HCURSOR WINAPI LoadCursor(HINSTANCE instance,LPCSTR name) {
    if(!instance) return StockCursor(name);
    return load_group(instance,name,TRUE);
}
HICON WINAPI CreateIcon(HINSTANCE instance,int w,int h,BYTE planes,BYTE bpp,const BYTE FAR *and_bits,const BYTE FAR *xor_bits) {
    static DWORD pixels[64*64]; static BYTE and_rows[64*8]; int x,y,and_stride=((w+15)/16)*2,stride;
    (void)instance;
    if(w<=0 || h<=0 || w>64 || h>64 || !and_bits || !xor_bits) return NULL;
    stride=planes*bpp==1?((w+15)/16)*2:w*4;
    for(y=0;y<h;y++) {
        for(x=0;x<(w+7)/8;x++) and_rows[y*8+x]=and_bits[y*and_stride+x];
        for(x=0;x<w;x++) pixels[y*w+x]=planes*bpp==1?(xor_bits[y*stride+x/8]&(0x80>>(x%8))?0xffffff:0):((const DWORD *)(xor_bits+y*stride))[x]&0xffffff;
    }
    return make_icon(FALSE,w,h,w/2,h/2,and_rows,8,pixels);
}
HCURSOR WINAPI CreateCursor(HINSTANCE instance,int hx,int hy,int w,int h,const void FAR *and_bits,const void FAR *xor_bits) {
    HICON i=CreateIcon(instance,w,h,1,1,(const BYTE *)and_bits,(const BYTE *)xor_bits); IconObj *o=icon_of(i);
    if(o) {o->cursor=TRUE; o->hot.x=hx; o->hot.y=hy;}
    return i;
}
BOOL WINAPI DestroyIcon(HICON h) {
    IconObj *o=icon_of(h);
    if(!o) return FALSE;
    if(o->shared) return TRUE;
    if(h==current_cursor) SetCursor(StockCursor(IDC_ARROW));
    DeleteObject(o->mask); DeleteObject(o->image); o->used=FALSE;
    return TRUE;
}
BOOL WINAPI DestroyCursor(HCURSOR h) {return DestroyIcon(h);}
HICON WINAPI CopyIcon(HINSTANCE instance,HICON h) {
    IconObj *o=icon_of(h),*n; HICON c; HDC dc,from,to;
    (void)instance;
    if(!o || !(c=new_icon(&n))) return NULL;
    *n=*o; n->shared=FALSE;
    dc=GetDC(NULL); n->image=CreateCompatibleBitmap(dc,o->w,o->h); ReleaseDC(NULL,dc);
    n->mask=CreateBitmap(o->w,o->h,1,1,NULL);
    from=CreateCompatibleDC(NULL); to=CreateCompatibleDC(NULL);
    SelectObject(from,o->mask); SelectObject(to,n->mask); BitBlt(to,0,0,o->w,o->h,from,0,0,SRCCOPY);
    SelectObject(from,o->image); SelectObject(to,n->image); BitBlt(to,0,0,o->w,o->h,from,0,0,SRCCOPY);
    DeleteDC(from); DeleteDC(to);
    return c;
}
BOOL WINAPI DrawIcon(HDC dc,int x,int y,HICON h) {
    IconObj *o=icon_of(h); HDC mem; COLORREF text,bk;
    if(!o) return FALSE;
    mem=CreateCompatibleDC(dc);
    text=SetTextColor(dc,RGB(0,0,0)); bk=SetBkColor(dc,RGB(255,255,255));
    SelectObject(mem,o->mask); BitBlt(dc,x,y,o->w,o->h,mem,0,0,SRCAND);
    SelectObject(mem,o->image); BitBlt(dc,x,y,o->w,o->h,mem,0,0,SRCINVERT);
    SetTextColor(dc,text); SetBkColor(dc,bk);
    DeleteDC(mem);
    return TRUE;
}
HCURSOR WINAPI SetCursor(HCURSOR c) {
    HCURSOR old=current_cursor; IconObj *o=icon_of(c);
    if(c==current_cursor) return old;
    current_cursor=c;
    if(o) GdiSetCursor(o->and_bits,o->xor_bits,o->hot.x,o->hot.y);
    return old;
}

/* --- icons from files ---------------------------------------------------------------------- */
typedef struct {BYTE *data; DWORD size,rsrc; const BYTE *sections; int nsections;} PeFile;
static const BYTE *rva(PeFile *f,DWORD a,DWORD len) {
    int i;
    for(i=0;i<f->nsections;i++) {
        const BYTE *s=f->sections+i*40; DWORD va=*(const DWORD *)(s+12),vs=*(const DWORD *)(s+8),raw=*(const DWORD *)(s+20),rs=*(const DWORD *)(s+16);
        if(a>=va && a<va+max(vs,rs) && a-va+len<=rs && raw+(a-va)+len<=f->size) return f->data+raw+(a-va);
    }
    return NULL;
}
static BOOL pe_open(PeFile *f,LPCSTR path) {
    HFILE h=_lopen(path,OF_READ); LONG size; DWORD pe,dirs; WORD magic,nsec,opt;
    memset(f,0,sizeof(*f));
    if(h==HFILE_ERROR) return FALSE;
    size=_llseek(h,0,2); _llseek(h,0,0);
    if(size<=0x40 || size>16*1024*1024 || !(f->data=(BYTE *)GlobalAlloc(GPTR,(DWORD)size))) {_lclose(h); return FALSE;}
    f->size=_lread(h,f->data,(UINT)size); _lclose(h);
    if(f->size<0x40 || f->data[0]!='M' || f->data[1]!='Z') return FALSE;
    pe=*(const DWORD *)(f->data+0x3c);
    if(pe+24>f->size || memcmp(f->data+pe,"PE\0\0",4)) return FALSE;
    nsec=*(const WORD *)(f->data+pe+6); opt=*(const WORD *)(f->data+pe+20); magic=*(const WORD *)(f->data+pe+24);
    dirs=pe+24+(magic==0x20b?112:96);
    if(dirs+24>f->size || pe+24+opt+nsec*40>f->size) return FALSE;
    f->rsrc=*(const DWORD *)(f->data+dirs+16);
    f->sections=f->data+pe+24+opt; f->nsections=nsec;
    return f->rsrc!=0;
}
/* The n-th entry (or the one with this id when n<0) of a resource directory. */
static const BYTE *pe_entry(PeFile *f,DWORD dir,int n,WORD id,BOOL *is_dir,DWORD *target) {
    const BYTE *d=rva(f,f->rsrc+dir,16); int count,i;
    if(!d) return NULL;
    count=*(const WORD *)(d+12)+*(const WORD *)(d+14);
    for(i=0;i<count;i++) {
        const BYTE *e=rva(f,f->rsrc+dir+16+i*8,8); DWORD name,off;
        if(!e) return NULL;
        name=*(const DWORD *)e; off=*(const DWORD *)(e+4);
        if(n>=0 ? i==n : (!(name&0x80000000U) && (WORD)name==id)) {*is_dir=(off&0x80000000U)!=0; *target=off&0x7fffffffU; return e;}
    }
    return NULL;
}
static const BYTE *pe_resource(PeFile *f,WORD type,int n,WORD id,DWORD *size) {
    BOOL d; DWORD t,u,v; const BYTE *e;
    if(!pe_entry(f,0,-1,type,&d,&t) || !d) return NULL;
    if(!pe_entry(f,t,n,id,&d,&u) || !d) return NULL;
    if(!pe_entry(f,u,0,0,&d,&v) || d) return NULL;
    if(!(e=rva(f,f->rsrc+v,16))) return NULL;
    *size=*(const DWORD *)(e+4);
    return rva(f,*(const DWORD *)e,*size);
}
/* The index'th icon of a program; with (UINT)-1, how many it has. */
HICON WINAPI ExtractIcon(HINSTANCE instance,LPCSTR path,UINT index) {
    PeFile f; DWORD size; const BYTE *dir,*data; HICON h=NULL; int id;
    (void)instance;
    if(!path) return NULL;
    if(!pe_open(&f,path)) {if(f.data) GlobalFree(f.data); return NULL;}
    if(index==(UINT)-1) {
        BOOL d; DWORD t; const BYTE *e=pe_entry(&f,0,-1,14,&d,&t); const BYTE *dd;
        if(e && d && (dd=rva(&f,f.rsrc+t,16))!=NULL) h=(HICON)(ULONG_PTR)(*(const WORD *)(dd+12)+*(const WORD *)(dd+14));
    } else if((dir=pe_resource(&f,14,(int)index,0,&size))!=NULL && (id=pick(dir,FALSE))>=0 &&
              (data=pe_resource(&f,3,-1,(WORD)id,&size))!=NULL)
        h=from_dib(FALSE,data,size,16,16);
    GlobalFree(f.data);
    return h;
}
