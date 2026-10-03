/* SPDX-License-Identifier: GPL-2.0-or-later
 * PIF Editor. The 0x171-byte base holds title, conventional memory,
 * command, startup directory, parameters and flags (also at 0x16F/0x170).
 * Byte 1 is the sum of bytes 2..0x170. MICROSOFT PIFEX at 0x171 introduces
 * extensions with name, next-header offset (0xFFFF=end), data offset and
 * size; offsets are file-relative. WINDOWS 286 3.0 holds standard-mode
 * XMS/reserved keys; WINDOWS 386 3.0 holds memory/priorities/flags/parameters.
 *
 * Edit only selected fields/bits, preserve unknown extensions and recompute
 * the checksum on save. New files get both extensions; missing extensions
 * are appended when needed.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "pifedit.h"
#define BASE 0x171
#define HEADER 0x16
#define DATA286 6
#define DATA386 0x68

static HINSTANCE instance;
static HWND form; /* the main window: a dialog of the mode's settings */
static UINT mode=IDM_STANDARD;
static BYTE *pif; static DWORD pif_size,at286,at386; /* the file; its extensions' data, or 0 */
static char file[MAX_PATH];
static BOOL modified,form_changed,filling,switching;
static const char sig_ex[16]="MICROSOFT PIFEX",sig286[16]="WINDOWS 286 3.0",sig386[16]="WINDOWS 386 3.0";

static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
static void put16(BYTE *p,WORD v) {p[0]=(BYTE)v; p[1]=(BYTE)(v>>8);}
static void put32(BYTE *p,DWORD v) {put16(p,LOWORD(v)); put16(p+2,HIWORD(v));}
/* One flag of a byte, word or doubleword; the other bits stay. */
static void flag8(BYTE *p,BYTE mask,BOOL on) {*p=(BYTE)(on?*p|mask:*p&~mask);}
static void flag16(BYTE *p,WORD mask,BOOL on) {put16(p,(WORD)(on?get16(p)|mask:get16(p)&~mask));}
static void flag32(BYTE *p,DWORD mask,BOOL on) {put32(p,on?get32(p)|mask:get32(p)&~mask);}

/* --- the file ----------------------------------------------------------------------------- */
/* Each header of the chain in turn, from MICROSOFT PIFEX: TRUE while there is
 * one (whole in the file; a chain that comes back on itself ends). */
static BOOL next_header(DWORD *h,int *hops) {
    if(*hops==0) *h=BASE; else *h=get16(pif+*h+16);
    return *h!=0xffff && *h+HEADER<=pif_size && (*hops)++<64 && (*hops>1 || !memcmp(pif+BASE,sig_ex,16));
}
/* The data of the first WINDOWS 286 3.0 and WINDOWS 386 3.0 with room enough. */
static void find_extensions(void) {
    DWORD h; int hops=0;
    at286=at386=0;
    if(pif_size<BASE+HEADER) return;
    while(next_header(&h,&hops)) {
        const BYTE *p=pif+h; DWORD off=get16(p+18),len=get16(p+20);
        if(off+len>pif_size) continue;
        if(!memcmp(p,sig286,16) && len>=DATA286 && !at286) at286=off;
        if(!memcmp(p,sig386,16) && len>=DATA386 && !at386) at386=off;
    }
}
static void put_header(BYTE *p,const char *sig,DWORD next,DWORD off,DWORD len) {
    memcpy(p,sig,16); put16(p+16,(WORD)next); put16(p+18,(WORD)off); put16(p+20,(WORD)len);
}
/* 386 enhanced mode's settings in a new extension: 640 KB desired and 128
 * required, priorities 100 and 50, 1024 KB of EMS and XMS allowed, full
 * screen, idle time detected, fast paste; text video memory, emulated, with
 * high graphics monitored. */
