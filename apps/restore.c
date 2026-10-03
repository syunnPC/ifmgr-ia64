/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * RESTORE: MS-DOS 4 CMD/RESTORE counterpart.
 *   RESTORE d: [d:][path][filename] [/S] [/P] [/B:date] [/A:date]
 *           [/E:time] [/L:time] [/M] [/N]
 *
 * Read DOS 3.3/4 BACKUP.nnn/CONTROL.nnn, or earlier 128-byte-header files
 * with BACKUPID.@@@, from successive diskettes or fixed-disk \BACKUP.
 * Restore matching paths/names, including subdirectories with /S; prompt
 * for continuation diskettes. Exclude root system files.
 *
 * For existing targets, /B and /A filter maximum/minimum dates; /E and /L
 * filter maximum/minimum times; /M selects archive-set files; /N skips all.
 * /P confirms replacement of read-only or changed files. Missing targets
 * are always restored. Messages: v4.0 RESTORE.SKL. Exit: 0=success,
 * 1=no files, 3=Ctrl+C, 4=error.
 */
#include "util.h"
#define DH_LENGTH 139
#define DB_LENGTH 70
#define OLD_HEADER 128
#define LAST_DB 0xffffffffU
#define LAST_PART 1
#define COMPLETE 2
enum {DONE,NO_FILES,SHARING,BREAK,ERROR};

static char source_letter,target_letter,in_path[DOS_PATH_MAX],in_spec[13],source_dir[16];
static int subdirs,prompt,modified,not_there,wildcard,old_format,removable,stopped,found;
static int have_before,have_after,have_earlier,have_later; static u16 before,after,earlier,later;
static unsigned wanted_disk=1; static int last_disk;
static unsigned control=0xffff,data=0xffff; static u8 *buffer; static const u32 buffer_size=32768;
/* The file a part belongs to. */
typedef struct {char path[DOS_PATH_MAX],name[13]; u8 flag,attr; u16 sequence,time,date; u32 offset,size;} Part;

static int ctrl_c(void *ctx) {(void)ctx; stopped=1; return DOS_BREAK_CANCEL;}
static void err(const char *text) {to_stderr(1); print("%s",text); to_stderr(0);}
static void any_key(void) {
    err("Press any key to continue . . .");
    DosRegs r={.ax=0x0c08}; if(!dos_call(&r) && !(r.ax&0xff)) {r=(DosRegs){.ax=0x0100}; dos_call(&r);}
    err("\n");
}
static void insert(unsigned n) {
    char text[4]={(char)('0'+n/10%10),(char)('0'+n%10),0};
    to_stderr(1); print("\nInsert backup diskette %s in drive %c:\n",text,source_letter); to_stderr(0);
    any_key();
}
static int read_all(unsigned h,void *p,u32 n) {u32 got; int e=dos_read(h,p,n,&got); return e?e:got<n?DE_FORMAT:0;}
static int seek(unsigned h,u32 at) {u32 pos; return dos_seek(h,at,0,&pos);}
static void close_files(void) {
    if(control!=0xffff) dos_close(control);
    if(data!=0xffff) dos_close(data);
    control=data=0xffff;
}

/* --- matching ------------------------------------------------------------------ */
/* A directory ("\DIR") of the backup and the one asked for; with /S those
 * below it too. */
