/* SPDX-License-Identifier: GPL-2.0-or-later
 * COMMDLG.DLL: Windows 3.1's common dialogs. Their templates are built in
 * memory (DIALOG format, the system font) with the control identifiers of
 * Windows' dlgs.h; hooks and custom templates are not supported.
 *   Open / Save As   file name, files of the current type, directories,
 *                    file types (lpstrFilter), drives, Read Only
 *   Color            48 basic and 16 custom colors, red/green/blue values
 *   Font             the built-in faces, styles and sizes, effects, sample
 *   Find / Replace   modeless; FINDMSGSTRING notifies the owner
 *   Print            the printer ([windows] device= or the program's
 *                    DEVNAMES), range, copies, print to file, collate;
 *                    Print Setup: the default or a specific printer from
 *                    [devices], orientation and paper, kept by the driver
 *                    (ExtDeviceMode); DEVMODE, DEVNAMES and a DC or IC back
 */
#define COMMDLG_BUILD
#include <commdlg.h>
#include <string.h>

static HINSTANCE module;
static DWORD error_code;
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {(void)reason; (void)reserved; module=instance; return TRUE;}
DWORD WINAPI CommDlgExtendedError(void) {return error_code;}

/* dlgs.h */
#define chx1 0x0410
#define chx2 0x0411
#define psh1 0x0400
#define psh2 0x0401
#define rad1 0x0420
#define rad2 0x0421
#define grp1 0x0430
#define grp2 0x0431
#define stc1 0x0440
#define stc2 0x0441
#define stc3 0x0442
#define stc4 0x0443
#define stc5 0x0444
#define lst1 0x0460
#define lst2 0x0461
#define cmb1 0x0470
#define cmb2 0x0471
#define cmb3 0x0472
#define cmb4 0x0473
#define edt1 0x0480
#define edt2 0x0481
#define edt3 0x0482
#define BUTTON 0x80
#define EDIT 0x81
#define STATIC 0x82
#define LISTBOX 0x83
#define COMBOBOX 0x85

/* --- templates ------------------------------------------------------------- */
typedef struct {DWORD buffer[1024]; WORD *p; int count;} Template;
static WORD *wide(WORD *p,LPCSTR s) {while(*s) *p++=(BYTE)*s++; *p++=0; return p;}
static void begin(Template *t,LPCSTR title,int cx,int cy,DWORD style) {
    WORD *p;
    memset(t,0,sizeof(*t));
    p=(WORD *)t->buffer;
    *(DWORD *)p=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|style; p+=2;
    *(DWORD *)p=0; p+=2;
    *p++=0; *p++=20; *p++=20; *p++=(WORD)cx; *p++=(WORD)cy;
    *p++=0; *p++=0; p=wide(p,title);
    t->p=p;
}
static void item(Template *t,WORD cls,DWORD style,int x,int y,int cx,int cy,WORD id,LPCSTR text) {
    WORD *p=t->p;
    while(((BYTE *)p-(BYTE *)t->buffer)&3) *p++=0;
    *(DWORD *)p=style|WS_CHILD|WS_VISIBLE; p+=2;
    *(DWORD *)p=0; p+=2;
    *p++=(WORD)x; *p++=(WORD)y; *p++=(WORD)cx; *p++=(WORD)cy; *p++=id;
    *p++=0xffff; *p++=cls;
    p=wide(p,text);
    *p++=0;
    t->p=p; t->count++;
}
static LPCDLGTEMPLATE end(Template *t) {((DLGTEMPLATE *)t->buffer)->cdit=(WORD)t->count; return (LPCDLGTEMPLATE)t->buffer;}
#define LABEL(t,x,y,cx,cy,id,text) item(t,STATIC,SS_LEFT,x,y,cx,cy,id,text)
#define PUSH(t,x,y,id,text) item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,x,y,50,14,id,text)

static BOOL wildcard(LPCSTR s) {for(;*s;s++) if(*s=='*' || *s=='?') return TRUE; return FALSE;}
static LPCSTR base_name(LPCSTR path) {
    LPCSTR p,base=path;
    for(p=path;*p;p++) if(*p=='\\' || *p==':') base=p+1;
    return base;
}
/* A path's directory part, without its last backslash unless it is a root's. */
static void dir_of(LPCSTR path,char *dir) {
    int n=(int)(base_name(path)-path);
    lstrcpyn(dir,path,n+1);
    if(n>3 && dir[n-1]=='\\') dir[n-1]=0;
}
short WINAPI GetFileTitle(LPCSTR file,LPSTR title,WORD size) {
    LPCSTR base; int n;
    if(!file || !*file || wildcard(file)) return -1;
    base=base_name(file); n=lstrlen(base);
    if(!n) return -1;
    if(!title || size<(WORD)(n+1)) return (short)(n+1);
    lstrcpy(title,base);
    return 0;
}

/* --- Open and Save As --------------------------------------------------------- */
typedef struct {
    LPOPENFILENAME ofn; BOOL save; char spec[128],initial_dir[MAX_PATH]; BOOL ok;
} FileDialog;
static void file_lists(HWND dlg,FileDialog *f) {
    char spec[128],dir[MAX_PATH],*p,*start; HWND files=GetDlgItem(dlg,lst1),dirs=GetDlgItem(dlg,lst2);
    SendMessage(files,LB_RESETCONTENT,0,0);
    lstrcpy(spec,f->spec);
    for(start=p=spec;;p++) if(!*p || *p==';') {
        char one=*p; *p=0;
        while(*start==' ') start++;
        if(*start) SendMessage(files,LB_DIR,DDL_READWRITE,(LPARAM)start);
        if(!one) break;
        start=p+1;
    }
    {
        int n=(int)SendMessage(files,LB_GETCOUNT,0,0),i; char name[MAX_PATH];
        for(i=0;i<n;i++) {SendMessage(files,LB_GETTEXT,(WPARAM)i,(LPARAM)name); AnsiLower(name); SendMessage(files,LB_DELETESTRING,(WPARAM)i,0); SendMessage(files,LB_INSERTSTRING,(WPARAM)i,(LPARAM)name);}
    }
    SendMessage(dirs,LB_RESETCONTENT,0,0);
    SendMessage(dirs,LB_DIR,DDL_DIRECTORY|DDL_EXCLUSIVE,(LPARAM)"*.*");
    GetCurrentDirectory(sizeof(dir),dir); AnsiLower(dir); SetDlgItemText(dlg,stc1,dir);
    {
        HWND drives=GetDlgItem(dlg,cmb2); int i,n; char item_text[16];
        SendMessage(drives,CB_RESETCONTENT,0,0);
        SendMessage(drives,CB_DIR,DDL_DRIVES|DDL_EXCLUSIVE,(LPARAM)"*.*");
        n=(int)SendMessage(drives,CB_GETCOUNT,0,0);
        for(i=0;i<n;i++) {
            SendMessage(drives,CB_GETLBTEXT,(WPARAM)i,(LPARAM)item_text);
            if(item_text[2]==dir[0]) {SendMessage(drives,CB_SETCURSEL,(WPARAM)i,0); break;}
        }
    }
}
/* The pattern of filter n (1-based): description, pattern, ..., two NULs. */
static LPCSTR filter_pattern(LPCSTR filter,DWORD n) {
    DWORD i;
    if(!filter || !n) return NULL;
    for(i=1;*filter;i++) {
        filter+=lstrlen(filter)+1;
        if(!*filter) return NULL;
        if(i==n) return filter;
        filter+=lstrlen(filter)+1;
    }
    return NULL;
}
static void set_type(HWND dlg,FileDialog *f,DWORD n) {
    LPCSTR pattern=filter_pattern(f->ofn->lpstrFilter,n);
    if(!pattern && !n && f->ofn->lpstrCustomFilter && *f->ofn->lpstrCustomFilter)
        pattern=f->ofn->lpstrCustomFilter+lstrlen(f->ofn->lpstrCustomFilter)+1;
    lstrcpyn(f->spec,pattern && *pattern?pattern:"*.*",sizeof(f->spec));
    file_lists(dlg,f);
}
static int complain(HWND dlg,FileDialog *f,LPCSTR path,LPCSTR why,UINT type) {
    char text[MAX_PATH+160];
    wsprintf(text,"%s\n%s",path,why);
    return MessageBox(dlg,text,f->ofn->lpstrTitle?f->ofn->lpstrTitle:f->save?"Save As":"Open",type);
}
/* The name typed: a pattern lists, a directory or drive changes to it, a
 * file ends the dialog after the checks the flags ask for. */
