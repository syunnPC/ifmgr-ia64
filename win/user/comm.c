/* SPDX-License-Identifier: GPL-2.0-or-later
 * Communications: Windows 3.0's COM and LPT functions over DOS's port
 * devices (PORTDRV.SYS), each port a DOS handle in the opening task's DOS
 * context. IOCTL 4402h reads a port's status and the bytes waiting, 4403h
 * sets its line (speed, data bits, parity, stop bits, DTR, RTS, CTS flow).
 * Reads take only what is waiting, so they never block; a write waits a
 * moment for each byte. Ids are 0 to 8 for COM1 to COM9 and LPTx (0x80)
 * plus 0 to 2 for LPT1 to LPT3. The event word is brought up to date when
 * the program asks for it.
 */
#include "user.h"
#define COMS 9
#define LPTS 3
#define PORT_ABI 1
#define PORT_DTR 1
#define PORT_RTS 2
#define PORT_CTS_FLOW 4
#define PORT_RX_READY 1
#define PORT_TX_EMPTY 4
#define PORT_CTS 8
#define PORT_DSR 16
#define PORT_CARRIER 32
#define PORT_ERROR 256
#define PORT_FAULTED 512
#define PORT_CLEAR_INPUT 0x8000
#define WRITE_TIMEOUT_US 200000
typedef struct {DWORD size,version,baud,timeout_us,data_bits,parity,stop_bits,flags;} PortConfig;
typedef struct {DWORD size,kind,base,status,line_status,modem_status,receive_errors,buffered; PortConfig config;} PortInfo;
typedef struct {
    BOOL used; HFILE h; HTASK task; DCB dcb; PortConfig config;
    UINT events,mask; DWORD last_status; BOOL unget; char unget_char;
} Port;
static Port ports[COMS+LPTS];

static Port *port_of(int id) {
    int i=id&LPTx?COMS+(id&~LPTx):id;
    if(id<0 || (id&LPTx && (id&~LPTx)>=LPTS) || (!(id&LPTx) && id>=COMS) || !ports[i].used) return NULL;
    return &ports[i];
}
static BOOL ioctl(Port *p,int function,void *packet,DWORD size) {
    WhRegs r; memset(&r,0,sizeof(r));
    r.ax=0x4400|function; r.bx=(wh_u64)p->h; r.cx=size; r.dx=(wh_u64)(ULONG_PTR)packet;
    wh_int21(&r);
    return !(r.flags&1);
}
static BOOL port_info(Port *p,PortInfo *info) {
    memset(info,0,sizeof(*info)); info->size=sizeof(*info);
    return ioctl(p,2,info,sizeof(*info));
}
static BOOL configure(Port *p) {
    PortConfig c=p->config;
    c.size=sizeof(c); c.version=PORT_ABI;
    return ioctl(p,3,&c,sizeof(c));
}
/* The DCB's line as the port driver's configuration. */
static void dcb_to_config(const DCB *d,PortConfig *c) {
    c->baud=d->BaudRate; c->data_bits=d->ByteSize; c->parity=d->Parity;
    c->stop_bits=d->StopBits==TWOSTOPBITS?2:d->StopBits==ONE5STOPBITS?15:1;
    c->flags=(d->fDtrDisable?0:PORT_DTR)|(d->fRtsDisable?0:PORT_RTS)|(d->fOutxCtsFlow?PORT_CTS_FLOW:0);
    c->timeout_us=WRITE_TIMEOUT_US;
}

