/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * BACKUP: MS-DOS 4 CMD/BACKUP counterpart.
 *   BACKUP [d:][path][filename] d: [/S] [/M] [/A] [/F[:size]] [/D:date]
 *          [/T:time] [/L[:[d:][path]filename]]
 *
 * DOS 3.3/4 format: BACKUP.nnn concatenates data; CONTROL.nnn uses 139-byte
 * disk, 70-byte directory and 34-byte file/part headers. Diskettes erase
 * root files first; fixed disks use \BACKUP. Split files continue on numbered,
 * labelled diskettes with read-only backup files. Write a complete control
 * header on the next diskette if the current control file cannot grow.
 *
 * /S traverses parent files before subdirectories; /M selects archive files.
 * Clear archive bits after backup. /A appends from the last diskette; /F
 * runs FORMAT.COM on unreadable diskettes. /D and /T filter by minimum date
 * and time on that date. /L appends date/time, diskette and file to a log
 * (default: BACKUP.LOG in the source root). Exclude root system files.
 * Messages: v4.0 BACKUP.SKL. Exit: 0=success, 1=no files, 2=open failures,
 * 3=Ctrl+C, 4=error.
 */
#include "util.h"
#include "maint.h"
#define DH_LENGTH 139
#define DB_LENGTH 70
#define FH_LENGTH 34
#define LAST_DB 0xffffffffU
#define LAST_PART 1
#define SUCCESSFUL 2
#define STOP (-1) /* a walk ended with stop_code */
enum {DONE,NO_FILES,SHARING,BREAK,ERROR};

static char source_letter,target_letter,pattern[13],source_dir[DOS_PATH_MAX];
static int subdirs,modified,add,format,removable,stopped,first_target=1,result,stop_code;
static int have_date,have_time; static u16 since_date,since_time;
static char format_size[16],log_path[DOS_PATH_MAX]; static int logging,log_on_target;
static unsigned log_handle=0xffff,data=0xffff,control=0xffff;
static u32 data_length,control_length; /* the open BACKUP.nnn and CONTROL.nnn */
static unsigned disks;                 /* diskettes complete */
static u32 db_at,fh_at;                /* the current directory block and file header */
static unsigned entries;               /* files in the current directory block */
static int new_directory=1;
static char directory[DOS_PATH_MAX];   /* the directory the files come from, "D:\PATH\" */
static u8 *buffer; static const u32 buffer_size=32768;
/* The file being backed up: its entry, the bytes of it on this diskette and
 * before, the part's number, whether its header is on this diskette. */
static DosFind file; static u32 part_size,done_size; static unsigned sequence; static int fh_here;

static int ctrl_c(void *ctx) {(void)ctx; stopped=1; return DOS_BREAK_CANCEL;}
static void err(const char *text) {to_stderr(1); print("%s",text); to_stderr(0);}
static void any_key(void) {
    err("Press any key to continue . . .");
    DosRegs r={.ax=0x0c08}; if(!dos_call(&r) && !(r.ax&0xff)) {r=(DosRegs){.ax=0x0100}; dos_call(&r);}
    err("\n");
}
static int prefix(const char *s,const char *p) {while(*p) if(upper(*s++)!=*p++) return 0; return 1;}
/* Decimal digits, at least width of them. */
static char *digits(char *out,unsigned v,unsigned width) {
    char t[10]; unsigned n=0;
    do t[n++]=(char)('0'+v%10); while(v/=10);
    while(n<width) t[n++]='0';
    while(n) *out++=t[--n];
    *out=0; return out;
}
static void extension(char out[4],unsigned n) {digits(out,n%1000,3);}
/* A diskette number as BACKUP shows it: two digits, three from 100. */
static void number(char out[4],unsigned n) {digits(out,n,n<100?2:3);}
/* The target's BACKUP.nnn or CONTROL.nnn for diskette n. */
static void target_name(char *out,const char *kind,unsigned n) {
    char ext[4]; extension(ext,n);
    out[0]=target_letter; out[1]=':'; out[2]=0;
    strappend(out,DOS_PATH_MAX,removable?"\\":"\\BACKUP\\");
    strappend(out,DOS_PATH_MAX,kind); strappend(out,DOS_PATH_MAX,"."); strappend(out,DOS_PATH_MAX,ext);
}
static int write_all(unsigned h,const void *p,u32 n,u32 *done) {
    *done=0; int e=dos_write(h,p,n,done);
    return e?e:*done<n?DE_FULL:0;
}
static int create(const char *path,u8 attr,unsigned *h) {unsigned result; return dos_open_ex(path,DOS_OPEN_RDWR,attr,0x12,h,&result);}
static int patch(u32 at,const void *p,u32 n) {
    u32 pos,done; int e=dos_seek(control,at,0,&pos);
    if(!e) e=write_all(control,p,n,&done);
    int back=dos_seek(control,0,2,&pos); return e?e:back;
}

