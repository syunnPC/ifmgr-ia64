/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos.h"
#include "device.h"
#include "console.h"
static const IoServices *io;
static int has(size_t end) {return io->size>=end;}
#define HAS_MEMBER(m) (has(offsetof(IoServices,m)+sizeof(io->m)) && io->m)
void dos_bind_io(const IoServices *services) {io=services;}
const IoServices *platform_io_services(void) {return io;}
int platform_module_load(const void *image,u32 size,u64 *token) {
    if(!HAS_MEMBER(module_unload) || !io->module_load) return DE_FUNCTION;
    return io->module_load(io->context,image,size,token);
}
int platform_module_unload(u64 token) {return io->module_unload(io->context,token);}
int platform_fat_page(void **out) {return io->alloc_pages(io->context,1,out);}
void platform_fat_free_page(void *p) {io->free_pages(io->context,p,1);}
void con_write(const void *p,size_t n) {io->console_write(io->context,p,n);}
void con_puts(const char *s) {con_write(s,strlen(s));}
void platform_console_text(const u16 *text,size_t n) {
    if(HAS_MEMBER(console_write_text)) {
        io->console_write_text(io->context,text,n); return;
    }
    /* An older IO.SYS writes Latin-1 bytes; other code units become '?'. */
    while(n) {
        u8 bytes[64]; size_t take=MIN(n,sizeof(bytes));
        for(size_t i=0;i<take;i++) bytes[i]=text[i]<256?(u8)text[i]:'?';
        con_write(bytes,take); text+=take; n-=take;
    }
}
int con_getch(void) {return io->console_read(io->context);}
int platform_console_key(IoEvent *event,unsigned flags) {
    if(HAS_MEMBER(console_key))
        return io->console_key(io->context,event,flags);
    if(flags!=IO_KEY_WAIT) return DE_NOTREADY;
    int c=con_getch(); if(c<0) return -c;
    memset(event,0,sizeof(*event)); event->type=IO_EVENT_KEY; event->unicode=c; return 0;
}
/* Up to ms for input: the firmware's wait, or a pause. */
void platform_wait(u32 ms) {
    if(HAS_MEMBER(wait) && !io->wait(io->context,ms)) return;
    if(HAS_MEMBER(stall_us)) io->stall_us(io->context,ms*1000);
}
void con_clear(void) {io->console_clear(io->context);}
int platform_exec(const void *p,u32 n,const char *tail,unsigned *code) {
    return io->exec(io->context,p,n,tail,code);
}
void platform_exit(unsigned code) {io->exit_image(io->context,code);}
void platform_shutdown(void) {io->shutdown(io->context);}
void platform_restart(void) {
    if(HAS_MEMBER(restart)) io->restart(io->context);
    con_puts("MSDOS.SYS: restart is not supported by IO.SYS\n");
}
int platform_exit_resident(unsigned code,u64 *token) {
    return HAS_MEMBER(exit_resident)?io->exit_resident(io->context,code,token):DE_FUNCTION;
}
int platform_image_load(const void *image,u32 size,u64 *token,u64 *base,u64 *bytes) {
    return HAS_MEMBER(image_load)?io->image_load(io->context,image,size,token,base,bytes):DE_FUNCTION;
}
int platform_image_start(u64 token,const char *tail,unsigned *result) {
    return HAS_MEMBER(image_start)?io->image_start(io->context,token,tail,result):DE_FUNCTION;
}
int platform_image_discard(u64 token) {
    return HAS_MEMBER(image_discard)?io->image_discard(io->context,token):DE_FUNCTION;
}
int platform_text_available(void) {
    return (io->capabilities&IO_CAP_TEXT_SCREEN) && HAS_MEMBER(text_query) && HAS_MEMBER(text_locate) &&
           HAS_MEMBER(text_attribute) && HAS_MEMBER(text_erase) && HAS_MEMBER(text_mode);
}
int platform_text_query(IoTextScreen *s) {return HAS_MEMBER(text_query)?io->text_query(io->context,s):DE_FUNCTION;}
int platform_text_locate(u32 column,u32 row) {return HAS_MEMBER(text_locate)?io->text_locate(io->context,column,row):DE_FUNCTION;}
int platform_text_attribute(u32 attribute,u32 flags) {return HAS_MEMBER(text_attribute)?io->text_attribute(io->context,attribute,flags):DE_FUNCTION;}
int platform_text_erase(u32 column,u32 row,u32 cells) {return HAS_MEMBER(text_erase)?io->text_erase(io->context,column,row,cells):DE_FUNCTION;}
int platform_text_mode(u32 mode) {return HAS_MEMBER(text_mode)?io->text_mode(io->context,mode):DE_FUNCTION;}
