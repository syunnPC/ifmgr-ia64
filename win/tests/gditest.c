/* SPDX-License-Identifier: GPL-2.0-or-later
 * GDI tests use GetPixel on memory DCs and report via OutputDebugString.
 * QEMU also checks the rendered window and decodes C:\WINDOWS\GDITEST.PS.
 *
 * Window: red ellipse at (10,10)-(60,60); doubled blue/green bitmap at
 * (70,10)-(130,40); red/yellow monochrome bitmap at (140,10)-(156,26);
 * black/red/green/blue DIB quadrants at (10,70)-(74,134); magenta XOR region
 * at (90,70)-(150,130); Courier 15 bold text at (10,150).
 *
 * Four Letter pages at 150 dpi: red/black rectangle and "Printed"; blue band
 * via StartPage/EndPage; grey rectangle and 300 colors via NEXTBAND; black
 * "Last page" via ENDDOC.
 */
#include <windows.h>

static int passed,failed;
static void check(BOOL ok,int line) {
    char text[64];
    if(ok) {passed++; return;}
    failed++; wsprintf(text,"GDITEST: FAIL line %d",line); OutputDebugString(text);
}
#define CHECK(x) check((x)!=0,__LINE__)

static void tests(void) {
    HDC screen=GetDC(NULL),mem=CreateCompatibleDC(screen),mem2=CreateCompatibleDC(screen);
    HBITMAP bm=CreateCompatibleBitmap(screen,40,40),old,mono,bm2;
    HBRUSH red=CreateSolidBrush(RGB(255,0,0)),hatch=CreateHatchBrush(HS_CROSS,RGB(0,0,255));
    HRGN a,b,c; BITMAP info; TEXTMETRIC tm; LOGFONT lf; HFONT font; RECT r; SIZE size;
    static const BYTE mono_bits[4]={0xaa,0,0x55,0}; /* 8x2, rows of a word: 10101010 / 01010101 */
    struct {BITMAPINFOHEADER h; RGBQUAD colors[4];} dib; BYTE pixels[4*4]; char face[LF_FACESIZE]; int i;

    CHECK(screen && mem && bm);
    CHECK(GetDeviceCaps(screen,BITSPIXEL)==32 && GetDeviceCaps(screen,HORZRES)==800);
    CHECK(GetDeviceCaps(mem,BITSPIXEL)==1); /* the default bitmap is 1x1 monochrome */
    old=SelectObject(mem,bm); CHECK(old!=NULL);
    CHECK(GetObject(bm,sizeof(info),&info)==sizeof(info) && info.bmWidth==40 && info.bmHeight==40 && info.bmBitsPixel==32);
    PatBlt(mem,0,0,40,40,WHITENESS);
    CHECK(GetPixel(mem,5,5)==RGB(255,255,255));
    /* Rectangle: the pen inside the box, brush within, right/bottom exclusive. */
    SelectObject(mem,red); Rectangle(mem,2,2,12,12);
    CHECK(GetPixel(mem,2,2)==RGB(0,0,0) && GetPixel(mem,11,11)==RGB(0,0,0) && GetPixel(mem,12,12)==RGB(255,255,255));
    CHECK(GetPixel(mem,6,6)==RGB(255,0,0));
    /* Ellipse: centre filled, bounding-box corner untouched. */
    PatBlt(mem,0,0,40,40,WHITENESS); Ellipse(mem,0,0,40,40);
    CHECK(GetPixel(mem,20,20)==RGB(255,0,0) && GetPixel(mem,1,1)==RGB(255,255,255) && GetPixel(mem,20,0)==RGB(0,0,0));
    /* Raster operations. */
    PatBlt(mem,0,0,40,40,PATINVERT);
    CHECK(GetPixel(mem,20,20)==RGB(0,0,0) && GetPixel(mem,1,1)==RGB(0,255,255));
    PatBlt(mem,0,0,40,40,DSTINVERT); CHECK(GetPixel(mem,1,1)==RGB(255,0,0));
    PatBlt(mem,0,0,40,40,BLACKNESS); CHECK(GetPixel(mem,1,1)==RGB(0,0,0));
    /* Hatched brush: lines in the brush color, gaps in the background. */
    SelectObject(mem,hatch); SetBkColor(mem,RGB(255,255,0)); PatBlt(mem,0,0,16,16,PATCOPY);
    CHECK(GetPixel(mem,4,4)==RGB(0,0,255) && GetPixel(mem,1,1)==RGB(255,255,0));
    SetBkColor(mem,RGB(255,255,255));
    /* Clipping. */
    PatBlt(mem,0,0,40,40,WHITENESS); SelectObject(mem,red);
    IntersectClipRect(mem,10,10,20,20); PatBlt(mem,0,0,40,40,PATCOPY);
    CHECK(GetPixel(mem,15,15)==RGB(255,0,0) && GetPixel(mem,25,25)==(COLORREF)-1);
    CHECK(GetClipBox(mem,&r)==SIMPLEREGION && r.left==10 && r.bottom==20);
    ExcludeClipRect(mem,12,12,14,14); CHECK(GetClipBox(mem,&r)==COMPLEXREGION);
    SelectClipRgn(mem,NULL); CHECK(GetPixel(mem,25,25)==RGB(255,255,255));
    /* SaveDC/RestoreDC bring back objects and colors. */
    i=SaveDC(mem); SetTextColor(mem,RGB(1,2,3)); SelectObject(mem,GetStockObject(BLACK_BRUSH));
    RestoreDC(mem,i); CHECK(GetTextColor(mem)==RGB(0,0,0));
    PatBlt(mem,0,0,2,2,PATCOPY); CHECK(GetPixel(mem,0,0)==RGB(255,0,0));
    /* Monochrome to color: 1 bits take the background, 0 bits the text color. */
    mono=CreateBitmap(8,2,1,1,mono_bits);
    CHECK(GetObject(mono,sizeof(info),&info) && info.bmBitsPixel==1 && info.bmWidthBytes==2);
    SelectObject(mem2,mono);
    SetTextColor(mem,RGB(255,0,0)); SetBkColor(mem,RGB(255,255,0));
    BitBlt(mem,0,0,8,2,mem2,0,0,SRCCOPY);
    CHECK(GetPixel(mem,0,0)==RGB(255,255,0) && GetPixel(mem,1,0)==RGB(255,0,0) && GetPixel(mem,0,1)==RGB(255,0,0));
    /* Color to monochrome: the background color becomes white. */
    PatBlt(mem,0,0,8,2,WHITENESS); SetPixel(mem,3,0,RGB(0,0,255)); SetBkColor(mem,RGB(255,255,255));
    BitBlt(mem2,0,0,8,2,mem,0,0,SRCCOPY);
    CHECK(GetPixel(mem2,3,0)==RGB(0,0,0) && GetPixel(mem2,4,0)==RGB(255,255,255));
    {
        BYTE out[4]; CHECK(GetBitmapBits(mono,sizeof(out),out)==4 && out[0]==0xef && out[2]==0xff);
    }
    /* StretchBlt doubles, and mirrors with a negative width. */
    bm2=CreateCompatibleBitmap(screen,4,4); SelectObject(mem2,bm2);
    PatBlt(mem2,0,0,4,4,BLACKNESS); SetPixel(mem2,0,0,RGB(0,255,0));
    StretchBlt(mem,0,0,8,8,mem2,0,0,4,4,SRCCOPY);
    CHECK(GetPixel(mem,1,1)==RGB(0,255,0) && GetPixel(mem,2,2)==RGB(0,0,0));
    StretchBlt(mem,8,0,-4,4,mem2,0,0,4,4,SRCCOPY);
    CHECK(GetPixel(mem,7,0)==RGB(0,255,0) && GetPixel(mem,4,0)==RGB(0,0,0));
    /* Overlapping BitBlt on one bitmap. */
    PatBlt(mem,0,0,40,40,WHITENESS); SetPixel(mem,0,0,RGB(9,9,9)); SetPixel(mem,1,0,RGB(8,8,8));
    BitBlt(mem,1,0,10,1,mem,0,0,SRCCOPY);
    CHECK(GetPixel(mem,1,0)==RGB(9,9,9) && GetPixel(mem,2,0)==RGB(8,8,8));
    BitBlt(mem,0,0,2,1,mem,0,0,SRCINVERT); CHECK(GetPixel(mem,0,0)==RGB(0,0,0));
    /* DIBs: a bottom-up 4x4 image with a 4-color table, 8 bits per pixel. */
    memset(&dib,0,sizeof(dib));
    dib.h.biSize=sizeof(dib.h); dib.h.biWidth=4; dib.h.biHeight=4; dib.h.biPlanes=1; dib.h.biBitCount=8; dib.h.biClrUsed=4;
    dib.colors[1].rgbRed=255; dib.colors[2].rgbGreen=255; dib.colors[3].rgbBlue=255;
    for(i=0;i<16;i++) pixels[i]=(BYTE)((i/4<2?2:0)+(i%4<2?0:1)); /* bottom rows: green, blue */
    SetDIBitsToDevice(mem,0,0,4,4,0,0,0,4,pixels,(BITMAPINFO *)&dib,DIB_RGB_COLORS);
    CHECK(GetPixel(mem,0,0)==RGB(0,0,0) && GetPixel(mem,3,0)==RGB(255,0,0) && GetPixel(mem,0,3)==RGB(0,255,0) && GetPixel(mem,3,3)==RGB(0,0,255));
    SelectObject(mem,old);
    CHECK(SetDIBits(screen,bm,0,4,pixels,(BITMAPINFO *)&dib,DIB_RGB_COLORS)==4);
    SelectObject(mem,bm); CHECK(GetPixel(mem,3,39)==RGB(0,0,255) && GetPixel(mem,0,36)==RGB(0,0,0));
    SelectObject(mem,old);
    {
        struct {BITMAPINFOHEADER h; RGBQUAD colors[256];} out; static BYTE bits[40*40*4];
        memset(&out,0,sizeof(out)); out.h.biSize=sizeof(out.h); out.h.biBitCount=24;
        CHECK(GetDIBits(screen,bm,0,40,NULL,(BITMAPINFO *)&out,DIB_RGB_COLORS)==40 && out.h.biWidth==40 && out.h.biSizeImage==40*120);
        CHECK(GetDIBits(screen,bm,0,40,bits,(BITMAPINFO *)&out,DIB_RGB_COLORS)==40 && bits[3*3]==255 && bits[3*3+2]==0);
    }
    /* Regions. */
    a=CreateRectRgn(0,0,60,60); b=CreateRectRgn(20,20,40,40); c=CreateRectRgn(0,0,0,0);
    CHECK(CombineRgn(c,a,b,RGN_XOR)==COMPLEXREGION && !PtInRegion(c,30,30) && PtInRegion(c,10,10));
    CHECK(CombineRgn(c,a,b,RGN_AND)==SIMPLEREGION && GetRgnBox(c,&r)==SIMPLEREGION && r.left==20 && r.right==40);
    CHECK(EqualRgn(c,b) && !EqualRgn(a,b));
    CHECK(OffsetRgn(c,5,5)==SIMPLEREGION && PtInRegion(c,44,44) && !PtInRegion(c,20,20));
    DeleteObject(a); DeleteObject(b); DeleteObject(c);
    /* Fonts. */
    font=CreateFont(15,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,0,0,0,FIXED_PITCH|FF_MODERN,"Courier");
    SelectObject(screen,font); GetTextMetrics(screen,&tm); GetTextFace(screen,sizeof(face),face);
    CHECK(tm.tmHeight==15 && tm.tmAveCharWidth==9 && tm.tmWeight==FW_BOLD && !lstrcmp(face,"Courier"));
    GetTextExtentPoint(screen,"abc",3,&size); CHECK(size.cx==27 && size.cy==15);
    SetTextCharacterExtra(screen,2); GetTextExtentPoint(screen,"abc",3,&size); CHECK(size.cx==33);
    SelectObject(screen,GetStockObject(SYSTEM_FONT)); GetTextMetrics(screen,&tm); GetTextFace(screen,sizeof(face),face);
    CHECK(tm.tmHeight==16 && tm.tmAveCharWidth==7 && tm.tmInternalLeading==3 && !lstrcmp(face,"System"));
    {int i_w,w_w; GetCharWidth(screen,'i','i',&i_w); GetCharWidth(screen,'W','W',&w_w); CHECK(i_w<w_w && (tm.tmPitchAndFamily&TMPF_FIXED_PITCH));}
    /* A character height below the smallest size gets the smallest. */
    memset(&lf,0,sizeof(lf)); lf.lfHeight=-10; lstrcpy(lf.lfFaceName,"Helv");
    DeleteObject(font); font=CreateFontIndirect(&lf); SelectObject(screen,font); GetTextMetrics(screen,&tm);
    CHECK(tm.tmHeight==13 && tm.tmAveCharWidth==5 && tm.tmInternalLeading==2);
    CHECK(GetObject(font,sizeof(lf),&lf)==sizeof(lf) && lf.lfHeight==-10);
    /* Raster fonts scale by whole multiples: Helv 20 three times for 60, 13
     * twice for 26 (nearer than 20), across only as asked when a width is. */
    {
        TEXTMETRIC tm20; SIZE one,three;
        SetTextCharacterExtra(screen,0);
        lf.lfHeight=20; DeleteObject(font); font=CreateFontIndirect(&lf); SelectObject(screen,font);
        GetTextMetrics(screen,&tm20); GetTextExtentPoint(screen,"abc",3,&one);
        lf.lfHeight=60; DeleteObject(font); font=CreateFontIndirect(&lf); SelectObject(screen,font); GetTextMetrics(screen,&tm);
        GetTextExtentPoint(screen,"abc",3,&three);
        CHECK(tm20.tmHeight==20 && tm.tmHeight==60 && tm.tmAscent==3*tm20.tmAscent && tm.tmAveCharWidth==3*tm20.tmAveCharWidth);
        CHECK(three.cx==3*one.cx && three.cy==60);
        lf.lfHeight=26; DeleteObject(font); font=CreateFontIndirect(&lf); SelectObject(screen,font); GetTextMetrics(screen,&tm);
        CHECK(tm.tmHeight==26 && tm.tmAveCharWidth==10);
        lf.lfHeight=40; lf.lfWidth=tm20.tmAveCharWidth; DeleteObject(font); font=CreateFontIndirect(&lf); SelectObject(screen,font);
        GetTextMetrics(screen,&tm); GetTextExtentPoint(screen,"abc",3,&three);
        CHECK(tm.tmHeight==40 && tm.tmAveCharWidth==tm20.tmAveCharWidth && three.cx==one.cx);
        lf.lfWidth=0;
    }
    SelectObject(screen,GetStockObject(SYSTEM_FONT)); DeleteObject(font);
    /* Text into a memory bitmap: opaque cell in the background color. */
    SelectObject(mem,bm); PatBlt(mem,0,0,40,40,WHITENESS);
    SetTextColor(mem,RGB(0,0,0)); SetBkColor(mem,RGB(0,255,0)); TextOut(mem,0,0,"I",1);
    CHECK(GetPixel(mem,0,0)==RGB(0,255,0) && GetPixel(mem,8,0)==RGB(255,255,255));
    {
        int dark=0,x,y;
        for(y=0;y<13;y++) for(x=0;x<8;x++) dark+=GetPixel(mem,x,y)==RGB(0,0,0);
        CHECK(dark>8);
    }
    CHECK(!DeleteObject(bm)); /* still selected */
    SelectObject(mem,old);
    CHECK(DeleteDC(mem2) && DeleteObject(mono) && DeleteObject(bm2));
    DeleteDC(mem); CHECK(DeleteObject(bm)); DeleteObject(red); DeleteObject(hatch);
    ReleaseDC(NULL,screen);
}

