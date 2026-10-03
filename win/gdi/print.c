/* SPDX-License-Identifier: GPL-2.0-or-later
 * The printer driver, PSCRIPT: a PostScript printer driven as a raster. A
 * printer DC draws on a page in memory, the paper's printable area at 150
 * dots per inch; when a page ends it goes out as one PostScript Level 2
 * image (run-length encoded, then ASCII85): one bit per dot when the page is
 * only black and white, indexed when it has 256 colors or fewer, RGB
 * otherwise. Output goes to the port: a device (LPT1:, COM1:), a file named
 * as the port or by StartDoc, or FILE:, which asks for a file name through
 * USER. Windows 3.0's printing escapes and Windows 3.1's StartDoc functions
 * both drive it; the abort procedure is called before each page and as it
 * is sent.
 */
#include "gdip.h"
#define DPI 150
#define MARGIN 18 /* points at each edge */
#define BUFFER 4096
#define SLOTS 1024 /* the color hash */

static const struct {short id,width,height; const char *name;} papers[]={ /* points, upright */
    {DMPAPER_LETTER,612,792,"Letter 8 1/2 x 11 in"},{DMPAPER_LEGAL,612,1008,"Legal 8 1/2 x 14 in"},
    {DMPAPER_EXECUTIVE,522,756,"Executive 7 1/4 x 10 1/2 in"},{DMPAPER_A4,595,842,"A4 210 x 297 mm"},
    {DMPAPER_A5,420,595,"A5 148 x 210 mm"}};
#define PAPERS (sizeof(papers)/sizeof(papers[0]))
typedef struct {int paper,orientation,copies;} Settings;

struct Printer {
    BOOL info; /* CreateIC: settings only, no page */
    char device[CCHDEVICENAME],port[MAX_PATH],path[MAX_PATH],target[MAX_PATH],title[64];
    int paper,orientation,copies;
    POINT points;            /* the paper in points, upright */
    POINT size,offset,area;  /* the paper, the printable area's corner and size, in dots as the page lies */
    Surface page;
    ABORTPROC abort;
    BOOL in_doc,band,file,failed,busy,spooled;
    HFILE out; int pages; DWORD written;
    BYTE buffer[BUFFER]; UINT used;
    DWORD tuple; int tuple_bytes,column; /* ASCII85 */
    DWORD keys[SLOTS],colors[256]; BYTE indexes[SLOTS]; int count; /* the page's colors */
};
static GDIFILEPROMPT file_prompt;
void WINAPI GdiSetFilePrompt(GDIFILEPROMPT prompt) {file_prompt=prompt;}

/* --- the spooler ------------------------------------------------------------------ */
/* With WIN.INI's spooler= not no, a document for a device port goes to a
 * file in TEMP; when it ends it waits in this queue, and USER's notifier
 * tells Print Manager (starting it if need be), which sends it to the port
 * in its own task and ends the job. Without Print Manager the document goes
 * to the port at once. */
