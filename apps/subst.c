/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * SUBST/JOIN from one source (-DJOIN selects JOIN).
 *   SUBST d: [d:]path     JOIN d: [d:]path
 *   SUBST d: /D           JOIN d: /D
 *
 * SUBST maps a letter to another drive's directory. JOIN maps a drive to
 * an empty directory directly below another drive's root, creating it if
 * needed. /D removes mappings; no arguments lists them. MSDOS.SYS owns
 * and validates the table (DosApi drive_map). Messages: v4.0 SUBST.SKL/JOIN.SKL.
 */
#include "util.h"
#ifdef JOIN
#define KIND DOS_MAP_JOIN
#define NAME "JOIN"
#else
#define KIND DOS_MAP_SUBST
#define NAME "SUBST"
#endif
static int fail(const char *message,const char *arg) {
    to_stderr(1);
    if(arg && *arg) print("%s - %s\n",message,arg); else print("%s\n",message);
    to_stderr(0); return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char arg[DOS_PATH_MAX],letter[DOS_PATH_MAX]="",path[DOS_PATH_MAX]="",remove_switch[8]="";
    const char *p=app_dos->command_tail(); int any=0;
    while((p=next_arg(p,arg,sizeof(arg)))!=NULL) {
        any=1;
        if(arg[0]=='/') {
            if(upper(arg[1])!='D' || arg[2]) return fail("Invalid switch",arg);
            if(remove_switch[0]) return fail("Invalid switch",arg);
            strcopy(remove_switch,sizeof(remove_switch),arg);
        } else if(!letter[0] && arg[0] && arg[1]==':' && !arg[2]) strcopy(letter,sizeof(letter),arg);
        else if(!path[0] && letter[0]) strcopy(path,sizeof(path),arg);
        else if(!letter[0]) return fail("Invalid parameter",arg);
        else return fail("Too many parameters",arg);
    }
    if(!any) {
        for(u32 d=0;d<DOS_DRIVES;d++) {
            DosDriveMap m={.size=sizeof(m)};
            if(!dos_drive_map(d,NULL,&m) && m.kind==KIND) print("%c: => %s\n",(char)('A'+d),m.path);
        }
        return 0;
    }
    if(!letter[0]) return fail("Invalid parameter",path);
    if(!path[0] && !remove_switch[0]) return fail("Invalid parameter",letter);
    if(path[0] && remove_switch[0]) return fail("Too many parameters",remove_switch);
    u32 drive=(u32)(upper(letter[0])-'A');
    if(drive>=DOS_DRIVES) return fail("Invalid parameter",letter);
    DosDriveMap set={.size=sizeof(set),.kind=remove_switch[0]?DOS_MAP_NONE:KIND};
    if(!remove_switch[0]) {
        DosDriveMap now={.size=sizeof(now)};
        if(!dos_drive_map(drive,NULL,&now) && now.kind && now.kind!=KIND) return fail("Invalid parameter",letter);
        if(dos_canonical(path,set.path)) return fail("Invalid parameter",path);
#ifdef JOIN
        u8 attr;
        if(dos_attribute(set.path,0,&attr) && dos_mkdir(set.path)) return fail("Invalid parameter",path);
#endif
    } else {
        DosDriveMap now={.size=sizeof(now)};
        if(dos_drive_map(drive,NULL,&now) || now.kind!=KIND) return fail("Invalid parameter",letter);
    }
    int e=dos_drive_map(drive,&set,NULL);
    if(e==DE_REMOTE) return fail("Cannot " NAME " a network drive",NULL);
#ifdef JOIN
    if(e==DE_ACCESS && set.kind) {
        DosDriveInfo info;
        if(!dos_drive_info(drive,&info) && (info.flags&DOS_DRIVE_REMOTE)) return fail("Cannot " NAME " a network drive",NULL);
        return fail("Directory not empty",path);
    }
#endif
    if(e) return fail("Invalid parameter",set.kind?path:letter);
    return 0;
}
