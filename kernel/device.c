/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native character driver registry and DOS request dispatch.
 */
#include "dos.h"
#include "device.h"
#include "nls.h"
#include "console.h"
#include "block.h"
#include "codepage.h"
typedef struct {DosDeviceSpec spec; unsigned owner,refs; int active,initialized;} Device;
static Device devices[DOS_MAX_DEVICES];
static unsigned loading,configured; /* configured: the image loading changed the console (ANSI.SYS) */
static int loading_error;
static int unavailable(void *context,DosDeviceRequest *r) {
    (void)context;
    switch(r->command) {
    case DOS_DEV_CLOSE: case DOS_DEV_FINISH: case DOS_DEV_OUTPUT_FLUSH: return 0;
    case DOS_DEV_INPUT_STATUS: case DOS_DEV_OUTPUT_STATUS: r->ready=0; return 0;
    default: return DE_NOTREADY;
    }
}
static int builtin(void *context,DosDeviceRequest *r) {
    int console=(uintptr_t)context==DOS_CON_DEVICE;
    switch(r->command) {
    case DOS_DEV_INIT: case DOS_DEV_OPEN: case DOS_DEV_CLOSE: case DOS_DEV_FINISH:
    case DOS_DEV_OUTPUT_FLUSH: return 0;
    case DOS_DEV_INPUT_FLUSH: return console?console_flush():0;
    case DOS_DEV_OUTPUT_STATUS: r->ready=1; return 0;
    case DOS_DEV_INPUT_STATUS: {
        if(!console) {r->ready=0; return 0;}
        u8 c; int e=console_byte(0,1,&c);
        r->ready=!e; return e==DE_NOTREADY || e==DE_EOF?0:e;
    }
    case DOS_DEV_PEEK:
        if(!console) return DE_EOF;
        if(!r->count || !r->buffer) return DE_FUNCTION;
        return console_byte(!!(r->flags&DOS_DEVICE_WAIT),1,r->buffer);
    case DOS_DEV_READ:
        if(console) while(r->transferred<r->count) {
            int e=console_byte(!!(r->flags&DOS_DEVICE_WAIT),0,(u8 *)r->buffer+r->transferred);
            if(e) return e;
            r->transferred++;
        }
        return 0;
    case DOS_DEV_WRITE:
        if(console) console_write(r->buffer,r->count);
        r->transferred=r->count; return 0;
    case DOS_DEV_IOCTL_WRITE: case DOS_DEV_GENERIC_IOCTL:
        if(!console) return DE_FUNCTION;
        /* ANSI.SYS's display information, category 3. */
        if(r->command==DOS_DEV_GENERIC_IOCTL && r->control>>8==3 && ((r->control&255)==0x7f || (r->control&255)==0x5f))
            return ansi_request(r->control&255,r->buffer,r->count,&r->transferred);
        return codepage_request(r);
    default: return DE_FUNCTION;
    }
}
void device_reset(void) {
    memset(devices,0,sizeof(devices)); loading=configured=0; loading_error=0;
    block_reset();
    for(unsigned i=0;i<2;i++) {
        Device *d=&devices[i]; d->active=d->initialized=1;
        d->spec=(DosDeviceSpec){.version=DOS_DEVICE_ABI,.size=sizeof(DosDeviceSpec),
            .attributes=DOS_DEVICE_CHAR|(i?4:(3|DOS_DEVICE_IOCTL|DOS_DEVICE_GENERIC)),.capabilities=DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE,
            .context=(void *)(uintptr_t)(i+1),.request=builtin};
        strcopy(d->spec.name,sizeof(d->spec.name),i?"NUL":"CON");
    }
    for(unsigned i=2;i<4;i++) {
        Device *d=&devices[i]; d->active=d->initialized=1;
        d->spec=(DosDeviceSpec){.version=DOS_DEVICE_ABI,.size=sizeof(DosDeviceSpec),
            .attributes=DOS_DEVICE_CHAR|DOS_DEVICE_OPEN_CLOSE,
            .capabilities=DOS_DEVICE_CAN_WRITE|(i==2?DOS_DEVICE_CAN_READ:0),.request=unavailable};
        strcopy(d->spec.name,sizeof(d->spec.name),i==2?"AUX":"PRN");
    }
}
const DosDeviceSpec *device_spec(unsigned id) {
    return id && id<=ARRAY_SIZE(devices) && devices[id-1].active?&devices[id-1].spec:NULL;
}
int device_find(const char *path) {
    if(!path) return 0;
    const char *leaf=path;
    if(leaf[0] && !nls_lead((u8)leaf[0]) && leaf[1]==':') leaf+=2;
    for(const char *p=leaf;*p;) {
        unsigned width=nls_char_size(p); if(!width) return 0;
        if(width==1 && (*p=='/' || *p=='\\')) leaf=p+1;
        p+=width;
    }
    char name[9]; unsigned n=0;
    while(*leaf && *leaf!='.' && *leaf!=':') {
        if(nls_lead((u8)*leaf)) return 0; /* Resident device names are ASCII. */
        if(n==8) return 0;
        name[n++]=upper(*leaf++);
    }
    if(*leaf==':' && leaf[1]) return 0;
    name[n]=0;
    if(!strcmp(name,"AUX")) strcopy(name,sizeof(name),"COM1");
    if(!strcmp(name,"PRN")) strcopy(name,sizeof(name),"LPT1");
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++)
        if(devices[i].active && !strcmp(devices[i].spec.name,name)) return i+1;
    /* Reserved names cannot silently become disk files when hardware is absent. */
    if(n==4 && !memcmp(name,"COM",3) && name[3]>='1' && name[3]<='4') return DOS_AUX_DEVICE;
    if(n==4 && !memcmp(name,"LPT",3) && name[3]>='1' && name[3]<='3') return DOS_PRN_DEVICE;
    if(!strcmp(name,"COM1")) return DOS_AUX_DEVICE;
    if(!strcmp(name,"LPT1")) return DOS_PRN_DEVICE;
    return 0;
}
int device_info(unsigned index,DosDeviceInfo *info) {
    if(!info) return DE_FUNCTION;
    unsigned at=0;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(devices[i].active) {
        if(at++!=index) continue;
        Device *d=&devices[i];
        *info=(DosDeviceInfo){.size=sizeof(*info),.index=index,.attributes=d->spec.attributes,
            .capabilities=d->spec.capabilities,.open_descriptions=d->refs};
        memcpy(info->name,d->spec.name,sizeof(info->name)); return 0;
    }
    return DE_NOMORE;
}
int device_begin(unsigned owner) {
    if(loading) return DE_BUSY;
    if(!owner) return DE_FUNCTION;
    loading=owner; loading_error=0; return 0;
}
unsigned device_name(const char *raw,char name[9]) {
    unsigned n=0;
    while(n<9 && raw[n]) {
        unsigned c=(u8)raw[n];
        if(c<33 || c>126 || strchr(".\"/\\[]:;|=,+*?<>",c)) return 0;
        name[n]=upper(raw[n]); n++;
    }
    if(n==9) return 0;
    name[n]=0; return n;
}
static int register_spec(const DosDeviceSpec *spec) {
    if(!spec || spec->version!=DOS_DEVICE_ABI || spec->size<sizeof(*spec) || !spec->request ||
       !(spec->attributes&DOS_DEVICE_CHAR) ||
       (spec->attributes&~(DOS_DEVICE_CHAR|DOS_DEVICE_IOCTL|DOS_DEVICE_OPEN_CLOSE|DOS_DEVICE_GENERIC)) ||
       (spec->capabilities&~(DOS_DEVICE_CAN_READ|DOS_DEVICE_CAN_WRITE))) return DE_FORMAT;
    char name[9]; unsigned length=device_name(spec->name,name);
    if(!length) return DE_PATH;
    unsigned free_slot=DOS_MAX_DEVICES;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) {
        Device *d=&devices[i];
        if(d->spec.request && !strcmp(d->spec.name,name)) return DE_EXISTS;
        if(!d->spec.request && free_slot==DOS_MAX_DEVICES) free_slot=i;
    }
    if(free_slot==DOS_MAX_DEVICES) return DE_NOMEM;
    Device *d=&devices[free_slot]; d->spec=*spec; d->owner=loading;
    memcpy(d->spec.name,name,length+1); return 0;
}
int device_register(const DosDeviceSpec *spec) {
    if(!loading) return DE_ACCESS;
    int e=register_spec(spec); if(e && !loading_error) loading_error=e; return e;
}
int device_register_block(const DosBlockSpec *spec) {
    if(!loading) return DE_ACCESS;
    int e=block_register(loading,spec); if(e && !loading_error) loading_error=e; return e;
}
int device_load_error(unsigned owner) {return loading==owner?loading_error:0;}
unsigned device_loading(void) {return loading;}
/* An image loading that registers nothing but sets something up (ANSI.SYS)
 * loads all the same. */