#define JOBS 32
static SPOOLJOB jobs[JOBS]; static BOOL job_used[JOBS]; static DWORD next_job=1;
static GDISPOOLNOTIFY spool_notify;
void WINAPI GdiSetSpoolNotify(GDISPOOLNOTIFY notify) {spool_notify=notify;}
int WINAPI GdiSpoolJobs(SPOOLJOB FAR *out,int max) {
    int i,n=0; DWORD id,last=0;
    /* In the order they came. */
    for(;n<max;n++) {
        int at=-1;
        for(i=0;i<JOBS;i++) if(job_used[i] && jobs[i].id>last && (at<0 || jobs[i].id<jobs[at].id)) at=i;
        if(at<0) break;
        if(out) out[n]=jobs[at];
        id=jobs[at].id; last=id;
    }
    return n;
}
BOOL WINAPI GdiEndSpoolJob(DWORD id) {
    int i;
    for(i=0;i<JOBS;i++) if(job_used[i] && jobs[i].id==id) {job_used[i]=FALSE; return TRUE;}
    return FALSE;
}
static BOOL spooling(void) {
    char value[8];
    GetProfileString("windows","spooler","yes",value,sizeof(value));
    return lstrcmpi(value,"no")!=0;
}
/* Without Print Manager: the spool file to the port now, in the printing task. */
static void send_now(LPCSTR path,LPCSTR port) {
    HFILE in=_lopen(path,OF_READ),out=_lopen(port,OF_WRITE); BYTE buffer[1024]; UINT n;
    if(in!=HFILE_ERROR && out!=HFILE_ERROR)
        while((n=_lread(in,buffer,sizeof(buffer)))!=0 && n!=(UINT)HFILE_ERROR) if(_lwrite(out,buffer,n)!=n) break;
    if(in!=HFILE_ERROR) _lclose(in);
    if(out!=HFILE_ERROR) _lclose(out);
    DeleteFile(path);
}
static void queue_job(Printer *p) {
    int i;
    for(i=0;i<JOBS && job_used[i];i++) {}
    if(i<JOBS) {
        SPOOLJOB *j=&jobs[i];
        memset(j,0,sizeof(*j));
        j->id=next_job++; j->size=p->written; GetLocalTime(&j->sent);
        lstrcpyn(j->device,p->device,sizeof(j->device)); lstrcpyn(j->port,p->target,sizeof(j->port));
        lstrcpyn(j->document,p->title,sizeof(j->document)); lstrcpyn(j->path,p->path,sizeof(j->path));
        job_used[i]=TRUE;
        if(spool_notify && spool_notify()) return;
        job_used[i]=FALSE;
    }
    send_now(p->path,p->target);
}

static Printer *printer_of(HDC h) {DC *dc=dc_of(h); return dc?dc->printer:NULL;}
static void blank(Surface *s) {DWORD i,n=(DWORD)s->width*(DWORD)s->height; for(i=0;i<n;i++) s->bits[i]=0xffffff;}
static BOOL is_blank(const Surface *s) {
    DWORD i,n=(DWORD)s->width*(DWORD)s->height;
    for(i=0;i<n;i++) if((s->bits[i]&0xffffff)!=0xffffff) return FALSE;
    return TRUE;
}

/* --- settings ------------------------------------------------------------------ */
/* A printer's settings live in WIN.INI (or the profile given) in the section
 * [PSCRIPT,port], the port without its colon, as paper, orient and copies. */
static void section_of(char *out,LPCSTR port) {
    int n;
    lstrcpy(out,"PSCRIPT,"); lstrcpyn(out+8,port && *port?port:"LPT1:",MAX_PATH);
    n=lstrlen(out);
    if(out[n-1]==':') out[n-1]=0;
}
static int paper_index(int id) {int i; for(i=0;i<(int)PAPERS && papers[i].id!=id;i++) {} return i<(int)PAPERS?i:0;}
static void read_settings(Settings *s,LPCSTR port,LPCSTR profile,const DEVMODE FAR *dm) {
    char section[MAX_PATH+16]; LPCSTR file=profile?profile:"WIN.INI";
    section_of(section,port);
    s->paper=(int)GetPrivateProfileInt(section,"paper",DMPAPER_LETTER,file);
    s->orientation=(int)GetPrivateProfileInt(section,"orient",DMORIENT_PORTRAIT,file);
    s->copies=(int)GetPrivateProfileInt(section,"copies",1,file);
    if(dm) {
        if(dm->dmFields&DM_PAPERSIZE) s->paper=dm->dmPaperSize;
        if(dm->dmFields&DM_ORIENTATION) s->orientation=dm->dmOrientation;
        if(dm->dmFields&DM_COPIES) s->copies=dm->dmCopies;
    }
    s->paper=papers[paper_index(s->paper)].id;
    if(s->orientation!=DMORIENT_LANDSCAPE) s->orientation=DMORIENT_PORTRAIT;
    if(s->copies<1) s->copies=1;
    if(s->copies>99) s->copies=99;
}
static void write_number(LPCSTR section,LPCSTR key,int v,LPCSTR file) {
    char text[8]; int i=sizeof(text)-1;
    text[i]=0;
    do {text[--i]=(char)('0'+v%10); v/=10;} while(v);
    WritePrivateProfileString(section,key,text+i,file);
}
/* PSCRIPT's ExtDeviceMode: with no mode the size of its DEVMODE; otherwise
 * the settings, changed by the DEVMODE given (DM_MODIFY), copied out
 * (DM_COPY) and kept (DM_UPDATE). It has no dialog of its own (DM_PROMPT). */
