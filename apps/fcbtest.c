/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
#define FCB_CHECK(x) do {if(!(x)) {print("[FAIL] FCB line %u: %s\n",(unsigned long long)__LINE__,#x); failed++; return -1;} passed++;} while(0)
#define FCB_CALL app_call
#define FCB_DIAGNOSTIC(fn,want,status,error) print("FCB function %x: expected %u, status %u, error %u\n",(unsigned long long)(fn),(unsigned long long)(want),(unsigned long long)(status),(unsigned long long)(error))
#include "../tests/fcb_cases.h"
static int verify(void) {
    FCB_CHECK(!dos_chdir("C:\\"));
    DosFcb f; FCB_CHECK(!fcb_test_name(&f,"c:fcbkeep.dat"));
    FCB_CHECK(!fcb_test_call(0x0f,&f,NULL,0) && rd32(f.bytes+16)==5 && rd16(f.bytes+20)==0x285d && rd16(f.bytes+22)==0x645c);
    void *saved; u32 capacity; FCB_CHECK(!dos_get_dta(&saved,&capacity));
    u8 data[128]; FCB_CHECK(!dos_set_dta(data,sizeof(data))); unsigned status;
    FCB_CHECK(!dos_fcb_call(0x14,&f,NULL,&status) && status==3 && !memcmp(data,"fcb!!",5));
    for(unsigned i=5;i<128;i++) FCB_CHECK(!data[i]);
    FCB_CHECK(!dos_fcb_call(0x10,&f,NULL,&status) && !status);
    FCB_CHECK(!dos_set_dta(saved,capacity));
    return 0;
}
static int client(void) {
    void *saved,*pointer; u32 capacity,size; FCB_CHECK(!dos_get_dta(&saved,&capacity));
    u8 data[256]; FCB_CHECK(!dos_set_dta(data,sizeof(data)));
    DosFind find; FCB_CHECK(!dos_find_first("C:\\FCBKEEP.DAT",0,&find));
    FCB_CHECK(!dos_get_dta(&pointer,&size) && pointer==data && size==sizeof(data));
    FCB_CHECK(dos_find_first("C:\\FCBNOEXI.BIN",0,&find)==DE_NOMORE);
    FCB_CHECK(!dos_get_dta(&pointer,&size) && pointer==data && size==sizeof(data));
    DosFcb f={0}; const char *text="C:NUL",*end=text+5; unsigned status; u32 records=2;
    FCB_CHECK(!dos_fcb_parse(&text,&f,0,&status) && !status && text==end);
    FCB_CHECK(!dos_fcb_call(0x0f,&f,NULL,&status) && !status);
    FCB_CHECK(!dos_fcb_call(0x28,&f,&records,&status) && !status && records==2);
    FCB_CHECK(!dos_fcb_call(0x10,&f,NULL,&status) && !status);
    FCB_CHECK(!dos_set_dta(saved,capacity)); return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    char cwd[DOS_PATH_MAX]; if(dos_canonical("C:",cwd)) return 1;
    if(!strcmp(app_dos->command_tail(),"verify")) verify();
    else if(!fcb_test_suite()) client();
    if(dos_chdir(cwd)) failed++;
    print("FCBTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
