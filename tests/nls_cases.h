/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shared host/native behavior checks. NLS_CALL returns the DOS carry error.
 */
static int nls_test_info(u16 country,u16 page,DosCountryInfo *out) {
    DosRegs r={.ax=0x6501,.bx=page,.dx=country,.cx=sizeof(*out),.di=(uintptr_t)out};
    return NLS_CALL(&r);
}
static int nls_test_country(u16 country) {
    DosRegs r={.ax=0x38ff,.bx=country,.dx=65535}; return NLS_CALL(&r);
}
static int nls_test_page(u16 page) {
    DosRegs r={.ax=0x6602,.bx=page}; return NLS_CALL(&r);
}
/* MODE CON CP PREPARE equivalent: AH=6602h needs CON to hold the page. */
static int nls_test_prepare(void) {
    unsigned con,file; u32 got; static u8 data[1024];
    u8 packet[10]={0,0,6,0,2,0,0x52,3,0xa4,3};
    NLS_CHECK(!dos_open("CON",2,0,&con) && !dos_open("C:\\EFI.CPI",0,0,&file));
    DosRegs r={.ax=0x440c,.bx=con,.cx=0x034c,.dx=(uintptr_t)packet,.si=sizeof(packet)};
    NLS_CHECK(!NLS_CALL(&r));
    for(;;) {
        NLS_CHECK(!dos_read(file,data,sizeof(data),&got)); if(!got) break;
        r=(DosRegs){.ax=0x4403,.bx=con,.cx=got,.dx=(uintptr_t)data}; NLS_CHECK(!NLS_CALL(&r) && r.ax==got);
    }
    r=(DosRegs){.ax=0x440c,.bx=con,.cx=0x034d}; NLS_CHECK(!NLS_CALL(&r));
    NLS_CHECK(!dos_close(file) && !dos_close(con)); return 0;
}
static int nls_test_parse(DosFcb *f,const char *name,unsigned status,int error) {
    memset(f,0,sizeof(*f));
    DosRegs r={.ax=0x2900,.si=(uintptr_t)name,.di=(uintptr_t)f};
    NLS_CHECK(!NLS_CALL(&r) && r.ax==status && r.bx==(u64)error);
    return 0;
}
static int nls_test_file(const char *path,const char *content) {
    unsigned h; u32 n; char data[16];
    NLS_CHECK(!dos_open(path,2,1,&h));
    NLS_CHECK(!dos_write(h,content,strlen(content),&n) && n==strlen(content));
    NLS_CHECK(!dos_close(h) && !dos_open(path,0,0,&h));
    NLS_CHECK(!dos_read(h,data,sizeof(data),&n) && n==strlen(content) && !memcmp(data,content,n));
    NLS_CHECK(!dos_close(h)); return 0;
}
static int nls_test_suite(void) {
    DosCountryInfo original,info;
    NLS_CHECK(!nls_test_info(65535,65535,&original));
    NLS_CHECK(original.size==sizeof(original));
    /* The fixtures boot US/437 or Japan/932. Save their state for the caller. */
    NLS_CHECK(!nls_test_page(437) && !nls_test_country(1));
    NLS_CHECK(!nls_test_prepare());
    /* Portugal/860 exists in COUNTRY.SYS, but CON has not prepared it. */
    NLS_CHECK(!nls_test_page(850) && !nls_test_country(351));
    DosRegs page={.ax=0x6601}; NLS_CHECK(!NLS_CALL(&page) && page.bx==850);
    NLS_CHECK(nls_test_page(860)==DOS_CP_SYSTEM_NOT_PREPARED);
    page=(DosRegs){.ax=0x6601}; NLS_CHECK(!NLS_CALL(&page) && page.bx==850);
    NLS_CHECK(!nls_test_country(1) && !nls_test_page(437));
    u8 storage[300],before[300]; memset(storage,0xcc,sizeof(storage)); memcpy(before,storage,sizeof(before));
    DosRegs r={.ax=0x6501,.bx=437,.dx=49,.cx=39,.di=(uintptr_t)(storage+1)};
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION && r.cx==40 && !memcmp(storage,before,sizeof(storage)));
    r=(DosRegs){.ax=0x6501,.bx=437,.dx=49,.cx=40,.di=(uintptr_t)(storage+1)};
    NLS_CHECK(!NLS_CALL(&r) && r.cx==40 && storage[0]==0xcc && storage[41]==0xcc);
    memcpy(&info,storage+1,sizeof(info));
    NLS_CHECK(info.country==49 && info.code_page==437 && info.date_order==1 && info.date_separator[0]=='.');
    r=(DosRegs){.ax=0x3800,.cx=40,.dx=(uintptr_t)(storage+1)};
    NLS_CHECK(!NLS_CALL(&r) && r.bx==1 && r.cx==40);
    r=(DosRegs){.ax=0x38ff,.bx=65536,.dx=65535};
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION);
    NLS_CHECK(nls_test_country(999)==DE_NOFILE && nls_test_page(999)==DE_NOFILE);
    NLS_CHECK(!nls_test_info(65535,65535,&info) && info.country==1 && info.code_page==437);
    static const u16 pairs[][3]={
        {1,437,850},{44,437,850},{33,437,850},{49,437,850},{34,850,437},
        {39,437,850},{46,437,850},{45,850,865},{41,850,437},{47,850,865},
        {31,437,850},{32,850,437},{358,850,437},{972,862,850},{2,863,850},
        {785,864,850},{351,850,860},{3,850,437},{61,437,850},{81,932,437},
        {82,934,437},{86,936,437},{88,938,437}
    };
    const unsigned kinds[]={2,4,5,6,7},lengths[]={128,128,22,256,0};
    for(unsigned i=0;i<ARRAY_SIZE(pairs);i++) for(unsigned page=1;page<3;page++) {
        NLS_CHECK(!nls_test_info(pairs[i][0],pairs[i][page],&info));
        NLS_CHECK(info.country==pairs[i][0] && info.code_page==pairs[i][page]);
        for(unsigned j=0;j<ARRAY_SIZE(kinds);j++) {
            memset(storage,0xcc,sizeof(storage));
            r=(DosRegs){.ax=0x6500|kinds[j],.bx=pairs[i][page],.dx=pairs[i][0],.cx=256,.di=(uintptr_t)(storage+1)};
            NLS_CHECK(!NLS_CALL(&r) && r.cx<=256 && (!lengths[j] || r.cx==lengths[j]));
            NLS_CHECK(storage[0]==0xcc && storage[r.cx+1]==0xcc);
            if(kinds[j]==7) NLS_CHECK(r.cx>=2 && !storage[r.cx-1] && !storage[r.cx]);
        }
    }
    r=(DosRegs){.ax=0x6506,.bx=437,.dx=1,.cx=255,.di=(uintptr_t)storage};
    memcpy(before,storage,sizeof(storage));
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION && r.cx==256 && !memcmp(storage,before,sizeof(storage)));
    r=(DosRegs){.ax=0x6503,.bx=437,.dx=1,.cx=256,.di=(uintptr_t)storage};
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION);
    r=(DosRegs){.ax=0x6501,.bx=UINT64_MAX,.dx=UINT64_MAX,.cx=sizeof(info),.di=(uintptr_t)&info};
    NLS_CHECK(!NLS_CALL(&r) && info.country==1 && info.code_page==437);
    r=(DosRegs){.ax=0x6520,.dx=0x123481}; NLS_CHECK(!NLS_CALL(&r) && r.dx==0x12349a);
    NLS_CHECK(!nls_test_page(850));
    r=(DosRegs){.ax=0x6520,.dx=0x81}; NLS_CHECK(!NLS_CALL(&r) && r.dx=='U');
    NLS_CHECK(!nls_test_page(437));
    NLS_CHECK(!dos_mkdir("C:\\NLSWORK") && !dos_chdir("C:\\NLSWORK"));
    NLS_CHECK(!nls_test_file("\x81.bin","upper"));
    unsigned h; NLS_CHECK(!dos_open("\x9a.BIN",0,0,&h) && !dos_close(h));
    NLS_CHECK(!dos_remove("\x9a.BIN",0));
    NLS_CHECK(!nls_test_country(81));
    NLS_CHECK(!nls_test_info(65535,65535,&info) && info.country==81 && info.code_page==437);
    NLS_CHECK(!nls_test_page(932));
    r=(DosRegs){.ax=0x6300}; NLS_CHECK(!NLS_CALL(&r) && r.cx==6);
    const u8 *ranges=(const u8 *)(uintptr_t)r.si;
    NLS_CHECK(!memcmp(ranges,"\x81\x9f\xe0\xfc\0\0",6));
    u8 text[]={'a',0x81,'a',0x81,'\\','z',0};
    r=(DosRegs){.ax=0x6522,.cx=sizeof(text),.dx=(uintptr_t)text};
    NLS_CHECK(!NLS_CALL(&r) && !memcmp(text,"A\x81""a\x81\\Z",sizeof(text)));
    u8 short_text[]={'a',0x81};
    r=(DosRegs){.ax=0x65a1,.cx=sizeof(short_text),.dx=(uintptr_t)short_text};
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION && short_text[0]=='a' && short_text[1]==0x81);
    r=(DosRegs){.ax=0x6522,.cx=sizeof(short_text),.dx=(uintptr_t)short_text};
    NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION && short_text[0]=='a');
    r=(DosRegs){.ax=0x6520,.dx=0x81}; NLS_CHECK(NLS_CALL(&r)==DE_FUNCTION);
    char path[DOS_PATH_MAX];
    NLS_CHECK(!dos_mkdir("\x81\\") && !dos_chdir("\x81\\"));
    NLS_CHECK(!dos_canonical("..",path) && !strcmp(path,"C:\\NLSWORK"));
    NLS_CHECK(!nls_test_file("\x81""a.txt","lower"));
    NLS_CHECK(!nls_test_file("\x81""A.txt","upper"));
    NLS_CHECK(!dos_chdir("..") && !dos_chdir("\x81\\"));
    NLS_CHECK(!dos_open("\x81""a.TXT",0,0,&h)); u32 got;
    NLS_CHECK(!dos_read(h,storage,16,&got) && got==5 && !memcmp(storage,"lower",5));
    NLS_CHECK(!dos_close(h));
    NLS_CHECK(!nls_test_file("\x81\\CON.TXT","disk"));
    NLS_CHECK(!nls_test_file("\xe5""a.DAT","escape"));
    DosFind found;
    NLS_CHECK(!dos_find_first("\x81""a.*",0,&found) && !memcmp(found.name,"\x81""a.TXT",7));
    NLS_CHECK(dos_find_next(&found)==DE_NOMORE);
    NLS_CHECK(!dos_find_first("\xe5""a.*",0,&found) && !memcmp(found.name,"\xe5""a.DAT",7));
    NLS_CHECK(!dos_canonical("\x81\\CON.txt",path) &&
        !strcmp(path,"C:\\NLSWORK\\" "\x81\\" "\\" "\x81\\CON.TXT"));
    NLS_CHECK(dos_open("bad\x81",2,1,&h)==DE_PATH);
    NLS_CHECK(dos_open("ABCDEFG\x81""a",2,1,&h)==DE_PATH);
    NLS_CHECK(dos_open("OK.AB\x81""a",2,1,&h)==DE_PATH);
    DosFcb f;
    NLS_CHECK(!nls_test_parse(&f,"ab\x81\\c.txt",0,0) && !memcmp(f.bytes+1,"AB\x81\\C   TXT",11));
    NLS_CHECK(!nls_test_parse(&f,"ABCDEFG\x81""aZ.TXT",0,0) && !memcmp(f.bytes+1,"ABCDEFG TXT",11));
    NLS_CHECK(!nls_test_parse(&f,"BAD\x81",255,DE_PATH));
    NLS_CHECK(!nls_test_parse(&f,"\xe5""a.DAT",0,0));
    r=(DosRegs){.ax=0x0f00,.dx=(uintptr_t)&f}; NLS_CHECK(!NLS_CALL(&r) && !r.ax && !r.bx && rd32(f.bytes+16)==6);
    r=(DosRegs){.ax=0x1000,.dx=(uintptr_t)&f}; NLS_CHECK(!NLS_CALL(&r) && !r.ax && !r.bx);
    NLS_CHECK(!dos_rename("\x81""a.TXT","\x81|.TXT"));
    NLS_CHECK(!dos_find_first("\x81|.*",0,&found) && !memcmp(found.name,"\x81|.TXT",7));
    NLS_CHECK(!dos_remove("\x81|.TXT",0) && !dos_remove("\x81""A.TXT",0));
    NLS_CHECK(!dos_remove("\xe5""a.DAT",0) && !dos_remove("\x81\\CON.TXT",0));
    NLS_CHECK(!dos_chdir("..") && !dos_remove("\x81\\",1));
    NLS_CHECK(!dos_chdir("C:\\") && !dos_remove("C:\\NLSWORK",1));
    NLS_CHECK(!nls_test_file("C:\\NLSKEEP.DAT","NLS \x81\\\xe5""a"));
    NLS_CHECK(!nls_test_page(437) && !nls_test_country(original.country) && !nls_test_page(original.code_page));
    NLS_CHECK(!nls_test_info(65535,65535,&info) && !memcmp(&info,&original,sizeof(info)));
    return 0;
}
