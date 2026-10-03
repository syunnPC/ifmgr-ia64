/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Native command interpreter. Syntax, messages and batch semantics follow
 * MS-DOS 4.0 COMMAND.COM (v4.0/src/CMD/COMMAND); no code is shared.
 */
#include "command.h"
#include "calendar.h"
#define REPORTED 257 /* The command printed its own message. */
#define BATCH_ARGS 64
static int shell_running=1,permanent;
static int echo_on=1;
static int interrupted;
static DosBreakHandler previous_handler;
static DosCriticalHandler previous_critical;
static u32 shell_pid;
static const u8 *dbcs; /* AH=63h: kernel view of the active lead ranges. */
static void critical_text(const IoServices *io,const char *text) {io->console_write(io->context,text,strlen(text));}
static unsigned critical_handler(void *context,const DosCriticalError *event) {
    (void)context; const IoServices *io=dos_io_services();
    if(!io || !io->console_write || !io->console_read) return DOS_CRITICAL_FAIL;
    critical_text(io,"\nCritical disk error ");
    char number[11],drive[4]={'A'+event->drive,':',0,0}; unsigned n=sizeof(number),value=event->error;
    number[--n]=0; do {number[--n]='0'+value%10; value/=10;} while(value);
    critical_text(io,number+n); critical_text(io," on "); critical_text(io,drive);
    const char *operations[]={"read","write","flush","media"};
    critical_text(io," ("); critical_text(io,event->operation<4?operations[event->operation]:"I/O"); critical_text(io,").\n");
    if(event->allowed&(1U<<DOS_CRITICAL_RETRY)) critical_text(io,"[R]etry ");
    if(event->allowed&(1U<<DOS_CRITICAL_IGNORE)) critical_text(io,"[I]gnore ");
    critical_text(io,"[F]ail [A]bort. Action: ");
    for(;;) {
        int c=io->console_read(io->context); if(c<0) return DOS_CRITICAL_FAIL;
        c=upper(c); unsigned action;
        if(c=='R') action=DOS_CRITICAL_RETRY;
        else if(c=='I') action=DOS_CRITICAL_IGNORE;
        else if(c=='F') action=DOS_CRITICAL_FAIL;
        else if(c=='A' || c==3) action=DOS_CRITICAL_ABORT;
        else continue;
        if(!(event->allowed&(1U<<action))) continue;
        char answer[3]={(char)c,'\n',0}; critical_text(io,answer);
        /* A built-in command belongs to this shell: cancel its batch without
         * ending the shell; a child program can end normally. */
        if(action==DOS_CRITICAL_ABORT && event->pid==shell_pid) {interrupted=1; return DOS_CRITICAL_FAIL;}
        return action;
    }
}
static int break_handler(void *context) {(void)context; interrupted=1; return DOS_BREAK_CANCEL;}
static int finish_shell(int result) {
    dos_critical_handler(&previous_critical,NULL); dos_break_handler(&previous_handler,NULL);
    print_sink=NULL; return result;
}
/* DBCS-aware scanning: a trail byte such as 5Ch or 7Ch is never a separator. */
static int lead(u8 c) {
    if(!dbcs || c<0x80) return 0;
    for(unsigned i=0;i+1<512 && (dbcs[i] || dbcs[i+1]);i+=2) if(c>=dbcs[i] && c<=dbcs[i+1]) return 1;
    return 0;
}
static unsigned step(const char *p) {return lead((u8)*p) && p[1]?2:1;}
static char *last_sep(const char *s) {
    const char *last=NULL;
    for(;*s;s+=step(s)) if(*s=='\\' || *s=='/' || *s==':') last=s;
    return (char *)last;
}
static char *leaf(const char *s) {char *p=last_sep(s); return p?p+1:(char *)s;}
static int wild(const char *s) {
    for(;*s;s+=step(s)) if(*s=='*' || *s=='?') return 1;
    return 0;
}
static const char *cwd(void) {
    static char path[DOS_PATH_MAX];
    if(dos_full_path("",path)) strcopy(path,sizeof(path),"?");
    return path;
}
typedef struct Batch {
    char *text; size_t size,pc; unsigned depth,shift,argc;
    struct Batch *parent;
    char args[512]; unsigned argv[BATCH_ARGS];
    int chain; char chain_name[DOS_PATH_MAX],chain_zero[DOS_PATH_MAX],chain_args[256];
} Batch;
static Batch *batch;
static unsigned command_depth;
static int output_error,calling,for_active;
static int emit(const void *buf,u32 size) {u32 n; int e=dos_write(1,buf,size,&n); return e?e:n==size?0:DE_FULL;}
static int say(const char *s) {return emit(s,strlen(s));}
static void shell_print(const void *p,size_t n) {
    int e=emit(p,n); if(e && !output_error) output_error=e;
}
static void error(int e) {if(e && e!=DE_BREAK && e!=REPORTED) print("%s (DOS error %u)\n",dos_error(e),(unsigned long long)e);}
static int report(const char *message) {say(message); return REPORTED;}
static int separator(char c) {return c==' ' || c=='\t';}
/* DOS argument delimiters for batch parameters, IF and FOR lists. */
static int delimiter(char c) {return c==' ' || c=='\t' || c==',' || c==';' || c=='=';}
static char *trim(char *s) {
    while(separator(*s)) s++;
    size_t n=strlen(s); while(n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r')) s[--n]=0;
    return s;
}
static char *word(char **cursor) {
    char *p=*cursor; while(separator(*p)) p++;
    if(!*p) {*cursor=p; return NULL;}
    char *start=p;
    if(*p=='"') {start=++p; while(*p && *p!='"') p+=step(p);}
    else while(*p && !separator(*p)) p+=step(p);
    if(*p) *p++=0;
    *cursor=p; return start;
}
static char *number(char *p,u64 value,unsigned width,char pad) {
    char digits[24]; unsigned n=0;
    do {digits[n++]='0'+value%10; value/=10;} while(value);
    while(width>n) {*p++=pad; width--;}
    while(n) *p++=digits[--n];
    *p=0; return p;
}
static void country(DosCountryInfo *info) {
    if(!dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,info)) return;
    memset(info,0,sizeof(*info)); info->date_separator[0]='-'; info->time_separator[0]=':'; info->decimal[0]='.';
}
/* Country date order; short years for DIR, four digits for $D. */
static void format_date(char *p,unsigned year,unsigned month,unsigned day,int full) {
    DosCountryInfo c; country(&c); char sep=c.date_separator[0]?c.date_separator[0]:'-';
    unsigned y=full?year:year%100,fields[3];
    if(c.date_order==1) {fields[0]=day; fields[1]=month; fields[2]=y;}
    else if(c.date_order==2) {fields[0]=y; fields[1]=month; fields[2]=day;}
    else {fields[0]=month; fields[1]=day; fields[2]=y;}
    for(unsigned i=0;i<3;i++) {
        if(i) *p++=sep;
        p=number(p,fields[i],c.date_order==2 && !i?(full?4:2):2,i || full || c.date_order==2?'0':' ');
    }
}
static void format_time(char *p,unsigned hour,unsigned minute,int seconds,unsigned second,unsigned hundredth) {
    DosCountryInfo c; country(&c); char sep=c.time_separator[0]?c.time_separator[0]:':';
    int twelve=!seconds && !(c.time_format&1);
    unsigned h=twelve?(hour%12?hour%12:12):hour;
    p=number(p,h,2,' '); *p++=sep; p=number(p,minute,2,'0');
    if(seconds) {*p++=sep; p=number(p,second,2,'0'); *p++=c.decimal[0]?c.decimal[0]:'.'; p=number(p,hundredth,2,'0');}
    if(twelve) {*p++=hour<12?'a':'p'; *p=0;}
}
static int print_prompt(void) {
    char text[256];
    if(dos_env_get("PROMPT",text,sizeof(text)) || !*text) strcopy(text,sizeof(text),"$P$G");
    for(char *p=text;*p;) {
        if(step(p)==2) {emit(p,2); p+=2; continue;}
        if(*p!='$') {emit(p++,1); continue;}
        if(!*++p) break;
        char c=upper(*p++),buffer[DOS_PATH_MAX];
        switch(c) {
        case 'P': if(dos_full_path("",buffer)) say("Current drive is no longer valid"); else say(buffer); break;
        case 'N': {char drive[2]={'A'+dos_current_drive(),0}; say(drive); break;}
        case 'G': say(">"); break;
        case 'L': say("<"); break;
        case 'B': say("|"); break;
        case 'Q': say("="); break;
        case '$': say("$"); break;
        case '_': say("\n"); break;
        case 'E': say("\x1b"); break;
        case 'H': say("\b \b"); break;
        case 'V': say("DOS Version 4.00"); break;
        case 'D': case 'T': {
            DosDateTime t; if(dos_get_datetime(&t)) break;
            if(c=='D') {
                static const char days[]="SunMonTueWedThuFriSat";
                emit(days+3*(t.weekday%7),3); say(" ");
                format_date(buffer,t.year,t.month,t.day,1);
            } else format_time(buffer,t.hour,t.minute,1,t.second,t.hundredth);
            say(buffer); break;
        }
        default: break; /* DOS ignores unknown $ codes. */
        }
    }
    return output_error;
}
static int clock_number(const char **text,unsigned low,unsigned high,unsigned *out) {
    const char *p=*text; unsigned n=0,value=0;
    while(*p>='0' && *p<='9') {
        if(n==high) return 0;
        value=value*10+(*p++-'0'); n++;
    }
    if(n<low) return 0;
    *out=value; *text=p; return 1;
}
static int parse_clock(const char *text,int date,unsigned values[4]) {
    const char *p=text; memset(values,0,4*sizeof(*values));
    if(date) {
        if(!clock_number(&p,4,4,&values[0]) || *p++!='-' ||
           !clock_number(&p,2,2,&values[1]) || *p++!='-' ||
           !clock_number(&p,2,2,&values[2]) || *p) return 0;
        return values[0]>=1980 && values[0]<=2099 && calendar_date(values[0],values[1],values[2]);
    }
    if(!clock_number(&p,1,2,&values[0]) || *p++!=':' || !clock_number(&p,1,2,&values[1])) return 0;
    if(*p==':') {
        p++; if(!clock_number(&p,1,2,&values[2])) return 0;
        if(*p=='.') {
            const char *fraction=++p;
            if(!clock_number(&p,1,2,&values[3])) return 0;
            if(p-fraction==1) values[3]*=10;
        }
    }
    return !*p && calendar_time(values[0],values[1],values[2],0);
}
static int cmd_clock(char *args,int date) {
    const char *format=date?"YYYY-MM-DD (1980-2099)":"HH:MM[:SS[.hh]]";
    if(!stricmp(args,"/?")) {print("%s [%s | /T]\n",date?"DATE":"TIME",format); return 0;}
    int prompt=!*args;
    if(prompt || !stricmp(args,"/T")) {
        DosDateTime t; int e=dos_get_datetime(&t); if(e) return e;
        if(date) print("%04u-%02u-%02u\n",(unsigned long long)t.year,(unsigned long long)t.month,(unsigned long long)t.day);
        else print("%02u:%02u:%02u.%02u\n",(unsigned long long)t.hour,(unsigned long long)t.minute,
            (unsigned long long)t.second,(unsigned long long)t.hundredth);
        if(!prompt) return 0;
    }
    for(;;) {
        u8 input[82]={80,0};
        if(prompt) {
            print("Enter new %s (%s), or press Enter to keep it: ",date?"date":"time",format);
            if(output_error) return output_error;
            int e=dos_line_input(input); if(e==DE_EOF) return 0; if(e) return e;
            e=say("\n"); if(e) return e;
            input[2+input[1]]=0; args=trim((char *)input+2); if(!*args) return 0;
        }
        unsigned values[4];
        if(parse_clock(args,date,values))
            return date?dos_set_date(values[0],values[1],values[2]):dos_set_time(values[0],values[1],values[2],values[3]);
        print("Invalid %s; use %s.\n",date?"date":"time",format);
        if(!prompt) return DE_MODE;
        if(output_error) return output_error;
    }
}
static const char *getenv_dos(const char *key) {
    static char value[DOS_ENV_CAPACITY];
    if(dos_env_get(key,value,sizeof(value))) value[0]=0;
    return value;
}
static int env_name(const char *s,size_t n) {
    if(!n || n>DOS_ENV_NAME_MAX) return 0;
    for(size_t i=0;i<n;i++) if((u8)s[i]<=32 || (u8)s[i]>126 || s[i]=='=') return 0;
    return 1;
}
static const char *param(const Batch *b,unsigned n) {
    n+=b->shift; return n<b->argc?b->args+b->argv[n]:"";
}
/* Batch lines (TBATCH.ASM): %% is %, %0-%9 are parameters, %NAME% is the
 * environment, and an unterminated % is dropped. Interactive lines keep the
 * native extension of expanding valid %NAME% references only. */