static int path_match(const char *path) {
    if(!stricmp(path,in_path)) return 1;
    if(!subdirs) return 0;
    if(!strcmp(in_path,"\\")) return 1;
    size_t n=strlen(in_path);
    char head[DOS_PATH_MAX]; strcopy(head,sizeof(head),path); if(strlen(head)<n) return 0;
    head[n]=0;
    return !stricmp(head,in_path) && path[n]=='\\';
}
/* A name and a pattern, '*' running to the dot or the end, as DOS 4's. */
static int name_match(const char *pattern,const char *name) {
    for(;;) {
        if(*pattern=='*') {
            while(*pattern && *pattern!='.') pattern++;
            while(*name && *name!='.') name++;
            if(!*pattern) return !*name;
            if(pattern[1]=='*') return 1;
            if(!*name) return !pattern[1];
            pattern++; name++; continue;
        }
        if(*pattern=='?' ? 1 : upper(*pattern)==upper(*name)) {
            if(!*pattern) return 1;
            if(!*name) {if(*pattern=='?') {pattern++; continue;} return 0;}
            pattern++; name++; continue;
        }
        if(*pattern=='.' && pattern[1]=='*' && !*name) return 1;
        if(*pattern=='.' && !*name) {pattern++; continue;}
        return 0;
    }
}
static void target_of(const Part *p,char out[DOS_PATH_MAX]) {
    out[0]=target_letter; out[1]=':'; out[2]=0;
    strappend(out,DOS_PATH_MAX,p->path);
    if(strcmp(p->path,"\\")) strappend(out,DOS_PATH_MAX,"\\");
    strappend(out,DOS_PATH_MAX,p->name);
}
/* Yes or no as the country has them (AH=65h AL=23h). */
static int yes_no(void) {
    for(;;) {
        DosRegs r={.ax=0x0c01}; if(dos_call(&r)) return 0;
        u8 c=(u8)r.ax; err("\n");
        DosRegs y={.ax=0x6523,.dx=c};
        if(!dos_call(&y) && (y.ax&0xff)<2) return (y.ax&0xff)==1;
        if(upper((char)c)=='Y') return 1;
        if(upper((char)c)=='N') return 0;
    }
}
/* The switches for a file that is already there. */
static int switches_match(const Part *p) {
    char path[DOS_PATH_MAX]; DosFind f;
    target_of(p,path);
    if(dos_find_first(path,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f)) return 1;
    if(not_there) return 0;
    if(have_before && f.date>before) return 0;
    if(have_after && f.date<after) return 0;
    if(have_earlier && f.time>earlier) return 0;
    if(have_later && f.time<later) return 0;
    if(modified && !(f.attr&FA_ARCHIVE)) return 0;
    if(prompt && (f.attr&(FA_RDONLY|FA_ARCHIVE))) {
        to_stderr(1);
        print("\nWarning! File %s\n%s\nReplace the file (Y/N)?",p->name,f.attr&FA_RDONLY?"is a read-only file":"was changed after it was backed up");
        to_stderr(0);
        if(!yes_no()) return 0;
        u8 a=0; dos_attribute(path,1,&a);
    }
    return 1;
}
static int restorable(const Part *p) {
    static const char *const system_files[]={"IBMBIO.COM","IBMDOS.COM","IO.SYS","MSDOS.SYS","CMD.EXE","COMMAND.COM"};
    if(!strcmp(p->path,"\\")) for(unsigned i=0;i<ARRAY_SIZE(system_files);i++) if(!stricmp(p->name,system_files[i])) return 0;
    return path_match(p->path) && name_match(in_spec,p->name) && switches_match(p);
}

/* --- diskettes ----------------------------------------------------------------- */
/* The diskette in the drive: its number, out of sequence or not, and
 * whether it is the last; for the new format CONTROL.nnn and BACKUP.nnn
 * opened. */
