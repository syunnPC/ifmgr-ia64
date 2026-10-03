/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host checks for IO.SYS serial services: EFI Serial I/O units and the
 * timer-sampled legacy UART ring, using mock Boot Services and devices.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../platform/efi_serial.h"
#include "../platform/efi_ports.h"
static unsigned checks;
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"FAIL serial:%u: %s\n",__LINE__,#x); exit(1);}} while(0)
void con_write(const void *p,size_t n) {fwrite(p,1,n,stdout);}
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE st;
static IoServices io;
static unsigned pools,stalled;
/* --- Timer events and TPL ------------------------------------------------ */
static EFI_TPL tpl=TPL_APPLICATION;
static EFI_EVENT_NOTIFY notify;
static void *notify_context;
static int timer_live,timer_periodic,timer_pending,events,raised_access;
static u64 timer_period;
static void tick(void) {
    if(!timer_live || !timer_periodic) return;
    /* An asynchronous timer cannot preempt code already at TPL_NOTIFY. */
    if(tpl>=TPL_NOTIFY) {timer_pending=1; return;}
    EFI_TPL saved=tpl; tpl=TPL_NOTIFY; notify((EFI_EVENT)&timer_live,notify_context); tpl=saved;
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL value) {CHECK(value>=tpl); EFI_TPL old=tpl; tpl=value; return old;}
static VOID EFIAPI restore_tpl(EFI_TPL value) {
    CHECK(value<=tpl); tpl=value;
    if(timer_pending && tpl<TPL_NOTIFY) {timer_pending=0; tick();}
}
static EFI_STATUS EFIAPI create_event(UINT32 type,EFI_TPL level,EFI_EVENT_NOTIFY function,VOID *context,EFI_EVENT *event) {
    CHECK(type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && level==TPL_NOTIFY && function && !timer_live);
    notify=function; notify_context=context; timer_live=1; events++; *event=(EFI_EVENT)&timer_live; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_timer(EFI_EVENT event,EFI_TIMER_DELAY type,UINT64 trigger) {
    CHECK(event==(EFI_EVENT)&timer_live && timer_live);
    timer_periodic=type==TimerPeriodic; timer_period=trigger; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT event) {
    CHECK(event==(EFI_EVENT)&timer_live && timer_live); timer_live=timer_periodic=0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall(UINTN us) {stalled+=us; tick(); return EFI_SUCCESS;}
static unsigned pool_failures;
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type,UINTN size,VOID **out) {
    (void)type; if(pool_failures) {pool_failures--; return EFI_OUT_OF_RESOURCES;}
    *out=malloc(size); CHECK(*out); pools++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pool(VOID *p) {CHECK(pools); pools--; free(p); return EFI_SUCCESS;}
/* --- Handles and device paths --------------------------------------------- */
enum {H_ROOT=1,H_CONIN,H_CONOUT,H_TEXT,H_S0,H_S1,H_S2,H_S3,H_S4,H_S5,H_S6,H_S7,H_LAST};
#define SERIALS (H_S7-H_S0+1)
#define HANDLE(n) ((EFI_HANDLE)(uintptr_t)(n))
static u8 paths[H_LAST][64];
static int has_path[H_LAST];
/* ACPI PNP0501 serial, optionally its UART node and a terminal child node;
 * terminated by an end node of the given subtype. Returns the bytes used. */
static unsigned build_path(u8 *p,u32 uid,u32 baud,int terminal,u8 end) {
    unsigned n=0;
    p[n]=2; p[n+1]=1; wr16(p+n+2,12); wr32(p+n+4,0x050141d0); wr32(p+n+8,uid); n+=12;
    if(baud) {p[n]=3; p[n+1]=14; wr16(p+n+2,19); memset(p+n+4,0,15); wr32(p+n+8,baud); p[n+16]=8; n+=19;}
    if(terminal) {p[n]=3; p[n+1]=10; wr16(p+n+2,20); memset(p+n+4,0x5a,16); n+=20;}
    p[n]=0x7f; p[n+1]=end; wr16(p+n+2,4); return n+4;
}
static void acpi_path(unsigned handle,u32 uid,u32 baud,int terminal) {build_path(paths[handle],uid,baud,terminal,0xff); has_path[handle]=1;}
/* ConOut names S5 at another baud rate; ConIn's second instance names S6. */
static u8 con_out[64],con_in[128];
static unsigned con_out_size,con_in_size;
static int bad_variable;
static EFI_STATUS EFIAPI get_variable(CHAR16 *name,EFI_GUID *vendor,UINT32 *attributes,UINTN *size,VOID *data) {
    EFI_GUID global=EFI_GLOBAL_VARIABLE; CHECK(!memcmp(vendor,&global,sizeof(global)) && !attributes && size);
    static const char *names[]={"ConIn","ConOut","ErrOut","ConInDev","ConOutDev","ErrOutDev"};
    unsigned which=ARRAY_SIZE(names);
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) {
        unsigned j=0; while(names[i][j] && name[j]==(CHAR16)names[i][j]) j++;
        if(!names[i][j] && !name[j]) which=i;
    }
    CHECK(which<ARRAY_SIZE(names));
    const u8 *value=which==0?con_in:which==1?con_out:NULL; UINTN length=which==0?con_in_size:con_out_size;
    if(!value) return EFI_NOT_FOUND;
    if(bad_variable) length=6;
    if(*size<length) {*size=length; return EFI_BUFFER_TOO_SMALL;}
    CHECK(data); memcpy(data,value,length); *size=length; return EFI_SUCCESS;
}
static EFI_RUNTIME_SERVICES rt={.GetVariable=get_variable};
typedef struct {
    EFI_SERIAL_IO_PROTOCOL protocol;
    SERIAL_IO_MODE mode;
    u32 control,modem,rx_pos,rx_used,tx_used,tx_limit,reads,writes,attributes,controls;
    u8 rx[64],tx[64];
    EFI_STATUS read_error,write_error,control_error;
    u64 baud; u32 fifo,timeout,parity,data,stop,last_control;
} MockSerial;
static MockSerial serial[SERIALS];
static MockSerial *mock(EFI_SERIAL_IO_PROTOCOL *p) {return (MockSerial *)p;}
static EFI_STATUS EFIAPI m_reset(EFI_SERIAL_IO_PROTOCOL *p) {(void)p; CHECK(0); return EFI_SUCCESS;}
static EFI_STATUS EFIAPI m_attributes(EFI_SERIAL_IO_PROTOCOL *p,UINT64 baud,UINT32 fifo,UINT32 timeout,EFI_PARITY_TYPE parity,UINT8 data,EFI_STOP_BITS_TYPE stop) {
    MockSerial *m=mock(p); m->attributes++;
    if(baud==12345) return EFI_INVALID_PARAMETER;
    m->baud=baud; m->fifo=fifo; m->timeout=timeout; m->parity=parity; m->data=data; m->stop=stop;
    m->mode.BaudRate=baud; m->mode.ReceiveFifoDepth=fifo; m->mode.Timeout=timeout;
    m->mode.Parity=parity; m->mode.DataBits=data; m->mode.StopBits=stop; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_set_control(EFI_SERIAL_IO_PROTOCOL *p,UINT32 control) {
    MockSerial *m=mock(p); m->controls++; m->last_control=control;
    CHECK(!(control&~0x7003U));
    if(m->control_error) {EFI_STATUS e=m->control_error; m->control_error=0; return e;}
    m->control=control; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_get_control(EFI_SERIAL_IO_PROTOCOL *p,UINT32 *out) {
    MockSerial *m=mock(p);
    *out=m->control|m->modem|(m->rx_used?0:EFI_SERIAL_INPUT_BUFFER_EMPTY)|EFI_SERIAL_OUTPUT_BUFFER_EMPTY; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_write(EFI_SERIAL_IO_PROTOCOL *p,UINTN *size,VOID *data) {
    MockSerial *m=mock(p); m->writes++;
    if(m->write_error) {*size=0; return m->write_error;}
    UINTN take=MIN(*size,(UINTN)m->tx_limit); memcpy(m->tx+m->tx_used,data,take); m->tx_used+=take; m->tx_limit-=take;
    EFI_STATUS e=take<*size?EFI_TIMEOUT:EFI_SUCCESS; *size=take; return e;
}
static EFI_STATUS EFIAPI m_read(EFI_SERIAL_IO_PROTOCOL *p,UINTN *size,VOID *data) {
    MockSerial *m=mock(p); m->reads++;
    if(m->read_error) {*size=0; return m->read_error;}
    UINTN take=MIN(*size,(UINTN)m->rx_used); memcpy(data,m->rx+m->rx_pos,take); m->rx_pos+=take; m->rx_used-=take;
    EFI_STATUS e=take<*size?EFI_TIMEOUT:EFI_SUCCESS; *size=take; return e;
}
static void serial_reset(MockSerial *m) {
    memset(m,0,sizeof(*m));
    m->protocol=(EFI_SERIAL_IO_PROTOCOL){SERIAL_IO_INTERFACE_REVISION,m_reset,m_attributes,m_set_control,m_get_control,m_write,m_read,&m->mode};
    m->mode=(SERIAL_IO_MODE){.ControlMask=0x37f3,.Timeout=1000000,.BaudRate=115200,.ReceiveFifoDepth=16,.DataBits=8,.Parity=NoParity,.StopBits=OneStopBit};
    m->control=EFI_SERIAL_DATA_TERMINAL_READY|EFI_SERIAL_REQUEST_TO_SEND; m->tx_limit=sizeof(m->tx);
}
static void rx(MockSerial *m,const char *text) {
    size_t n=strlen(text); CHECK(m->rx_pos+m->rx_used+n<=sizeof(m->rx));
    memcpy(m->rx+m->rx_pos+m->rx_used,text,n); m->rx_used+=n;
}
/* --- Legacy UART at 2F8 and printer at 378 behind the root bridge ---------- */
typedef struct {
    u8 fifo[16],lcr,mcr,ier,scr,dll,dlm,errors; unsigned head,used;
    u8 line[8192]; unsigned line_pos,line_used;
    unsigned read_failure,accesses;
} MockUart;
static MockUart uart;
static int buffered_mode,fire_inside;
static void refill(void) {
    while(uart.used<16 && uart.line_pos<uart.line_used) {uart.fifo[(uart.head+uart.used++)%16]=uart.line[uart.line_pos++];}
}
static void wire(const u8 *data,unsigned n) {
    CHECK(uart.line_used+n<=sizeof(uart.line)); memcpy(uart.line+uart.line_used,data,n); uart.line_used+=n; refill();
}
static EFI_STATUS EFIAPI io_read(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 address,UINTN count,VOID *buffer) {
    (void)This; CHECK(!width && count==1); u8 *v=buffer; uart.accesses++;
    if(buffered_mode && address>=0x2f8 && address<0x300) {CHECK(tpl==TPL_NOTIFY); raised_access++;}
    if(fire_inside) tick();
    if(uart.read_failure && !--uart.read_failure) return EFI_DEVICE_ERROR;
    if(address>=0x378 && address<0x37b) {*v=0x5a; return EFI_SUCCESS;}
    CHECK(address>=0x2f8 && address<0x300);
    switch(address-0x2f8) {
    case 0:
        if(uart.lcr&0x80) {*v=uart.dll; break;}
        if(uart.used) {*v=uart.fifo[uart.head]; uart.head=(uart.head+1)%16; uart.used--; refill();} else *v=0;
        break;
    case 1: *v=uart.lcr&0x80?uart.dlm:uart.ier; break;
    case 2: *v=0xc1; break;
    case 3: *v=uart.lcr; break;
    case 4: *v=uart.mcr; break;
    case 5: *v=(uart.used?1:0)|0x60|uart.errors; uart.errors=0; break;
    case 6: *v=0xb0; break;
    default: *v=uart.scr;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI io_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *This,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,UINT64 address,UINTN count,VOID *buffer) {
    (void)This; CHECK(!width && count==1); u8 v=*(u8 *)buffer; uart.accesses++;
    if(buffered_mode && address>=0x2f8 && address<0x300) {CHECK(tpl==TPL_NOTIFY); raised_access++;}
    if(address>=0x378 && address<0x37b) return EFI_SUCCESS;
    CHECK(address>=0x2f8 && address<0x300);
    switch(address-0x2f8) {
    case 0: if(uart.lcr&0x80) uart.dll=v; else if(uart.mcr&0x10) wire(&v,1); break;
    case 1: if(uart.lcr&0x80) uart.dlm=v; else uart.ier=v; break;
    case 2: if(v&2) {uart.head=uart.used=0; refill();} break;
    case 3: uart.lcr=v; break;
    case 4: uart.mcr=v; break;
    case 7: uart.scr=v; break;
    }
    return EFI_SUCCESS;
}
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL root_bridge={.Io={io_read,io_write}};
/* --- Protocol database ---------------------------------------------------- */
static EFI_GUID serial_guid=EFI_SERIAL_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID,
    root_guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID,in_guid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,
    out_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static int same(const EFI_GUID *a,const EFI_GUID *b) {return !memcmp(a,b,sizeof(*a));}
static int no_text_consoles;
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **out) {
    CHECK(type==ByProtocol && !key);
    unsigned list[SERIALS],n=0;
    if(same(guid,&root_guid)) list[n++]=H_ROOT;
    else if(same(guid,&serial_guid)) for(unsigned h=H_S0;h<=H_S7;h++) list[n++]=h;
    else if(no_text_consoles==2) return EFI_OUT_OF_RESOURCES;
    else if(same(guid,&in_guid) && !no_text_consoles) list[n++]=H_CONIN;
    else if(same(guid,&out_guid) && !no_text_consoles) {list[n++]=H_CONOUT; list[n++]=H_TEXT;}
    if(!n) return EFI_NOT_FOUND;
    EFI_HANDLE *handles; CHECK(!allocate_pool(EfiLoaderData,n*sizeof(*handles),(void **)&handles));
    for(unsigned i=0;i<n;i++) handles[i]=HANDLE(list[i]);
    *count=n; *out=handles; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE handle,EFI_GUID *guid,VOID **out) {
    unsigned h=(unsigned)(uintptr_t)handle; CHECK(h && h<H_LAST);
    if(same(guid,&root_guid) && h==H_ROOT) {*out=&root_bridge; return EFI_SUCCESS;}
    if(same(guid,&serial_guid) && h>=H_S0 && h<=H_S7) {*out=&serial[h-H_S0].protocol; return EFI_SUCCESS;}
    if(same(guid,&path_guid) && has_path[h]) {*out=paths[h]; return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI open_information(EFI_HANDLE handle,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count) {
    CHECK(same(guid,&serial_guid)); *count=0; *out=NULL;
    if(handle!=HANDLE(H_S1)) return EFI_SUCCESS;
    CHECK(!allocate_pool(EfiLoaderData,sizeof(**out),(void **)out));
    **out=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){HANDLE(H_TEXT),handle,EFI_OPEN_PROTOCOL_BY_DRIVER,1};
    *count=1; return EFI_SUCCESS;
}
static void boot_reset(void) {
    memset(&bs,0,sizeof(bs)); memset(&st,0,sizeof(st)); memset(&io,0,sizeof(io));
    bs.RaiseTPL=raise_tpl; bs.RestoreTPL=restore_tpl; bs.CreateEvent=create_event; bs.SetTimer=set_timer;
    bs.CloseEvent=close_event; bs.Stall=stall; bs.AllocatePool=allocate_pool; bs.FreePool=free_pool;
    bs.LocateHandleBuffer=locate; bs.HandleProtocol=handle_protocol; bs.OpenProtocolInformation=open_information;
    st.BootServices=&bs; st.RuntimeServices=&rt; st.ConsoleInHandle=HANDLE(H_CONIN); st.ConsoleOutHandle=HANDLE(H_CONOUT);
    st.StandardErrorHandle=HANDLE(H_S4); /* A firmware serial that is itself the error console. */
    io.version=IO_ABI_VERSION; io.size=sizeof(io);
}
static void test_firmware_serial(void) {
    boot_reset();
    for(unsigned i=0;i<SERIALS;i++) serial_reset(&serial[i]);
    acpi_path(H_S0,0,0,0); acpi_path(H_CONOUT,0,115200,1); acpi_path(H_CONIN,9,0,1); acpi_path(H_TEXT,8,0,1);
    acpi_path(H_S1,1,0,0); acpi_path(H_S2,2,115200,0); acpi_path(H_S4,4,0,0); acpi_path(H_S5,5,115200,0);
    acpi_path(H_S6,6,0,0); acpi_path(H_S7,7,57600,0);
    acpi_path(H_S3,3,0,0); has_path[H_S3]=0; /* No device path: cannot prove it is not a console. */
    con_out_size=build_path(con_out,5,9600,1,0xff);
    con_in_size=build_path(con_in,0x30,0,0,0x01); con_in_size+=build_path(con_in+con_in_size,6,0,1,0xff);
    /* S0 underlies the ConOut handle, S1 is held by a terminal driver, S3 has
     * no path, S4 is StdErr, S5/S6 are ConOut/ConIn variable instances. Only S2
     * and S7 become units, in firmware order. */
    efi_serial_init(&st,&io); CHECK(!pools);
    CHECK((io.capabilities&IO_CAP_SERIAL) && io.serial_count(NULL)==2);
    /* S0, under the ConOut handle, is the first console output device. While
     * a graphical shell owns the screen it gets ASCII console text, LF as CR LF. */
    CHECK(efi_serial_console_present());
    serial[0].tx_limit=sizeof(serial[0].tx);
    efi_serial_console_write("a\nb\0\x80" "c",6);
    static const u16 wide[]={'d','\n',0x3042,0,'e'}; efi_serial_console_write_text(wide,5);
    CHECK(serial[0].tx_used==9 && !memcmp(serial[0].tx,"a\r\nbcd\r\ne",9) && !serial[4].writes && !serial[5].writes);
    u64 a=0,b=0,stale; IoSerialConfig c; u32 bits,n; u8 buffer[8];
    CHECK(io.serial_open(NULL,2,&a)==DE_DRIVE && !a && io.serial_open(NULL,0,NULL)==DE_FUNCTION);
    CHECK(!io.serial_open(NULL,0,&a) && a && io.serial_open(NULL,0,&b)==DE_BUSY && !b);
    CHECK(!io.serial_open(NULL,1,&b) && b && b!=a);
    MockSerial *m=&serial[2],*other=&serial[7];
    c=(IoSerialConfig){sizeof(c),9600,500,7,IO_PARITY_EVEN,IO_STOP_TWO,IO_SERIAL_DTR|IO_SERIAL_LOOPBACK|IO_SERIAL_HW_FLOW,0};
    CHECK(!io.serial_config(NULL,a,&c));
    CHECK(m->baud==9600 && m->timeout==500 && m->data==7 && m->parity==EvenParity && m->stop==TwoStopBits && m->fifo==16);
    CHECK(m->control==(EFI_SERIAL_DATA_TERMINAL_READY|EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE|EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE));
    static const u32 parities[]={NoParity,OddParity,EvenParity,MarkParity,SpaceParity};
    for(u32 parity=0;parity<5;parity++) {
        c=(IoSerialConfig){sizeof(c),1200,0,5,parity,IO_STOP_ONE_HALF,IO_SERIAL_RTS,0};
        CHECK(!io.serial_config(NULL,a,&c) && m->parity==parities[parity] && m->stop==OneFiveStopBits && m->timeout==1);
        CHECK(m->control==EFI_SERIAL_REQUEST_TO_SEND && m->data==5);
    }
    unsigned calls=m->attributes; IoSerialConfig good={sizeof(good),115200,0,8,IO_PARITY_NONE,IO_STOP_ONE,0,0};
    IoSerialConfig bad[]={good,good,good,good,good,good,good,good,good};
    bad[0].size=0; bad[1].baud=49; bad[2].baud=921601; bad[3].data_bits=9; bad[4].parity=5;
    bad[5].stop_bits=3; bad[6].control=IO_SERIAL_CTS; bad[7].timeout_us=100000001; bad[8].reserved=1;
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++) CHECK(io.serial_config(NULL,a,&bad[i])==DE_FUNCTION);
    CHECK(io.serial_config(NULL,a,NULL)==DE_FUNCTION && m->attributes==calls);
    SERIAL_IO_MODE before=m->mode; u32 control=m->control;
    good.baud=12345; CHECK(io.serial_config(NULL,a,&good)==DE_FUNCTION);
    CHECK(!memcmp(&m->mode,&before,sizeof(before)) && m->control==control);
    good.baud=19200; m->control_error=EFI_DEVICE_ERROR;
    CHECK(io.serial_config(NULL,a,&good)==DE_IO); /* Attributes and control are rolled back. */
    CHECK(!memcmp(&m->mode,&before,sizeof(before)) && m->control==control);
    m->modem=EFI_SERIAL_CLEAR_TO_SEND|EFI_SERIAL_DATA_SET_READY|EFI_SERIAL_RING_INDICATE|EFI_SERIAL_CARRIER_DETECT;
    CHECK(!io.serial_status(NULL,a,&bits));
    CHECK(bits==(IO_SERIAL_RTS|IO_SERIAL_CTS|IO_SERIAL_DSR|IO_SERIAL_RING|IO_SERIAL_CARRIER|IO_SERIAL_INPUT_EMPTY|IO_SERIAL_OUTPUT_EMPTY));
    CHECK(io.serial_status(NULL,a,NULL)==DE_FUNCTION);
    /* Zero timeout never waits and never calls Read without input. */
    unsigned reads=m->reads; stalled=0;
    CHECK(io.serial_read(NULL,a,buffer,1,&n)==DE_NOTREADY && !n && m->reads==reads && !stalled);
    rx(m,"ABC"); good=(IoSerialConfig){sizeof(good),115200,300,8,IO_PARITY_NONE,IO_STOP_ONE,0,0};
    CHECK(!io.serial_config(NULL,a,&good) && m->timeout==300);
    CHECK(io.serial_read(NULL,a,buffer,5,&n)==DE_NOTREADY && n==3 && !memcmp(buffer,"ABC",3) && stalled==300);
    rx(m,"Z"); m->read_error=EFI_DEVICE_ERROR;
    CHECK(io.serial_read(NULL,a,buffer,1,&n)==DE_IO && !n); m->read_error=0;
    CHECK(!io.serial_read(NULL,a,buffer,1,&n) && n==1 && buffer[0]=='Z');
    CHECK(io.serial_read(NULL,a,NULL,1,&n)==DE_FUNCTION && io.serial_read(NULL,a,buffer,1,NULL)==DE_FUNCTION);
    m->tx_limit=2; stalled=0;
    CHECK(io.serial_write(NULL,a,"WXYZ",4,&n)==DE_NOTREADY && n==2 && !memcmp(m->tx,"WX",2) && stalled==300);
    m->tx_limit=sizeof(m->tx)-m->tx_used;
    CHECK(!io.serial_write(NULL,a,"ok",2,&n) && n==2 && !memcmp(m->tx+2,"ok",2));
    m->write_error=EFI_DEVICE_ERROR; CHECK(io.serial_write(NULL,a,"!",1,&n)==DE_IO && !n); m->write_error=0;
    CHECK(!io.serial_write(NULL,a,NULL,0,&n) && !n);
    /* Close restores the settings found at open; unrelated units are untouched. */
    CHECK(!io.serial_close(NULL,a) && m->baud==115200 && m->timeout==1000000 && m->data==8);
    CHECK(m->parity==NoParity && m->stop==OneStopBit && m->control==(EFI_SERIAL_DATA_TERMINAL_READY|EFI_SERIAL_REQUEST_TO_SEND));
    stale=a; CHECK(io.serial_close(NULL,stale)==DE_HANDLE && io.serial_read(NULL,stale,buffer,1,&n)==DE_HANDLE);
    CHECK(io.serial_config(NULL,stale,&good)==DE_HANDLE && io.serial_status(NULL,stale,&bits)==DE_HANDLE);
    CHECK(!io.serial_open(NULL,0,&a) && a!=stale);
    good.baud=2400; CHECK(!io.serial_config(NULL,b,&good) && other->baud==2400);
    efi_serial_close(); CHECK(other->baud==115200 && io.serial_count(NULL)==0 && !pools);
    /* Without text console handles, console handle paths and variables still
     * exclude S0, S5 and S6. Unknown or malformed topology publishes nothing. */
    boot_reset(); for(unsigned i=0;i<SERIALS;i++) serial_reset(&serial[i]);
    no_text_consoles=1; efi_serial_init(&st,&io); CHECK(io.serial_count(NULL)==2 && !pools); efi_serial_close();
    no_text_consoles=2; efi_serial_init(&st,&io); CHECK(io.serial_count(NULL)==0 && !pools); efi_serial_close();
    no_text_consoles=0; bad_variable=1; efi_serial_init(&st,&io); CHECK(io.serial_count(NULL)==0 && !pools);
    bad_variable=0; efi_serial_close();
    st.RuntimeServices=NULL; efi_serial_init(&st,&io); CHECK(io.serial_count(NULL)==4 && !pools); efi_serial_close();
    /* Only positive evidence names the console's serial device: unreadable
     * variables and unrelated console handles leave console text nowhere. */
    boot_reset(); for(unsigned i=0;i<SERIALS;i++) serial_reset(&serial[i]);
    st.ConsoleOutHandle=st.StandardErrorHandle=HANDLE(H_CONIN); bad_variable=1;
    efi_serial_init(&st,&io); CHECK(io.serial_count(NULL)==0 && !efi_serial_console_present() && !pools);
    efi_serial_console_write("x\n",2);
    for(unsigned i=0;i<SERIALS;i++) CHECK(!serial[i].writes);
    bad_variable=0; efi_serial_close();
}
static int legacy_read(u64 token,u32 reg,u8 *value) {return io.port_read(io.context,token,reg,value);}
static void test_buffered_uart(void) {
    boot_reset(); memset(&uart,0,sizeof(uart)); uart.lcr=3;
    efi_ports_init(&st,&io); CHECK((io.capabilities&IO_CAP_LEGACY_PORTS) && (io.capabilities&IO_CAP_PORT_BUFFER) && !pools);
    u64 token=0,printer=0; u8 v; u32 pending;
    CHECK(!io.port_claim(NULL,IO_PORT_UART,0x2f8,&token) && token);
    CHECK(!io.port_claim(NULL,IO_PORT_PRINTER,0x378,&printer) && printer);
    CHECK(io.port_buffer(NULL,printer,1)==DE_FUNCTION && io.port_pending(NULL,printer,&pending)==DE_FUNCTION);
    CHECK(io.port_pending(NULL,token,&pending)==DE_FUNCTION); /* Not buffered yet. */
    pool_failures=1; CHECK(io.port_buffer(NULL,token,1)==DE_NOMEM && !timer_live && !pools);
    CHECK(!io.port_buffer(NULL,token,1) && timer_live && timer_periodic && timer_period==10000 && events==1 && pools==1);
    CHECK(!io.port_buffer(NULL,token,1) && events==1 && pools==1); buffered_mode=1;
    /* Bytes arriving while nobody reads move from the 16-byte FIFO to the ring. */
    u8 data[5000]; for(unsigned i=0;i<sizeof(data);i++) data[i]=(u8)(i*7+1);
    wire(data,40); CHECK(uart.used==16);
    tick(); CHECK(!io.port_pending(NULL,token,&pending) && pending==40 && !uart.used);
    CHECK(!legacy_read(token,5,&v) && v==0x61);
    for(unsigned i=0;i<40;i++) CHECK(!legacy_read(token,0,&v) && v==data[i]);
    CHECK(!legacy_read(token,5,&v) && v==0x60 && !io.port_pending(NULL,token,&pending) && !pending);
    /* Line errors survive until one LSR read reports them. */
    wire(data,3); uart.errors=0x08; tick();
    CHECK(!legacy_read(token,5,&v) && v==0x69); CHECK(!legacy_read(token,5,&v) && v==0x61);
    /* DLAB hides RBR: the timer must not drain divisor latch reads. */
    u8 dlab=0x83; CHECK(!io.port_write(NULL,token,3,dlab));
    wire(data+3,5); tick(); CHECK(!io.port_pending(NULL,token,&pending) && pending==3 && uart.used==5);
    uart.dll=12; CHECK(!legacy_read(token,0,&v) && v==12);
    CHECK(!io.port_write(NULL,token,3,3)); tick(); CHECK(!io.port_pending(NULL,token,&pending) && pending==8);
    /* A receive FIFO reset also discards the ring. */
    CHECK(!io.port_write(NULL,token,2,3) && !io.port_pending(NULL,token,&pending) && !pending);
    /* A full ring leaves data in the FIFO; nothing is dropped in software. */
    uart.line_pos=uart.line_used=0; wire(data,sizeof(data));
    for(unsigned i=0;i<200;i++) tick();
    CHECK(!io.port_pending(NULL,token,&pending) && pending==4096 && uart.used==16);
    fire_inside=1;
    for(unsigned i=0;i<sizeof(data);i++) CHECK(!legacy_read(token,0,&v) && v==data[i]);
    fire_inside=0; CHECK(!io.port_pending(NULL,token,&pending) && !pending && !timer_pending);
    /* A sampling failure is reported once by the next foreground access. */
    wire(data,2); uart.read_failure=1; tick();
    CHECK(legacy_read(token,0,&v)==DE_IO); tick();
    CHECK(!legacy_read(token,0,&v) && v==data[0] && !legacy_read(token,0,&v) && v==data[1]);
    CHECK(raised_access && io.port_buffer(NULL,0,1)==DE_HANDLE && io.port_pending(NULL,token,NULL)==DE_FUNCTION);
    /* Disabling stops the timer; release of a buffered claim does too. */
    buffered_mode=0; CHECK(!io.port_buffer(NULL,token,0) && !timer_periodic && !pools);
    wire(data,1); tick(); CHECK(uart.used==1 && !legacy_read(token,0,&v) && v==data[0]);
    CHECK(!io.port_buffer(NULL,token,1) && timer_periodic && pools==1); buffered_mode=1;
    buffered_mode=0; CHECK(!io.port_release(NULL,token) && !timer_periodic && !pools);
    CHECK(io.port_pending(NULL,token,&pending)==DE_HANDLE && !io.port_release(NULL,printer));
    efi_ports_close(); CHECK(!timer_live && !pools && tpl==TPL_APPLICATION);
}
int main(void) {
    test_firmware_serial();
    test_buffered_uart();
    printf("PASS serial backends: %u assertions (EFI Serial I/O units, console exclusion and console text route, timeouts, rollback and timer-sampled UART ring)\n",checks);
    return 0;
}
