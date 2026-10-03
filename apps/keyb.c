/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * KEYB: MS-DOS 4 CMD/KEYB counterpart (KEYBCMD.ASM, KEYBTBBL.ASM).
 *   KEYB [xx[,[yyy][,[d:][path]KEYBOARD.SYS]]] [/ID:nnn]
 *   KEYB nnn[,...]
 *
 * Load language state/common tables and code-page sections supported by
 * CON into MSDOS.SYS (DosApi keyb), matching TABLE_BUILD. Select the
 * requested page, else CON's, else the language's first. Search for an
 * unnamed KEYBOARD.SYS in the current directory, KEYB's directory, then
 * the root, proceeding only on not-found. No arguments reports layout/CON
 * page. Messages and exit codes follow DOS 4 USA-MS.MSG (KEYB.SKL).
 */
#include "util.h"
#define HEADER 28 /* FFh,"KEYB   ", reserved, sizes, IDs and languages */
#define TABLE_LIMIT 300 /* KEYB's buffer for a file's tables */
enum {EXIT_PARAMETERS=1,EXIT_FILE,EXIT_MEMORY,EXIT_CON,EXIT_NOT_PREPARED,EXIT_NOT_VALID};

static unsigned file=0xffff;
static u8 header[HEADER],tables[DOS_KEYB_TABLE_MAX];

static void say(const char *text) {print("%s\n",text);}
static int fail(const char *text,int code) {say(text); return code;}
static int digits(const char *s) {if(!*s) return 0; for(;*s;s++) if(*s<'0' || *s>'9') return 0; return 1;}
/* A decimal value of at most 999; 0 when it is not one. */
static int decimal(const char *s,u32 *out) {
    u32 v=0; if(!digits(s)) return 0;
    for(;*s;s++) {v=v*10+(u32)(*s-'0'); if(v>999) return 0;}
    *out=v; return 1;
}
static int fetch(u32 at,void *buffer,u32 n) {
    u32 pos,got=0;
    return !dos_seek(file,at,0,&pos) && !dos_read(file,buffer,n,&got) && got==n;
}
static int open_file(const char *name) {
    int e=dos_open(name,DOS_OPEN_READ,0,&file); if(e) file=0xffff;
    return e;
}
/* KEYBOARD.SYS: the given name only, else here, beside KEYB, at the root. */
static int open_tables(const char *given) {
    if(given[0]) return !open_file(given);
    int e=open_file("KEYBOARD.SYS");
    if(e==DE_NOFILE || e==DE_PATH) {
        char path[DOS_PATH_MAX];
        if(app_dos->size>=offsetof(DosApi,program_path)+sizeof(app_dos->program_path) && app_dos->program_path && !app_dos->program_path(path)) {
            char *end=path; for(char *s=path;*s;s++) if(*s=='\\' || *s==':') end=s+1;
            *end=0; if(!strappend(path,sizeof(path),"KEYBOARD.SYS")) e=open_file(path);
        }
    }
    if(e==DE_NOFILE || e==DE_PATH) e=open_file("\\KEYBOARD.SYS");
    return !e;
}
static int signature(void) {return fetch(0,header,HEADER) && header[0]==0xff && !memcmp(header+1,"KEYB   ",7);}

