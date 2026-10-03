/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * SORT: native counterpart of MS-DOS 4 CMD/SORT.
 *   SORT [/R] [/+n] < input > output
 * The lines of standard input (Ctrl+Z ends it) in order, by the active
 * country's collating sequence (AH=65h AL=06h) from column n on (1 to
 * 65535; a line shorter than that sorts as an empty one), equal ones as
 * they came; /R the other way. As DOS 4's, it holds up to 64 KiB of text,
 * and its errors begin with "SORT: ".
 */
#include "util.h"
#define LIMIT 0x10000U
static u8 order[256]; static char *text; static u32 column;
typedef struct {u32 at,length;} Line;
static void fail(void (*say)(unsigned,const char *),unsigned which,const char *arg) {
    to_stderr(1); print("SORT: "); to_stderr(0); say(which,arg);
}
static void parse_fail(unsigned which,const char *arg) {parse_error(which,arg);}
static void extended_fail(unsigned which,const char *arg) {extended_error((int)which,arg);}
static int compare(const Line *a,const Line *b) {
    u32 na=a->length>column?a->length-column:0,nb=b->length>column?b->length-column:0;
    const u8 *pa=(const u8 *)text+a->at+column,*pb=(const u8 *)text+b->at+column;
    for(u32 i=0;i<na && i<nb;i++) if(order[pa[i]]!=order[pb[i]]) return order[pa[i]]<order[pb[i]]?-1:1;
    return na<nb?-1:na>nb;
}
/* Stable merge sort of the line table. */
static void merge_sort(Line *lines,Line *scratch,u32 n,int reverse) {
    if(n<2) return;
    u32 half=n/2;
    merge_sort(lines,scratch,half,reverse); merge_sort(lines+half,scratch,n-half,reverse);
    u32 i=0,j=half,k=0;
    while(i<half && j<n) {
        int c=compare(&lines[j],&lines[i]);
        if(reverse?c>0:c<0) scratch[k++]=lines[j++]; else scratch[k++]=lines[i++];
    }
    while(i<half) scratch[k++]=lines[i++];
    while(j<n) scratch[k++]=lines[j++];
    memcpy(lines,scratch,n*sizeof(*lines));
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[64]; const char *p=app_dos->command_tail(); int reverse=0; u32 size=0,got,count=0,done;
    column=1;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        if(arg[0]=='/' && upper(arg[1])=='R' && !arg[2]) reverse=1;
        else if(arg[0]=='/' && arg[1]=='+') {
            u32 n=0; const char *d=arg+2;
            if(!*d) {fail(parse_fail,PARSE_FORMAT,arg); return 1;}
            for(;*d;d++) {if(*d<'0' || *d>'9' || n>65535) break; n=n*10+(u32)(*d-'0');}
            if(*d) {fail(parse_fail,PARSE_FORMAT,arg); return 1;}
            if(!n || n>65535) {fail(parse_fail,PARSE_RANGE,arg); return 1;}
            column=n;
        } else if(arg[0]=='/') {fail(parse_fail,PARSE_SWITCH,arg); return 1;}
        else {fail(parse_fail,PARSE_TOO_MANY,arg); return 1;}
    }
    column--;
    for(unsigned i=0;i<256;i++) order[i]=(u8)i;
    {u32 n=0; dos_nls_table(DOS_NLS_CURRENT,DOS_NLS_CURRENT,6,order,sizeof(order),&n);}
    if(dos_alloc(LIMIT/16,(void **)&text)) {fail(extended_fail,DE_NOMEM,NULL); return 1;}
    while(!dos_read(0,text+size,LIMIT-size,&got) && got) {
        size+=got;
        if(size==LIMIT) {fail(extended_fail,DE_NOMEM,NULL); return 1;}
    }
    for(u32 i=0;i<size;i++) if(text[i]==0x1a) {size=i; break;}
    for(u32 i=0;i<size;i++) if(text[i]=='\n' || i+1==size) count++;
    Line *lines,*scratch;
    if(dos_alloc((count*sizeof(Line)+15)/16+1,(void **)&lines) || dos_alloc((count*sizeof(Line)+15)/16+1,(void **)&scratch)) {
        fail(extended_fail,DE_NOMEM,NULL); return 1;
    }
    count=0;
    for(u32 start=0,i=0;i<size;i++) if(text[i]=='\n' || i+1==size) {
        u32 end=text[i]=='\n'?i:i+1;
        if(end>start && text[end-1]=='\r') end--;
        lines[count++]=(Line){start,end-start}; start=i+1;
    }
    merge_sort(lines,scratch,count,reverse);
    for(u32 i=0;i<count;i++) {
        if(dos_write(1,text+lines[i].at,lines[i].length,&done) || done!=lines[i].length ||
           dos_write(1,"\r\n",2,&done) || done!=2) {
            to_stderr(1); print("SORT: Insufficient disk space\n"); to_stderr(0); return 1;
        }
    }
    return 0;
}
