/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
static unsigned passed,failed;
#define NLS_CHECK(x) do {if(!(x)) {print("[FAIL] NLS line %u: %s\n",(unsigned long long)__LINE__,#x); failed++; return -1;} passed++;} while(0)
#define NLS_CALL app_call
#include "../tests/nls_cases.h"
static int client(void) {
    DosCountryInfo info; u16 page,boot; u32 size=0; u8 data[256];
    NLS_CHECK(!dos_country_info(65535,65535,&info));
    NLS_CHECK(!dos_code_page(&page,&boot) && page==info.code_page);
    NLS_CHECK(!dos_nls_table(1,850,2,data,sizeof(data),&size) && size==128 && data[1]=='U');
    NLS_CHECK(dos_nls_table(1,437,6,NULL,0,&size)==DE_FUNCTION && size==256);
    NLS_CHECK(!dos_nls_case(data,0,0) && dos_nls_case(data,0,2)==DE_FUNCTION);
    NLS_CHECK(dos_country_info(1,437,NULL)==DE_FUNCTION);
    NLS_CHECK(dos_code_page(NULL,&boot)==DE_FUNCTION);
    NLS_CHECK(dos_country_set(0)==DE_FUNCTION && dos_code_page_set(0)==DE_FUNCTION);
    NLS_CHECK(!dos_code_page_set(437) && !dos_country_set(1));
    NLS_CHECK(!dos_country_set(info.country) && !dos_code_page_set(info.code_page));
    NLS_CHECK(!dos_code_page(&page,&boot) && page==info.code_page);
    return 0;
}
static int verify(void) {
    unsigned h; u32 got; u8 data[16];
    NLS_CHECK(!dos_open("C:\\NLSKEEP.DAT",0,0,&h));
    NLS_CHECK(!dos_read(h,data,sizeof(data),&got) && got==8 && !memcmp(data,"NLS \x81\\\xe5""a",8));
    NLS_CHECK(!dos_close(h));
    DosCountryInfo info; NLS_CHECK(!dos_country_info(65535,65535,&info));
    NLS_CHECK(info.country==81 && info.code_page==932);
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS e=app_init(st); if(EFI_ERROR(e)) return e;
    char cwd[DOS_PATH_MAX]; if(dos_canonical("C:",cwd)) return 1;
    if(!strcmp(app_dos->command_tail(),"verify")) verify();
    else if(!nls_test_suite()) client();
    if(dos_chdir(cwd)) failed++;
    print("NLSTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