/* The query: KEYB's language and code page, the ID typed, CON's page. */
static int query(void) {
    DosKeybRequest r={.size=sizeof(r)};
    if(!app_dos->keyb(DOS_KEYB_QUERY,&r)) {
        if(r.language[0]) {print("Current keyboard code: %c%c",r.language[0],r.language[1]); print("  code page: %u\n",(unsigned long long)r.code_page);}
        if(r.id || !r.language[0]) {
            print("Current keyboard ID: %u",(unsigned long long)r.id);
            if(r.language[0]) print("\n\r"); else print("  code page: %u\n",(unsigned long long)r.code_page);
        }
    } else say("KEYB has not been installed");
    unsigned con; u8 packet[4]; DosRegs q;
    int e=dos_open("CON",DOS_OPEN_RDWR,0,&con);
    if(!e) {
        q=(DosRegs){.ax=0x440c,.bx=con,.cx=DOS_CP_CATEGORY<<8|DOS_CP_QUERY,.dx=(uintptr_t)packet,.si=sizeof(packet)};
        e=dos_call(&q); dos_close(con);
    }
    if(e) return fail("Active code page not available from CON device",EXIT_CON);
    print("Current CON code page: %u\n",(unsigned long long)rd16(packet+2));
    return 0;
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    if(app_dos->size<offsetof(DosApi,keyb_key)+sizeof(app_dos->keyb_key) || !app_dos->keyb) {
        to_stderr(1); print("Incorrect DOS version\n"); to_stderr(0); return 1;
    }
    /* Positionals by blanks or commas (two commas leave one out), and /ID. */
    char given[3][DOS_PATH_MAX]={{0}}; int present[3]={0},id_switch=0,positional_id=0,after=0;
    u32 id=0,page=0; unsigned slot=0;
    const char *p=app_dos->command_tail();
    for(;;) {
        while(*p==' ' || *p=='\t') p++;
        if(!*p) break;
        if(*p==',') {
            if(!after) {if(slot>=3 || positional_id) {parse_error(PARSE_TOO_MANY,","); return EXIT_PARAMETERS;} present[slot++]=1;}
            after=0; p++; continue;
        }
        char arg[DOS_PATH_MAX]; unsigned n=0;
        do {if(n+1<sizeof(arg)) arg[n++]=(char)upper(*p); p++;} while(*p && *p!=' ' && *p!='\t' && *p!=',' && *p!='/');
        arg[n]=0;
        if(arg[0]=='/') {
            if(memcmp(arg,"/ID",3) || (arg[3] && arg[3]!=':')) {parse_error(PARSE_SWITCH,arg); return EXIT_PARAMETERS;}
            if(arg[3]!=':' || !arg[4]) return fail("Invalid keyboard ID specified",EXIT_PARAMETERS);
            if(!digits(arg+4)) {parse_error(PARSE_SWITCH,arg); return EXIT_PARAMETERS;}
            if(!decimal(arg+4,&id)) {parse_error(PARSE_RANGE,arg); return EXIT_PARAMETERS;}
            id_switch=1; continue;
        }
        if(slot>=3 || positional_id) {parse_error(PARSE_TOO_MANY,arg); return EXIT_PARAMETERS;}
        for(char *s=arg;*s;s++) if(*s=='=') {parse_error(PARSE_KEYWORD,arg); return EXIT_PARAMETERS;}
        /* A keyboard ID first: no positionals after it; the last ID given counts. */
        if(!slot && digits(arg)) {
            if(!decimal(arg,&id)) {parse_error(PARSE_RANGE,arg); return EXIT_PARAMETERS;}
            positional_id=1;
        }
        present[slot]=1; strcopy(given[slot],DOS_PATH_MAX,arg); slot++; after=1;
    }
    if(!present[0] && !present[1] && !present[2] && !id_switch) return query();
    /* The first a two-letter language unless an ID; the second a code
     * page; the third the file. */
    if(positional_id) id_switch=0;
    else if(strlen(given[0])!=2) {parse_error(present[0]?PARSE_PARAMETER:PARSE_MISSING,given[0]); return EXIT_PARAMETERS;}
    if(present[1] && given[1][0]) {
        if(!digits(given[1])) {parse_error(PARSE_SWITCH,given[1]); return EXIT_PARAMETERS;}
        if(!decimal(given[1],&page)) {parse_error(PARSE_RANGE,given[1]); return EXIT_PARAMETERS;}
    }
    int with_page=present[1] && given[1][0];

    if(!open_tables(given[2])) return fail("Bad or missing Keyboard Definition File",EXIT_FILE);
    u32 languages=0,ids=0,entry=0; u8 directory[TABLE_LIMIT],lang[10];
    if(positional_id || id_switch) {
        /* SCAN_ID: the first entry of the ID table with it; any trouble
         * with the file is an invalid ID here. */
        int ok=signature();
        if(ok) {languages=rd16(header+26); ids=rd16(header+24); ok=(languages+ids)*6<=TABLE_LIMIT && fetch(HEADER,directory,(languages+ids)*6);}
        u32 first=ids;
        for(u32 i=0;ok && i<ids;i++) if(rd16(directory+6*(languages+i))==id) {first=i; break;}
        if(!ok || first==ids) {dos_close(file); return fail("Invalid keyboard ID specified",EXIT_PARAMETERS);}
        entry=rd32(directory+6*(languages+first)+2);
        if(id_switch) {
            /* GET_ID: as many entries from there as that language entry has
             * IDs, the first whose own ID and language match. */
            u32 count=fetch(entry,lang,10)?lang[8]:0,found=0;
            for(u32 i=0;i<count && first+i<ids;i++) {
                u32 at=rd32(directory+6*(languages+first+i)+2);
                if(fetch(at,lang,10) && rd16(lang+2)==id && !memcmp(lang,given[0],2)) {found=at; break;}
            }
            if(!found) {dos_close(file); return fail("Keyboard ID specified is inconsistent with the selected keyboard layout",EXIT_PARAMETERS);}
            entry=found;
        }
    } else {
        if(!signature() || (languages=rd16(header+26))*6>TABLE_LIMIT || !fetch(HEADER,directory,languages*6)) {dos_close(file); return fail("Bad or missing Keyboard Definition File",EXIT_FILE);}
        u32 i=0; while(i<languages && memcmp(directory+6*i,given[0],2)) i++;
        if(i==languages) {dos_close(file); return fail("Invalid keyboard code specified",EXIT_PARAMETERS);}
        entry=rd32(directory+6*i+2);
    }
    u8 pages[TABLE_LIMIT];
    if(!fetch(entry,lang,10) || lang[9]*6>TABLE_LIMIT || !fetch(entry+10,pages,lang[9]*6)) {dos_close(file); return fail("Bad or missing Keyboard Definition File",EXIT_FILE);}
    unsigned count=lang[9];
    if(with_page) {
        unsigned i=0; while(i<count && rd16(pages+6*i)!=page) i++;
        if(i==count) {dos_close(file); return fail("Invalid code page specified",EXIT_PARAMETERS);}
    }
    /* CON's pages (DISPLAY.SYS's AD03h) and its selected one (AD02h). */
    unsigned con; u8 list[2*(4+DOS_CP_PREPARED_MAX)],current[4]; DosRegs q; int e=dos_open("CON",DOS_OPEN_RDWR,0,&con),selected=0;
    if(!e) {
        q=(DosRegs){.ax=0x440c,.bx=con,.cx=DOS_CP_CATEGORY<<8|DOS_CP_QUERY_LIST,.dx=(uintptr_t)list,.si=sizeof(list)};
        e=dos_call(&q);
        q=(DosRegs){.ax=0x440c,.bx=con,.cx=DOS_CP_CATEGORY<<8|DOS_CP_QUERY,.dx=(uintptr_t)current,.si=sizeof(current)};
        if(!e) selected=!dos_call(&q);
        dos_close(con);
    }
    if(e) {dos_close(file); return fail("Active code page not available from CON device",EXIT_CON);}
    u16 designated[2+DOS_CP_PREPARED_MAX]; unsigned designated_count=0,hardware=rd16(list+2),prepared=0;
    if(hardware>2) hardware=2;
    for(unsigned i=0;i<hardware;i++) designated[designated_count++]=rd16(list+4+2*i);
    prepared=rd16(list+4+2*hardware); if(prepared>DOS_CP_PREPARED_MAX) prepared=DOS_CP_PREPARED_MAX;
    for(unsigned i=0;i<prepared && 6+2*(hardware+i)+2<=sizeof(list);i++) designated[designated_count++]=rd16(list+6+2*(hardware+i));
    u16 active=with_page?(u16)page:selected?rd16(current+2):rd16(pages);
    unsigned found=0; for(unsigned i=0;i<designated_count;i++) if(designated[i]==active) found=1;
    if(!found) {dos_close(file); return fail("Code page specified has not been prepared",EXIT_NOT_PREPARED);}
    if(with_page && selected && rd16(current+2)!=page) say("Code page specified is inconsistent with the selected code page");
    /* TABLE_BUILD: the state logic, the common section, then a section for
     * each of CON's pages the language has. */
    unsigned i=0; while(i<count && rd16(pages+6*i)!=active) i++;
    if(i==count) {dos_close(file); print("Code page requested (%u) is not valid for given keyboard code\n",(unsigned long long)active); return EXIT_NOT_VALID;}
    u32 logic=rd32(lang+4),used=0,size=0; u8 word2[2]; int invalid=0,memory=0,ok=fetch(logic,word2,2);
    if(ok) {size=rd16(word2); ok=size>=4 && size<=sizeof(tables) && fetch(logic,tables,size); used=size;}
    if(ok) {ok=fetch(logic+used,word2,2); size=rd16(word2);}
    if(ok) {if(used+size>sizeof(tables)) memory=1; else ok=size>=4 && fetch(logic+used,tables+used,size); used+=size;}
    for(unsigned d=0;ok && !memory && d<designated_count;d++) {
        if(designated[d]==0xffff) continue;
        unsigned c=0; while(c<count && rd16(pages+6*c)!=designated[d]) c++;
        if(c==count) {invalid=1; continue;}
        u32 at=rd32(pages+6*c+2); ok=fetch(at,word2,2); size=rd16(word2);
        if(ok && used+size>sizeof(tables)) memory=1;
        else if(ok) {ok=size>=4 && fetch(at,tables+used,size); used+=size;}
    }
    dos_close(file);
    if(!ok) return fail("Bad or missing Keyboard Definition File",EXIT_FILE);
    DosKeybRequest r={.size=sizeof(r),.id=(u16)(positional_id || id_switch?id:0),.code_page=active,.tables=tables,.table_size=used};
    if(!positional_id) memcpy(r.language,given[0],2);
    e=memory?DE_NOMEM:app_dos->keyb(DOS_KEYB_LOAD,&r);
    if(e==DE_NOMEM) return fail("Unable to create KEYB table in resident memory",EXIT_MEMORY);
    if(e) return fail("Bad or missing Keyboard Definition File",EXIT_FILE);
    if(invalid) say("One or more CON code pages invalid for given keyboard code");
    return 0;
}
