/* SPDX-License-Identifier: GPL-2.0-or-later
 * WOW.DLL: COMMDLG for Win16 programs, through the native COMMDLG.DLL. The
 * structures have 16-bit handles and far pointers; their strings and
 * buffers stay in 16-bit memory, which native code reaches directly.
 * Hooks and custom templates are left out (their flags are cleared). A
 * Find or Replace dialog keeps a native FINDREPLACE for the program's
 * own: FINDMSGSTRING reaches the owner with the program's structure,
 * brought up to date.
 */
#include "api.h"
#include <commdlg.h>

#define STR(p) ((LPSTR)Lin16(get32(p)))

/* OPENFILENAME: size, owner, instance, filter, custom filter and its
 * size, filter index, file and its size, file title and its size, initial
 * directory, title, flags, file offset and extension, default extension,
 * custom data, hook, template. */
#define OFN_UNSUPPORTED (OFN_ENABLEHOOK|OFN_ENABLETEMPLATE|OFN_ENABLETEMPLATEHANDLE|OFN_ALLOWMULTISELECT|OFN_SHOWHELP)
static DWORD file_dialog(Args16 *a,BOOL save) {
    BYTE *p=(BYTE *)PTR(a->a[0]); OPENFILENAME o; BOOL r; DWORD flags;
    if(!p) return 0;
    memset(&o,0,sizeof(o));
    o.lStructSize=sizeof(o); o.hwndOwner=HWND32(get16(p+4)); o.hInstance=Instance32(get16(p+6));
    o.lpstrFilter=STR(p+8); o.lpstrCustomFilter=STR(p+12); o.nMaxCustFilter=get32(p+16); o.nFilterIndex=get32(p+20);
    o.lpstrFile=STR(p+24); o.nMaxFile=get32(p+28); o.lpstrFileTitle=STR(p+32); o.nMaxFileTitle=get32(p+36);
    o.lpstrInitialDir=STR(p+40); o.lpstrTitle=STR(p+44); flags=get32(p+48); o.Flags=flags&~OFN_UNSUPPORTED;
    o.lpstrDefExt=STR(p+56); o.lCustData=(LPARAM)get32(p+60);
    r=save?GetSaveFileName(&o):GetOpenFileName(&o);
    put32(p+20,o.nFilterIndex); put32(p+48,(o.Flags&~OFN_UNSUPPORTED)|(flags&OFN_UNSUPPORTED));
    put16(p+52,o.nFileOffset); put16(p+54,o.nFileExtension);
    return (DWORD)r;
}
DWORD W16_GetOpenFileName(Args16 *a) {return file_dialog(a,FALSE);}
DWORD W16_GetSaveFileName(Args16 *a) {return file_dialog(a,TRUE);}
DWORD W16_GetFileTitle(Args16 *a) {return (WORD)GetFileTitle((LPCSTR)PTR(a->a[0]),(LPSTR)PTR(a->a[1]),(WORD)a->a[2]);}
DWORD W16_CommDlgExtendedError(Args16 *a) {(void)a; return CommDlgExtendedError();}

/* CHOOSECOLOR: size, owner, instance, result, custom colors (far), flags,
 * custom data, hook, template. */
DWORD W16_ChooseColor(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[0]); CHOOSECOLOR c; COLORREF custom[16]; BYTE *in; int i; BOOL r;
    if(!p) return 0;
    memset(&c,0,sizeof(c));
    c.lStructSize=sizeof(c); c.hwndOwner=HWND32(get16(p+4)); c.rgbResult=get32(p+8);
    in=(BYTE *)Lin16(get32(p+12));
    for(i=0;i<16;i++) custom[i]=in?get32(in+i*4):RGB(255,255,255);
    c.lpCustColors=custom; c.Flags=get32(p+16)&~(CC_ENABLEHOOK|CC_ENABLETEMPLATE|CC_ENABLETEMPLATEHANDLE|CC_SHOWHELP);
    r=ChooseColor(&c);
    if(r) {put32(p+8,c.rgbResult); if(in) for(i=0;i<16;i++) put32(in+i*4,custom[i]);}
    return (DWORD)r;
}

