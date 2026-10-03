/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos_client.h"
#include "calendar.h"
static DosApi *api;
#define HAS(member) (api && api->size>=offsetof(DosApi,member)+sizeof(api->member) && api->member)
int dos_client_bind(DosApi *p) {
    if(!p || p->version!=DOS_ABI_VERSION || p->size<DOS_API_V1_SIZE || !p->int21 || !p->command_tail) return DE_FORMAT;
    api=p; return 0;
}
int dos_call(DosRegs *r) {if(!api) return DE_FUNCTION; api->int21(r); return r->flags&1?(int)r->ax:0;}
static int path_call(unsigned fn,const char *s) {
    DosRegs r={.ax=fn<<8,.dx=(uintptr_t)s}; return dos_call(&r);
}
int dos_open(const char *s,unsigned mode,unsigned action,unsigned *h) {
    if(action>2) return DE_FUNCTION;
    if(action || mode>255) {
        unsigned result; return dos_open_ex(s,mode,0,action==0?1:action==1?0x12:0x10,h,&result);
    }
    DosRegs r={.ax=0x3d00|mode,.dx=(uintptr_t)s};
    int e=dos_call(&r); if(!e) *h=r.ax; return e;
}
int dos_open_ex(const char *s,unsigned mode,u8 attr,unsigned action,unsigned *h,unsigned *result) {
    DosRegs r={.ax=0x6c00,.si=(uintptr_t)s,.bx=mode,.cx=attr,.dx=action};
    int e=dos_call(&r); if(!e) {*h=r.ax; *result=r.cx;} return e;
}
int dos_file_time(unsigned h,int set,u16 *date,u16 *time) {
    if(set<0 || set>1) return DE_FUNCTION;
    DosRegs r={.ax=0x5700|set,.bx=h,.cx=set?*time:0,.dx=set?*date:0};
    int e=dos_call(&r); if(!e) {*date=r.dx; *time=r.cx;} return e;
}
int dos_lock(unsigned h,int unlock,u32 start,u32 length) {
    if(unlock<0 || unlock>1) return DE_FUNCTION;
    DosRegs r={.ax=0x5c00|unlock,.bx=h,.dx=start,.cx=length}; return dos_call(&r);
}
int dos_canonical(const char *s,char out[DOS_PATH_MAX]) {
    DosRegs r={.ax=0x6000,.si=(uintptr_t)s,.di=(uintptr_t)out}; return dos_call(&r);
}
static u32 transferred(const DosRegs *r,int e) {
    return !e?r->ax:HAS(capabilities) && (api->capabilities&DOS_CAP_PARTIAL_IO)?r->cx:0;
}
int dos_close(unsigned h) {DosRegs r={.ax=0x3e00,.bx=h}; return dos_call(&r);}
int dos_read(unsigned h,void *p,u32 n,u32 *done) {
    DosRegs r={.ax=0x3f00,.bx=h,.cx=n,.dx=(uintptr_t)p};
    int e=dos_call(&r); *done=transferred(&r,e); return e;
}
int dos_write(unsigned h,const void *p,u32 n,u32 *done) {
    DosRegs r={.ax=0x4000,.bx=h,.cx=n,.dx=(uintptr_t)p};
    int e=dos_call(&r); *done=transferred(&r,e); return e;
}
int dos_seek(unsigned h,i64 offset,unsigned origin,u32 *pos) {
    DosRegs r={.ax=0x4200|origin,.bx=h,.dx=(u64)offset};
    int e=dos_call(&r); if(!e) *pos=r.ax; return e;
}
int dos_dup(unsigned h,unsigned *out) {
    DosRegs r={.ax=0x4500,.bx=h}; int e=dos_call(&r); if(!e) *out=r.ax; return e;
}
int dos_dup2(unsigned h,unsigned dest) {DosRegs r={.ax=0x4600,.bx=h,.cx=dest}; return dos_call(&r);}
static int find_call(unsigned fn,const char *s,u8 attr,DosFind *find) {
    DosRegs r={.ax=0x2f00}; int e=dos_call(&r); if(e) return e;
    u64 saved=r.bx,capacity=r.cx;
    unsigned sized=HAS(capabilities) && (api->capabilities&DOS_CAP_FCB)?1:0;
    r=(DosRegs){.ax=0x1a00|sized,.dx=(uintptr_t)find,.cx=sizeof(*find)}; e=dos_call(&r); if(e) return e;
    r=(DosRegs){.ax=fn<<8,.dx=(uintptr_t)s,.cx=attr}; e=dos_call(&r);
    r=(DosRegs){.ax=0x1a00|sized,.dx=saved,.cx=capacity}; int restore=dos_call(&r); return e?e:restore;
}
int dos_find_first(const char *s,u8 attr,DosFind *find) {return find_call(0x4e,s,attr,find);}
int dos_find_next(DosFind *find) {return find_call(0x4f,NULL,0,find);}
int dos_set_dta(void *data,u32 capacity) {
    if(!HAS(capabilities) || !(api->capabilities&DOS_CAP_FCB)) return DE_FUNCTION;
    DosRegs r={.ax=0x1a01,.dx=(uintptr_t)data,.cx=capacity}; return dos_call(&r);
}
int dos_get_dta(void **data,u32 *capacity) {
    if(!data || !capacity || !HAS(capabilities) || !(api->capabilities&DOS_CAP_FCB)) return DE_FUNCTION;
    DosRegs r={.ax=0x2f00}; int e=dos_call(&r); if(!e) {*data=(void *)(uintptr_t)r.bx; *capacity=r.cx;} return e;
}
int dos_fcb_call(unsigned fn,void *fcb,u32 *records,unsigned *status) {
    if(!HAS(capabilities) || !(api->capabilities&DOS_CAP_FCB)) return DE_FUNCTION;
    if(!fcb || !status || !((fn>=0x0f && fn<=0x17) || (fn>=0x21 && fn<=0x24) || fn==0x27 || fn==0x28)) return DE_FUNCTION;
    if((fn==0x27 || fn==0x28) && !records) return DE_FUNCTION;
    DosRegs r={.ax=fn<<8,.dx=(uintptr_t)fcb,.cx=records?*records:0}; int e=dos_call(&r);
    if(fn==0x27 || fn==0x28) *records=e?0:r.cx;
    *status=e?255:r.ax&255; return e?e:(int)r.bx;
}
int dos_fcb_parse(const char **text,DosFcb *fcb,unsigned flags,unsigned *status) {
    if(!text || !*text || !fcb || !status || flags>15 || !HAS(capabilities) || !(api->capabilities&DOS_CAP_FCB)) return DE_FUNCTION;
    DosRegs r={.ax=0x2900|flags,.si=(uintptr_t)*text,.di=(uintptr_t)fcb}; int e=dos_call(&r);
    if(!e) {*text=(const char *)(uintptr_t)r.si; *status=r.ax&255;}
    return e?e:(int)r.bx;
}
int dos_getcwd(char out[DOS_PATH_MAX]) {
    return dos_drive_cwd(dos_current_drive(),out);
}
int dos_drive_cwd(unsigned drive,char out[DOS_PATH_MAX]) {
    if(drive>=DOS_DRIVES) return DE_DRIVE;
    char part[DOS_PATH_MAX]; DosRegs r={.ax=0x4700,.dx=drive+1,.si=(uintptr_t)part};
    int e=dos_call(&r); if(e) return e;
    out[0]='\\'; out[1]=0; return strappend(out,DOS_PATH_MAX,part);
}
unsigned dos_current_drive(void) {DosRegs r={.ax=0x1900}; return dos_call(&r)?DOS_DRIVES:(unsigned)r.ax;}
int dos_select_drive(unsigned drive) {
    if(drive>=DOS_DRIVES) return DE_DRIVE;
    DosRegs r={.ax=0x0e00,.dx=drive}; return dos_call(&r);
}
int dos_drive_info(u32 drive,DosDriveInfo *info) {return HAS(drive_info)?api->drive_info(drive,info):DE_FUNCTION;}
int dos_chdir(const char *s) {return path_call(0x3b,s);}
int dos_mkdir(const char *s) {return path_call(0x39,s);}
int dos_remove(const char *s,int dir) {return path_call(dir?0x3a:0x41,s);}
int dos_rename(const char *a,const char *b) {
    DosRegs r={.ax=0x5600,.dx=(uintptr_t)a,.di=(uintptr_t)b}; return dos_call(&r);
}
int dos_attribute(const char *s,int set,u8 *attr) {
    DosRegs r={.ax=0x4300|(set?1:0),.dx=(uintptr_t)s,.cx=set?*attr:0};
    int e=dos_call(&r); if(!e) *attr=r.cx; return e;
}
int dos_exec(const char *s,const char *tail) {
    DosRegs r={.ax=0x4b00,.dx=(uintptr_t)s,.bx=(uintptr_t)tail}; return dos_call(&r);
}
int dos_flush(void) {DosRegs r={.ax=0x0d00}; return dos_call(&r);}
int dos_alloc(u32 paragraphs,void **out) {
    DosRegs r={.ax=0x4800,.bx=paragraphs}; int e=dos_call(&r); *out=e?NULL:(void *)(uintptr_t)r.ax; return e;
}
int dos_free(void *p) {DosRegs r={.ax=0x4900,.dx=(uintptr_t)p}; return dos_call(&r);}
int dos_query(DosInfo *p) {return HAS(query)?api->query(p):DE_FUNCTION;}
unsigned dos_get_errorlevel(void) {
    DosExitInfo status;
    if(HAS(last_exit) && !api->last_exit(&status)) return status.code;
    DosInfo info={0}; int e=dos_query(&info);
    return e==DE_FUNCTION || e==DE_BUSY?1:info.errorlevel;
}
void dos_set_errorlevel(unsigned n) {if(HAS(set_errorlevel)) api->set_errorlevel(n);}
void con_write(const void *p,size_t n) {u32 done; dos_write(1,p,n,&done);}
void con_puts(const char *s) {con_write(s,strlen(s));}
int con_getch(void) {DosRegs r={.ax=0x0700}; int e=dos_call(&r); return e?-e:(int)r.ax;}
void con_clear(void) {if(HAS(console_clear)) api->console_clear();}
int dos_datetime(unsigned t[7]) {
    if(!t) return DE_FUNCTION;
    DosDateTime now; int e=dos_get_datetime(&now); if(e) return e;
    t[0]=now.year; t[1]=now.month; t[2]=now.day; t[6]=now.weekday;
    t[3]=now.hour; t[4]=now.minute; t[5]=now.second; return 0;
}
int dos_get_datetime(DosDateTime *out) {
    if(!out) return DE_FUNCTION;
    if(HAS(datetime)) return api->datetime(out);
    /* Older kernels expose date/time separately. Discard a sample crossing
     * midnight instead of combining yesterday's date with today's time. */
    for(unsigned tries=0;tries<3;tries++) {
        DosRegs date={.ax=0x2a00},time={.ax=0x2c00},after={.ax=0x2a00};
        int e=dos_call(&date); if(e) return e;
        e=dos_call(&time); if(e) return e;
        e=dos_call(&after); if(e) return e;
        if(date.cx!=after.cx || date.dx!=after.dx) continue;
        *out=(DosDateTime){date.cx,(date.dx>>8)&255,date.dx&255,(time.cx>>8)&255,time.cx&255,
            (time.dx>>8)&255,time.dx&255,date.ax&255}; return 0;
    }
    return DE_BUSY;
}
int dos_set_date(unsigned year,unsigned month,unsigned day) {
    if(year<1980 || year>2099 || !calendar_date(year,month,day)) return DE_FUNCTION;
    DosRegs r={.ax=0x2b00,.cx=year,.dx=(month<<8)|day}; int e=dos_call(&r);
    return e?e:(r.ax&255)?DE_FUNCTION:0;
}
int dos_set_time(unsigned hour,unsigned minute,unsigned second,unsigned hundredth) {
    if(hundredth>99 || !calendar_time(hour,minute,second,0)) return DE_FUNCTION;
    DosRegs r={.ax=0x2d00,.cx=(hour<<8)|minute,.dx=(second<<8)|hundredth}; int e=dos_call(&r);
    return e?e:(r.ax&255)?DE_FUNCTION:0;
}
void dos_shutdown(void) {if(HAS(shutdown)) api->shutdown();}
static int nls_supported(void) {return HAS(capabilities) && (api->capabilities&DOS_CAP_NLS);}
int dos_country_info(u16 country,u16 page,DosCountryInfo *out) {
    if(!nls_supported() || !out) return DE_FUNCTION;
    DosRegs r={.ax=0x6501,.bx=page,.dx=country,.cx=sizeof(*out),.di=(uintptr_t)out}; return dos_call(&r);
}
int dos_country_set(u16 country) {
    if(!nls_supported() || !country || country==DOS_NLS_CURRENT) return DE_FUNCTION;
    DosRegs r={.ax=0x38ff,.bx=country,.dx=65535}; return dos_call(&r);
}
int dos_code_page(u16 *active,u16 *boot) {
    if(!nls_supported() || !active || !boot) return DE_FUNCTION;
    DosRegs r={.ax=0x6601}; int e=dos_call(&r); if(!e) {*active=r.bx; *boot=r.dx;} return e;
}
int dos_code_page_set(u16 page) {
    if(!nls_supported() || !page || page==DOS_NLS_CURRENT) return DE_FUNCTION;
    DosRegs r={.ax=0x6602,.bx=page}; return dos_call(&r);
}
int dos_nls_table(u16 country,u16 page,unsigned kind,void *out,u32 capacity,u32 *size) {
    if(!nls_supported() || !size || (kind!=2 && kind!=4 && kind!=5 && kind!=6 && kind!=7)) return DE_FUNCTION;
    DosRegs r={.ax=0x6500|kind,.bx=page,.dx=country,.cx=capacity,.di=(uintptr_t)out};
    int e=dos_call(&r); if(!e || e==DE_FUNCTION) *size=r.cx; return e;
}
int dos_nls_case(void *data,u32 count,int filename) {
    if(!nls_supported() || (filename!=0 && filename!=1)) return DE_FUNCTION;
    DosRegs r={.ax=filename?0x65a1:0x6521,.dx=(uintptr_t)data,.cx=count}; return dos_call(&r);
}
int dos_env_get(const char *name,char *out,u32 cap) {return HAS(env_get)?api->env_get(name,out,cap):DE_FUNCTION;}
int dos_env_set(const char *name,const char *value) {return HAS(env_set)?api->env_set(name,value):DE_FUNCTION;}
int dos_env_list(u32 index,char *out,u32 cap) {return HAS(env_list)?api->env_list(index,out,cap):DE_FUNCTION;}
int dos_set_handle_count(unsigned n) {DosRegs r={.ax=0x6700,.bx=n}; return dos_call(&r);}
int dos_break_handler(const DosBreakHandler *handler,DosBreakHandler *previous) {
    return HAS(break_handler)?api->break_handler(handler,previous):DE_FUNCTION;
}
int dos_last_exit(DosExitInfo *info) {return HAS(last_exit)?api->last_exit(info):DE_FUNCTION;}
int dos_critical_handler(const DosCriticalHandler *handler,DosCriticalHandler *previous) {
    return HAS(critical_handler)?api->critical_handler(handler,previous):DE_FUNCTION;
}
int dos_extended_error(DosExtendedError *info) {return HAS(extended_error)?api->extended_error(info):DE_FUNCTION;}
const IoServices *dos_io_services(void) {return HAS(io)?api->io:NULL;}
int dos_redirect(u32 *drive,const DosRedirector *r) {return HAS(redirect)?api->redirect(drive,r):DE_FUNCTION;}
int dos_unredirect(u32 drive) {return HAS(unredirect)?api->unredirect(drive):DE_FUNCTION;}
int dos_full_path(const char *name,char out[DOS_PATH_MAX]) {return HAS(full_path)?api->full_path(name,out):dos_canonical(name,out);}
int dos_drive_map(u32 drive,const DosDriveMap *set,DosDriveMap *previous) {return HAS(drive_map)?api->drive_map(drive,set,previous):DE_FUNCTION;}
int dos_assign(const u8 *table,u8 *previous) {return HAS(assign)?api->assign(table,previous):DE_FUNCTION;}
int dos_append(const DosAppend *set,DosAppend *previous) {return HAS(append)?api->append(set,previous):DE_FUNCTION;}
int dos_append_task(u32 mask,u32 *previous) {return HAS(append_task)?api->append_task(mask,previous):DE_FUNCTION;}
int dos_installed(u32 program,const u32 *set,u32 *previous) {return HAS(installed)?api->installed(program,set,previous):DE_FUNCTION;}
int dos_device_register(const DosDeviceSpec *spec) {return HAS(device_register)?api->device_register(spec):DE_FUNCTION;}
int dos_device_info(u32 index,DosDeviceInfo *info) {return HAS(device_info)?api->device_info(index,info):DE_FUNCTION;}
int dos_block_register(const DosBlockSpec *spec) {return HAS(block_register)?api->block_register(spec):DE_FUNCTION;}
int dos_block_info(u32 index,DosBlockInfo *info) {return HAS(block_info)?api->block_info(index,info):DE_FUNCTION;}
int dos_break_check(int set,int *enabled) {
    if(set!=0 && set!=1) return DE_FUNCTION;
    DosRegs r={.ax=0x3300|(set?1:0),.dx=set?(u64)*enabled:0};
    int e=dos_call(&r); if(!e && !set) *enabled=r.dx; return e;
}
int dos_line_input(u8 *buffer) {DosRegs r={.ax=0x0a00,.dx=(uintptr_t)buffer}; return dos_call(&r);}
static int disk_io(void) {return HAS(capabilities) && (api->capabilities&DOS_CAP_DISK_IO);}
int dos_disk_read(u32 drive,u64 sector,u32 count,void *buffer,u32 *done) {
    if(done) *done=0;
    return disk_io() && HAS(disk_read)?api->disk_read(drive,sector,count,buffer,done):DE_FUNCTION;
}
int dos_disk_write(u32 drive,u64 sector,u32 count,const void *buffer,u32 *done) {
    if(done) *done=0;
    return disk_io() && HAS(disk_write)?api->disk_write(drive,sector,count,buffer,done):DE_FUNCTION;
}
int dos_volume_lock(u32 drive,int lock) {return disk_io() && HAS(volume_lock)?api->volume_lock(drive,lock):DE_FUNCTION;}
static int physical(void) {return HAS(capabilities) && (api->capabilities&DOS_CAP_PHYSICAL);}
int dos_physical_info(u32 index,DosPhysicalInfo *info) {
    return physical() && HAS(physical_info)?api->physical_info(index,info):DE_FUNCTION;
}
int dos_physical_read(u32 index,u64 sector,u32 count,void *buffer,u32 *done) {
    if(done) *done=0;
    return physical() && HAS(physical_read)?api->physical_read(index,sector,count,buffer,done):DE_FUNCTION;
}
int dos_physical_write(u32 index,u64 sector,u32 count,const void *buffer,u32 *done) {
    if(done) *done=0;
    return physical() && HAS(physical_write)?api->physical_write(index,sector,count,buffer,done):DE_FUNCTION;
}
void dos_restart(void) {if(HAS(restart)) api->restart();}
static int generic_block(unsigned drive,unsigned function,void *data) {
    if(drive>=DOS_DRIVES || !disk_io()) return DE_FUNCTION;
    DosRegs r={.ax=0x440d,.bx=drive+1,.cx=0x0800|function,.dx=(uintptr_t)data}; return dos_call(&r);
}
int dos_device_params(unsigned drive,DosDeviceParams *params) {return generic_block(drive,0x60,params);}
int dos_set_device_params(unsigned drive,const DosDeviceParams *params) {return generic_block(drive,0x40,(void *)params);}
int dos_sector_io(unsigned drive,unsigned function,DosSectorIo *io) {
    if(function!=0x41 && function!=0x42 && function!=0x61 && function!=0x62) return DE_FUNCTION;
    return generic_block(drive,function,io);
}
int dos_media_id(unsigned drive,DosMediaId *id,int set) {
    if(set!=0 && set!=1) return DE_FUNCTION;
    return generic_block(drive,set?0x46:0x66,id);
}
int dos_get_dpb(unsigned drive,const DosDpb **out) {
    if(!out || drive>=DOS_DRIVES) return DE_FUNCTION;
    DosRegs r={.ax=0x3200,.dx=drive+1}; int e=dos_call(&r); if(e) return e;
    if((r.ax&255)==255) return DE_DRIVE;
    *out=(const DosDpb *)(uintptr_t)r.bx; return 0;
}
int dos_verify(int set,int *enabled) {
    if(set!=0 && set!=1) return DE_FUNCTION;
    if(set) {DosRegs r={.ax=0x2e00|(*enabled?1:0)}; return dos_call(&r);}
    DosRegs r={.ax=0x5400}; int e=dos_call(&r); if(!e) *enabled=r.ax&1; return e;
}
int dos_temp_file(char *directory,unsigned attr,unsigned *handle) {
    DosRegs r={.ax=0x5a00,.cx=attr,.dx=(uintptr_t)directory}; int e=dos_call(&r); if(!e) *handle=r.ax; return e;
}