static void default386(BYTE *q) {
    memset(q,0,DATA386);
    put16(q,640); put16(q+2,128); put16(q+4,100); put16(q+6,50); put16(q+8,1024); put16(q+12,1024);
    put32(q+0x10,0x00021008); put32(q+0x14,0x17);
}
static void replace(BYTE *data,DWORD size) {
    if(pif) GlobalFree(pif);
    pif=data; pif_size=size; find_extensions();
}
/* A new file: the basic part (the title in spaces, 640 KB and 128 KB, closing
 * on exit, text mode, 25 x 80) and both extensions, one after the other. */
static BOOL new_pif(void) {
    DWORD size=BASE+3*HEADER+DATA286+DATA386; BYTE *p=(BYTE *)GlobalAlloc(GPTR,size);
    if(!p) return FALSE;
    memset(p+2,' ',30); put16(p+0x20,640); put16(p+0x22,128); p[0x63]=0x10;
    p[0xe5]=0x7f; p[0xe6]=1; p[0xe8]=0xff; p[0xe9]=25; p[0xea]=80; put16(p+0xed,7);
    put_header(p+BASE,sig_ex,BASE+HEADER,0,BASE);
    put_header(p+BASE+HEADER,sig286,BASE+2*HEADER+DATA286,BASE+2*HEADER,DATA286);
    put_header(p+BASE+2*HEADER+DATA286,sig386,0xffff,BASE+3*HEADER+DATA286,DATA386);
    default386(p+BASE+3*HEADER+DATA286);
    replace(p,size);
    return TRUE;
}
/* An extension the file lacks, at its end, linked from the last header (a
 * basic part alone gets the MICROSOFT PIFEX header first): its data, or 0
 * when the file cannot take one (other data after the basic part, or no room
 * for 16-bit offsets). */
static DWORD add_extension(const char *sig,DWORD len) {
    DWORD h,last=0,at,size; int hops=0; BOOL alone=pif_size==BASE; BYTE *n;
    if(!alone) {
        if(pif_size<BASE+HEADER || memcmp(pif+BASE,sig_ex,16)) return 0;
        while(next_header(&h,&hops)) last=h;
        if(!last || get16(pif+last+16)!=0xffff) return 0;
    }
    at=pif_size+(alone?HEADER:0); size=at+HEADER+len;
    if(size>0xffff || !(n=(BYTE *)GlobalAlloc(GPTR,size))) return 0;
    memcpy(n,pif,pif_size);
    if(alone) {put_header(n+BASE,sig_ex,0xffff,0,BASE); last=BASE;}
    put16(n+last+16,(WORD)at);
    put_header(n+at,sig,0xffff,at+HEADER,len);
    if(sig==sig386) default386(n+at+HEADER);
    replace(n,size);
    return at+HEADER;
}
static BYTE checksum(void) {DWORD i,sum=0; for(i=2;i<BASE;i++) sum+=pif[i]; return (BYTE)sum;}