/* CHOOSEFONT: size, owner, DC, LOGFONT (far), point size, flags, colors,
 * custom data, hook, template, instance, style (far), font type, minimum
 * and maximum size. */
DWORD W16_ChooseFont(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[0]),*lf16; CHOOSEFONT c; LOGFONT lf; char style[64]; BOOL r;
    if(!p || !(lf16=(BYTE *)Lin16(get32(p+8)))) return 0;
    memset(&c,0,sizeof(c));
    LogFontIn16(lf16,&lf);
    c.lStructSize=sizeof(c); c.hwndOwner=HWND32(get16(p+4)); c.hDC=(HDC)HGDI32(get16(p+6)); c.lpLogFont=&lf;
    c.Flags=get32(p+14)&~(CF_ENABLEHOOK|CF_ENABLETEMPLATE|CF_ENABLETEMPLATEHANDLE|CF_SHOWHELP|CF_APPLY);
    c.rgbColors=get32(p+18); c.nSizeMin=(short)get16(p+42); c.nSizeMax=(short)get16(p+44);
    style[0]=0; c.lpszStyle=style;
    r=ChooseFont(&c);
    if(r) {
        LogFontOut16(lf16,&lf);
        put16(p+12,(WORD)c.iPointSize); put32(p+18,c.rgbColors); put16(p+40,c.nFontType);
        if((c.Flags&CF_USESTYLE) && Lin16(get32(p+36))) lstrcpy((LPSTR)Lin16(get32(p+36)),style);
    }
    return (DWORD)r;
}

/* PRINTDLG: size, owner, DEVMODE and DEVNAMES (global memory, the same
 * bytes in both worlds, copied each way), DC, flags, from, to, minimum and
 * maximum page, copies, instance, custom data, hooks, templates. */
#define PD_UNSUPPORTED (PD_ENABLEPRINTHOOK|PD_ENABLESETUPHOOK|PD_ENABLEPRINTTEMPLATE|PD_ENABLESETUPTEMPLATE| \
                        PD_ENABLEPRINTTEMPLATEHANDLE|PD_ENABLESETUPTEMPLATEHANDLE|PD_SHOWHELP)
static HGLOBAL native_block(WORD sel) {
    DWORD size=sel?GlobalSize16(sel):0; const BYTE *p=(const BYTE *)Lin16(MAKELONG(0,sel)); HGLOBAL h; BYTE *d;
    if(!p || !size || !(h=GlobalAlloc(GMEM_MOVEABLE,size))) return NULL;
    if((d=(BYTE *)GlobalLock(h))!=NULL) {memcpy(d,p,size); GlobalUnlock(h);}
    return h;
}
/* The result in the program's block if it is large enough, else in a new one. */
static WORD block16(Task16 *t,HGLOBAL h,WORD sel) {
    SIZE_T size=h?GlobalSize(h):0; const BYTE *p; BYTE *d;
    if(!size || size>0xffff) return sel;
    if(!sel || GlobalSize16(sel)<size) {
        if(sel) GlobalFree16(sel);
        if(!(sel=GlobalAlloc16(t,GMEM_MOVEABLE|GMEM_ZEROINIT,(DWORD)size))) return 0;
    }
    if((p=(const BYTE *)GlobalLock(h))!=NULL) {
        if((d=(BYTE *)Lin16(MAKELONG(0,sel)))!=NULL) memcpy(d,p,size);
        GlobalUnlock(h);
    }
    return sel;
}
DWORD W16_PrintDlg(Args16 *a) {
    BYTE *p=(BYTE *)PTR(a->a[0]); PRINTDLG d; WORD mode16,names16; DWORD flags; BOOL r;
    if(!p) return 0;
    memset(&d,0,sizeof(d)); d.lStructSize=sizeof(d);
    d.hwndOwner=HWND32(get16(p+4)); mode16=get16(p+6); names16=get16(p+8);
    d.hDevMode=native_block(mode16); d.hDevNames=native_block(names16);
    flags=get32(p+12); d.Flags=flags&~PD_UNSUPPORTED;
    d.nFromPage=get16(p+16); d.nToPage=get16(p+18); d.nMinPage=get16(p+20); d.nMaxPage=get16(p+22); d.nCopies=get16(p+24);
    d.hInstance=Instance32(get16(p+26)); d.lCustData=(LPARAM)get32(p+28);
    r=PrintDlg(&d);
    if(r) {
        put16(p+6,block16(a->task,d.hDevMode,mode16)); put16(p+8,block16(a->task,d.hDevNames,names16));
        put16(p+10,(WORD)HGDI16(d.hDC)); put32(p+12,(d.Flags&~PD_UNSUPPORTED)|(flags&PD_UNSUPPORTED));
        put16(p+16,d.nFromPage); put16(p+18,d.nToPage); put16(p+24,d.nCopies);
    }
    if(d.hDevMode) GlobalFree(d.hDevMode);
    if(d.hDevNames) GlobalFree(d.hDevNames);
    return (DWORD)r;
}

