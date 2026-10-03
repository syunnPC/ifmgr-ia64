/* SPDX-License-Identifier: GPL-2.0-or-later
 * Explicit PC legacy peripheral resources on a single EFI PCI root bridge.
 * This backend never scans ports. The firmware's primary UART is reserved.
 * Buffered UART claims are sampled by a TPL_NOTIFY timer event; Boot Services
 * own interrupt routing, so no UART IRQ is ever enabled or delivered here.
 */
#include "efi_ports.h"
#define RING_BYTES 4096U
#define SAMPLE_100NS 10000U /* 1 ms; firmware timer granularity may be coarser. */
typedef struct {u64 token; u32 base,span,kind; int buffered,dlab,failed; u32 errors,head,used; u8 *ring;} Claim;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
static EFI_BOOT_SERVICES *boot;
static Claim claims[8];
static u32 console_bases[64],console_count;
static u64 next_token=1;
static EFI_EVENT sampler;
static int sampling,can_buffer;
static Claim *claim_for(u64 token) {
    for(unsigned i=0;i<ARRAY_SIZE(claims);i++) if(token && claims[i].token==token) return &claims[i];
    return NULL;
}
static int hw_read(const Claim *c,u32 offset,u8 *value) {
    return efi_dos_error(root->Io.Read(root,EfiPciIoWidthUint8,c->base+offset,1,value));
}
static int hw_write(const Claim *c,u32 offset,u8 value) {
    return efi_dos_error(root->Io.Write(root,EfiPciIoWidthUint8,c->base+offset,1,&value));
}
/* Move FIFO bytes into the ring with TPL_NOTIFY held. A full ring leaves data
 * in the UART FIFO, where the hardware reports any later overrun itself. */