/* --- the log ----------------------------------------------------------------- */
static void close_log(void) {if(log_handle!=0xffff) dos_close(log_handle); log_handle=0xffff;}
static void log_text(const char *text) {
    u32 done;
    if(log_handle==0xffff || !logging) return;
    if(write_all(log_handle,text,(u32)strlen(text),&done)) {
        err("\nDisk full error writing to BACKUP Log File\n"); any_key(); logging=0;
    }
}
/* Opened (made if missing) to be added to, named on the screen, and the
 * date and time of this backup written, in the country's order. */
static int open_log(void) {
    unsigned h; u32 pos; DosDateTime now; DosCountryInfo info; char line[48],*p=line;
    int e=dos_open(log_path,DOS_OPEN_RDWR,0,&h);
    if(e) e=create(log_path,0,&h);
    if(!e) e=dos_seek(h,0,2,&pos);
    if(e) {err("\nError opening logfile\n"); return e;}
    log_handle=h;
    print("\nLogging to file %s\n",log_path);
    if(dos_get_datetime(&now)) return 0;
    if(dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,&info)) {info.date_order=0; info.date_separator[0]='-'; info.time_separator[0]=':';}
    unsigned order[3][3]={{now.month,now.day,now.year},{now.day,now.month,now.year},{now.year,now.month,now.day}};
    const unsigned *d=order[info.date_order<3?info.date_order:0];
    *p++='\r'; *p++='\n';
    p=digits(p,d[0],info.date_order==2?4:1); *p++=(char)info.date_separator[0];
    p=digits(p,d[1],2); *p++=(char)info.date_separator[0];
    p=digits(p,d[2],info.date_order==2?2:4); *p++=' '; *p++=' ';
    p=digits(p,now.hour,1); *p++=(char)info.time_separator[0];
    p=digits(p,now.minute,2); *p++=(char)info.time_separator[0];
    digits(p,now.second,2);
    log_text(line);
    return 0;
}
/* "\PATH\NAME" on standard output and, after its diskette, in the log. */
static void show_path(void) {
    char path[DOS_PATH_MAX],line[DOS_PATH_MAX+16];
    strcopy(path,sizeof(path),directory+2); strappend(path,sizeof(path),file.name);
    print("\n%s",path);
    line[0]='\r'; line[1]='\n'; extension(line+2,disks+1); strappend(line,sizeof(line),"  "); strappend(line,sizeof(line),path);
    log_text(line);
}

