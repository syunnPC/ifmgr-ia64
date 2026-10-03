/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed,breaks_seen;
static int callback_reentry;
static void check(const char *name,int ok) {
    print("[%s] %s\n",ok?"PASS":"FAIL",name); if(ok) passed++; else failed++;
}
static EFI_STATUS finish(void) {
    print("CONTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
static int on_break(void *context) {
    if(context!=&breaks_seen) return DOS_BREAK_ABORT;
    breaks_seen++;
    DosRegs r={.ax=0x3000};
    callback_reentry=dos_call(&r)==DE_BUSY;
    return DOS_BREAK_CONTINUE;
}
static void ready(const char *mode) {print("CONTEST: %s-ready\n",mode);}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *mode=app_dos->command_tail(); DosRegs r; u32 n,pos; unsigned h;
    if(!strcmp(mode,"queue")) {
        const IoServices *io=app_dos->io; IoEvent a,b;
        check("keyboard-capability",io && (io->capabilities&IO_CAP_CONSOLE_KEY));
        if(!io || io->size<offsetof(IoServices,console_key)+sizeof(io->console_key) || !io->console_key) return 1;
        ready(mode);
        int e=io->console_key(io->context,&a,IO_KEY_PEEK|IO_KEY_WAIT);
        check("peek-key",!e && a.type==IO_EVENT_KEY && a.unicode=='K');
        e=io->console_key(io->context,&b,IO_KEY_PEEK);
        check("peek-does-not-consume",!e && b.type==a.type && b.unicode==a.unicode);
        e=io->poll_event(io->context,&b);
        check("GUI-poll-shares-key-queue",!e && b.type==a.type && b.unicode==a.unicode);
        r=(DosRegs){.ax=0x0600,.dx=255}; check("key-consumed-once",!dos_call(&r) && (r.flags&0x40));
        return finish();
    }
    if(!strcmp(mode,"keys")) {
        /* Each key event as the firmware gives it, until q. */
        const IoServices *io=app_dos->io; IoEvent k;
        print("CONTEST: modifiers %s\n",io->capabilities&IO_CAP_KEY_MODIFIERS?"yes":"no");
        ready(mode);
        do {
            if(io->console_key(io->context,&k,IO_KEY_WAIT)) return 1;
            print("CONTEST: key unicode %u scan %u flags %u modifiers %u\n",(unsigned long long)k.unicode,(unsigned long long)k.scan,
                  (unsigned long long)k.flags,(unsigned long long)k.modifiers);
        } while(k.unicode!='q');
        return 0;
    }
    if(!strcmp(mode,"setup")) {
        int e=dos_open("C:\\CONIN.TMP",2,1,&h); check("create-input",!e); if(e) return finish();
        check("write-input",!dos_write(h,"JK\3Oline\r",9,&n) && n==9); check("close-input",!dos_close(h)); return finish();
    }
    if(!strcmp(mode,"redir")) {
        r=(DosRegs){.ax=0x4400,.bx=0}; check("stdin-is-file",!dos_call(&r) && !(r.dx&128));
        r=(DosRegs){.ax=0x0100}; check("echo-input",!dos_call(&r) && r.ax=='J');
        r=(DosRegs){.ax=0x0b00}; check("input-ready",!dos_call(&r) && r.ax==255);
        r=(DosRegs){.ax=0x0600,.dx=255}; check("nonblocking-input",!dos_call(&r) && !(r.flags&0x40) && r.ax=='K');
        r=(DosRegs){.ax=0x0700}; check("raw-control-C",!dos_call(&r) && r.ax==3);
        r=(DosRegs){.ax=0x0800}; check("input-without-echo",!dos_call(&r) && r.ax=='O');
        u8 buffer[18]={16,0};
        check("redirected-line",!dos_line_input(buffer) && buffer[1]==4 && !memcmp(buffer+2,"line\r",5));
        r=(DosRegs){.ax=0x0b00}; check("EOF-status",!dos_call(&r) && !r.ax);
        r=(DosRegs){.ax=0x0600,.dx=255}; check("EOF-zero-flag",!dos_call(&r) && (r.flags&0x40) && !r.ax);
        r=(DosRegs){.ax=0x0700}; check("native-character-EOF",dos_call(&r)==DE_EOF);
        return finish();
    }
    if(!strcmp(mode,"line") || !strcmp(mode,"edit")) {
        u8 buffer[18]={16,3,'O','L','D','\r'};
        ready(mode); int e=dos_line_input(buffer); con_puts("\n");
        const char *want=!strcmp(mode,"line")?"aC\r":"OLD\r"; unsigned length=strlen(want)-1;
        check("buffered-editing",!e && buffer[1]==length && !memcmp(buffer+2,want,length+1));
        return finish();
    }
    if(!strcmp(mode,"raw")) {
        r=(DosRegs){.ax=0x4400,.bx=0}; int e=dos_call(&r); unsigned saved=r.dx&255;
        check("get-console-mode",!e && (r.dx&128));
        r=(DosRegs){.ax=0x4401,.bx=0,.dx=saved|32}; check("set-raw-mode",!dos_call(&r));
        ready(mode); u8 buffer[4]; e=dos_read(0,buffer,4,&n);
        r=(DosRegs){.ax=0x4401,.bx=0,.dx=saved}; int restored=dos_call(&r);
        check("raw-bytes",!e && n==4 && !memcmp(buffer,"\3\x1a\rZ",4));
        check("restore-console-mode",!restored); return finish();
    }
    if(!strcmp(mode,"callback")) {
        DosBreakHandler handler={on_break,&breaks_seen},previous;
        check("install-break-handler",!dos_break_handler(&handler,&previous));
        ready(mode); r=(DosRegs){.ax=0x0800}; int e=dos_call(&r);
        check("handler-continues",!e && r.ax=='X' && breaks_seen==1);
        check("handler-cannot-reenter",callback_reentry);
        check("restore-break-handler",!dos_break_handler(&previous,NULL)); return finish();
    }
    if(!strcmp(mode,"abort-child")) {
        void *memory;
        if(dos_alloc(1024,&memory) || dos_open("C:\\BREAK.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,1,&h) ||
           dos_write(h,"held",4,&n) || dos_lock(h,0,0,4)) return 1;
        ready(mode); r=(DosRegs){.ax=0x0800}; dos_call(&r);
        check("default-handler-must-not-return",0); return finish();
    }
    if(!strcmp(mode,"abort")) {
        DosBreakHandler handler={on_break,&breaks_seen},previous;
        check("parent-break-handler",!dos_break_handler(&handler,&previous));
        DosInfo before,after; check("arena-before",!dos_query(&before));
        int e=dos_exec("C:\\CONTEST.EFI","abort-child");
        DosExitInfo exit; check("child-aborted",!e && !dos_last_exit(&exit) && exit.kind==DOS_EXIT_BREAK);
        check("parent-handler-not-inherited",!breaks_seen);
        check("arena-reclaimed",!dos_query(&after) && after.largest_paragraphs==before.largest_paragraphs);
        e=dos_open("C:\\BREAK.TMP",DOS_OPEN_RDWR|DOS_SHARE_DENY_ALL,0,&h); check("aborted-handle-released",!e);
        if(!e) {
            check("aborted-lock-released",!dos_lock(h,0,0,4));
            char data[4]; check("committed-data-preserved",!dos_seek(h,0,0,&pos) && !dos_read(h,data,4,&n) && n==4 && !memcmp(data,"held",4));
            check("close-file",!dos_close(h));
        }
        check("remove-file",!dos_remove("C:\\BREAK.TMP",0));
        check("restore-parent-handler",!dos_break_handler(&previous,NULL)); return finish();
    }
    print("CONTEST modes: queue setup redir line edit raw callback abort\n"); return 0;
}