static int check_disk(void) {
    int warned=0;
    for(;;) {
        char spec[24],path[24]; DosFind f; u8 header[DH_LENGTH];
        unsigned number=0; int e;
        close_files();
        if(old_format) {
            unsigned h; strcopy(path,sizeof(path),source_dir); strappend(path,sizeof(path),"BACKUPID.@@@");
            if(dos_open(path,DOS_OPEN_READ,0,&h)) {err("\nSource does not contain backup files\n"); return ERROR;}
            e=read_all(h,header,7); dos_close(h);
            if(e) {err("\nSource does not contain backup files\n"); return ERROR;}
            number=header[1]+header[2]*10u; last_disk=header[0]==0xff;
        } else {
            strcopy(spec,sizeof(spec),source_dir); strappend(spec,sizeof(spec),"CONTROL.???");
            e=dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);
            const char *x=f.name+8;
            if(e || strlen(f.name)!=11 || x[-1]!='.' || x[0]<'0' || x[0]>'9' || x[1]<'0' || x[1]>'9' || x[2]<'0' || x[2]>'9') {
                err("\nSource does not contain backup files\n"); return ERROR;
            }
            unsigned name_number=(unsigned)(x[0]-'0')*100+(unsigned)(x[1]-'0')*10+(unsigned)(x[2]-'0');
            if(name_number!=wanted_disk && !warned) {
                err("\nWarning! Diskette is out of sequence\nReplace diskette or continue if okay\n"); any_key();
                warned=1; continue;
            }
            strcopy(path,sizeof(path),source_dir); strappend(path,sizeof(path),f.name);
            if(dos_open(path,DOS_OPEN_READ,0,&control) || read_all(control,header,DH_LENGTH) || header[0]!=DH_LENGTH) {
                err("\nSource does not contain backup files\n"); return ERROR;
            }
            path[strlen(path)-11]=0; strappend(path,sizeof(path),"BACKUP"); strappend(path,sizeof(path),f.name+7);
            if(dos_open(path,DOS_OPEN_READ,0,&data)) {err("\n*** Not able to restore file ***\n"); return ERROR;}
            number=header[9]; last_disk=header[DH_LENGTH-1]==0xff;
        }
        if(old_format && number!=wanted_disk && !warned) {
            err("\nWarning! Diskette is out of sequence\nReplace diskette or continue if okay\n"); any_key();
            warned=1; continue;
        }
        print("\n*** Restoring files from drive %c: ***\n",source_letter);
        if(removable) {char n[4]={(char)('0'+number/10%10),(char)('0'+number%10),0}; print("Diskette: %s\n",n);}
        wanted_disk=number+1;
        return DONE;
    }
}
/* Bytes of a part to the file. */
static int copy_part(unsigned from,u32 offset,u32 size,unsigned to) {
    if(!old_format && seek(from,offset)) return DE_FORMAT;
    while(size) {
        u32 take=size<buffer_size?size:buffer_size,got,done;
        int e=dos_read(from,buffer,take,&got); if(e) return e;
        if(!got) return DE_FORMAT;
        e=dos_write(to,buffer,got,&done);
        if(e==DE_BREAK || stopped) return DE_BREAK;
        if(e || done<got) return DE_FULL;
        size-=got;
    }
    return stopped?DE_BREAK:0;
}
/* The next part from the header of the control file the cursor is at;
 * DOS 4's header may be 38 bytes long. Directory blocks are passed through. */