static int expand(const char *s,char *out,size_t cap,const Batch *b) {
    size_t n=0;
    while(*s) {
        unsigned width=step(s);
        if(width==1 && *s=='%') {
            const char *value=NULL; const char *next=NULL;
            if(s[1]=='%') {value="%"; next=s+2;}
            else if(b && s[1]>='0' && s[1]<='9') {value=param(b,s[1]-'0'); next=s+2;}
            else {
                const char *end=s+1; while(*end && *end!='%') end+=step(end);
                if(*end=='%' && end>s+1 && (b || env_name(s+1,end-s-1))) {
                    char key[DOS_ENV_NAME_MAX+1]; size_t length=end-s-1;
                    if(length>DOS_ENV_NAME_MAX) value="";
                    else {memcpy(key,s+1,length); key[length]=0; value=env_name(key,length)?getenv_dos(key):"";}
                    next=end+1;
                } else if(b && !*end) {s++; continue;}
            }
            if(value) {
                size_t length=strlen(value); if(n+length>=cap) return DE_PATH;
                memcpy(out+n,value,length); n+=length; s=next; continue;
            }
        }
        if(n+width>=cap) return DE_PATH;
        memcpy(out+n,s,width); n+=width; s+=width;
    }
    out[n]=0; return 0;
}
/* A '/' starts a switch only when a known letter follows and ends the token,
 * so native forward-slash paths such as C:/LAB/FILE keep working. */
static char switch_at(const char *p,const char *letters) {
    if(*p!='/' || !p[1]) return 0;
    char c=upper(p[1]);
    if(!strchr(letters,c) || (p[2] && !separator(p[2]) && p[2]!='/' && p[2]!='+' && p[2]!=',')) return 0;
    return c;
}
static int ask(const char *question) {
    for(;;) {
        int e=say(question); if(e) return -e;
        u8 input[16]={14,0}; e=dos_line_input(input);
        if(e==DE_EOF) {say("\n"); return 0;}
        if(e) return -e;
        say("\n"); input[2+input[1]]=0;
        char c=upper(*trim((char *)input+2));
        if(c=='Y') return 1;
        if(c=='N') return 0;
    }
}
static int pause_key(void) {
    int e=say("Press any key to continue . . ."); if(e) return e;
    DosRegs r={.ax=0x0800}; e=dos_call(&r); return e?e:say("\n");
}
/* Volume labels come from an extended-FCB search: the DTA keeps all eleven
 * raw bytes, including embedded blanks a dotted 8.3 name cannot express. */
