/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * APPEND: MS-DOS 4 CMD/APPEND counterpart.
 *   APPEND [d:]path[;[d:]path...]     APPEND ;     APPEND
 *   APPEND [list] [/X[:ON|:OFF]] [/PATH:ON|/PATH:OFF] [/E]
 *
 * DosApi append supplies fallback directories for opens and, with /X,
 * searches/EXEC. The first call installs; /E stores the list in each task's
 * APPEND= variable, subsequently managed by COMMAND.COM. Later calls
 * replace list/flags; ; clears the list; no arguments prints status.
 * Messages: v4.0 APPEND.SKL.
 */
#include "util.h"
static int say(const char *message) {to_stderr(1); print("%s\n",message); to_stderr(0); return 1;}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    const char *p=app_dos->command_tail(); char list[DOS_PATH_MAX]=""; int have_list=0,environment=0,x=-1,path=-1;
    DosAppend now={.size=sizeof(now)};
    if(dos_append(NULL,&now)) return say("Incorrect APPEND version");
    int installed=(now.flags&DOS_APPEND_INSTALLED)!=0;
    for(;;) {
        while(*p==' ' || *p=='\t' || *p==',' || *p=='=') p++;
        if(!*p) break;
        if(*p=='/') {
            char word[16]; unsigned n=0;
            p++; while(*p && *p!=' ' && *p!='\t' && *p!='/' && n<sizeof(word)-1) word[n++]=(char)upper(*p++);
            word[n]=0;
            if(!strcmp(word,"E")) {if(environment || installed) return say("Invalid combination of parameters"); environment=1;}
            else if(!strcmp(word,"X") || !strcmp(word,"X:ON")) {if(x>=0) return say("Invalid combination of parameters"); x=1;}
            else if(!strcmp(word,"X:OFF")) {if(x>=0) return say("Invalid combination of parameters"); x=0;}
            else if(!strcmp(word,"PATH:ON")) {if(path>=0) return say("Invalid combination of parameters"); path=1;}
            else if(!strcmp(word,"PATH:OFF")) {if(path>=0) return say("Invalid combination of parameters"); path=0;}
            else return say("Invalid parameter");
            continue;
        }
        /* The list runs to the next blank or switch; ";" alone empties it. */
        if(have_list) return say("Invalid parameter");
        unsigned n=0;
        while(*p && *p!=' ' && *p!='\t' && *p!='/') {if(n+1>=sizeof(list)) return say("Invalid path"); list[n++]=(char)upper(*p++);}
        list[n]=0; have_list=1;
        if(!strcmp(list,";")) list[0]=0;
        for(char *entry=list;*entry;) {
            char *end=entry; while(*end && *end!=';') end++;
            char saved=*end,full[DOS_PATH_MAX]; *end=0;
            int bad=*entry && dos_full_path(entry,full);
            *end=saved;
            if(bad) return say("Invalid path");
            entry=*end?end+1:end;
        }
    }
    if(!installed) {
        now.flags=DOS_APPEND_ENABLED|DOS_APPEND_DRIVE|DOS_APPEND_PATH;
        now.list[0]=0;
    }
    if(environment) now.flags|=DOS_APPEND_ENV;
    if(x>=0) now.flags=x?now.flags|DOS_APPEND_X:now.flags&~DOS_APPEND_X;
    if(path>=0) now.flags=path?now.flags|DOS_APPEND_DRIVE|DOS_APPEND_PATH:now.flags&~(DOS_APPEND_DRIVE|DOS_APPEND_PATH);
    if(have_list) {
        /* With /E the list is the APPEND= variable, which this program can
         * only set in its own environment: COMMAND.COM's APPEND sets it. */
        if(now.flags&DOS_APPEND_ENV) {if(installed) return say("Invalid combination of parameters");}
        else strcopy(now.list,sizeof(now.list),list);
    }
    if(!installed || have_list || x>=0 || path>=0 || environment) {
        if(dos_append(&now,NULL)) return say("Incorrect APPEND version");
        return 0;
    }
    /* The list in effect. */
    char shown[DOS_PATH_MAX];
    if(now.flags&DOS_APPEND_ENV) {if(dos_env_get("APPEND",shown,sizeof(shown))) shown[0]=0;}
    else strcopy(shown,sizeof(shown),now.list);
    if(shown[0]) print("APPEND=%s\n",shown); else print("No Append\n");
    return 0;
}