int WINAPI OpenComm(LPCSTR name,UINT in_size,UINT out_size) {
    char device[8]; int n,id,i; Port *p; PortInfo info;
    (void)in_size; (void)out_size;
    if(!name) return IE_BADID;
    for(n=0;name[n] && name[n]!=':' && n<5;n++) device[n]=(char)(name[n]>='a' && name[n]<='z'?name[n]-0x20:name[n]);
    device[n]=0;
    if(n==4 && !memcmp(device,"COM",3) && device[3]>='1' && device[3]<='9') {id=device[3]-'1'; i=id;}
    else if(n==4 && !memcmp(device,"LPT",3) && device[3]>='1' && device[3]<'1'+LPTS) {id=LPTx|(device[3]-'1'); i=COMS+(device[3]-'1');}
    else return IE_BADID;
    p=&ports[i];
    if(p->used) return IE_OPEN;
    memset(p,0,sizeof(*p));
    if((p->h=_lopen(device,OF_READWRITE))==HFILE_ERROR) return IE_HARDWARE;
    p->used=TRUE; p->task=GetCurrentTask();
    if(!port_info(p,&info)) {_lclose(p->h); p->used=FALSE; return IE_HARDWARE;}
    p->config=info.config;
    /* The DCB from the line as it is set. */
    p->dcb.Id=(BYTE)id; p->dcb.BaudRate=info.config.baud?info.config.baud:CBR_9600;
    p->dcb.ByteSize=(BYTE)(info.config.data_bits?info.config.data_bits:8); p->dcb.Parity=(BYTE)info.config.parity;
    p->dcb.StopBits=(BYTE)(info.config.stop_bits==2?TWOSTOPBITS:info.config.stop_bits==15?ONE5STOPBITS:ONESTOPBIT);
    p->dcb.fBinary=1; p->dcb.fDtrDisable=!(info.config.flags&PORT_DTR); p->dcb.fRtsDisable=!(info.config.flags&PORT_RTS);
    p->dcb.fOutxCtsFlow=(info.config.flags&PORT_CTS_FLOW)!=0;
    p->dcb.XonChar=0x11; p->dcb.XoffChar=0x13; p->dcb.EofChar=0x1a;
    if(!(id&LPTx)) {p->config.timeout_us=WRITE_TIMEOUT_US; configure(p);}
    p->last_status=info.status;
    return id;
}
int WINAPI CloseComm(int id) {
    Port *p=port_of(id);
    if(!p) return -1;
    _lclose(p->h); p->used=FALSE;
    return 0;
}
/* What is waiting, never more: a byte at a time when the driver does not count. */
int WINAPI ReadComm(int id,void FAR *buffer,int size) {
    Port *p=port_of(id); BYTE *out=(BYTE *)buffer; int n=0;
    if(!p || size<0 || !out) return -1;
    if(p->unget && size) {out[n++]=(BYTE)p->unget_char; p->unget=FALSE;}
    while(n<size) {
        PortInfo info; UINT want,got;
        if(!port_info(p,&info) || !(info.status&PORT_RX_READY)) break;
        want=info.buffered?min((UINT)(size-n),info.buffered):1;
        got=_lread(p->h,out+n,want);
        if(got==(UINT)HFILE_ERROR || !got) break;
        n+=(int)got;
        if(got<want) break;
    }
    return n;
}
int WINAPI WriteComm(int id,const void FAR *buffer,int size) {
    Port *p=port_of(id); UINT done;
    if(!p || size<0) return -1;
    if(!size) return 0;
    done=_lwrite(p->h,buffer,(UINT)size);
    return done==(UINT)HFILE_ERROR?-1:(int)done;
}
int WINAPI TransmitCommChar(int id,char c) {return WriteComm(id,&c,1)==1?0:-1;}
int WINAPI UngetCommChar(int id,char c) {
    Port *p=port_of(id);
    if(!p || p->unget) return -1;
    p->unget=TRUE; p->unget_char=c; return 0;
}
int WINAPI GetCommState(int id,DCB FAR *out) {
    Port *p=port_of(id);
    if(!p || !out) return -1;
    *out=p->dcb; return 0;
}
int WINAPI SetCommState(const DCB FAR *d) {
    Port *p=d?port_of(d->Id):NULL;
    if(!p) return IE_BADID;
    if(d->ByteSize<5 || d->ByteSize>8) return IE_BYTESIZE;
    if(!d->BaudRate || d->BaudRate>115200 || 115200%d->BaudRate) return IE_BAUDRATE;
    if(!(d->Id&LPTx)) {
        PortConfig old=p->config;
        dcb_to_config(d,&p->config);
        if(!configure(p)) {p->config=old; return IE_DEFAULT;}
    }
    p->dcb=*d; return 0;
}
/* The line's errors (cleared by reading them), and what is waiting. */
int WINAPI GetCommError(int id,COMSTAT FAR *stat) {
    Port *p=port_of(id); PortInfo info; int errors=0;
    if(!p) return -1;
    if(!port_info(p,&info)) errors=CE_IOE;
    else {
        if(info.status&PORT_FAULTED) errors|=CE_IOE;
        if(info.receive_errors&2) errors|=CE_OVERRUN;
        if(info.receive_errors&4) errors|=CE_RXPARITY;
        if(info.receive_errors&8) errors|=CE_FRAME;
        if(info.receive_errors&16) errors|=CE_BREAK;
        if((info.receive_errors&0x80) && !(info.receive_errors&0x1e)) errors|=CE_FRAME;
    }
    if(stat) {
        stat->status=0;
        stat->cbInQue=(info.buffered?info.buffered:(info.status&PORT_RX_READY)?1:0)+(p->unget?1:0);
        stat->cbOutQue=0;
    }
    return errors;
}
int WINAPI FlushComm(int id,int queue) {
    Port *p=port_of(id);
    if(!p) return -1;
    if(queue==1) {
        WhRegs r; memset(&r,0,sizeof(r));
        p->unget=FALSE;
        r.ax=0x440c; r.bx=(wh_u64)p->h; r.cx=PORT_CLEAR_INPUT; wh_int21(&r);
    }
    return 0;
}
LONG WINAPI EscapeCommFunction(int id,int function) {
    Port *p=port_of(id);
    if(!p) return -1;
    switch(function) {
    case SETDTR: p->config.flags|=PORT_DTR; break;
    case CLRDTR: p->config.flags&=~PORT_DTR; break;
    case SETRTS: p->config.flags|=PORT_RTS; break;
    case CLRRTS: p->config.flags&=~PORT_RTS; break;
    case SETXON: case SETXOFF: case RESETDEV: return 0;
    default: return -1;
    }
    p->dcb.fDtrDisable=!(p->config.flags&PORT_DTR); p->dcb.fRtsDisable=!(p->config.flags&PORT_RTS);
    return configure(p)?0:-1;
}
int WINAPI SetCommBreak(int id) {return port_of(id)?0:-1;}
int WINAPI ClearCommBreak(int id) {return port_of(id)?0:-1;}
/* The events since the program last cleared them, found from the port's status. */
static void update_events(Port *p) {
    PortInfo info; DWORD changed;
    if(!port_info(p,&info)) return;
    changed=info.status^p->last_status;
    if(info.status&PORT_RX_READY) p->events|=EV_RXCHAR;
    if(info.status&PORT_TX_EMPTY) p->events|=EV_TXEMPTY;
    if(changed&PORT_CTS) p->events|=EV_CTS;
    if(changed&PORT_DSR) p->events|=EV_DSR;
    if(changed&PORT_CARRIER) p->events|=EV_RLSD;
    if(info.status&PORT_ERROR) p->events|=EV_ERR;
    if(info.modem_status&0x40) p->events|=EV_RING;
    p->last_status=info.status;
    p->events&=p->mask;
}
UINT FAR * WINAPI SetCommEventMask(int id,UINT mask) {
    Port *p=port_of(id);
    if(!p) return NULL;
    p->mask=mask; p->events=0;
    return &p->events;
}
UINT WINAPI GetCommEventMask(int id,int clear) {
    Port *p=port_of(id); UINT events;
    if(!p) return 0;
    update_events(p);
    events=p->events; p->events&=~(UINT)clear;
    return events;
}
/* "COM1:9600,n,8,1" (MODE's form; a trailing p or x picks the flow control). */
int WINAPI BuildCommDCB(LPCSTR text,DCB FAR *d) {
    char device[8]; int n=0,field=0; LPCSTR s=text; DWORD value;
    if(!text || !d) return -1;
    while(*s==' ') s++;
    for(;*s && *s!=':' && n<5;s++) device[n++]=(char)(*s>='a' && *s<='z'?*s-0x20:*s);
    device[n]=0;
    if(n!=4 || *s!=':' || device[3]<'1' || device[3]>'9') return IE_BADID;
    memset(d,0,sizeof(*d));
    if(!memcmp(device,"COM",3)) d->Id=(BYTE)(device[3]-'1');
    else if(!memcmp(device,"LPT",3) && device[3]<'1'+LPTS) d->Id=(BYTE)(LPTx|(device[3]-'1'));
    else return IE_BADID;
    d->BaudRate=CBR_9600; d->ByteSize=8; d->Parity=NOPARITY; d->StopBits=ONESTOPBIT; d->fBinary=1;
    d->XonChar=0x11; d->XoffChar=0x13; d->EofChar=0x1a;
    for(s++;*s;field++) {
        while(*s==' ') s++;
        if(*s>='0' && *s<='9') {
            for(value=0;*s>='0' && *s<='9';s++) value=value*10+(DWORD)(*s-'0');
            if(*s=='.' && field==3) {s++; while(*s>='0' && *s<='9') s++; d->StopBits=ONE5STOPBITS;}
            else if(field==0) d->BaudRate=value<100?(value==11?110:value==15?150:value==30?300:value==60?600:value==12?1200:
                                                   value==24?2400:value==48?4800:value==96?9600:value==19?19200:value):value;
            else if(field==2) d->ByteSize=(BYTE)value;
            else if(field==3) d->StopBits=(BYTE)(value==2?TWOSTOPBITS:ONESTOPBIT);
        } else if(field==1) {
            switch(*s|0x20) {
            case 'n': d->Parity=NOPARITY; break;
            case 'o': d->Parity=ODDPARITY; break;
            case 'e': d->Parity=EVENPARITY; break;
            case 'm': d->Parity=MARKPARITY; break;
            case 's': d->Parity=SPACEPARITY; break;
            default: return -1;
            }
            s++;
        } else if(*s && *s!=',') {
            if((*s|0x20)=='p') d->fOutxCtsFlow=1;
            else if((*s|0x20)=='x') {d->fOutX=1; d->fInX=1;}
            s++;
        }
        while(*s==' ') s++;
        if(*s==',') s++;
        else if(*s) return -1;
    }
    return 0;
}
/* A task's ports close when it ends. */
void CommTaskEnded(HTASK task) {
    int i;
    for(i=0;i<COMS+LPTS;i++) if(ports[i].used && ports[i].task==task) {ports[i].used=FALSE;}
}