static int CALLBACK count_font(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    (void)type;
    if(lf->lfFaceName[0] && tm->tmHeight>0) (*(int *)lp)++;
    return 1;
}
static void CALLBACK count_point(int x,int y,LPARAM lp) {if(y==0 && x>=0) (*(int *)lp)++;}
/* Mapping modes, arcs, flood fills, regions from shapes, palettes and the
 * rest of Windows 3.0's GDI, on a 64x64 memory bitmap. */
static void more_tests(void) {
    HDC screen=GetDC(NULL),mem=CreateCompatibleDC(screen),display;
    HBITMAP bm=CreateCompatibleBitmap(screen,64,64),old=SelectObject(mem,bm);
    HBRUSH red=CreateSolidBrush(RGB(255,0,0)),blue=CreateSolidBrush(RGB(0,0,255)),green=CreateSolidBrush(RGB(0,255,0));
    HRGN rgn; HPALETTE pal,oldpal; POINT p[8]; SIZE s; RECT r; int i,n,counts[2],dark;
    struct {WORD version,count; PALETTEENTRY e[2];} lp;
    WORD entries;

    /* Mapping modes. */
    PatBlt(mem,0,0,64,64,WHITENESS);
    CHECK(GetMapMode(mem)==MM_TEXT);
    SetViewportOrg(mem,10,10); SetPixel(mem,0,0,RGB(255,0,0)); SetViewportOrg(mem,0,0);
    CHECK(GetPixel(mem,10,10)==RGB(255,0,0));
    CHECK(SetMapMode(mem,MM_ANISOTROPIC)==MM_TEXT);
    SetWindowExt(mem,100,100); SetViewportExt(mem,50,50);
    CHECK(GetWindowExtEx(mem,&s) && s.cx==100 && s.cy==100 && GetViewportExt(mem)==MAKELONG(50,50));
    SelectObject(mem,red); SelectObject(mem,GetStockObject(NULL_PEN));
    Rectangle(mem,20,20,60,60);
    /* GetPixel takes logical coordinates too: device (15,15) is (30,30). */
    CHECK(GetPixel(mem,30,30)==RGB(255,0,0) && GetPixel(mem,24,24)==RGB(255,0,0) && GetPixel(mem,70,70)==RGB(255,255,255) && GetPixel(mem,10,10)==RGB(255,255,255));
    SetMapMode(mem,MM_TEXT); CHECK(GetPixel(mem,15,15)==RGB(255,0,0) && GetPixel(mem,35,35)==RGB(255,255,255));
    SetMapMode(mem,MM_ANISOTROPIC); SetWindowExt(mem,100,100); SetViewportExt(mem,50,50);
    p[0].x=100; p[0].y=40; LPtoDP(mem,p,1); CHECK(p[0].x==50 && p[0].y==20);
    DPtoLP(mem,p,1); CHECK(p[0].x==100 && p[0].y==40);
    CHECK(GetClipBox(mem,&r)!=ERROR && r.left==0 && r.right==128 && r.bottom==128);
    SetWindowOrg(mem,-20,0); p[0].x=0; p[0].y=0; LPtoDP(mem,p,1); CHECK(p[0].x==10 && p[0].y==0);
    SetWindowOrg(mem,0,0);
    SetMapMode(mem,MM_ISOTROPIC); SetWindowExt(mem,100,100); SetViewportExt(mem,64,32);
    CHECK(GetViewportExtEx(mem,&s) && s.cx==32 && s.cy==32);
    SetMapMode(mem,MM_LOENGLISH); p[0].x=100; p[0].y=-50; LPtoDP(mem,p,1); CHECK(p[0].x==96 && p[0].y==48);
    SetMapMode(mem,MM_TEXT); CHECK(GetWindowExt(mem)==MAKELONG(1,1));
    i=SaveDC(mem); SetMapMode(mem,MM_TWIPS); RestoreDC(mem,i); CHECK(GetMapMode(mem)==MM_TEXT);
    /* Pie, chord and arc in a 40x40 box: the sweep runs counterclockwise. */
    PatBlt(mem,0,0,64,64,WHITENESS);
    SelectObject(mem,red); SelectObject(mem,GetStockObject(NULL_PEN));
    Pie(mem,0,0,40,40,40,20,20,0);
    CHECK(GetPixel(mem,28,12)==RGB(255,0,0) && GetPixel(mem,12,12)==RGB(255,255,255) && GetPixel(mem,28,28)==RGB(255,255,255));
    PatBlt(mem,0,0,64,64,WHITENESS);
    Chord(mem,0,0,40,40,40,20,0,20);
    CHECK(GetPixel(mem,20,10)==RGB(255,0,0) && GetPixel(mem,20,30)==RGB(255,255,255));
    PatBlt(mem,0,0,64,64,WHITENESS);
    SelectObject(mem,GetStockObject(BLACK_PEN)); Arc(mem,0,0,40,40,40,20,20,0);
    for(dark=0,n=0,i=0;i<64;i++) {
        if(GetPixel(mem,30+i%8,2+i/8)==RGB(0,0,0)) dark++;
        if(GetPixel(mem,2+i%8,30+i/8)==RGB(0,0,0)) n++;
    }
    CHECK(dark>2 && n==0);
    /* Flood fills: to a border, and over a color. */
    PatBlt(mem,0,0,64,64,WHITENESS);
    SelectObject(mem,GetStockObject(NULL_BRUSH)); Rectangle(mem,10,10,30,30);
    SelectObject(mem,blue); CHECK(FloodFill(mem,20,20,RGB(0,0,0)));
    CHECK(GetPixel(mem,20,20)==RGB(0,0,255) && GetPixel(mem,11,28)==RGB(0,0,255) && GetPixel(mem,10,20)==RGB(0,0,0) && GetPixel(mem,5,5)==RGB(255,255,255));
    SelectObject(mem,green); CHECK(ExtFloodFill(mem,2,2,RGB(255,255,255),FLOODFILLSURFACE));
    CHECK(GetPixel(mem,2,2)==RGB(0,255,0) && GetPixel(mem,63,63)==RGB(0,255,0) && GetPixel(mem,20,20)==RGB(0,0,255));
    /* Polygons together, and regions from shapes. */
    PatBlt(mem,0,0,64,64,WHITENESS); SelectObject(mem,red); SelectObject(mem,GetStockObject(NULL_PEN));
    p[0].x=0; p[0].y=0; p[1].x=10; p[1].y=0; p[2].x=10; p[2].y=10; p[3].x=0; p[3].y=10;
    p[4].x=20; p[4].y=20; p[5].x=30; p[5].y=20; p[6].x=30; p[6].y=30; p[7].x=20; p[7].y=30;
    counts[0]=counts[1]=4; PolyPolygon(mem,p,counts,2);
    CHECK(GetPixel(mem,5,5)==RGB(255,0,0) && GetPixel(mem,25,25)==RGB(255,0,0) && GetPixel(mem,15,15)==RGB(255,255,255));
    rgn=CreateEllipticRgn(0,0,40,20);
    CHECK(rgn && PtInRegion(rgn,20,10) && !PtInRegion(rgn,1,1) && GetRgnBox(rgn,&r)==COMPLEXREGION && r.top==0 && r.bottom==20);
    DeleteObject(rgn);
    p[0].x=0; p[0].y=0; p[1].x=40; p[1].y=0; p[2].x=0; p[2].y=40;
    rgn=CreatePolygonRgn(p,3,ALTERNATE); CHECK(rgn && PtInRegion(rgn,5,5) && !PtInRegion(rgn,35,35)); DeleteObject(rgn);
    rgn=CreateRoundRectRgn(0,0,40,40,20,20); CHECK(rgn && !PtInRegion(rgn,1,1) && PtInRegion(rgn,20,1)); DeleteObject(rgn);
    SetMapMode(mem,MM_ANISOTROPIC); SetWindowExt(mem,2,2); SetViewportExt(mem,1,1);
    rgn=CreateRectRgn(0,0,20,20); PatBlt(mem,0,0,64,64,WHITENESS); FillRgn(mem,rgn,blue);
    SetMapMode(mem,MM_TEXT);
    CHECK(GetPixel(mem,9,9)==RGB(0,0,255) && GetPixel(mem,11,11)==RGB(255,255,255)); DeleteObject(rgn);
    IntersectClipRect(mem,0,0,10,10); CHECK(OffsetClipRgn(mem,10,10)==SIMPLEREGION && PtVisible(mem,15,15) && !PtVisible(mem,5,5));
    SelectClipRgn(mem,NULL);
    /* Palettes: PALETTEINDEX colors come from the DC's palette. */
    lp.version=0x300; lp.count=2;
    lp.e[0].peRed=255; lp.e[0].peGreen=lp.e[0].peBlue=0; lp.e[0].peFlags=0;
    lp.e[1].peRed=lp.e[1].peGreen=0; lp.e[1].peBlue=255; lp.e[1].peFlags=0;
    pal=CreatePalette((LOGPALETTE *)&lp);
    CHECK(pal && GetObjectType(pal)==OBJ_PAL && GetObject(pal,sizeof(entries),&entries)==sizeof(entries) && entries==2);
    oldpal=SelectPalette(mem,pal,FALSE); CHECK(oldpal==GetStockObject(DEFAULT_PALETTE));
    RealizePalette(mem);
    SetPixel(mem,0,0,PALETTEINDEX(1)); CHECK(GetPixel(mem,0,0)==RGB(0,0,255));
    CHECK(GetNearestPaletteIndex(pal,RGB(250,10,0))==0 && GetPaletteEntries(pal,0,0,NULL)==2);
    SelectPalette(mem,oldpal,FALSE); DeleteObject(pal);
    CHECK(GetObjectType(GetStockObject(DEFAULT_PALETTE))==OBJ_PAL && GetObjectType(bm)==OBJ_BITMAP && GetObjectType(mem)==OBJ_DC);
    /* Text justification, fonts, LineDDA, bitmap dimensions, CreateDC. */
    SelectObject(mem,GetStockObject(SYSTEM_FONT));
    GetTextExtentPoint(mem,"a b",3,&s); n=s.cx;
    SetTextJustification(mem,10,1); GetTextExtentPoint(mem,"a b",3,&s); CHECK(s.cx==n+10);
    SetTextJustification(mem,0,0);
    n=0; EnumFonts(mem,NULL,count_font,(LPARAM)&n); CHECK(n==13);
    n=0; EnumFonts(mem,"Helv",count_font,(LPARAM)&n); CHECK(n==3);
    n=0; LineDDA(0,0,10,0,count_point,(LPARAM)&n); CHECK(n==10);
    SetBitmapDimension(bm,100,50); CHECK(GetBitmapDimension(bm)==MAKELONG(100,50));
    display=CreateDC("DISPLAY",NULL,NULL,NULL); CHECK(display && GetDeviceCaps(display,HORZRES)==800); DeleteDC(display);
    CHECK(!CreateDC("EPSON",NULL,NULL,NULL));
    SelectObject(mem,old); DeleteDC(mem); DeleteObject(bm);
    DeleteObject(red); DeleteObject(blue); DeleteObject(green);
    ReleaseDC(NULL,screen);
}

