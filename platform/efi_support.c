/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "efi_support.h"

void efi_output(EFI_SYSTEM_TABLE *st,const void *data,size_t length) {
    const u8 *p=data; CHAR16 buf[128]; size_t n=0;
    while(length--) {
        if(*p=='\n') buf[n++]='\r';
        if(*p) buf[n++]=*p;
        p++;
        if(n>=125) {buf[n]=0; st->ConOut->OutputString(st->ConOut,buf); n=0;}
    }
    if(n) {buf[n]=0; st->ConOut->OutputString(st->ConOut,buf);}
}
void efi_output_text(EFI_SYSTEM_TABLE *st,const u16 *text,size_t length) {
    CHAR16 buf[128]; size_t n=0;
    while(length--) {
        u16 c=*text++;
        if(c=='\n') buf[n++]='\r';
        if(c) buf[n++]=c;
        if(n>=125) {buf[n]=0; st->ConOut->OutputString(st->ConOut,buf); n=0;}
    }
    if(n) {buf[n]=0; st->ConOut->OutputString(st->ConOut,buf);}
}
int efi_dos_error(EFI_STATUS status) {
    if(!EFI_ERROR(status)) return 0;
    if(status==EFI_WRITE_PROTECTED) return DE_READONLY;
    if(status==EFI_OUT_OF_RESOURCES) return DE_NOMEM;
    if(status==EFI_NOT_FOUND) return DE_NOFILE;
    if(status==EFI_UNSUPPORTED || status==EFI_LOAD_ERROR) return DE_FORMAT;
    if(status==EFI_NOT_READY || status==EFI_NO_MEDIA) return DE_NOTREADY;
    if(status==EFI_MEDIA_CHANGED) return DE_CHANGED;
    return DE_IO;
}
unsigned efi_path_bytes(const void *path) {
    const u8 *p=path;
    for(unsigned n=0;n<4096;) {
        unsigned length=rd16(p+n+2);
        if(length<4 || length>4096-n) return 0;
        if(p[n]==END_DEVICE_PATH_TYPE && p[n+1]==END_ENTIRE_DEVICE_PATH_SUBTYPE) return n;
        n+=length;
    }
    return 0;
}
/* Bootstrap loads use firmware's filesystem loader. Runtime DOS file I/O does
 * not: MSDOS.SYS owns FAT and uses IO.SYS Block I/O. Preserve the device path so
 * each loaded system image can identify exactly the same boot volume. */
EFI_STATUS efi_load_sibling(EFI_SYSTEM_TABLE *st,EFI_HANDLE parent,
                           const CHAR16 *name,EFI_HANDLE *child) {
    EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *loaded; EFI_DEVICE_PATH *path;
    EFI_BOOT_SERVICES *bs=st->BootServices;
    EFI_STATUS e=bs->HandleProtocol(parent,&loaded_guid,(void **)&loaded);
    if(EFI_ERROR(e)) return e;
    e=bs->HandleProtocol(loaded->DeviceHandle,&path_guid,(void **)&path);
    if(EFI_ERROR(e)) return e;
    size_t prefix=0,chars=0;
    while(chars<128 && name[chars]) chars++;
    if(chars==128) return EFI_INVALID_PARAMETER;
    const u8 *p=(const u8 *)path;
    while(prefix<4096) {
        u16 length=rd16(p+prefix+2);
        if(length<4 || length>4096-prefix) return EFI_LOAD_ERROR;
        if(p[prefix]==0x7f) break;
        prefix+=length;
    }
    if(prefix>=4096) return EFI_LOAD_ERROR;
    size_t node_size=4+(chars+1)*2,bytes=prefix+node_size+4;
    u8 *full;
    e=bs->AllocatePool(EfiLoaderData,bytes,(void **)&full); if(EFI_ERROR(e)) return e;
    memcpy(full,path,prefix); full[prefix]=4; full[prefix+1]=4;
    wr16(full+prefix+2,node_size); memcpy(full+prefix+4,name,(chars+1)*2);
    memcpy(full+prefix+node_size,"\x7f\xff\x04\x00",4);
    e=bs->LoadImage(FALSE,parent,(EFI_DEVICE_PATH *)full,NULL,0,child);
    bs->FreePool(full); return e;
}