int WINAPI ExtDeviceMode(HWND owner,HANDLE driver,LPDEVMODE out,LPSTR device,LPSTR port,LPDEVMODE in,LPSTR profile,WORD mode) {
    Settings s; char section[MAX_PATH+16];
    (void)owner; (void)driver;
    if(!mode) return sizeof(DEVMODE);
    read_settings(&s,port,profile,mode&DM_MODIFY?in:NULL);
    if((mode&DM_COPY) && out) {
        memset(out,0,sizeof(*out));
        lstrcpyn(out->dmDeviceName,device?device:"PostScript Printer",CCHDEVICENAME);
        out->dmSpecVersion=DM_SPECVERSION; out->dmDriverVersion=0x300; out->dmSize=sizeof(*out);
        out->dmFields=DM_ORIENTATION|DM_PAPERSIZE|DM_COPIES|DM_COLOR;
        out->dmOrientation=(short)s.orientation; out->dmPaperSize=(short)s.paper; out->dmCopies=(short)s.copies;
        out->dmColor=DMCOLOR_COLOR; out->dmScale=100;
    }
    if(mode&DM_UPDATE) {
        LPCSTR file=profile?profile:"WIN.INI";
        section_of(section,port);
        write_number(section,"paper",s.paper,file);
        write_number(section,"orient",s.orientation,file);
        write_number(section,"copies",s.copies,file);
    }
    return IDOK;
}
/* PSCRIPT's DeviceCapabilities: the papers (their ids, names and sizes in
 * tenths of a millimetre), copies, orientation and the DEVMODE. */
DWORD WINAPI DeviceCapabilities(LPCSTR device,LPCSTR port,WORD index,LPSTR out,const DEVMODE FAR *dm) {
    int i;
    (void)device; (void)port; (void)dm;
    switch(index) {
    case DC_FIELDS: return DM_ORIENTATION|DM_PAPERSIZE|DM_COPIES|DM_COLOR;
    case DC_PAPERS:
        if(out) for(i=0;i<(int)PAPERS;i++) ((WORD FAR *)out)[i]=(WORD)papers[i].id;
        return PAPERS;
    case DC_PAPERNAMES:
        if(out) for(i=0;i<(int)PAPERS;i++) {memset(out+i*64,0,64); lstrcpyn(out+i*64,papers[i].name,64);}
        return PAPERS;
    case DC_PAPERSIZE:
        if(out) for(i=0;i<(int)PAPERS;i++) {
            ((POINT FAR *)out)[i].x=papers[i].width*254/72; ((POINT FAR *)out)[i].y=papers[i].height*254/72;
        }
        return PAPERS;
    case DC_SIZE: return sizeof(DEVMODE);
    case DC_EXTRA: return 0;
    case DC_VERSION: return DM_SPECVERSION;
    case DC_DRIVER: return 0x300;
    case DC_ORIENTATION: return 90;
    case DC_COPIES: return 99;
    }
    return (DWORD)-1;
}
/* The page from the settings: the paper as it lies, its printable area
 * inside a margin all round, at DPI. */
