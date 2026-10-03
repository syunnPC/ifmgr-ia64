/* SPDX-License-Identifier: GPL-2.0-or-later
 * WINAPP.LIB: the File Open and Save As dialogs (built in memory as DIALOG
 * templates, as Print's Cancel box is too) and whole-file helpers.
 */
#include "winapp.h"
#define IDC_NAME 1100
#define IDC_DIR 1101
#define IDC_FILES 1102
#define IDC_DIRS 1103
#define IDC_FILES_LABEL 1104

typedef struct {LPCSTR title,spec; LPSTR path; int size; BOOL save; char current_spec[64];} FileDialog;

LPCSTR FileTitle(LPCSTR path) {
    LPCSTR p=path,base=path;
    for(;p && *p;p++) if(*p=='\\' || *p==':') base=p+1;
    return base;
}
LPSTR ReadWholeFile(LPCSTR path,DWORD *size) {
    HFILE h=_lopen(path,OF_READ); LONG n; LPSTR text;
    if(h==HFILE_ERROR) return NULL;
    n=_llseek(h,0,2); _llseek(h,0,0);
    if(n<0 || !(text=(LPSTR)GlobalAlloc(GPTR,(DWORD)n+1))) {_lclose(h); return NULL;}
    if(n && _lread(h,text,(UINT)n)!=(UINT)n) {GlobalFree(text); _lclose(h); return NULL;}
    _lclose(h);
    text[n]=0;
    if(size) *size=(DWORD)n;
    return text;
}
BOOL WriteWholeFile(LPCSTR path,const void *data,DWORD size) {
    HFILE h=_lcreat(path,0); BOOL ok;
    if(h==HFILE_ERROR) return FALSE;
    ok=!size || _lwrite(h,data,(UINT)size)==(UINT)size;
    return _lclose(h)!=HFILE_ERROR && ok;
}

/* --- the dialog template ---------------------------------------------------------- */
WORD *TemplateText(WORD *p,LPCSTR s) {while(*s) *p++=(BYTE)*s++; *p++=0; return p;}
static WORD *align(WORD *p,const void *base) {
    while(((BYTE *)p-(BYTE *)base)&3) *p++=0;
    return p;
}
WORD *TemplateItem(WORD *p,const void *base,DWORD style,int x,int y,int cx,int cy,WORD id,WORD atom,LPCSTR text) {
    p=align(p,base);
    *(DWORD *)p=style|WS_CHILD|WS_VISIBLE; p+=2;
    *(DWORD *)p=0; p+=2;
    *p++=(WORD)x; *p++=(WORD)y; *p++=(WORD)cx; *p++=(WORD)cy; *p++=id;
    *p++=0xffff; *p++=atom;
    p=TemplateText(p,text);
    *p++=0;
    return p;
}
static void build(DWORD *buffer,LPCSTR title,BOOL save) {
    WORD *p=(WORD *)buffer;
    *(DWORD *)p=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME; p+=2;
    *(DWORD *)p=0; p+=2;
    *p++=10; *p++=10; *p++=10; *p++=180; *p++=124;
    *p++=0; *p++=0; p=TemplateText(p,title);
    p=TemplateItem(p,buffer,SS_LEFT,6,6,106,10,(WORD)-1,0x82,save?"Save File &Name As:":"File&name:");
    p=TemplateItem(p,buffer,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP|WS_GROUP,6,17,106,12,IDC_NAME,0x81,"");
    p=TemplateItem(p,buffer,SS_LEFT,6,32,40,10,(WORD)-1,0x82,"Directory:");
    p=TemplateItem(p,buffer,SS_LEFT|SS_NOPREFIX,48,32,70,10,IDC_DIR,0x82,"");
    p=TemplateItem(p,buffer,SS_LEFT,6,45,50,10,IDC_FILES_LABEL,0x82,"&Files:");
    p=TemplateItem(p,buffer,LBS_STANDARD|WS_TABSTOP|WS_GROUP,6,56,62,64,IDC_FILES,0x83,"");
    p=TemplateItem(p,buffer,SS_LEFT,74,45,50,10,(WORD)-1,0x82,"&Directories:");
    p=TemplateItem(p,buffer,LBS_STANDARD|WS_TABSTOP|WS_GROUP,74,56,62,64,IDC_DIRS,0x83,"");
    p=TemplateItem(p,buffer,BS_DEFPUSHBUTTON|WS_TABSTOP|WS_GROUP,124,6,50,14,IDOK,0x80,save?"OK":"&Open");
    p=TemplateItem(p,buffer,BS_PUSHBUTTON|WS_TABSTOP,124,24,50,14,IDCANCEL,0x80,"Cancel");
}

