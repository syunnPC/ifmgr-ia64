/* SPDX-License-Identifier: GPL-2.0-or-later
 * The clipboard: one set of data handles shared by all tasks, the owner and
 * the window that has it open, registered formats and the viewer chain.
 * Memory handles should be allocated with GMEM_DDESHARE so they outlive
 * the task that put them there.
 */
#include "user.h"
#define FORMATS 32
#define NAMES 64

static struct {UINT format; HANDLE data;} formats[FORMATS];
static int count;
static HWND owner,opener,viewer;
static BOOL changed;
static char names[NAMES][32];
static int named;

static BOOL gdi_format(UINT format) {
    return format==CF_BITMAP || format==CF_DSPBITMAP || format==CF_PALETTE || (format>=CF_GDIOBJFIRST && format<=CF_GDIOBJLAST);
}
/* A METAFILEPICT's metafile, which goes with it. */
static HMETAFILE picture_metafile(UINT format,HANDLE h) {
    const METAFILEPICT *p; HMETAFILE mf=NULL;
    if((format!=CF_METAFILEPICT && format!=CF_DSPMETAFILEPICT) || !h || GlobalSize(h)<sizeof(METAFILEPICT)) return NULL;
    if((p=(const METAFILEPICT *)GlobalLock(h))!=NULL) {mf=p->hMF; GlobalUnlock(h);}
    return mf;
}
static void free_data(UINT format,HANDLE h) {
    HMETAFILE mf;
    if(!h) return;
    if(gdi_format(format)) DeleteObject(h);
    else {
        if((mf=picture_metafile(format,h))!=NULL) DeleteMetaFile(mf);
        GlobalFree(h);
    }
}
/* GDI objects on the clipboard (a picture's metafile too) belong to no task:
 * they stay when the program that put them there ends. */
static void keep_data(UINT format,HANDLE h) {
    HMETAFILE mf;
    if(gdi_format(format)) GdiSetOwner(h,NULL);
    else if((mf=picture_metafile(format,h))!=NULL) GdiSetOwner(mf,NULL);
}
BOOL WINAPI OpenClipboard(HWND h) {
    if(opener && opener!=h && IsWindow(opener)) return FALSE;
    opener=h?h:(HWND)1;
    return TRUE;
}
BOOL WINAPI CloseClipboard(void) {
    if(!opener) return FALSE;
    opener=NULL;
    if(changed && viewer && IsWindow(viewer)) SendMessage(viewer,WM_DRAWCLIPBOARD,0,0);
    changed=FALSE;
    return TRUE;
}
BOOL WINAPI EmptyClipboard(void) {
    int i;
    if(!opener) return FALSE;
    if(owner && owner!=opener && IsWindow(owner)) SendMessage(owner,WM_DESTROYCLIPBOARD,0,0);
    for(i=0;i<count;i++) free_data(formats[i].format,formats[i].data);
    count=0;
    owner=opener==(HWND)1?NULL:opener;
    changed=TRUE;
    return TRUE;
}
HANDLE WINAPI SetClipboardData(UINT format,HANDLE data) {
    int i;
    if(!opener || !format) return NULL;
    if(data) keep_data(format,data);
    for(i=0;i<count;i++) if(formats[i].format==format) {if(formats[i].data!=data) free_data(format,formats[i].data); formats[i].data=data; changed=TRUE; return data;}
    if(count==FORMATS) return NULL;
    formats[count].format=format; formats[count].data=data; count++;
    changed=TRUE;
    return data;
}
HANDLE WINAPI GetClipboardData(UINT format) {
    int i;
    if(!opener) return NULL;
    for(i=0;i<count;i++) if(formats[i].format==format) {
        /* Delayed rendering: the owner supplies the data now. */
        if(!formats[i].data && owner && IsWindow(owner)) SendMessage(owner,WM_RENDERFORMAT,format,0);
        return formats[i].data;
    }
    return NULL;
}
BOOL WINAPI IsClipboardFormatAvailable(UINT format) {
    int i;
    for(i=0;i<count;i++) if(formats[i].format==format) return TRUE;
    return FALSE;
}
UINT WINAPI EnumClipboardFormats(UINT format) {
    int i;
    if(!opener) return 0;
    if(!format) return count?formats[0].format:0;
    for(i=0;i+1<count;i++) if(formats[i].format==format) return formats[i+1].format;
    return 0;
}
int WINAPI CountClipboardFormats(void) {return count;}
UINT WINAPI RegisterClipboardFormat(LPCSTR name) {
    int i;
    if(!name || !name[0]) return 0;
    for(i=0;i<named;i++) if(!lstrcmpi(names[i],name)) return 0xc000+i;
    if(named==NAMES) return 0;
    lstrcpyn(names[named],name,sizeof(names[0]));
    return 0xc000+named++;
}
int WINAPI GetClipboardFormatName(UINT format,LPSTR out,int size) {
    if(format<0xc000 || format>=0xc000+(UINT)named || !out || size<=0) return 0;
    lstrcpyn(out,names[format-0xc000],size);
    return lstrlen(out);
}
HWND WINAPI GetClipboardOwner(void) {return owner;}
HWND WINAPI GetOpenClipboardWindow(void) {return opener==(HWND)1?NULL:opener;}
HWND WINAPI SetClipboardViewer(HWND h) {
    HWND old=viewer;
    viewer=h;
    if(h) SendMessage(h,WM_DRAWCLIPBOARD,0,0);
    return old;
}
HWND WINAPI GetClipboardViewer(void) {return viewer;}
BOOL WINAPI ChangeClipboardChain(HWND remove,HWND next) {
    if(remove==viewer) {viewer=next; return TRUE;}
    if(viewer && IsWindow(viewer)) return (BOOL)SendMessage(viewer,WM_CHANGECBCHAIN,(WPARAM)remove,(LPARAM)next);
    return FALSE;
}
void ClipboardWindowGone(Wnd *w) {
    if(opener==w->handle) opener=NULL;
    if(owner==w->handle) owner=NULL;
    if(viewer==w->handle) viewer=NULL;
}