static void settings(Printer *p,LPCSTR port,const DEVMODE FAR *dm) {
    Settings s; int i,w,h;
    read_settings(&s,port,NULL,dm);
    p->paper=s.paper; p->orientation=s.orientation; p->copies=s.copies;
    i=paper_index(p->paper);
    p->points.x=papers[i].width; p->points.y=papers[i].height;
    w=p->points.x; h=p->points.y;
    if(p->orientation==DMORIENT_LANDSCAPE) {w=p->points.y; h=p->points.x;}
    p->size.x=(w*DPI+36)/72; p->size.y=(h*DPI+36)/72;
    p->area.x=(w-2*MARGIN)*DPI/72; p->area.y=(h-2*MARGIN)*DPI/72;
    p->offset.x=(p->size.x-p->area.x)/2; p->offset.y=(p->size.y-p->area.y)/2;
}
/* A port that is a device rather than a file: PRN, AUX, NUL, LPTn, COMn. */
static BOOL is_device(LPCSTR name) {
    char n[8]; int i;
    for(i=0;name[i] && name[i]!=':' && i<7;i++) n[i]=(char)(name[i]>='a' && name[i]<='z'?name[i]-0x20:name[i]);
    if(name[i] && name[i]!=':') return FALSE;
    if(name[i]==':' && name[i+1]) return FALSE;
    n[i]=0;
    if(!lstrcmp(n,"PRN") || !lstrcmp(n,"AUX") || !lstrcmp(n,"NUL")) return TRUE;
    return i==4 && (!memcmp(n,"LPT",3) || !memcmp(n,"COM",3)) && n[3]>='1' && n[3]<='9';
}
static BOOL is_pscript(LPCSTR driver) {
    static const char name[]="PSCRIPT"; int i;
    if(!driver) return FALSE;
    for(i=0;name[i];i++) if((driver[i]&~0x20)!=name[i]) return FALSE;
    return !driver[i] || driver[i]=='.';
}
HDC printer_dc(LPCSTR driver,LPCSTR device,LPCSTR port,const DEVMODE FAR *dm,BOOL info) {
    Printer *p; DC *dc; HDC h;
    if(!is_pscript(driver)) return NULL;
    if(!(p=(Printer *)gdi_alloc(sizeof(Printer)))) return NULL;
    p->info=info; p->out=HFILE_ERROR;
    lstrcpyn(p->port,port && *port?port:"LPT1:",sizeof(p->port));
    lstrcpyn(p->device,device && *device?device:"PostScript Printer",sizeof(p->device));
    settings(p,p->port,dm);
    if(!surface_alloc(&p->page,info?1:p->area.x,info?1:p->area.y,FALSE)) {gdi_free(p); return NULL;}
    blank(&p->page);
    if(!(dc=new_dc(&h))) {surface_free(&p->page); gdi_free(p); return NULL;}
    dc->printer=p; dc->dpi=DPI; dc->surface=&p->page; whole(dc);
    return h;
}

