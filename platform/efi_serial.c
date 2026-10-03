/* SPDX-License-Identifier: GPL-2.0-or-later
 * IO.SYS firmware serial units: EFI Serial I/O devices that no console uses.
 * Units are discovered once. A device behind a console handle or a ConIn/
 * ConOut/ErrOut(Dev) variable instance, held by another driver, or lacking a
 * device path is never published: firmware often drives its console UART
 * through the same Serial I/O instance.
 */
#include "efi_serial.h"
#define MAX_UNITS 8
#define SETTABLE (EFI_SERIAL_DATA_TERMINAL_READY|EFI_SERIAL_REQUEST_TO_SEND|EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE|\
                  EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE|EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE)
typedef struct {
    SERIAL_IO_INTERFACE *io;
    u64 token;
    SERIAL_IO_MODE saved;
    u32 saved_control,timeout_us;
} Unit;
static Unit units[MAX_UNITS];
static SERIAL_IO_INTERFACE *console_serial;
static unsigned unit_count;
static EFI_BOOT_SERVICES *boot;
static u64 next_token=1;
static EFI_GUID serial_guid=EFI_SERIAL_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static int status_error(EFI_STATUS s) {
    if(s==EFI_INVALID_PARAMETER || s==EFI_UNSUPPORTED) return DE_FUNCTION;
    if(s==EFI_TIMEOUT) return DE_NOTREADY;
    return efi_dos_error(s);
}
static Unit *unit_for(u64 token) {
    for(unsigned i=0;i<unit_count;i++) if(token && units[i].token==token) return &units[i];
    return NULL;
}
/* Node-wise prefix test. A UART node's line settings may differ between the
 * device and a console variable written at another baud rate. */
static int prefix(const u8 *device,unsigned bytes,const u8 *path,unsigned length) {
    for(unsigned n=0;n<bytes;) {
        unsigned size=rd16(device+n+2);
        if(n+4>length || device[n]!=path[n] || device[n+1]!=path[n+1] || size!=rd16(path+n+2) || size>length-n) return 0;
        if(!(device[n]==MESSAGING_DEVICE_PATH && device[n+1]==MSG_UART_DP) && memcmp(device+n+4,path+n+4,size-4)) return 0;
        n+=size;
    }
    return 1;
}
static int under(EFI_HANDLE handle,const EFI_DEVICE_PATH *serial,unsigned bytes) {
    EFI_DEVICE_PATH *path=NULL;
    if(!handle || EFI_ERROR(boot->HandleProtocol(handle,&path_guid,(void **)&path)) || !path) return 0;
    return prefix((const u8 *)serial,bytes,(const u8 *)path,efi_path_bytes(path));
}
/* Console variables hold multi-instance device paths. Firmware may route a
 * console through a serial device whose handles carry no text protocol. */