typedef struct {u32 at,next_db; unsigned left; char path[DOS_PATH_MAX];} Cursor;
static int next_part(Cursor *c,Part *p) {
    for(;;) {
        if(!c->left) {
            if(c->next_db==LAST_DB) return DE_NOMORE;
            u8 db[DB_LENGTH];
            if(seek(control,c->next_db) || read_all(control,db,DB_LENGTH) || db[0]<DB_LENGTH) return DE_FORMAT;
            c->at=c->next_db+db[0]; c->left=rd16(db+64); c->next_db=rd32(db+66);
            char *q=c->path; *q++='\\'; for(unsigned i=0;i<63 && db[1+i];i++) *q++=(char)db[1+i];
            *q=0;
            continue;
        }
        u8 fh[38];
        if(seek(control,c->at) || read_all(control,fh,34) || fh[0]<34 || fh[0]>38) return DE_FORMAT;
        c->at+=fh[0]; c->left--;
        strcopy(p->path,sizeof(p->path),c->path);
        memcpy(p->name,fh+1,12); p->name[12]=0;
        p->flag=fh[13]; p->sequence=rd16(fh+18); p->offset=rd32(fh+20); p->size=rd32(fh+24);
        p->attr=(u8)rd16(fh+28); p->time=rd16(fh+30); p->date=rd16(fh+32);
        return 0;
    }
}
/* An old BACKUP's file on the diskette: its header. */
static int old_part(const DosFind *f,Part *p,unsigned *h) {
    char path[24]; u8 header[OLD_HEADER];
    strcopy(path,sizeof(path),source_dir); strappend(path,sizeof(path),f->name);
    if(dos_open(path,DOS_OPEN_READ,0,h)) return DE_ACCESS;
    if(read_all(*h,header,OLD_HEADER)) {dos_close(*h); return DE_FORMAT;}
    header[5+77]=0;
    const char *where=(const char *)header+5,*slash=where;
    for(const char *s=where;*s;s++) if(*s=='\\') slash=s;
    memcpy(p->path,where,(size_t)(slash-where)); p->path[slash-where]=0;
    if(!p->path[0]) strcopy(p->path,sizeof(p->path),"\\");
    strcopy(p->name,sizeof(p->name),*slash=='\\'?slash+1:slash);
    p->flag=(u8)((header[0]==0xff?LAST_PART:0)|COMPLETE); p->sequence=(u16)(header[1]|header[2]<<8);
    p->offset=OLD_HEADER; p->size=f->size>OLD_HEADER?f->size-OLD_HEADER:0;
    p->attr=f->attr; p->time=f->time; p->date=f->date;
    return 0;
}
/* The rest of a file from the next diskettes. */
static int follow(Part *p,unsigned to,Cursor *c) {
    unsigned sequence=p->sequence;
    while(!(p->flag&LAST_PART)) {
        Part next; char name[13]; strcopy(name,sizeof(name),p->name);
        close_files(); insert(wanted_disk);
        if(stopped) return BREAK;
        int r=check_disk(); if(r) return r;
        int e;
        if(old_format) {
            char spec[24]; DosFind f; unsigned h;
            strcopy(spec,sizeof(spec),source_dir); strappend(spec,sizeof(spec),name);
            e=dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);
            if(!e) e=old_part(&f,&next,&h);
            if(!e) {
                if(next.sequence!=sequence+1) {dos_close(h); err("\nRestore file sequence error\n"); return ERROR;}
                e=copy_part(h,0,next.size,to); dos_close(h);
            }
        } else {
            *c=(Cursor){.next_db=DH_LENGTH};
            e=next_part(c,&next);
            if(!e && (next.sequence!=sequence+1 || stricmp(next.name,name))) {err("\nRestore file sequence error\n"); return ERROR;}
            if(!e) e=copy_part(data,next.offset,next.size,to);
        }
        if(e==DE_BREAK) return BREAK;
        if(e==DE_FULL) {err("\nInsufficient disk space\n"); return ERROR;}
        if(e) {err("\n*** Not able to restore file ***\n"); return ERROR;}
        sequence=next.sequence; p->flag=next.flag;
    }
    return DONE;
}
/* A file, its directories made where missing; its date, time and
 * attributes as they were. */