/* --- output ---------------------------------------------------------------------- */
static void flush(Printer *p) {
    if(p->used && !p->failed && _lwrite(p->out,p->buffer,p->used)!=p->used) p->failed=TRUE;
    p->written+=p->used; p->used=0;
}
static void put(Printer *p,const char *s,UINT n) {
    while(n && !p->failed) {
        UINT k=min(n,BUFFER-p->used);
        memcpy(p->buffer+p->used,s,k); p->used+=k; s+=k; n-=k;
        if(p->used==BUFFER) flush(p);
    }
}
/* v with the given number of decimals (v counts the last decimal's units). */
static void number(Printer *p,long v,int decimals) {
    char b[24]; int i=sizeof(b),d=0; BOOL negative=v<0;
    if(negative) v=-v;
    do {
        b[--i]=(char)('0'+v%10); v/=10;
        if(++d==decimals) b[--i]='.';
    } while(v || d<=decimals);
    if(negative) b[--i]='-';
    put(p,b+i,sizeof(b)-(UINT)i);
}
/* Text with %d (an int), %s (a string), %t (dots as points) and %%. */
static void print(Printer *p,const char *format,...) {
    va_list ap; const char *f;
    va_start(ap,format);
    for(f=format;*f;f++) {
        if(*f!='%') {put(p,f,1); continue;}
        switch(*++f) {
        case 'd': number(p,va_arg(ap,int),0); break;
        case 't': number(p,(long)va_arg(ap,int)*(7200/DPI),2); break;
        case 's': {const char *s=va_arg(ap,const char *); put(p,s,(UINT)lstrlen(s)); break;}
        default: put(p,f,1);
        }
    }
    va_end(ap);
}
static void a85_char(Printer *p,char c) {
    put(p,&c,1);
    if(++p->column==75) {put(p,"\n",1); p->column=0;}
}
static void a85_tuple(Printer *p) {
    DWORD v=p->tuple<<8*(4-p->tuple_bytes); char c[5]; int i;
    if(p->tuple_bytes==4 && !v) a85_char(p,'z');
    else {
        for(i=4;i>=0;i--) {c[i]=(char)('!'+v%85); v/=85;}
        for(i=0;i<=p->tuple_bytes;i++) a85_char(p,c[i]);
    }
    p->tuple=0; p->tuple_bytes=0;
}
static void a85_byte(Printer *p,BYTE b) {p->tuple=p->tuple<<8|b; if(++p->tuple_bytes==4) a85_tuple(p);}
static void a85_end(Printer *p) {if(p->tuple_bytes) a85_tuple(p); put(p,"~>\n",3); p->column=0;}
/* RunLengthDecode's runs: n+1 bytes as they are (n<128), or 257-n of one byte. */
static void run_length(Printer *p,const BYTE *b,int n) {
    int i=0,k;
    while(i<n) {
        int run=1,start=i;
        while(i+run<n && run<128 && b[i+run]==b[i]) run++;
        if(run>1) {a85_byte(p,(BYTE)(257-run)); a85_byte(p,b[i]); i+=run; continue;}
        while(i<n && i-start<128 && !(i+1<n && b[i+1]==b[i])) i++;
        a85_byte(p,(BYTE)(i-start-1));
        for(k=start;k<i;k++) a85_byte(p,b[k]);
    }
}