/* --- the form ------------------------------------------------------------------------------- */
/* A string field: up to its NUL, trailing spaces off. */
static void get_string(const BYTE *p,int n,char *out) {
    int k=0;
    while(k<n && p[k]) {out[k]=(char)p[k]; k++;}
    while(k && out[k-1]==' ') k--;
    out[k]=0;
}
static void show_string(HWND dlg,int id,const BYTE *p,int n) {
    char text[80]; get_string(p,n,text);
    SetDlgItemText(dlg,id,text); SendDlgItemMessage(dlg,id,EM_LIMITTEXT,(WPARAM)n,0);
}
/* Written back only when it changed: filled with spaces (the title) or NULs. */
static void keep_string(HWND dlg,int id,BYTE *p,int n,BOOL spaces) {
    char text[80],old[80]; int k;
    GetDlgItemText(dlg,id,text,sizeof(text)); get_string(p,n,old);
    if(!lstrcmp(text,old)) return;
    memset(p,spaces?' ':0,(size_t)n);
    for(k=0;text[k] && k<n;k++) p[k]=(BYTE)text[k];
}
/* Numbers are signed words (-1 is "all"); an unchanged one is not checked. */
static void number_text(WORD v,char *out) {wsprintf(out,"%d",v==0xffff?-1:(int)v);}
static void show_number(HWND dlg,int id,WORD v) {char text[16]; number_text(v,text); SetDlgItemText(dlg,id,text);}
static BOOL read_number(HWND dlg,int id,int lo,int hi,WORD old,WORD *out) {
    char text[16],was[16]; int i=0,v=0,digits=0; BOOL minus=FALSE;
    GetDlgItemText(dlg,id,text,sizeof(text)); number_text(old,was);
    if(!lstrcmp(text,was)) {*out=old; return TRUE;}
    while(text[i]==' ') i++;
    if(text[i]=='-') {minus=TRUE; i++;}
    for(;text[i]>='0' && text[i]<='9' && v<100000;i++,digits++) v=v*10+text[i]-'0';
    while(text[i]==' ') i++;
    if(minus) v=-v;
    if(!digits || text[i] || v<lo || v>hi) {
        char message[80];
        wsprintf(message,"Enter a number from %d to %d.",lo,hi);
        MessageBox(form,message,"PIF Editor",MB_OK|MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(dlg,id)); SendDlgItemMessage(dlg,id,EM_SETSEL,0,-1);
        return FALSE;
    }
    *out=(WORD)v;
    return TRUE;
}
static void check(HWND dlg,int id,BOOL on) {CheckDlgButton(dlg,id,on?1:0);}
static BOOL checked(HWND dlg,int id) {return IsDlgButtonChecked(dlg,id)!=0;}

/* The keys a mode may reserve for the program, and where each mode keeps them. */
static const struct {int id; WORD bit286; DWORD bit386;} keys[]={
    {IDC_ALTTAB,0x0001,0x0020},{IDC_ALTESC,0x0002,0x0040},{IDC_CTRLESC,0x0010,0x0800},
    {IDC_PRTSC,0x0008,0x0400},{IDC_ALTPRTSC,0x0004,0x0200},{IDC_ALTSPACE,0,0x0080},{IDC_ALTENTER,0,0x0100}};
#define KEYS (int)(sizeof(keys)/sizeof(keys[0]))

