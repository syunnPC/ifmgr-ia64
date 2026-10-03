/* SPDX-License-Identifier: GPL-2.0-or-later
 * Pipe and redirection filter for COMMAND.COM tests: EMIT n, UPPER or COUNT.
 */
#include "runtime.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256]; strcopy(tail,sizeof(tail),app_dos->command_tail());
    char *p=tail; while(*p==' ') p++;
    if(!stricmp(p,"UPPER") || !stricmp(p,"COUNT")) {
        int count=upper(*p)=='C'; u8 buf[512]; u32 got,lines=0,bytes=0; int e;
        while(!(e=dos_read(0,buf,sizeof(buf),&got)) && got) {
            for(u32 i=0;i<got;i++) {if(buf[i]=='\n') lines++; buf[i]=upper(buf[i]);}
            bytes+=got;
            if(!count) {u32 written; e=dos_write(1,buf,got,&written); if(e) break;}
        }
        if(e) {print("PIPETEST: read error %u\n",(unsigned long long)e); return EFI_DEVICE_ERROR;}
        if(count) print("PIPETEST: %u lines, %u bytes\n",(unsigned long long)lines,(unsigned long long)bytes);
        return EFI_SUCCESS;
    }
    if(upper(p[0])=='E' && upper(p[1])=='M' && upper(p[2])=='I' && upper(p[3])=='T' && p[4]==' ') {
        unsigned n=0; for(p+=5;*p>='0' && *p<='9';p++) n=n*10+(*p-'0');
        for(unsigned i=1;i<=n;i++) print("line %u\n",(unsigned long long)i);
        return EFI_SUCCESS;
    }
    print("usage: PIPETEST EMIT n | UPPER | COUNT\n"); return EFI_INVALID_PARAMETER;
}