/* --- documents and pages ---------------------------------------------------------- */
static void end_job(Printer *p,BOOL abort) {
    if(p->out!=HFILE_ERROR) {
        if(!abort) flush(p);
        _lclose(p->out);
        if(abort && p->file) DeleteFile(p->path);
    }
    p->out=HFILE_ERROR; p->in_doc=p->band=FALSE; p->used=0;
    if(p->page.bits) blank(&p->page);
}
/* The abort procedure says whether to go on; the job ends if not. */
static BOOL go_on(HDC h,Printer *p) {
    BOOL go;
    if(!p->abort) return TRUE;
    p->busy=TRUE; go=p->abort(h,p->failed?SP_OUTOFDISK:0); p->busy=FALSE;
    if(!go) end_job(p,TRUE);
    return go;
}
static int start_doc(Printer *p,LPCSTR title,LPCSTR output) {
    char name[MAX_PATH]; LPCSTR target=output && *output?output:p->port; int i,n;
    if(p->info || p->in_doc) return SP_ERROR;
    if(!lstrcmpi(target,"FILE:")) {
        name[0]=0;
        if(!file_prompt || !file_prompt(name,sizeof(name)) || !name[0]) return SP_USERABORT;
        target=name;
    }
    /* A device is spooled when Print Manager can take it; one that cannot be
     * opened is an error now rather than in Print Manager. */
    p->spooled=FALSE; lstrcpyn(p->target,target,sizeof(p->target));
    if(!(output && *output) && is_device(target) && spool_notify && spooling()) {
        HFILE probe=_lopen(target,OF_WRITE);
        if(probe==HFILE_ERROR) return SP_ERROR;
        _lclose(probe);
        p->spooled=GetTempFileName(0,"SPL",0,p->path)!=0;
    }
    if(!p->spooled) lstrcpyn(p->path,target,sizeof(p->path));
    if((p->out=_lcreat(p->path,0))==HFILE_ERROR) return SP_ERROR;
    p->file=p->spooled || !is_device(p->path); p->in_doc=TRUE; p->band=p->failed=FALSE;
    p->pages=0; p->used=0; p->written=0; p->tuple=0; p->tuple_bytes=p->column=0;
    for(i=n=0;title && title[i] && n<(int)sizeof(p->title)-1;i++)
        if((BYTE)title[i]>=' ' && (BYTE)title[i]<0x7f) p->title[n++]=title[i];
    p->title[n]=0;
    blank(&p->page);
    print(p,"%%!PS-Adobe-3.0\n%%%%Creator: PSCRIPT\n%%%%Title: %s\n%%%%Pages: (atend)\n",p->title);
    print(p,"%%%%BoundingBox: 0 0 %d %d\n%%%%Orientation: %s\n",p->points.x,p->points.y,
          p->orientation==DMORIENT_LANDSCAPE?"Landscape":"Portrait");
    print(p,"%%%%DocumentData: Clean7Bit\n%%%%LanguageLevel: 2\n%%%%EndComments\n%%%%BeginSetup\n");
    print(p,"[{<< /PageSize [%d %d] /NumCopies %d >> setpagedevice} stopped cleartomark\n%%%%EndSetup\n",
          p->points.x,p->points.y,p->copies);
    return p->failed?SP_OUTOFDISK:1;
}
/* The page's colors, up to 256, for an indexed image: a hash of color to index. */
static int color_index(Printer *p,DWORD v) {
    DWORD slot=(v*2654435761U)>>22&(SLOTS-1);
    while(p->keys[slot]) {
        if(p->keys[slot]==(v|0x1000000)) return p->indexes[slot];
        slot=(slot+1)&(SLOTS-1);
    }
    if(p->count==256) return -1;
    p->keys[slot]=v|0x1000000; p->indexes[slot]=(BYTE)p->count; p->colors[p->count]=v;
    return p->count++;
}
static void hex(Printer *p,DWORD v) {
    static const char digits[]="0123456789ABCDEF"; char c[6]; int i;
    for(i=5;i>=0;i--) {c[i]=digits[v&15]; v>>=4;}
    put(p,c,6);
}
/* The page as one image, then a fresh page: one bit a dot when it is only
 * black and white, an index into its colors when it has 256 or fewer, RGB
 * otherwise. */
