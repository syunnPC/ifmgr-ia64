/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * CON code pages: the DOS 4 DISPLAY.SYS prepare/select model for a Unicode
 * console. Prepared pages are translation tables, not fonts; the hardware
 * page 437 is compiled in. Tables live in the DOS arena under a system owner.
 */
#include "dos.h"
#include "codepage.h"
#include "console.h"
#include "keyb.h"
#include "../build/codepage_data.h"
#define SYSTEM_OWNER 0xffffffffU
#define STAGING_LIMIT (128U*1024U)
typedef struct {
    u16 page; u8 dbcs,leads; u8 ranges[16]; u8 slot[256];
    const u16 *single,*pairs; void *memory;
} Table;
static Table hardware,prepared[DOS_CP_PREPARED_MAX];
static const Table *selected;
static u8 *staging;
static u32 staged;
static int preparing,prepare_error;
static u16 requested[DOS_CP_PREPARED_MAX];
static unsigned requested_count;
static int allocate(u32 bytes,void **out) {
    u8 method=dos_arena.method; u32 largest; dos_arena.method=2;
    int e=arena_alloc(&dos_arena,(bytes+15)/16,SYSTEM_OWNER,out,&largest);
    dos_arena.method=method; return e;
}
static void release(void *p) {if(p) arena_free(&dos_arena,p,SYSTEM_OWNER);}
static void end_prepare(void) {release(staging); staging=NULL; staged=0; preparing=prepare_error=0;}
/* dos_init reinitializes the arena first, so earlier tables are not freed. */
void codepage_reset(void) {
    memset(prepared,0,sizeof(prepared)); staging=NULL; staged=0; preparing=prepare_error=0;
    hardware=(Table){.page=DOS_CP_HARDWARE,.single=builtin_cp437}; selected=&hardware;
}
static const Table *current_table(void) {if(!selected) codepage_reset(); return selected;}
int codepage_lead(u8 c) {const Table *t=current_table(); return t->dbcs && t->slot[c];}
u16 codepage_selected(void) {return current_table()->page;}
/* A trail must be a printable DBCS byte; anything else ends the character. */
u16 codepage_unicode(u8 c,int pair,u8 trail) {
    const Table *t=current_table();
    if(!pair) return t->single[c];
    if(!t->dbcs || !t->slot[c] || trail<0x40 || trail==0x7f) return 0;
    return t->pairs[(u32)(t->slot[c]-1)*256+trail];
}
unsigned codepage_encode(u32 unicode,u8 out[2]) {
    const Table *t=current_table();
    if(!unicode || unicode>0xffff) return 0;
    if(unicode<128 && t->single[unicode]==unicode) {out[0]=(u8)unicode; return 1;}
    for(unsigned c=1;c<256;c++) if((!t->dbcs || !t->slot[c]) && t->single[c]==unicode) {out[0]=(u8)c; return 1;}
    if(t->dbcs) for(unsigned c=128;c<256;c++) if(t->slot[c]) {
        const u16 *row=t->pairs+(u32)(t->slot[c]-1)*256;
        for(unsigned trail=0x40;trail<256;trail++) if(row[trail]==unicode) {out[0]=(u8)c; out[1]=(u8)trail; return 2;}
    }
    /* Typed ASCII stays usable on pages that remap it (e.g. 864's percent). */
    if(unicode<128) {out[0]=(u8)unicode; return 1;}
    return 0;
}
static const Table *find_prepared(u16 page) {
    if(page==DOS_CP_HARDWARE) return &hardware;
    for(unsigned i=0;i<DOS_CP_PREPARED_MAX;i++) if(prepared[i].memory && prepared[i].page==page) return &prepared[i];
    return NULL;
}
int codepage_check(u16 page) {return find_prepared(page)?0:DOS_CP_SYSTEM_NOT_PREPARED;}
int codepage_select(u16 page) {
    const Table *t=find_prepared(page);
    if(!t) return DOS_CP_NOT_PREPARED;
    if(t!=current_table()) {selected=t; console_codepage_changed();}
    return keyb_code_page(page)?DOS_CP_NOT_IN_FILE:0;
}
int codepage_find(const u8 *data,u32 size,u16 page,u32 *offset,u32 *length,unsigned *kind) {
    if(!data || size<12 || memcmp(data,DOS_CP_FILE_MAGIC,8) || rd16(data+8)!=1) return DOS_CP_BAD_FILE;
    u32 count=rd16(data+10); int found=0;
    if(!count || count>(size-12)/12) return DOS_CP_BAD_FILE;
    for(u32 i=0;i<count;i++) {
        const u8 *entry=data+12+i*12; u32 at=rd32(entry+4),bytes=rd32(entry+8);
        u16 id=rd16(entry); unsigned type=entry[2];
        if(!id || id==0xffff || entry[3] || (type!=1 && type!=2) || at<12+count*12 || at>size || bytes>size-at) return DOS_CP_BAD_FILE;
        if(type==1?bytes!=512:(bytes<16+512+512 || (bytes-16-512)%512)) return DOS_CP_BAD_FILE;
        for(u32 j=0;j<i;j++) if(rd16(data+12+j*12)==id) return DOS_CP_BAD_FILE;
        if(id==page) {*offset=at; *length=bytes; *kind=type; found=1;}
    }
    return found?0:DOS_CP_NOT_IN_FILE;
}
/* Copy, then validate the private copy so later caller writes cannot race. */
static int load(const u8 *data,u32 size,u16 page,Table *out) {
    u32 at,bytes; unsigned kind; int e=codepage_find(data,size,page,&at,&bytes,&kind); if(e) return e;
    Table t={.page=page,.dbcs=kind==2};
    u32 header=t.dbcs?16:0,table_bytes=bytes-header;
    e=allocate(table_bytes,&t.memory); if(e) return e;
    memcpy(t.memory,data+at+header,table_bytes);
    t.single=t.memory; t.pairs=t.single+256;
    int bad=0;
    if(t.dbcs) {
        memcpy(t.ranges,data+at,16);
        unsigned i=0,last=127;
        for(;i<16 && (t.ranges[i] || t.ranges[i+1]);i+=2) {
            if(t.ranges[i]<=last || t.ranges[i]>t.ranges[i+1]) bad=1;
            for(unsigned c=t.ranges[i];c<=t.ranges[i+1] && !bad;c++) t.slot[c]=++t.leads;
            last=t.ranges[i+1];
        }
        if(i==16 || !t.leads || table_bytes!=512U+t.leads*512U) bad=1;
    }
    for(unsigned c=0;c<256 && !bad;c++) {
        if((c<32 && t.single[c]!=c) || (t.slot[c] && t.single[c]) || (t.single[c]>=0xd800 && t.single[c]<0xe000)) bad=1;
    }
    for(u32 i=0;i<(u32)t.leads*256 && !bad;i++) if(t.pairs[i]>=0xd800 && t.pairs[i]<0xe000) bad=1;
    if(bad) {release(t.memory); return DOS_CP_BAD_FILE;}
    *out=t; return 0;
}
static int prepare_start(const u8 *p,u32 size) {
    if(!p || size<6) return DE_FUNCTION;
    u32 count=rd16(p+4);
    if(rd16(p) || rd16(p+2)!=2+2*count || size<6+2*count) return DE_FUNCTION;
    end_prepare();
    if(count>DOS_CP_PREPARED_MAX) return DOS_CP_DEVICE_ERROR;
    if(!count) {preparing=2; return 0;}
    u16 slots[DOS_CP_PREPARED_MAX]; unsigned wanted=0;
    for(unsigned i=0;i<DOS_CP_PREPARED_MAX;i++) slots[i]=prepared[i].memory?prepared[i].page:0;
    for(unsigned i=0;i<count;i++) {
        u16 page=rd16(p+6+2*i); requested[i]=page;
        if(page==0xffff) continue;
        if(!page) return DOS_CP_DEVICE_ERROR;
        slots[i]=page; wanted++;
    }
    if(!wanted) return DOS_CP_DEVICE_ERROR;
    for(unsigned i=0;i<DOS_CP_PREPARED_MAX;i++) for(unsigned j=i+1;j<DOS_CP_PREPARED_MAX;j++)
        if(slots[i] && slots[i]==slots[j]) return DOS_CP_DEVICE_ERROR;
    int e=allocate(STAGING_LIMIT,(void **)&staging); if(e) {staging=NULL; return e;}
    requested_count=count; preparing=1; return 0;
}
static int prepare_write(const void *data,u32 count,u32 *done) {
    if(preparing!=1) return DOS_CP_BAD_FILE;
    if(prepare_error) return prepare_error;
    if(count>STAGING_LIMIT-staged) {prepare_error=DOS_CP_BAD_FILE; return prepare_error;}
    memcpy(staging+staged,data,count); staged+=count; *done=count; return 0;
}
static int prepare_end(void) {
    if(!preparing) return DOS_CP_BAD_FILE;
    if(preparing==2) {end_prepare(); return 0;} /* REFRESH: Unicode needs no font reload. */
    int e=prepare_error; Table built[DOS_CP_PREPARED_MAX]; unsigned loaded=0;
    for(;!e && loaded<requested_count;loaded++) {
        memset(&built[loaded],0,sizeof(built[loaded]));
        if(requested[loaded]!=0xffff) e=load(staging,staged,requested[loaded],&built[loaded]);
    }
    if(e) {
        for(unsigned i=0;i<loaded;i++) release(built[i].memory);
        end_prepare(); return e;
    }
    u16 active=current_table()->page; int replaced=0;
    for(unsigned i=0;i<requested_count;i++) if(requested[i]!=0xffff) {
        if(selected==&prepared[i]) replaced=1;
        release(prepared[i].memory); prepared[i]=built[i];
    }
    end_prepare();
    if(replaced) {
        selected=find_prepared(active); if(!selected) selected=&hardware;
        console_codepage_changed();
    }
    return 0;
}
static int store(DosDeviceRequest *r,const u16 *words,unsigned count) {
    if(!r->buffer || r->count<count*2U) return DE_FUNCTION;
    for(unsigned i=0;i<count;i++) wr16((u8 *)r->buffer+i*2,words[i]);
    r->transferred=count*2; return 0;
}
int codepage_request(DosDeviceRequest *r) {
    if(r->command==DOS_DEV_IOCTL_WRITE) {
        if(!(r->mode&3)) return DE_ACCESS;
        return prepare_write(r->buffer,r->count,&r->transferred);
    }
    if(r->command!=DOS_DEV_GENERIC_IOCTL || ((r->control>>8)!=DOS_CP_CATEGORY && (r->control>>8))) return DE_FUNCTION;
    unsigned function=r->control&255; const u8 *packet=r->buffer;
    if((function==DOS_CP_SELECT || function==DOS_CP_PREPARE_START || function==DOS_CP_PREPARE_END) && !(r->mode&3)) return DE_ACCESS;
    switch(function) {
    case DOS_CP_PREPARE_START: return prepare_start(packet,r->count);
    case DOS_CP_PREPARE_END: return prepare_end();
    case DOS_CP_SELECT: {
        if(!packet || r->count<4 || rd16(packet)<2) return DE_FUNCTION;
        return codepage_select(rd16(packet+2));
    }
    case DOS_CP_QUERY: {
        u16 words[2]={2,codepage_selected()}; return store(r,words,2);
    }
    case DOS_CP_QUERY_LIST: {
        u16 words[4+DOS_CP_PREPARED_MAX]={2*(3+DOS_CP_PREPARED_MAX),1,DOS_CP_HARDWARE,DOS_CP_PREPARED_MAX};
        for(unsigned i=0;i<DOS_CP_PREPARED_MAX;i++) words[4+i]=prepared[i].memory?prepared[i].page:0xffff;
        return store(r,words,ARRAY_SIZE(words));
    }
    default: return DE_FUNCTION;
    }
}