/* --- the control file ---------------------------------------------------------- */
static int put_db(void) {
    u8 db[DB_LENGTH]; u32 done; memset(db,0,sizeof(db));
    if(entries) { /* The block before counts its files and leads to this one. */
        u8 next[6]; wr16(next,(u16)entries); wr32(next+2,control_length);
        int e=patch(db_at+64,next,6); if(e) return e;
    }
    db[0]=DB_LENGTH; strcopy((char *)db+1,63,directory+3);
    size_t n=strlen((char *)db+1); if(n && db[n]=='\\') db[n]=0;
    wr32(db+66,LAST_DB);
    db_at=control_length;
    int e=write_all(control,db,sizeof(db),&done); control_length+=done;
    if(!e) {new_directory=0; entries=0;}
    return e;
}
static int put_dh(void) {
    u8 dh[DH_LENGTH]; u32 done; memset(dh,0,sizeof(dh));
    dh[0]=DH_LENGTH; memcpy(dh+1,"BACKUP  ",8); dh[9]=(u8)(disks+1);
    int e=write_all(control,dh,sizeof(dh),&done); control_length+=done;
    if(e) return e;
    entries=0; return put_db();
}
static int put_fh(void) {
    u8 fh[FH_LENGTH]; u32 done; memset(fh,0,sizeof(fh));
    if(new_directory) {int e=put_db(); if(e) return e;}
    fh_at=control_length;
    fh[0]=FH_LENGTH; strcopy((char *)fh+1,13,file.name); fh[13]=LAST_PART|SUCCESSFUL;
    wr32(fh+14,file.size); wr16(fh+18,(u16)sequence); wr32(fh+20,data_length);
    wr32(fh+24,file.size-done_size); wr16(fh+28,file.attr); wr16(fh+30,file.time); wr16(fh+32,file.date);
    int e=write_all(control,fh,sizeof(fh),&done); control_length+=done;
    if(!e) fh_here=1;
    return e;
}
/* The current file header's flags and part size. */
static int update_fh(int last,int ok) {
    u8 flag=(u8)((last?LAST_PART:0)|(ok?SUCCESSFUL:0)),size[4]; wr32(size,part_size);
    int e=patch(fh_at+13,&flag,1); if(!e) e=patch(fh_at+24,size,4);
    return e;
}
/* Whether the control file can take another header (and directory block):
 * within its last cluster, or in a free one. */
static int control_room(void) {
    DosDriveInfo info;
    if(dos_drive_info((u32)(target_letter-'A'),&info) || !info.sectors_per_cluster) return 1;
    u32 cluster=info.sectors_per_cluster*512,used=control_length%cluster,need=FH_LENGTH+(new_directory?DB_LENGTH:0);
    if(control_length && !used) used=cluster;
    return (control_length && cluster-used>=need) || info.free_clusters>0;
}

/* --- targets ------------------------------------------------------------------- */
static void delete_target_files(void) {
    char spec[DOS_PATH_MAX],path[DOS_PATH_MAX]; DosFind f;
    spec[0]=target_letter; spec[1]=':'; spec[2]=0; strappend(spec,sizeof(spec),removable?"\\*.*":"\\BACKUP\\*.*");
    for(int e=dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f);!e;e=dos_find_next(&f)) {
        path[0]=target_letter; path[1]=':'; path[2]=0;
        strappend(path,sizeof(path),removable?"\\":"\\BACKUP\\"); strappend(path,sizeof(path),f.name);
        if(log_on_target && !stricmp(path,log_path)) continue;
        if(dos_remove(path,0)) {u8 a=0; dos_attribute(path,1,&a); dos_remove(path,0);}
    }
}
/* FORMAT.COM in a PATH directory. */
static int find_format(char out[DOS_PATH_MAX]) {
    char list[512]; const char *p=list;
    if(dos_env_get("PATH",list,sizeof(list))) list[0]=0;
    for(;;) {
        unsigned n=0; DosFind f;
        while(*p && *p!=';' && n<DOS_PATH_MAX-12) out[n++]=*p++;
        out[n]=0;
        if(n) {
            if(out[n-1]!='\\') strappend(out,DOS_PATH_MAX,"\\");
            strappend(out,DOS_PATH_MAX,"FORMAT.COM");
            if(!dos_find_first(out,0,&f)) return 0;
        }
        if(!*p) return DE_NOFILE;
        p++;
    }
}
static unsigned fail_errors(void *ctx,const DosCriticalError *e) {(void)ctx; (void)e; return DOS_CRITICAL_FAIL;}
/* /F: a diskette that cannot be read is formatted, as FORMAT d: [/F:size]
 * /BACKUP /V:BACKUP. */
