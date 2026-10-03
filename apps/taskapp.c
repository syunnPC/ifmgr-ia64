/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest task for TASKHOST (subsystem 11). efi_main only registers the entry;
 * the host runs it later on a fiber with its own DOS task context.
 * Arguments: a one-letter name and a digit for the number of rounds.
 */
#include "runtime.h"
#include "taskhost.h"
static TaskHost *host;
static char *init_block;
static int entry(const TaskStart *start) {
    const char *a=start->arguments; unsigned errors=0;
    char name=a[0]?a[0]:'X'; unsigned rounds=a[0] && a[1]>='1' && a[1]<='9'?(unsigned)(a[1]-'0'):3;
    char dir[8]={'\\','T','A','S','K',name,0},cwd[DOS_PATH_MAX];
    if(dos_mkdir(dir) || dos_chdir(dir)) errors++;
    unsigned log,keep; u32 n;
    if(dos_open("LOG.TXT",DOS_OPEN_WRITE,1,&log)) errors++;
    /* Left open and allocated on purpose: destroying the task reaps them. */
    if(dos_open("C:\\README.TXT",DOS_OPEN_READ,0,&keep)) errors++;
    void *leak; if(dos_alloc(64,&leak)) errors++;
    for(unsigned round=0;round<rounds;round++) {
        print("TASKAPP %c%u\n",name,(unsigned long long)round);
        char digit=(char)('0'+round);
        if(dos_write(log,&digit,1,&n) || n!=1) errors++;
        host->yield();
        if(dos_getcwd(cwd) || stricmp(cwd,dir)) errors++;
    }
    if(memcmp(init_block,"INIT",4)) errors++;
    if(host->register_task(entry)!=DE_ACCESS) errors++;
    char text[10]={0};
    if(dos_close(log) || dos_open("LOG.TXT",DOS_OPEN_READ,0,&log)) errors++;
    else {if(dos_read(log,text,sizeof(text),&n) || n!=rounds || memcmp(text,"012345678",rounds)) errors++; dos_close(log);}
    if(dos_remove("LOG.TXT",0) || dos_chdir("\\") || dos_remove(dir,1)) errors++;
    if(errors) print("TASKAPP %c: %u errors\n",name,(unsigned long long)errors);
    return errors?100+(int)errors:(int)rounds;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    EFI_GUID guid=TASK_HOST_GUID;
    if(EFI_ERROR(st->BootServices->LocateProtocol(&guid,NULL,(void **)&host)) || host->version!=TASK_HOST_VERSION) {
        print("TASKAPP: needs TASKHOST\n"); return EFI_UNSUPPORTED;
    }
    if(dos_alloc(4,(void **)&init_block)) return EFI_OUT_OF_RESOURCES;
    memcpy(init_block,"INIT",4);
    return host->register_task(entry)?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