/* Metafiles: recorded, played back, enumerated, out to bits and a file and in again. */
static int CALLBACK count_record(HDC dc,HANDLETABLE FAR *t,METARECORD UNALIGNED FAR *r,int n,LPARAM lp) {
    int *counts=(int *)lp;
    counts[0]++;
    if(r->rdFunction==META_RECTANGLE) counts[1]++;
    PlayMetaFileRecord(dc,t,r,(UINT)n);
    return TRUE;
}
static int blue_pixels(HDC dc,int x0,int y0,int w,int h) {
    int x,y,n=0;
    for(y=y0;y<y0+h;y++) for(x=x0;x<x0+w;x++) if(GetPixel(dc,x,y)==RGB(0,0,255)) n++;
    return n;
}
static void metafile_tests(void) {
    HDC screen=GetDC(NULL),mem=CreateCompatibleDC(screen),src=CreateCompatibleDC(screen),mf;
    HBITMAP bm=CreateCompatibleBitmap(screen,64,64),old=SelectObject(mem,bm),sbm=CreateCompatibleBitmap(screen,8,8),sold;
    HBRUSH red=CreateSolidBrush(RGB(255,0,0)),blue=CreateSolidBrush(RGB(0,0,255)); HPEN green=CreatePen(PS_SOLID,1,RGB(0,255,0));
    HRGN rgn; HMETAFILE h,h2,h3; UINT size; BYTE *bits; HGLOBAL g; int counts[2];

    sold=SelectObject(src,sbm); PatBlt(src,0,0,8,8,BLACKNESS); SetPixel(src,0,0,RGB(255,255,0));
    mf=CreateMetaFile(NULL);
    CHECK(mf && GetObjectType(mf)==OBJ_METADC && GetDeviceCaps(mf,TECHNOLOGY)==DT_METAFILE);
    SelectObject(mf,red); SelectObject(mf,GetStockObject(NULL_PEN));
    Rectangle(mf,0,0,10,10);
    SelectObject(mf,blue); Ellipse(mf,20,0,40,20);
    SelectObject(mf,green); MoveToEx(mf,0,30,NULL); LineTo(mf,30,30);
    SetTextColor(mf,RGB(0,0,255)); SetBkMode(mf,TRANSPARENT); TextOut(mf,40,40,"Hi",2);
    rgn=CreateRectRgn(50,0,60,10); FillRgn(mf,rgn,red); DeleteObject(rgn);
    BitBlt(mf,0,50,8,8,src,0,0,SRCCOPY);
    StretchBlt(mf,10,50,16,8,src,0,0,8,8,SRCCOPY);
    SaveDC(mf); SetWindowOrgEx(mf,-30,-50,NULL); SetPixel(mf,0,0,RGB(1,2,3)); RestoreDC(mf,-1);
    SelectObject(mf,GetStockObject(WHITE_BRUSH)); DeleteObject(blue);
    h=CloseMetaFile(mf);
    CHECK(h && GetObjectType(h)==OBJ_METAFILE);
    /* Played: what was drawn, and the DC's pen as it was. */
    PatBlt(mem,0,0,64,64,WHITENESS); SelectObject(mem,GetStockObject(BLACK_PEN));
    CHECK(PlayMetaFile(mem,h));
    CHECK(GetPixel(mem,5,5)==RGB(255,0,0) && GetPixel(mem,30,10)==RGB(0,0,255) && GetPixel(mem,15,30)==RGB(0,255,0) && GetPixel(mem,55,5)==RGB(255,0,0));
    CHECK(GetPixel(mem,0,50)==RGB(255,255,0) && GetPixel(mem,1,50)==RGB(0,0,0));
    CHECK(GetPixel(mem,11,50)==RGB(255,255,0) && GetPixel(mem,12,50)==RGB(0,0,0) && GetPixel(mem,30,50)==RGB(1,2,3));
    CHECK(blue_pixels(mem,40,40,24,20)>5);
    CHECK(SelectObject(mem,GetStockObject(BLACK_PEN))==GetStockObject(BLACK_PEN));
    /* The bits: a memory metafile's header, nine words. */
    size=GetMetaFileBitsEx(h,0,NULL);
    bits=(BYTE *)GlobalAlloc(GPTR,size);
    CHECK(size>18 && bits && GetMetaFileBitsEx(h,size,bits)==size && bits[0]==1 && bits[2]==9 && bits[4]==0 && bits[5]==3);
    h2=SetMetaFileBitsEx(size,bits); CHECK(h2!=NULL); GlobalFree(bits);
    /* Enumerated, each record played by the callback. */
    PatBlt(mem,0,0,64,64,WHITENESS); counts[0]=counts[1]=0;
    CHECK(EnumMetaFile(mem,h2,count_record,(LPARAM)counts) && counts[0]>15 && counts[1]==1);
    CHECK(GetPixel(mem,5,5)==RGB(255,0,0) && GetPixel(mem,0,50)==RGB(255,255,0));
    /* A disk metafile (the QEMU test reads the file), read back. */
    CHECK(DeleteMetaFile(h2) && !DeleteMetaFile(h2));
    h2=CopyMetaFile(h,"C:\\WINDOWS\\GDITEST.WMF"); CHECK(h2!=NULL); DeleteMetaFile(h2);
    h3=GetMetaFile("C:\\WINDOWS\\GDITEST.WMF"); CHECK(h3 && GetMetaFileBitsEx(h3,0,NULL)==size);
    /* Windows 3.0's bits in a global block: the metafile goes, and comes back. */
    g=GetMetaFileBits(h3); CHECK(g && GlobalSize(g)>=size && !GetObjectType(h3));
    h3=SetMetaFileBits(g); CHECK(h3!=NULL);
    /* Scaled by the DC's mapping: half size. */
    PatBlt(mem,0,0,64,64,WHITENESS);
    SetMapMode(mem,MM_ANISOTROPIC); SetWindowExt(mem,2,2); SetViewportExt(mem,1,1);
    PlayMetaFile(mem,h3); SetMapMode(mem,MM_TEXT);
    CHECK(GetPixel(mem,2,2)==RGB(255,0,0) && GetPixel(mem,7,7)==RGB(255,255,255) && GetPixel(mem,15,5)==RGB(0,0,255));
    /* Played into another metafile DC: recorded again. */
    mf=CreateMetaFile(NULL); PlayMetaFile(mf,h); DeleteMetaFile(h3); h3=CloseMetaFile(mf);
    PatBlt(mem,0,0,64,64,WHITENESS); CHECK(h3 && PlayMetaFile(mem,h3));
    CHECK(GetPixel(mem,5,5)==RGB(255,0,0) && GetPixel(mem,11,50)==RGB(255,255,0) && GetPixel(mem,55,5)==RGB(255,0,0));
    DeleteMetaFile(h3); DeleteMetaFile(h);
    SelectObject(src,sold); DeleteDC(src); DeleteObject(sbm);
    SelectObject(mem,old); DeleteDC(mem); DeleteObject(bm);
    DeleteObject(red); DeleteObject(green);
    ReleaseDC(NULL,screen);
}