static int volume_label(unsigned drive,char label[12]) {
    void *saved; u32 capacity; if(dos_get_dta(&saved,&capacity)) return 0;
    u8 dta[64],fcb[44]={0xff,0,0,0,0,0,FA_VOLUME,(u8)(drive+1)}; memset(fcb+8,'?',11);
    unsigned status=255; int e=dos_set_dta(dta,sizeof(dta));
    if(!e) e=dos_fcb_call(0x11,fcb,NULL,&status);
    dos_set_dta(saved,capacity);
    if(e || status) return 0;
    memcpy(label,dta+8,11); if((u8)label[0]==5) label[0]=(char)0xe5;
    unsigned n=11; while(n && label[n-1]==' ') n--;
    label[n]=0; return 1;
}
static int show_volume(unsigned drive) {
    char label[12]; u32 serial=0;
    DosMediaId id; memset(&id,0,sizeof(id)); id.size=sizeof(id);
    DosRegs r={.ax=0x440d,.bx=drive+1,.cx=0x0866,.dx=(uintptr_t)&id};
    int has_serial=!dos_call(&r); serial=id.serial;
    if(volume_label(drive,label)) print("\n Volume in drive %c is %s\n",(int)('A'+drive),label);
    else print("\n Volume in drive %c has no label\n",(int)('A'+drive));
    if(has_serial) print(" Volume Serial Number is %04x-%04x\n",(unsigned long long)(serial>>16),(unsigned long long)(serial&0xffff));
    return output_error;
}
static int cmd_vol(char *args) {
    char *p=word(&args); if(word(&args)) return report("Too many parameters\n");
    unsigned drive=dos_current_drive();
    if(p) {
        if(strlen(p)!=2 || p[1]!=':') return report("Invalid drive specification\n");
        drive=(unsigned)(upper(p[0])-'A');
        DosDriveInfo info; if(drive>=DOS_DRIVES || dos_drive_info(drive,&info)) return report("Invalid drive specification\n");
    }
    return show_volume(drive);
}
/* Fixed 8.3 display: NAME padded to eight, a blank, EXT padded to three. */
static void display_name(const char *name,char out[13]) {
    memset(out,' ',12); out[12]=0;
    if(name[0]=='.') {memcpy(out,name,strlen(name)); return;}
    unsigned n=0,base=0;
    for(const char *p=name;*p && n<12;) {
        if(*p=='.' && !base) {base=9; n=9; p++; continue;}
        unsigned width=step(p);
        for(unsigned i=0;i<width && n<12;i++) out[n++]=*p++;
    }
}
static int dir_pause(int enabled,unsigned *lines) {
    if(!enabled || ++*lines<23) return 0;
    *lines=0; return pause_key();
}
static int cmd_dir(char *args) {
    int wide=0,pause=0,bare=0; char *spec=NULL,*p;
    while((p=word(&args))) {
        for(char *q=p;*q;) {
            char c=switch_at(q,"WPB");
            if(!c) {if(*q=='/' && q==p) return report("Invalid switch\n"); q+=step(q); continue;}
            if(c=='W') wide=1; else if(c=='P') pause=1; else bare=1;
            memmove(q,q+2,strlen(q+2)+1);
        }
        if(!*p) continue;
        if(spec) return report("Too many parameters\n");
        spec=p;
    }
    char pattern[DOS_PATH_MAX]; int e=strcopy(pattern,sizeof(pattern),spec?spec:"*.*"); if(e) return e;
    u8 attr; char *tail=leaf(pattern);
    if((!dos_attribute(pattern,0,&attr) && (attr&FA_DIR)) || !*tail) {
        if(*tail) {e=strappend(pattern,sizeof(pattern),"\\"); if(e) return e;}
        e=strappend(pattern,sizeof(pattern),"*.*"); if(e) return e;
    } else if(tail[0]=='.' && tail[1] && strcmp(tail,"..")) {
        char rest[DOS_PATH_MAX]; strcopy(rest,sizeof(rest),tail);
        *tail=0; e=strappend(pattern,sizeof(pattern),"*"); if(!e) e=strappend(pattern,sizeof(pattern),rest); if(e) return e;
    } else {
        int dot=0; for(char *q=tail;*q;q+=step(q)) if(*q=='.') dot=1;
        if(!dot) {e=strappend(pattern,sizeof(pattern),".*"); if(e) return e;}
    }
    char directory[DOS_PATH_MAX],resolved[DOS_PATH_MAX];
    strcopy(directory,sizeof(directory),pattern); *leaf(directory)=0;
    /* As the program sees it: a SUBST letter's own directories. */
    e=dos_full_path(directory,resolved); if(e) return e;
    unsigned drive=(unsigned)(resolved[0]-'A');
    if(!bare) {
        e=show_volume(drive); if(e) return e;
        print(" Directory of  %s\n\n",resolved);
    }
    DosFind find; e=dos_find_first(pattern,FA_DIR,&find);
    unsigned files=0,column=0,lines=0;
    while(!e) {
        files++;
        if(bare) {say(find.name); e=say("\n");}
        else {
            char name[13]; display_name(find.name,name); say(name);
            if(wide) {
                if(++column==5) {column=0; say("\n"); e=dir_pause(pause,&lines);}
                else say("\t");
            } else {
                char line[48],date[16],time[16],*q=line;
                if(find.attr&FA_DIR) {strcopy(line,sizeof(line)," <DIR>    "); q=line+10;}
                else q=number(q,find.size,10,' ');
                if(find.date) {
                    format_date(date,1980+(find.date>>9),(find.date>>5)&15,find.date&31,0);
                    format_time(time,find.time>>11,(find.time>>5)&63,0,0,0);
                    *q++=' '; for(size_t n=strlen(date);n<8;n++) *q++=' ';
                    strcopy(q,16,date); q+=strlen(q); *q++=' '; *q++=' ';
                    for(size_t n=strlen(time);n<6;n++) *q++=' ';
                    strcopy(q,16,time);
                }
                say(line); say("\n"); e=dir_pause(pause,&lines);
            }
        }
        if(!e) e=output_error;
        if(!e) e=dos_find_next(&find);
    }
    if(e!=DE_NOMORE && e!=DE_NOFILE) return e;
    if(!files) return bare?0:report("File not found\n");
    if(bare) return 0;
    if(wide && column) say("\n");
    DosDriveInfo info; e=dos_drive_info(drive,&info); if(e) return e;
    print("%9u File(s) %10u bytes free\n",(unsigned long long)files,
        (unsigned long long)info.free_clusters*info.sectors_per_cluster*512);
    return 0;
}
static int cmd_type(char *args) {
    char *name=word(&args); if(!name) return report("Required parameter missing\n");
    if(word(&args)) return report("Too many parameters\n");
    if(wild(name)) return report("Invalid filename or file not found\n");
    unsigned h; int e=dos_open(name,0,0,&h); if(e) return e;
    u8 buf[1024]; u32 n; int end=0;
    while(!end && !(e=dos_read(h,buf,sizeof(buf),&n)) && n) {
        for(u32 i=0;i<n;i++) if(buf[i]==26) {n=i; end=1; break;}
        e=emit(buf,n); if(e) break;
    }
    int c=dos_close(h); return e?e:c;
}
/* 8.3 wildcard template (REN/COPY): '?' copies the source character. */
static void fields(const char *name,char out[11],int pattern) {
    memset(out,' ',11); unsigned n=0,end=8;
    for(const char *p=name;*p;) {
        if(*p=='.' && end==8) {n=8; end=11; p++; continue;}
        if(*p=='*' && pattern) {while(n<end) out[n++]='?'; p++; continue;}
        unsigned width=step(p);
        for(unsigned i=0;i<width;i++,p++) if(n<end) out[n++]=*p;
    }
}
static void apply_template(const char *name,const char *pattern,char out[13]) {
    char source[11],mask[11],result[11]; fields(name,source,0); fields(pattern,mask,1);
    for(unsigned i=0;i<11;i++) result[i]=mask[i]=='?'?source[i]:mask[i];
    unsigned n=0;
    for(unsigned i=0;i<8 && result[i]!=' ';i++) out[n++]=result[i];
    if(result[8]!=' ') {out[n++]='.'; for(unsigned i=8;i<11 && result[i]!=' ';i++) out[n++]=result[i];}
    out[n]=0;
}
static int join(char out[DOS_PATH_MAX],const char *directory,size_t length,const char *name) {
    if(length>=DOS_PATH_MAX) return DE_PATH;
    memcpy(out,directory,length); out[length]=0; return strappend(out,DOS_PATH_MAX,name);
}
static int cmd_ren(char *args) {
    char *a=word(&args),*b=word(&args); if(!a || !b) return report("Required parameter missing\n");
    if(word(&args)) return report("Too many parameters\n");
    if(!wild(a) && !wild(b)) {
        char target[DOS_PATH_MAX];
        if(!last_sep(b)) {
            int e=join(target,a,leaf(a)-a,b); if(e) return e; b=target;
        }
        return dos_rename(a,b);
    }
    if(last_sep(b)) return report("Invalid parameter\n");
    DosFind find; int e=dos_find_first(a,0,&find),matched=0,failed=0;
    size_t prefix=leaf(a)-a;
    while(!e) {
        matched++; char name[13],from[DOS_PATH_MAX],to[DOS_PATH_MAX]; apply_template(find.name,b,name);
        if(strcmp(name,find.name)) {
            int x=join(from,a,prefix,find.name); if(!x) x=join(to,a,prefix,name);
            if(!x) x=dos_rename(from,to);
            if(x) failed=1;
        }
        e=dos_find_next(&find);
    }
    if(e!=DE_NOMORE && e!=DE_NOFILE) return e;
    if(failed || !matched) return report("Duplicate file name or file not found\n");
    return 0;
}
static int all_files(const char *pattern) {
    char mask[11]; fields(pattern,mask,1);
    for(unsigned i=0;i<11;i++) if(mask[i]!='?') return 0;
    return 1;
}
static int cmd_del(char *args) {
    int prompt=0; char *spec=NULL,*p;
    while((p=word(&args))) {
        for(char *q=p;*q;) {
            if(!switch_at(q,"P")) {if(*q=='/' && q==p) return report("Invalid switch\n"); q+=step(q); continue;}
            prompt=1; memmove(q,q+2,strlen(q+2)+1);
        }
        if(!*p) continue;
        if(spec) return report("Too many parameters\n");
        spec=p;
    }
    if(!spec) return report("Required parameter missing\n");
    if(strlen(spec)==2 && spec[1]==':') return report("File not found\n");
    char pattern[DOS_PATH_MAX]; int e=strcopy(pattern,sizeof(pattern),spec); if(e) return e;
    u8 attr;
    if(!dos_attribute(pattern,0,&attr) && (attr&FA_DIR)) {e=strappend(pattern,sizeof(pattern),"\\*.*"); if(e) return e;}
    else if(!*leaf(pattern)) {e=strappend(pattern,sizeof(pattern),"*.*"); if(e) return e;}
    else if(!wild(pattern)) {
        if(!prompt) return dos_remove(pattern,0);
    }
    if(!prompt && all_files(leaf(pattern))) {
        int answer=ask("All files in directory will be deleted!\nAre you sure (Y/N)?");
        if(answer<=0) return answer<0?-answer:0;
    }
    char prefix[DOS_PATH_MAX],display[DOS_PATH_MAX];
    size_t length=leaf(pattern)-pattern; memcpy(prefix,pattern,length); prefix[length]=0;
    e=dos_full_path(*prefix?prefix:".",display); if(e) return e;
    if(strlen(display)>3) {e=strappend(display,sizeof(display),"\\"); if(e) return e;}
    DosFind find; e=dos_find_first(pattern,0,&find);
    if(e==DE_NOMORE || e==DE_NOFILE) return report("File not found\n");
    while(!e) {
        char path[DOS_PATH_MAX]; e=join(path,prefix,length,find.name); if(e) return e;
        int remove=1;
        if(prompt) {
            char question[DOS_PATH_MAX+32]; strcopy(question,sizeof(question),display);
            strappend(question,sizeof(question),find.name); strappend(question,sizeof(question),",    Delete (Y/N)?");
            remove=ask(question); if(remove<0) return -remove;
        }
        if(remove) {e=dos_remove(path,0); if(e) return e;}
        e=dos_find_next(&find);
    }
    return e==DE_NOMORE?0:e;
}
/* COPY (COPY.ASM semantics): '+' concatenates; /A and /B apply to the
 * preceding name and all later names; /V verifies the whole operation. */
