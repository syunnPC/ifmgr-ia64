/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * EXE2BIN: MS-DOS 4 CMD/EXE2BIN counterpart (E2BINIT.ASM).
 *   EXE2BIN [d:][path]filename[.ext] [d:][path][filename[.ext]]
 *
 * Defaults: input .EXE, output .BIN, input drive/name unless specified,
 * current directory when no output is given. Reject images >=64 KiB,
 * nonzero SS/SP/CS, or IP other than 0/100h. IP=100h creates .COM data by
 * dropping the first 100h bytes; relocations are forbidden. IP=0 applies
 * relocations at a prompted hexadecimal base segment. Read headers as
 * whole 512-byte pages. Messages: v4.0 EXE2BIN.SKL.
 */
#include "util.h"
#define MAX_IMAGE 0x10000U

static void say(const char *text) {to_stderr(1); print("%s\n",text); to_stderr(0);}
static const char *last_part(const char *path) {
    const char *l=path;
    for(const char *p=path;*p;p++) if(*p=='\\' || *p=='/' || *p==':') l=p+1;
    return l;
}
static int has_dot(const char *name) {for(;*name;name++) if(*name=='.') return 1; return 0;}
/* The base segment, asked for until it is hex digits (none: asked again). */
static int base_segment(u16 *out) {
    for(;;) {
        print("Fix-ups needed - base segment (hex):");
        u8 line[16]={12,0}; int e=dos_line_input(line); print("\n");
        if(e) return e;
        u32 v=0; unsigned n=line[1],i; if(!n) continue;
        for(i=0;i<n;i++) {
            char c=(char)upper((char)line[2+i]);
            if(c>='0' && c<='9') v=v<<4|(u32)(c-'0');
            else if(c>='A' && c<='F') v=v<<4|(u32)(c-'A'+10);
            else break;
        }
        if(i==n) {*out=(u16)v; return 0;}
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char in[DOS_PATH_MAX],out[DOS_PATH_MAX],extra[DOS_PATH_MAX];
    const char *p=app_dos->command_tail();
    if(!(p=next_arg(p,in,sizeof(in)))) {say("File name must be specified"); return 1;}
    int second=(p=next_arg(p,out,sizeof(out)))!=NULL;
    if(second && next_arg(p,extra,sizeof(extra))) {parse_error(PARSE_TOO_MANY,extra); return 1;}
    if(in[0]=='/' || (second && out[0]=='/')) {parse_error(PARSE_SWITCH,in[0]=='/'?in:out); return 1;}
    if(!has_dot(last_part(in)) && strappend(in,sizeof(in),".EXE")) {say("Invalid parameter"); return 1;}
    /* The output's name. */
    char base[13]; const char *leaf=last_part(in); unsigned n=0;
    while(leaf[n] && leaf[n]!='.' && n<8) {base[n]=leaf[n]; n++;}
    base[n]=0;
    int drive_in=in[0] && in[1]==':';
    if(!second) {
        out[0]=0;
        if(drive_in) {out[0]=in[0]; out[1]=':'; out[2]=0;}
        strappend(out,sizeof(out),base);
    } else {
        if(drive_in && !(out[0] && out[1]==':')) {
            char t[DOS_PATH_MAX]; t[0]=in[0]; t[1]=':'; t[2]=0;
            if(strappend(t,sizeof(t),out) || strcopy(out,sizeof(out),t)) {say("Invalid parameter"); return 1;}
        }
        size_t len=strlen(out); u8 attr=0;
        if(len && (out[len-1]==':' || out[len-1]=='\\')) strappend(out,sizeof(out),base);
        else if(!dos_attribute(out,0,&attr) && (attr&FA_DIR)) {strappend(out,sizeof(out),"\\"); strappend(out,sizeof(out),base);}
    }
    if(!has_dot(last_part(out)) && strappend(out,sizeof(out),".BIN")) {say("Invalid parameter"); return 1;}

    unsigned h; int e=dos_open(in,DOS_OPEN_READ,0,&h);
    if(e) {extended_error(e,NULL); return 1;}
    static u8 data[MAX_IMAGE+0x200]; u8 header[28]; u32 got=0,pos;
    e=dos_read(h,header,sizeof(header),&got);
    if(e) {dos_close(h); extended_error(e,NULL); return 1;}
    if(got<sizeof(header) || rd16(header)!=0x5a4d) {dos_close(h); say("File cannot be converted"); return 1;}
    u32 pages=rd16(header+4),last=rd16(header+2),relocations=rd16(header+6),head=rd16(header+8);
    u32 ss=rd16(header+14),sp=rd16(header+16),ip=rd16(header+20),cs=rd16(header+22),table=rd16(header+24);
    /* The header in whole 512-byte pages, and the program within 64 KiB. */
    if(head+31>=0x1000) {dos_close(h); say("Insufficient memory"); return 1;}
    u32 head_bytes=((head+31)&~31u)*16,head_pages=head_bytes/512;
    if(pages<head_pages || pages-head_pages>=0x80) {dos_close(h); say("Insufficient memory"); return 1;}
    /* The program's size as DOS 4 counts it, in 16 bits. */
    u16 size16=(u16)((pages-head_pages)*512);
    if(last) size16=(u16)(size16-0x200+last);
    u32 size=size16;
    if(ss || sp || cs || (ip && ip!=0x100) || (ip==0x100 && relocations)) {dos_close(h); say("File cannot be converted"); return 1;}
    u32 skip=ip==0x100?0x100:0;
    if(size<skip) {dos_close(h); say("File cannot be converted"); return 1;}
    e=dos_seek(h,head_bytes+skip,0,&pos);
    size-=skip; /* within data: below 64 KiB */
    if(!e) e=dos_read(h,data,size,&got);
    if(e) {dos_close(h); extended_error(e,NULL); return 1;}
    /* A file shorter than its header says: the rest as zeros (DOS 4 writes
     * whatever its memory held). */
    if(relocations) {
        u16 segment;
        e=base_segment(&segment); if(e) {dos_close(h); return 1;}
        e=dos_seek(h,table,0,&pos);
        for(u32 i=0;!e && i<relocations;i++) {
            u8 r[4]; e=dos_read(h,r,4,&got);
            if(!e && got!=4) e=DE_FORMAT;
            if(e) break;
            u32 at=(u32)rd16(r+2)*16+rd16(r);
            if(at+1>=size) continue; /* outside the image: nothing to fix */
            u16 v=(u16)(rd16(data+at)+segment); data[at]=(u8)v; data[at+1]=(u8)(v>>8);
        }
        if(e) {dos_close(h); say("File cannot be converted"); return 1;}
    }
    dos_close(h);
    unsigned o; e=dos_open(out,DOS_OPEN_WRITE,1,&o);
    if(e) {say("File creation error"); return 1;}
    u32 done=0; e=dos_write(o,data,size,&done);
    int ce=dos_close(o);
    if(!e && done<size) {dos_remove(out,0); say("Insufficient disk space"); return 1;}
    if(e || ce) {extended_error(e?e:ce,NULL); return 1;}
    return 0;
}
