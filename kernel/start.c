/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos.h"
#include "config.h"
#include "device.h"
static DosApi api={
    .version=DOS_ABI_VERSION,.size=sizeof(DosApi),
    .int21=dos_int21,.command_tail=dos_command_tail,.query=dos_query,
    .set_errorlevel=dos_set_errorlevel,.console_clear=con_clear,.shutdown=dos_shutdown,
    .task_create=dos_task_create,.task_select=dos_task_select,.task_destroy=dos_task_destroy,
    .env_get=dos_env_get,.env_set=dos_env_set,.env_list=dos_env_list,
    .capabilities=DOS_CAP_PARTIAL_IO|DOS_CAP_DRIVES|DOS_CAP_CONSOLE|DOS_CAP_CRITICAL|DOS_CAP_DEVICES|DOS_CAP_BLOCK_DRIVERS|DOS_CAP_DATETIME|DOS_CAP_FCB|DOS_CAP_NLS|DOS_CAP_DISK_IO|DOS_CAP_REDIRECT|DOS_CAP_DRIVE_MAP|DOS_CAP_APPEND,.drive_info=dos_drive_info,
    .break_handler=dos_break_handler,.last_exit=dos_last_exit,
    .critical_handler=dos_critical_handler,.extended_error=dos_extended_error,
    .device_register=dos_device_register,.device_info=dos_device_info,
    .block_register=dos_block_register,.block_info=dos_block_info,.datetime=dos_get_datetime,
    .disk_read=dos_disk_read,.disk_write=dos_disk_write,.volume_lock=dos_volume_lock,
    .physical_info=dos_physical_info,.physical_read=dos_physical_read,.physical_write=dos_physical_write,
    .restart=dos_restart,.redirect=dos_redirect,.unredirect=dos_unredirect,
    .switch_hook=dos_switch_hook,.switch_away=dos_switch_away,
    .full_path=dos_full_path,.drive_map=dos_drive_map,.assign=dos_assign,
    .append=dos_append,.append_task=dos_append_task,.installed=dos_installed,
    .print=dos_print,.idle=dos_idle,.keyb=dos_keyb,.keyb_key=dos_keyb_key,.program_path=dos_program_path,
    .arena=dos_arena_block
};
int dos_run(const IoServices *io) {
    if(!io || io->version!=IO_ABI_VERSION || io->size<IO_SERVICES_V1_SIZE) return DE_FORMAT;
    dos_bind_io(io);
    if(io->size<offsetof(IoServices,console_key)+sizeof(io->console_key) || !io->console_key)
        api.capabilities&=~DOS_CAP_CONSOLE;
    if(io->size>=offsetof(IoServices,disk_location)+sizeof(io->disk_location) && io->physical_count && io->physical_info)
        api.capabilities|=DOS_CAP_PHYSICAL;
    if(io->size>=offsetof(IoServices,image_discard)+sizeof(io->image_discard) && io->exit_resident && io->image_load)
        api.capabilities|=DOS_CAP_RESIDENT;
    const char *what=""; /* the shell, when it is what failed */
    void *memory; const u32 pages=DOS_MEMORY_BYTES/4096;
    int e=io->alloc_pages(io->context,pages,&memory); if(e) return e;
    e=dos_init(&io->boot_disk,memory); if(e) goto out;
    e=dos_attach_disks(io); if(e) goto out;
    DosConfig config;
    e=config_load(&config); if(e) goto out;
    e=dos_set_files(config.files); if(e) goto out;
    dos_set_break_check(config.break_check);
    e=dos_env_set("COMSPEC",config.shell); if(e) goto out;
    api.io=io;
    e=io->publish_dos(io->context,&api); if(e) goto out;
    for(unsigned i=0;i<config.device_count;i++) {
        DosConfigDevice *d=&config.devices[i]; if(d->install) continue;
        int loaded=dos_load_driver(d->path,d->tail);
        if(loaded) {config.warnings++; print("CONFIG.SYS line %u: DEVICE %s failed (%u)\n",(unsigned long long)d->line,d->path,(unsigned long long)loaded);}
    }
    /* INSTALL= programs, as DOS 4 runs them: after every driver, before the
     * shell (resident ones stay, AH=31h). */
    for(unsigned i=0;i<config.device_count;i++) {
        DosConfigDevice *d=&config.devices[i]; if(!d->install) continue;
        int ran=dos_exec(d->path,d->tail);
        if(ran) {config.warnings++; print("CONFIG.SYS line %u: INSTALL %s failed (%u)\n",(unsigned long long)d->line,d->path,(unsigned long long)ran);}
    }
    int bound=dos_bind_standard_devices();
    if(bound) print("MSDOS.SYS: standard device binding failed (%u)\n",(unsigned long long)bound);
    e=dos_exec(config.shell,config.tail);
    if(e) what=config.shell;
    int finished=dos_finish_drivers(); if(!e) e=finished;
    while(dos_drivers_pending()) {
        con_puts("MSDOS.SYS: driver unload failed. [R]etry or [S]hut down: ");
        int key=con_getch(); con_puts("\n");
        if(key=='s' || key=='S') platform_shutdown();
        else if(key=='r' || key=='R') dos_finish_drivers();
    }
    int unpublish=io->publish_dos(io->context,NULL); if(!e) e=unpublish;
out:
    if(e) print("MSDOS.SYS: %s%s%s (%u)\n",what,*what?": ":"",dos_error(e),(unsigned long long)e);
    io->free_pages(io->context,memory,pages);
    return e;
}
