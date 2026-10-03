/* SPDX-License-Identifier: GPL-2.0-or-later
 * Resident-program and two-step EXEC exerciser. Built twice: TSRTEST.EXE is a
 * subsystem-11 image that may stay resident, TSRAPP.EFI an ordinary
 * application that AH=31h must refuse.
 *   install  allocate DOS memory, publish a query protocol, AH=31h
 *   query    call the resident code and read its DOS memory
 *   exec     AH=4B01h + 4B80h (runs HELLO.EFI), then 4B01h + 4B81h
 *   overlay  AH=4B03h README.TXT
 */
#include "runtime.h"
#define TSR_GUID {0x3c9d1a62,0x51e0,0x4a5b,{0x8e,0x11,0x6f,0x72,0x73,0x72,0x00,0x01}}
typedef struct {u32 magic; char *block; int (*query)(char *,u32);} TsrProtocol;
static TsrProtocol protocol;
static EFI_HANDLE self;
static EFI_BOOT_SERVICES *boot;
static int resident_query(char *out,u32 capacity) {
    if(capacity<9) return DE_FUNCTION;
    memcpy(out,protocol.block,8); out[8]=0; return 0;
}
static EFI_STATUS EFIAPI resident_unload(EFI_HANDLE image) {
    EFI_GUID guid=TSR_GUID; (void)image;
    EFI_STATUS e=boot->UninstallProtocolInterface(self,&guid,&protocol);
    const IoServices *io=dos_io_services();
    if(io) {const char *text="TSRTEST: unload hook\n"; io->console_write(io->context,text,strlen(text));}
    return e;
}
static int install(void) {
    EFI_GUID guid=TSR_GUID; TsrProtocol *existing;
    if(!EFI_ERROR(boot->LocateProtocol(&guid,NULL,(void **)&existing))) {print("TSRTEST: already resident\n"); return 1;}
    void *block; int e=dos_alloc(4,&block); if(e) {print("TSRTEST: no memory (%u)\n",(unsigned long long)e); return 1;}
    memcpy(block,"RESIDENT",8);
    protocol=(TsrProtocol){0x52535424,block,resident_query};
    EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID; EFI_LOADED_IMAGE *loaded=NULL; EFI_IMAGE_UNLOAD previous=NULL;
    if(EFI_ERROR(boot->InstallProtocolInterface(&self,&guid,EFI_NATIVE_INTERFACE,&protocol))) {print("TSRTEST: protocol failed\n"); return 1;}
    /* A resident image releases firmware resources from its Unload handler. */
    if(!EFI_ERROR(boot->HandleProtocol(self,&loaded_guid,(void **)&loaded))) {previous=loaded->Unload; loaded->Unload=resident_unload;}
    print("TSRTEST: going resident\n");
    DosRegs r={.ax=0x3105}; e=dos_call(&r);
    /* Only a refused request returns. */
    if(loaded) loaded->Unload=previous;
    boot->UninstallProtocolInterface(self,&guid,&protocol); dos_free(block);
    print("TSRTEST: residency refused (%u)\n",(unsigned long long)e); return 2;
}
static int query(void) {
    EFI_GUID guid=TSR_GUID; TsrProtocol *p; char text[16];
    if(EFI_ERROR(boot->LocateProtocol(&guid,NULL,(void **)&p))) {print("TSRTEST: not resident\n"); return 1;}
    if(p->magic!=0x52535424 || p->query(text,sizeof(text))) {print("TSRTEST: damaged\n"); return 1;}
    print("TSRTEST: query %s\n",text); return 0;
}
static int exec_steps(void) {
    DosExecLoad load={.size=sizeof(load),.tail="from-4B01"};
    DosRegs r={.ax=0x4b01,.dx=(uintptr_t)"C:\\HELLO.EFI",.bx=(uintptr_t)&load};
    int e=dos_call(&r); if(e) {print("TSRTEST: 4B01h failed (%u)\n",(unsigned long long)e); return 1;}
    print("TSRTEST: loaded pid %u, entry %s image\n",(unsigned long long)load.pid,
          load.entry>load.base && load.entry<load.base+load.image_size?"inside":"outside");
    r=(DosRegs){.ax=0x4b80,.bx=load.token}; e=dos_call(&r);
    if(e) {print("TSRTEST: 4B80h failed (%u)\n",(unsigned long long)e); return 1;}
    r=(DosRegs){.ax=0x4d00}; dos_call(&r); print("TSRTEST: started, exit %u\n",(unsigned long long)(r.ax&255));
    DosExecLoad other={.size=sizeof(other)};
    r=(DosRegs){.ax=0x4b01,.dx=(uintptr_t)"C:\\HELLO.EFI",.bx=(uintptr_t)&other}; e=dos_call(&r);
    if(!e) {r=(DosRegs){.ax=0x4b81,.bx=other.token}; e=dos_call(&r);}
    if(e) {print("TSRTEST: discard failed (%u)\n",(unsigned long long)e); return 1;}
    r=(DosRegs){.ax=0x4b80,.bx=other.token}; e=dos_call(&r);
    print("TSRTEST: discarded (%u)\n",(unsigned long long)e); return 0;
}
static int overlay(void) {
    static char buffer[4096]; DosOverlay o={.size=sizeof(o),.capacity=sizeof(buffer)-1,.buffer=buffer};
    DosRegs r={.ax=0x4b03,.dx=(uintptr_t)"C:\\README.TXT",.bx=(uintptr_t)&o}; int e=dos_call(&r);
    if(e) {print("TSRTEST: overlay failed (%u)\n",(unsigned long long)e); return 1;}
    buffer[o.loaded]=0; char *end=strchr(buffer,'\n'); if(end) *end=0;
    print("TSRTEST: overlay %u bytes: %s\n",(unsigned long long)o.loaded,buffer); return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    self=image; boot=st->BootServices;
    const char *tail=app_dos->command_tail(); while(*tail==' ') tail++;
    int e;
    if(!stricmp(tail,"install")) e=install();
    else if(!stricmp(tail,"query")) e=query();
    else if(!stricmp(tail,"exec")) e=exec_steps();
    else if(!stricmp(tail,"overlay")) e=overlay();
    else {print("usage: TSRTEST install|query|exec|overlay\n"); e=1;}
    DosRegs r={.ax=0x4c00|(unsigned)e}; dos_call(&r);
    return EFI_SUCCESS;
}
