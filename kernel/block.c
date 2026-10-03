/* SPDX-License-Identifier: GPL-2.0-or-later
 * Resident block drivers and generation-bound Disk callbacks for DOS FAT.
 */
#include "dos.h"
#include "device.h"
#include "block.h"
typedef struct {DosBlockSpec spec; unsigned owner,units; int initialized,active;} Provider;
typedef struct {Provider *provider; u32 id,unit,drive; DosBlockMedia media;} Unit;
static Provider providers[DOS_MAX_DRIVERS];
static Unit units[DOS_DRIVES];
static u32 next_unit=1;
_Static_assert(sizeof(uintptr_t)==8,"native block snapshot token");
void block_reset(void) {memset(providers,0,sizeof(providers)); memset(units,0,sizeof(units)); next_unit=1;}
static Unit *unit_for(u32 id) {
    for(unsigned i=0;i<ARRAY_SIZE(units);i++) if(id && units[i].id==id) return &units[i];
    return NULL;
}
int block_register(unsigned owner,const DosBlockSpec *spec) {
    if(!owner) return DE_ACCESS;
    if(!spec || spec->version!=DOS_BLOCK_ABI || spec->size<sizeof(*spec) || !spec->request) return DE_FORMAT;
    char name[9]; unsigned n=device_name(spec->name,name);
    if(!n) return DE_PATH;
    Provider *free=NULL;
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) {
        Provider *p=&providers[i];
        if(p->owner && !strcmp(name,p->spec.name)) return DE_EXISTS;
        if(!p->owner && !free) free=p;
    }
    if(!free) return DE_NOMEM;
    *free=(Provider){.spec=*spec,.owner=owner}; memcpy(free->spec.name,name,n+1); return 0;
}
static int transfer(void *context,u32 command,u32 sector,void *buffer) {
    u64 token=(uintptr_t)context; Unit *u=unit_for((u32)token);
    if(!u || !u->provider->initialized) return DE_DRIVE;
    u32 generation=token>>32;
    if(!generation || generation!=u->media.generation) return DE_CHANGED;
    if(!(u->media.flags&IO_DISK_PRESENT)) return DE_NOTREADY;
    if(command==DOS_BLOCK_WRITE && u->media.readonly) return DE_READONLY;
    u32 count=command==DOS_BLOCK_FLUSH?0:1;
    if(count && (!buffer || sector>=u->media.sectors)) return DE_SEEK;
    DosBlockRequest r={.size=sizeof(r),.command=command,.pid=dos_pid(),.unit=u->unit,
        .generation=generation,.count=count,.sector=sector,.buffer=buffer};
    int e=dos_block_request(&u->provider->spec,&r);
    if(r.transferred>count || (!e && r.transferred!=count)) return DE_IO;
    return e;
}
static int read_sector(void *ctx,u32 sector,void *buffer) {return transfer(ctx,DOS_BLOCK_READ,sector,buffer);}
static int write_sector(void *ctx,u32 sector,const void *buffer) {return transfer(ctx,DOS_BLOCK_WRITE,sector,(void *)buffer);}
static int flush(void *ctx) {return transfer(ctx,DOS_BLOCK_FLUSH,0,NULL);}
int block_ioctl_transfer(u32 id,int write,void *buffer,u32 count,u32 *done) {
    *done=0; Unit *u=unit_for(id); if(!u || !u->provider->initialized) return DE_DRIVE;
    if(count && !buffer) return DE_FUNCTION;
    DosBlockRequest r={.size=sizeof(r),.command=write?DOS_BLOCK_IOCTL_WRITE:DOS_BLOCK_IOCTL_READ,.pid=dos_pid(),
        .unit=u->unit,.generation=u->media.generation,.count=count,.buffer=buffer};
    int e=dos_block_request(&u->provider->spec,&r);
    if(r.transferred>count) return DE_IO;
    *done=r.transferred; return e;
}
int block_media(u32 id,IoDiskInfo *out) {
    Unit *u=unit_for(id); if(!u || !u->provider->initialized) return DE_DRIVE;
    if(!out) return DE_FUNCTION;
    DosBlockMedia media={0};
    DosBlockRequest r={.size=sizeof(r),.command=DOS_BLOCK_MEDIA,.pid=dos_pid(),.unit=u->unit,
        .count=sizeof(media),.buffer=&media};
    int e=dos_block_request(&u->provider->spec,&r); if(e) return e;
    if(r.transferred!=sizeof(media) || media.size!=sizeof(media) || !media.generation ||
       (media.flags&~(IO_DISK_REMOVABLE|IO_DISK_PRESENT)) || media.sector_bytes!=512 ||
       media.readonly>1 || media.sectors>UINT32_MAX || ((media.flags&IO_DISK_PRESENT) && !media.sectors)) return DE_FORMAT;
    if(u->media.generation) {
        if(media.generation<u->media.generation) return DE_CHANGED;
        if((media.flags^u->media.flags)&IO_DISK_REMOVABLE) return DE_FORMAT;
        if(media.generation==u->media.generation && (media.flags!=u->media.flags ||
           media.sectors!=u->media.sectors || media.readonly!=u->media.readonly)) return DE_IO;
    }
    u->media=media;
    *out=(IoDiskInfo){.generation=media.generation,.flags=media.flags,
        .disk={(void *)(uintptr_t)(((u64)media.generation<<32)|u->id),read_sector,write_sector,flush,media.sectors,media.readonly}};
    return 0;
}
int block_initialize(unsigned owner,const IoServices *io,const char *tail,unsigned *count) {
    *count=0;
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) {
        Provider *p=&providers[i]; if(p->owner!=owner) continue;
        (*count)++; p->initialized=1;
        DosBlockRequest r={.size=sizeof(r),.command=DOS_BLOCK_INIT,.pid=dos_pid(),.io=io,.arguments=tail};
        int e=dos_block_request(&p->spec,&r); if(e) return e;
        if(!r.units || r.units>DOS_DRIVES) return DE_FORMAT;
        p->units=r.units;
        for(unsigned unit=0;unit<r.units;unit++) {
            unsigned slot=0; while(slot<ARRAY_SIZE(units) && units[slot].id) slot++;
            if(slot==ARRAY_SIZE(units) || !next_unit) return DE_NOMEM;
            units[slot]=(Unit){.provider=p,.id=next_unit++,.unit=unit,.drive=UINT32_MAX};
        }
    }
    return 0;
}
unsigned block_units(unsigned owner,u32 ids[DOS_DRIVES]) {
    unsigned count=0;
    for(unsigned i=0;i<ARRAY_SIZE(units);i++) if(units[i].id && units[i].provider->owner==owner) ids[count++]=units[i].id;
    return count;
}
void block_drive(u32 id,unsigned drive) {Unit *u=unit_for(id); if(u) u->drive=drive;}
void block_publish(unsigned owner) {
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) if(providers[i].owner==owner) providers[i].active=1;
}
int block_info(u32 index,DosBlockInfo *out) {
    if(!out) return DE_FUNCTION;
    u32 ordinal=0;
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) {
        Provider *p=&providers[i]; if(!p->active || ordinal++!=index) continue;
        *out=(DosBlockInfo){.size=sizeof(*out),.index=index,.units=p->units};
        memcpy(out->name,p->spec.name,sizeof(out->name));
        for(unsigned j=0;j<ARRAY_SIZE(units);j++) if(units[j].provider==p && units[j].drive<DOS_DRIVES) out->drive_mask|=1U<<units[j].drive;
        return 0;
    }
    return DE_NOMORE;
}
int block_pending(unsigned owner) {
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) if(providers[i].owner==owner) return 1;
    return 0;
}
void block_cancel(unsigned owner) {
    if(!owner) return;
    for(unsigned i=0;i<ARRAY_SIZE(units);i++) if(units[i].id && units[i].provider->owner==owner) memset(&units[i],0,sizeof(units[i]));
    for(unsigned i=0;i<ARRAY_SIZE(providers);i++) if(providers[i].owner==owner) memset(&providers[i],0,sizeof(providers[i]));
}
int block_finish(unsigned owner) {
    int error=0;
    for(unsigned i=ARRAY_SIZE(providers);i;i--) {
        Provider *p=&providers[i-1]; if(p->owner!=owner || !p->initialized) continue;
        DosBlockRequest r={.size=sizeof(r),.command=DOS_BLOCK_FINISH,.pid=dos_pid()};
        int e=dos_block_request(&p->spec,&r); if(e && !error) error=dos_preserve_error(e);
        p->initialized=p->active=0;
    }
    block_cancel(owner); return error;
}
