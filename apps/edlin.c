/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * EDLIN: MS-DOS 4 CMD/EDLIN counterpart.
 *   EDLIN [d:][path]filename [/B]
 *
 * Read to Ctrl+Z, or EOF with /B. Commands: line=edit; I=insert until Ctrl+C;
 * D=delete; L=list 23 lines; P=page; S=find; R old^Znew=replace; C=copy;
 * M=move; T=merge file; W=write; A=read more; E=save with .BAK; Q=quit.
 * Line references accept numbers, . (current), # (after last), or +n/-n.
 * Display uses an eight-column line number, colon and * for the current
 * line (v4.0 EDLMES message 32).
 */
#include "util.h"
#define MAX_LINES 32760
#define LINE_MAX 253
static char **lines; static u32 count,current=1; /* MAX_LINES of them */
static char name[DOS_PATH_MAX],backup[DOS_PATH_MAX];
static u8 *heap; static u32 heap_used,heap_size;
static int binary,eof_seen,interrupted;
static unsigned out_handle=~0u; static int out_open;

/* Ctrl+C ends the line being typed (and insert mode), not EDLIN. */
static int ctrl_c(void *ctx) {(void)ctx; interrupted=1; return DOS_BREAK_CANCEL;}
/* The last '.' of a path's final component, or NULL. */
static char *extension(char *path) {
    char *dot=NULL;
    for(char *q=path;*q;q++) {if(*q=='.') dot=q; else if(*q=='\\' || *q==':') dot=NULL;}
    return dot;
}
/* The file's name with another extension. */
static void with_extension(char out[DOS_PATH_MAX],const char *ext) {
    strcopy(out,DOS_PATH_MAX,name);
    char *dot=extension(out);
    if(dot) *dot=0;
    strappend(out,DOS_PATH_MAX,ext);
}
static char *keep(const char *text,unsigned n) {
    if(heap_used+n+1>heap_size) return NULL;
    char *p=(char *)heap+heap_used; memcpy(p,text,n); p[n]=0; heap_used+=n+1;
    return p;
}
static void show(u32 at) {print("%8u:%c%s\n",(unsigned long long)at,at==current?'*':' ',lines[at-1]);}
static int read_line(const char *prompt_fmt,u32 number,char *out) {
    u8 buffer[LINE_MAX+3]={LINE_MAX+1};
    interrupted=0;
    print(prompt_fmt,(unsigned long long)number);
    int e=dos_line_input(buffer);
    if(e || interrupted) return 0;
    print("\n");
    unsigned n=buffer[1]; memcpy(out,buffer+2,n); out[n]=0;
    return 1;
}
static int ask(const char *question) {
    for(;;) {
        print("%s",question);
        DosRegs r={.ax=0x0c01}; if(dos_call(&r)) return 0;
        char c=(char)upper((int)(r.ax&0xff)); print("\n");
        if(c=='Y' || c=='N') return c=='Y';
    }
}
static int insert_at(u32 at,const char *text,unsigned n) {
    char *copy=keep(text,n);
    if(!copy || count>=MAX_LINES) {print("Insufficient memory\n"); return 0;}
    memmove(&lines[at],&lines[at-1],(count-at+1)*sizeof(*lines));
    lines[at-1]=copy; count++;
    return 1;
}
/* Lines from a handle, Ctrl+Z ending the text unless /B; at most limit. */
static int load(unsigned handle,u32 at,u32 limit,int stop_at_eof) {
    static LineReader reader; static char text[1024]; unsigned n; u32 added=0;
    lines_open(&reader,handle);
    while(added<limit) {
        if(stop_at_eof) {
            /* Ctrl+Z is seen by lines_next; keep it as text with /B. */
            if(!lines_next(&reader,text,sizeof(text),&n)) {eof_seen=1; break;}
        } else {
            u32 got; unsigned k=0; u8 c=0;
            while(k<sizeof(text)-1 && !dos_read(handle,&c,1,&got) && got) {if(c=='\n') break; text[k++]=(char)c;}
            if(!k && (c!='\n')) {eof_seen=1; break;}
            if(k && text[k-1]=='\r') k--;
            n=k; text[n]=0;
        }
        if(n>LINE_MAX) n=LINE_MAX;
        if(!insert_at(at+added,text,n)) return 0;
        added++;
    }
    return 1;
}
static unsigned input_handle=~0u;
/* Parses a line reference; 0 when none was given. */
static const char *line_ref(const char *p,u32 *out,int *given) {
    while(*p==' ') p++;
    *given=1;
    if(*p=='.') {*out=current; return p+1;}
    if(*p=='#') {*out=count+1; return p+1;}
    if(*p=='+' || *p=='-') {
        int minus=*p=='-'; u32 n=0; p++;
        while(*p>='0' && *p<='9') n=n*10+(u32)(*p++-'0');
        *out=minus?(n>=current?1:current-n):current+n; return p;
    }
    if(*p>='0' && *p<='9') {u32 n=0; while(*p>='0' && *p<='9') n=n*10+(u32)(*p++-'0'); *out=n?n:1; return p;}
    *given=0; return p;
}
static u32 clamp(u32 n) {return n<1?1:n>count+1?count+1:n;}
static void list(u32 from,u32 to) {
    for(u32 i=from;i<=to && i<=count;i++) show(i);
}
static int matches(const char *line,const char *text,unsigned n) {
    for(unsigned i=0;line[i];i++) if(!memcmp(line+i,text,n) && strlen(line+i)>=n) return (int)i+1;
    return 0;
}
static int write_lines(unsigned handle,u32 from,u32 to) {
    u32 done;
    for(u32 i=from;i<=to;i++) {
        unsigned n=(unsigned)strlen(lines[i-1]);
        if(dos_write(handle,lines[i-1],n,&done) || done!=n || dos_write(handle,"\r\n",2,&done) || done!=2) return DE_FULL;
    }
    return 0;
}
static int open_output(void) {
    if(out_open) return 0;
    char temp[DOS_PATH_MAX]; unsigned result;
    with_extension(temp,".$$$");
    int e=dos_open_ex(temp,1,0,0x12,&out_handle,&result);
    if(e) {print("File Creation Error\n"); return e;}
    out_open=1; return 0;
}
static int finish(void) {
    char temp[DOS_PATH_MAX]; u32 done;
    if(open_output()) return 1;
    if(write_lines(out_handle,1,count) || dos_write(out_handle,"\x1a",1,&done) || done!=1) {
        print("Disk full. Edits lost.\n"); dos_close(out_handle); return 1;
    }
    /* The rest of a file not read in yet follows unchanged. */
    if(input_handle!=~0u) {
        static u8 rest[4096]; u32 got;
        while(!dos_read(input_handle,rest,sizeof(rest),&got) && got) dos_write(out_handle,rest,got,&done);
        dos_close(input_handle); input_handle=~0u;
    }
    dos_close(out_handle);
    with_extension(temp,".$$$");
    u8 attr;
    if(!dos_attribute(name,0,&attr)) {dos_remove(backup,0); dos_rename(name,backup);}
    return dos_rename(temp,name)?1:0;
}
static void edit_line(u32 at) {
    char text[LINE_MAX+1];
    if(at<1 || at>count) {if(at>count && count) at=count; else return;}
    current=at; show(at);
    if(!read_line("%8u:*",at,text)) return;
    if(!text[0]) return;
    char *copy=keep(text,(unsigned)strlen(text));
    if(!copy) {print("Insufficient memory\n"); return;}
    lines[at-1]=copy;
}
static void insert(u32 at) {
    char text[LINE_MAX+1];
    at=clamp(at);
    for(;;) {
        if(!read_line("%8u:*",at,text)) break;
        if(!insert_at(at,text,(unsigned)strlen(text))) break;
        at++;
    }
    current=at;
}
static void search_replace(u32 from,u32 to,int query,int replacing,const char *args) {
    char old[LINE_MAX+1],fresh[LINE_MAX+1]; unsigned on=0,fn=0; const char *p=args;
    static char last_old[LINE_MAX+1],last_new[LINE_MAX+1];
    while(*p && *p!=0x1a && on<LINE_MAX) old[on++]=*p++;
    old[on]=0;
    if(*p==0x1a) {p++; while(*p && fn<LINE_MAX) fresh[fn++]=*p++;}
    fresh[fn]=0;
    if(on) {strcopy(last_old,sizeof(last_old),old); if(replacing) strcopy(last_new,sizeof(last_new),fresh);}
    else {strcopy(old,sizeof(old),last_old); strcopy(fresh,sizeof(fresh),last_new); on=(unsigned)strlen(old); fn=(unsigned)strlen(fresh);}
    if(!on) {print("Entry error\n"); return;}
    int any=0;
    for(u32 i=from;i<=to && i<=count;i++) {
        int at=matches(lines[i-1],old,on);
        if(!at) continue;
        if(!replacing) {
            u32 saved=current; current=i; show(i);
            if(!query || ask("O.K.? ")) return;
            current=saved; any=1; continue;
        }
        char line[512]; unsigned n=0; const char *s=lines[i-1];
        while(*s && n<LINE_MAX) {
            if(strlen(s)>=on && !memcmp(s,old,on)) {for(unsigned k=0;k<fn && n<LINE_MAX;k++) line[n++]=fresh[k]; s+=on;}
            else line[n++]=*s++;
        }
        line[n]=0;
        char *was=lines[i-1],*copy=keep(line,n);
        if(!copy) {print("Insufficient memory\n"); return;}
        lines[i-1]=copy; u32 saved=current; current=i; show(i);
        if(query && !ask("O.K.? ")) {lines[i-1]=was; current=saved; continue;}
        any=1;
    }
    if(!any) print("Not found\n");
}
static void copy_move(u32 from,u32 to,u32 dest,u32 times,int move) {
    if(from<1 || to<from || to>count || dest<1 || dest>count+1 || (dest>from && dest<=to)) {print("Entry error\n"); return;}
    u32 n=to-from+1;
    if(move) {
        char *temp[MAX_LINES/8]; if(n>ARRAY_SIZE(temp)) {print("Insufficient memory\n"); return;}
        memcpy(temp,&lines[from-1],n*sizeof(*lines));
        memmove(&lines[from-1],&lines[to],(count-to)*sizeof(*lines)); count-=n;
        if(dest>to) dest-=n;
        memmove(&lines[dest-1+n],&lines[dest-1],(count-dest+1)*sizeof(*lines));
        memcpy(&lines[dest-1],temp,n*sizeof(*lines)); count+=n;
        current=dest; return;
    }
    if(count+n*times>MAX_LINES) {print("Insufficient memory\n"); return;}
    for(u32 t=0;t<times;t++) {
        memmove(&lines[dest-1+n],&lines[dest-1],(count-dest+1)*sizeof(*lines));
        u32 src=from>=dest?from+n:from;
        for(u32 k=0;k<n;k++) lines[dest-1+k]=lines[src-1+k];
        count+=n; if(from>=dest) {from+=n; to+=n;}
    }
    current=dest;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[DOS_PATH_MAX],command[LINE_MAX+3]; const char *p=app_dos->command_tail(); int e;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/' && upper(arg[1])=='B' && !arg[2]) binary=1;
        else if(arg[0]=='/') {parse_error(PARSE_SWITCH,arg); return 1;}
        else if(name[0]) {parse_error(PARSE_TOO_MANY,arg); return 1;}
        else strcopy(name,sizeof(name),arg);
    }
    if(!name[0]) {print("File name must be specified\n"); return 1;}
    for(char *q=name;*q;q++) *q=(char)upper(*q);
    {
        char *dot=extension(name);
        if(dot && !strcmp(dot,".BAK")) {print("Cannot edit .BAK file--rename file\n"); return 1;}
        with_extension(backup,".BAK");
    }
    heap_size=1u<<20;
    if(dos_alloc(heap_size/16,(void **)&heap) || dos_alloc(MAX_LINES*sizeof(*lines)/16+1,(void **)&lines)) {
        print("Insufficient memory\n"); return 1;
    }
    DosBreakHandler handler={ctrl_c,NULL},previous; int handled=!dos_break_handler(&handler,&previous);
    u8 attr; unsigned h;
    e=dos_attribute(name,0,&attr);
    if(!e && (attr&FA_RDONLY)) {print("File is READ-ONLY\n"); return 1;}
    if(!e && (attr&FA_DIR)) {print("Invalid drive or file name\n"); return 1;}
    if(e) {
        if(e!=DE_NOFILE) {print("Invalid drive or file name\n"); return 1;}
        print("New file\n"); eof_seen=1;
    } else if(dos_open(name,0,0,&h)) {print("Invalid drive or file name\n"); return 1;}
    else {
        /* Three quarters of the room; A reads more, W makes room. */
        if(!load(h,1,MAX_LINES*3/4,!binary)) {dos_close(h); return 1;}
        if(eof_seen) {dos_close(h); print("End of input file\n");}
        else input_handle=h;
    }
    for(;;) {
        u8 buffer[LINE_MAX+3]={LINE_MAX+1};
        interrupted=0;
        print("*");
        if(dos_line_input(buffer) || interrupted) continue;
        print("\n");
        unsigned n=buffer[1]; memcpy(command,buffer+2,n); command[n]=0;
        u32 a[4]={0,0,0,0}; int given[4]={0,0,0,0}; unsigned k=0; const char *q=command;
        for(;;) {
            q=line_ref(q,&a[k],&given[k]);
            while(*q==' ') q++;
            if(*q!=',' || k==3) break;
            q++; k++;
        }
        int query=0;
        if(*q=='?') {query=1; q++;}
        char c=(char)upper(*q);
        if(c) q++;
        if(!c) {edit_line(given[0]?a[0]:current+1); continue;}
        if(c==';') continue;
        switch(c) {
        case 'A': {
            u32 more=given[0]?a[0]:MAX_LINES/4;
            if(input_handle==~0u || eof_seen) {print("End of input file\n"); break;}
            if(!load(input_handle,count+1,more,!binary)) break;
            if(eof_seen) {dos_close(input_handle); input_handle=~0u; print("End of input file\n");}
            break;
        }
        case 'C': case 'M':
            if(!given[2]) {print("Must specify destination line number\n"); break;}
            copy_move(given[0]?a[0]:current,given[1]?a[1]:(given[0]?a[0]:current),a[2],given[3]?a[3]:1,c=='M');
            break;
        case 'D': {
            u32 from=given[0]?a[0]:current,to=given[1]?a[1]:from;
            if(from<1 || to<from || from>count) {if(from>count) break; print("Entry error\n"); break;}
            if(to>count) to=count;
            memmove(&lines[from-1],&lines[to],(count-to)*sizeof(*lines)); count-=to-from+1;
            current=from>count?count+1:from;
            break;
        }
        case 'E':
            if(handled) dos_break_handler(&previous,NULL);
            return finish()?1:0;
        case 'I': insert(given[0]?a[0]:current); break;
        case 'L': {
            u32 from,to;
            if(given[0]) {from=a[0]; to=given[1]?a[1]:from+22;}
            else {from=current>11?current-11:1; to=given[1]?a[1]:from+22;}
            list(from,to);
            break;
        }
        case 'P': {
            u32 from=given[0]?a[0]:(current>1?current+1:1),to=given[1]?a[1]:from+22;
            if(from>count) break;
            if(to>count) to=count;
            current=to; list(from,to);
            break;
        }
        case 'Q':
            if(ask("Abort edit (Y/N)? ")) {
                if(out_open) {dos_close(out_handle);}
                if(input_handle!=~0u) dos_close(input_handle);
                if(handled) dos_break_handler(&previous,NULL);
                return 0;
            }
            break;
        case 'R': case 'S': {
            u32 from=given[0]?a[0]:(c=='S'?current:current+1),to=given[1]?a[1]:count;
            search_replace(from,to,query,c=='R',q);
            break;
        }
        case 'T': {
            u32 at=given[0]?a[0]:current; unsigned th;
            while(*q==' ') q++;
            if(!*q || dos_open(q,0,0,&th)) {print("Not found\n"); break;}
            u32 before=count; int saved=eof_seen; eof_seen=0;
            load(th,clamp(at),MAX_LINES,1); dos_close(th); eof_seen=saved;
            current=clamp(at)+(count-before);
            break;
        }
        case 'W': {
            u32 n=given[0]?a[0]:count;
            if(n>count) n=count;
            if(!n || open_output()) break;
            if(write_lines(out_handle,1,n)) {print("Disk full. Edits lost.\n"); break;}
            memmove(&lines[0],&lines[n],(count-n)*sizeof(*lines)); count-=n;
            current=1;
            break;
        }
        default: print("Entry error\n");
        }
    }
}