static int end_page(HDC h,Printer *p) {
    const Surface *s=&p->page; DWORD i,n=(DWORD)s->width*(DWORD)s->height,last=0xffffffff;
    BOOL mono=TRUE,indexed=TRUE; int row,x,y,index=0; BYTE *line;
    if(!go_on(h,p)) return SP_APPABORT;
    memset(p->keys,0,sizeof(p->keys)); p->count=0;
    for(i=0;i<n;i++) {
        DWORD v=s->bits[i]&0xffffff;
        if(v==last) continue;
        last=v;
        if(v && v!=0xffffff) mono=FALSE;
        if(color_index(p,v)<0) {indexed=FALSE; break;}
    }
    row=mono?(s->width+7)/8:indexed?s->width:s->width*3;
    if(!(line=(BYTE *)gdi_alloc((DWORD)row))) return SP_OUTOFMEMORY;
    p->pages++;
    print(p,"%%%%Page: %d %d\nsave\n",p->pages,p->pages);
    if(p->orientation==DMORIENT_LANDSCAPE) print(p,"%d 0 translate 90 rotate\n",p->points.x);
    print(p,"%t %t translate %t %t scale\n",p->offset.x,p->size.y-p->offset.y-p->area.y,s->width,s->height);
    if(mono) print(p,"/DeviceGray setcolorspace\n");
    else if(!indexed) print(p,"/DeviceRGB setcolorspace\n");
    else {
        print(p,"[/Indexed /DeviceRGB %d <",p->count-1);
        for(x=0;x<p->count;x++) {if(!(x%12)) put(p,"\n",1); hex(p,p->colors[x]);}
        print(p,"\n>] setcolorspace\n");
    }
    print(p,"<< /ImageType 1 /Width %d /Height %d /BitsPerComponent %d /Decode [%s]\n",
          s->width,s->height,mono?1:8,mono?"0 1":indexed?"0 255":"0 1 0 1 0 1");
    print(p,"/ImageMatrix [%d 0 0 -%d 0 %d]\n",s->width,s->height,s->height);
    print(p,"/DataSource currentfile /ASCII85Decode filter /RunLengthDecode filter >> image\n");
    last=0xffffffff;
    for(y=0;y<s->height;y++) {
        const DWORD *src=s->bits+(ULONG_PTR)y*(ULONG_PTR)s->stride;
        if(mono) {
            memset(line,0,(size_t)row);
            for(x=0;x<s->width;x++) if(src[x]&0xffffff) line[x>>3]|=(BYTE)(0x80>>(x&7));
        } else if(indexed) for(x=0;x<s->width;x++) {
            DWORD v=src[x]&0xffffff;
            if(v!=last) {last=v; index=color_index(p,v);}
            line[x]=(BYTE)index;
        } else for(x=0;x<s->width;x++) {line[x*3]=(BYTE)(src[x]>>16); line[x*3+1]=(BYTE)(src[x]>>8); line[x*3+2]=(BYTE)src[x];}
        run_length(p,line,row);
        if(y%256==255 && !go_on(h,p)) {gdi_free(line); return SP_APPABORT;}
    }
    gdi_free(line);
    a85_byte(p,128); a85_end(p);
    print(p,"restore showpage\n");
    blank(&p->page);
    return p->failed?SP_OUTOFDISK:1;
}
static int end_doc(HDC h,Printer *p) {
    int r=1;
    if(!p->in_doc) return SP_ERROR;
    if(!is_blank(&p->page) && (r=end_page(h,p))<0) return r;
    print(p,"%%%%Trailer\n%%%%Pages: %d\n%%%%EOF\n",p->pages);
    flush(p);
    r=p->failed?SP_OUTOFDISK:1;
    if(p->spooled && r>0) {
        _lclose(p->out); p->out=HFILE_ERROR;
        queue_job(p);
    }
    end_job(p,r<=0);
    return r;
}
static int abort_doc(Printer *p) {
    if(!p->in_doc) return SP_ERROR;
    end_job(p,TRUE); return 1;
}
void printer_free(DC *dc) {
    Printer *p=dc->printer;
    if(p->in_doc) end_job(p,TRUE);
    surface_free(&p->page); gdi_free(p); dc->printer=NULL;
}