/* Standard mode: the basic part, XMS and the keys in WINDOWS 286 3.0. */
static void fill_standard(HWND dlg) {
    int i; BYTE flags=pif[0x63];
    show_string(dlg,IDC_PARAMS,pif+0xa5,64);
    CheckRadioButton(dlg,IDC_TEXT,IDC_GRAPHICS,flags&0x02?IDC_GRAPHICS:IDC_TEXT);
    show_number(dlg,IDC_MEMREQ,get16(pif+0x22));
    show_number(dlg,IDC_XMSREQ,at286?get16(pif+at286+2):0); show_number(dlg,IDC_XMSLIM,at286?get16(pif+at286):0);
    check(dlg,IDC_COM1,flags&0x80); check(dlg,IDC_COM2,flags&0x40); check(dlg,IDC_KEYBOARD,pif[0x16f]&0x10);
    check(dlg,IDC_NOSCREEN,flags&0x08); check(dlg,IDC_NOSWITCH,flags&0x04);
    for(i=0;i<KEYS;i++) if(keys[i].bit286) check(dlg,keys[i].id,at286 && (get16(pif+at286+4)&keys[i].bit286));
}
static BOOL apply_standard(HWND dlg) {
    WORD mem,xreq,xlim,keymask=0; int i; BOOL graphics=checked(dlg,IDC_GRAPHICS);
    if(!read_number(dlg,IDC_MEMREQ,-1,640,get16(pif+0x22),&mem) ||
       !read_number(dlg,IDC_XMSREQ,0,16384,at286?get16(pif+at286+2):0,&xreq) ||
       !read_number(dlg,IDC_XMSLIM,-1,16384,at286?get16(pif+at286):0,&xlim)) return FALSE;
    for(i=0;i<KEYS;i++) if(keys[i].bit286 && checked(dlg,keys[i].id)) keymask|=keys[i].bit286;
    if(!at286 && (xreq || xlim || keymask) && !add_extension(sig286,DATA286)) {
        MessageBox(form,"The file has no room for standard mode's XMS memory and shortcut keys.","PIF Editor",MB_OK|MB_ICONEXCLAMATION);
        return FALSE;
    }
    keep_string(dlg,IDC_PARAMS,pif+0xa5,64,FALSE);
    /* Text or graphics: the flag, and the old screen field as the PIF Editor sets it. */
    if(graphics!=((pif[0x63]&0x02)!=0)) {
        WORD sysmem=get16(pif+0xed);
        flag8(pif+0x63,0x02,graphics);
        if(graphics && sysmem<16) put16(pif+0xed,23);
        if(!graphics && sysmem>=16) put16(pif+0xed,7);
    }
    put16(pif+0x22,mem);
    if(at286) {
        put16(pif+at286,xlim); put16(pif+at286+2,xreq);
        for(i=0;i<KEYS;i++) if(keys[i].bit286) flag16(pif+at286+4,keys[i].bit286,checked(dlg,keys[i].id));
    }
    flag8(pif+0x63,0x80,checked(dlg,IDC_COM1)); flag8(pif+0x63,0x40,checked(dlg,IDC_COM2));
    flag8(pif+0x16f,0x10,checked(dlg,IDC_KEYBOARD));
    flag8(pif+0x63,0x08,checked(dlg,IDC_NOSCREEN)); flag8(pif+0x63,0x04,checked(dlg,IDC_NOSWITCH));
    return TRUE;
}
/* 386 enhanced mode: WINDOWS 386 3.0 (its defaults shown when the file has none). */
static BYTE *data386(BYTE *defaults) {if(at386) return pif+at386; default386(defaults); return defaults;}
static void fill_enhanced(HWND dlg) {
    BYTE defaults[DATA386],*q=data386(defaults); DWORD flags=get32(q+0x10),video=get32(q+0x14);
    show_string(dlg,IDC_PARAMS,q+0x28,64);
    CheckRadioButton(dlg,IDC_VTEXT,IDC_VHIGH,video&0x40?IDC_VHIGH:video&0x20?IDC_VLOW:IDC_VTEXT);
    show_number(dlg,IDC_MEMREQ,get16(q+2)); show_number(dlg,IDC_MEMDES,get16(q));
    show_number(dlg,IDC_EMSREQ,get16(q+10)); show_number(dlg,IDC_EMSLIM,get16(q+8));
    show_number(dlg,IDC_XMSREQ,get16(q+14)); show_number(dlg,IDC_XMSLIM,get16(q+12));
    CheckRadioButton(dlg,IDC_FULLSCREEN,IDC_WINDOWED,flags&0x08?IDC_FULLSCREEN:IDC_WINDOWED);
    check(dlg,IDC_BACKGROUND,flags&0x02); check(dlg,IDC_EXCLUSIVE,flags&0x04);
}
static BOOL ensure386(void) {
    if(at386 || add_extension(sig386,DATA386)) return TRUE;
    MessageBox(form,"The file has no room for 386 enhanced mode's settings.","PIF Editor",MB_OK|MB_ICONEXCLAMATION);
    return FALSE;
}
static BOOL apply_enhanced(HWND dlg) {
    BYTE defaults[DATA386],*q=data386(defaults); WORD v[6]; BYTE *p; DWORD video;
    if(!read_number(dlg,IDC_MEMREQ,-1,640,get16(q+2),&v[0]) || !read_number(dlg,IDC_MEMDES,-1,640,get16(q),&v[1]) ||
       !read_number(dlg,IDC_EMSREQ,0,16384,get16(q+10),&v[2]) || !read_number(dlg,IDC_EMSLIM,-1,16384,get16(q+8),&v[3]) ||
       !read_number(dlg,IDC_XMSREQ,0,16384,get16(q+14),&v[4]) || !read_number(dlg,IDC_XMSLIM,-1,16384,get16(q+12),&v[5]) ||
       !ensure386()) return FALSE;
    p=pif+at386;
    keep_string(dlg,IDC_PARAMS,p+0x28,64,FALSE);
    put16(p+2,v[0]); put16(p,v[1]); put16(p+10,v[2]); put16(p+8,v[3]); put16(p+14,v[4]); put16(p+12,v[5]);
    /* One of the three video memory modes. */
    video=get32(p+0x14)&~0x70UL;
    put32(p+0x14,video|(checked(dlg,IDC_VHIGH)?0x40:checked(dlg,IDC_VLOW)?0x20:0x10));
    flag32(p+0x10,0x08,checked(dlg,IDC_FULLSCREEN));
    flag32(p+0x10,0x02,checked(dlg,IDC_BACKGROUND)); flag32(p+0x10,0x04,checked(dlg,IDC_EXCLUSIVE));
    return TRUE;
}
/* The settings both modes share: the program, title, start-up directory and closing on exit. */
static void fill(HWND dlg) {
    filling=TRUE;
    show_string(dlg,IDC_PROGRAM,pif+0x24,63); show_string(dlg,IDC_TITLE,pif+2,30); show_string(dlg,IDC_DIR,pif+0x65,64);
    check(dlg,IDC_CLOSE,pif[0x63]&0x10);
    if(mode==IDM_STANDARD) fill_standard(dlg); else fill_enhanced(dlg);
    filling=FALSE; form_changed=FALSE;
}
static BOOL apply(void) {
    if(!form_changed) return TRUE;
    if(!(mode==IDM_STANDARD?apply_standard(form):apply_enhanced(form))) return FALSE;
    keep_string(form,IDC_PROGRAM,pif+0x24,63,FALSE); keep_string(form,IDC_TITLE,pif+2,30,TRUE);
    keep_string(form,IDC_DIR,pif+0x65,64,FALSE);
    flag8(pif+0x63,0x10,checked(form,IDC_CLOSE));
    form_changed=FALSE;
    return TRUE;
}

