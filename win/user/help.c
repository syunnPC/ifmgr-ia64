/* SPDX-License-Identifier: GPL-2.0-or-later
 * WinHelp: the help program is WINHELP.EXE, its window of class MS_WINHELP.
 * A request goes to it (started first if it is not running) as the
 * registered message WM_WINHELP with a global block: a WINHLP, the help
 * file's name and the request's data.
 */
#include "user.h"

BOOL WINAPI WinHelp(HWND owner,LPCSTR file,UINT command,ULONG_PTR data) {
    static UINT message; HWND help=FindWindow("MS_WINHELP",NULL); HGLOBAL g; WINHLP *w; BYTE *p;
    DWORD file_bytes=file?(DWORD)lstrlen(file)+1:0,data_bytes=0; const void *extra=NULL; int i;
    if(!message) message=RegisterWindowMessage("WM_WINHELP");
    if(!help) {
        if(command==HELP_QUIT) return TRUE;
        if(WinExec("WINHELP.EXE",SW_SHOWNORMAL)<32) return FALSE;
        /* The new task makes its window when it runs. */
        for(i=0;i<500 && !(help=FindWindow("MS_WINHELP",NULL));i++) Yield();
        if(!help) return FALSE;
    }
    switch(command) {
    case HELP_KEY: case HELP_PARTIALKEY: case HELP_COMMAND:
        if(data) {extra=(const void *)data; data_bytes=(DWORD)lstrlen((LPCSTR)data)+1;}
        break;
    case HELP_MULTIKEY:
        if(data) {extra=(const void *)data; data_bytes=((const MULTIKEYHELP *)data)->mkSize;}
        break;
    }
    if(sizeof(WINHLP)+file_bytes+data_bytes>0xffff || !(g=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,sizeof(WINHLP)+file_bytes+data_bytes))) return FALSE;
    if(!(w=(WINHLP *)GlobalLock(g))) {GlobalFree(g); return FALSE;}
    memset(w,0,sizeof(*w)); p=(BYTE *)w;
    w->cbData=(WORD)(sizeof(WINHLP)+file_bytes+data_bytes); w->usCommand=(WORD)command; w->ulTopic=extra?0:(DWORD)data;
    if(file) {w->ofsHelpFile=sizeof(WINHLP); memcpy(p+sizeof(WINHLP),file,file_bytes);}
    if(extra) {w->ofsData=(WORD)(sizeof(WINHLP)+file_bytes); memcpy(p+w->ofsData,extra,data_bytes);}
    GlobalUnlock(g);
    SendMessage(help,message,(WPARAM)owner,(LPARAM)g);
    GlobalFree(g);
    return TRUE;
}