static BOOL accept_file(HWND dlg,FileDialog *f) {
    LPOPENFILENAME o=f->ofn; char name[MAX_PATH],full[MAX_PATH]; DWORD attrs; LPCSTR base,ext; int n;
    GetDlgItemText(dlg,edt1,name,sizeof(name));
    if(!name[0]) return FALSE;
    if(wildcard(name)) {
        base=base_name(name);
        if(base!=name) {
            char dir[MAX_PATH]; dir_of(name,dir);
            if(!SetCurrentDirectory(dir)) {complain(dlg,f,name,"Path not found.\nPlease verify the correct path was given.",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
        }
        lstrcpyn(f->spec,base,sizeof(f->spec)); file_lists(dlg,f); SetDlgItemText(dlg,edt1,f->spec);
        return FALSE;
    }
    if((name[1]==':' && !name[2]) || ((attrs=GetFileAttributes(name))!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_DIRECTORY))) {
        if(!SetCurrentDirectory(name)) {complain(dlg,f,name,"Drive or directory not available.",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
        file_lists(dlg,f); SetDlgItemText(dlg,edt1,f->spec);
        return FALSE;
    }
    if(name[1]==':' || name[0]=='\\') lstrcpy(full,name);
    else {
        GetCurrentDirectory(sizeof(full),full); n=lstrlen(full);
        if(n && full[n-1]!='\\') lstrcat(full,"\\");
        lstrcat(full,name);
    }
    base=base_name(full);
    for(ext=base;*ext && *ext!='.';ext++) {}
    if(!*ext && o->lpstrDefExt && *o->lpstrDefExt && lstrlen(full)+lstrlen(o->lpstrDefExt)+2<MAX_PATH) {lstrcat(full,"."); lstrcat(full,o->lpstrDefExt);}
    AnsiLower(full);
    attrs=GetFileAttributes(full);
    if(attrs==INVALID_FILE_ATTRIBUTES) {
        char dir[MAX_PATH]; dir_of(full,dir);
        if((o->Flags&(OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST)) && dir[0] && GetFileAttributes(dir)==INVALID_FILE_ATTRIBUTES && lstrlen(dir)>2) {
            complain(dlg,f,full,"Path not found.\nPlease verify the correct path was given.",MB_OK|MB_ICONEXCLAMATION); return FALSE;
        }
        if(o->Flags&OFN_FILEMUSTEXIST) {complain(dlg,f,full,"File not found.\nPlease verify the correct filename was given.",MB_OK|MB_ICONEXCLAMATION); return FALSE;}
        if((o->Flags&OFN_CREATEPROMPT) && complain(dlg,f,full,"This file does not exist.\nCreate the file?",MB_YESNO|MB_ICONQUESTION)!=IDYES) return FALSE;
    } else if(f->save && (o->Flags&OFN_OVERWRITEPROMPT) &&
              complain(dlg,f,full,"This file already exists.\nReplace existing file?",MB_YESNO|MB_ICONQUESTION)!=IDYES) return FALSE;
    n=lstrlen(full);
    if(!o->lpstrFile || (DWORD)n+1>o->nMaxFile) {
        error_code=FNERR_BUFFERTOOSMALL;
        if(o->lpstrFile && o->nMaxFile>=2) {o->lpstrFile[0]=(char)(n+1); o->lpstrFile[1]=(char)((n+1)>>8);}
        f->ok=FALSE; return TRUE;
    }
    lstrcpy(o->lpstrFile,full);
    base=base_name(o->lpstrFile);
    o->nFileOffset=(WORD)(base-o->lpstrFile);
    for(ext=base;*ext && *ext!='.';ext++) {}
    o->nFileExtension=(WORD)(*ext?ext+1-o->lpstrFile:0);
    if(o->lpstrDefExt && *o->lpstrDefExt && *ext && lstrcmpi(ext+1,o->lpstrDefExt)) o->Flags|=OFN_EXTENSIONDIFFERENT;
    else o->Flags&=~OFN_EXTENSIONDIFFERENT;
    if(o->lpstrFileTitle && o->nMaxFileTitle) lstrcpyn(o->lpstrFileTitle,base,(int)o->nMaxFileTitle);
    if(!(o->Flags&OFN_HIDEREADONLY) && IsDlgButtonChecked(dlg,chx1)) o->Flags|=OFN_READONLY; else o->Flags&=~OFN_READONLY;
    f->ok=TRUE;
    return TRUE;
}
static INT_PTR CALLBACK FileProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    FileDialog *f=(FileDialog *)GetProp(dlg,"CommDlg");
    switch(msg) {
    case WM_INITDIALOG: {
        LPOPENFILENAME o; LPCSTR filter; char start[MAX_PATH]; int n=0;
        f=(FileDialog *)lp; o=f->ofn; SetProp(dlg,"CommDlg",(HANDLE)f);
        if(o->lpstrTitle) SetWindowText(dlg,o->lpstrTitle);
        if(o->lpstrInitialDir && *o->lpstrInitialDir) SetCurrentDirectory(o->lpstrInitialDir);
        for(filter=o->lpstrFilter;filter && *filter;n++) {
            SendDlgItemMessage(dlg,cmb1,CB_ADDSTRING,0,(LPARAM)filter);
            filter+=lstrlen(filter)+1;
            if(*filter) filter+=lstrlen(filter)+1;
        }
        if(n) SendDlgItemMessage(dlg,cmb1,CB_SETCURSEL,(WPARAM)(o->nFilterIndex && o->nFilterIndex<=(DWORD)n?o->nFilterIndex-1:0),0);
        if(!o->nFilterIndex && !(o->lpstrCustomFilter && *o->lpstrCustomFilter) && n) o->nFilterIndex=1;
        start[0]=0;
        if(o->lpstrFile && *o->lpstrFile) lstrcpyn(start,o->lpstrFile,sizeof(start));
        if(start[0] && base_name(start)!=start) {
            char dir[MAX_PATH]; dir_of(start,dir);
            SetCurrentDirectory(dir);
        }
        if(start[0] && wildcard(start)) {lstrcpyn(f->spec,base_name(start),sizeof(f->spec)); file_lists(dlg,f);}
        else set_type(dlg,f,o->nFilterIndex);
        SetDlgItemText(dlg,edt1,start[0] && !wildcard(start)?base_name(start):f->spec);
        if(o->Flags&OFN_READONLY) CheckDlgButton(dlg,chx1,1);
        SendDlgItemMessage(dlg,edt1,EM_SETSEL,0,-1);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDOK: if(accept_file(dlg,f)) EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        case lst1:
            if(HIWORD(wp)==LBN_SELCHANGE || HIWORD(wp)==LBN_DBLCLK) {
                char name[MAX_PATH]; LRESULT i=SendDlgItemMessage(dlg,lst1,LB_GETCURSEL,0,0);
                if(i>=0) {SendDlgItemMessage(dlg,lst1,LB_GETTEXT,(WPARAM)i,(LPARAM)name); SetDlgItemText(dlg,edt1,name);}
                if(HIWORD(wp)==LBN_DBLCLK && accept_file(dlg,f)) EndDialog(dlg,IDOK);
            }
            return TRUE;
        case lst2:
            if(HIWORD(wp)==LBN_DBLCLK) {
                char name[MAX_PATH];
                if(DlgDirSelect(dlg,name,lst2)) {SetCurrentDirectory(name); file_lists(dlg,f); SetDlgItemText(dlg,edt1,f->spec);}
            }
            return TRUE;
        case cmb1:
            if(HIWORD(wp)==CBN_SELCHANGE) {
                LRESULT i=SendDlgItemMessage(dlg,cmb1,CB_GETCURSEL,0,0);
                if(i>=0) {f->ofn->nFilterIndex=(DWORD)i+1; set_type(dlg,f,(DWORD)i+1); SetDlgItemText(dlg,edt1,f->spec);}
            }
            return TRUE;
        case cmb2:
            if(HIWORD(wp)==CBN_SELCHANGE) {
                char name[16];
                if(DlgDirSelectComboBox(dlg,name,cmb2) && SetCurrentDirectory(name)) file_lists(dlg,f);
                else {file_lists(dlg,f); MessageBox(dlg,"Drive not ready.","Open",MB_OK|MB_ICONEXCLAMATION);}
            }
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY: RemoveProp(dlg,"CommDlg"); return FALSE;
    }
    return FALSE;
}
static BOOL file_dialog(LPOPENFILENAME o,BOOL save) {
    static Template t; FileDialog f; BOOL ok;
    error_code=0;
    if(!o || o->lStructSize<sizeof(OPENFILENAME)) {error_code=CDERR_STRUCTSIZE; return FALSE;}
    memset(&f,0,sizeof(f)); f.ofn=o; f.save=save;
    GetCurrentDirectory(sizeof(f.initial_dir),f.initial_dir);
    begin(&t,save?"Save As":"Open",264,134,0);
    LABEL(&t,6,6,90,9,stc3,"File &Name:");
    item(&t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,6,16,90,12,edt1,"");
    item(&t,LISTBOX,LBS_SORT|LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP,6,32,90,68,lst1,"");
    LABEL(&t,110,6,92,9,(WORD)-1,"&Directories:");
    item(&t,STATIC,SS_LEFT|SS_NOPREFIX,110,18,92,9,stc1,"");
    item(&t,LISTBOX,LBS_SORT|LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP,110,32,92,68,lst2,"");
    LABEL(&t,6,104,90,9,stc2,save?"Save File as &Type:":"List Files of &Type:");
    item(&t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,6,114,90,60,cmb1,"");
    LABEL(&t,110,104,92,9,stc4,"Dri&ves:");
    item(&t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,110,114,92,68,cmb2,"");
    item(&t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,208,6,50,14,IDOK,"OK");
    PUSH(&t,208,24,IDCANCEL,"Cancel");
    if(!(o->Flags&OFN_HIDEREADONLY)) item(&t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP,208,68,50,12,chx1,"&Read Only");
    ok=DialogBoxIndirectParam(module,end(&t),o->hwndOwner,FileProc,(LPARAM)&f)==IDOK && f.ok;
    if(o->Flags&OFN_NOCHANGEDIR) SetCurrentDirectory(f.initial_dir);
    return ok;
}
BOOL WINAPI GetOpenFileName(LPOPENFILENAME o) {return file_dialog(o,FALSE);}
BOOL WINAPI GetSaveFileName(LPOPENFILENAME o) {return file_dialog(o,TRUE);}

/* --- Color ------------------------------------------------------------------- */
#define BASIC_ID 0x0700
#define CUSTOM_ID 0x0740
static const COLORREF basic[48]={
    RGB(255,128,128),RGB(255,255,232),RGB(128,255,128),RGB(0,255,128),RGB(128,255,255),RGB(0,128,255),RGB(255,128,192),RGB(255,128,255),
    RGB(255,0,0),RGB(255,255,128),RGB(128,255,0),RGB(0,255,64),RGB(0,255,255),RGB(0,128,192),RGB(128,128,192),RGB(255,0,255),
    RGB(128,64,64),RGB(255,255,0),RGB(0,255,0),RGB(0,128,128),RGB(0,64,128),RGB(128,128,255),RGB(128,0,64),RGB(255,0,128),
    RGB(128,0,0),RGB(255,128,0),RGB(0,128,0),RGB(0,128,64),RGB(0,0,255),RGB(0,0,160),RGB(128,0,128),RGB(128,0,255),
    RGB(64,0,0),RGB(128,64,0),RGB(0,64,0),RGB(0,64,64),RGB(0,0,128),RGB(0,0,64),RGB(64,0,64),RGB(64,0,128),
    RGB(0,0,0),RGB(128,128,0),RGB(128,128,64),RGB(128,128,128),RGB(64,128,128),RGB(192,192,192),RGB(64,0,64),RGB(255,255,255)};
typedef struct {LPCHOOSECOLOR cc; COLORREF current; COLORREF custom[16]; int next_custom; BOOL busy;} ColorDialog;
static void color_values(HWND dlg,ColorDialog *c) {
    c->busy=TRUE;
    SetDlgItemInt(dlg,edt1,GetRValue(c->current),FALSE); SetDlgItemInt(dlg,edt2,GetGValue(c->current),FALSE);
    SetDlgItemInt(dlg,edt3,GetBValue(c->current),FALSE);
    c->busy=FALSE;
    InvalidateRect(GetDlgItem(dlg,stc5),NULL,TRUE);
}
static INT_PTR CALLBACK ColorProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    ColorDialog *c=(ColorDialog *)GetProp(dlg,"CommDlg");
    switch(msg) {
    case WM_INITDIALOG:
        c=(ColorDialog *)lp; SetProp(dlg,"CommDlg",(HANDLE)c);
        color_values(dlg,c);
        /* Enter on a color would pick it: start on OK. */
        SetFocus(GetDlgItem(dlg,IDOK));
        return FALSE;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d=(const DRAWITEMSTRUCT *)lp; COLORREF color; HBRUSH b; RECT r=d->rcItem;
        if(!c) return FALSE;
        if(d->CtlID>=BASIC_ID && d->CtlID<BASIC_ID+48) color=basic[d->CtlID-BASIC_ID];
        else if(d->CtlID>=CUSTOM_ID && d->CtlID<CUSTOM_ID+16) color=c->custom[d->CtlID-CUSTOM_ID];
        else if(d->CtlID==stc5) color=c->current;
        else return FALSE;
        FrameRect(d->hDC,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        InflateRect(&r,-1,-1);
        b=CreateSolidBrush(color); FillRect(d->hDC,&r,b); DeleteObject(b);
        if(d->itemState&ODS_FOCUS) {InflateRect(&r,-1,-1); DrawFocusRect(d->hDC,&r);}
        return TRUE;
    }
    case WM_COMMAND: {
        UINT id=LOWORD(wp);
        if(id>=BASIC_ID && id<BASIC_ID+48) {c->current=basic[id-BASIC_ID]; color_values(dlg,c); return TRUE;}
        if(id>=CUSTOM_ID && id<CUSTOM_ID+16) {c->current=c->custom[id-CUSTOM_ID]; color_values(dlg,c); return TRUE;}
        switch(id) {
        case edt1: case edt2: case edt3:
            if(HIWORD(wp)==EN_CHANGE && c && !c->busy) {
                BOOL ok; UINT r=GetDlgItemInt(dlg,edt1,&ok,FALSE),g=GetDlgItemInt(dlg,edt2,&ok,FALSE),b=GetDlgItemInt(dlg,edt3,&ok,FALSE);
                c->current=RGB(min(r,255),min(g,255),min(b,255));
                InvalidateRect(GetDlgItem(dlg,stc5),NULL,TRUE);
            }
            return TRUE;
        case psh1:
            c->custom[c->next_custom]=c->current; InvalidateRect(GetDlgItem(dlg,CUSTOM_ID+c->next_custom),NULL,TRUE);
            c->next_custom=(c->next_custom+1)%16;
            return TRUE;
        case IDOK: EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    }
    case WM_DESTROY: RemoveProp(dlg,"CommDlg"); return FALSE;
    }
    return FALSE;
}
BOOL WINAPI ChooseColor(LPCHOOSECOLOR cc) {
    static Template t; ColorDialog c; int i;
    error_code=0;
    if(!cc || cc->lStructSize<sizeof(CHOOSECOLOR)) {error_code=CDERR_STRUCTSIZE; return FALSE;}
    memset(&c,0,sizeof(c)); c.cc=cc; c.current=cc->Flags&CC_RGBINIT?cc->rgbResult&0xffffff:RGB(0,0,0);
    for(i=0;i<16;i++) c.custom[i]=cc->lpCustColors?cc->lpCustColors[i]&0xffffff:RGB(255,255,255);
    begin(&t,"Color",258,148,0);
    LABEL(&t,6,4,140,9,(WORD)-1,"&Basic Colors:");
    for(i=0;i<48;i++) item(&t,BUTTON,BS_OWNERDRAW|(i?0:WS_TABSTOP|WS_GROUP),6+(i%8)*18,14+(i/8)*14,16,12,(WORD)(BASIC_ID+i),"");
    LABEL(&t,6,100,140,9,(WORD)-1,"&Custom Colors:");
    for(i=0;i<16;i++) item(&t,BUTTON,BS_OWNERDRAW|(i?0:WS_TABSTOP|WS_GROUP),6+(i%8)*18,110+(i/8)*14,16,12,(WORD)(CUSTOM_ID+i),"");
    item(&t,BUTTON,BS_OWNERDRAW|WS_DISABLED,154,14,98,26,stc5,"");
    LABEL(&t,154,46,30,9,(WORD)-1,"&Red:");
    item(&t,EDIT,WS_BORDER|WS_TABSTOP|WS_GROUP,190,44,30,12,edt1,"");
    LABEL(&t,154,62,30,9,(WORD)-1,"&Green:");
    item(&t,EDIT,WS_BORDER|WS_TABSTOP,190,60,30,12,edt2,"");
    LABEL(&t,154,78,30,9,(WORD)-1,"Bl&ue:");
    item(&t,EDIT,WS_BORDER|WS_TABSTOP,190,76,30,12,edt3,"");
    item(&t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,154,94,98,14,psh1,"&Add to Custom Colors");
    item(&t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,154,114,46,14,IDOK,"OK");
    item(&t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,206,114,46,14,IDCANCEL,"Cancel");
    if(DialogBoxIndirectParam(module,end(&t),cc->hwndOwner,ColorProc,(LPARAM)&c)!=IDOK) return FALSE;
    cc->rgbResult=c.current;
    if(cc->lpCustColors) for(i=0;i<16;i++) cc->lpCustColors[i]=c.custom[i];
    return TRUE;
}

/* --- Font -------------------------------------------------------------------- */
static const int point_sizes[]={8,9,10,11,12,14};
static const COLORREF effect_colors[16]={
    RGB(0,0,0),RGB(128,0,0),RGB(0,128,0),RGB(128,128,0),RGB(0,0,128),RGB(128,0,128),RGB(0,128,128),RGB(128,128,128),
    RGB(192,192,192),RGB(255,0,0),RGB(0,255,0),RGB(255,255,0),RGB(0,0,255),RGB(255,0,255),RGB(0,255,255),RGB(255,255,255)};
static const char *const color_names[16]={"Black","Maroon","Green","Olive","Navy","Purple","Teal","Gray","Silver","Red","Lime",
    "Yellow","Blue","Fuchsia","Aqua","White"};
typedef struct {LPCHOOSEFONT cf; LOGFONT lf; int points; COLORREF color; HFONT sample;} FontDialog;
static int CALLBACK add_face(const LOGFONT FAR *lf,const TEXTMETRIC FAR *tm,int type,LPARAM lp) {
    const FontDialog *f=(const FontDialog *)((HWND *)lp)[1]; HWND combo=((HWND *)lp)[0];
    (void)tm; (void)type;
    if((f->cf->Flags&CF_FIXEDPITCHONLY) && (lf->lfPitchAndFamily&3)!=FIXED_PITCH) return 1;
    if(SendMessage(combo,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)lf->lfFaceName)<0) SendMessage(combo,CB_ADDSTRING,0,(LPARAM)lf->lfFaceName);
    return 1;
}
static void font_from_controls(HWND dlg,FontDialog *f) {
    LRESULT i; char text[LF_FACESIZE]; BOOL ok; int points;
    i=SendDlgItemMessage(dlg,cmb1,CB_GETCURSEL,0,0);
    if(i>=0) {SendDlgItemMessage(dlg,cmb1,CB_GETLBTEXT,(WPARAM)i,(LPARAM)text); lstrcpyn(f->lf.lfFaceName,text,LF_FACESIZE);}
    i=SendDlgItemMessage(dlg,cmb2,CB_GETCURSEL,0,0);
    if(i>=0) {f->lf.lfWeight=i>=2?FW_BOLD:FW_NORMAL; f->lf.lfItalic=(BYTE)(i&1);}
    points=(int)GetDlgItemInt(dlg,cmb3,&ok,FALSE);
    if(ok && points>0 && points<200) f->points=points;
    f->lf.lfHeight=-MulDiv(f->points,96,72);
    f->lf.lfWidth=0;
    f->lf.lfUnderline=(BYTE)IsDlgButtonChecked(dlg,chx2); f->lf.lfStrikeOut=(BYTE)IsDlgButtonChecked(dlg,chx1);
    i=SendDlgItemMessage(dlg,cmb4,CB_GETCURSEL,0,0);
    if(i>=0 && i<16) f->color=effect_colors[i];
    if(f->sample) DeleteObject(f->sample);
    f->sample=CreateFontIndirect(&f->lf);
    InvalidateRect(GetDlgItem(dlg,stc5),NULL,FALSE);
}
static INT_PTR CALLBACK FontProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    FontDialog *f=(FontDialog *)GetProp(dlg,"CommDlg");
    switch(msg) {
    case WM_INITDIALOG: {
        HDC dc; HWND args[2]; int i; char text[16];
        f=(FontDialog *)lp; SetProp(dlg,"CommDlg",(HANDLE)f);
        args[0]=GetDlgItem(dlg,cmb1); args[1]=(HWND)f;
        dc=f->cf->hDC?f->cf->hDC:GetDC(NULL);
        EnumFonts(dc,NULL,add_face,(LPARAM)args);
        if(!f->cf->hDC) ReleaseDC(NULL,dc);
        if(SendDlgItemMessage(dlg,cmb1,CB_SELECTSTRING,(WPARAM)-1,(LPARAM)f->lf.lfFaceName)<0) SendDlgItemMessage(dlg,cmb1,CB_SETCURSEL,0,0);
        SendDlgItemMessage(dlg,cmb2,CB_ADDSTRING,0,(LPARAM)"Regular"); SendDlgItemMessage(dlg,cmb2,CB_ADDSTRING,0,(LPARAM)"Italic");
        SendDlgItemMessage(dlg,cmb2,CB_ADDSTRING,0,(LPARAM)"Bold"); SendDlgItemMessage(dlg,cmb2,CB_ADDSTRING,0,(LPARAM)"Bold Italic");
        SendDlgItemMessage(dlg,cmb2,CB_SETCURSEL,(WPARAM)((f->lf.lfWeight>=FW_SEMIBOLD?2:0)+(f->lf.lfItalic?1:0)),0);
        for(i=0;i<(int)(sizeof(point_sizes)/sizeof(point_sizes[0]));i++) {
            wsprintf(text,"%d",point_sizes[i]); SendDlgItemMessage(dlg,cmb3,CB_ADDSTRING,0,(LPARAM)text);
        }
        SetDlgItemInt(dlg,cmb3,(UINT)f->points,FALSE);
        CheckDlgButton(dlg,chx1,f->lf.lfStrikeOut); CheckDlgButton(dlg,chx2,f->lf.lfUnderline);
        if(GetDlgItem(dlg,cmb4)) {
            for(i=0;i<16;i++) SendDlgItemMessage(dlg,cmb4,CB_ADDSTRING,0,(LPARAM)color_names[i]);
            for(i=0;i<16 && effect_colors[i]!=f->color;i++) {}
            SendDlgItemMessage(dlg,cmb4,CB_SETCURSEL,(WPARAM)(i<16?i:0),0);
        }
        font_from_controls(dlg,f);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d=(const DRAWITEMSTRUCT *)lp; RECT r=d->rcItem; HGDIOBJ old;
        if(!f || d->CtlID!=stc5) return FALSE;
        FrameRect(d->hDC,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        InflateRect(&r,-1,-1); FillRect(d->hDC,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
        old=SelectObject(d->hDC,f->sample?(HGDIOBJ)f->sample:GetStockObject(SYSTEM_FONT));
        SetTextColor(d->hDC,f->color); SetBkMode(d->hDC,TRANSPARENT);
        DrawText(d->hDC,"AaBbYyZz",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(d->hDC,old);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case cmb1: case cmb2: case cmb4: if(HIWORD(wp)==CBN_SELCHANGE) font_from_controls(dlg,f); return TRUE;
        case cmb3:
            if(HIWORD(wp)==CBN_SELCHANGE) {
                LRESULT i=SendDlgItemMessage(dlg,cmb3,CB_GETCURSEL,0,0);
                if(i>=0) {char text[16]; SendDlgItemMessage(dlg,cmb3,CB_GETLBTEXT,(WPARAM)i,(LPARAM)text); SetDlgItemText(dlg,cmb3,text);}
                font_from_controls(dlg,f);
            }
            return TRUE;
        case chx1: case chx2: font_from_controls(dlg,f); return TRUE;
        case IDOK: font_from_controls(dlg,f); EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        return FALSE;
    case WM_DESTROY:
        if(f && f->sample) {DeleteObject(f->sample); f->sample=NULL;}
        RemoveProp(dlg,"CommDlg");
        return FALSE;
    }
    return FALSE;
}
BOOL WINAPI ChooseFont(LPCHOOSEFONT cf) {
    static Template t; FontDialog f; BOOL effects;
    error_code=0;
    if(!cf || cf->lStructSize<sizeof(CHOOSEFONT)) {error_code=CDERR_STRUCTSIZE; return FALSE;}
    if(!cf->lpLogFont) {error_code=CDERR_INITIALIZATION; return FALSE;}
    memset(&f,0,sizeof(f)); f.cf=cf; f.points=10; f.color=cf->Flags&CF_EFFECTS?cf->rgbColors&0xffffff:RGB(0,0,0);
    lstrcpy(f.lf.lfFaceName,"System"); f.lf.lfWeight=FW_NORMAL; f.lf.lfCharSet=ANSI_CHARSET;
    if(cf->Flags&CF_INITTOLOGFONTSTRUCT) {
        f.lf=*cf->lpLogFont;
        if(f.lf.lfHeight) f.points=MulDiv(f.lf.lfHeight<0?-f.lf.lfHeight:f.lf.lfHeight,72,96);
    }
    effects=(cf->Flags&CF_EFFECTS)!=0;
    begin(&t,"Font",254,112,0);
    LABEL(&t,6,4,94,9,stc1,"&Font:");
    item(&t,COMBOBOX,CBS_DROPDOWNLIST|CBS_SORT|WS_VSCROLL|WS_TABSTOP|WS_GROUP,6,14,94,80,cmb1,"");
    LABEL(&t,106,4,64,9,stc2,"Font St&yle:");
    item(&t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,106,14,64,60,cmb2,"");
    LABEL(&t,176,4,30,9,stc3,"&Size:");
    item(&t,COMBOBOX,CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP|WS_GROUP,176,14,30,80,cmb3,"");
    item(&t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,212,6,36,14,IDOK,"OK");
    item(&t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,212,24,36,14,IDCANCEL,"Cancel");
    if(effects) {
        item(&t,BUTTON,BS_GROUPBOX,6,36,84,68,grp1,"Effects");
        item(&t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|WS_GROUP,12,48,70,10,chx1,"Stri&keout");
        item(&t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP,12,62,70,10,chx2,"&Underline");
        LABEL(&t,12,76,30,9,stc4,"&Color:");
        item(&t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,12,86,70,80,cmb4,"");
    }
    item(&t,BUTTON,BS_GROUPBOX,98,36,108,68,grp2,"Sample");
    item(&t,BUTTON,BS_OWNERDRAW|WS_DISABLED,104,48,96,50,stc5,"");
    if(DialogBoxIndirectParam(module,end(&t),cf->hwndOwner,FontProc,(LPARAM)&f)!=IDOK) return FALSE;
    *cf->lpLogFont=f.lf;
    cf->iPointSize=f.points*10;
    if(effects) cf->rgbColors=f.color;
    cf->nFontType=(WORD)(SCREEN_FONTTYPE|(f.lf.lfWeight>=FW_BOLD?BOLD_FONTTYPE:0)|(f.lf.lfItalic?ITALIC_FONTTYPE:0)|
                         (f.lf.lfWeight<FW_BOLD && !f.lf.lfItalic?REGULAR_FONTTYPE:0));
    if((cf->Flags&CF_USESTYLE) && cf->lpszStyle) {
        static const char *const styles[4]={"Regular","Italic","Bold","Bold Italic"};
        lstrcpy(cf->lpszStyle,styles[(f.lf.lfWeight>=FW_BOLD?2:0)+(f.lf.lfItalic?1:0)]);
    }
    return TRUE;
}

/* --- Find and Replace ----------------------------------------------------------- */
static UINT find_message;
static void find_flags(HWND dlg,LPFINDREPLACE fr,DWORD action) {
    fr->Flags&=~(FR_FINDNEXT|FR_REPLACE|FR_REPLACEALL|FR_DIALOGTERM|FR_DOWN|FR_WHOLEWORD|FR_MATCHCASE);
    fr->Flags|=action;
    if(IsDlgButtonChecked(dlg,chx1)) fr->Flags|=FR_WHOLEWORD;
    if(IsDlgButtonChecked(dlg,chx2)) fr->Flags|=FR_MATCHCASE;
    if(!GetDlgItem(dlg,rad2) || IsDlgButtonChecked(dlg,rad2)) fr->Flags|=FR_DOWN;
    if(fr->lpstrFindWhat && fr->wFindWhatLen) GetDlgItemText(dlg,edt1,fr->lpstrFindWhat,fr->wFindWhatLen);
    if(fr->lpstrReplaceWith && fr->wReplaceWithLen && GetDlgItem(dlg,edt2)) GetDlgItemText(dlg,edt2,fr->lpstrReplaceWith,fr->wReplaceWithLen);
}
static INT_PTR CALLBACK FindProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    LPFINDREPLACE fr=(LPFINDREPLACE)GetProp(dlg,"CommDlg");
    switch(msg) {
    case WM_INITDIALOG:
        fr=(LPFINDREPLACE)lp; SetProp(dlg,"CommDlg",(HANDLE)fr);
        if(fr->lpstrFindWhat) SetDlgItemText(dlg,edt1,fr->lpstrFindWhat);
        if(fr->lpstrReplaceWith && GetDlgItem(dlg,edt2)) SetDlgItemText(dlg,edt2,fr->lpstrReplaceWith);
        CheckDlgButton(dlg,chx1,(fr->Flags&FR_WHOLEWORD)!=0); CheckDlgButton(dlg,chx2,(fr->Flags&FR_MATCHCASE)!=0);
        if(GetDlgItem(dlg,rad1)) CheckRadioButton(dlg,rad1,rad2,fr->Flags&FR_DOWN?rad2:rad1);
        EnableWindow(GetDlgItem(dlg,IDOK),GetWindowTextLength(GetDlgItem(dlg,edt1))>0);
        SendDlgItemMessage(dlg,edt1,EM_SETSEL,0,-1);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case edt1:
            if(HIWORD(wp)==EN_CHANGE) {
                BOOL any=GetWindowTextLength(GetDlgItem(dlg,edt1))>0;
                EnableWindow(GetDlgItem(dlg,IDOK),any);
                if(GetDlgItem(dlg,psh1)) {EnableWindow(GetDlgItem(dlg,psh1),any); EnableWindow(GetDlgItem(dlg,psh2),any);}
            }
            return TRUE;
        case IDOK: find_flags(dlg,fr,FR_FINDNEXT); SendMessage(fr->hwndOwner,find_message,0,(LPARAM)fr); return TRUE;
        case psh1: find_flags(dlg,fr,FR_REPLACE); SendMessage(fr->hwndOwner,find_message,0,(LPARAM)fr); return TRUE;
        case psh2: find_flags(dlg,fr,FR_REPLACEALL); SendMessage(fr->hwndOwner,find_message,0,(LPARAM)fr); return TRUE;
        case IDCANCEL:
            find_flags(dlg,fr,FR_DIALOGTERM); SendMessage(fr->hwndOwner,find_message,0,(LPARAM)fr);
            DestroyWindow(dlg);
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY: RemoveProp(dlg,"CommDlg"); return FALSE;
    }
    return FALSE;
}
static HWND find_dialog(LPFINDREPLACE fr,BOOL replace) {
    static Template find,repl; Template *t=replace?&repl:&find;
    error_code=0;
    if(!fr || fr->lStructSize<sizeof(FINDREPLACE)) {error_code=CDERR_STRUCTSIZE; return NULL;}
    if(!fr->lpstrFindWhat || !fr->wFindWhatLen) {error_code=FRERR_BUFFERLENGTHZERO; return NULL;}
    if(!find_message) find_message=RegisterWindowMessage(FINDMSGSTRING);
    if(!replace) {
        begin(t,"Find",236,62,WS_VISIBLE);
        LABEL(t,4,8,42,9,(WORD)-1,"Fi&nd What:");
        item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,47,6,128,12,edt1,"");
        if(!(fr->Flags&FR_HIDEWHOLEWORD)) item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|WS_GROUP|(fr->Flags&FR_NOWHOLEWORD?WS_DISABLED:0),4,26,100,12,chx1,"Match &Whole Word Only");
        if(!(fr->Flags&FR_HIDEMATCHCASE)) item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|(fr->Flags&FR_NOMATCHCASE?WS_DISABLED:0),4,42,64,12,chx2,"Match &Case");
        if(!(fr->Flags&FR_HIDEUPDOWN)) {
            item(t,BUTTON,BS_GROUPBOX,107,26,68,28,grp1,"Direction");
            item(t,BUTTON,BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP|(fr->Flags&FR_NOUPDOWN?WS_DISABLED:0),111,38,25,12,rad1,"&Up");
            item(t,BUTTON,BS_AUTORADIOBUTTON|(fr->Flags&FR_NOUPDOWN?WS_DISABLED:0),138,38,35,12,rad2,"&Down");
        }
        item(t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,182,5,50,14,IDOK,"&Find Next");
        PUSH(t,182,23,IDCANCEL,"Cancel");
    } else {
        begin(t,"Replace",236,94,WS_VISIBLE);
        LABEL(t,4,9,48,9,(WORD)-1,"Fi&nd What:");
        item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,54,7,114,12,edt1,"");
        LABEL(t,4,26,48,9,(WORD)-1,"Re&place With:");
        item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,54,24,114,12,edt2,"");
        if(!(fr->Flags&FR_HIDEWHOLEWORD)) item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|WS_GROUP|(fr->Flags&FR_NOWHOLEWORD?WS_DISABLED:0),5,46,104,12,chx1,"Match &Whole Word Only");
        if(!(fr->Flags&FR_HIDEMATCHCASE)) item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|(fr->Flags&FR_NOMATCHCASE?WS_DISABLED:0),5,62,59,12,chx2,"Match &Case");
        item(t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,174,4,58,14,IDOK,"&Find Next");
        item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,174,21,58,14,psh1,"&Replace");
        item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,174,38,58,14,psh2,"Replace &All");
        item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,174,55,58,14,IDCANCEL,"Cancel");
    }
    return CreateDialogIndirectParam(module,end(t),fr->hwndOwner,FindProc,(LPARAM)fr);
}
HWND WINAPI FindText(LPFINDREPLACE fr) {return find_dialog(fr,FALSE);}
HWND WINAPI ReplaceText(LPFINDREPLACE fr) {return find_dialog(fr,TRUE);}

