/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * MODE.COM: CON code pages and, with ANSI.SYS, columns and lines; COMn line
 * settings and LPTn printer setup.
 * Syntax and messages follow MS-DOS 4 CMD/MODE (MIT); the code is new.
 */
#include "runtime.h"
#include "port_device.h"
static int failed;
static const char parities[]="NOEMS";
static void fail(const char *text) {print("%s\n",text); failed=1;}
static char *skip(char *p) {while(*p==' ' || *p=='\t') p++; return p;}
/* Match a keyword or its abbreviation, ending at a delimiter or '='. */
static int word(char **cursor,const char *name,const char *abbreviation) {
    const char *names[2]={name,abbreviation};
    for(unsigned i=0;i<2;i++) {
        const char *n=names[i]; if(!n) continue;
        char *p=*cursor; while(*n && upper(*p)==*n) {p++; n++;}
        if(!*n && (!*p || *p==' ' || *p=='\t' || *p=='=' || *p==',' || *p==':' || *p=='/')) {*cursor=p; return 1;}
    }
    return 0;
}
static int number(char **cursor,u32 *out) {
    char *p=*cursor; u32 value=0; unsigned digits=0;
    while(*p>='0' && *p<='9') {if(value>99999999) return 0; value=value*10+(*p++-'0'); digits++;}
    if(!digits) return 0;
    *out=value; *cursor=p; return 1;
}
static int status_switch(char *p) {p=skip(p); return !*p || ((word(&p,"/STATUS","/STA")) && !*skip(p));}
static int ioctl(unsigned h,unsigned function,void *packet,u32 size,u32 *stored) {
    DosRegs r={.ax=0x440c,.bx=h,.cx=(DOS_CP_CATEGORY<<8)|function,.dx=(uintptr_t)packet,.si=size};
    int e=dos_call(&r); if(!e && stored) *stored=r.ax; return e;
}
static int open_device(const char *name,unsigned mode,unsigned *h) {
    int e=dos_open(name,mode,0,h); if(e) {print("Failure to access device: %s\n",name); failed=1;}
    return e;
}
static void cp_status(void) {
    unsigned h; if(open_device("CON",2,&h)) return;
    u8 packet[64]; u32 got;
    int e=ioctl(h,DOS_CP_QUERY,packet,sizeof(packet),&got);
    if(e==DOS_CP_NOT_PREPARED) fail("No code page has been selected");
    else if(e) fail("Code page operation not supported on this device");
    else print("Active code page for device CON is %u\n",(unsigned long long)rd16(packet+2));
    if(!e) e=ioctl(h,DOS_CP_QUERY_LIST,packet,sizeof(packet),&got);
    if(e) {if(!failed) fail("Device error during status");}
    else {
        unsigned hardware=rd16(packet+2),at=4;
        print("Hardware code pages:\n");
        for(unsigned i=0;i<hardware && at+2<=got;i++,at+=2) print("  code page %u\n",(unsigned long long)rd16(packet+at));
        unsigned count=at+2<=got?rd16(packet+at):0; at+=2;
        print("Prepared code pages:\n");
        for(unsigned i=0;i<count && at+2<=got;i++,at+=2) {
            u16 page=rd16(packet+at);
            if(page==0xffff) print("  code page not prepared\n");
            else print("  code page %u\n",(unsigned long long)page);
        }
        print("MODE status code page function completed\n");
    }
    dos_close(h);
}
static void cp_prepare(char *p) {
    u16 pages[DOS_CP_PREPARED_MAX]; unsigned count=0;
    p=skip(p); if(*p++!='(' || *(p=skip(p))!='(') {fail("Invalid parameter"); return;}
    p++;
    for(;;) {
        p=skip(p); u32 page;
        if(*p==',' || *p==')') page=0xffff; /* An empty position keeps that slot. */
        else if(!number(&p,&page) || !page || page>=0xffff) {fail("Invalid parameter"); return;}
        if(count==DOS_CP_PREPARED_MAX) {fail("Invalid number of parameters"); return;}
        pages[count++]=(u16)page; p=skip(p);
        if(*p==')') break;
        if(*p++!=',') {fail("Invalid parameter"); return;}
    }
    p=skip(p+1); char path[DOS_PATH_MAX]; unsigned n=0;
    while(*p && *p!=')' && *p!=' ' && *p!='\t' && n+1<sizeof(path)) path[n++]=*p++;
    path[n]=0; p=skip(p);
    if(!n || *p++!=')' || *skip(p)) {fail("Invalid parameter"); return;}
    unsigned file,h;
    if(dos_open(path,0,0,&file)) {fail("Failure to access code page font file"); return;}
    if(open_device("CON",2,&h)) {dos_close(file); return;}
    u8 packet[6+2*DOS_CP_PREPARED_MAX]; wr16(packet,0); wr16(packet+2,2+2*count); wr16(packet+4,count);
    for(unsigned i=0;i<count;i++) wr16(packet+6+2*i,pages[i]);
    int e=ioctl(h,DOS_CP_PREPARE_START,packet,6+2*count,NULL);
    static u8 buffer[4096];
    while(!e) {
        u32 got,written; int x=dos_read(file,buffer,sizeof(buffer),&got);
        if(x) {fail("Error during read of font file"); e=x; break;}
        if(!got) break;
        DosRegs r={.ax=0x4403,.bx=h,.cx=got,.dx=(uintptr_t)buffer};
        e=dos_call(&r); written=r.ax;
        if(!e && written!=got) e=DOS_CP_DEVICE_ERROR;
    }
    /* Always end the prepare so the device discards staged data on errors. */
    int end=ioctl(h,DOS_CP_PREPARE_END,NULL,0,NULL); if(!e) e=end;
    if(!failed) {
        if(!e) print("MODE prepare code page function completed\n");
        else if(e==DOS_CP_NOT_IN_FILE) fail("Device or code page missing from font file");
        else if(e==DOS_CP_BAD_FILE) fail("Font file contents invalid");
        else if(e==DE_FUNCTION) fail("Code page operation not supported on this device");
        else fail("Device error during prepare");
    }
    dos_close(h); dos_close(file);
}
static void cp_select(char *p) {
    u32 page; p=skip(p);
    if(!number(&p,&page) || !page || page>=0xffff || *skip(p)) {fail("Invalid parameter"); return;}
    unsigned h; if(open_device("CON",2,&h)) return;
    u8 packet[4]; wr16(packet,2); wr16(packet+2,(u16)page);
    int e=ioctl(h,DOS_CP_SELECT,packet,sizeof(packet),NULL);
    if(!e) print("MODE select code page function completed\n");
    else if(e==DOS_CP_NOT_PREPARED) fail("Code page not prepared");
    else if(e==DOS_CP_NOT_IN_FILE) fail("Current keyboard does not support this code page");
    else fail("Device error during select");
    dos_close(h);
}
static void cp_refresh(void) {
    unsigned h; if(open_device("CON",2,&h)) return;
    u8 packet[6]={0,0,2,0,0,0};
    int e=ioctl(h,DOS_CP_PREPARE_START,packet,sizeof(packet),NULL);
    int end=ioctl(h,DOS_CP_PREPARE_END,NULL,0,NULL); if(!e) e=end;
    if(e) fail("Unable to perform refresh operation");
    else print("MODE refresh code page function completed\n");
    dos_close(h);
}
static void heading(const char *name) {
    print("\nStatus for device %s:\n------------------%s\n",name,strlen(name)>3?"-----":"----");
}
/* ANSI.SYS's display information (IOCTL 440Ch 7Fh, set by 5Fh), which DOS 4's
 * MODE shows and changes only while ANSI.SYS is there. */