static void format_diskette(void) {
    DosCriticalHandler quiet={fail_errors,NULL},previous; DosDriveInfo info;
    int hooked=!dos_critical_handler(&quiet,&previous);
    int unreadable=dos_drive_info((u32)(target_letter-'A'),&info)!=0;
    if(hooked) dos_critical_handler(&previous,NULL);
    if(!unreadable) return;
    char program[DOS_PATH_MAX],tail[48]={target_letter,':',0},n[4];
    err("\n");
    if(find_format(program)) {err("\nCannot find FORMAT.COM\n"); return;}
    if(format_size[0]) {strappend(tail,sizeof(tail)," /F:"); strappend(tail,sizeof(tail),format_size);}
    strappend(tail,sizeof(tail)," /BACKUP /V:BACKUP");
    if(!dos_exec(program,tail) && !dos_get_errorlevel()) {err("\n"); return;}
    number(n,disks+1);
    to_stderr(1);
    print("\nError executing FORMAT\n\nInsert backup diskette %s in drive %c:\n",n,target_letter);
    print("\nWarning! Files in the target drive\n%c:\\ root directory will be erased\n",target_letter);
    to_stderr(0);
    any_key();
}
/* /A: the last diskette of a backup made by DOS 3.3 or later. */
static int check_last(void) {
    char path[DOS_PATH_MAX],dir[16]={target_letter,':',0}; DosFind f; unsigned h; u8 header[DH_LENGTH]; u32 got=0;
    strappend(dir,sizeof(dir),removable?"\\":"\\BACKUP\\");
    strcopy(path,sizeof(path),dir); strappend(path,sizeof(path),"BACKUPID.@@@");
    if(!dos_find_first(path,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f)) {err("\nTarget can not be used for backup\n"); return ERROR;}
    strcopy(path,sizeof(path),dir); strappend(path,sizeof(path),"CONTROL.*");
    if(dos_find_first(path,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f)) {err("\nLast backup diskette not inserted\n"); return ERROR;}
    strcopy(path,sizeof(path),dir); strappend(path,sizeof(path),f.name);
    if(dos_open(path,DOS_OPEN_READ,0,&h)) {err("\nLast backup diskette not inserted\n"); return ERROR;}
    int e=dos_read(h,header,sizeof(header),&got); dos_close(h);
    if(e || got<DH_LENGTH || header[DH_LENGTH-1]!=0xff) {err("\nLast backup diskette not inserted\n"); return ERROR;}
    disks=header[9]?header[9]-1u:0;
    return DONE;
}
static int open_files(int append) {
    char path[DOS_PATH_MAX]; unsigned *handles[2]={&data,&control}; u32 *lengths[2]={&data_length,&control_length};
    static const char *const kinds[2]={"BACKUP","CONTROL"};
    for(unsigned i=0;i<2;i++) {
        int e;
        target_name(path,kinds[i],disks+1);
        if(append) {u8 a=FA_ARCHIVE; dos_attribute(path,1,&a); e=dos_open(path,DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,handles[i]);}
        else e=create(path,FA_ARCHIVE,handles[i]);
        *lengths[i]=0;
        if(!e && append) e=dos_seek(*handles[i],0,2,lengths[i]);
        if(e) {err("\nTarget can not be used for backup\n"); return ERROR;}
    }
    return DONE;
}
/* /A on the last diskette: no longer the last, its last directory block
 * leading on to what is added. */