/* --- behaviour ---------------------------------------------------------------------- */
static BOOL wildcard(LPCSTR s) {for(;*s;s++) if(*s=='*' || *s=='?') return TRUE; return FALSE;}
static void fill(HWND dlg,FileDialog *f) {
    char spec[64],dirs[8]="*.*";
    lstrcpy(spec,f->current_spec);
    DlgDirList(dlg,spec,IDC_FILES,IDC_DIR,0);
    DlgDirList(dlg,dirs,IDC_DIRS,0,DDL_DIRECTORY|DDL_DRIVES|DDL_EXCLUSIVE);
}
/* The name typed: a spec lists, a directory changes to it, a file ends the dialog. */
static BOOL accept(HWND dlg,FileDialog *f) {
    char name[128],full[260]; DWORD attrs; int n;
    GetDlgItemText(dlg,IDC_NAME,name,sizeof(name));
    if(!name[0]) return FALSE;
    if(wildcard(name)) {
        LPCSTR base=FileTitle(name);
        if(base!=name) {char dir[128]; lstrcpyn(dir,name,(int)(base-name)+1); if(lstrlen(dir)>3 && dir[lstrlen(dir)-1]=='\\') dir[lstrlen(dir)-1]=0; SetCurrentDirectory(dir);}
        lstrcpyn(f->current_spec,base,sizeof(f->current_spec));
        fill(dlg,f); SetDlgItemText(dlg,IDC_NAME,f->current_spec);
        return FALSE;
    }
    attrs=GetFileAttributes(name);
    if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_DIRECTORY)) {
        SetCurrentDirectory(name); fill(dlg,f); SetDlgItemText(dlg,IDC_NAME,f->current_spec);
        return FALSE;
    }
    if(name[1]==':' || name[0]=='\\') lstrcpy(full,name);
    else {
        GetCurrentDirectory(sizeof(full),full); n=lstrlen(full);
        if(n && full[n-1]!='\\') lstrcat(full,"\\");
        lstrcat(full,name);
    }
    /* A name without an extension takes the spec's. */
    if(!wildcard(f->current_spec) || f->current_spec[0]!='*' || f->current_spec[1]!='.') {}
    else {
        LPCSTR base=FileTitle(full); const char *dot=base; BOOL has=FALSE;
        for(;*dot;dot++) if(*dot=='.') has=TRUE;
        if(!has && lstrcmp(f->current_spec,"*.*")) lstrcat(full,f->current_spec+1);
    }
    if(!f->save && GetFileAttributes(full)==INVALID_FILE_ATTRIBUTES) {
        char text[300]; wsprintf(text,"Cannot find file %s.",full);
        MessageBox(dlg,text,f->title,MB_OK|MB_ICONEXCLAMATION);
        return FALSE;
    }
    if(f->save && GetFileAttributes(full)!=INVALID_FILE_ATTRIBUTES) {
        char text[300]; wsprintf(text,"Replace existing %s?",full);
        if(MessageBox(dlg,text,f->title,MB_YESNO|MB_ICONQUESTION)!=IDYES) return FALSE;
    }
    lstrcpyn(f->path,full,f->size);
    AnsiUpper(f->path);
    return TRUE;
}
static INT_PTR CALLBACK DialogProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    FileDialog *f=(FileDialog *)GetProp(dlg,"FileDialog");
    switch(msg) {
    case WM_INITDIALOG: {
        LPCSTR initial;
        f=(FileDialog *)lp; SetProp(dlg,"FileDialog",(HANDLE)f);
        initial=f->path[0]?f->path:f->spec;
        if(wildcard(initial)) lstrcpyn(f->current_spec,FileTitle(initial),sizeof(f->current_spec));
        else lstrcpyn(f->current_spec,f->spec?f->spec:"*.*",sizeof(f->current_spec));
        if(FileTitle(initial)!=initial) {
            char dir[128]; lstrcpyn(dir,initial,(int)(FileTitle(initial)-initial)+1);
            if(lstrlen(dir)>3 && dir[lstrlen(dir)-1]=='\\') dir[lstrlen(dir)-1]=0;
            SetCurrentDirectory(dir);
        }
        fill(dlg,f);
        SetDlgItemText(dlg,IDC_NAME,wildcard(initial)?f->current_spec:FileTitle(initial));
        SendDlgItemMessage(dlg,IDC_NAME,EM_SETSEL,0,-1);
        return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case IDOK: if(accept(dlg,f)) EndDialog(dlg,IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL); return TRUE;
        case IDC_FILES:
            if(HIWORD(wp)==LBN_SELCHANGE || HIWORD(wp)==LBN_DBLCLK) {
                char name[64]; DlgDirSelect(dlg,name,IDC_FILES); SetDlgItemText(dlg,IDC_NAME,name);
                if(HIWORD(wp)==LBN_DBLCLK && accept(dlg,f)) EndDialog(dlg,IDOK);
            }
            return TRUE;
        case IDC_DIRS:
            if(HIWORD(wp)==LBN_DBLCLK) {
                char name[64];
                if(DlgDirSelect(dlg,name,IDC_DIRS)) {SetCurrentDirectory(name); fill(dlg,f); SetDlgItemText(dlg,IDC_NAME,f->current_spec);}
            }
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY: RemoveProp(dlg,"FileDialog"); return FALSE;
    }
    return FALSE;
}
static BOOL run(HWND owner,LPCSTR title,LPCSTR spec,LPSTR path,int size,BOOL save) {
    static DWORD buffer[512]; FileDialog f; HINSTANCE instance=(HINSTANCE)GetWindowLongPtr(owner,GWL_HINSTANCE);
    f.title=title; f.spec=spec?spec:"*.*"; f.path=path; f.size=size; f.save=save;
    memset(buffer,0,sizeof(buffer));
    build(buffer,title,save);
    if(!path) return FALSE;
    return DialogBoxIndirectParam(instance,(LPCDLGTEMPLATE)buffer,owner,DialogProc,(LPARAM)&f)==IDOK;
}
BOOL FileOpenDialog(HWND owner,LPCSTR title,LPCSTR spec,LPSTR path,int size) {return run(owner,title,spec,path,size,FALSE);}
BOOL FileSaveDialog(HWND owner,LPCSTR title,LPCSTR spec,LPSTR path,int size) {return run(owner,title,spec,path,size,TRUE);}
