/* SPDX-License-Identifier: GPL-2.0-or-later
 * Fiber exerciser: deep recursion across switches (register stack spills),
 * values kept in preserved integer/FP registers, and one DOS task context
 * per fiber selected at each switch (InDOS == 0).
 */
#include "runtime.h"
#include "fiber.h"
#define FIBERS 3
#define ROUNDS 4
#define STACK_BYTES 65536
#define BACKING_BYTES 65536
typedef struct {FiberContext ctx; unsigned index,done,errors,handle; u32 task; char dir[16];} Worker;
static FiberContext scheduler;
static Worker workers[FIBERS];
static EFI_BOOT_SERVICES *boot;
static unsigned passed,failed;
static void check(const char *name,int ok) {
    if(ok) passed++; else {failed++; print("[FAIL] %s\n",name);}
}
/* Raise TPL around the switch: firmware must not run callbacks while the
 * register stack is between two backing stores. */
static void switch_fiber(FiberContext *from,const FiberContext *to) {
    EFI_TPL old=boot->RaiseTPL(TPL_HIGH_LEVEL);
    fiber_switch(from,to);
    boot->RestoreTPL(old);
}
static u64 descend(Worker *w,unsigned depth,u64 a,u64 b,double f) {
    u64 x=a*3+b+depth,y=b^(a<<1); double g=f*1.5+depth;
    if(!depth) {
        if(w) switch_fiber(&w->ctx,&scheduler);
        return x+y+(u64)g;
    }
    u64 r=descend(w,depth-1,x,y,g);
    return r^(x+y+depth)^(u64)(g*2.0);
}
static void worker_main(void *arg) {
    Worker *w=arg;
    boot->RestoreTPL(TPL_APPLICATION); /* switch_fiber raised it for the first switch */
    int e=dos_mkdir(w->dir); check("worker-mkdir",!e);
    e=dos_chdir(w->dir); check("worker-chdir",!e);
    e=dos_open("LOG.TXT",DOS_OPEN_WRITE,1,&w->handle); check("worker-open",!e);
    for(unsigned round=0;round<ROUNDS;round++) {
        unsigned depth=300-w->index*70;
        u64 got=descend(w,depth,round+1,w->index+7,1.0+w->index);
        u64 want=descend(NULL,depth,round+1,w->index+7,1.0+w->index);
        if(got!=want) w->errors++;
        char cwd[DOS_PATH_MAX]; e=dos_getcwd(cwd);
        if(e || stricmp(cwd,w->dir)) w->errors++;
        char digit=(char)('0'+round); u32 n;
        e=dos_write(w->handle,&digit,1,&n); if(e || n!=1) w->errors++;
        switch_fiber(&w->ctx,&scheduler);
    }
    w->done=1;
    for(;;) switch_fiber(&w->ctx,&scheduler);
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    boot=st->BootServices;
    DosInfo info; dos_query(&info); u32 self=info.pid;
    char before[DOS_PATH_MAX]; dos_getcwd(before);
    for(unsigned i=0;i<FIBERS;i++) {
        Worker *w=&workers[i]; void *stack,*backing;
        w->index=i; strcopy(w->dir,sizeof(w->dir),"\\FIBER0"); w->dir[6]+=(char)i;
        check("stack-alloc",!dos_alloc(STACK_BYTES/16,&stack) && !dos_alloc(BACKING_BYTES/16,&backing));
        check("prepare",!fiber_prepare(&w->ctx,stack,STACK_BYTES,backing,BACKING_BYTES,worker_main,w));
        check("task-create",!app_dos->task_create(&w->task));
    }
    check("prepare-rejects-null",fiber_prepare(&workers[0].ctx,NULL,STACK_BYTES,NULL,0,worker_main,NULL)==DE_FUNCTION);
    unsigned switches=0,live=FIBERS;
    while(live) {
        live=0;
        for(unsigned i=0;i<FIBERS;i++) if(!workers[i].done) {
            if(app_dos->task_select(workers[i].task)) {check("task-select",0); return 1;}
            switch_fiber(&scheduler,&workers[i].ctx); switches++;
            if(app_dos->task_select(self)) {check("task-select-back",0); return 1;}
            if(!workers[i].done) live++;
        }
    }
    char after[DOS_PATH_MAX]; dos_getcwd(after);
    check("scheduler-cwd-unchanged",!stricmp(before,after));
    for(unsigned i=0;i<FIBERS;i++) {
        Worker *w=&workers[i];
        check("worker-results",!w->errors);
        check("task-destroy",!app_dos->task_destroy(w->task));
        char path[32]; strcopy(path,sizeof(path),w->dir); strappend(path,sizeof(path),"\\LOG.TXT");
        unsigned h; char text[8]={0}; u32 n;
        check("log-open",!dos_open(path,DOS_OPEN_READ,0,&h));
        check("log-read",!dos_read(h,text,sizeof(text),&n) && n==ROUNDS && !memcmp(text,"0123",4));
        dos_close(h); dos_remove(path,0); dos_remove(w->dir,1);
    }
    print("FIBERTEST: %u switches; %u passed, %u failed\n",(unsigned long long)switches,
          (unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