/* --- Print and Print Setup --------------------------------------------------------- */
#define rad3 0x0422
#define rad4 0x0423
#define grp3 0x0432
#define PRINTERS 16
/* A printer as WIN.INI names it: the device, its driver and a port. */
typedef struct {char device[CCHDEVICENAME],driver[16],port[MAX_PATH];} Printer;
typedef struct {
    LPPRINTDLG pd;
    Printer printer; BOOL is_default; DEVMODE dm; /* the choice */
    Printer fallback; BOOL has_default;          /* [windows] device= */
    Printer list[PRINTERS]; int count;            /* [devices], a printer for each port */
} PrintDialog;
/* The next comma-separated field, without surrounding spaces. */
static LPCSTR field(LPCSTR s,char *out,int size) {
    int n=0;
    while(*s==' ') s++;
    while(*s && *s!=',') {if(n<size-1) out[n++]=*s; s++;}
    while(n && out[n-1]==' ') n--;
    out[n]=0;
    return *s==','?s+1:s;
}
static BOOL default_printer(Printer *p) {
    char line[160]; LPCSTR s=line;
    GetProfileString("windows","device","",line,sizeof(line));
    s=field(s,p->device,sizeof(p->device)); s=field(s,p->driver,sizeof(p->driver)); field(s,p->port,sizeof(p->port));
    return p->device[0] && p->driver[0] && p->port[0];
}
/* [devices]: device=driver,port[,port...]. */
static int list_printers(Printer *out,int max) {
    static char names[1024]; char line[256],driver[16]; LPCSTR name,s; int n=0;
    GetProfileString("devices",NULL,"",names,sizeof(names));
    for(name=names;*name && n<max;name+=lstrlen(name)+1) {
        GetProfileString("devices",name,"",line,sizeof(line));
        for(s=field(line,driver,sizeof(driver));*s && n<max;) {
            s=field(s,out[n].port,sizeof(out[n].port));
            if(!out[n].port[0]) continue;
            lstrcpyn(out[n].device,name,sizeof(out[n].device)); lstrcpy(out[n].driver,driver); n++;
        }
    }
    return n;
}
static void describe(const Printer *p,char *out) {wsprintf(out,"%s on %s",(LPCSTR)p->device,(LPCSTR)p->port);}
static BOOL same_printer(const Printer *a,const Printer *b) {
    return !lstrcmpi(a->device,b->device) && !lstrcmpi(a->driver,b->driver) && !lstrcmpi(a->port,b->port);
}
/* The printer's settings from its driver (PSCRIPT, in GDI). */
static BOOL driver_settings(const Printer *p,DEVMODE *dm) {
    Printer copy=*p;
    if(lstrcmpi(p->driver,"PSCRIPT")) return FALSE;
    return ExtDeviceMode(NULL,NULL,dm,copy.device,copy.port,NULL,NULL,DM_COPY)==IDOK;
}
/* A DEVNAMES block: its driver, device and port. */
static BOOL names_in(HGLOBAL h,Printer *p,BOOL *is_default) {
    const DEVNAMES *n; SIZE_T size=GlobalSize(h); BOOL ok=FALSE;
    if(size<sizeof(DEVNAMES) || !(n=(const DEVNAMES *)GlobalLock(h))) return FALSE;
    if(n->wDriverOffset<size && n->wDeviceOffset<size && n->wOutputOffset<size) {
        lstrcpyn(p->driver,(LPCSTR)n+n->wDriverOffset,sizeof(p->driver));
        lstrcpyn(p->device,(LPCSTR)n+n->wDeviceOffset,sizeof(p->device));
        lstrcpyn(p->port,(LPCSTR)n+n->wOutputOffset,sizeof(p->port));
        *is_default=(n->wDefault&DN_DEFAULTPRN)!=0; ok=p->driver[0] && p->device[0] && p->port[0];
    }
    GlobalUnlock(h);
    return ok;
}