/* FINDREPLACE: size, owner, instance, flags, find and replace buffers
 * (far), their sizes, custom data, hook, template. */
#define FR_UNSUPPORTED (FR_ENABLEHOOK|FR_ENABLETEMPLATE|FR_ENABLETEMPLATEHANDLE|FR_SHOWHELP)
#define FINDS 8
static struct {FINDREPLACE fr; DWORD segptr; HWND dialog;} finds[FINDS];
static DWORD find_dialog(Args16 *a,BOOL replace) {
    BYTE *p=(BYTE *)PTR(a->a[0]); unsigned i; FINDREPLACE *fr; HWND h;
    if(!p) return 0;
    for(i=0;i<FINDS;i++) if(finds[i].segptr==a->raw[0] || !finds[i].segptr) break;
    if(i==FINDS) for(i=0;i<FINDS && IsWindow(finds[i].dialog);i++) {}
    if(i==FINDS) return 0;
    fr=&finds[i].fr; memset(fr,0,sizeof(*fr));
    fr->lStructSize=sizeof(*fr); fr->hwndOwner=HWND32(get16(p+4)); fr->hInstance=Instance32(get16(p+6));
    fr->Flags=get32(p+8)&~FR_UNSUPPORTED;
    fr->lpstrFindWhat=STR(p+12); fr->lpstrReplaceWith=STR(p+16); fr->wFindWhatLen=get16(p+20); fr->wReplaceWithLen=get16(p+22);
    fr->lCustData=(LPARAM)get32(p+24);
    finds[i].segptr=a->raw[0];
    h=replace?ReplaceText(fr):FindText(fr);
    finds[i].dialog=h;
    if(!h) finds[i].segptr=0;
    return HWND16(h);
}
DWORD W16_FindText(Args16 *a) {return find_dialog(a,FALSE);}
DWORD W16_ReplaceText(Args16 *a) {return find_dialog(a,TRUE);}
/* FINDMSGSTRING's lParam: the program's structure with the flags the
 * dialog set; 0 when lp is not one of these. */
DWORD FindReplace16(LPARAM lp) {
    unsigned i; BYTE *p;
    for(i=0;i<FINDS;i++) if(finds[i].segptr && (LPARAM)&finds[i].fr==lp) {
        if((p=(BYTE *)Lin16(finds[i].segptr))!=NULL) put32(p+8,finds[i].fr.Flags|(get32(p+8)&FR_UNSUPPORTED));
        return finds[i].segptr;
    }
    return 0;
}