/* --- Advanced: the rest of 386 enhanced mode's settings ------------------------------------- */
/* Flags of the doubleword at 0x10, and of the video one at 0x14; inverted ones are "not". */
static const struct {int id; BYTE where; DWORD bit; BOOL inverted;} options[]={
    {IDC_IDLE,0x10,0x1000,FALSE},{IDC_EMSLOCK,0x10,0x8000,FALSE},{IDC_XMSLOCK,0x10,0x10000,FALSE},
    {IDC_HMA,0x10,0x2000,TRUE},{IDC_LOCKAPP,0x10,0x40000,FALSE},{IDC_FASTPASTE,0x10,0x20000,FALSE},
    {IDC_ALLOWCLOSE,0x10,0x0001,FALSE},{IDC_MONTEXT,0x14,0x02,TRUE},{IDC_MONLOW,0x14,0x04,TRUE},
    {IDC_MONHIGH,0x14,0x08,TRUE},{IDC_EMULATE,0x14,0x01,FALSE},{IDC_RETAIN,0x14,0x80,FALSE}};
#define OPTIONS (int)(sizeof(options)/sizeof(options[0]))
static INT_PTR CALLBACK AdvancedProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    BYTE defaults[DATA386],*q=data386(defaults); int i;
    (void)lp;
    if(msg==WM_INITDIALOG) {
        show_number(dlg,IDC_BGPRI,get16(q+6)); show_number(dlg,IDC_FGPRI,get16(q+4));
        for(i=0;i<OPTIONS;i++) check(dlg,options[i].id,((get32(q+options[i].where)&options[i].bit)!=0)!=options[i].inverted);
        for(i=0;i<KEYS;i++) check(dlg,keys[i].id,get32(q+0x10)&keys[i].bit386);
        return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        WORD bg,fg;
        if(!read_number(dlg,IDC_BGPRI,0,10000,get16(q+6),&bg) || !read_number(dlg,IDC_FGPRI,0,10000,get16(q+4),&fg) || !ensure386()) return TRUE;
        q=pif+at386; put16(q+6,bg); put16(q+4,fg);
        for(i=0;i<OPTIONS;i++) flag32(q+options[i].where,options[i].bit,checked(dlg,options[i].id)!=options[i].inverted);
        for(i=0;i<KEYS;i++) flag32(q+0x10,keys[i].bit386,checked(dlg,keys[i].id));
        modified=TRUE;
        EndDialog(dlg,IDOK); return TRUE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {EndDialog(dlg,IDCANCEL); return TRUE;}
    return FALSE;
}