static void show_printer(HWND dlg,PrintDialog *d) {
    char name[CCHDEVICENAME+MAX_PATH+8],text[CCHDEVICENAME+MAX_PATH+32];
    describe(&d->printer,name);
    if(d->is_default) wsprintf(text,"Default Printer (%s)",(LPCSTR)name); else lstrcpy(text,name);
    SetDlgItemText(dlg,stc1,text);
}
static INT_PTR CALLBACK SetupProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    PrintDialog *d=(PrintDialog *)GetProp(dlg,"CommDlg"); char text[CCHDEVICENAME+MAX_PATH+32]; int i,n,at;
    switch(msg) {
    case WM_INITDIALOG: {
        static char names[16][64]; WORD ids[16];
        d=(PrintDialog *)lp; SetProp(dlg,"CommDlg",(HANDLE)d);
        if(d->has_default) wsprintf(text,"(currently %s on %s)",(LPCSTR)d->fallback.device,(LPCSTR)d->fallback.port);
        else {lstrcpy(text,"(No Default Printer)"); EnableWindow(GetDlgItem(dlg,rad3),FALSE);}
        SetDlgItemText(dlg,stc1,text);
        for(i=0,at=0;i<d->count;i++) {
            describe(&d->list[i],text); SendDlgItemMessage(dlg,cmb1,CB_ADDSTRING,0,(LPARAM)text);
            if(same_printer(&d->list[i],&d->printer)) at=i;
        }
        SendDlgItemMessage(dlg,cmb1,CB_SETCURSEL,(WPARAM)at,0);
        CheckRadioButton(dlg,rad3,rad4,d->is_default && d->has_default?rad3:rad4);
        CheckRadioButton(dlg,rad1,rad2,d->dm.dmOrientation==DMORIENT_LANDSCAPE?rad2:rad1);
        n=(int)DeviceCapabilities(d->printer.device,d->printer.port,DC_PAPERS,(LPSTR)ids,NULL);
        if(n>16) n=16;
        DeviceCapabilities(d->printer.device,d->printer.port,DC_PAPERNAMES,(LPSTR)names,NULL);
        for(i=0,at=0;i<n;i++) {
            SendDlgItemMessage(dlg,cmb2,CB_ADDSTRING,0,(LPARAM)names[i]);
            SendDlgItemMessage(dlg,cmb2,CB_SETITEMDATA,(WPARAM)i,(LPARAM)ids[i]);
            if(ids[i]==(WORD)d->dm.dmPaperSize) at=i;
        }
        SendDlgItemMessage(dlg,cmb2,CB_SETCURSEL,(WPARAM)at,0);
        SendDlgItemMessage(dlg,cmb3,CB_ADDSTRING,0,(LPARAM)"Upper Tray");
        SendDlgItemMessage(dlg,cmb3,CB_SETCURSEL,0,0);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case cmb1: if(HIWORD(wp)==CBN_SELCHANGE) CheckRadioButton(dlg,rad3,rad4,rad4); return TRUE;
        case IDOK: {
            Printer chosen; DEVMODE dm; BOOL use_default=IsDlgButtonChecked(dlg,rad3)!=0;
            if(use_default) chosen=d->fallback;
            else {
                i=(int)SendDlgItemMessage(dlg,cmb1,CB_GETCURSEL,0,0);
                if(i<0 || i>=d->count) {MessageBeep(0); return TRUE;}
                chosen=d->list[i];
            }
            if(!same_printer(&chosen,&d->printer)) {
                if(!driver_settings(&chosen,&dm)) {MessageBox(dlg,"The printer's driver cannot be loaded.","Print Setup",MB_OK|MB_ICONEXCLAMATION); return TRUE;}
                d->dm=dm; d->printer=chosen;
            }
            d->is_default=use_default || (d->has_default && same_printer(&chosen,&d->fallback));
            d->dm.dmOrientation=(short)(IsDlgButtonChecked(dlg,rad2)?DMORIENT_LANDSCAPE:DMORIENT_PORTRAIT);
            i=(int)SendDlgItemMessage(dlg,cmb2,CB_GETCURSEL,0,0);
            if(i>=0) d->dm.dmPaperSize=(short)SendDlgItemMessage(dlg,cmb2,CB_GETITEMDATA,(WPARAM)i,0);
            d->dm.dmFields|=DM_ORIENTATION|DM_PAPERSIZE;
            /* The paper and orientation become the printer's own, as its driver keeps them. */
            ExtDeviceMode(dlg,NULL,NULL,d->printer.device,d->printer.port,&d->dm,NULL,DM_MODIFY|DM_UPDATE);
            EndDialog(dlg,IDOK); return TRUE;
        }
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        break;
    case WM_DESTROY: RemoveProp(dlg,"CommDlg"); break;
    }
    return FALSE;
}
static LPCDLGTEMPLATE setup_template(Template *t) {
    begin(t,"Print Setup",287,122,0);
    item(t,BUTTON,BS_GROUPBOX,4,6,224,66,grp3,"Printer");
    item(t,BUTTON,BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP,8,16,218,12,rad3,"&Default Printer");
    LABEL(t,18,30,208,9,stc1,"");
    item(t,BUTTON,BS_AUTORADIOBUTTON,8,42,218,12,rad4,"Specific &Printer:");
    item(t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,18,56,208,80,cmb1,"");
    item(t,BUTTON,BS_GROUPBOX,4,74,91,46,grp1,"Orientation");
    item(t,BUTTON,BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP,10,86,80,12,rad1,"Po&rtrait");
    item(t,BUTTON,BS_AUTORADIOBUTTON,10,102,80,12,rad2,"&Landscape");
    item(t,BUTTON,BS_GROUPBOX,100,74,128,46,grp2,"Paper");
    LABEL(t,105,88,26,9,stc2,"Si&ze:");
    item(t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,133,86,92,80,cmb2,"");
    LABEL(t,105,104,26,9,stc3,"&Source:");
    item(t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,133,102,92,40,cmb3,"");
    item(t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,233,8,50,14,IDOK,"OK");
    item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,233,26,50,14,IDCANCEL,"Cancel");
    return end(t);
}
static INT_PTR CALLBACK PrintProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    PrintDialog *d=(PrintDialog *)GetProp(dlg,"CommDlg"); LPPRINTDLG pd=d?d->pd:NULL; UINT from,to,copies; BOOL ok;
    switch(msg) {
    case WM_INITDIALOG:
        d=(PrintDialog *)lp; pd=d->pd; SetProp(dlg,"CommDlg",(HANDLE)d);
        show_printer(dlg,d);
        CheckRadioButton(dlg,rad1,rad3,pd->Flags&PD_PAGENUMS?rad3:pd->Flags&PD_SELECTION?rad2:rad1);
        if(pd->Flags&PD_NOSELECTION) EnableWindow(GetDlgItem(dlg,rad2),FALSE);
        if(pd->Flags&PD_NOPAGENUMS) {
            static const int ids[]={rad3,stc2,edt1,stc3,edt2}; int i;
            for(i=0;i<5;i++) EnableWindow(GetDlgItem(dlg,ids[i]),FALSE);
        } else if(pd->nFromPage!=0xffff) {SetDlgItemInt(dlg,edt1,pd->nFromPage,FALSE); SetDlgItemInt(dlg,edt2,pd->nToPage,FALSE);}
        SetDlgItemInt(dlg,edt3,pd->nCopies?pd->nCopies:1,FALSE);
        CheckDlgButton(dlg,chx1,(pd->Flags&PD_PRINTTOFILE)!=0);
        if(pd->Flags&PD_DISABLEPRINTTOFILE) EnableWindow(GetDlgItem(dlg,chx1),FALSE);
        if(pd->Flags&PD_HIDEPRINTTOFILE) ShowWindow(GetDlgItem(dlg,chx1),SW_HIDE);
        CheckDlgButton(dlg,chx2,(pd->Flags&PD_COLLATE)!=0);
        SendDlgItemMessage(dlg,cmb1,CB_ADDSTRING,0,(LPARAM)"150 dpi");
        SendDlgItemMessage(dlg,cmb1,CB_SETCURSEL,0,0);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case edt1: case edt2:
            /* Typing a page number chooses Pages. */
            if(HIWORD(wp)==EN_CHANGE && GetFocus()==(HWND)lp) CheckRadioButton(dlg,rad1,rad3,rad3);
            return TRUE;
        case psh1: {
            static Template t;
            if(DialogBoxIndirectParam(module,setup_template(&t),dlg,SetupProc,(LPARAM)d)==IDOK) show_printer(dlg,d);
            return TRUE;
        }
        case IDOK:
            copies=GetDlgItemInt(dlg,edt3,&ok,FALSE);
            if(!ok || copies<1 || copies>99) {
                MessageBox(dlg,"The Copies field must contain a number between 1 and 99.","Print",MB_OK|MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(dlg,edt3)); return TRUE;
            }
            if(IsDlgButtonChecked(dlg,rad3)) {
                BOOL ok_to;
                from=GetDlgItemInt(dlg,edt1,&ok,FALSE); to=GetDlgItemInt(dlg,edt2,&ok_to,FALSE);
                if(!ok_to) to=from;
                if(!ok || from>to || from<pd->nMinPage || to>pd->nMaxPage) {
                    char text[80];
                    wsprintf(text,"This value is not within the page range.\nEnter a number between %u and %u.",pd->nMinPage,pd->nMaxPage);
                    MessageBox(dlg,text,"Print",MB_OK|MB_ICONEXCLAMATION);
                    SetFocus(GetDlgItem(dlg,edt1)); return TRUE;
                }
                pd->nFromPage=(WORD)from; pd->nToPage=(WORD)to;
            }
            pd->Flags&=~(PD_SELECTION|PD_PAGENUMS|PD_PRINTTOFILE|PD_COLLATE);
            if(IsDlgButtonChecked(dlg,rad2)) pd->Flags|=PD_SELECTION;
            if(IsDlgButtonChecked(dlg,rad3)) pd->Flags|=PD_PAGENUMS;
            if(IsDlgButtonChecked(dlg,chx1)) pd->Flags|=PD_PRINTTOFILE;
            if(IsDlgButtonChecked(dlg,chx2)) pd->Flags|=PD_COLLATE;
            pd->nCopies=(WORD)copies;
            EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        }
        break;
    case WM_DESTROY: RemoveProp(dlg,"CommDlg"); break;
    }
    return FALSE;
}
static LPCDLGTEMPLATE print_template(Template *t) {
    begin(t,"Print",225,130,0);
    LABEL(t,4,4,32,9,(WORD)-1,"Printer:");
    LABEL(t,36,4,130,18,stc1,"");
    item(t,BUTTON,BS_GROUPBOX,4,27,132,67,grp1,"Print Range");
    item(t,BUTTON,BS_AUTORADIOBUTTON|WS_TABSTOP|WS_GROUP,10,39,76,12,rad1,"&All");
    item(t,BUTTON,BS_AUTORADIOBUTTON,10,52,76,12,rad2,"S&election");
    item(t,BUTTON,BS_AUTORADIOBUTTON,10,65,76,12,rad3,"&Pages");
    item(t,STATIC,SS_RIGHT,18,80,24,9,stc2,"&From:");
    item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,44,78,26,12,edt1,"");
    item(t,STATIC,SS_RIGHT,72,80,22,9,stc3,"&To:");
    item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,96,78,26,12,edt2,"");
    LABEL(t,4,100,50,9,stc4,"Print &Quality:");
    item(t,COMBOBOX,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP|WS_GROUP,55,98,81,36,cmb1,"");
    LABEL(t,148,100,32,9,stc5,"&Copies:");
    item(t,EDIT,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,184,98,22,12,edt3,"");
    item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|WS_GROUP,4,113,120,12,chx1,"Print to Fi&le");
    item(t,BUTTON,BS_AUTOCHECKBOX|WS_TABSTOP|WS_GROUP,148,113,72,12,chx2,"Collate Cop&ies");
    item(t,BUTTON,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,170,4,50,14,IDOK,"OK");
    item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,170,21,50,14,IDCANCEL,"Cancel");
    item(t,BUTTON,BS_PUSHBUTTON|WS_TABSTOP,170,41,50,14,psh1,"&Setup...");
    return end(t);
}
/* A block of global memory for a result: the program's own if it is large enough. */
static HGLOBAL result_block(HGLOBAL h,SIZE_T size) {
    if(h && GlobalSize(h)>=size) return h;
    if(h) GlobalFree(h);
    return GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,(DWORD)size);
}
/* DEVNAMES, DEVMODE and the DC or IC asked for. Printing to a file names FILE: as the port. */
/* The DC comes first, so that a failure leaves the program's handles as they were. */
static BOOL print_result(PrintDialog *d) {
    LPPRINTDLG pd=d->pd; LPCSTR output=pd->Flags&PD_PRINTTOFILE?"FILE:":d->printer.port; DEVNAMES *n; DEVMODE *m; char *s; HDC dc=NULL;
    int a=lstrlen(d->printer.driver)+1,b=lstrlen(d->printer.device)+1,c=lstrlen(output)+1;
    if(pd->Flags&(PD_RETURNDC|PD_RETURNIC)) {
        dc=pd->Flags&PD_RETURNDC?CreateDC(d->printer.driver,d->printer.device,output,&d->dm)
                                :CreateIC(d->printer.driver,d->printer.device,output,&d->dm);
        if(!dc) {error_code=PDERR_CREATEICFAILURE; return FALSE;}
    }
    pd->hDevNames=result_block(pd->hDevNames,sizeof(DEVNAMES)+a+b+c);
    pd->hDevMode=result_block(pd->hDevMode,sizeof(DEVMODE));
    if(!pd->hDevNames || !pd->hDevMode || !(n=(DEVNAMES *)GlobalLock(pd->hDevNames))) {
        if(dc) DeleteDC(dc);
        error_code=CDERR_MEMALLOCFAILURE; return FALSE;
    }
    n->wDriverOffset=sizeof(DEVNAMES); n->wDeviceOffset=(WORD)(sizeof(DEVNAMES)+a); n->wOutputOffset=(WORD)(sizeof(DEVNAMES)+a+b);
    n->wDefault=(WORD)(d->is_default?DN_DEFAULTPRN:0);
    s=(char *)n; lstrcpy(s+n->wDriverOffset,d->printer.driver); lstrcpy(s+n->wDeviceOffset,d->printer.device); lstrcpy(s+n->wOutputOffset,output);
    GlobalUnlock(pd->hDevNames);
    if(!(m=(DEVMODE *)GlobalLock(pd->hDevMode))) {
        if(dc) DeleteDC(dc);
        error_code=CDERR_MEMLOCKFAILURE; return FALSE;
    }
    *m=d->dm; GlobalUnlock(pd->hDevMode);
    pd->hDC=dc;
    return TRUE;
}
static BOOL print_dialog(PrintDialog *d) {
    static Template t; LPPRINTDLG pd=d->pd; LPCDLGTEMPLATE dialog;
    d->has_default=default_printer(&d->fallback);
    d->count=list_printers(d->list,PRINTERS);
    if(pd->Flags&PD_RETURNDEFAULT) {
        if(pd->hDevMode || pd->hDevNames) {error_code=PDERR_RETDEFFAILURE; return FALSE;}
        if(!d->has_default) {error_code=PDERR_NODEFAULTPRN; return FALSE;}
        d->printer=d->fallback; d->is_default=TRUE;
        if(!driver_settings(&d->printer,&d->dm)) {error_code=PDERR_LOADDRVFAILURE; return FALSE;}
        return print_result(d);
    }
    /* The printer the program had, or the default. */
    if(!pd->hDevNames || !names_in(pd->hDevNames,&d->printer,&d->is_default)) {
        if(!d->has_default) {
            error_code=PDERR_NODEFAULTPRN;
            MessageBox(pd->hwndOwner,"No default printer.\nUse the Control Panel to install and select a default printer.","Print",MB_OK|MB_ICONEXCLAMATION);
            return FALSE;
        }
        d->printer=d->fallback; d->is_default=TRUE;
    }
    if(!driver_settings(&d->printer,&d->dm)) {error_code=PDERR_LOADDRVFAILURE; return FALSE;}
    if(pd->hDevMode && GlobalSize(pd->hDevMode)>=sizeof(DEVMODE)) {
        const DEVMODE *m=(const DEVMODE *)GlobalLock(pd->hDevMode);
        if(m) {
            if(m->dmFields&DM_ORIENTATION) d->dm.dmOrientation=m->dmOrientation;
            if(m->dmFields&DM_PAPERSIZE) d->dm.dmPaperSize=m->dmPaperSize;
            GlobalUnlock(pd->hDevMode);
        }
    }
    dialog=pd->Flags&PD_PRINTSETUP?setup_template(&t):print_template(&t);
    if(DialogBoxIndirectParam(module,dialog,pd->hwndOwner,pd->Flags&PD_PRINTSETUP?SetupProc:PrintProc,(LPARAM)d)!=IDOK) return FALSE;
    return print_result(d);
}
BOOL WINAPI PrintDlg(LPPRINTDLG pd) {
    PrintDialog *d; BOOL r;
    error_code=0;
    if(!pd || pd->lStructSize<sizeof(PRINTDLG)) {error_code=CDERR_STRUCTSIZE; return FALSE;}
    if(!(d=(PrintDialog *)GlobalAlloc(GMEM_FIXED|GMEM_ZEROINIT,sizeof(PrintDialog)))) {error_code=CDERR_MEMALLOCFAILURE; return FALSE;}
    d->pd=pd; r=print_dialog(d);
    GlobalFree(d);
    return r;
}
