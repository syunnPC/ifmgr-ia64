/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * FASTOPEN: native counterpart of MS-DOS 4 CMD/FASTOPEN.
 *   FASTOPEN d:[=n | =(n,m)] [d:[=...]...] [/X]
 * MSDOS.SYS keeps directory entries and FAT chains in its own caches, so
 * FASTOPEN checks what DOS 4's checked (fixed drives, 10 to 999 entries and
 * 1 to 999 extents, 999 in all, 24 drives at most; no expanded memory for
 * /X here) and counts as installed from its first run, as v4.0
 * FASTOPEN.SKL's messages say.
 */
#include "util.h"
static int fail(const char *message,const char *arg) {
    to_stderr(1); if(arg) print("\n%s %s\n",message,arg); else print("\n%s\n",message); to_stderr(0); return 1;
}
static int number(const char **s,u32 *out) {
    u32 n=0; const char *p=*s; if(*p<'0' || *p>'9') return 0;
    while(*p>='0' && *p<='9') {n=n*10+(u32)(*p++-'0'); if(n>9999) return 0;}
    *s=p; *out=n; return 1;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *p=app_dos->command_tail(); u32 seen=0,drives=0,total=0,one=1,before=0; int expanded=0;
    for(;;) {
        while(*p==' ' || *p=='\t' || *p==',' || *p==';') p++;
        if(!*p) break;
        if(*p=='/') {
            if(upper(p[1])!='X' || (p[2] && p[2]!=' ' && p[2]!='\t')) return fail("Invalid switch",NULL);
            expanded=1; p+=2; continue;
        }
        char letter[3]={(char)upper(p[0]),':',0}; u32 drive=(u32)(letter[0]-'A'),n=34,m=0;
        if(letter[0]<'A' || letter[0]>'Z' || p[1]!=':') return fail("Invalid parameter",NULL);
        p+=2;
        if(*p=='=') {
            p++;
            if(*p=='(') {
                p++; if(!number(&p,&n)) return fail("Invalid number of file/directory entries",NULL);
                if(*p==',') {p++; if(!number(&p,&m) || m<1 || m>999) return fail("Invalid extent entry",NULL);}
                if(*p++!=')') return fail("Invalid parameter",NULL);
            } else if(!number(&p,&n)) return fail("Invalid number of file/directory entries",NULL);
            if(n<10 || n>999) return fail("Invalid number of file/directory entries",NULL);
        }
        DosDriveInfo info;
        if(dos_drive_info(drive,&info)) return fail("Invalid drive specification",letter);
        if(info.flags&(DOS_DRIVE_REMOVABLE|DOS_DRIVE_REMOTE|DOS_DRIVE_SUBST|DOS_DRIVE_ASSIGNED)) return fail("Cannot use FASTOPEN for drive",letter);
        if(seen&(1u<<drive)) return fail("Same drive specified more than once",NULL);
        seen|=1u<<drive;
        if(++drives>24) return fail("Too many drive entries",NULL);
        total+=n; if(total>999) return fail("Too many file/directory entries",NULL);
    }
    if(expanded) {to_stderr(1); print("\nExpanded memory not available\n"); to_stderr(0);}
    if(dos_installed(DOS_INSTALLED_FASTOPEN,NULL,&before)) return 1;
    if(before) {print("\nFASTOPEN already installed\n"); return 1;}
    dos_installed(DOS_INSTALLED_FASTOPEN,&one,NULL);
    print("\nFASTOPEN installed\n");
    return 0;
}
