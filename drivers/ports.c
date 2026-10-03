/* SPDX-License-Identifier: GPL-2.0-or-later
 * Polled 16550 UART, firmware serial and standard parallel-port drivers. All
 * bus access, buffering and delays use IO.SYS; no firmware, IA-64 MMIO or DOS
 * calls occur here.
 */
#include "ports.h"
enum {RBR=0,IER=1,FCR=2,LCR=3,MCR=4,LSR=5,MSR=6,SCR=7};
enum {SAVE_LCR=1,SAVE_IER=2,SAVE_MCR=4,SAVE_DIVISOR=8,SAVE_SCR=16,CHANGED_FIFO=32,
      SAVE_DATA=64,SAVE_CONTROL=128};
typedef struct {
    u64 token;
    u32 base,kind,saved_flags,errors,unit;
    DosPortConfig config;
    u8 saved[8],peek,have_peek,pending_output,configured,firmware,buffered;
    int fault;
} Port;
static Port ports[PORT_DEVICE_COUNT];
static const IoServices *io;
#define HAS(member) (io->size>=offsetof(IoServices,member)+sizeof(io->member) && io->member)
static int get(Port *p,u32 reg,u8 *out) {return io->port_read(io->context,p->token,reg,out);}
static int put(Port *p,u32 reg,u8 value) {return io->port_write(io->context,p->token,reg,value);}
static int delay(u32 us) {return io->stall_us(io->context,us);}
static int parse(const char *tail) {
    unsigned count=0;
    while(*tail) {
        while(*tail==' ' || *tail=='\t') tail++;
        if(!*tail) break;
        char name[5]={0}; unsigned n=0;
        while(*tail && *tail!='=' && n<4) name[n++]=upper(*tail++);
        if(n!=4 || *tail++!='=') return DE_FUNCTION;
        unsigned index;
        if(!memcmp(name,"COM",3) && name[3]>='1' && name[3]<='4') index=name[3]-'1';
        else if(!memcmp(name,"LPT",3) && name[3]>='1' && name[3]<='3') index=4+name[3]-'1';
        else return DE_FUNCTION;
        Port *p=&ports[index];
        if(p->configured) return DE_EXISTS;
        if(index<4 && upper(tail[0])=='E' && upper(tail[1])=='F' && upper(tail[2])=='I') {
            tail+=3; u32 unit=0; n=0;
            while(*tail>='0' && *tail<='9') {unit=unit*10+(*tail++-'0'); if(++n>2) return DE_FUNCTION;}
            if(!n || unit>15 || (*tail && *tail!=' ' && *tail!='\t')) return DE_FUNCTION;
            p->unit=unit; p->firmware=p->configured=1; count++; continue;
        }
        if(tail[0]=='0' && upper(tail[1])=='X') tail+=2;
        u32 base=0; n=0;
        while(*tail && *tail!=' ' && *tail!='\t') {
            char c=upper(*tail++); unsigned digit;
            if(c>='0' && c<='9') digit=c-'0';
            else if(c>='A' && c<='F') digit=10+c-'A';
            else return DE_FUNCTION;
            if(++n>4) return DE_FUNCTION;
            base=base*16+digit;
        }
        if(!n || !base) return DE_FUNCTION;
        p->base=base; p->configured=1; count++;
    }
    return count?0:DE_FUNCTION;
}
static int firmware_status(Port *p,DosPortInfo *info) {
    u32 bits; int e=io->serial_status(io->context,p->token,&bits); if(e) return e;
    int rx=p->have_peek || !(bits&IO_SERIAL_INPUT_EMPTY);
    int tx=!(p->config.flags&DOS_PORT_CTS_FLOW) || (bits&IO_SERIAL_CTS);
    info->line_status=(rx?1:0)|(tx?0x20:0)|(bits&IO_SERIAL_OUTPUT_EMPTY?0x40:0)|(p->errors?0x80:0);
    info->modem_status=(bits&IO_SERIAL_CTS?0x10:0)|(bits&IO_SERIAL_DSR?0x20:0)|
        (bits&IO_SERIAL_RING?0x40:0)|(bits&IO_SERIAL_CARRIER?0x80:0);
    if(rx) info->status|=DOS_PORT_RX_READY;
    if(tx) info->status|=DOS_PORT_TX_READY;
    if(bits&IO_SERIAL_OUTPUT_EMPTY) info->status|=DOS_PORT_TX_EMPTY;
    if(bits&IO_SERIAL_CTS) info->status|=DOS_PORT_CTS;
    if(bits&IO_SERIAL_DSR) info->status|=DOS_PORT_DSR;
    if(bits&IO_SERIAL_CARRIER) info->status|=DOS_PORT_CARRIER;
    if(p->errors) info->status|=DOS_PORT_ERROR;
    info->receive_errors=p->errors; return 0;
}
static int status(Port *p,DosPortInfo *info) {
    *info=(DosPortInfo){.size=sizeof(*info),.kind=p->firmware?IO_PORT_SERIAL:p->kind,
        .base=p->firmware?p->unit:p->base,.config=p->config,.buffered=p->have_peek};
    if(p->fault) {info->status=DOS_PORT_FAULTED; info->receive_errors=p->errors; return 0;}
    if(p->firmware) return firmware_status(p,info);
    u8 line,modem; int e=get(p,p->kind==IO_PORT_UART?LSR:1,&line); if(e) return e;
    info->line_status=line;
    if(p->kind==IO_PORT_UART) {
        p->errors|=line&0x1e;
        e=get(p,MSR,&modem); if(e) return e;
        info->modem_status=modem;
        if(p->have_peek || (line&1)) info->status|=DOS_PORT_RX_READY;
        if((line&0x20) && (!(p->config.flags&DOS_PORT_CTS_FLOW) || (modem&0x10))) info->status|=DOS_PORT_TX_READY;
        if(line&0x40) info->status|=DOS_PORT_TX_EMPTY;
        if(modem&0x10) info->status|=DOS_PORT_CTS;
        if(modem&0x20) info->status|=DOS_PORT_DSR;
        if(modem&0x80) info->status|=DOS_PORT_CARRIER;
        if(p->errors) info->status|=DOS_PORT_ERROR;
        if(p->buffered) info->status|=DOS_PORT_BUFFERED;
    } else {
        if((line&0xb8)==0x98) {info->status|=DOS_PORT_TX_READY; p->pending_output=0;}
        if(!p->pending_output) info->status|=DOS_PORT_TX_EMPTY;
        if(line&0x20) info->status|=DOS_PORT_PAPER_OUT;
        if(line&0x10) info->status|=DOS_PORT_SELECTED;
        if(!(line&8)) info->status|=DOS_PORT_ERROR;
    }
    info->receive_errors=p->errors; return 0;
}
static int await_status(Port *p,u32 wanted,int wait) {
    u32 remaining=wait?p->config.timeout_us:0;
    for(;;) {
        DosPortInfo info; int e=status(p,&info); if(e) return e;
        if(wanted==DOS_PORT_RX_READY && p->errors) return DE_IO;
        if(info.status&wanted) return 0;
        if(!remaining) return DE_NOTREADY;
        u32 us=MIN(remaining,100); e=delay(us); if(e) return e;
        remaining-=us;
    }
}
static int configure(Port *p,const DosPortConfig *c,int initial) {
    if(c->size!=sizeof(*c) || c->version!=DOS_PORT_ABI || c->timeout_us>1000000) return DE_FUNCTION;
    if(p->kind==IO_PORT_PRINTER) {
        if(c->baud || c->data_bits || c->parity || c->stop_bits || c->flags) return DE_FUNCTION;
        p->config=*c; return 0;
    }
    if(!c->baud || c->baud>115200 || 115200%c->baud || 115200/c->baud>65535 ||
       c->data_bits<5 || c->data_bits>8 || c->parity>DOS_PARITY_SPACE ||
       (c->flags&~(DOS_PORT_DTR|DOS_PORT_RTS|DOS_PORT_CTS_FLOW|DOS_PORT_LOOPBACK)) ||
       (c->stop_bits!=DOS_STOP_ONE && c->stop_bits!=DOS_STOP_TWO && c->stop_bits!=DOS_STOP_ONE_HALF) ||
       (c->stop_bits==DOS_STOP_ONE_HALF && c->data_bits!=5) ||
       (c->stop_bits==DOS_STOP_TWO && c->data_bits==5)) return DE_FUNCTION;
    if(!initial) {int e=await_status(p,DOS_PORT_TX_EMPTY,1); if(e) return e;}
    if(p->firmware) {
        /* DOS parity/stop encodings match IO.SYS; CTS flow stays in PORTDRV. */
        IoSerialConfig s={.size=sizeof(s),.baud=c->baud,.timeout_us=c->timeout_us,.data_bits=c->data_bits,
            .parity=c->parity,.stop_bits=c->stop_bits,
            .control=(c->flags&DOS_PORT_DTR?IO_SERIAL_DTR:0)|(c->flags&DOS_PORT_RTS?IO_SERIAL_RTS:0)|
                (c->flags&DOS_PORT_LOOPBACK?IO_SERIAL_LOOPBACK:0)};
        int e=io->serial_config(io->context,p->token,&s);
        if(e) {if(e!=DE_FUNCTION) p->fault=e; return e;}
        p->config=*c; return 0;
    }
    u8 line=c->data_bits-5;
    if(c->stop_bits!=DOS_STOP_ONE) line|=4;
    if(c->parity) {
        line|=8;
        if(c->parity==DOS_PARITY_EVEN || c->parity==DOS_PARITY_SPACE) line|=0x10;
        if(c->parity==DOS_PARITY_MARK || c->parity==DOS_PARITY_SPACE) line|=0x20;
    }
    u32 divisor=115200/c->baud; u8 modem=(u8)(c->flags&3);
    if(c->flags&DOS_PORT_LOOPBACK) modem|=0x10;
    int e=put(p,LCR,line|0x80);
    if(!e) e=put(p,0,(u8)divisor);
    if(!e) e=put(p,1,(u8)(divisor>>8));
    if(!e) e=put(p,LCR,line);
    if(!e) e=put(p,IER,0);
    if(!e) e=put(p,MCR,modem);
    if(e) {p->fault=e; return e;}
    p->config=*c; return 0;
}
static const DosPortConfig default_serial={sizeof(DosPortConfig),DOS_PORT_ABI,115200,100000,8,DOS_PARITY_NONE,DOS_STOP_ONE,DOS_PORT_DTR|DOS_PORT_RTS};
static int uart_start(Port *p) {
    int e=get(p,SCR,&p->saved[SCR]); if(e) return e;
    p->saved_flags|=SAVE_SCR;
    u8 value;
    e=put(p,SCR,0x5a); if(!e) e=get(p,SCR,&value);
    if(e || value!=0x5a) return e?e:DE_NOTREADY;
    e=put(p,SCR,0xa5); if(!e) e=get(p,SCR,&value);
    if(e || value!=0xa5) return e?e:DE_NOTREADY;
    e=put(p,SCR,p->saved[SCR]); if(e) return e;
    e=get(p,LCR,&p->saved[LCR]); if(e) return e; p->saved_flags|=SAVE_LCR;
    e=put(p,LCR,p->saved[LCR]&0x7f); if(e) return e;
    e=get(p,IER,&p->saved[IER]); if(e) return e; p->saved_flags|=SAVE_IER;
    e=get(p,MCR,&p->saved[MCR]); if(e) return e; p->saved_flags|=SAVE_MCR;
    e=put(p,LCR,p->saved[LCR]|0x80); if(e) return e;
    e=get(p,0,&p->saved[0]); if(!e) e=get(p,1,&p->saved[2]);
    if(e) return e;
    p->saved_flags|=SAVE_DIVISOR;
    e=configure(p,&default_serial,1); if(e) return e;
    p->saved_flags|=CHANGED_FIFO;
    e=put(p,FCR,7); if(!e) e=get(p,FCR,&value); /* IIR on reads. */
    if(e) return e;
    if((value&0xc0)!=0xc0) return DE_NOTREADY;
    /* Timer-sampled reception is optional; without it this stays polled. */
    if(HAS(port_buffer) && HAS(port_pending) && (io->capabilities&IO_CAP_PORT_BUFFER) &&
       !io->port_buffer(io->context,p->token,1)) p->buffered=1;
    return 0;
}
static int firmware_start(Port *p) {
    int e=io->serial_open(io->context,p->unit,&p->token); if(e) {p->token=0; return e;}
    return configure(p,&default_serial,1);
}
static int printer_start(Port *p) {
    int e=get(p,2,&p->saved[2]); if(e) return e; p->saved_flags|=SAVE_CONTROL;
    e=get(p,0,&p->saved[0]); if(e) return e; p->saved_flags|=SAVE_DATA;
    e=put(p,2,0x0c); if(e) return e;
    u8 value; e=get(p,2,&value); if(e || (value&0x3f)!=0x0c) return e?e:DE_NOTREADY;
    e=put(p,0,0x5a); if(!e) e=get(p,0,&value);
    if(e || value!=0x5a) return e?e:DE_NOTREADY;
    e=put(p,0,0xa5); if(!e) e=get(p,0,&value);
    if(e || value!=0xa5) return e?e:DE_NOTREADY;
    e=put(p,0,p->saved[0]); if(e) return e;
    p->config=(DosPortConfig){.size=sizeof(DosPortConfig),.version=DOS_PORT_ABI,.timeout_us=100000};
    return 0;
}
static int finish(Port *p) {
    if(!p->token) return 0;
    int error=0,e;
    if(p->firmware) {
        error=io->serial_close(io->context,p->token); p->token=0; return error;
    }
#define RESTORE(reg,value) do {e=put(p,reg,value); if(e && !error) error=e;} while(0)
    if(p->buffered) {e=io->port_buffer(io->context,p->token,0); if(e && !error) error=e; p->buffered=0;}
    if(p->kind==IO_PORT_UART) {
        if(p->saved_flags&SAVE_LCR) {
            RESTORE(LCR,p->saved[LCR]&0x7f);
            if(p->saved_flags&SAVE_IER) RESTORE(IER,0);
        }
        if(p->saved_flags&CHANGED_FIFO) RESTORE(FCR,0);
        if(p->saved_flags&SAVE_DIVISOR) {
            RESTORE(LCR,p->saved[LCR]|0x80); RESTORE(0,p->saved[0]); RESTORE(1,p->saved[2]);
            RESTORE(LCR,p->saved[LCR]&0x7f);
        }
        if(p->saved_flags&SAVE_MCR) RESTORE(MCR,p->saved[MCR]);
        if(p->saved_flags&SAVE_IER) RESTORE(IER,p->saved[IER]);
        if(p->saved_flags&SAVE_LCR) RESTORE(LCR,p->saved[LCR]);
        if(p->saved_flags&SAVE_SCR) RESTORE(SCR,p->saved[SCR]);
    } else {
        if(p->saved_flags&SAVE_CONTROL) RESTORE(2,0x0c); /* Never assert a saved strobe. */
        if(p->saved_flags&SAVE_DATA) RESTORE(0,p->saved[0]);
        if(p->saved_flags&SAVE_CONTROL) RESTORE(2,p->saved[2]&~1U);
    }
#undef RESTORE
    e=io->port_release(io->context,p->token); if(e && !error) error=e;
    p->token=0; return error;
}
static int clear_input(Port *p) {
    if(p->kind!=IO_PORT_UART) return DE_FUNCTION;
    if(p->firmware) {
        /* Firmware units have no receive reset; discard what is queued now. */
        for(unsigned budget=65536;budget;budget--) {
            u32 bits; u8 byte; u32 got; int e=io->serial_status(io->context,p->token,&bits); if(e) return e;
            if(bits&IO_SERIAL_INPUT_EMPTY) break;
            e=io->serial_read(io->context,p->token,&byte,1,&got); if(e) return e;
        }
    } else {int e=put(p,FCR,3); if(e) return e;}
    p->have_peek=0; p->errors=0; return 0;
}
static int uart_byte(Port *p,int wait,int peek,u8 *out) {
    if(p->have_peek) {*out=p->peek; if(!peek) p->have_peek=0; return 0;}
    int e=await_status(p,DOS_PORT_RX_READY,wait); if(e) return e;
    if(p->firmware) {
        u32 got; e=io->serial_read(io->context,p->token,out,1,&got);
        if(e==DE_IO) p->errors|=0x80;
        if(e) return e;
    } else {e=get(p,RBR,out); if(e) return e;}
    if(peek) {p->have_peek=1; p->peek=*out;} return 0;
}
static int transmit(Port *p,u8 value) {
    if(!p->firmware) return put(p,0,value);
    u32 sent; return io->serial_write(io->context,p->token,&value,1,&sent);
}
static int request(void *context,DosDeviceRequest *r) {
    unsigned index=(unsigned)(uintptr_t)context;
    if(index>=PORT_DEVICE_COUNT) return DE_FUNCTION;
    Port *p=&ports[index];
    if(r->command==DOS_DEV_INIT) {
        if(index==0) {
            memset(ports,0,sizeof(ports)); io=r->io;
            if(!io || io->version!=IO_ABI_VERSION || !r->arguments) return DE_FUNCTION;
            int e=parse(r->arguments); if(e) return e;
            int legacy=0,firmware=0;
            for(unsigned i=0;i<PORT_DEVICE_COUNT;i++) if(ports[i].configured) {if(ports[i].firmware) firmware=1; else legacy=1;}
            if(!HAS(stall_us) || (legacy && (!(io->capabilities&IO_CAP_LEGACY_PORTS) || !io->port_claim ||
               !io->port_release || !io->port_read || !io->port_write)) ||
               (firmware && (!(io->capabilities&IO_CAP_SERIAL) || !HAS(serial_write) || !io->serial_count ||
               !io->serial_open || !io->serial_close || !io->serial_config || !io->serial_status || !io->serial_read))) return DE_FUNCTION;
        }
        p->kind=index<4?IO_PORT_UART:IO_PORT_PRINTER;
        if(!p->configured) return 0;
        if(p->firmware) return firmware_start(p);
        int e=io->port_claim(io->context,p->kind,p->base,&p->token); if(e) return e;
        return p->kind==IO_PORT_UART?uart_start(p):printer_start(p);
    }
    if(r->command==DOS_DEV_FINISH) return finish(p);
    if(r->command==DOS_DEV_CLOSE) return 0;
    if(!p->token) return DE_NOTREADY;
    if(r->command==DOS_DEV_IOCTL_READ) {
        if(r->count<sizeof(DosPortInfo)) return DE_FUNCTION;
        DosPortInfo info; int e=status(p,&info); if(e) return e;
        if(p->buffered && !p->fault) {
            u32 queued; e=io->port_pending(io->context,p->token,&queued); if(e) return e;
            info.buffered+=queued;
        }
        memcpy(r->buffer,&info,sizeof(info)); r->transferred=sizeof(info); return 0;
    }
    if(p->fault) return p->fault;
    switch(r->command) {
    case DOS_DEV_OPEN: return 0;
    case DOS_DEV_IOCTL_WRITE: {
        if(r->count!=sizeof(DosPortConfig)) return DE_FUNCTION;
        DosPortConfig c; memcpy(&c,r->buffer,sizeof(c));
        int e=configure(p,&c,0); if(!e) r->transferred=sizeof(c); return e;
    }
    case DOS_DEV_GENERIC_IOCTL:
        if(r->control!=DOS_PORT_CLEAR_INPUT || r->count || r->argument) return DE_FUNCTION;
        if(!(r->mode&3)) return DE_ACCESS;
        return clear_input(p);
    case DOS_DEV_INPUT_FLUSH: return clear_input(p);
    case DOS_DEV_INPUT_STATUS: case DOS_DEV_OUTPUT_STATUS: {
        DosPortInfo info; int e=status(p,&info); if(e) return e;
        r->ready=!!(info.status&(r->command==DOS_DEV_INPUT_STATUS?DOS_PORT_RX_READY:DOS_PORT_TX_READY)); return 0;
    }
    case DOS_DEV_OUTPUT_FLUSH:
        if(p->kind==IO_PORT_PRINTER && !p->pending_output) return 0;
        return await_status(p,DOS_PORT_TX_EMPTY,1);
    case DOS_DEV_PEEK:
        if(p->kind!=IO_PORT_UART) return DE_ACCESS;
        if(!r->buffer || !r->count) return DE_FUNCTION;
        return uart_byte(p,!!(r->flags&DOS_DEVICE_WAIT),1,r->buffer);
    case DOS_DEV_READ:
        if(p->kind!=IO_PORT_UART) return DE_ACCESS;
        while(r->transferred<r->count) {
            int e=uart_byte(p,!!(r->flags&DOS_DEVICE_WAIT),0,(u8 *)r->buffer+r->transferred);
            if(e) return e;
            r->transferred++;
        }
        return 0;
    case DOS_DEV_WRITE:
        while(r->transferred<r->count) {
            int e=await_status(p,DOS_PORT_TX_READY,!!(r->flags&DOS_DEVICE_WAIT)); if(e) return e;
            e=transmit(p,((const u8 *)r->buffer)[r->transferred]); if(e) return e;
            if(p->kind==IO_PORT_PRINTER) {
                e=delay(1); if(e) return e;
                e=put(p,2,0x0d); if(e) {p->fault=e; return e;}
                r->transferred++; /* Rising strobe has committed this byte. */
                p->pending_output=1;
                e=delay(1); int clear=put(p,2,0x0c);
                if(e || clear) {p->fault=e?e:clear; return p->fault;}
            } else r->transferred++;
        }
        return 0;
    default: return DE_FUNCTION;
    }
}
void port_device_spec(unsigned index,DosDeviceSpec *spec) {
    *spec=(DosDeviceSpec){.version=DOS_DEVICE_ABI,.size=sizeof(*spec),
        .attributes=DOS_DEVICE_CHAR|DOS_DEVICE_IOCTL|DOS_DEVICE_OPEN_CLOSE|DOS_DEVICE_GENERIC,
        .capabilities=DOS_DEVICE_CAN_WRITE|(index<4?DOS_DEVICE_CAN_READ:0),
        .context=(void *)(uintptr_t)index,.request=request};
    strcopy(spec->name,sizeof(spec->name),index<4?"COM1":"LPT1");
    spec->name[3]+=(char)(index<4?index:index-4);
}