static int reopen_last(void) {
    u8 zero=0,next[4]; u32 at=DH_LENGTH,pos,got;
    int e=patch(DH_LENGTH-1,&zero,1);
    for(unsigned guard=0;!e && guard<65536;guard++) {
        e=dos_seek(control,at+66,0,&pos); if(!e) e=dos_read(control,next,4,&got);
        if(!e && got<4) e=DE_FORMAT;
        if(e || rd32(next)==LAST_DB) break;
        at=rd32(next);
    }
    if(!e) {wr32(next,control_length); e=patch(at+66,next,4);}
    new_directory=1; entries=0;
    return e;
}
static int get_target(void) {
    char n[4]; int append=add && first_target;
    number(n,disks+1);
    if(removable) {
        to_stderr(1);
        if(append) print("\nInsert last backup diskette in drive %c:\n",target_letter);
        else {
            print("\nInsert backup diskette %s in drive %c:\n",n,target_letter);
            print("\nWarning! Files in the target drive\n%c:\\ root directory will be erased\n",target_letter);
        }
        to_stderr(0); any_key();
        if(stopped) return BREAK;
        if(format && !append) format_diskette();
        if(append && check_last()) return ERROR;
    } else {
        char dir[16]={target_letter,':','\\','B','A','C','K','U','P',0},spec[24]; DosFind f;
        strcopy(spec,sizeof(spec),dir); strappend(spec,sizeof(spec),"\\*.*");
        if(!dos_find_first(spec,FA_HIDDEN|FA_SYSTEM|FA_RDONLY|FA_ARCHIVE,&f)) {
            if(!add) {
                to_stderr(1); print("\nWarning! Files in the target drive\n%c:\\BACKUP directory will be erased\n",target_letter); to_stderr(0);
                any_key();
            }
        } else if(dos_mkdir(dir) && dos_find_first(dir,FA_DIR,&f)) {err("\nTarget can not be used for backup\n"); return ERROR;}
        if(stopped) return BREAK;
        if(append && check_last()) return ERROR;
    }
    number(n,disks+1);
    to_stderr(1); print("\n*** Backing up files to drive %c: ***\n",target_letter); to_stderr(0);
    print("Diskette Number: %s\n",n);
    if(!append) delete_target_files();
    if(open_files(append)) return ERROR;
    if(logging && log_on_target && open_log()) return ERROR;
    if(append?reopen_last():put_dh()) {err("\nTarget can not be used for backup\n"); return ERROR;}
    return DONE;
}
/* The diskette's files closed and read-only, the diskette labelled. */
static void finish_target(int last) {
    char path[DOS_PATH_MAX];
    if(control!=0xffff) {
        u8 count[2],flag=last?0xff:0; wr16(count,(u16)entries);
        patch(db_at+64,count,2); patch(DH_LENGTH-1,&flag,1);
        dos_close(control); control=0xffff;
    }
    if(data!=0xffff) {dos_close(data); data=0xffff;}
    u8 a=FA_ARCHIVE|FA_RDONLY;
    target_name(path,"CONTROL",disks+1); dos_attribute(path,1,&a);
    target_name(path,"BACKUP",disks+1); dos_attribute(path,1,&a);
    if(removable) {
        u8 label[11]; char ext[4]; extension(ext,disks+1);
        memcpy(label,"BACKUP  ",8); memcpy(label+8,ext,3);
        label_set((unsigned)(target_letter-'A'),label);
    }
    if(log_on_target) close_log();
}
/* The diskette is full: the file's part on it (perhaps none of its bytes)
 * is counted, and the file goes on with its next part on the next one. */
