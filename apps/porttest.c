/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "port_device.h"
static unsigned passed,failed;
static void check(const char *name,int ok) {print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;}
static int info(unsigned h,DosPortInfo *out) {
    DosRegs r={.ax=0x4402,.bx=h,.cx=sizeof(*out),.dx=(uintptr_t)out}; return dos_call(&r);
}
static int config(unsigned h,const DosPortConfig *c) {
    DosRegs r={.ax=0x4403,.bx=h,.cx=sizeof(*c),.dx=(uintptr_t)c}; return dos_call(&r);
}
static int commit(unsigned h) {DosRegs r={.ax=0x6800,.bx=h}; return dos_call(&r);}
static int clear(unsigned h) {DosRegs r={.ax=0x440c,.bx=h,.cx=DOS_PORT_CLEAR_INPUT}; return dos_call(&r);}
static EFI_STATUS finish(void) {print("PORTTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed); return failed?1:0;}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *tail=app_dos->command_tail(); unsigned h=0,aux=0; u32 n; u8 buffer[16]; DosRegs r;
    if(!strcmp(tail,"absent")) {
        check("absent-AUX",dos_read(3,buffer,1,&n)==DE_NOTREADY && !n);
        check("absent-PRN",dos_write(4,"X",1,&n)==DE_NOTREADY && !n);
        check("reserve-COM1",dos_open("COM1.TXT",2,1,&h)==DE_NOTREADY);
        check("reserve-COM4",dos_open("COM4",2,1,&h)==DE_NOTREADY);
        check("reserve-LPT3",dos_open("LPT3",1,1,&h)==DE_NOTREADY);
        r=(DosRegs){.ax=0x0400,.dx='X'}; check("absent-legacy-AUX",dos_call(&r)==DE_NOTREADY);
        r=(DosRegs){.ax=0x0500,.dx='X'}; check("absent-legacy-PRN",dos_call(&r)==DE_NOTREADY);
        return finish();
    }
    if(!strcmp(tail,"child")) {
        check("inherited-AUX",!dos_write(3,"CHILD\n",6,&n) && n==6);
        r=(DosRegs){.ax=0x0500,.dx='?'}; check("inherited-PRN",!dos_call(&r)); return finish();
    }
    DosPortInfo serial,printer;
    int e=info(3,&serial); check("standard-AUX-binding",!e && serial.kind==IO_PORT_UART && serial.base==0x2f8);
    if(e) return finish();
    const IoServices *io=app_dos->io;
    if(!strcmp(tail,"")) {
        u64 token=0;
        check("console-port-reserved",io->port_claim(io->context,IO_PORT_UART,0x3f8,&token)==DE_BUSY && !token);
        check("duplicate-port-reserved",io->port_claim(io->context,IO_PORT_UART,0x2f8,&token)==DE_BUSY && !token);
        check("unrelated-port-denied",io->port_claim(io->context,IO_PORT_UART,0x60,&token)==DE_FUNCTION && !token);
        check("claim-unused-range",!io->port_claim(io->context,IO_PORT_UART,0x2e8,&token) && token);
        u8 value;
        check("register-offset-bounded",io->port_read(io->context,token,8,&value)==DE_FUNCTION);
        check("release-range",!io->port_release(io->context,token));
        check("stale-range-token",io->port_read(io->context,token,0,&value)==DE_HANDLE);
    }
    e=info(4,&printer); check("standard-PRN-binding",!e && printer.kind==IO_PORT_PRINTER && printer.base==0x378);
    if(e) return finish();
    check("receive-buffered",!!(serial.status&DOS_PORT_BUFFERED));
    if(!memcmp(tail,"buffer ",7)) {
        /* The host sent N bytes while the shell idled at its prompt. IO.SYS
         * sampled them into its ring (4 KiB); the rest waits in the FIFO. */
        u32 expected=0; for(const char *p=tail+7;*p>='0' && *p<='9';p++) expected=expected*10+(u32)(*p-'0');
        DosPortInfo now; e=info(3,&now); u32 first=e?0:now.buffered,waited=0;
        print("PORTTEST: %u bytes buffered before any DOS read\n",(unsigned long long)first);
        /* One status query can move at most 64 bytes itself. */
        check("idle-sampling",!e && first>64 && first<=MIN(expected,4096U) && (now.status&DOS_PORT_RX_READY));
        while(!(e=info(3,&now)) && now.buffered<MIN(expected,4096U) && waited<20000) {io->stall_us(io->context,1000); waited++;}
        check("ring-filled",!e && now.buffered==MIN(expected,4096U));
        DosPortConfig c=serial.config; c.timeout_us=expected>4096?1000000:0;
        check("read-timeout",!config(3,&c));
        int ordered=1; u32 total=0;
        while(total<expected && ordered) {
            u32 want=MIN(expected-total,sizeof(buffer));
            if(dos_read(3,buffer,want,&n) || n!=want) {ordered=0; break;}
            for(u32 i=0;i<n;i++) if(buffer[i]!=(u8)((total+i)*7+1)) ordered=0;
            total+=n;
        }
        check("buffered-order",ordered && total==expected);
        check("buffer-drained",!info(3,&now) && !now.buffered && !now.receive_errors && !(now.status&DOS_PORT_RX_READY));
        check("restore-timeout",!config(3,&serial.config)); return finish();
    }
    if(!strcmp(tail,"receive")) {
        DosPortConfig c=serial.config; c.timeout_us=1000000;
        check("receive-timeout",!config(3,&c));
        con_puts("PORTTEST: receive-ready\n");
        static const u8 expected[]={0,3,0x80,0xff,'R'};
        check("external-binary-receive",!dos_read(3,buffer,sizeof(expected),&n) && n==sizeof(expected) && !memcmp(buffer,expected,n));
        r=(DosRegs){.ax=0x0300}; check("legacy-AUX-receive",!dos_call(&r) && r.ax=='Z');
        check("restore-timeout",!config(3,&serial.config)); return finish();
    }
    check("open-COM-alias",!dos_open("c:\\COM1.txt",2,0,&h));
    check("open-AUX-alias",!dos_open("aux:",2,0,&aux));
    unsigned absent; check("missing-COM2",dos_open("COM2",2,0,&absent)==DE_NOTREADY);
    check("alias-info",!info(h,&printer) && printer.base==serial.base);
    DosPortConfig c=serial.config; c.flags|=DOS_PORT_LOOPBACK; c.timeout_us=1000;
    check("configure-loopback",!config(h,&c));
    check("shared-configuration",!info(3,&printer) && printer.config.flags==c.flags);
    check("clear-input",!clear(h));
    check("loop-write",!dos_write(h,"AB",2,&n) && n==2);
    check("loop-transmitter-empty",!commit(h));
    unsigned saved=0; check("save-stdin",!dos_dup(0,&saved)); check("redirect-stdin",!dos_dup2(h,0));
    r=(DosRegs){.ax=0x0b00}; check("serial-peek",!dos_call(&r) && r.ax==255);
    r=(DosRegs){.ax=0x0b00}; check("serial-peek-retained",!dos_call(&r) && r.ax==255);
    check("restore-stdin",!dos_dup2(saved,0) && !dos_close(saved));
    check("partial-timeout",dos_read(h,buffer,3,&n)==DE_NOTREADY && n==2 && !memcmp(buffer,"AB",2));
    DosExtendedError error; check("device-error-locus",!dos_extended_error(&error) && error.locus==DOS_LOCUS_DEVICE);
    check("empty-timeout",dos_read(h,buffer,1,&n)==DE_NOTREADY && !n);
    c.data_bits=5; c.stop_bits=DOS_STOP_TWO; check("invalid-framing",config(h,&c)==DE_FUNCTION);
    c=serial.config; c.timeout_us=1000001; check("bounded-timeout",config(h,&c)==DE_FUNCTION);
    check("restore-serial",!config(h,&serial.config));
    static const u8 binary[]={0,'A',0x80,0xff};
    check("external-transmit",!dos_write(h,"PORT-TX\n",8,&n) && n==8);
    check("binary-transmit",!dos_write(h,binary,sizeof(binary),&n) && n==sizeof(binary));
    r=(DosRegs){.ax=0x0400,.dx='#'}; check("legacy-AUX-output",!dos_call(&r));
    check("printer-output",!dos_write(4,"PRINT\0\n",7,&n) && n==7);
    r=(DosRegs){.ax=0x0500,.dx='!'}; check("legacy-printer-output",!dos_call(&r));
    check("child-EXEC",!dos_exec("C:\\PORTTEST.EFI","child"));
    check("device-flush",!dos_flush());
    check("close-COM",!dos_close(h)); check("close-AUX",!dos_close(aux));
    return finish();
}