/* --- escapes and device capabilities --------------------------------------------- */
static BOOL supported(int code) {
    switch(code) {
    case NEWFRAME: case ABORTDOC: case NEXTBAND: case FLUSHOUTPUT: case DRAFTMODE: case QUERYESCSUPPORT:
    case SETABORTPROC: case STARTDOC: case ENDDOC: case GETPHYSPAGESIZE: case GETPRINTINGOFFSET:
    case GETSCALINGFACTOR: case SETCOPYCOUNT: case GETTECHNOLOGY: return TRUE;
    }
    return FALSE;
}
int WINAPI Escape(HDC h,int code,int bytes,LPCSTR in,void FAR *out) {
    Printer *p=printer_of(h); char name[64]; int n;
    if(!p) return 0;
    if(p->busy && code!=QUERYESCSUPPORT) return SP_ERROR; /* from within the abort procedure */
    switch(code) {
    case QUERYESCSUPPORT: return in && supported(*(const int FAR *)in);
    case SETABORTPROC: p->abort=(ABORTPROC)in; return 1;
    case STARTDOC:
        n=in?min(bytes>0?bytes:lstrlen(in),(int)sizeof(name)-1):0;
        if(n) memcpy(name,in,(size_t)n);
        name[n]=0;
        return start_doc(p,name,NULL);
    case NEWFRAME:
        if(!p->in_doc) return SP_ERROR;
        p->band=FALSE; return end_page(h,p);
    case NEXTBAND: {
        RECT FAR *r=(RECT FAR *)out;
        if(!p->in_doc || !r) return SP_ERROR;
        if(!p->band) {p->band=TRUE; r_set(r,0,0,p->area.x,p->area.y); return 1;}
        p->band=FALSE; r_set(r,0,0,0,0);
        return end_page(h,p);
    }
    case ENDDOC: return end_doc(h,p);
    case ABORTDOC: return abort_doc(p);
    case FLUSHOUTPUT: case DRAFTMODE: return 1;
    case GETPHYSPAGESIZE: if(out) *(POINT FAR *)out=p->size; return 1;
    case GETPRINTINGOFFSET: if(out) *(POINT FAR *)out=p->offset; return 1;
    case GETSCALINGFACTOR: if(out) {((POINT FAR *)out)->x=((POINT FAR *)out)->y=0;} return 1;
    case SETCOPYCOUNT:
        if(in) p->copies=max(1,min(99,*(const int FAR *)in));
        if(out) *(int FAR *)out=p->copies;
        return 1;
    case GETTECHNOLOGY: if(out) lstrcpy((LPSTR)out,"PostScript"); return 1;
    }
    return 0;
}
int WINAPI StartDoc(HDC h,const DOCINFO FAR *d) {
    Printer *p=printer_of(h);
    if(!p || p->busy) return SP_ERROR;
    return start_doc(p,d?d->lpszDocName:NULL,d?d->lpszOutput:NULL);
}
int WINAPI StartPage(HDC h) {Printer *p=printer_of(h); return p && p->in_doc && !p->busy?1:SP_ERROR;}
int WINAPI EndPage(HDC h) {return printer_of(h)?Escape(h,NEWFRAME,0,NULL,NULL):SP_ERROR;}
int WINAPI EndDoc(HDC h) {return printer_of(h)?Escape(h,ENDDOC,0,NULL,NULL):SP_ERROR;}
int WINAPI AbortDoc(HDC h) {return printer_of(h)?Escape(h,ABORTDOC,0,NULL,NULL):SP_ERROR;}
int WINAPI SetAbortProc(HDC h,ABORTPROC proc) {Printer *p=printer_of(h); if(!p) return SP_ERROR; p->abort=proc; return 1;}
BOOL printer_caps(const DC *dc,int index,int *value) {
    const Printer *p=dc->printer;
    switch(index) {
    case TECHNOLOGY: *value=DT_RASPRINTER; break;
    case HORZSIZE: *value=p->area.x*254/(DPI*10); break;
    case VERTSIZE: *value=p->area.y*254/(DPI*10); break;
    case HORZRES: *value=p->area.x; break;
    case VERTRES: *value=p->area.y; break;
    case ASPECTX: case ASPECTY: *value=DPI; break;
    case ASPECTXY: *value=DPI*1414/1000; break;
    case PHYSICALWIDTH: *value=p->size.x; break;
    case PHYSICALHEIGHT: *value=p->size.y; break;
    case PHYSICALOFFSETX: *value=p->offset.x; break;
    case PHYSICALOFFSETY: *value=p->offset.y; break;
    default: return FALSE;
    }
    return TRUE;
}
