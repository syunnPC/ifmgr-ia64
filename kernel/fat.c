/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * Copyright (c) Microsoft Corporation (translated MS-DOS 4 portions,
 * MIT License; see vendor/msdos4/LICENSE). Other code: GPL-2.0-or-later.
 * FAT UNPACK/PACK/IsEOF translated from MS-DOS 4 DOS/FAT.ASM.
 * The block adapter, directory and stream implementation are new portable C.
 */
#include "fat_io.h"
#include "nls.h"
#define read_sector fat_sector_read
#define write_sector fat_sector_write
int fat_parse_bpb(const u8 *b,u64 sectors,FatBpb *p) {
    if(rd16(b+510)!=0xaa55 || rd16(b+11)!=512) return DE_FORMAT;
    u32 spc=b[13],reserved=rd16(b+14),fats=b[16],root=rd16(b+17),spf=rd16(b+22),total=rd16(b+19);
    if(!total) total=rd32(b+32);
    if(!spc || (spc&(spc-1)) || spc>128 || !reserved || !fats || fats>2 || !root || !spf || total>sectors) return DE_FORMAT;
    u32 root_start=reserved+fats*spf,data_start=root_start+(root*32+511)/512;
    if(data_start>=total) return DE_FORMAT;
    u32 clusters=(total-data_start)/spc,bits=clusters<4085?12:16;
    if(!clusters || clusters>=65525 || ((u64)(clusters+2)*bits+7)/8>(u64)spf*512) return DE_FORMAT;
    *p=(FatBpb){spc,reserved,fats,root,spf,total,root_start,data_start,clusters,bits}; return 0;
}
int fat_mount(Fat *f,const Disk *disk) {
    return fat_mount_ex(f,disk,NULL,NULL);
}
int fat_mount_ex(Fat *f,const Disk *disk,FatErrorHandler handler,void *context) {
    Disk source=*disk; memset(f,0,sizeof(*f)); f->disk=source; f->faulted=1; disk=&f->disk;
    f->error_handler=handler; f->error_context=context;
    u8 b[512]; int e=fat_io_read(f,0,b,DOS_ERROR_MOUNT); if(e) return e;
    FatBpb p; e=fat_parse_bpb(b,disk->sectors,&p); if(e) return e;
    f->spc=p.spc; f->fat_start=p.reserved; f->copies=p.fats;
    f->root_entries=p.root_entries; f->fat_sectors=p.fat_sectors; f->total=p.total;
    f->root_start=p.root_start; f->data_start=p.data_start; f->clusters=p.clusters; f->bits=p.bits;
    /* Never silently pick a FAT copy after an interrupted or external update.
     * CHKDSK/recovery must resolve the disagreement before this volume is used. */
    u8 other[512];
    if(f->copies>1) for(u32 i=0;i<f->fat_sectors;i++) {
        e=fat_io_read(f,f->fat_start+i,b,DOS_ERROR_MOUNT); if(e) return e;
        e=fat_io_read(f,f->fat_start+f->fat_sectors+i,other,DOS_ERROR_MOUNT); if(e) return e;
        if(memcmp(b,other,512)) return DE_IO;
    }
    f->faulted=0;
    return 0;
}
int fat_eof(const Fat *f,u16 value) {
    /* DOS 4 also accepts 0xFF0 on FAT12 (FAT.ASM IsEOF_other). */
    return f->bits==12 ? value==0xff0 || value>=0xff8 : value>=0xfff8;
}
int fat_get(Fat *f,u32 c,u16 *value) {
    u8 next[512]; u32 off=f->bits==12 ? c+c/2 : c*2;
    if(f->faulted || c>=f->clusters+2) return DE_IO;
    u32 sector=f->fat_start+off/512; int e;
    if(!f->cache_valid || f->cache_sector!=sector) {
        e=read_sector(f,sector,f->cache); if(e) return e;
        f->cache_sector=sector; f->cache_valid=1;
    }
    u16 v=f->cache[off%512];
    if(off%512==511) {
        e=read_sector(f,f->fat_start+off/512+1,next); if(e) return e;
        v|=(u16)next[0]<<8;
    } else v|=(u16)f->cache[off%512+1]<<8;
    /* UNPACK: odd FAT12 clusters occupy the upper twelve bits. */
    if(f->bits==12) {if(c&1) v>>=4; v&=0xfff;}
    *value=v; return 0;
}
static int set_entry(Fat *f,u32 c,u16 value) {
    u8 b[512],next[512]; u32 off=f->bits==12?c+c/2:c*2;
    if(c<2 || c>=f->clusters+2) return DE_IO;
    for(unsigned copy=0;copy<f->copies;copy++) {
        u32 lba=f->fat_start+copy*f->fat_sectors+off/512;
        int e=read_sector(f,lba,b); if(e) return e;
        u16 old=b[off%512];
        if(off%512==511) {e=read_sector(f,lba+1,next); if(e) return e; old|=(u16)next[0]<<8;}
        else old|=(u16)b[off%512+1]<<8;
        u16 v=value;
        /* PACK: preserve the neighbouring cluster's nibble. */
        if(f->bits==12) v=c&1 ? (old&15)|((value&0xfff)<<4) : (old&0xf000)|(value&0xfff);
        b[off%512]=v;
        if(off%512==511) next[0]=v>>8; else b[off%512+1]=v>>8;
        e=write_sector(f,lba,b); if(e) return e;
        if(off%512==511) {e=write_sector(f,lba+1,next); if(e) return e;}
    }
    return 0;
}
int fat_set(Fat *f,u32 c,u16 value) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,set_entry(f,c,value));
}
static int valid_cluster(Fat *f,u16 c) {
    return c>=2 && c<f->clusters+2 && c<(f->bits==12?0xff0:0xfff0);
}
static u32 cluster_sector(Fat *f,u16 c) {return f->data_start+(c-2)*f->spc;}
static int next_cluster(Fat *f,u16 c,u16 *next) {
    int e=fat_get(f,c,next); if(e) return e;
    if(fat_eof(f,*next)) return DE_NOMORE;
    return valid_cluster(f,*next)?0:DE_IO;
}
static int alloc_cluster(Fat *f,u16 *result) {
    u8 zero[512]; memset(zero,0,sizeof(zero));
    for(u32 c=2;c<f->clusters+2;c++) {
        u16 v; int e=fat_get(f,c,&v); if(e) return e;
        if(v || !valid_cluster(f,(u16)c)) continue;
        for(u32 s=0;s<f->spc;s++) {
            e=write_sector(f,cluster_sector(f,(u16)c)+s,zero);
            if(e) return e;
        }
        e=fat_set(f,c,0xffff); if(e) return e;
        *result=c; return 0;
    }
    return DE_FULL;
}
static int free_chain(Fat *f,u16 c) {
    for(u32 budget=f->clusters;c && budget;budget--) {
        u16 n; if(!valid_cluster(f,c)) return DE_IO;
        int e=fat_get(f,c,&n); if(e) return e;
        if(!fat_eof(f,n) && !valid_cluster(f,n)) return DE_IO;
        e=fat_set(f,c,0); if(e) return e;
        if(fat_eof(f,n)) return 0;
        c=n;
    }
    return c?DE_IO:0;
}
/* Fetch/extend one cluster, bounding chain walks even on damaged media. */
static int nth_cluster(Fat *f,u16 first,u32 index,int grow,u16 *out) {
    if(!valid_cluster(f,first) || index>=f->clusters) return DE_IO;
    u16 c=first;
    while(index--) {
        u16 next; int e=next_cluster(f,c,&next);
        if(e==DE_NOMORE && grow) {
            e=alloc_cluster(f,&next); if(e) return e;
            e=fat_set(f,c,next); if(e) return e;
        } else if(e) return e;
        c=next;
    }
    *out=c; return 0;
}
u16 fat_cluster(const Node *n) {return rd16(n->raw+26);}
u32 fat_size(const Node *n) {return rd32(n->raw+28);}
static int dir_entry(Fat *f,u16 dir,u32 index,int grow,Node *n) {
    u32 lba;
    if(!dir) {
        if(index>=f->root_entries) return DE_NOMORE;
        lba=f->root_start+index/16;
    } else {
        u16 c; int e=nth_cluster(f,dir,index/(f->spc*16),grow,&c); if(e) return e;
        lba=cluster_sector(f,c)+(index/16)%f->spc;
    }
    u8 buf[512]; int e=read_sector(f,lba,buf); if(e) return e;
    n->sector=lba; n->offset=(index%16)*32;
    memcpy(n->raw,buf+n->offset,32); return 0;
}
static int sync_node(Fat *f,Node *n) {
    if(!n->sector || n->offset>480) return DE_ACCESS;
    u8 b[512]; int e=read_sector(f,n->sector,b); if(e) return e;
    memcpy(b+n->offset,n->raw,32); return write_sector(f,n->sector,b);
}
int fat_sync_node(Fat *f,Node *n) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,sync_node(f,n));
}
int fat_next(Fat *f,u16 dir,u32 *index,Node *n) {
    for(;;) {
        int e=dir_entry(f,dir,(*index)++,0,n); if(e) return e;
        if(!n->raw[0]) return DE_NOMORE;
        if(n->raw[0]!=0xe5 && n->raw[11]!=0x0f) return 0;
    }
}
int fat_name83(const char *s,u8 out[11]) {
    memset(out,' ',11); unsigned n=0,end=8; int extension=0;
    if(!*s || *s=='.') return DE_PATH;
    while(*s) {
        unsigned width=nls_char_size(s); if(!width) return DE_PATH;
        if(width==1 && *s=='.') {
            if(extension) return DE_PATH;
            extension=1; n=8; end=11; s++; continue;
        }
        if(n+width>end) return DE_PATH;
        if(width==2) {out[n++]=(u8)*s++; out[n++]=(u8)*s++;}
        else {
            u8 c=(u8)*s++;
            if(!nls_file_char(c) || c=='*' || c=='?') return DE_PATH;
            out[n++]=nls_upper(c,1);
        }
    }
    if(out[0]==0xe5) out[0]=5;
    return 0;
}
void fat_name(const Node *n,char out[13]) {
    unsigned p=0;
    for(unsigned start=0;start<11;start+=8) {
        unsigned end=start?11:8;
        if(start && n->raw[start]!=' ') out[p++]='.';
        for(unsigned i=start;i<end && n->raw[i]!=' ';i++) {
            u8 c=!i && n->raw[0]==5?0xe5:n->raw[i]; out[p++]=c;
            if(nls_lead(c) && i+1<end) out[p++]=n->raw[++i];
        }
    }
    out[p]=0;
}
static int find_in(Fat *f,u16 dir,const u8 name[11],Node *n) {
    u32 index=0; int e;
    while(!(e=fat_next(f,dir,&index,n))) if(!(n->raw[11]&FA_VOLUME) && !memcmp(n->raw,name,11)) return 0;
    return e==DE_NOMORE?DE_NOFILE:e;
}
int fat_lookup(Fat *f,const char *path,Node *n) {
    if(f->faulted) return DE_IO;
    u16 dir=0; memset(n,0,sizeof(*n)); n->raw[11]=FA_DIR;
    while(*path=='\\') path++;
    while(*path) {
        char part[13]; unsigned len=0;
        while(*path && *path!='\\') {
            unsigned width=nls_char_size(path);
            if(!width || len+width>12) return DE_PATH;
            memcpy(part+len,path,width); len+=width; path+=width;
        }
        part[len]=0; if(*path) path++;
        u8 key[11]; int e=fat_name83(part,key); if(e) return e;
        e=find_in(f,dir,key,n); if(e) return *path && e==DE_NOFILE?DE_PATH:e;
        if(*path) {
            if(!(n->raw[11]&FA_DIR)) return DE_PATH;
            dir=fat_cluster(n); if(!valid_cluster(f,dir)) return DE_IO;
        }
    }
    return 0;
}
static int parent(Fat *f,const char *path,u16 *dir,u8 name[11]) {
    char buf[DOS_PATH_MAX]; int e=strcopy(buf,sizeof(buf),path); if(e) return e;
    char *last=nls_last_sep(buf,"\\");
    char *leaf=last?last+1:buf;
    e=fat_name83(leaf,name); if(e) return e;
    if(last) *last=0; else buf[0]=0;
    Node n; e=fat_lookup(f,buf,&n); if(e) return e==DE_NOFILE?DE_PATH:e;
    if(!(n.raw[11]&FA_DIR)) return DE_PATH;
    *dir=fat_cluster(&n); return 0;
}
static void stamp(Node *n) {
    u16 date,time; platform_fat_time(&date,&time); wr16(n->raw+22,time); wr16(n->raw+24,date);
}
static int free_slot(Fat *f,u16 dir,Node *n) {
    for(u32 i=0;;i++) {
        int e=dir_entry(f,dir,i,1,n); if(e) return e==DE_NOMORE?DE_FULL:e;
        if(!n->raw[0]) {
            /* Keep an end marker after replacing one. Bytes beyond the old
             * marker are unspecified and must not become visible as files. */
            Node next; e=dir_entry(f,dir,i+1,0,&next);
            if(e && e!=DE_NOMORE) return e;
            if(!e && next.raw[0]) {next.raw[0]=0; e=fat_sync_node(f,&next); if(e) return e;}
            return 0;
        }
        if(n->raw[0]==0xe5) return 0;
    }
}
static int create_file(Fat *f,const char *path,u8 attr,Node *n) {
    u16 dir; u8 name[11]; int e=parent(f,path,&dir,name); if(e) return e;
    e=find_in(f,dir,name,n); if(!e) return DE_EXISTS; if(e!=DE_NOFILE) return e;
    e=free_slot(f,dir,n); if(e) return e;
    u16 first=0;
    if(attr&FA_DIR) {
        e=alloc_cluster(f,&first); if(e) return e;
        u8 b[512]; memset(b,0,sizeof(b)); memset(b,' ',11); b[0]='.'; b[11]=FA_DIR; wr16(b+26,first);
        memcpy(b+32,b,32); b[33]='.'; wr16(b+58,dir);
        e=write_sector(f,cluster_sector(f,first),b); if(e) return e;
    }
    memset(n->raw,0,32); memcpy(n->raw,name,11); n->raw[11]=attr;
    wr16(n->raw+26,first); stamp(n);
    return fat_sync_node(f,n);
}
int fat_create(Fat *f,const char *path,u8 attr,Node *n) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,create_file(f,path,attr,n));
}
static int remove_file(Fat *f,const char *path,int directory) {
    Node n; int e=fat_lookup(f,path,&n); if(e) return e;
    if(!n.sector || (n.raw[11]&FA_RDONLY) || !!(n.raw[11]&FA_DIR)!=!!directory) return DE_ACCESS;
    u16 first=fat_cluster(&n);
    if(directory) {
        Node child; u32 i=0;
        while(!(e=fat_next(f,first,&i,&child))) if(child.raw[0]!='.') return DE_ACCESS;
        if(e!=DE_NOMORE) return e;
    }
    /* Unlink before freeing during commit; all changes have preimages. */
    n.raw[0]=0xe5; e=fat_sync_node(f,&n); if(e) return e;
    return free_chain(f,first);
}
int fat_remove(Fat *f,const char *path,int directory) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,remove_file(f,path,directory));
}
static int rename_file(Fat *f,const char *from,const char *to) {
    u16 a,b; u8 old[11],name[11]; Node n,exists;
    int e=parent(f,from,&a,old); if(e) return e;
    e=parent(f,to,&b,name); if(e) return e;
    e=find_in(f,a,old,&n); if(e) return e;
    if(n.raw[11]&FA_RDONLY) return DE_ACCESS;
    e=find_in(f,b,name,&exists); if(!e) return DE_EXISTS; if(e!=DE_NOFILE) return e;
    if(a==b) {memcpy(n.raw,name,11); return fat_sync_node(f,&n);}
    u16 first=fat_cluster(&n);
    if(n.raw[11]&FA_DIR) {
        /* Follow '..' on disk, rejecting moves into self/descendants and
         * malformed ancestry before changing either directory. */
        u16 ancestor=b; u32 budget=f->clusters;
        while(ancestor) {
            if(ancestor==first) return DE_ACCESS;
            if(!budget-- || !valid_cluster(f,ancestor)) return DE_IO;
            Node up; e=dir_entry(f,ancestor,1,0,&up); if(e) return e;
            if(memcmp(up.raw,"..         ",11) || !(up.raw[11]&FA_DIR)) return DE_IO;
            ancestor=fat_cluster(&up);
        }
    }
    Node original=n; n.raw[0]=0xe5;
    e=fat_sync_node(f,&n); if(e) return e;
    if(n.raw[11]&FA_DIR) {
        Node up; e=dir_entry(f,first,1,0,&up); if(e) return e;
        if(memcmp(up.raw,"..         ",11) || !(up.raw[11]&FA_DIR)) return DE_IO;
        wr16(up.raw+26,b); e=fat_sync_node(f,&up); if(e) return e;
    }
    Node dest; e=free_slot(f,b,&dest); if(e) return e;
    memcpy(dest.raw,original.raw,32); memcpy(dest.raw,name,11);
    return fat_sync_node(f,&dest);
}
int fat_rename(Fat *f,const char *from,const char *to) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,rename_file(f,from,to));
}
static int boot_label(Fat *f,const u8 *name) {
    u8 boot[512]; int e=read_sector(f,0,boot); if(e) return e;
    /* Older BPBs have no extended label field. Do not overwrite boot code. */
    if(boot[38]!=0x29) return 0;
    memcpy(boot+43,name?name:(const u8 *)"NO NAME    ",11);
    return write_sector(f,0,boot);
}
static int create_label(Fat *f,const u8 name[11],Node *out) {
    Node n; u32 index=0; int e;
    while(!(e=fat_next(f,0,&index,&n))) if(n.raw[11]&FA_VOLUME) return DE_ACCESS;
    if(e!=DE_NOMORE) return e;
    e=free_slot(f,0,&n); if(e) return e;
    memset(n.raw,0,32); memcpy(n.raw,name,11); n.raw[11]=FA_VOLUME; stamp(&n);
    e=fat_sync_node(f,&n); if(!e) e=boot_label(f,name);
    if(!e) *out=n;
    return e;
}
int fat_create_label(Fat *f,const u8 name[11],Node *out) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,create_label(f,name,out));
}
static int change_entry(Fat *f,u16 dir,const Node *original,const u8 *name) {
    if(original->raw[11]&(FA_RDONLY|FA_DIR)) return DE_ACCESS;
    if((original->raw[11]&FA_VOLUME) && (dir || fat_cluster(original) || fat_size(original))) return DE_IO;
    /* Bind the operation to the found entry, including its directory. */
    Node n; u32 index=0; int e,found=0;
    while(!(e=fat_next(f,dir,&index,&n))) {
        if(n.sector==original->sector && n.offset==original->offset) {
            if(memcmp(n.raw,original->raw,32)) return DE_CHANGED;
            found=1;
        }
        if(name && !!(n.raw[11]&FA_VOLUME)==!!(original->raw[11]&FA_VOLUME) && !memcmp(n.raw,name,11)) return DE_EXISTS;
    }
    if(e!=DE_NOMORE) return e;
    if(!found) return DE_NOFILE;
    n=*original;
    if(name) memcpy(n.raw,name,11); else n.raw[0]=0xe5;
    e=fat_sync_node(f,&n); if(e) return e;
    if(n.raw[11]&FA_VOLUME) return boot_label(f,name);
    if(name) return 0;
    return free_chain(f,fat_cluster(&n));
}
int fat_change_entry(Fat *f,u16 dir,const Node *original,const u8 *name) {
    int e=fat_begin(f); if(e) return e;
    return fat_end(f,change_entry(f,dir,original,name));
}
int fat_read(Fat *f,FatFile *file,void *dst,u32 count,u32 *done) {
    *done=0; u8 *p=dst; u32 size=fat_size(&file->node);
    if(f->faulted) return DE_IO;
    if(file->pos>=size) return 0;
    count=MIN(count,size-file->pos);
    while(count) {
        u16 c; int e=nth_cluster(f,fat_cluster(&file->node),file->pos/(f->spc*512),0,&c);
        if(e) return e==DE_NOMORE?DE_IO:e;
        u8 buf[512]; u32 off=file->pos%512;
        e=fat_data_read(f,cluster_sector(f,c)+(file->pos/512)%f->spc,buf); if(e) return e;
        u32 take=MIN(count,512-off); memcpy(p,buf+off,take);
        file->pos+=take; *done+=take; p+=take; count-=take;
    }
    return 0;
}
static int write_piece(Fat *f,FatFile *file,const u8 *data,u32 count) {
    u16 c,first=fat_cluster(&file->node); int e;
    if(!first) {
        e=alloc_cluster(f,&first); if(e) return e;
        wr16(file->node.raw+26,first);
    }
    e=nth_cluster(f,first,file->pos/(f->spc*512),1,&c); if(e) return e;
    u32 off=file->pos%512,sector=cluster_sector(f,c)+(file->pos/512)%f->spc; u8 buf[512];
    if(off || count<512) {e=read_sector(f,sector,buf); if(e) return e;}
    if(data) memcpy(buf+off,data,count); else memset(buf+off,0,count);
    e=write_sector(f,sector,buf); if(e) return e;
    file->pos+=count;
    if(file->pos>fat_size(&file->node)) wr32(file->node.raw+28,file->pos);
    file->node.raw[11]|=FA_ARCHIVE; stamp(&file->node);
    return fat_sync_node(f,&file->node);
}
static int write_data(Fat *f,FatFile *file,const u8 *data,u32 count,u32 *done) {
    *done=0;
    /* The DOS handle layer authorizes writes, including creating a read-only
     * file whose initial handle remains writable until closed. */
    if(file->node.raw[11]&FA_DIR) return DE_ACCESS;
    if(count>UINT32_MAX-file->pos) return DE_FULL;
    while(count) {
        u32 take=MIN(count,512-file->pos%512);
        FatFile before=*file; int e=fat_begin(f); if(e) return e;
        e=fat_end(f,write_piece(f,file,data,take));
        if(e) {*file=before; return e;}
        if(data) data+=take;
        count-=take; *done+=take;
    }
    return 0;
}
int fat_write(Fat *f,FatFile *file,const void *data,u32 count,u32 *done) {
    *done=0;
    if(!count) return fat_truncate(f,file,file->pos);
    if(file->pos>fat_size(&file->node)) {
        u32 wanted=file->pos, filled;
        file->pos=fat_size(&file->node);
        int e=write_data(f,file,NULL,wanted-file->pos,&filled);
        file->pos=wanted; if(e) return e;
    }
    return write_data(f,file,data,count,done);
}
static int shrink_file(Fat *f,FatFile *file,u32 size) {
    u16 first=fat_cluster(&file->node), tail=first, last=0;
    if(size && first) {
        int e=nth_cluster(f,first,(size-1)/(f->spc*512),0,&last); if(e) return e;
        e=next_cluster(f,last,&tail); if(e==DE_NOMORE) tail=0; else if(e) return e;
    }
    if(!size) wr16(file->node.raw+26,0);
    wr32(file->node.raw+28,size); file->node.raw[11]|=FA_ARCHIVE; stamp(&file->node);
    int e=fat_sync_node(f,&file->node); if(e) return e;
    if(last && tail) {e=fat_set(f,last,0xffff); if(e) return e;}
    return free_chain(f,tail);
}
int fat_truncate(Fat *f,FatFile *file,u32 size) {
    if(file->node.raw[11]&FA_DIR) return DE_ACCESS;
    u32 old=fat_size(&file->node),saved=file->pos;
    if(size>old) {
        u32 done; file->pos=old; int e=write_data(f,file,NULL,size-old,&done); file->pos=saved; return e;
    }
    FatFile before=*file; int e=fat_begin(f); if(e) return e;
    e=fat_end(f,shrink_file(f,file,size));
    if(e) *file=before;
    return e;
}
int fat_replace(Fat *f,FatFile *file,u8 attr) {
    FatFile before=*file; int e=fat_begin(f); if(e) return e;
    e=shrink_file(f,file,0);
    if(!e) {file->node.raw[11]=attr|FA_ARCHIVE; e=fat_sync_node(f,&file->node);}
    e=fat_end(f,e); if(e) *file=before;
    return e;
}
int fat_free_space(Fat *f,u32 *free) {
    *free=0; for(u32 c=2;c<f->clusters+2;c++) {
        u16 v; int e=fat_get(f,c,&v); if(e) return e; if(!v && valid_cluster(f,c)) (*free)++;
    }
    return 0;
}
