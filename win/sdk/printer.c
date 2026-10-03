/* SPDX-License-Identifier: GPL-2.0-or-later
 * WINAPP.LIB: printing for the accessories. The printer is the one chosen
 * in Print Setup (COMMDLG's, as Windows 3.1's accessories have it) or the
 * default; while a document prints, a box with Cancel stays up and the
 * abort procedure lets the program's other windows have their messages.
 */
#include "winapp.h"
#define IDC_DOCUMENT 1200

static HGLOBAL dev_mode,dev_names; /* Print Setup's choice, kept for the program */
static HWND cancel_box,cancel_owner;
static BOOL cancelled;

/* The chosen printer's DC, or the default's; NULL (and a message) without one. */
HDC PrinterDC(HWND owner) {
    PRINTDLG pd; HDC dc=NULL;
    if(dev_names) {
        const DEVNAMES *n=(const DEVNAMES *)GlobalLock(dev_names); const DEVMODE *m=dev_mode?(const DEVMODE *)GlobalLock(dev_mode):NULL;
        if(n) {dc=CreateDC((LPCSTR)n+n->wDriverOffset,(LPCSTR)n+n->wDeviceOffset,(LPCSTR)n+n->wOutputOffset,m); GlobalUnlock(dev_names);}
        if(m) GlobalUnlock(dev_mode);
        if(dc) return dc;
    }
    memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=owner; pd.Flags=PD_RETURNDEFAULT|PD_RETURNDC;
    if(PrintDlg(&pd)) {
        if(pd.hDevMode) GlobalFree(pd.hDevMode);
        if(pd.hDevNames) GlobalFree(pd.hDevNames);
        return pd.hDC;
    }
    MessageBox(owner,"There is no printer. Use the Control Panel to install and select a default printer.",
               "Print",MB_OK|MB_ICONEXCLAMATION);
    return NULL;
}
void PrinterSetup(HWND owner) {
    PRINTDLG pd;
    memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=owner; pd.Flags=PD_PRINTSETUP;
    pd.hDevMode=dev_mode; pd.hDevNames=dev_names;
    if(PrintDlg(&pd)) {dev_mode=pd.hDevMode; dev_names=pd.hDevNames;}
}

/* --- the Cancel box ------------------------------------------------------------------ */
static INT_PTR CALLBACK CancelProc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) return TRUE;
    if(msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) {
        cancelled=TRUE; EnableWindow(GetDlgItem(dlg,IDCANCEL),FALSE);
        return TRUE;
    }
    return FALSE;
}
static BOOL CALLBACK AbortProc(HDC dc,int code) {
    MSG m;
    (void)dc; (void)code;
    while(!cancelled && PeekMessage(&m,NULL,0,0,PM_REMOVE))
        if(!cancel_box || !IsDialogMessage(cancel_box,&m)) {TranslateMessage(&m); DispatchMessage(&m);}
    return !cancelled;
}
BOOL PrintCancelled(void) {return cancelled;}
/* The document starts: the owner is disabled while the Cancel box is up.
 * A failure to start has been reported (or the user cancelled). */
BOOL PrintStart(HDC dc,HWND owner,LPCSTR program,LPCSTR document) {
    static DWORD buffer[256]; WORD *p=(WORD *)buffer; DOCINFO doc; char title[128]; int r;
    HINSTANCE instance=(HINSTANCE)GetWindowLongPtr(owner,GWL_HINSTANCE);
    memset(buffer,0,sizeof(buffer));
    *(DWORD *)p=WS_POPUP|WS_VISIBLE|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME; p+=2;
    *(DWORD *)p=0; p+=2;
    *p++=3; *p++=40; *p++=40; *p++=140; *p++=54;
    *p++=0; *p++=0; p=TemplateText(p,program);
    p=TemplateItem(p,buffer,SS_CENTER,4,6,132,10,(WORD)-1,0x82,"Now Printing");
    p=TemplateItem(p,buffer,SS_CENTER|SS_NOPREFIX,4,18,132,10,IDC_DOCUMENT,0x82,document);
    p=TemplateItem(p,buffer,BS_DEFPUSHBUTTON|WS_TABSTOP,45,34,50,14,IDCANCEL,0x80,"Cancel");
    cancelled=FALSE; cancel_owner=owner;
    cancel_box=CreateDialogIndirect(instance,(LPCDLGTEMPLATE)buffer,owner,CancelProc);
    EnableWindow(owner,FALSE);
    SetAbortProc(dc,AbortProc);
    wsprintf(title,"%s - %s",program,document);
    memset(&doc,0,sizeof(doc)); doc.cbSize=sizeof(doc); doc.lpszDocName=title;
    if((r=StartDoc(dc,&doc))>0) return TRUE;
    PrintEnd(dc,FALSE);
    if(r!=SP_USERABORT && r!=SP_APPABORT) {
        wsprintf(title,"Cannot print %s. Be sure the printer is connected and set up properly.",document);
        MessageBox(owner,title,program,MB_OK|MB_ICONEXCLAMATION);
    }
    return FALSE;
}
/* Ends the document (or abandons it), takes the Cancel box down and gives
 * the printer DC back. */
void PrintEnd(HDC dc,BOOL ok) {
    if(ok && !cancelled) EndDoc(dc); else AbortDoc(dc);
    if(cancel_owner) EnableWindow(cancel_owner,TRUE);
    if(cancel_box) DestroyWindow(cancel_box);
    cancel_box=NULL; cancel_owner=NULL;
    DeleteDC(dc);
}
/* COMMDLG's Print dialog for the chosen printer (or the default): pages
 * first_page to last_page offered; the range and the copies come back, and
 * the printer's DC, NULL when cancelled. */
HDC PrintDialogDC(HWND owner,int first_page,int last_page,int *from,int *to,int *copies) {
    PRINTDLG pd;
    memset(&pd,0,sizeof(pd)); pd.lStructSize=sizeof(pd); pd.hwndOwner=owner; pd.Flags=PD_RETURNDC|PD_NOSELECTION;
    pd.hDevMode=dev_mode; pd.hDevNames=dev_names;
    pd.nMinPage=(WORD)first_page; pd.nMaxPage=(WORD)last_page; pd.nFromPage=(WORD)first_page; pd.nToPage=(WORD)last_page; pd.nCopies=1;
    if(!PrintDlg(&pd)) return NULL;
    dev_mode=pd.hDevMode; dev_names=pd.hDevNames;
    *from=pd.Flags&PD_PAGENUMS?pd.nFromPage:first_page; *to=pd.Flags&PD_PAGENUMS?pd.nToPage:last_page;
    *copies=pd.nCopies?pd.nCopies:1;
    return pd.hDC;
}