/* unknown is the answer when the variable cannot be read or parsed. */
static int listed(EFI_RUNTIME_SERVICES *rt,const CHAR16 *name,const EFI_DEVICE_PATH *serial,unsigned bytes,int unknown) {
    EFI_GUID global=EFI_GLOBAL_VARIABLE; UINTN size=0; u8 *data=NULL;
    if(!rt || !rt->GetVariable) return 0;
    EFI_STATUS s=rt->GetVariable((CHAR16 *)name,&global,NULL,&size,NULL);
    if(s==EFI_NOT_FOUND) return 0;
    if(s!=EFI_BUFFER_TOO_SMALL || !size || size>65536) return unknown;
    if(EFI_ERROR(boot->AllocatePool(EfiLoaderData,size,(void **)&data)) || !data) return unknown;
    s=rt->GetVariable((CHAR16 *)name,&global,NULL,&size,data);
    int found=EFI_ERROR(s)?unknown:0;
    for(UINTN start=0,n=0;!EFI_ERROR(s) && !found;) {
        if(n+4>size || rd16(data+n+2)<4 || rd16(data+n+2)>size-n) {found=unknown; break;} /* Malformed. */
        unsigned length=rd16(data+n+2);
        if(data[n]==END_DEVICE_PATH_TYPE) {
            if(prefix((const u8 *)serial,bytes,data+start,(unsigned)(n-start))) found=1;
            if(data[n+1]==END_ENTIRE_DEVICE_PATH_SUBTYPE) break;
            start=n+length;
        }
        n+=length;
    }
    boot->FreePool(data); return found;
}
static int console_device(EFI_SYSTEM_TABLE *st,EFI_HANDLE handle) {
    if(handle==st->ConsoleInHandle || handle==st->ConsoleOutHandle || handle==st->StandardErrorHandle) return 1;
    if(boot->OpenProtocolInformation) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL; UINTN count=0; int held=0;
        if(!EFI_ERROR(boot->OpenProtocolInformation(handle,&serial_guid,&entries,&count))) {
            for(UINTN i=0;i<count;i++)
                if(entries[i].Attributes&(EFI_OPEN_PROTOCOL_BY_DRIVER|EFI_OPEN_PROTOCOL_EXCLUSIVE)) held=1;
            if(entries) boot->FreePool(entries);
        }
        if(held) return 1;
    }
    EFI_DEVICE_PATH *path=NULL;
    if(EFI_ERROR(boot->HandleProtocol(handle,&path_guid,(void **)&path)) || !path) return 1;
    unsigned bytes=efi_path_bytes(path); if(!bytes) return 1;
    if(under(st->ConsoleInHandle,path,bytes) || under(st->ConsoleOutHandle,path,bytes) ||
       under(st->StandardErrorHandle,path,bytes)) return 1;
    static const CHAR16 *const variables[]={(const CHAR16 *)u"ConIn",(const CHAR16 *)u"ConOut",(const CHAR16 *)u"ErrOut",
        (const CHAR16 *)u"ConInDev",(const CHAR16 *)u"ConOutDev",(const CHAR16 *)u"ErrOutDev"};
    for(unsigned i=0;i<ARRAY_SIZE(variables);i++) if(listed(st->RuntimeServices,variables[i],path,bytes,1)) return 1;
    EFI_GUID text[2]={EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID};
    for(unsigned kind=0;kind<2;kind++) {
        EFI_HANDLE *handles=NULL; UINTN count=0; int console=0;
        EFI_STATUS s=boot->LocateHandleBuffer(ByProtocol,&text[kind],NULL,&count,&handles);
        if(s==EFI_NOT_FOUND) continue;
        if(EFI_ERROR(s)) return 1; /* Unknown console topology: do not publish. */
        for(UINTN i=0;i<count;i++) if(handles[i]==handle || under(handles[i],path,bytes)) console=1;
        boot->FreePool(handles);
        if(console) return 1;
    }
    return 0;
}
/* Positive identification of a console output device: an output console
 * handle, or a device path under one or listed in ConOut/ErrOut(Dev). */
static int console_output(EFI_SYSTEM_TABLE *st,EFI_HANDLE handle) {
    if(handle==st->ConsoleOutHandle || handle==st->StandardErrorHandle) return 1;
    EFI_DEVICE_PATH *path=NULL;
    if(EFI_ERROR(boot->HandleProtocol(handle,&path_guid,(void **)&path)) || !path) return 0;
    unsigned bytes=efi_path_bytes(path); if(!bytes) return 0;
    if(under(st->ConsoleOutHandle,path,bytes) || under(st->StandardErrorHandle,path,bytes)) return 1;
    static const CHAR16 *const variables[]={(const CHAR16 *)u"ConOut",(const CHAR16 *)u"ErrOut",
        (const CHAR16 *)u"ConOutDev",(const CHAR16 *)u"ErrOutDev"};
    for(unsigned i=0;i<ARRAY_SIZE(variables);i++) if(listed(st->RuntimeServices,variables[i],path,bytes,0)) return 1;
    return 0;
}
int efi_serial_console_present(void) {return console_serial!=NULL;}
/* The firmware console sends only ASCII to its UART; this matches it. Bytes
 * are written with the device's own timeout and failures are dropped. */
