/* SPDX-License-Identifier: GPL-2.0-or-later
 * TASKHOST: loads guest images as resident modules and runs their entries
 * as cooperative tasks, each with a fiber and a DOS task context.
 *   TASKHOST image[=arguments] ...
 */
#include "runtime.h"
#include "fiber.h"
#include "taskhost.h"
#define TASKS 8
#define STACK_BYTES 65536
#define BACKING_BYTES 65536
typedef struct {
    FiberContext context; u64 module; u32 task; int (*entry)(const TaskStart *);
    TaskStart start; char arguments[64]; int done,result; void *stack,*backing;
} Task;
static Task tasks[TASKS];
static unsigned task_count,running;
static int (*pending)(const TaskStart *);
static int registering;
static FiberContext scheduler;
static EFI_BOOT_SERVICES *boot;
static void switch_fiber(FiberContext *from,const FiberContext *to) {
    EFI_TPL old=boot->RaiseTPL(TPL_HIGH_LEVEL);
    fiber_switch(from,to);
    boot->RestoreTPL(old);
}
static int register_task(int (*entry)(const TaskStart *)) {
    if(!registering || pending || !entry) return DE_ACCESS;
    pending=entry; return 0;
}
static void yield(void) {switch_fiber(&tasks[running].context,&scheduler);}
static TaskHost host={TASK_HOST_VERSION,sizeof(TaskHost),register_task,yield};
/* A new fiber starts with the TPL switch_fiber raised; lower it as a switch
 * back into an older fiber does. */
static void task_main(void *arg) {
    Task *t=arg;
    boot->RestoreTPL(TPL_APPLICATION);
    t->result=t->entry(&t->start); t->done=1;
    for(;;) switch_fiber(&t->context,&scheduler);
}
static char *next_word(char **cursor) {
    char *p=*cursor; while(*p==' ' || *p=='\t') p++;
    if(!*p) return NULL;
    char *word=p; while(*p && *p!=' ' && *p!='\t') p++;
    if(*p) *p++=0;
    *cursor=p; return word;
}
/* The guest's initialization runs in its own DOS task so that what it
 * allocates there is released with the task. */
static int load(const char *spec,unsigned index,u32 self) {
    Task *t=&tasks[index]; char path[DOS_PATH_MAX]; unsigned n=0;
    while(spec[n] && spec[n]!='=' && n+1<sizeof(path)) {path[n]=spec[n]; n++;}
    path[n]=0; strcopy(t->arguments,sizeof(t->arguments),spec[n]=='='?spec+n+1:"");
    unsigned h; int e=dos_open(path,DOS_OPEN_READ,0,&h); if(e) return e;
    u32 size,pos,got; void *image=NULL;
    e=dos_seek(h,0,2,&size); if(!e) e=dos_alloc((size+15)/16,&image);
    if(!e) e=dos_seek(h,0,0,&pos);
    if(!e) {e=dos_read(h,image,size,&got); if(!e && got!=size) e=DE_IO;}
    dos_close(h);
    const IoServices *io=dos_io_services();
    if(!e) e=app_dos->task_create(&t->task);
    if(!e) e=app_dos->task_select(t->task);
    if(!e) {
        registering=1; pending=NULL;
        e=io->module_load(io->context,image,size,&t->module);
        registering=0;
        int back=app_dos->task_select(self); if(!e) e=back;
        if(!e && !pending) e=DE_FORMAT;
    }
    if(image) dos_free(image);
    if(e) return e;
    t->entry=pending; t->start=(TaskStart){sizeof(TaskStart),index,t->arguments};
    e=dos_alloc(STACK_BYTES/16,&t->stack); if(!e) e=dos_alloc(BACKING_BYTES/16,&t->backing);
    if(!e) e=fiber_prepare(&t->context,t->stack,STACK_BYTES,t->backing,BACKING_BYTES,task_main,t);
    return e;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    boot=st->BootServices; EFI_GUID guid=TASK_HOST_GUID; EFI_HANDLE self=image;
    if(EFI_ERROR(boot->InstallProtocolInterface(&self,&guid,EFI_NATIVE_INTERFACE,&host))) {print("TASKHOST: cannot publish\n"); return 1;}
    char tail[256],*cursor=tail; strcopy(tail,sizeof(tail),app_dos->command_tail());
    int failed=0; DosInfo info; dos_query(&info); u32 self_pid=info.pid;
    for(char *p;(p=next_word(&cursor));) {
        if(task_count==TASKS) {print("TASKHOST: too many tasks\n"); failed=1; break;}
        int e=load(p,task_count++,self_pid);
        if(e) {print("TASKHOST: %s failed (%u)\n",p,(unsigned long long)e); failed=1; break;}
    }
    for(unsigned live=task_count;live && !failed;) {
        live=0;
        for(unsigned i=0;i<task_count;i++) if(!tasks[i].done) {
            running=i;
            if(app_dos->task_select(tasks[i].task)) {failed=1; break;}
            switch_fiber(&scheduler,&tasks[i].context);
            app_dos->task_select(self_pid);
            if(!tasks[i].done) live++;
        }
    }
    const IoServices *io=dos_io_services();
    for(unsigned i=0;i<task_count;i++) {
        Task *t=&tasks[i];
        if(t->done) print("TASKHOST: task %u returned %u\n",(unsigned long long)i,(unsigned long long)t->result);
        if(t->task && app_dos->task_destroy(t->task)) failed=1;
        if(t->module && io->module_unload(io->context,t->module)) failed=1;
        if(t->stack) dos_free(t->stack);
        if(t->backing) dos_free(t->backing);
    }
    boot->UninstallProtocolInterface(self,&guid,&host);
    print("TASKHOST: %s\n",failed?"failed":"all tasks finished");
    return failed?1:0;
}