static INT_PTR CALLBACK AboutProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) return TRUE;
    if(msg==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)) {EndDialog(dlg,0); return TRUE;}
    return FALSE;
}

/* --- files ------------------------------------------------------------------------------------ */
static void set_title(void) {
    char title[300];
    wsprintf(title,"PIF Editor - %s",file[0]?FileTitle(file):"(Untitled)");
    SetWindowText(form,title);
}
static BOOL load(LPCSTR path) {
    DWORD size; BYTE *data=(BYTE *)ReadWholeFile(path,&size);
    if(!data) return FALSE;
    if(size<BASE || size>0xffff) {GlobalFree(data); return FALSE;}
    replace(data,size);
    lstrcpyn(file,path,sizeof(file)); AnsiUpper(file); modified=FALSE;
    return TRUE;
}
static BOOL save_to(LPCSTR path) {
    char program[64]; HFILE f; BOOL ok;
    if(!apply()) return FALSE;
    get_string(pif+0x24,63,program);
    if(!program[0]) {
        MessageBox(form,"Type the name of the program's file.","PIF Editor",MB_OK|MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(form,IDC_PROGRAM)); return FALSE;
    }
    pif[1]=checksum();
    if((f=_lcreat(path,0))==HFILE_ERROR) {MessageBox(form,"Cannot write the file.","PIF Editor",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    ok=_lwrite(f,pif,(UINT)pif_size)==(UINT)pif_size;
    _lclose(f);
    if(!ok) {MessageBox(form,"Cannot write the file.","PIF Editor",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
    lstrcpyn(file,path,sizeof(file)); AnsiUpper(file); modified=FALSE; set_title();
    return TRUE;
}
static BOOL save_as(void) {
    char path[MAX_PATH];
    if(!apply()) return FALSE;
    lstrcpy(path,file[0]?file:"*.PIF");
    return FileSaveDialog(form,"Save As","*.PIF",path,sizeof(path)) && save_to(path);
}
/* Changes kept, dropped or the action cancelled (FALSE). */
static BOOL query_save(void) {
    char text[300]; int r;
    if(!modified && !form_changed) return TRUE;
    wsprintf(text,"Save changes to %s?",file[0]?FileTitle(file):"(Untitled)");
    r=MessageBox(form,text,"PIF Editor",MB_YESNOCANCEL|MB_ICONEXCLAMATION);
    if(r==IDCANCEL) return FALSE;
    return r==IDNO || (file[0]?save_to(file):save_as());
}

/* --- the window -------------------------------------------------------------------------------- */
static INT_PTR CALLBACK FormProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp);
/* The form of a mode, in the place of the last one. */
static BOOL show_form(UINT new_mode,int x,int y) {
    HWND old=form,dlg;
    mode=new_mode;
    dlg=CreateDialog(instance,mode==IDM_STANDARD?"STANDARD":"ENHANCED",NULL,FormProc);
    if(!dlg) return FALSE;
    form=dlg;
    if(old) {switching=TRUE; DestroyWindow(old); switching=FALSE;}
    if(x!=CW_USEDEFAULT) SetWindowPos(dlg,NULL,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER);
    fill(dlg); set_title();
    ShowWindow(dlg,SW_SHOW);
    return TRUE;
}
static void command(UINT id) {
    char path[MAX_PATH]; RECT r;
    switch(id) {
    case IDM_NEW:
        if(query_save() && new_pif()) {file[0]=0; modified=FALSE; fill(form); set_title();}
        return;
    case IDM_OPEN:
        if(!query_save()) return;
        lstrcpy(path,"*.PIF");
        if(FileOpenDialog(form,"Open","*.PIF",path,sizeof(path))) {
            if(load(path)) {fill(form); set_title();}
            else MessageBox(form,"This file is not a program information file.","PIF Editor",MB_OK|MB_ICONEXCLAMATION);
        }
        return;
    case IDM_SAVE: if(file[0]) save_to(file); else save_as(); return;
    case IDM_SAVEAS: save_as(); return;
    case IDM_EXIT: SendMessage(form,WM_CLOSE,0,0); return;
    case IDM_STANDARD: case IDM_ENHANCED:
        if(id==mode || !apply()) return;
        GetWindowRect(form,&r); show_form(id,r.left,r.top);
        return;
    case IDM_ABOUT: DialogBox(instance,"ABOUT",form,AboutProc); return;
    }
}
static INT_PTR CALLBACK FormProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_INITMENUPOPUP:
        CheckMenuItem((HMENU)wp,IDM_STANDARD,mode==IDM_STANDARD?MF_CHECKED:MF_UNCHECKED);
        CheckMenuItem((HMENU)wp,IDM_ENHANCED,mode==IDM_ENHANCED?MF_CHECKED:MF_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        if(LOWORD(wp)>=IDM_NEW && LOWORD(wp)<=IDM_ABOUT) {command(LOWORD(wp)); return TRUE;}
        if(HelpCommand(dlg,LOWORD(wp),"PIFEDIT.HLP")) return TRUE;
        if(LOWORD(wp)==IDC_ADVANCED) {DialogBox(instance,"ADVANCED",dlg,AdvancedProc); return TRUE;}
        if(!filling && (HIWORD(wp)==EN_CHANGE || HIWORD(wp)==BN_CLICKED) && LOWORD(wp)>=IDC_PROGRAM) {form_changed=modified=TRUE; return TRUE;}
        return TRUE;
    case WM_CLOSE: if(query_save()) DestroyWindow(dlg); return TRUE;
    case WM_DESTROY: if(!switching) {WinHelp(dlg,"PIFEDIT.HLP",HELP_QUIT,0); PostQuitMessage(0);} return TRUE;
    }
    return FALSE;
}
int PASCAL WinMain(HINSTANCE inst,HINSTANCE previous,LPSTR command_line,int show) {
    MSG m; HACCEL accel;
    (void)previous; (void)show;
    instance=inst;
    if(!new_pif()) return 1;
    while(*command_line==' ') command_line++;
    if(*command_line && !load(command_line)) {
        MessageBox(NULL,"This file is not a program information file.","PIF Editor",MB_OK|MB_ICONEXCLAMATION);
        new_pif();
    }
    if(!show_form(IDM_STANDARD,CW_USEDEFAULT,0)) return 1;
    accel=LoadAccelerators(inst,"PIFEDIT");
    while(GetMessage(&m,NULL,0,0)) if(!form || (!TranslateAccelerator(form,accel,&m) && !IsDialogMessage(form,&m))) {TranslateMessage(&m); DispatchMessage(&m);}
    return (int)m.wParam;
}