static int drain(Claim *c,u8 *line) {
    u8 status=0; int e=0;
    for(unsigned budget=64;budget;budget--) {
        e=hw_read(c,5,&status); if(e) break;
        c->errors|=status&0x1e;
        if(!(status&1) || c->used==RING_BYTES) break;
        u8 value; e=hw_read(c,0,&value); if(e) break;
        c->ring[(c->head+c->used++)%RING_BYTES]=value;
    }
    if(line) *line=status;
    return e;
}
static void EFIAPI sample(EFI_EVENT event,void *context) {
    (void)event; (void)context;
    for(unsigned i=0;i<ARRAY_SIZE(claims);i++) {
        Claim *c=&claims[i];
        if(c->token && c->buffered && !c->dlab && !c->failed) c->failed=drain(c,NULL);
    }
}
static int update_sampler(void) {
    int wanted=0;
    for(unsigned i=0;i<ARRAY_SIZE(claims);i++) if(claims[i].token && claims[i].buffered) wanted=1;
    if(wanted==sampling) return 0;
    if(wanted && !sampler) {
        EFI_STATUS e=boot->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_NOTIFY,sample,NULL,&sampler);
        if(EFI_ERROR(e)) {sampler=NULL; return efi_dos_error(e);}
    }
    EFI_STATUS e=boot->SetTimer(sampler,wanted?TimerPeriodic:TimerCancel,wanted?SAMPLE_100NS:0);
    if(EFI_ERROR(e)) return efi_dos_error(e);
    sampling=wanted; return 0;
}
static void stop_buffer(Claim *c) {
    EFI_TPL old=boot->RaiseTPL(TPL_NOTIFY);
    u8 *ring=c->ring; c->ring=NULL;
    c->buffered=0; c->head=c->used=c->errors=0; c->failed=0;
    boot->RestoreTPL(old);
    if(ring) boot->FreePool(ring);
}
static int claim(void *context,u32 kind,u32 base,u64 *token) {
    (void)context;
    if(!token) return DE_FUNCTION;
    *token=0;
    if(!root) return DE_FUNCTION;
    /* The VPC's firmware console owns 3F8, including its MMIO alias. Do not
     * permit a raw driver to claim it even when the selected console is VGA. */
    if(kind==IO_PORT_UART && base==0x3f8) return DE_BUSY;
    if((kind==IO_PORT_UART && base!=0x2f8 && base!=0x3e8 && base!=0x2e8) ||
       (kind==IO_PORT_PRINTER && base!=0x378 && base!=0x278) ||
       (kind!=IO_PORT_UART && kind!=IO_PORT_PRINTER)) return DE_FUNCTION;
    u32 span=kind==IO_PORT_UART?8:3;
    for(unsigned i=0;i<console_count;i++)
        if(base<console_bases[i]+8 && console_bases[i]<base+span) return DE_BUSY;
    unsigned free_slot=ARRAY_SIZE(claims);
    for(unsigned i=0;i<ARRAY_SIZE(claims);i++) {
        Claim *c=&claims[i];
        if(!c->token) {if(free_slot==ARRAY_SIZE(claims)) free_slot=i;}
        else if(base<c->base+c->span && c->base<base+span) return DE_BUSY;
    }
    if(free_slot==ARRAY_SIZE(claims) || !next_token) return DE_NOMEM;
    claims[free_slot]=(Claim){.token=next_token++,.base=base,.span=span,.kind=kind};
    *token=claims[free_slot].token; return 0;
}
static int release(void *context,u64 token) {
    (void)context; Claim *c=claim_for(token); if(!c) return DE_HANDLE;
    int e=0;
    if(c->buffered) {stop_buffer(c); c->token=0; e=update_sampler();}
    memset(c,0,sizeof(*c)); return e;
}
static int read_port(void *context,u64 token,u32 offset,u8 *value) {
    (void)context; Claim *c=claim_for(token);
    if(!c) return DE_HANDLE;
    if(!value || offset>=c->span) return DE_FUNCTION;
    if(!c->buffered) return hw_read(c,offset,value);
    EFI_TPL old=boot->RaiseTPL(TPL_NOTIFY);
    int e=c->failed; c->failed=0;
    if(!e && offset==5) {
        u8 line; e=c->dlab?hw_read(c,5,&line):drain(c,&line);
        if(!e) {
            c->errors|=line&0x1e;
            *value=(line&0xe0)|(c->used || (line&1)?1:0)|c->errors; c->errors=0;
        }
    } else if(!e && offset==0 && !c->dlab) {
        e=drain(c,NULL);
        if(!e && c->used) {*value=c->ring[c->head]; c->head=(c->head+1)%RING_BYTES; c->used--;}
        else if(!e) e=hw_read(c,0,value);
    } else if(!e) e=hw_read(c,offset,value);
    boot->RestoreTPL(old); return e;
}
static int write_port(void *context,u64 token,u32 offset,u8 value) {
    (void)context; Claim *c=claim_for(token);
    if(!c) return DE_HANDLE;
    if(offset>=c->span) return DE_FUNCTION;
    if(!c->buffered) return hw_write(c,offset,value);
    EFI_TPL old=boot->RaiseTPL(TPL_NOTIFY);
    int e=hw_write(c,offset,value);
    if(!e && offset==3) c->dlab=!!(value&0x80);
    if(!e && offset==2 && (value&2)) c->head=c->used=c->errors=0;
    boot->RestoreTPL(old); return e;
}
static int buffer(void *context,u64 token,u32 enable) {
    (void)context; Claim *c=claim_for(token);
    if(!c) return DE_HANDLE;
    if(c->kind!=IO_PORT_UART || !can_buffer) return DE_FUNCTION;
    if(!!enable==c->buffered) return 0;
    if(!enable) {stop_buffer(c); return update_sampler();}
    u8 *ring=NULL; EFI_STATUS s=boot->AllocatePool(EfiLoaderData,RING_BYTES,(void **)&ring);
    if(EFI_ERROR(s) || !ring) return EFI_ERROR(s)?efi_dos_error(s):DE_NOMEM;
    EFI_TPL old=boot->RaiseTPL(TPL_NOTIFY);
    u8 lcr; int e=hw_read(c,3,&lcr);
    if(!e) {c->dlab=!!(lcr&0x80); c->head=c->used=c->errors=0; c->failed=0; c->ring=ring; c->buffered=1;}
    boot->RestoreTPL(old);
    if(e) boot->FreePool(ring);
    if(!e) {e=update_sampler(); if(e) stop_buffer(c);}
    return e;
}
static int pending(void *context,u64 token,u32 *bytes) {
    (void)context; Claim *c=claim_for(token);
    if(!c) return DE_HANDLE;
    if(!bytes) return DE_FUNCTION;
    *bytes=0;
    if(!c->buffered) return DE_FUNCTION;
    EFI_TPL old=boot->RaiseTPL(TPL_NOTIFY);
    int e=c->failed; c->failed=0;
    if(!e && !c->dlab) e=drain(c,NULL);
    *bytes=c->used; boot->RestoreTPL(old); return e;
}
static int stall(void *context,u32 us) {
    (void)context;
    if(us>1000000) return DE_FUNCTION;
    return us?efi_dos_error(boot->Stall(us)):0;
}
static int console_resources(EFI_SYSTEM_TABLE *st) {
    EFI_GUID hcdp={0xf951938d,0x620b,0x42ef,{0x82,0x79,0xa8,0x4b,0x79,0x61,0x78,0x98}};
    for(UINTN i=0;i<st->NumberOfTableEntries;i++) {
        EFI_CONFIGURATION_TABLE *t=&st->ConfigurationTable[i];
        if(memcmp(&t->VendorGuid,&hcdp,sizeof(hcdp))) continue;
        const u8 *p=t->VendorTable;
        if(!p || memcmp(p,"HCDP",4)) return DE_FORMAT;
        u32 length=rd32(p+4);
        if(length<40 || length>65536) return DE_FORMAT;
        u8 sum=0; for(u32 j=0;j<length;j++) sum+=p[j];
        u32 count=rd32(p+36);
        if(sum || count>(length-40)/48 || count>ARRAY_SIZE(console_bases)-console_count) return DE_FORMAT;
        for(u32 j=0;j<count;j++) {
            const u8 *entry=p+40+j*48;
            /* HCDP UART descriptors contain a GAS at byte 16. Reserve all
             * advertised system-I/O UARTs, including debug/secondary consoles. */
            if(entry[16]==1 && !rd32(entry+24) && rd32(entry+20)<=65528)
                console_bases[console_count++]=rd32(entry+20);
        }
    }
    return 0;
}
void efi_ports_init(EFI_SYSTEM_TABLE *st,IoServices *io) {
    boot=st->BootServices; root=NULL; console_count=0; sampler=NULL; sampling=can_buffer=0;
    memset(claims,0,sizeof(claims));
    EFI_GUID guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles=NULL; UINTN count=0;
    if(!EFI_ERROR(boot->LocateHandleBuffer(ByProtocol,&guid,NULL,&count,&handles))) {
        /* Ambiguous roots need a richer resource description, never a guess. */
        if(count==1 && !console_resources(st)) {
            EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *p=NULL;
            if(!EFI_ERROR(boot->HandleProtocol(handles[0],&guid,(void **)&p)) && p &&
               !p->SegmentNumber && p->Io.Read && p->Io.Write) root=p;
        }
        boot->FreePool(handles);
    }
    io->port_claim=claim; io->port_release=release;
    io->port_read=read_port; io->port_write=write_port; io->stall_us=stall;
    io->port_buffer=buffer; io->port_pending=pending;
    if(root) io->capabilities|=IO_CAP_LEGACY_PORTS;
    can_buffer=root && boot->CreateEvent && boot->SetTimer && boot->CloseEvent && boot->RaiseTPL && boot->RestoreTPL &&
        boot->AllocatePool && boot->FreePool;
    if(can_buffer) io->capabilities|=IO_CAP_PORT_BUFFER;
}
void efi_ports_close(void) {
    for(unsigned i=0;i<ARRAY_SIZE(claims);i++) if(claims[i].token && claims[i].buffered) stop_buffer(&claims[i]);
    if(sampler) {boot->SetTimer(sampler,TimerCancel,0); boot->CloseEvent(sampler);}
    sampler=NULL; sampling=0;
}