typedef struct {char *name; unsigned group; char mode;} CopyItem;
static int copy_stream(unsigned in,unsigned out,int ascii) {
    u8 buf[2048]; u32 got,written; int end=0,e;
    while(!end && !(e=dos_read(in,buf,sizeof(buf),&got)) && got) {
        if(ascii) for(u32 i=0;i<got;i++) if(buf[i]==26) {got=i; end=1; break;}
        e=dos_write(out,buf,got,&written); if(e) return e;
        if(written!=got) return DE_FULL;
    }
    return e;
}
static int copy_into(unsigned out,const char *source,int ascii,int *found) {
    unsigned in; int e=dos_open(source,0,0,&in); if(e) return e;
    if(found) *found=1;
    e=copy_stream(in,out,ascii);
    int c=dos_close(in); return e?e:c;
}
/* An ASCII destination ends with ^Z; a zero-length write sets the size. */
static int copy_finish(unsigned out,int ascii,int truncate,int e) {
    u32 written;
    if(!e && ascii) {e=dos_write(out,"\x1a",1,&written); if(!e && written!=1) e=DE_FULL;}
    if(!e && truncate) e=dos_write(out,"",0,&written);
    int c=dos_close(out); return e?e:c;
}
static int same_file(const char *a,const char *b) {
    char x[DOS_PATH_MAX],y[DOS_PATH_MAX];
    return !dos_canonical(a,x) && !dos_canonical(b,y) && !strcmp(x,y);
}
static int is_device(const char *path) {u8 attr; return !dos_attribute(path,0,&attr) && attr==0x40;}
static int copy_one(const char *source,char smode,const char *target,char dmode) {
    int device=is_device(source),target_device=is_device(target);
    if(device && smode=='B') return report("Cannot do binary reads from a device\n");
    unsigned in,out; int e=dos_open(source,0,0,&in); if(e) return e;
    e=dos_open(target,1,1,&out); if(e) {dos_close(in); return e;}
    e=copy_stream(in,out,smode=='A' || device);
    /* A single copy keeps the source's directory time stamp. */
    u16 date,time;
    if(!e && !device && !target_device && !dos_file_time(in,0,&date,&time)) e=dos_file_time(out,1,&date,&time);
    int c=dos_close(in); if(!e) e=c;
    e=copy_finish(out,dmode=='A',0,e);
    if(e && !target_device) dos_remove(target,0);
    return e;
}
static int copy_target(const char *dest,int directory,const char *name,char out[DOS_PATH_MAX]) {
    if(!dest) return strcopy(out,DOS_PATH_MAX,name);
    if(directory) {
        int e=strcopy(out,DOS_PATH_MAX,dest); if(e) return e;
        size_t n=strlen(out);
        if(n && out[n-1]!='\\' && out[n-1]!='/' && out[n-1]!=':') {e=strappend(out,DOS_PATH_MAX,"\\"); if(e) return e;}
        return strappend(out,DOS_PATH_MAX,name);
    }
    if(wild(leaf(dest))) {char renamed[13]; apply_template(name,leaf(dest),renamed); return join(out,dest,leaf(dest)-dest,renamed);}
    return strcopy(out,DOS_PATH_MAX,dest);
}
static int copied(unsigned count) {print("%9u file(s) copied.\n",(unsigned long long)count); return output_error;}
static int copy_files(CopyItem *items,unsigned count) {
    unsigned sources=0; CopyItem *dest=NULL;
    for(unsigned i=0;i<count;i++) {
        if(items[i].group==0) sources++;
        else if(items[i].group==1 && !dest) dest=&items[i];
        else return report(items[i].group>1?"Too many parameters\n":"Invalid parameter\n");
    }
    if(!sources) return report("Required parameter missing\n");
    u8 attr; int dest_device=dest && is_device(dest->name),dest_dir=0;
    if(dest && !dest_device) {
        size_t n=strlen(dest->name); char last=n?dest->name[n-1]:0;
        dest_dir=(!dos_attribute(dest->name,0,&attr) && (attr&FA_DIR)) || last=='\\' || last=='/' || (n==2 && last==':');
    }
    int concat=sources>1 || (wild(items[0].name) && dest && !dest_dir && !dest_device && !wild(dest->name));
    char default_mode=concat?'A':'B';
    if(!concat) {
        CopyItem *s=&items[0]; char dmode=dest && dest->mode?dest->mode:default_mode;
        char smode=s->mode?s->mode:is_device(s->name)?'A':default_mode; /* Devices default to /A. */
        unsigned done=0; char target[DOS_PATH_MAX];
        if(!wild(s->name)) {
            int e=copy_target(dest?dest->name:NULL,dest_dir,leaf(s->name),target); if(e) return e;
            if(!is_device(s->name) && !is_device(target) && same_file(s->name,target)) {
                say("File cannot be copied onto itself\n"); copied(0); return DE_ACCESS;
            }
            e=copy_one(s->name,smode,target,dmode); if(e) return e;
            return copied(1);
        }
        DosFind find; int e=dos_find_first(s->name,0,&find);
        if(e==DE_NOMORE || e==DE_NOFILE) {say("File not found\n"); copied(0); return REPORTED;}
        size_t prefix=leaf(s->name)-s->name;
        while(!e) {
            char source[DOS_PATH_MAX]; e=join(source,s->name,prefix,find.name); if(e) break;
            e=copy_target(dest?dest->name:NULL,dest_dir,find.name,target); if(e) break;
            if(same_file(source,target)) {say("File cannot be copied onto itself\n"); e=DE_ACCESS; break;}
            print("%s\n",source);
            e=copy_one(source,smode,target,dmode); if(e) break;
            done++; e=dos_find_next(&find);
        }
        if(e==DE_NOMORE) e=0;
        int x=copied(done); return e?e:x;
    }
    /* Concatenation: without a destination, append to the first source. */
    char target[DOS_PATH_MAX]; int e;
    if(dest) e=copy_target(dest->name,dest_dir,leaf(items[0].name),target);
    else if(wild(items[0].name)) {
        DosFind find; e=dos_find_first(items[0].name,0,&find);
        if(!e) e=join(target,items[0].name,leaf(items[0].name)-items[0].name,find.name);
    } else e=strcopy(target,sizeof(target),items[0].name);
    if(e) return e==DE_NOMORE?DE_NOFILE:e;
    char dmode=dest && dest->mode?dest->mode:default_mode;
    char first[DOS_PATH_MAX]; unsigned out;
    if(wild(items[0].name)) {
        DosFind find; e=dos_find_first(items[0].name,0,&find);
        if(!e) e=join(first,items[0].name,leaf(items[0].name)-items[0].name,find.name);
        if(e) first[0]=0;
    } else strcopy(first,sizeof(first),items[0].name);
    int append=*first && same_file(first,target);
    if(append) {
        e=dos_open(target,2,0,&out); if(e) return e;
        u32 position=0,end=0;
        if(items[0].mode!='B') {
            u8 buf[512]; u32 got; int found=0;
            while(!found && !(e=dos_read(out,buf,sizeof(buf),&got)) && got) {
                for(u32 i=0;i<got;i++) if(buf[i]==26) {position+=i; found=1; break;}
                if(!found) position+=got;
            }
            if(!e) e=dos_seek(out,position,0,&end);
        } else e=dos_seek(out,0,2,&end);
        if(e) {dos_close(out); return e;}
    } else {e=dos_open(target,1,1,&out); if(e) return e;}
    int any=append;
    for(unsigned i=0;i<count && !e;i++) {
        CopyItem *s=&items[i]; if(s->group) continue;
        int ascii=(s->mode?s->mode:default_mode)=='A' || is_device(s->name);
        if(!wild(s->name)) {
            if(i==0 && append) continue;
            if(same_file(s->name,target)) {say("Content of destination lost before copy\n"); continue;}
            print("%s\n",s->name); e=copy_into(out,s->name,ascii,&any);
            continue;
        }
        DosFind find; int x=dos_find_first(s->name,0,&find); size_t prefix=leaf(s->name)-s->name;
        while(!x && !e) {
            char source[DOS_PATH_MAX]; e=join(source,s->name,prefix,find.name); if(e) break;
            if(same_file(source,target)) {
                if(!(append && i==0)) say("Content of destination lost before copy\n");
            } else {print("%s\n",source); e=copy_into(out,source,ascii,&any);}
            if(!e) x=dos_find_next(&find);
        }
        if(!e && x!=DE_NOMORE && x!=DE_NOFILE) e=x;
    }
    if(!e && !any) e=DE_NOFILE;
    e=copy_finish(out,dmode=='A',append,e);
    if(e && !append) dos_remove(target,0);
    if(e) return e;
    return copied(1);
}
static int cmd_copy(char *args) {
    char storage[512]; size_t used=0; CopyItem items[16]; unsigned count=0,group=0;
    int plus=0,verify=0; char mode=0;
    for(char *p=args;*p;) {
        if(delimiter(*p)) {p++; continue;}
        if(*p=='+') {plus=1; p++; continue;}
        char c=switch_at(p,"ABV");
        if(c) {
            if(c=='V') verify=1;
            else {mode=c; if(count) items[count-1].mode=c;}
            p+=2; continue;
        }
        if(*p=='/') return report("Invalid switch\n");
        char *start=p;
        while(*p && !delimiter(*p) && *p!='+' && !switch_at(p,"ABV")) p+=step(p);
        size_t length=p-start;
        if(count==ARRAY_SIZE(items) || used+length+1>sizeof(storage)) return report("Too many parameters\n");
        if(count && !plus) group++;
        items[count]=(CopyItem){storage+used,group,mode}; memcpy(storage+used,start,length);
        storage[used+length]=0; used+=length+1; count++; plus=0;
    }
    int saved=0,on=1;
    if(verify) {int e=dos_verify(0,&saved); if(!e) e=dos_verify(1,&on); if(e) return e;}
    int e=copy_files(items,count);
    if(verify) dos_verify(1,&saved);
    return e;
}
static int cmd_set(char *args) {
    args=trim(args);
    if(!*args) {
        char entry[DOS_ENV_CAPACITY];
        for(u32 i=0;;i++) {
            int e=dos_env_list(i,entry,sizeof(entry));
            if(e==DE_NOMORE) return 0;
            if(e) return e;
            print("%s\n",entry);
        }
    }
    char *equal=strchr(args,'='); if(!equal) return report("Syntax error\n");
    *equal++=0; char *name=trim(args);
    if(!env_name(name,strlen(name))) return report("Syntax error\n");
    int e=dos_env_set(name,equal);
    return e==DE_ENV?report("Out of environment space\n"):e;
}
static int cmd_goto(char *args) {
    if(!batch) return 0;
    char *label=word(&args);
    if(label && *label==':') label++;
    if(label) for(size_t p=0;p<batch->size;) {
        size_t start=p; while(p<batch->size && batch->text[p]!='\n') p++;
        size_t end=p; if(p<batch->size) p++;
        while(start<end && delimiter(batch->text[start])) start++;
        if(start>=end || batch->text[start]!=':') continue;
        size_t name=++start; while(start<end && !delimiter(batch->text[start]) && batch->text[start]!='\r') start++;
        if(start-name!=strlen(label)) continue;
        unsigned match=1;
        for(size_t i=0;i<start-name;i++) if(upper(batch->text[name+i])!=upper(label[i])) match=0;
        if(match) {batch->pc=p; return 0;}
    }
    batch->pc=batch->size;
    return report("Label not found\n");
}
static int dispatch(char *line);
static int run_batch(const char *,const char *,const char *,unsigned);
static char *keyword(char *p,const char *name) {
    size_t n=strlen(name);
    for(size_t i=0;i<n;i++) if(upper(p[i])!=name[i]) return NULL;
    return !p[n] || delimiter(p[n])?p+n:NULL;
}
static char *skip(char *p) {while(delimiter(*p)) p++; return p;}
/* IF (TBATCH2.ASM): NOT, ERRORLEVEL n, EXIST file or string1==string2. */
static int cmd_if(char *args) {
    int negate=0,condition; char *p=skip(args),*q;
    while((q=keyword(p,"NOT"))) {negate^=1; p=skip(q);}
    if(!*p) return report("Syntax error\n");
    if((q=keyword(p,"ERRORLEVEL"))) {
        p=skip(q); unsigned n=0,digits=0;
        while(*p>='0' && *p<='9') {if(n<256) n=n*10+(*p-'0'); p++; digits++;}
        if(!digits || n>255 || (*p && !delimiter(*p))) return report("Syntax error\n");
        condition=dos_get_errorlevel()>=n;
    } else if((q=keyword(p,"EXIST"))) {
        p=skip(q); char *name=p; while(*p && !delimiter(*p)) p+=step(p);
        if(p==name) return report("Syntax error\n");
        char path[DOS_PATH_MAX]; size_t length=p-name; if(length>=sizeof(path)) return DE_PATH;
        memcpy(path,name,length); path[length]=0;
        DosFind found; u8 attr;
        condition=!dos_find_first(path,FA_DIR|FA_HIDDEN|FA_SYSTEM,&found) || (!dos_attribute(path,0,&attr) && attr==0x40);
    } else {
        char *first=p; while(*p && !delimiter(*p)) p+=step(p);
        size_t length=p-first;
        while(*p && *p!='=') p++;
        if(*p!='=' || p[1]!='=') return report("Syntax error\n");
        p=skip(p+2); if(!*p) return report("Syntax error\n");
        char *second=p; while(*p && !delimiter(*p)) p+=step(p);
        condition=(size_t)(p-second)==length && !memcmp(first,second,length);
    }
    p=skip(p); if(!*p) return report("Syntax error\n");
    return condition!=negate?dispatch(p):0;
}
/* FOR %v IN (set) DO command (TFOR.ASM): one level, case-sensitive variable,
 * wildcard items expand to normal files with their original path prefix. */
