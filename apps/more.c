/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * MORE: native counterpart of MS-DOS 4 CMD/MORE.
 *   MORE < file      command | MORE
 * Standard input a screenful at a time: lines are counted as DOS 4 counted
 * them (a line wider than the screen takes more, tabs reach the next eighth
 * column, Ctrl+Z ends the text), and after 24 of a 25-line screen
 * "-- More --" goes to standard error and a key is waited for, read from the
 * console through standard error's handle, which takes standard input's
 * place as DOS 4's did.
 */
#include "util.h"
#define ROWS 25
#define COLUMNS 80
static char pending[512]; static unsigned queued;
static void flush(void) {u32 done; if(queued) dos_write(1,pending,queued,&done); queued=0;}
static void emit(char c) {if(queued==sizeof(pending)) flush(); pending[queued++]=c;}
static void ask(void) {
    u32 done; DosRegs r={.ax=0x0c08};
    flush();
    dos_write(2,"-- More --",10,&done);
    if(!dos_call(&r) && !(r.ax&0xff)) {r=(DosRegs){.ax=0x0800}; dos_call(&r);}
    dos_write(2,"\r\n",2,&done);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    static u8 buffer[4096]; unsigned input,row=1,column=1; u32 got,done;
    if(dos_dup(0,&input) || dos_dup2(2,0)) return 1;
    dos_write(1,"\r\n",2,&done);
    while(!dos_read(input,buffer,sizeof(buffer),&got) && got) {
        for(u32 i=0;i<got;i++) {
            u8 c=buffer[i];
            if(c==0x1a) {flush(); return 0;}
            if(c=='\r') column=1;
            else if(c=='\n') row++;
            else if(c==8) {if(column>1) column--;}
            else if(c=='\t') column=((column+7)&~7u)+1;
            else if(c>=' ' && ++column>COLUMNS) {row++; column=1;}
            emit((char)c);
            if(row>=ROWS) {ask(); row=column=1;}
        }
    }
    flush();
    return 0;
}
