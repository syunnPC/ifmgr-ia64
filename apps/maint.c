/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Shared raw-volume helpers for the native disk maintenance utilities.
 */
#include "maint.h"
int volume_read(const Volume *v,u32 sector,u32 count,void *buffer) {
    u32 done; return dos_disk_read(v->drive,sector,count,buffer,&done);
}
int volume_write(const Volume *v,u32 sector,u32 count,const void *buffer) {
    u32 done; return dos_disk_write(v->drive,sector,count,buffer,&done);
}
int volume_boot(unsigned drive,Volume *v) {
    memset(v,0,sizeof(*v)); v->drive=drive;
    DosDeviceParams device={.size=sizeof(device)}; int e=dos_device_params(drive,&device); if(e) return e;
    e=volume_read(v,0,1,v->boot); if(e) return e;
    const u8 *b=v->boot;
    v->spc=b[13]; v->reserved=rd16(b+14); v->fats=b[16]; v->root_entries=rd16(b+17);
    v->spf=rd16(b+22); v->media=b[21]; v->total=rd16(b+19); if(!v->total) v->total=rd32(b+32);
    if(rd16(b+510)!=0xaa55 || rd16(b+11)!=512 || !v->spc || (v->spc&(v->spc-1)) || v->spc>128 ||
       !v->reserved || !v->fats || v->fats>2 || !v->root_entries || !v->spf || v->total>device.sectors) return DE_FORMAT;
    v->root_start=v->reserved+v->fats*v->spf; v->root_sectors=(v->root_entries*32+511)/512;
    v->data_start=v->root_start+v->root_sectors;
    if(v->data_start>=v->total) return DE_FORMAT;
    v->clusters=(v->total-v->data_start)/v->spc;
    if(!v->clusters || v->clusters>=65525) return DE_FORMAT;
    v->bits=v->clusters<4085?12:16;
    return ((u64)(v->clusters+2)*v->bits+7)/8>(u64)v->spf*512?DE_FORMAT:0;
}
int volume_fat(Volume *v,unsigned copy) {
    if(!v->fat) {int e=dos_alloc((v->spf*512+15)/16,(void **)&v->fat); if(e) return e;}
    return volume_read(v,v->reserved+copy*v->spf,v->spf,v->fat);
}
int volume_save_fat(Volume *v) {
    for(unsigned copy=0;copy<v->fats;copy++) {
        int e=volume_write(v,v->reserved+copy*v->spf,v->spf,v->fat); if(e) return e;
    }
    return 0;
}
void volume_free(Volume *v) {if(v->fat) dos_free(v->fat); v->fat=NULL;}
u32 fat_value(const Volume *v,u32 c) {
    if(v->bits==16) return rd16(v->fat+c*2);
    u16 value=rd16(v->fat+c+c/2); return (c&1?value>>4:value)&0xfff;
}
void fat_store(Volume *v,u32 c,u32 value) {
    if(v->bits==16) {wr16(v->fat+c*2,(u16)value); return;}
    u16 old=rd16(v->fat+c+c/2);
    wr16(v->fat+c+c/2,c&1?(u16)((old&15)|((value&0xfff)<<4)):(u16)((old&0xf000)|(value&0xfff)));
}
int fat_end(const Volume *v,u32 value) {return v->bits==12?value==0xff0 || value>=0xff8:value>=0xfff8;}
u32 fat_bad(const Volume *v) {return v->bits==12?0xff7:0xfff7;}
u32 fat_eof(const Volume *v) {return v->bits==12?0xfff:0xffff;}
u32 cluster_lba(const Volume *v,u32 c) {return v->data_start+(c-2)*v->spc;}
int read_text(const char *prompt,char *out,unsigned capacity) {
    if(prompt) print("%s",prompt);
    u8 buffer[130]={(u8)MIN(capacity,128U),0};
    int e=dos_line_input(buffer); print("\n");
    if(e) return e;
    unsigned n=buffer[1]; if(n>=capacity) n=capacity-1;
    memcpy(out,buffer+2,n); out[n]=0; return 0;
}
int ask_yes_no(const char *question) {
    for(;;) {
        char answer[8]; if(read_text(question,answer,sizeof(answer))) return 0;
        char *p=answer; while(*p==' ') p++;
        if(upper(*p)=='Y') return 1;
        if(upper(*p)=='N') return 0;
    }
}
int drive_argument(const char *text,unsigned *drive) {
    if(!text || !text[0] || text[1]!=':' || text[2]) return DE_DRIVE;
    char c=upper(text[0]); if(c<'A' || c>'Z') return DE_DRIVE;
    *drive=(unsigned)(c-'A'); return 0;
}
int refuse_mapped(unsigned drive,const char *utility) {
    DosDriveInfo info;
    if(dos_drive_info(drive,&info)) return 0;
    if(info.flags&(DOS_DRIVE_SUBST|DOS_DRIVE_ASSIGNED)) {print("Cannot %s a SUBSTed or ASSIGNed drive\n",utility); return 1;}
    if(info.flags&DOS_DRIVE_REMOTE) {print("Cannot %s a network drive\n",utility); return 1;}
    return 0;
}
int same_text(const char *a,const char *b) {return !stricmp(a,b);}
char *next_word(char **cursor) {
    char *p=*cursor; while(*p==' ' || *p=='\t') p++;
    if(!*p) {*cursor=p; return NULL;}
    char *start=p; while(*p && *p!=' ' && *p!='\t') p++;
    if(*p) *p++=0;
    *cursor=p; return start;
}
void serial_number(u32 serial,char out[10]) {
    static const char hex[]="0123456789ABCDEF";
    for(unsigned i=0;i<8;i++) out[i+(i>=4)]=hex[(serial>>(28-4*i))&15];
    out[4]='-'; out[9]=0;
}
/* MS-DOS 4 FORMAT Create_Serial_ID: date and time register sums. */
u32 new_serial(void) {
    DosRegs date={.ax=0x2a00},time={.ax=0x2c00};
    if(dos_call(&date) || dos_call(&time)) return 0x12345678;
    u16 low=(u16)(date.dx+time.dx),high=(u16)(date.cx+time.cx);
    return ((u32)high<<16)|low;
}
void fat_timestamp(u16 *date,u16 *time) {
    DosDateTime t; *date=0x21; *time=0;
    if(dos_get_datetime(&t) || t.year<1980) return;
    *date=(u16)(((t.year-1980)<<9)|(t.month<<5)|t.day);
    *time=(u16)((t.hour<<11)|(t.minute<<5)|(t.second/2));
}
/* Country-formatted FAT date and time, e.g. "09-29-2026 12:00a". */
void date_text(u16 date,u16 time,char out[24]) {
    DosCountryInfo c={.size=sizeof(c)}; if(dos_country_info(DOS_NLS_CURRENT,DOS_NLS_CURRENT,&c)) c=(DosCountryInfo){0};
    char ds=c.date_separator[0]?(char)c.date_separator[0]:'-',ts=c.time_separator[0]?(char)c.time_separator[0]:':';
    unsigned year=(date>>9)+1980,month=(date>>5)&15,day=date&31,hour=time>>11,minute=(time>>5)&63;
    unsigned a=c.date_order==1?day:c.date_order==2?year:month,b=c.date_order==1?month:c.date_order==2?month:day,
        z=c.date_order==2?day:year;
    char suffix=0;
    if(!c.time_format) {suffix=hour>=12?'p':'a'; hour%=12; if(!hour) hour=12;}
    unsigned n=0; const unsigned parts[3]={a,b,z};
    for(unsigned i=0;i<3;i++) {
        unsigned v=parts[i],width=v>=1000?4:2;
        for(unsigned d=width;d;d--) {unsigned div=1; for(unsigned k=1;k<d;k++) div*=10; out[n++]='0'+v/div%10;}
        if(i<2) out[n++]=ds;
    }
    out[n++]=' ';
    if(hour>=10 || c.time_format) out[n++]='0'+hour/10%10; else out[n++]=' ';
    out[n++]='0'+hour%10; out[n++]=ts; out[n++]='0'+minute/10; out[n++]='0'+minute%10;
    if(suffix) out[n++]=suffix;
    out[n]=0;
}
int label_name(const char *text,u8 out[11]) {
    memset(out,' ',11); unsigned n=0;
    for(const u8 *p=(const u8 *)text;*p;p++) {
        if(*p<32 || strchr("*?/\\|.,;:+=[]<>\"",*p)) return DE_PATH;
        if(n==11) return DE_PATH;
        out[n++]=(u8)upper((char)*p);
    }
    return n?0:DE_PATH;
}
static int label_fcb(unsigned function,unsigned drive,const u8 name[11]) {
    DosExtendedFcb fcb; memset(&fcb,0,sizeof(fcb)); fcb.marker=255; fcb.attr=FA_VOLUME;
    fcb.fcb.bytes[0]=(u8)(drive+1); memcpy(fcb.fcb.bytes+1,name,11);
    unsigned status; return dos_fcb_call(function,&fcb,NULL,&status);
}
int label_get(unsigned drive,char out[12]) {
    void *saved; u32 capacity; out[0]=0;
    int e=dos_get_dta(&saved,&capacity); if(e) return e;
    u8 dta[64]; e=dos_set_dta(dta,sizeof(dta)); if(e) return e;
    u8 any[11]; memset(any,'?',11);
    e=label_fcb(0x11,drive,any);
    if(!e) {memcpy(out,dta+8,11); out[11]=0;}
    int restore=dos_set_dta(saved,capacity);
    return e?e:restore;
}
int label_set(unsigned drive,const u8 *name) {
    u8 any[11]; memset(any,'?',11);
    int e=label_fcb(0x13,drive,any); /* Delete: absent labels are not an error. */
    if(e && e!=DE_NOFILE && e!=DE_NOMORE) return e;
    return name?label_fcb(0x16,drive,name):0;
}
int load_file(const char *path,u8 **data,u32 *size) {
    unsigned h; int e=dos_open(path,DOS_OPEN_READ,0,&h); if(e) return e;
    u32 pos,got=0; *data=NULL; e=dos_seek(h,0,2,size);
    if(!e) e=dos_alloc((*size+16)/16,(void **)data);
    if(!e) e=dos_seek(h,0,0,&pos);
    if(!e) {e=dos_read(h,*data,*size,&got); if(!e && got!=*size) e=DE_IO;}
    int c=dos_close(h); if(!e) e=c;
    if(e && *data) {dos_free(*data); *data=NULL;}
    return e;
}
int store_file(const char *path,const u8 *data,u32 size,u8 attr) {
    u8 old=0; if(!dos_attribute(path,0,&old) && (old&FA_RDONLY)) {old=0; dos_attribute(path,1,&old);}
    unsigned h; int e=dos_open(path,DOS_OPEN_WRITE,1,&h); if(e) return e;
    u32 written; e=dos_write(h,data,size,&written); if(!e && written!=size) e=DE_FULL;
    int c=dos_close(h); if(!e) e=c;
    if(!e && attr) e=dos_attribute(path,1,&attr);
    return e;
}
