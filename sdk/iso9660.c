/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "iso9660.h"
#define FA_RDONLY 0x01
#define FA_HIDDEN 0x02
#define FA_DIR 0x10
static int sector(IsoVolume *v,u32 lba,const u8 **out) {
    if(lba>=v->volume_sectors) return DE_SEEK;
    if(v->cached!=lba) {
        v->cached=UINT32_MAX;
        int e=v->read(v->context,lba,1,v->buffer); if(e) return e;
        v->cached=lba;
    }
    *out=v->buffer; return 0;
}
static u32 rd32le(const u8 *p) {return rd32(p);}
static void dos_stamp(const u8 *t,u16 *date,u16 *time) {
    unsigned year=1900+t[0];
    if(year<1980 || year>2107 || t[1]<1 || t[1]>12 || t[2]<1 || t[2]>31) {*date=(1<<5)|1; *time=0; return;}
    *date=(u16)(((year-1980)<<9)|(t[1]<<5)|t[2]);
    *time=(u16)((MIN(t[3],23U)<<11)|(MIN(t[4],59U)<<5)|(MIN(t[5],59U)/2));
}
static int valid83(const char *s) {
    unsigned n=0,e=0,dot=0;
    for(;*s;s++) {
        char c=*s;
        if(c=='.') {if(dot++) return 0; continue;}
        if(!((c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='$' || c=='~' || c=='!' ||
             c=='#' || c=='%' || c=='&' || c=='@' || c=='(' || c==')' || c=='{' || c=='}' || c=='\'')) return 0;
        if(dot) e++; else n++;
    }
    return n>=1 && n<=8 && e<=3;
}
/* Directory record name to 8.3, or 0 to skip it. */
static int dos_name(const u8 *r,char out[13]) {
    unsigned length=r[32],n=0;
    if(length==1 && (r[33]==0 || r[33]==1)) return 0; /* . and .. */
    if(length>=13+2) return 0;
    for(unsigned i=0;i<length && r[33+i]!=';';i++) {
        char c=(char)r[33+i]; if(c>='a' && c<='z') c-=32;
        out[n++]=c;
    }
    if(n && out[n-1]=='.') n--;
    out[n]=0;
    return valid83(out);
}
int iso_mount(IsoVolume *v,IsoRead read,void *context) {
    memset(v,0,sizeof(*v)); v->read=read; v->context=context; v->cached=UINT32_MAX; v->volume_sectors=UINT32_MAX;
    for(u32 lba=16;lba<16+32;lba++) {
        const u8 *d; int e=sector(v,lba,&d); if(e) return e;
        if(memcmp(d+1,"CD001",5) || d[6]!=1) return DE_FORMAT;
        if(d[0]==255) return DE_FORMAT;
        if(d[0]!=1) continue;
        if(rd16(d+128)!=ISO_SECTOR) return DE_FORMAT;
        v->volume_sectors=rd32le(d+80);
        const u8 *root=d+156;
        v->root_sector=rd32le(root+2); v->root_size=rd32le(root+10);
        unsigned n=0;
        for(unsigned i=0;i<11;i++) v->label[n++]=(char)d[40+i];
        while(n && v->label[n-1]==' ') n--;
        v->label[n]=0;
        if(!v->volume_sectors || v->root_sector>=v->volume_sectors || !v->root_size) return DE_FORMAT;
        v->cached=UINT32_MAX;
        return 0;
    }
    return DE_FORMAT;
}
int iso_next(IsoVolume *v,u32 lba,u32 size,u32 *offset,IsoEntry *out) {
    while(*offset<size) {
        u32 within=*offset%ISO_SECTOR; const u8 *d;
        int e=sector(v,lba+*offset/ISO_SECTOR,&d); if(e) return e;
        unsigned length=d[within];
        if(!length) {*offset+=ISO_SECTOR-within; continue;} /* records never cross a sector */
        if(length<34 || within+length>ISO_SECTOR || 33U+d[within+32]>length) return DE_FORMAT;
        const u8 *r=d+within; *offset+=length;
        if(r[25]&0x80) continue; /* multi-extent */
        if(!dos_name(r,out->name)) continue;
        out->sector=rd32le(r+2); out->size=rd32le(r+10);
        out->attributes=FA_RDONLY|(r[25]&2?FA_DIR:0)|(r[25]&1?FA_HIDDEN:0);
        dos_stamp(r+18,&out->date,&out->time);
        if(out->sector>=v->volume_sectors) return DE_FORMAT;
        return 0;
    }
    return DE_NOMORE;
}
int iso_lookup(IsoVolume *v,const char *path,IsoEntry *out) {
    IsoEntry here={v->root_sector,v->root_size,(1<<5)|1,0,FA_RDONLY|FA_DIR,""};
    while(*path=='\\') path++;
    while(*path) {
        char part[13]; unsigned n=0;
        while(*path && *path!='\\') {if(n>=12) return DE_PATH; part[n++]=*path++;}
        part[n]=0;
        while(*path=='\\') path++;
        if(!(here.attributes&FA_DIR)) return DE_PATH;
        u32 offset=0; IsoEntry e; int found=0,err;
        while(!(err=iso_next(v,here.sector,here.size,&offset,&e))) if(!strcmp(e.name,part)) {found=1; break;}
        if(!found) return err==DE_NOMORE?(*path?DE_PATH:DE_NOFILE):err;
        here=e;
    }
    *out=here; return 0;
}
int iso_read(IsoVolume *v,const IsoEntry *file,u32 offset,void *buffer,u32 count,u32 *done) {
    *done=0;
    if(offset>=file->size) return 0;
    count=MIN(count,file->size-offset);
    u8 *p=buffer;
    while(*done<count) {
        u32 at=offset+*done,lba=file->sector+at/ISO_SECTOR,within=at%ISO_SECTOR;
        u32 left=count-*done;
        if(!within && left>=ISO_SECTOR) {
            u32 whole=left/ISO_SECTOR; if(whole>16) whole=16;
            if(lba+whole>v->volume_sectors) return DE_FORMAT;
            int e=v->read(v->context,lba,whole,p+*done); if(e) return e;
            *done+=whole*ISO_SECTOR; continue;
        }
        const u8 *d; int e=sector(v,lba,&d); if(e) return e;
        u32 take=MIN(left,ISO_SECTOR-within);
        memcpy(p+*done,d+within,take); *done+=take;
    }
    return 0;
}