static int for_run(const char *command,char var,const char *value) {
    char line[512]; size_t n=0;
    for(const char *p=command;*p;) {
        unsigned width=step(p);
        if(width==1 && p[0]=='%' && p[1]==var) {
            size_t length=strlen(value); if(n+length>=sizeof(line)) return DE_PATH;
            memcpy(line+n,value,length); n+=length; p+=2; continue;
        }
        if(n+width>=sizeof(line)) return DE_PATH;
        memcpy(line+n,p,width); n+=width; p+=width;
    }
    line[n]=0;
    if(echo_on) {print_prompt(); print("%s\n",line);}
    int e=dispatch(line);
    if(e && e!=DE_BREAK) {error(e); dos_set_errorlevel(1);}
    return interrupted?DE_BREAK:e==DE_BREAK?e:0;
}
static int cmd_for(char *args) {
    if(for_active) return report("FOR cannot be nested\n");
    char *p=skip(args);
    if(p[0]!='%' || !p[1] || lead((u8)p[1]) || (p[2] && !delimiter(p[2]))) return report("Syntax error\n");
    char var=p[1]; p=skip(p+2);
    if(upper(p[0])!='I' || upper(p[1])!='N' || (p[2]!='(' && !delimiter(p[2]))) return report("Syntax error\n");
    p=skip(p+2); if(*p!='(') return report("Syntax error\n");
    char *list=++p; while(*p && *p!=')') p+=step(p);
    if(*p!=')') return report("Syntax error\n");
    *p++=0; p=skip(p);
    char *command=keyword(p,"DO"); if(!command) return report("Syntax error\n");
    command=skip(command); if(!*command) return report("Syntax error\n");
    for_active=1; int e=0;
    for(char *item=list;!e;) {
        item=skip(item); if(!*item) break;
        char *end=item; while(*end && !delimiter(*end)) end+=step(end);
        char saved=*end; *end=0;
        if(wild(item)) {
            DosFind find; int x=dos_find_first(item,0,&find); size_t prefix=leaf(item)-item;
            while(!x && !e) {
                char value[DOS_PATH_MAX]; e=join(value,item,prefix,find.name);
                if(!e) e=for_run(command,var,value);
                if(!e && batch && batch->chain) break;
                if(!e) x=dos_find_next(&find);
            }
        } else e=for_run(command,var,item);
        *end=saved; item=end;
        if(!shell_running || (batch && batch->chain)) break;
    }
    for_active=0; return e;
}
static int cmd_chcp(char *args) {
    char *p=word(&args);
    if(!p) {
        u16 active,boot; int e=dos_code_page(&active,&boot); if(e) return e;
        print("Active code page: %u\n",(unsigned long long)active); return 0;
    }
    if(word(&args)) return report("Too many parameters\n");
    unsigned page=0;
    for(char *q=p;*q;q++) {if(*q<'0' || *q>'9' || page>9999) return report("Invalid parameter\n"); page=page*10+(*q-'0');}
    if(page<100 || page>999) return report("Parameter value not in allowed range\n");
    int e=dos_code_page_set(page);
    if(!e) return 0;
    if(e==DE_NOFILE || e==DE_FUNCTION || e==DE_DATA) return report("Invalid code page\n");
    /* Chiefly error 65: a device (CON, or KEYB through it) refuses the page. */
    print("Code page %u not prepared for system\n",(unsigned long long)page);
    return REPORTED;
}
static int cmd_ctty(char *args) {
    char *name=word(&args); if(!name) return report("Required parameter missing\n");
    if(word(&args)) return report("Too many parameters\n");
    if(!is_device(name)) return report("Invalid device\n");
    unsigned h; int e=dos_open(name,2,0,&h); if(e) return e;
    for(unsigned i=0;i<3 && !e;i++) e=dos_dup2(h,i);
    int c=dos_close(h); return e?e:c;
}
static int on_off(const char *args,int *value) {
    if(!stricmp(args,"ON")) *value=1;
    else if(!stricmp(args,"OFF")) *value=0;
    else return 0;
    return 1;
}
static int external(const char *name,const char *tail) {
    int call=calling; calling=0;
    const char *ext=strchr(leaf(name),'.');
    const char *suffixes[]={".EFI",".EXE",".BAT",".COM"};
    unsigned rounds=ext?1:ARRAY_SIZE(suffixes);
    char dirs[DOS_ENV_CAPACITY]; strcopy(dirs,sizeof(dirs),getenv_dos("PATH")); char *dir=dirs;
    for(unsigned pass=0;;pass++) {
        char *next=NULL; if(pass) {next=strchr(dir,';'); if(next) *next++=0;}
        for(unsigned i=0;i<rounds;i++) {
            char path[DOS_PATH_MAX]=""; int e;
            if(pass && *dir) {e=strcopy(path,sizeof(path),dir); if(e) return e; if(path[strlen(path)-1]!='\\') {e=strappend(path,sizeof(path),"\\"); if(e) return e;}}
            e=strappend(path,sizeof(path),name); if(e) return e;
            if(!ext) {e=strappend(path,sizeof(path),suffixes[i]); if(e) return e;}
            const char *dot=strchr(leaf(path),'.');
            if(dot && !stricmp(dot,".BAT")) {
                u8 attr; e=dos_attribute(path,0,&attr);
                if(!e && (attr&(FA_DIR|FA_VOLUME))) e=DE_NOFILE;
                if(!e && batch && !call) {
                    /* Without CALL, a batch file replaces the current one. */
                    e=strcopy(batch->chain_name,sizeof(batch->chain_name),path);
                    if(!e) e=strcopy(batch->chain_zero,sizeof(batch->chain_zero),name);
                    if(!e) e=strcopy(batch->chain_args,sizeof(batch->chain_args),tail);
                    if(!e) batch->chain=1;
                } else if(!e) e=run_batch(path,name,tail,batch?batch->depth+1:1);
            } else {
                e=dos_exec(path,tail);
                if(!e) {
                    DosExitInfo status;
                    if(!dos_last_exit(&status) && (status.kind==DOS_EXIT_BREAK || status.kind==DOS_EXIT_CRITICAL)) {interrupted=1; e=DE_BREAK;}
                }
            }
            if(e!=DE_NOFILE && e!=DE_PATH) return e;
        }
        if(last_sep(name)) break;
        if(pass) {if(!next) break; dir=next;}
    }
    return report("Bad command or file name\n");
}
static int command_char(char c) {
    return c && !separator(c) && !strchr("/\\.,;=+\"[]<>|:",c) && !lead((u8)c);
}
static int dispatch(char *line) {
    char *p=trim(line); if(*p=='@') p=trim(p+1);
    if(!*p || *p==':') return 0;
    char name[12]; size_t n=0; char *q=p;
    while(command_char(*q) && n<sizeof(name)-1) name[n++]=upper(*q++);
    name[n]=0;
    if(n==1 && *q==':' && (!q[1] || separator(q[1]))) {
        if(q[1] && *trim(q+1)) return report("Invalid drive specification\n");
        return dos_select_drive((unsigned)(name[0]-'A'));
    }
    char *raw=q; if(separator(*raw) || *raw==',' || *raw==';' || *raw=='=') raw++;
    char *args=trim(q);
    if(command_char(*q)) n=0; /* Name longer than any internal command. */
#define IS(text) (n && !strcmp(name,text))
    if(IS("REM")) return 0;
    if(IS("VER")) return say(DOS_PRODUCT "\n");
    if(IS("HELP")) return say(
        "BREAK CALL CD/CHDIR CHCP CLS COPY CTTY DATE DEL/ERASE DIR ECHO EXIT FOR GOTO\n"
        "IF MD/MKDIR PATH PAUSE PROMPT RD/RMDIR REM REN/RENAME SET SHIFT TIME\n"
        "TRUENAME TYPE VER VERIFY VOL SHUTDOWN; native .EFI/.EXE/.COM and .BAT files.\n"
        "Redirection: < > >> and pipes |. COMMAND [path] [device] [/E:n] [/P] [/MSG] [/C cmd].\n");
    if(IS("BREAK") || IS("VERIFY")) {
        int verify=IS("VERIFY"),on;
        if(!*args) {
            int e=verify?dos_verify(0,&on):dos_break_check(0,&on);
            if(!e) print("%s is %s\n",verify?"VERIFY":"BREAK",on?"on":"off");
            return e;
        }
        if(!on_off(args,&on)) return report("Must specify ON or OFF\n");
        return verify?dos_verify(1,&on):dos_break_check(1,&on);
    }
    if(IS("ECHO")) {
        int on;
        if(*q=='.') {int e=say(q+1); return e?e:say("\n");}
        if(!*args) {print("ECHO is %s\n",echo_on?"on":"off"); return 0;}
        if(on_off(args,&on)) {echo_on=on; return 0;}
        int e=say(raw); if(e) return e; return say("\n");
    }
    if(IS("DIR")) return cmd_dir(args);
    if(IS("CD") || IS("CHDIR")) {
        char *d=word(&args); if(!d) {print("%s\n",cwd()); return 0;}
        if(strlen(d)==2 && d[1]==':') {
            char path[DOS_PATH_MAX]; int e=dos_drive_cwd((unsigned)(upper(d[0])-'A'),path);
            if(!e) print("%c:%s\n",upper(d[0]),path);
            return e;
        }
        int e=dos_chdir(d);
        return e==DE_PATH || e==DE_NOFILE?report("Invalid directory\n"):e;
    }
    if(IS("MD") || IS("MKDIR")) {char *d=word(&args); return d?dos_mkdir(d):report("Required parameter missing\n");}
    if(IS("RD") || IS("RMDIR")) {char *d=word(&args); return d?dos_remove(d,1):report("Required parameter missing\n");}
    if(IS("TYPE")) return cmd_type(args);
    if(IS("COPY")) return cmd_copy(args);
    if(IS("DEL") || IS("ERASE")) return cmd_del(args);
    if(IS("REN") || IS("RENAME")) return cmd_ren(args);
    if(IS("SET")) return cmd_set(args);
    if(IS("PATH")) {
        if(!*args) {const char *path=getenv_dos("PATH"); if(*path) print("PATH=%s\n",path); else say("No Path\n"); return 0;}
        return dos_env_set("PATH",!strcmp(args,";")?"":args);
    }
    if(IS("PROMPT")) return dos_env_set("PROMPT",*args?args:NULL);
    if(IS("CLS")) {con_clear(); return 0;}
    if(IS("DATE") || IS("TIME")) return cmd_clock(args,IS("DATE"));
    if(IS("PAUSE")) return pause_key();
    if(IS("VOL")) return cmd_vol(args);
    if(IS("CHCP")) return cmd_chcp(args);
    if(IS("CTTY")) return cmd_ctty(args);
    if(IS("TRUENAME")) {
        char *path=word(&args),full[DOS_PATH_MAX]; int e=dos_canonical(path?path:".",full);
        if(!e) print("%s\n",full);
        return e;
    }
    /* APPEND installed with /E keeps its list in the APPEND= variable, which
     * the shell sets itself, as DOS 4's COMMAND.COM did; switches still go
     * to APPEND.EXE. */
    if(IS("APPEND") && !strchr(args,'/')) {
        DosAppend a={.size=sizeof(a)};
        if(!dos_append(NULL,&a) && (a.flags&DOS_APPEND_INSTALLED) && (a.flags&DOS_APPEND_ENV)) {
            char value[DOS_PATH_MAX];
            if(!*args) {
                if(dos_env_get("APPEND",value,sizeof(value)) || !value[0]) return say("No Append\n");
                print("APPEND=%s\n",value); return 0;
            }
            if(!strcmp(args,";")) return dos_env_set("APPEND","");
            strcopy(value,sizeof(value),args);
            for(char *c=value;*c;c++) *c=upper(*c);
            return dos_env_set("APPEND",value);
        }
    }
    if(IS("GOTO")) return cmd_goto(args);
    if(IS("SHIFT")) {if(batch && batch->shift<batch->argc) batch->shift++; return 0;}
    if(IS("IF")) return cmd_if(args);
    if(IS("FOR")) return cmd_for(args);
    if(IS("CALL")) {calling=1; int e=dispatch(args); calling=0; return e;}
    if(IS("EXIT")) {if(batch && !stricmp(args,"/B")) batch->pc=batch->size; else if(permanent) say("Permanent COMMAND.COM cannot exit.\n"); else shell_running=0; return 0;}
    if(IS("SHUTDOWN")) {say("Flushing disks and shutting down.\n"); dos_shutdown(); return 0;}
#undef IS
    char *rest=p; char *command=word(&rest);
    return external(command,trim(rest));
}
static int open_redirect(const char *name,int output,int append,unsigned *h) {
    int e=dos_open(name,output?1:0,output&&!append?1:0,h);
    if(e==DE_NOFILE && append) e=dos_open(name,1,1,h);
    if(!e && append) {u32 pos; e=dos_seek(*h,0,2,&pos); if(e) dos_close(*h);}
    return e;
}
/* One pipeline stage: strip < > >> (outside quotes), run, then restore. */
static int run_segment(char *cmd) {
    int saved_in=-1,saved_out=-1,quote=0,e=0; unsigned h,save;
    for(char *p=cmd;*p;) {
        if(step(p)==2) {p+=2; continue;}
        if(*p=='"') {quote=!quote; p++; continue;}
        if(quote || (*p!='>' && *p!='<')) {p++; continue;}
        int output=*p=='>',append=output && p[1]=='>';
        char *rest=p+1+append; while(separator(*rest)) rest++;
        char name[DOS_PATH_MAX]; unsigned n=0; int quoted=*rest=='"'; if(quoted) rest++;
        while(*rest && (quoted?*rest!='"':!separator(*rest)&&*rest!='<'&&*rest!='>'&&*rest!='|')) {
            unsigned width=step(rest);
            if(n+width>=sizeof(name)) {e=DE_PATH; goto cleanup;}
            memcpy(name+n,rest,width); n+=width; rest+=width;
        }
        if(quoted && *rest=='"') rest++;
        name[n]=0; if(!n) {e=report("Syntax error\n"); goto cleanup;}
        if((output && saved_out>=0)||(!output && saved_in>=0)) {e=report("Duplicate redirection\n"); goto cleanup;}
        e=open_redirect(name,output,append,&h); if(e) goto cleanup;
        e=dos_dup(output?1:0,&save);
        if(e) {dos_close(h); goto cleanup;}
        if(output) saved_out=save; else saved_in=save;
        e=dos_dup2(h,output?1:0); dos_close(h); if(e) goto cleanup;
        memmove(p,rest,strlen(rest)+1);
    }
    output_error=0;
    e=dispatch(cmd);
    if(interrupted) e=DE_BREAK;
    if(!e) e=output_error;
cleanup:
    if(saved_in>=0) {dos_dup2(saved_in,0); dos_close(saved_in);}
    if(saved_out>=0) {dos_dup2(saved_out,1); dos_close(saved_out);}
    return e;
}
/* Pipes (TPIPE.ASM) use AH=5Ah files in %TEMP% (a DOS 5 convention) or the
 * root of the current drive, as DOS 4 does, deleted after each stage. */