int device_configure(void) {
    if(!loading) return DE_ACCESS;
    configured=loading; return 0;
}
int device_request(unsigned id,DosDeviceRequest *r) {
    const DosDeviceSpec *spec=device_spec(id); if(!spec) return DE_HANDLE;
    if(!r || r->size<sizeof(*r)) return DE_FUNCTION;
    u32 count=r->count; r->transferred=0; r->ready=0;
    if(count && !r->buffer && (r->command==DOS_DEV_READ || r->command==DOS_DEV_WRITE ||
       r->command==DOS_DEV_PEEK || r->command==DOS_DEV_IOCTL_READ || r->command==DOS_DEV_IOCTL_WRITE ||
       r->command==DOS_DEV_GENERIC_IOCTL)) return DE_FUNCTION;
    int e=dos_driver_request(spec,r);
    if(r->transferred>count) {r->transferred=0; return DE_IO;}
    return e;
}
void device_reference(unsigned id,int delta) {
    if(device_spec(id)) {
        if(delta>0) devices[id-1].refs++;
        else if(devices[id-1].refs) devices[id-1].refs--;
    }
}
void device_cancel(unsigned owner) {
    if(!owner) return;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(devices[i].owner==owner) memset(&devices[i],0,sizeof(devices[i]));
    if(loading==owner) loading=0;
    if(configured==owner) configured=0;
    block_cancel(owner);
}
int device_pending(unsigned owner) {
    if(!owner) return 0;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(devices[i].owner==owner) return 1;
    return block_pending(owner);
}
int device_finish(unsigned owner) {
    if(!owner) return DE_ACCESS;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(devices[i].owner==owner && devices[i].refs) return DE_BUSY;
    int error=dos_detach_block_drives(owner); if(error) return error;
    error=dos_preserve_error(block_finish(owner));
    for(unsigned i=ARRAY_SIZE(devices);i;i--) {
        Device *d=&devices[i-1]; if(d->owner!=owner || !d->initialized) continue;
        DosDeviceRequest r={.size=sizeof(r),.command=DOS_DEV_FINISH,.pid=dos_pid()};
        int e=dos_driver_request(&d->spec,&r); if(e && !error) error=dos_preserve_error(e);
        d->initialized=d->active=0;
    }
    device_cancel(owner); return error;
}
int device_commit(unsigned owner,const IoServices *io,const char *tail) {
    if(loading!=owner) return DE_ACCESS;
    loading=0;
    if(loading_error) {int e=loading_error; device_cancel(owner); return e;}
    unsigned count=0;
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) {
        Device *d=&devices[i]; if(d->owner!=owner) continue;
        count++; d->initialized=1; /* Failed INIT receives FINISH too. */
        DosDeviceRequest r={.size=sizeof(r),.command=DOS_DEV_INIT,.pid=dos_pid(),.io=io,.arguments=tail};
        int e=dos_driver_request(&d->spec,&r);
        if(e) {dos_preserve_error(e); device_finish(owner); return e;}
    }
    unsigned blocks=0; int e=block_initialize(owner,io,tail,&blocks);
    if(!e && count+blocks) e=dos_attach_block_drives(owner);
    if(e) {dos_preserve_error(e); device_finish(owner); return e;}
    if(!count && !blocks && configured!=owner) return DE_FORMAT;
    configured=0;
    block_publish(owner);
    for(unsigned i=0;i<ARRAY_SIZE(devices);i++) if(devices[i].owner==owner) devices[i].active=1;
    return 0;
}