static void paint(HDC dc) {
    HDC mem=CreateCompatibleDC(dc); HBITMAP bm=CreateCompatibleBitmap(dc,30,15),mono,old;
    HBRUSH red=CreateSolidBrush(RGB(255,0,0)),blue=CreateSolidBrush(RGB(0,0,255)),green=CreateSolidBrush(RGB(0,255,0));
    HBRUSH magenta=CreateSolidBrush(RGB(255,0,255)),oldbrush;
    HRGN a=CreateRectRgn(90,70,150,130),b=CreateRectRgn(110,90,130,110);
    static const WORD checker[16]={0xaaaa,0x5555,0xaaaa,0x5555,0xaaaa,0x5555,0xaaaa,0x5555,
                                   0xaaaa,0x5555,0xaaaa,0x5555,0xaaaa,0x5555,0xaaaa,0x5555};
    struct {BITMAPINFOHEADER h; RGBQUAD colors[4];} dib; static BYTE pixels[64*64]; HFONT font; int i;

    oldbrush=SelectObject(dc,red); Ellipse(dc,10,10,60,60); SelectObject(dc,oldbrush);
    old=SelectObject(mem,bm);
    SelectObject(mem,blue); PatBlt(mem,0,0,15,15,PATCOPY);
    SelectObject(mem,green); PatBlt(mem,15,0,15,15,PATCOPY);
    StretchBlt(dc,70,10,60,30,mem,0,0,30,15,SRCCOPY);
    SelectObject(mem,old); SelectObject(mem,GetStockObject(WHITE_BRUSH));
    mono=CreateBitmap(16,16,1,1,checker); SelectObject(mem,mono);
    SetTextColor(dc,RGB(255,0,0)); SetBkColor(dc,RGB(255,255,0));
    BitBlt(dc,140,10,16,16,mem,0,0,SRCCOPY);
    SetTextColor(dc,RGB(0,0,0)); SetBkColor(dc,RGB(255,255,255));
    memset(&dib,0,sizeof(dib));
    dib.h.biSize=sizeof(dib.h); dib.h.biWidth=64; dib.h.biHeight=64; dib.h.biPlanes=1; dib.h.biBitCount=8; dib.h.biClrUsed=4;
    dib.colors[1].rgbRed=255; dib.colors[2].rgbGreen=255; dib.colors[3].rgbBlue=255;
    /* Bottom-up: the first rows are the bottom half (green, blue). */
    for(i=0;i<64*64;i++) pixels[i]=(BYTE)((i/64<32?2:0)+(i%64<32?0:1));
    StretchDIBits(dc,10,70,64,64,0,0,64,64,pixels,(BITMAPINFO *)&dib,DIB_RGB_COLORS,SRCCOPY);
    CombineRgn(a,a,b,RGN_XOR); FillRgn(dc,a,magenta);
    font=CreateFont(15,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,0,0,0,FIXED_PITCH|FF_MODERN,"Courier");
    SelectObject(dc,font); TextOut(dc,10,150,"Courier 15 bold",15);
    SelectObject(dc,GetStockObject(SYSTEM_FONT)); DeleteObject(font);
    SelectObject(mem,old); DeleteDC(mem);
    DeleteObject(bm); DeleteObject(mono); DeleteObject(a); DeleteObject(b);
    DeleteObject(red); DeleteObject(blue); DeleteObject(green); DeleteObject(magenta);
}