static int ansi_loaded(void) {u32 v=0; return !dos_installed(DOS_INSTALLED_ANSI,NULL,&v) && v;}
static int display(unsigned h,unsigned function,u8 packet[18]) {
    if(function==0x7f) {memset(packet,0,18); wr16(packet+2,14);}
    return ioctl(h,function,packet,18,NULL);
}
static void lines_status(void) {
    unsigned h; u8 packet[18];
    if(!ansi_loaded() || open_device("CON",2,&h)) return;
    if(!display(h,0x7f,packet)) print("COLUMNS=%u\nLINES=%u\n",(unsigned long long)rd16(packet+14),(unsigned long long)rd16(packet+16));
    dos_close(h);
}
/* MODE CON [COLS=c] [LINES=n] [RATE=r DELAY=d]: columns and lines through
 * ANSI.SYS (40 or 80, 25, 43 or 50, as the screen has them); the keyboard's
 * typematic rate is the firmware's own. */
static void con_features(char *p) {
    u32 cols=0,lines=0,rate=0,delay=0; char offending[24]="";
    for(;;) {
        p=skip(p); if(*p==',') p=skip(p+1);
        if(!*p) break;
        char *start=p; u32 *field=NULL;
        if(word(&p,"COLUMNS",NULL) || word(&p,"COLS",NULL)) field=&cols;
        else if(word(&p,"LINES",NULL)) field=&lines;
        else if(word(&p,"RATE",NULL)) field=&rate;
        else if(word(&p,"DELAY","DEL")) field=&delay;
        p=skip(p);
        if(!field || *p++!='=') {fail("Invalid parameter"); return;}
        p=skip(p);
        if(!number(&p,field) || (field==&cols && cols!=40 && cols!=80) || (field==&lines && lines!=25 && lines!=43 && lines!=50) ||
           (field==&rate && (rate<1 || rate>32)) || (field==&delay && (delay<1 || delay>4))) {fail("Invalid parameter"); return;}
        if(field!=&rate && field!=&delay && !offending[0]) {
            unsigned n=0; while(start<p && n<sizeof(offending)-1) offending[n++]=upper(*start++);
            offending[n]=0;
        }
    }
    if(!rate!=!delay) {fail("RATE and DELAY must be specified together"); return;}
    if(cols || lines) {
        unsigned h; u8 packet[18];
        if(!ansi_loaded()) {
            if(lines || cols!=80) fail(lines?"ANSI.SYS must be installed to perform requested function":"Function not supported on this computer - COLS=40");
        } else if(!open_device("CON",2,&h)) {
            int e=display(h,0x7f,packet);
            if(!e) {
                if(cols) wr16(packet+14,(u16)cols);
                if(lines) wr16(packet+16,(u16)lines);
                e=display(h,0x5f,packet);
            }
            if(e) {print("Function not supported on this computer - %s\n",offending); failed=1;}
            dos_close(h);
        }
    }
    if(rate) {print("Function not supported on this computer - RATE=%u\n",(unsigned long long)rate); failed=1;}
}
static void console(char *p) {
    p=skip(p);
    if(status_switch(p)) {heading("CON"); lines_status(); cp_status(); return;}
    char *keyword=p;
    if(!word(&p,"CODEPAGE","CP")) {con_features(keyword); return;}
    p=skip(p);
    if(status_switch(p)) {cp_status(); return;}
    if(word(&p,"PREPARE","PREP")) {p=skip(p); if(*p++!='=') {fail("Invalid parameter"); return;} cp_prepare(p);}
    else if(word(&p,"SELECT","SEL")) {p=skip(p); if(*p++!='=') {fail("Invalid parameter"); return;} cp_select(p);}
    else if(word(&p,"REFRESH","REF") && !*skip(p)) cp_refresh();
    else fail("Invalid parameter");
}
static int port_info(unsigned h,DosPortInfo *info) {
    DosRegs r={.ax=0x4402,.bx=h,.cx=sizeof(*info),.dx=(uintptr_t)info}; int e=dos_call(&r);
    return e?e:r.ax==sizeof(*info) && info->size==sizeof(*info)?0:DE_FUNCTION;
}
static int port_config(unsigned h,const DosPortConfig *config) {
    DosRegs r={.ax=0x4403,.bx=h,.cx=sizeof(*config),.dx=(uintptr_t)config}; return dos_call(&r);
}
static void show_serial(const char *name,const DosPortConfig *c) {
    static const char parity[]="noems";
    print("%s: %u,%c,%u,%s,%c\n",name,(unsigned long long)c->baud,parity[c->parity<5?c->parity:0],
          (unsigned long long)c->data_bits,c->stop_bits==DOS_STOP_TWO?"2":c->stop_bits==DOS_STOP_ONE_HALF?"1.5":"1",
          c->timeout_us>=1000000?'p':'-');
}
static void serial(const char *name,char *p) {
    unsigned h; if(open_device(name,2,&h)) return;
    DosPortInfo info; int e=port_info(h,&info);
    if(e || info.kind!=IO_PORT_UART) {fail("Must specify COM1, COM2, COM3 or COM4"); dos_close(h); return;}
    p=skip(p);
    if(status_switch(p)) {heading(name); show_serial(name,&info.config); dos_close(h); return;}
    /* MS-DOS 4 defaults: even parity, seven data bits, one stop bit. */
    DosPortConfig c=info.config; c.parity=DOS_PARITY_EVEN; c.data_bits=7; c.stop_bits=DOS_STOP_ONE;
    u32 baud=0; int retry=0;
    if(strchr(p,'=')) {
        while(*(p=skip(p))) {
            u32 value; char letter;
            if(word(&p,"BAUD","BAUD") && *p++=='=' && number(&p,&value)) baud=value;
            else if(word(&p,"PARITY","PARITY") && *p++=='=' && (letter=upper(*p++))) {
                const char *at=strchr(parities,letter); if(!at) {fail("Invalid parameter"); goto done;}
                c.parity=(u32)(at-parities);
            } else if(word(&p,"DATA","DATA") && *p++=='=' && number(&p,&value)) c.data_bits=value;
            else if(word(&p,"STOP","STOP") && *p++=='=' && number(&p,&value)) {
                if(*p=='.' && p[1]=='5' && value==1) {p+=2; c.stop_bits=DOS_STOP_ONE_HALF;} else c.stop_bits=value;
            } else if(word(&p,"RETRY","RETRY") && *p++=='=' && (letter=upper(*p++))) retry=letter!='N';
            else {fail("Invalid parameter"); goto done;}
        }
    } else {
        unsigned field=0;
        while(field<5) {
            p=skip(p); u32 value;
            if(*p!=',' && *p) {
                if(field==0) {if(!number(&p,&value)) {fail("Invalid baud rate specified"); goto done;} baud=value;}
                else if(field==1) {const char *at=strchr(parities,upper(*p)); if(!at) {fail("Invalid parameter"); goto done;} c.parity=(u32)(at-parities); p++;}
                else if(field==2) {if(!number(&p,&value)) {fail("Invalid parameter"); goto done;} c.data_bits=value;}
                else if(field==3) {
                    if(!number(&p,&value)) {fail("Invalid parameter"); goto done;}
                    if(*p=='.' && p[1]=='5' && value==1) {p+=2; c.stop_bits=DOS_STOP_ONE_HALF;} else c.stop_bits=value;
                } else {char letter=upper(*p++); if(letter!='P' && letter!='-' && letter!='N') {fail("Invalid parameter"); goto done;} retry=letter=='P';}
            }
            p=skip(p); field++;
            if(!*p) break;
            if(*p++!=',') {fail("Invalid parameter"); goto done;}
        }
        if(*skip(p)) {fail("Invalid number of parameters"); goto done;}
    }
    if(!baud) {fail("Baud rate required"); goto done;}
    /* Two-digit abbreviations name the classic rates, as in MS-DOS 4. */
    static const u32 short_rates[][2]={{11,110},{15,150},{30,300},{60,600},{12,1200},{24,2400},{48,4800},{96,9600},{19,19200}};
    for(unsigned i=0;i<ARRAY_SIZE(short_rates);i++) if(baud==short_rates[i][0]) baud=short_rates[i][1];
    c.baud=baud; if(baud==110 && c.stop_bits==DOS_STOP_ONE) c.stop_bits=DOS_STOP_TWO;
    if(retry) c.timeout_us=1000000;
    e=port_config(h,&c);
    if(e) {
        if(!c.baud || c.baud>115200 || 115200%c.baud) fail("Invalid baud rate specified");
        else fail("Invalid parameter");
    } else show_serial(name,&c);
done:
    dos_close(h);
}
static void printer(const char *name,char *p) {
    unsigned h; if(open_device(name,1,&h)) return;
    DosPortInfo info; int e=port_info(h,&info);
    if(e || info.kind!=IO_PORT_PRINTER) {fail("Printer error"); dos_close(h); return;}
    p=skip(p);
    if(status_switch(p)) {
        heading(name); print("%s: not rerouted\nRETRY=%s\n",name,info.config.timeout_us>=1000000?"B":"NONE");
        print("Code page operation not supported on this device\n"); dos_close(h); return;
    }
    if(*p=='=') {print("Function not supported on this computer - reroute\n"); failed=1; dos_close(h); return;}
    u32 columns=0,lines=0; int retry=-1;
    for(unsigned field=0;field<3;field++) {
        p=skip(p);
        if(*p && *p!=',') {
            if(field<2) {
                u32 value; if(!number(&p,&value)) {fail("Invalid parameter"); goto done;}
                if(field==0) columns=value; else lines=value;
            } else {char letter=upper(*p++); if(letter!='P' && letter!='-' && letter!='N') {fail("Invalid parameter"); goto done;} retry=letter=='P';}
        }
        p=skip(p); if(!*p) break;
        if(*p++!=',') {fail("Invalid parameter"); goto done;}
    }
    if(*skip(p) || (columns && columns!=80 && columns!=132) || (lines && lines!=6 && lines!=8)) {fail("Invalid parameter"); goto done;}
    /* Epson/IBM control codes, as sent by MS-DOS 4 MODE LPTn. */
    u8 codes[4]; unsigned n=0; u32 written;
    if(columns) codes[n++]=columns==80?0x12:0x0f;
    if(lines) {codes[n++]=27; codes[n++]=lines==8?'0':'2';}
    if(n && (dos_write(h,codes,n,&written) || written!=n)) {fail("Printer error"); goto done;}
    if(retry>=0) {
        DosPortConfig c=info.config; c.timeout_us=retry?1000000:100000;
        if(port_config(h,&c)) {fail("Printer error"); goto done;}
    }
    if(columns) print("%s: set for %u\n",name,(unsigned long long)columns);
    if(lines) print("Printer lines per inch set\n");
    if(retry>=0) print("%s retry on parallel printer time-out\n",retry?"Infinite":"No");
done:
    dos_close(h);
}
static void all_status(void) {
    heading("CON"); lines_status(); cp_status();
    static const char *names[]={"COM1","COM2","COM3","COM4","LPT1","LPT2","LPT3"};
    for(unsigned i=0;i<ARRAY_SIZE(names);i++) {
        unsigned h; DosPortInfo info;
        if(dos_open(names[i],i<4?2:1,0,&h)) continue;
        int e=port_info(h,&info); dos_close(h);
        if(e) continue;
        char tail[8]="/STATUS";
        if(i<4) serial(names[i],tail); else printer(names[i],tail);
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256]; if(strcopy(tail,sizeof(tail),app_dos->command_tail())) {fail("Invalid number of parameters"); return 1;}
    char *p=skip(tail);
    if(!*p) {all_status(); return failed;}
    char device[6]; unsigned n=0;
    while(n<5 && ((upper(*p)>='A' && upper(*p)<='Z') || (*p>='0' && *p<='9'))) device[n++]=upper(*p++);
    device[n]=0;
    if(*p==':') p++; /* COM1:96,N,8,1 needs no separator after the colon. */
    else if(*p && *p!=' ' && *p!='\t' && *p!='/' && *p!='=') {fail("Illegal device name"); return 1;}
    if(!strcmp(device,"CON")) console(p);
    else if(n==4 && !memcmp(device,"COM",3) && device[3]>='1' && device[3]<='4') serial(device,p);
    else if(n==4 && !memcmp(device,"LPT",3) && device[3]>='1' && device[3]<='3') printer(device,p);
    else fail("Illegal device name");
    return failed;
}