static void console_put(const u8 *bytes,UINTN n) {
    if(n) {UINTN size=n; console_serial->Write(console_serial,&size,(void *)bytes);}
}
void efi_serial_console_write(const void *data,size_t length) {
    if(!console_serial) return;
    const u8 *p=data; u8 buf[128]; UINTN n=0;
    while(length--) {
        u8 c=*p++;
        if(c=='\n') buf[n++]='\r';
        if(c && c<0x80) buf[n++]=c;
        if(n>=126) {console_put(buf,n); n=0;}
    }
    console_put(buf,n);
}
void efi_serial_console_write_text(const u16 *text,size_t length) {
    if(!console_serial) return;
    u8 buf[128]; UINTN n=0;
    while(length--) {
        u16 c=*text++;
        if(c=='\n') buf[n++]='\r';
        if(c && c<0x80) buf[n++]=(u8)c;
        if(n>=126) {console_put(buf,n); n=0;}
    }
    console_put(buf,n);
}
static u32 status_bits(u32 control) {
    return (control&EFI_SERIAL_DATA_TERMINAL_READY?IO_SERIAL_DTR:0)|(control&EFI_SERIAL_REQUEST_TO_SEND?IO_SERIAL_RTS:0)|
        (control&(EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE|EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE)?IO_SERIAL_LOOPBACK:0)|
        (control&EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE?IO_SERIAL_HW_FLOW:0)|
        (control&EFI_SERIAL_CLEAR_TO_SEND?IO_SERIAL_CTS:0)|(control&EFI_SERIAL_DATA_SET_READY?IO_SERIAL_DSR:0)|
        (control&EFI_SERIAL_RING_INDICATE?IO_SERIAL_RING:0)|(control&EFI_SERIAL_CARRIER_DETECT?IO_SERIAL_CARRIER:0)|
        (control&EFI_SERIAL_INPUT_BUFFER_EMPTY?IO_SERIAL_INPUT_EMPTY:0)|(control&EFI_SERIAL_OUTPUT_BUFFER_EMPTY?IO_SERIAL_OUTPUT_EMPTY:0);
}
static u32 serial_count(void *context) {(void)context; return unit_count;}
static int serial_open(void *context,u32 index,u64 *token) {
    (void)context;
    if(!token) return DE_FUNCTION;
    *token=0;
    if(index>=unit_count) return DE_DRIVE;
    Unit *u=&units[index];
    if(u->token) return DE_BUSY;
    if(!next_token) return DE_NOMEM;
    UINT32 control=0; EFI_STATUS s=u->io->GetControl(u->io,&control);
    if(EFI_ERROR(s)) return status_error(s);
    u->saved=*u->io->Mode; u->saved_control=control&SETTABLE; u->timeout_us=u->saved.Timeout;
    u->token=next_token++; *token=u->token; return 0;
}
/* Line settings as a SERIAL_IO_MODE records them. */
static EFI_STATUS set_mode(SERIAL_IO_INTERFACE *io,const SERIAL_IO_MODE *m) {
    return io->SetAttributes(io,m->BaudRate,m->ReceiveFifoDepth,m->Timeout,
        (EFI_PARITY_TYPE)m->Parity,(UINT8)m->DataBits,(EFI_STOP_BITS_TYPE)m->StopBits);
}
static int restore(Unit *u) {
    EFI_STATUS a=set_mode(u->io,&u->saved);
    EFI_STATUS b=u->io->SetControl(u->io,u->saved_control);
    return EFI_ERROR(a)?status_error(a):EFI_ERROR(b)?status_error(b):0;
}
static int serial_close(void *context,u64 token) {
    (void)context; Unit *u=unit_for(token); if(!u) return DE_HANDLE;
    int e=restore(u); u->token=0; return e;
}
static int serial_config(void *context,u64 token,const IoSerialConfig *c) {
    (void)context; Unit *u=unit_for(token); if(!u) return DE_HANDLE;
    if(!c || c->size!=sizeof(*c) || c->baud<50 || c->baud>921600 || c->data_bits<5 || c->data_bits>8 ||
       c->parity>IO_PARITY_SPACE || (c->stop_bits!=IO_STOP_ONE && c->stop_bits!=IO_STOP_TWO && c->stop_bits!=IO_STOP_ONE_HALF) ||
       (c->control&~(IO_SERIAL_DTR|IO_SERIAL_RTS|IO_SERIAL_LOOPBACK|IO_SERIAL_HW_FLOW)) ||
       c->timeout_us>100000000 || c->reserved) return DE_FUNCTION;
    static const EFI_PARITY_TYPE parity[]={NoParity,OddParity,EvenParity,MarkParity,SpaceParity};
    EFI_STOP_BITS_TYPE stop=c->stop_bits==IO_STOP_ONE?OneStopBit:c->stop_bits==IO_STOP_TWO?TwoStopBits:OneFiveStopBits;
    SERIAL_IO_MODE before=*u->io->Mode; UINT32 old=0;
    EFI_STATUS s=u->io->GetControl(u->io,&old); if(EFI_ERROR(s)) return status_error(s);
    /* The firmware timeout only bounds a call that DOS already expects to
     * complete; IO.SYS enforces the requested wait itself, including zero. */
    s=u->io->SetAttributes(u->io,c->baud,before.ReceiveFifoDepth,c->timeout_us?c->timeout_us:1,
        parity[c->parity],(UINT8)c->data_bits,stop);
    if(EFI_ERROR(s)) return status_error(s);
    UINT32 control=(c->control&IO_SERIAL_DTR?EFI_SERIAL_DATA_TERMINAL_READY:0)|(c->control&IO_SERIAL_RTS?EFI_SERIAL_REQUEST_TO_SEND:0)|
        (c->control&IO_SERIAL_LOOPBACK?EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE:0)|
        (c->control&IO_SERIAL_HW_FLOW?EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE:0);
    s=u->io->SetControl(u->io,control);
    if(EFI_ERROR(s)) {
        set_mode(u->io,&before);
        u->io->SetControl(u->io,old&SETTABLE);
        return status_error(s);
    }
    u->timeout_us=c->timeout_us; return 0;
}
static int serial_status(void *context,u64 token,u32 *out) {
    (void)context; Unit *u=unit_for(token); if(!u) return DE_HANDLE;
    if(!out) return DE_FUNCTION;
    UINT32 control=0; EFI_STATUS s=u->io->GetControl(u->io,&control);
    if(EFI_ERROR(s)) return status_error(s);
    *out=status_bits(control); return 0;
}
static int pause(u32 *waited,u32 limit) {
    if(*waited>=limit) return DE_NOTREADY;
    u32 us=MIN(100,limit-*waited); *waited+=us;
    return efi_dos_error(boot->Stall(us));
}
static int serial_read(void *context,u64 token,void *buffer,u32 count,u32 *done) {
    (void)context;
    if(!done) return DE_FUNCTION;
    *done=0; Unit *u=unit_for(token); if(!u) return DE_HANDLE;
    if(count && !buffer) return DE_FUNCTION;
    while(*done<count) {
        for(u32 waited=0;;) {
            UINT32 control=0; EFI_STATUS s=u->io->GetControl(u->io,&control);
            if(EFI_ERROR(s)) return status_error(s);
            if(!(control&EFI_SERIAL_INPUT_BUFFER_EMPTY)) break;
            int e=pause(&waited,u->timeout_us); if(e) return e;
        }
        UINTN size=1; EFI_STATUS s=u->io->Read(u->io,&size,(u8 *)buffer+*done);
        if(size>1) return DE_IO;
        *done+=(u32)size;
        if(EFI_ERROR(s)) return status_error(s);
        if(!size) return DE_IO;
    }
    return 0;
}
static int serial_write(void *context,u64 token,const void *buffer,u32 count,u32 *done) {
    (void)context;
    if(!done) return DE_FUNCTION;
    *done=0; Unit *u=unit_for(token); if(!u) return DE_HANDLE;
    if(count && !buffer) return DE_FUNCTION;
    for(u32 waited=0;*done<count;) {
        UINTN size=count-*done; EFI_STATUS s=u->io->Write(u->io,&size,(u8 *)buffer+*done);
        if(size>count-*done) return DE_IO;
        *done+=(u32)size;
        if(size) waited=0;
        if(!EFI_ERROR(s)) {if(*done<count && !size) return DE_IO; continue;}
        if(s!=EFI_TIMEOUT) return status_error(s);
        if(!size) {int e=pause(&waited,u->timeout_us); if(e) return e;}
    }
    return 0;
}
void efi_serial_init(EFI_SYSTEM_TABLE *st,IoServices *io) {
    boot=st->BootServices; unit_count=0; memset(units,0,sizeof(units)); console_serial=NULL;
    EFI_HANDLE *handles=NULL; UINTN count=0;
    if(boot->LocateHandleBuffer && boot->HandleProtocol && boot->AllocatePool && boot->FreePool && boot->Stall &&
       !EFI_ERROR(boot->LocateHandleBuffer(ByProtocol,&serial_guid,NULL,&count,&handles))) {
        for(UINTN i=0;i<count && unit_count<MAX_UNITS;i++) {
            SERIAL_IO_INTERFACE *serial=NULL;
            if(EFI_ERROR(boot->HandleProtocol(handles[i],&serial_guid,(void **)&serial)) || !serial ||
               !serial->Mode || !serial->SetAttributes || !serial->SetControl || !serial->GetControl ||
               !serial->Read || !serial->Write) continue;
            if(console_device(st,handles[i])) {
                if(!console_serial && console_output(st,handles[i])) console_serial=serial;
                continue;
            }
            units[unit_count++]=(Unit){.io=serial};
        }
        boot->FreePool(handles);
    }
    io->serial_count=serial_count; io->serial_open=serial_open; io->serial_close=serial_close;
    io->serial_config=serial_config; io->serial_status=serial_status;
    io->serial_read=serial_read; io->serial_write=serial_write;
    io->capabilities|=IO_CAP_SERIAL;
}
void efi_serial_close(void) {
    for(unsigned i=0;i<unit_count;i++) if(units[i].token) {restore(&units[i]); units[i].token=0;}
    unit_count=0;
}
