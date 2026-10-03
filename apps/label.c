/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * LABEL: native counterpart of MS-DOS 4 CMD/LABEL. Labels are changed with
 * extended-FCB delete/create, which also update the boot record's label.
 */
#include "maint.h"
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256],*p=tail; strcopy(tail,sizeof(tail),app_dos->command_tail());
    while(*p==' ' || *p=='\t') p++;
    unsigned drive=dos_current_drive();
    if(p[0] && p[1]==':') {
        char letter[3]={p[0],':',0};
        if(drive_argument(letter,&drive)) {print("Invalid drive specification\n"); return 1;}
        p+=2; while(*p==' ' || *p=='\t') p++;
    }
    size_t n=strlen(p); while(n && (p[n-1]==' ' || p[n-1]=='\t')) p[--n]=0;
    DosDriveInfo info; int e=dos_drive_info(drive,&info);
    if(e) {print("Invalid drive specification\n"); return 1;}
    if(refuse_mapped(drive,"LABEL")) return 1;
    u8 name[11];
    if(*p) {
        if(label_name(p,name)) {print("Invalid characters in volume label\n"); return 1;}
        e=label_set(drive,name); if(e) {print("%s (DOS error %u)\n",dos_error(e),(unsigned long long)e); return 1;}
        return 0;
    }
    char current[12]; e=label_get(drive,current);
    if(e && e!=DE_NOMORE && e!=DE_NOFILE) {print("%s (DOS error %u)\n",dos_error(e),(unsigned long long)e); return 1;}
    if(e) current[0]=0;
    unsigned end=11; while(end && current[end-1]==' ') current[--end]=0;
    if(current[0]) print("Volume in drive %c is %s\n",'A'+drive,current);
    else print("Volume in drive %c has no label\n",'A'+drive);
    DosMediaId id={.size=sizeof(id)};
    if(!dos_media_id(drive,&id,0)) {char text[10]; serial_number(id.serial,text); print("Volume Serial Number is %s\n",text);}
    for(;;) {
        char text[16];
        if(read_text("Volume label (11 characters, ENTER for none)? ",text,sizeof(text))) return 1;
        if(!text[0]) {
            if(current[0] && ask_yes_no("\nDelete current volume label (Y/N)?")) {
                e=label_set(drive,NULL); if(e) {print("%s (DOS error %u)\n",dos_error(e),(unsigned long long)e); return 1;}
            }
            return 0;
        }
        if(label_name(text,name)) {print("Invalid characters in volume label\n"); continue;}
        e=label_set(drive,name); if(e) {print("%s (DOS error %u)\n",dos_error(e),(unsigned long long)e); return 1;}
        return 0;
    }
}