static int restore_file(Part *p,unsigned from,Cursor *c) {
    char path[DOS_PATH_MAX],dir[DOS_PATH_MAX]; unsigned h,result;
    target_of(p,path);
    print("%s\n",path+2);
    strcopy(dir,sizeof(dir),path);
    {char *slash=dir; for(char *s=dir;*s;s++) if(*s=='\\') slash=s; slash[1]=0;}
    if(strlen(dir)>3) make_dirs(dir);
    {u8 a=0; DosFind f; if(!dos_find_first(path,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f) && (f.attr&(FA_RDONLY|FA_HIDDEN|FA_SYSTEM))) dos_attribute(path,1,&a);}
    if(dos_open_ex(path,DOS_OPEN_WRITE,0,0x12,&h,&result)) {err("\nFile creation error\n"); return DONE;}
    found=1;
    int e=copy_part(from,p->offset,p->size,h),r=DONE;
    if(e==DE_BREAK) r=BREAK;
    else if(e==DE_FULL) {err("\nInsufficient disk space\n"); r=ERROR;}
    else if(e) {err("\n*** Not able to restore file ***\n"); r=ERROR;}
    if(!r && !(p->flag&LAST_PART)) r=follow(p,h,c);
    if(!r) {u16 date=p->date,time=p->time; dos_file_time(h,1,&date,&time);}
    dos_close(h);
    if(r) {dos_remove(path,0); if(r==ERROR) err("\nThe last file was not restored\n"); return r;}
    u8 a=(u8)(p->attr&(FA_RDONLY|FA_HIDDEN|FA_SYSTEM|FA_ARCHIVE)); dos_attribute(path,1,&a);
    return DONE;
}
static int restore_disk(void) {
    if(old_format) {
        char spec[24]; DosFind f;
        strcopy(spec,sizeof(spec),source_dir); strappend(spec,sizeof(spec),"*.*");
        for(int e=dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);!e;e=dos_find_next(&f)) {
            Part p; unsigned h;
            if(stopped) return BREAK;
            if(!stricmp(f.name,"BACKUPID.@@@") || old_part(&f,&p,&h)) continue;
            /* A file's later part, its first on an earlier diskette. */
            if(p.sequence!=1 || !restorable(&p)) {dos_close(h); continue;}
            DosFind keep=f;
            int r=restore_file(&p,h,NULL); dos_close(h);
            if(r) return r;
            if(!(p.flag&LAST_PART)) return DONE; /* went on to later diskettes */
            /* The search is the diskette's own; start it again past this file. */
            for(e=dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);!e && strcmp(f.name,keep.name);e=dos_find_next(&f)) {}
            if(e) break;
        }
        return DONE;
    }
    Cursor c={.next_db=DH_LENGTH}; Part p; int first=1;
    for(int e;!(e=next_part(&c,&p));first=0) {
        if(stopped) return BREAK;
        if(!(p.flag&COMPLETE)) continue;
        /* The rest of a file whose first part was on an earlier diskette. */
        if(p.sequence!=1) {
            if(first) continue;
            err("\nRestore file sequence error\n"); return ERROR;
        }
        if(!restorable(&p)) continue;
        int r=restore_file(&p,data,&c);
        if(r) return r;
    }
    return DONE;
}
static int date_switch(const char *arg,u16 *out,int *have,int time) {
    if(arg[2]!=':') return 0;
    if(!(time?parse_time(arg+3,out):parse_date(arg+3,out))) {err(time?"\nInvalid time\n":"\nInvalid date\n"); return -1;}
    *have=1; return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],source[DOS_PATH_MAX]="",target[DOS_PATH_MAX]="";
    const char *p=app_dos->command_tail(); unsigned args=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        args++;
        if(arg[0]!='/') {
            if(!source[0]) strcopy(source,sizeof(source),arg);
            else if(!target[0]) strcopy(target,sizeof(target),arg);
            else {err("\nInvalid number of parameters\n"); return ERROR;}
            continue;
        }
        char c=(char)upper(arg[1]); int r=0;
        /* A date's slashes would start new words: it runs on to the next blank. */
        if((c=='B' || c=='A') && arg[2]==':') {
            size_t n=strlen(arg);
            while(*p && *p!=' ' && *p!='\t' && n<sizeof(arg)-1) arg[n++]=*p++;
            arg[n]=0;
        }
        if(!arg[2] && (c=='S' || c=='P' || c=='M' || c=='N')) *(c=='S'?&subdirs:c=='P'?&prompt:c=='M'?&modified:&not_there)=1;
        else if(c=='B') r=date_switch(arg,&before,&have_before,0);
        else if(c=='A') r=date_switch(arg,&after,&have_after,0);
        else if(c=='E') r=date_switch(arg,&earlier,&have_earlier,1);
        else if(c=='L') r=date_switch(arg,&later,&have_later,1);
        if(r<0) return ERROR;
        if(!r && !(!arg[2] && (c=='S' || c=='P' || c=='M' || c=='N'))) {parse_error(PARSE_SWITCH,arg); return ERROR;}
    }
    if(!args) {err("\nInvalid number of parameters\n"); return ERROR;}
    if(!source[0]) {err("\nNo source drive specified\n"); return ERROR;}
    if(!target[0]) {err("\nNo target drive specified\n"); return ERROR;}
    source_letter=(char)upper(source[0]);
    if(source[1]!=':' || source[2] || source_letter<'A' || source_letter>'Z') {err("\nInvalid drive specification\n"); return ERROR;}
    /* The target: its drive, the directory from its root, the name. */
    const char *spec=target;
    target_letter=(char)(spec[1]==':'?upper(spec[0]):'A'+(char)dos_current_drive());
    if(spec[1]==':') spec+=2;
    if(target_letter<'A' || target_letter>'Z') {err("\nInvalid drive specification\n"); return ERROR;}
    if(source_letter==target_letter) {err("\nSource and target drives are the same\n"); return ERROR;}
    static const char *const devices[]={"LPT1","LPT2","PRN","CON","NUL","AUX","LPT1:","LPT2:","PRN:","CON:","NUL:","AUX:"};
    {
        const char *last=spec; for(const char *s=spec;*s;s++) if(*s=='\\') last=s+1;
        for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(!stricmp(last,devices[i])) {parse_error(PARSE_PARAMETER,last); return ERROR;}
        /* The directory part, its final backslash left out but for the root. */
        char dir[DOS_PATH_MAX]={target_letter,':',0},full[DOS_PATH_MAX];
        size_t n=(size_t)(last-spec);
        if(n>=sizeof(dir)-3) {err("\nInvalid path\n"); return ERROR;}
        memcpy(dir+2,spec,n); dir[2+n]=0;
        if(n>1) dir[1+n]=0;
        if(!n) strappend(dir,sizeof(dir),".");
        if(dos_full_path(dir,full)) {err("\nInvalid path\n"); return ERROR;}
        strcopy(in_path,sizeof(in_path),full+2);
        size_t k=strlen(in_path); if(k>1 && in_path[k-1]=='\\') in_path[k-1]=0;
        if(strlen(last)>12) {err("\nInvalid path\n"); return ERROR;}
        strcopy(in_spec,sizeof(in_spec),*last?last:"*.*");
        for(char *s=in_spec;*s;s++) {*s=upper(*s); if(*s=='*' || *s=='?') wildcard=1;}
    }
    DosDeviceParams params={.size=sizeof(params)};
    if(dos_device_params((unsigned)(source_letter-'A'),&params)) {err("\nInvalid drive specification\n"); return ERROR;}
    removable=!(params.attributes&DOS_DEVICE_NONREMOVABLE);
    source_dir[0]=source_letter; source_dir[1]=':'; source_dir[2]=0;
    strappend(source_dir,sizeof(source_dir),removable?"\\":"\\BACKUP\\");
    if(dos_alloc(buffer_size/16,(void **)&buffer)) {err("\nInsufficient memory\n"); return ERROR;}
    DosBreakHandler handler={ctrl_c,NULL},previous; int handled=!dos_break_handler(&handler,&previous);
    if(removable) insert(1);
    int r=DONE;
    /* The format from the first diskette's files, and when they were made. */
    {
        char search[24]; DosFind f; int e;
        strcopy(search,sizeof(search),source_dir); strappend(search,sizeof(search),"BACKUP*.???");
        for(e=dos_find_first(search,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);!e;e=dos_find_next(&f)) {
            if(!stricmp(f.name,"BACKUPID.@@@")) {old_format=1; break;}
            if(strlen(f.name)==10 && f.name[6]=='.' && f.name[7]>='0' && f.name[7]<='9' && f.name[8]>='0' && f.name[8]<='9' && f.name[9]>='0' && f.name[9]<='9') break;
        }
        if(e) {err("\nSource does not contain backup files\n"); r=ERROR;}
        else {
            DosCountryInfo info; char date[16],*q=date; unsigned y=1980+(f.date>>9),m=f.date>>5&15,d=f.date&31;
            if(dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,&info)) {info.date_order=0; info.date_separator[0]='-';}
            unsigned v[3]={m,d,y}; if(info.date_order==1) {v[0]=d; v[1]=m;} else if(info.date_order==2) {v[0]=y; v[1]=m; v[2]=d;}
            for(unsigned i=0;i<3;i++) {
                unsigned x=v[i],w=x>=1000?4:2;
                char t[4]; for(unsigned k=w;k--;) {t[k]=(char)('0'+x%10); x/=10;}
                for(unsigned k=0;k<w;k++) *q++=t[k];
                if(i<2) *q++=(char)info.date_separator[0];
            }
            *q=0;
            print("\n*** Files were backed up %s ***\n",date);
        }
    }
    while(!r) {
        r=check_disk(); if(r) break;
        r=restore_disk();
        close_files();
        if(r || stopped) break;
        if(last_disk || (!wildcard && found && !subdirs)) break;
        print("\n"); insert(wanted_disk);
        if(stopped) r=BREAK;
    }
    if(stopped && !r) r=BREAK;
    close_files();
    if(handled) dos_break_handler(&previous,NULL);
    dos_free(buffer);
    if(!r && !found) {err("\nWarning! No files were found to restore\n"); r=NO_FILES;}
    return (EFI_STATUS)r;
}