static int next_target(void) {
    int part=fh_here;
    if(part) {entries++; update_fh(0,1);}
    if(!removable) {
        if(part) update_fh(1,0);
        finish_target(1);
        err("\n*** Last file not backed up ***\n");
        to_stderr(1); print("\nFixed backup device %c: is full\n",target_letter); to_stderr(0);
        return ERROR;
    }
    finish_target(0);
    if(part) sequence++;
    disks++; fh_here=0; part_size=0;
    err("\n");
    int r=get_target(); if(r) return r;
    if(part) show_path();
    if(put_fh()) {err("\nTarget can not be used for backup\n"); return ERROR;}
    return DONE;
}
static int backup_file(const char *path) {
    unsigned h; int e=0;
    show_path();
    for(int tries=0;;tries++) {
        e=dos_open(path,DOS_OPEN_READ|DOS_SHARE_DENY_WRITE,0,&h);
        if(!e || tries==9 || (e!=DE_SHARE && e!=DE_LOCK)) break;
    }
    if(e) {
        err("\n*** Not able to backup file ***\n"); result=SHARING;
        log_text("\r\n*** Last file not backed up ***");
        return DONE;
    }
    sequence=1; part_size=done_size=0; fh_here=0;
    int r=control_room()?(put_fh()?ERROR:DONE):next_target();
    if(r==ERROR && !fh_here) err("\nTarget can not be used for backup\n");
    while(!r) {
        u32 got,done=0;
        e=dos_read(h,buffer,buffer_size,&got);
        if(e || !got) break;
        while(!r && done<got) {
            u32 wrote=0; int w=dos_write(data,buffer+done,got-done,&wrote);
            part_size+=wrote; done_size+=wrote; data_length+=wrote; done+=wrote;
            if(stopped || w==DE_BREAK) r=BREAK;
            else if(w && w!=DE_FULL) {err("\nTarget can not be used for backup\n"); r=ERROR;}
            else if(done<got) r=next_target();
        }
        if(stopped) r=BREAK;
    }
    dos_close(h);
    if(!r && (e==DE_BREAK || stopped)) r=BREAK;
    if(!r && e) {err("\n*** Not able to backup file ***\n"); r=ERROR;}
    if(r) return r;
    update_fh(1,1); entries++;
    u8 a=(u8)(file.attr&~FA_ARCHIVE); dos_attribute(path,1,&a);
    return DONE;
}
static int wanted(const char *dir,const DosFind *f) {
    static const char *const system_files[]={"IBMBIO.COM","IBMDOS.COM","IO.SYS","MSDOS.SYS","COMMAND.COM","CMD.EXE"};
    if(f->attr&(FA_DIR|FA_VOLUME)) return 0;
    if(modified && !(f->attr&FA_ARCHIVE)) return 0;
    if(have_time && (!have_date || f->date==since_date) && f->time<since_time) return 0;
    if(have_date && f->date<since_date) return 0;
    if(strlen(dir)==3) for(unsigned i=0;i<ARRAY_SIZE(system_files);i++) if(!stricmp(f->name,system_files[i])) return 0;
    if(logging) {
        char path[DOS_PATH_MAX]; strcopy(path,sizeof(path),dir); strappend(path,sizeof(path),f->name);
        if(!stricmp(path,log_path)) return 0;
    }
    return 1;
}
static int each(void *ctx,const char *dir,const DosFind *f) {
    char path[DOS_PATH_MAX]; (void)ctx;
    if(stopped) {stop_code=BREAK; return STOP;}
    if(!wanted(dir,f)) return 0;
    if(stricmp(dir,directory)) {strcopy(directory,sizeof(directory),dir); new_directory=1;}
    file=*f;
    if(first_target) {
        int r=get_target(); if(r) {stop_code=r; return STOP;}
        first_target=0;
    }
    strcopy(path,sizeof(path),dir); strappend(path,sizeof(path),f->name);
    int r=backup_file(path); if(r) {stop_code=r; return STOP;}
    return 0;
}
/* /X or /X:value. */
static int switch_value(const char *arg,const char *name,const char **value) {
    unsigned n=(unsigned)strlen(name);
    if(!prefix(arg,name)) return 0;
    if(arg[n]==':') {*value=arg+n+1; return 1;}
    if(!arg[n]) {*value=NULL; return 1;}
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    append_files_only();
    char arg[DOS_PATH_MAX],source[DOS_PATH_MAX]="",target[DOS_PATH_MAX]="";
    const char *p=app_dos->command_tail(),*value; int have_target=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]!='/') {
            if(!source[0]) strcopy(source,sizeof(source),arg);
            else if(!have_target) {strcopy(target,sizeof(target),arg); have_target=1;}
            else {parse_error(PARSE_TOO_MANY,arg); return ERROR;}
            continue;
        }
        char c=(char)upper(arg[1]);
        /* A date's slashes would start new words: it runs on to the next blank. */
        if(c=='D' && arg[2]==':') {
            size_t n=strlen(arg);
            while(*p && *p!=' ' && *p!='\t' && n<sizeof(arg)-1) arg[n++]=*p++;
            arg[n]=0;
        }
        if((c=='S' || c=='M' || c=='A') && !arg[2]) *(c=='S'?&subdirs:c=='M'?&modified:&add)=1;
        else if(switch_value(arg,"/F",&value)) {format=1; if(value) strcopy(format_size,sizeof(format_size),value);}
        else if(switch_value(arg,"/D",&value) && value) {
            if(!parse_date(value,&since_date)) {err("\nInvalid date\n"); return ERROR;}
            have_date=1;
        } else if(switch_value(arg,"/T",&value) && value) {
            if(!parse_time(value,&since_time)) {err("\nInvalid time\n"); return ERROR;}
            have_time=1;
        } else if(switch_value(arg,"/L",&value)) {logging=1; if(value) strcopy(log_path,sizeof(log_path),value);}
        else {parse_error(PARSE_SWITCH,arg); return ERROR;}
    }
    if(!source[0]) {err("\nNo source drive specified\n"); return ERROR;}
    if(!have_target) {err("\nNo target drive specified\n"); return ERROR;}
    /* Devices are no source. */
    static const char *const devices[]={"LPT1","LPT2","PRN","CON","NUL","AUX","LPT1:","LPT2:","PRN:","CON:","NUL:","AUX:"};
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) {
        size_t n=strlen(devices[i]),length=strlen(source);
        if(length>=n && !stricmp(source+length-n,devices[i])) {parse_error(PARSE_PARAMETER,source+length-n); return ERROR;}
    }
    source_letter=(char)(source[1]==':'?upper(source[0]):'A'+(char)dos_current_drive());
    target_letter=(char)upper(target[0]);
    if(target[1]!=':' || target[2] || target_letter<'A' || target_letter>'Z') {err("\nInvalid drive specification\n"); return ERROR;}
    if(source_letter==target_letter) {err("\nSource and target drives are the same\n"); return ERROR;}
    DosDeviceParams params={.size=sizeof(params)}; DosDriveInfo info;
    if(source_letter<'A' || source_letter>'Z' || dos_drive_info((u32)(source_letter-'A'),&info) ||
       dos_device_params((unsigned)(target_letter-'A'),&params)) {err("\nInvalid drive specification\n"); return ERROR;}
    removable=!(params.attributes&DOS_DEVICE_NONREMOVABLE);
    if(format && !removable) {to_stderr(1); print("\nCannot FORMAT nonremovable drive %c:\n",target_letter); to_stderr(0); return ERROR;}
    for(const char *s=source;*s;s++) if(s[0]=='\\' && s[1]=='\\') {err("\nInvalid path\n"); return ERROR;}
    if((source[1]==':' && !source[2]) || source[strlen(source)-1]=='\\') strappend(source,sizeof(source),"*.*");
    if(file_spec(source,source_dir,pattern,1)) {err("\nInvalid path\n"); return ERROR;}
    if(logging) {
        if(!log_path[0]) {log_path[0]=source_letter; log_path[1]=':'; log_path[2]=0; strappend(log_path,sizeof(log_path),"\\BACKUP.LOG");}
        char full[DOS_PATH_MAX]; if(!dos_canonical(log_path,full)) strcopy(log_path,sizeof(log_path),full);
        log_on_target=upper(log_path[0])==target_letter;
    }
    if(dos_alloc(buffer_size/16,(void **)&buffer)) {err("\nInsufficient memory\n"); return ERROR;}
    DosBreakHandler handler={ctrl_c,NULL},previous; int handled=!dos_break_handler(&handler,&previous);
    int r=DONE;
    if(logging && !log_on_target && open_log()) r=ERROR;
    if(!r) {
        int e=walk(source_dir,pattern,FA_HIDDEN|FA_SYSTEM,subdirs,0,each,NULL);
        if(e==STOP) r=stop_code;
        else if(stopped) r=BREAK;
        else if(e && e!=DE_NOFILE && e!=DE_NOMORE) {err("\nInvalid path\n"); r=ERROR;}
    }
    if(first_target && !r) {err("\nWarning! No files were found to back up\n"); r=NO_FILES;}
    if(control!=0xffff) finish_target(1);
    close_log();
    if(handled) dos_break_handler(&previous,NULL);
    dos_free(buffer);
    if(!first_target) print("\n");
    return (EFI_STATUS)(r?r:result);
}