static int pipe_file(char path[DOS_PATH_MAX],unsigned *h) {
    const char *temp=getenv_dos("TEMP"); int e=0;
    if(*temp) {e=strcopy(path,DOS_PATH_MAX-13,temp); if(!e) e=dos_temp_file(path,0,h); if(!e) return 0;}
    path[0]='A'+dos_current_drive(); path[1]=':'; path[2]='\\'; path[3]=0;
    return dos_temp_file(path,0,h);
}
static int run_pipeline(char **stages,unsigned count) {
    char files[2][DOS_PATH_MAX]; int have[2]={0,0},e=0;
    for(unsigned i=0;i<count && !e;i++) {
        int saved_in=-1,saved_out=-1; unsigned h,save,slot=i&1;
        if(i+1<count) {
            e=pipe_file(files[slot],&h);
            if(e) {say("Intermediate file error during pipe\n"); e=REPORTED; break;}
            have[slot]=1;
            e=dos_dup(1,&save); if(!e) {saved_out=save; e=dos_dup2(h,1);}
            dos_close(h);
        }
        if(!e && i) {
            e=dos_open(files[slot^1],0,0,&h);
            if(!e) {e=dos_dup(0,&save); if(!e) {saved_in=save; e=dos_dup2(h,0);} dos_close(h);}
        }
        if(!e) {e=run_segment(stages[i]); if(e && e!=DE_BREAK) {error(e); e=0;}}
        if(saved_in>=0) {dos_dup2(saved_in,0); dos_close(saved_in);}
        if(saved_out>=0) {dos_dup2(saved_out,1); dos_close(saved_out);}
        if(i && have[slot^1]) {dos_remove(files[slot^1],0); have[slot^1]=0;}
        if(interrupted) e=DE_BREAK;
    }
    for(unsigned i=0;i<2;i++) if(have[i]) dos_remove(files[i],0);
    return e;
}
static int run_line(char *line) {
    char *cmd=trim(line); if(*cmd=='@') cmd=trim(cmd+1);
    char *stages[16]; unsigned count=1; int quote=0; stages[0]=cmd;
    for(char *p=cmd;*p;) {
        if(step(p)==2) {p+=2; continue;}
        if(*p=='"') quote=!quote;
        else if(*p=='|' && !quote) {
            if(count==ARRAY_SIZE(stages)) return report("Syntax error\n");
            *p=0; stages[count++]=p+1;
        }
        p++;
    }
    if(count==1) return run_segment(cmd);
    for(unsigned i=0;i<count;i++) if(!*trim(stages[i])) return report("Syntax error\n");
    return run_pipeline(stages,count);
}
static int execute(char *line) {
    if(interrupted) return DE_BREAK;
    if(command_depth>=16) return DE_NOMEM;
    command_depth++; int e=run_line(line); command_depth--;
    if(interrupted) e=DE_BREAK;
    if(e) dos_set_errorlevel(1);
    int flushed=dos_flush(); return interrupted?DE_BREAK:e?e:flushed;
}
int shell_line(const char *input) {
    if(interrupted) return DE_BREAK;
    char line[512]; int e=expand(input,line,sizeof(line),NULL); if(e) return e;
    return execute(line);
}
/* %0 is the batch name as invoked; parameters split on DOS delimiters. */
static int batch_load(Batch *b,const char *path,const char *name,const char *tail) {
    unsigned h; int e=dos_open(path,0,0,&h); if(e) return e;
    u32 size; e=dos_seek(h,0,2,&size); if(e || size>65535) {dos_close(h); return e?e:DE_NOMEM;}
    void *text;
    e=dos_alloc((size+16)/16,&text); if(e) {dos_close(h); return e;}
    u32 pos,n; dos_seek(h,0,0,&pos); e=dos_read(h,text,size,&n); dos_close(h);
    if(!e && n!=size) e=DE_IO;
    if(e) {dos_free(text); return e;}
    b->text=text; b->size=size; b->pc=0; b->shift=0; b->argc=0;
    size_t used=0;
    for(const char *p=name;;) {
        while(b->argc && delimiter(*p)) p++;
        if(!*p || b->argc==BATCH_ARGS) break;
        size_t start=used;
        while(*p && (!b->argc || !delimiter(*p))) {
            unsigned width=step(p);
            if(used+width+1>=sizeof(b->args)) return DE_NOMEM;
            memcpy(b->args+used,p,width); used+=width; p+=width;
        }
        b->args[used++]=0; b->argv[b->argc++]=start;
        if(b->argc==1) p=tail;
    }
    return 0;
}
static int run_batch(const char *path,const char *name,const char *tail,unsigned level) {
    if(level>8) return DE_NOMEM;
    Batch frame; memset(&frame,0,sizeof(frame)); frame.depth=level; frame.parent=batch;
    int e=batch_load(&frame,path,name,tail?tail:""); if(e) {if(frame.text) dos_free(frame.text); return e;}
    int echo=echo_on; batch=&frame; unsigned steps=0;
    while(!e && shell_running && !interrupted) {
        if(frame.chain) {
            char next[DOS_PATH_MAX],zero[DOS_PATH_MAX],args[256];
            strcopy(next,sizeof(next),frame.chain_name); strcopy(zero,sizeof(zero),frame.chain_zero);
            strcopy(args,sizeof(args),frame.chain_args);
            frame.chain=0; dos_free(frame.text); frame.text=NULL;
            e=batch_load(&frame,next,zero,args); if(e) {if(frame.text) {dos_free(frame.text); frame.text=NULL;} break;}
            continue;
        }
        if(frame.pc>=frame.size) break;
        if(++steps>10000) {e=DE_FUNCTION; break;}
        char raw[256],line[512]; size_t len=0;
        while(frame.pc<frame.size && frame.text[frame.pc]!='\n') {
            char c=frame.text[frame.pc++]; if(c=='\r') continue;
            if(c==26) {frame.pc=frame.size; break;}
            if(len+1>=sizeof(raw)) {e=DE_PATH; break;} raw[len++]=c;
        }
        if(e) break;
        if(frame.pc<frame.size) frame.pc++;
        raw[len]=0; char *cmd=raw; while(delimiter(*cmd)) cmd++;
        if(!*trim(cmd) || *cmd==':') continue;
        int quiet=*cmd=='@';
        int result=expand(cmd,line,sizeof(line),&frame);
        if(!result) {
            if(echo_on && !quiet) {print_prompt(); print("%s\n",trim(line));}
            result=execute(line);
        }
        error(result);
        if(result==DE_BREAK) e=result;
        /* DOS batch execution continues after a failed command. */
    }
    batch=frame.parent; if(frame.text) dos_free(frame.text);
    if(level==1) echo_on=echo;
    return e;
}
int shell_batch(const char *name,const char *tail,unsigned level) {return run_batch(name,name,tail,level);}
int shell(const char *tail) {
    shell_running=1;
    print_sink=shell_print;
    {DosRegs r={.ax=0x6300}; dbcs=dos_call(&r)?NULL:(const u8 *)(uintptr_t)r.si;}
    DosBreakHandler handler={break_handler,NULL};
    int setup=dos_break_handler(&handler,&previous_handler);
    if(setup) {print_sink=NULL; return 1;}
    DosInfo info; dos_query(&info); shell_pid=info.pid;
    DosCriticalHandler disk_handler={critical_handler,NULL};
    setup=dos_critical_handler(&disk_handler,&previous_critical);
    if(setup) {dos_break_handler(&previous_handler,NULL); print_sink=NULL; return 1;}
    char options[256];
    if(strcopy(options,sizeof(options),tail)) return finish_shell(1);
    /* COMMAND [[d:]path] [device] [/E:n] [/P] [/MSG] [/C string] (INIT.ASM). */
    char *p=options,*command=NULL,*path=NULL,*device=NULL; permanent=0;
    while(*p) {
        while(separator(*p)) p++;
        if(!*p) break;
        if(*p=='/') {
            char c=upper(p[1]);
            if(c=='C') {command=p+2; break;}
            char *end=p+1; while(*end && !separator(*end) && *end!='/') end++;
            size_t length=end-p;
            if(c=='P' && length==2) permanent=1;
            else if(c=='D' && length==2) {}
            else if(length==4 && upper(p[2])=='S' && upper(p[3])=='G' && c=='M') {}
            else if(c=='E' && p[2]==':') {
                unsigned size=0; char *q=p+3;
                while(q<end && *q>='0' && *q<='9' && size<100000) size=size*10+(*q++-'0');
                /* The native environment is a fixed 4 KiB per task. */
                if(q!=end || q==p+3 || size<160 || size>32768) say("Parameter value not in allowed range\n");
            } else say("Invalid switch\n");
            p=end; continue;
        }
        char *token=word(&p);
        if(!path) path=token; else if(!device) device=token; else say("Too many parameters\n");
    }
    if(path) {
        char full[DOS_PATH_MAX]; u8 attr;
        if(dos_attribute(path,0,&attr) || !(attr&FA_DIR) || dos_canonical(path,full) ||
           (strlen(full)>3 && strappend(full,sizeof(full),"\\")) || strappend(full,sizeof(full),"COMMAND.COM"))
            say("Specified COMMAND search directory bad\n");
        else dos_env_set("COMSPEC",full);
    }
    if(device) {char ctty[DOS_PATH_MAX+8]="CTTY "; strappend(ctty,sizeof(ctty),device); error(shell_line(ctty));}
    if(command) {
        int result=shell_line(command); error(result);
        return finish_shell(result?1:(int)dos_get_errorlevel());
    }
    if(!permanent) con_puts(DOS_PRODUCT "\n"); /* a second shell, as DOS's says which it is */
    if(permanent) {int e=shell_batch("C:\\AUTOEXEC.BAT","",1); if(e!=DE_NOFILE) error(e);}
    u8 input[257]={255,0};
    while(shell_running) {
        interrupted=0;
        if(echo_on) print_prompt(); /* TCODE.ASM: no prompt while ECHO is off. */
        int e=dos_line_input(input);
        if(e==DE_BREAK) {dos_set_errorlevel(1); continue;}
        if(e==DE_EOF) return finish_shell(permanent?1:(int)dos_get_errorlevel());
        if(e) {error(e); return finish_shell(1);}
        con_puts("\n");
        char line[256]; memcpy(line,input+2,input[1]); line[input[1]]=0;
        error(shell_line(line));
    }
    return finish_shell(dos_get_errorlevel());
}