static int aborts_called;
static BOOL CALLBACK abort_proc(HDC h,int code) {(void)h; (void)code; aborts_called++; return TRUE;}
static void print_tests(void) {
    HDC p=CreateDC("PSCRIPT","PostScript Printer","C:\\WINDOWS\\GDITEST.PS",NULL),ic;
    HBRUSH red=CreateSolidBrush(RGB(255,0,0)),blue=CreateSolidBrush(RGB(0,0,255)),gray=CreateSolidBrush(RGB(128,128,128));
    POINT pt; RECT band; int code,bands=0,n; char tech[32]; DOCINFO doc; OFSTRUCT of; DEVMODE dm;
    CHECK(p!=NULL);
    if(!p) return;
    CHECK(GetDeviceCaps(p,TECHNOLOGY)==DT_RASPRINTER && GetDeviceCaps(p,LOGPIXELSY)==150);
    CHECK(GetDeviceCaps(p,HORZRES)==1200 && GetDeviceCaps(p,VERTRES)==1575);
    code=NEWFRAME; CHECK(Escape(p,QUERYESCSUPPORT,sizeof(code),(LPCSTR)&code,NULL));
    code=PASSTHROUGH; CHECK(!Escape(p,QUERYESCSUPPORT,sizeof(code),(LPCSTR)&code,NULL));
    CHECK(Escape(p,GETPHYSPAGESIZE,0,NULL,&pt)>0 && pt.x==1275 && pt.y==1650);
    CHECK(Escape(p,GETPRINTINGOFFSET,0,NULL,&pt)>0 && pt.x==37 && pt.y==37);
    CHECK(Escape(p,GETTECHNOLOGY,0,NULL,tech)>0 && !lstrcmp(tech,"PostScript"));
    CHECK(Escape(p,NEWFRAME,0,NULL,NULL)==SP_ERROR); /* no document yet */
    CHECK(Escape(p,SETABORTPROC,0,(LPCSTR)abort_proc,NULL)>0);
    CHECK(Escape(p,STARTDOC,7,"GDITEST",NULL)>0);
    SelectObject(p,red); Rectangle(p,150,150,450,300); TextOut(p,150,400,"Printed",7);
    CHECK(GetPixel(p,300,200)==RGB(255,0,0));
    CHECK(Escape(p,NEWFRAME,0,NULL,NULL)>0 && GetPixel(p,300,200)==RGB(255,255,255));
    CHECK(StartPage(p)>0);
    SelectObject(p,blue); PatBlt(p,0,0,1200,75,PATCOPY);
    CHECK(EndPage(p)>0);
    SelectObject(p,gray);
    while(Escape(p,NEXTBAND,0,NULL,&band)>0 && !IsRectEmpty(&band)) {
        CHECK(band.right==1200 && band.bottom==1575);
        PatBlt(p,100,100,200,100,PATCOPY); bands++;
        for(n=0;n<300;n++) SetPixel(p,100+n,400,RGB(n&255,0,n>>8?255:0));
    }
    CHECK(bands==1);
    TextOut(p,150,150,"Last page",9);
    CHECK(EndDoc(p)>0 && aborts_called>=4 && EndDoc(p)==SP_ERROR);
    /* An aborted document's file goes. */
    memset(&doc,0,sizeof(doc)); doc.cbSize=sizeof(doc); doc.lpszDocName="Aborted"; doc.lpszOutput="C:\\WINDOWS\\GDIABORT.PS";
    CHECK(StartDoc(p,&doc)>0 && OpenFile("C:\\WINDOWS\\GDIABORT.PS",&of,OF_EXIST)!=HFILE_ERROR);
    CHECK(AbortDoc(p)>0 && OpenFile("C:\\WINDOWS\\GDIABORT.PS",&of,OF_EXIST)==HFILE_ERROR);
    SelectObject(p,GetStockObject(WHITE_BRUSH));
    CHECK(DeleteDC(p));
    DeleteObject(red); DeleteObject(blue); DeleteObject(gray);
    /* Landscape A4 from a DEVMODE; an information context prints nothing. */
    memset(&dm,0,sizeof(dm)); dm.dmSize=sizeof(dm);
    dm.dmFields=DM_PAPERSIZE|DM_ORIENTATION; dm.dmPaperSize=DMPAPER_A4; dm.dmOrientation=DMORIENT_LANDSCAPE;
    ic=CreateIC("PSCRIPT","PostScript Printer","FILE:",&dm);
    CHECK(ic && GetDeviceCaps(ic,HORZRES)==1679 && GetDeviceCaps(ic,VERTRES)==1164 && Escape(ic,STARTDOC,4,"none",NULL)==SP_ERROR);
    DeleteDC(ic);
}

LRESULT FAR PASCAL WndProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    PAINTSTRUCT ps;
    switch(message) {
    case WM_PAINT: paint(BeginPaint(hwnd,&ps)); EndPaint(hwnd,&ps); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd,message,wParam,lParam);
}

int PASCAL WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show) {
    WNDCLASS wc; HWND hwnd; MSG msg; char text[64];
    (void)previous; (void)command;
    tests();
    more_tests();
    metafile_tests();
    print_tests();
    wsprintf(text,failed?"GDITEST: %d failed, %d passed":"GDITEST: %d checks passed",failed?failed:passed,passed);
    OutputDebugString(text);
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=WndProc; wc.hInstance=instance; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hbrBackground=GetStockObject(WHITE_BRUSH); wc.lpszClassName="GdiTest";
    RegisterClass(&wc);
    hwnd=CreateWindow("GdiTest","GDI test",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,instance,NULL);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    while(GetMessage(&msg,NULL,0,0)) {TranslateMessage(&msg); DispatchMessage(&msg);}
    return (int)msg.wParam;
}
