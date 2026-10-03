/* SPDX-License-Identifier: GPL-2.0-or-later
 * IO.SYS: the firmware backend. No FAT, DOS process state or shell is linked here.
 */
#include "efi_support.h"
#include "efi_disk.h"
#include "efi_ports.h"
#include "efi_serial.h"
#include "efi_clock.h"
#include "efi_cdrom.h"
#include "efi_ia32.h"
#include "efi_vga.h"
#include "dos_api.h"

static EFI_SYSTEM_TABLE *system_table;
static EFI_HANDLE current_image, io_image, dos_publisher;
static void *published_dos;
static EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID api_guid=DOS_API_GUID;
static EFI_GUID io_guid=IO_SERVICES_GUID;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *graphics;
static EFI_SIMPLE_POINTER_PROTOCOL *pointer;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *keyboard_ex;
static EFI_EVENT clock_event, idle_event;
static volatile u64 clock_ms;
static IoServices services;
static IoEvent pending_key;
static int have_key;
/* Resident modules: DEVICE drivers, TSRs, and AH=4B01h images not yet started (loaded). */
typedef struct {EFI_HANDLE image; u64 token; int loaded,driver; u32 entry;} Module;
static Module modules[32];
static u64 next_module=1;
static int io_console_key(void *,IoEvent *,unsigned);
/* Screen owner (display_claim); token 0 when the text console has it. */
typedef struct {
    u64 token; EFI_HANDLE owner; u32 mode; int text;
    void (*draw)(void *,const u16 *,size_t); void *draw_context; /* display_console */
    const IoTextOps *text_ops; void *text_context; /* display_text */
} DisplayClaim;
static DisplayClaim claim;
static u64 next_claim=1;
/* VGA text claims made while one is held, by programs the holder started
 * (a 16-bit program's child's 16-bit program): the claims below. */
#define CLAIM_NESTING 4
static DisplayClaim outer_claims[CLAIM_NESTING];
static unsigned nested_claims;
/* The text console is the VGA in a text mode, as a VGA text claim leaves
 * it, until a graphics claim or the end of IO.SYS sets a GOP mode again;
 * console text is then drawn there and sent to the serial device only. */
static int vga_console;
static int vga_text(void) {return claim.token?claim.text:vga_console;}

/* ConOut while the console is the VGA. The firmware's console would draw
 * into the VGA's memory, which it takes for its frame buffer, so EFI
 * programs that write it directly find this one in the system table
 * instead (ConOut and StdErr): their text goes where console text goes, on
 * the VGA in their colors and to the serial device. */
static SIMPLE_TEXT_OUTPUT_INTERFACE *firmware_out,*firmware_err;
static SIMPLE_TEXT_OUTPUT_MODE vga_mode;
static void vga_mode_follow(void) {
    unsigned col,row;
    if(efi_vga_where(&col,&row)) {vga_mode.CursorColumn=(INT32)col; vga_mode.CursorRow=(INT32)row;}
}
static EFI_STATUS EFIAPI vga_string(SIMPLE_TEXT_OUTPUT_INTERFACE *This,CHAR16 *text) {
    (void)This; size_t n=0;
    if(!text) return EFI_INVALID_PARAMETER;
    while(text[n]) n++;
    if(claim.token && claim.draw) claim.draw(claim.draw_context,text,n);
    else efi_vga_console(text,n,1,vga_mode.Attribute&0x7f,0);
    efi_serial_console_write_text(text,n);
    vga_mode_follow();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI vga_test(SIMPLE_TEXT_OUTPUT_INTERFACE *This,CHAR16 *text) {(void)This; return text?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
static EFI_STATUS EFIAPI vga_query(SIMPLE_TEXT_OUTPUT_INTERFACE *This,UINTN mode,UINTN *cols,UINTN *rows) {
    (void)This; unsigned c,r;
    if(!cols || !rows) return EFI_INVALID_PARAMETER;
    if(mode || !efi_vga_size(&c,&r)) return EFI_UNSUPPORTED;
    *cols=c; *rows=r; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI vga_clear(SIMPLE_TEXT_OUTPUT_INTERFACE *This) {
    (void)This;
    if(claim.token && claim.draw) claim.draw(claim.draw_context,NULL,0); else efi_vga_clear((u8)(vga_mode.Attribute&0x7f));
    vga_mode_follow();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI vga_reset(SIMPLE_TEXT_OUTPUT_INTERFACE *This,BOOLEAN verify) {(void)verify; return vga_clear(This);}
static EFI_STATUS EFIAPI vga_set_mode(SIMPLE_TEXT_OUTPUT_INTERFACE *This,UINTN mode) {return mode?EFI_UNSUPPORTED:vga_clear(This);}
static EFI_STATUS EFIAPI vga_attribute(SIMPLE_TEXT_OUTPUT_INTERFACE *This,UINTN attribute) {
    (void)This;
    if(attribute&~0x7fULL) return EFI_INVALID_PARAMETER;
    vga_mode.Attribute=(INT32)attribute; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI vga_locate(SIMPLE_TEXT_OUTPUT_INTERFACE *This,UINTN col,UINTN row) {
    (void)This; unsigned c,r;
    if(!efi_vga_size(&c,&r) || col>=c || row>=r) return EFI_UNSUPPORTED;
    efi_vga_locate((unsigned)col,(unsigned)row); vga_mode_follow();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI vga_cursor(SIMPLE_TEXT_OUTPUT_INTERFACE *This,BOOLEAN on) {
    (void)This; efi_vga_show_cursor(on); vga_mode.CursorVisible=on; return EFI_SUCCESS;
}
static SIMPLE_TEXT_OUTPUT_INTERFACE vga_out={(EFI_TEXT_RESET)vga_reset,(EFI_TEXT_OUTPUT_STRING)vga_string,
    (EFI_TEXT_TEST_STRING)vga_test,(EFI_TEXT_QUERY_MODE)vga_query,(EFI_TEXT_SET_MODE)vga_set_mode,
    (EFI_TEXT_SET_ATTRIBUTE)vga_attribute,(EFI_TEXT_CLEAR_SCREEN)vga_clear,(EFI_TEXT_SET_CURSOR_POSITION)vga_locate,
    (EFI_TEXT_ENABLE_CURSOR)vga_cursor,&vga_mode};
static void console_on_vga(int on) {
    EFI_SYSTEM_TABLE *st=system_table;
    if(on==vga_console) return;
    if(on) {
        firmware_out=st->ConOut; firmware_err=st->StdErr;
        vga_mode=(SIMPLE_TEXT_OUTPUT_MODE){1,0,EFI_TEXT_ATTR(EFI_LIGHTGRAY,EFI_BLACK),0,0,TRUE};
        vga_mode_follow();
        st->ConOut=&vga_out;
        if(st->StdErr==firmware_out) st->StdErr=&vga_out;
    } else {st->ConOut=firmware_out; st->StdErr=firmware_err;}
    vga_console=on;
    st->Hdr.CRC32=0; st->BootServices->CalculateCrc32(st,st->Hdr.HeaderSize,&st->Hdr.CRC32);
}

/* --- the text console as a screen of cells (ANSI.SYS) --------------------- */
/* The attribute console text is written in, and IO_TEXT_NOWRAP; -1 until
 * text_attribute sets one (the VGA console keeps its cells' attributes,
 * the firmware's console its own). */
static int text_attr=-1;
static u32 text_flags;
/* Where the serial device should show the same, while IO.SYS mirrors the
 * console text there itself (the console on the VGA or claimed). */
static int mirrored(void) {return claim.token || vga_console;}
static unsigned decimal(char *s,u32 v) {
    char digits[10]; unsigned n=0,k=0;
    do digits[n++]=(char)('0'+v%10); while(v/=10);
    while(n) s[k++]=digits[--n];
    return k;
}
static void serial_locate(u32 column,u32 row) {
    char s[24]; unsigned n=0; s[n++]=27; s[n++]='[';
    n+=decimal(s+n,row+1); s[n++]=';'; n+=decimal(s+n,column+1); s[n++]='H';
    efi_serial_console_write(s,n);
}
static void serial_attribute(u8 a) {
    static const u8 ansi[8]={0,4,2,6,1,5,3,7}; /* PC color to ANSI order */
    char s[24]; unsigned n=0;
    s[n++]=27; s[n++]='['; s[n++]='0';
    if(a&8) {s[n++]=';'; s[n++]='1';}
    if(a&0x80) {s[n++]=';'; s[n++]='5';}
    s[n++]=';'; s[n++]='3'; s[n++]=(char)('0'+ansi[a&7]);
    s[n++]=';'; s[n++]='4'; s[n++]=(char)('0'+ansi[a>>4&7]);
    s[n++]='m'; efi_serial_console_write(s,n);
}
static SIMPLE_TEXT_OUTPUT_INTERFACE *firmware_console(void) {return system_table->ConOut;}
static int firmware_size(UINTN *cols,UINTN *rows) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console();
    return !EFI_ERROR(out->QueryMode(out,(UINTN)out->Mode->Mode,cols,rows)) && *cols && *rows;
}
/* Firmware console text in the console attribute (EFI's colors: the
 * background's intensity and blink dropped), which the console keeps, as
 * a terminal driver would have to send it again each time otherwise; with
 * IO_TEXT_NOWRAP a character for the last column stays there. */
static void firmware_text(const u16 *p,size_t n) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console(); UINTN cols,rows;
    if(text_attr>=0 && (out->Mode->Attribute&0x7f)!=(text_attr&0x7f)) out->SetAttribute(out,(UINTN)(text_attr&0x7f));
    int nowrap=(text_flags&IO_TEXT_NOWRAP) && firmware_size(&cols,&rows);
    if(!nowrap) efi_output_text(system_table,p,n);
    else for(size_t i=0;i<n;i++) {
        UINTN row=(UINTN)out->Mode->CursorRow; int last=p[i]>=32 && (UINTN)out->Mode->CursorColumn+1>=cols;
        efi_output_text(system_table,&p[i],1);
        if(last) out->SetCursorPosition(out,cols-1,row<rows?row:rows-1);
    }
    efi_vga_shadow(p,n,1,(u8)(out->Mode->Attribute&0x7f),nowrap);
}
static int io_text_query(void *ctx,IoTextScreen *s) {
    (void)ctx; unsigned c,r,col,row;
    if(!s || s->size<sizeof(*s)) return DE_FUNCTION;
    if(claim.token && claim.draw) return claim.text_ops?claim.text_ops->query(claim.text_context,s):DE_ACCESS;
    if(claim.token && !claim.text) return DE_ACCESS;
    u32 attribute=text_attr>=0?(u32)text_attr:0x07;
    if(vga_text()) {
        if(!efi_vga_size(&c,&r) || !efi_vga_where(&col,&row)) return DE_ACCESS;
        *s=(IoTextScreen){.size=sizeof(*s),.columns=c,.rows=r,.column=col,.row=row,.attribute=attribute,
            .flags=efi_vga_intensity()?IO_TEXT_INTENSITY:0};
        return 0;
    }
    SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console(); UINTN cols,rows;
    if(!firmware_size(&cols,&rows)) return DE_FUNCTION;
    if(text_attr<0) attribute=(u32)out->Mode->Attribute&0x7f;
    *s=(IoTextScreen){.size=sizeof(*s),.columns=(u32)cols,.rows=(u32)rows,.column=(u32)out->Mode->CursorColumn,
        .row=(u32)out->Mode->CursorRow,.attribute=attribute};
    return 0;
}
static int io_text_locate(void *ctx,u32 column,u32 row) {
    (void)ctx; int e;
    if(claim.token && claim.draw) e=claim.text_ops?claim.text_ops->locate(claim.text_context,column,row):DE_ACCESS;
    else if(claim.token && !claim.text) e=DE_ACCESS;
    else if(vga_text()) {
        unsigned c,r;
        if(!efi_vga_size(&c,&r) || column>=c || row>=r) return DE_FUNCTION;
        efi_vga_locate(column,row); if(vga_console) vga_mode_follow(); e=0;
    } else {
        SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console();
        e=efi_dos_error(out->SetCursorPosition(out,column,row));
        if(e) return DE_FUNCTION;
        efi_vga_shadow_locate(column,row);
    }
    if(!e && mirrored()) serial_locate(column,row);
    return e;
}
static int io_text_attribute(void *ctx,u32 attribute,u32 flags) {
    (void)ctx;
    if(attribute>255 || (flags&~IO_TEXT_NOWRAP)) return DE_FUNCTION;
    text_attr=(int)attribute; text_flags=flags;
    if(claim.token && claim.draw && claim.text_ops) claim.text_ops->attribute(claim.text_context,attribute,flags);
    if(mirrored()) serial_attribute((u8)attribute);
    return 0;
}
static int io_text_erase(void *ctx,u32 column,u32 row,u32 cells) {
    (void)ctx; u8 attribute=(u8)(text_attr>=0?text_attr:0x07);
    IoTextScreen s={.size=sizeof(s)}; int e=io_text_query(ctx,&s); if(e) return e;
    if(column>=s.columns || row>=s.rows) return DE_FUNCTION;
    u32 total=s.columns*s.rows,from=row*s.columns+column;
    if(cells>total-from) cells=total-from;
    int whole=!from && cells==total;
    if(claim.token && claim.draw) e=claim.text_ops->erase(claim.text_context,column,row,cells);
    else if(vga_text()) {
        efi_vga_erase(column,row,cells,attribute);
        if(whole) efi_vga_locate(0,0);
        if(vga_console) vga_mode_follow();
    } else {
        SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console(); INT32 was=out->Mode->Attribute;
        out->SetAttribute(out,attribute&0x7f);
        if(whole) {out->ClearScreen(out); efi_vga_shadow_erase(0,0,total,attribute&0x7f); efi_vga_shadow_locate(0,0);}
        else {
            /* Spaces from there, the cursor put back; never the bottom-right
             * cell, which would scroll the screen on most firmware. */
            CHAR16 blanks[81]; u32 left=from+cells>=total?cells-1:cells;
            out->SetCursorPosition(out,column,row);
            efi_vga_shadow_erase(column,row,cells,attribute&0x7f);
            while(left) {
                u32 take=left<80?left:80;
                for(u32 i=0;i<take;i++) blanks[i]=' ';
                blanks[take]=0; out->OutputString(out,blanks); left-=take;
            }
            out->SetCursorPosition(out,s.column,s.row);
        }
        out->SetAttribute(out,(UINTN)was);
    }
    if(!e && mirrored()) {
        if(whole) {efi_serial_console_write("\x1b[2J\x1b[H",7);}
        else if(from+cells==(row+1)*s.columns) {serial_locate(column,row); efi_serial_console_write("\x1b[K",3); serial_locate(s.column,s.row);}
    }
    return e;
}
/* PC video modes 2 and 3, the 80x25 text the console is: the screen cleared
 * as a mode set clears it. A claimant takes the others it can show. */
static int io_text_mode(void *ctx,u32 mode) {
    (void)ctx;
    if(claim.token && claim.draw) return claim.text_ops?claim.text_ops->mode(claim.text_context,mode):DE_ACCESS;
    if(claim.token && !claim.text) return DE_ACCESS;
    if(mode!=2 && mode!=3) return DE_FUNCTION;
    if(vga_text()) {efi_vga_clear(0x07); if(vga_console) vga_mode_follow();}
    else {
        SIMPLE_TEXT_OUTPUT_INTERFACE *out=firmware_console(); INT32 was=out->Mode->Attribute;
        out->SetAttribute(out,0x07); out->ClearScreen(out); out->SetAttribute(out,(UINTN)was);
        efi_vga_shadow_clear();
    }
    if(mirrored()) efi_serial_console_write("\x1b[0m\x1b[2J\x1b[H",11);
    return 0;
}
static int io_display_text(void *ctx,u64 token,const IoTextOps *ops,void *context) {
    (void)ctx;
    if(!token || token!=claim.token) return DE_ACCESS;
    if(ops && (ops->size<sizeof(*ops) || !ops->query || !ops->locate || !ops->attribute || !ops->erase || !ops->mode)) return DE_FUNCTION;
    claim.text_ops=ops; claim.text_context=context;
    if(ops && text_attr>=0) ops->attribute(context,(u32)text_attr,text_flags);
    return 0;
}

void con_write(const void *p,size_t n) {
    if(claim.token && claim.draw) {
        const u8 *bytes=p; u16 text[64];
        for(size_t done=0;done<n;) {
            size_t k=0;
            while(k<64 && done<n) text[k++]=bytes[done++];
            claim.draw(claim.draw_context,text,k);
        }
    } else if(vga_text()) efi_vga_console(p,n,0,text_attr,text_flags&IO_TEXT_NOWRAP);
    if(claim.token || vga_console) efi_serial_console_write(p,n);
    else {
        u16 text[64];
        for(size_t done=0;done<n;) {
            size_t k=0;
            while(k<64 && done<n) text[k++]=((const u8 *)p)[done++];
            firmware_text(text,k);
        }
    }
}
void con_puts(const char *s) {con_write(s,strlen(s));}
static void io_console_write(void *ctx,const void *p,size_t n) {
    (void)ctx; con_write(p,n);
}
static void io_console_write_text(void *ctx,const u16 *p,size_t n) {
    (void)ctx;
    if(claim.token && claim.draw) claim.draw(claim.draw_context,p,n);
    else if(vga_text()) efi_vga_console(p,n,1,text_attr,text_flags&IO_TEXT_NOWRAP);
    if(claim.token || vga_console) efi_serial_console_write_text(p,n); else firmware_text(p,n);
}
static int io_console_read(void *ctx) {
    IoEvent key;
    for(;;) {
        int e=io_console_key(ctx,&key,IO_KEY_WAIT); if(e) return -e;
        if(key.unicode) return key.unicode;
    }
}
static void io_console_clear(void *ctx) {
    (void)ctx;
    if(claim.token && claim.draw) claim.draw(claim.draw_context,NULL,0);
    else if(vga_text()) efi_vga_clear((u8)(text_attr>=0?text_attr:0x07));
    else if(!claim.token) {
        SIMPLE_TEXT_OUTPUT_INTERFACE *out=system_table->ConOut; INT32 was=out->Mode->Attribute;
        if(text_attr>=0) out->SetAttribute(out,(UINTN)(text_attr&0x7f));
        out->ClearScreen(out); efi_vga_shadow_clear();
        if(text_attr>=0) {efi_vga_shadow_erase(0,0,80*25,(u8)(text_attr&0x7f)); out->SetAttribute(out,(UINTN)was);}
    }
}
/* The firmware's text console in GOP mode again, cleared. */
static EFI_STATUS gop_console(u32 mode) {
    if(vga_console) {efi_vga_leave(); console_on_vga(0);}
    EFI_STATUS e=graphics->SetMode(graphics,mode);
    system_table->ConOut->ClearScreen(system_table->ConOut);
    efi_vga_shadow_clear();
    return e;
}
/* The end of a claim: a nested one gives the screen back to the outer
 * claim; a VGA text claim leaves the console on the VGA, in mode 3 again if
 * the program left a graphics mode; a graphics claim restores the GOP mode. */
static int release_display(void) {
    if(nested_claims) {
        claim=outer_claims[--nested_claims];
        /* The attribute may have changed meanwhile. */
        if(claim.text_ops && text_attr>=0) claim.text_ops->attribute(claim.text_context,(u32)text_attr,text_flags);
        return 0;
    }
    int text=claim.text; u32 mode=claim.mode;
    claim=(DisplayClaim){0};
    if(text) {efi_vga_console_mode(); return 0;}
    return efi_dos_error(gop_console(mode));
}
static void *io_image_switch(void *ctx,void *image) {
    (void)ctx; void *was=current_image;
    if(image) current_image=image;
    return was;
}
static int io_console_save(void *ctx,void *out,u32 size) {
    (void)ctx;
    return claim.token || vga_console?DE_ACCESS:efi_vga_shadow_save(out,size);
}
static int io_console_restore(void *ctx,const void *in,u32 size) {
    (void)ctx;
    return claim.token || vga_console?DE_ACCESS:efi_vga_shadow_restore(in,size);
}
/* Pages taken in a range (alloc_pages_range), for the image they were
 * taken for. */
#define RANGE_RECORDS 512
typedef struct {EFI_HANDLE image; EFI_PHYSICAL_ADDRESS address; u32 pages;} RangeRecord;
static RangeRecord ranges[RANGE_RECORDS];
static void forget_range(EFI_PHYSICAL_ADDRESS address) {
    for(unsigned i=0;i<RANGE_RECORDS;i++) if(ranges[i].pages && ranges[i].address==address) {ranges[i].pages=0; return;}
}
/* An image's claim and range pages end with it. */
static void image_ended(EFI_HANDLE image) {
    while(claim.token && claim.owner==image) release_display();
    for(unsigned i=0;i<RANGE_RECORDS;i++) if(ranges[i].pages && ranges[i].image==image) {
        system_table->BootServices->FreePages(ranges[i].address,ranges[i].pages); ranges[i].pages=0;
    }
}
/* The firmware's memory map, in pool memory the caller frees. */
static EFI_MEMORY_DESCRIPTOR *memory_map(UINTN *size,UINTN *stride) {
    EFI_BOOT_SERVICES *bs=system_table->BootServices; UINTN key; UINT32 version;
    for(unsigned tries=0;tries<4;tries++) {
        EFI_MEMORY_DESCRIPTOR *map=NULL; *size=0;
        EFI_STATUS e=bs->GetMemoryMap(size,NULL,&key,stride,&version);
        if(e!=EFI_BUFFER_TOO_SMALL) return NULL;
        *size+=4*sizeof(EFI_MEMORY_DESCRIPTOR)+256;
        if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,*size,(void **)&map))) return NULL;
        e=bs->GetMemoryMap(size,map,&key,stride,&version);
        if(!EFI_ERROR(e) && *stride>=sizeof(EFI_MEMORY_DESCRIPTOR)) return map;
        bs->FreePool(map);
    }
    return NULL;
}
/* Free memory within [low,high): its pages, its largest run, and the
 * highest place a run of pages fits. */
static int range_scan(u64 low,u64 high,u32 pages,u64 *free_pages,u64 *largest,EFI_PHYSICAL_ADDRESS *place) {
    UINTN size,stride; EFI_MEMORY_DESCRIPTOR *map=memory_map(&size,&stride);
    if(!map) return DE_NOMEM;
    u64 total=0,big=0,best=0; int found=0;
    for(UINTN at=0;at+stride<=size;at+=stride) {
        const EFI_MEMORY_DESCRIPTOR *d=(const EFI_MEMORY_DESCRIPTOR *)((const u8 *)map+at);
        if(d->Type!=EfiConventionalMemory) continue;
        u64 start=d->PhysicalStart,end=start+d->NumberOfPages*4096ULL;
        if(start<low) start=(low+4095)&~4095ULL;
        if(end>high) end=high&~4095ULL;
        if(end<=start) continue;
        u64 run=(end-start)/4096; total+=run; if(run>big) big=run;
        if(pages && run>=pages && (!found || end-pages*4096ULL>best)) {best=end-pages*4096ULL; found=1;}
    }
    system_table->BootServices->FreePool(map);
    if(free_pages) *free_pages=total;
    if(largest) *largest=big;
    if(place) *place=best;
    return pages && !found?DE_NOMEM:0;
}
static int io_alloc_pages_range(void *ctx,u32 pages,u64 low,u64 high,void **out) {
    (void)ctx; unsigned slot;
    if(!pages || !out || high<=low) return DE_FUNCTION;
    *out=NULL;
    for(slot=0;slot<RANGE_RECORDS && ranges[slot].pages;slot++) {}
    if(slot==RANGE_RECORDS) return DE_NOMEM;
    /* The map may change between looking and taking: look again. */
    for(unsigned tries=0;tries<4;tries++) {
        EFI_PHYSICAL_ADDRESS place;
        int e=range_scan(low,high,pages,NULL,NULL,&place); if(e) return e;
        if(EFI_ERROR(system_table->BootServices->AllocatePages(AllocateAddress,EfiLoaderData,pages,&place))) continue;
        ranges[slot]=(RangeRecord){current_image,place,pages};
        *out=(void *)(uintptr_t)place; return 0;
    }
    return DE_NOMEM;
}
static int io_range_free(void *ctx,u64 low,u64 high,u64 *free_pages,u64 *largest) {
    (void)ctx;
    return high<=low?DE_FUNCTION:range_scan(low,high,0,free_pages,largest,NULL);
}
static int io_alloc_pages(void *ctx,u32 pages,void **out) {
    (void)ctx; EFI_PHYSICAL_ADDRESS memory=0;
    if(!pages || !out) return DE_FUNCTION;
    EFI_STATUS e=system_table->BootServices->AllocatePages(AllocateAnyPages,EfiLoaderData,pages,&memory);
    *out=EFI_ERROR(e)?NULL:(void *)(uintptr_t)memory;
    return efi_dos_error(e);
}
static void io_free_pages(void *ctx,void *memory,u32 pages) {
    (void)ctx;
    forget_range((EFI_PHYSICAL_ADDRESS)(uintptr_t)memory);
    system_table->BootServices->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)memory,pages);
}
static EFI_STATUS EFIAPI unload_module(EFI_HANDLE image) {(void)image; return EFI_SUCCESS;}
static unsigned pe_subsystem(const void *data,u32 size) {
    const u8 *b=data; if(!data || size<64) return 0;
    u32 pe=rd32(b+60);
    if(rd16(b)!=0x5a4d || pe>size-24 || rd32(b+pe)!=0x4550 || rd16(b+pe+4)!=0x200) return 0;
    u32 optional=rd16(b+pe+20);
    if(optional<70 || optional>size-pe-24 || rd16(b+pe+24)!=0x20b) return 0;
    return rd16(b+pe+24+68);
}
/* The entry point's RVA (on IA-64, of its function descriptor), from an
 * image pe_subsystem accepted. */
static u32 pe_entry(const void *data) {const u8 *b=data; return rd32(b+rd32(b+60)+24+16);}

/* Enter EXEC applications directly: EFI StartImage permits only the
 * last-started image to Exit, but DOS fibers can finish in any order.
 * Each run saves its own return path for DOS exit or the Boot Services Exit
 * hook installed by IO.SYS. Drivers still use StartImage to connect their
 * installed protocols. */
typedef struct {EFI_HANDLE image; EFI_STATUS status; unsigned slot; UINTN jump[8];} Run;
static Run *runs[32];
static EFI_EXIT firmware_exit;
static Run *run_of(EFI_HANDLE image) {
    for(unsigned i=0;i<ARRAY_SIZE(runs);i++) if(runs[i] && runs[i]->image==image) return runs[i];
    return NULL;
}
static EFI_STATUS EFIAPI exit_hook(EFI_HANDLE image,EFI_STATUS status,UINTN size,CHAR16 *data) {
    Run *r=run_of(image);
    if(!r) return firmware_exit(image,status,size,data);
    if(data) system_table->BootServices->FreePool(data);
    r->status=status;
    __builtin_longjmp(r->jump,1);
}
static void set_exit(EFI_EXIT exit) {
    EFI_BOOT_SERVICES *bs=system_table->BootServices; UINT32 crc=0;
    bs->Exit=exit; bs->Hdr.CRC32=0;
    if(!EFI_ERROR(bs->CalculateCrc32(bs,bs->Hdr.HeaderSize,&crc))) bs->Hdr.CRC32=crc;
}
static EFI_STATUS run_application(EFI_HANDLE child,u32 entry) {
    EFI_LOADED_IMAGE *loaded=NULL; Run run={child,EFI_SUCCESS,0,{0}};
    if(EFI_ERROR(system_table->BootServices->HandleProtocol(child,&loaded_guid,(void **)&loaded)) || entry>=loaded->ImageSize) return EFI_LOAD_ERROR;
    while(run.slot<ARRAY_SIZE(runs) && runs[run.slot]) run.slot++;
    if(run.slot==ARRAY_SIZE(runs)) return EFI_OUT_OF_RESOURCES;
    EFI_IMAGE_ENTRY_POINT start=(EFI_IMAGE_ENTRY_POINT)(void *)((u8 *)loaded->ImageBase+entry);
    runs[run.slot]=&run;
    if(!__builtin_setjmp(run.jump)) run.status=start(child,system_table);
    runs[run.slot]=NULL;
    return run.status;
}
static Module *module_for(u64 token) {
    for(unsigned i=0;i<ARRAY_SIZE(modules);i++) if(modules[i].image && modules[i].token==token) return &modules[i];
    return NULL;
}
static int new_module(EFI_HANDLE image,int loaded,int driver,u64 *token) {
    unsigned slot=0; while(slot<ARRAY_SIZE(modules) && modules[slot].image) slot++;
    if(slot==ARRAY_SIZE(modules) || !next_module) return DE_NOMEM;
    modules[slot]=(Module){image,next_module++,loaded,driver,0}; *token=modules[slot].token; return 0;
}
/* Subsystem-11 (resident-capable) images and detached AH=4B01h loads get
 * IO.SYS as parent: firmware refuses Exit for an image with loaded children. */
static int load_exec(const void *data,u32 size,int detached,EFI_HANDLE *child,int *driver,u32 *entry) {
    unsigned subsystem=pe_subsystem(data,size); if(subsystem!=10 && subsystem!=11) return DE_FORMAT;
    EFI_BOOT_SERVICES *bs=system_table->BootServices; *driver=subsystem==11; *entry=pe_entry(data);
    EFI_STATUS e=bs->LoadImage(FALSE,*driver || detached?io_image:current_image,NULL,(void *)data,size,child);
    if(EFI_ERROR(e)) return efi_dos_error(e);
    if(*driver) {
        EFI_LOADED_IMAGE *loaded=NULL;
        e=bs->HandleProtocol(*child,&loaded_guid,(void **)&loaded);
        if(EFI_ERROR(e) || loaded->ImageCodeType!=EfiBootServicesCode) {bs->UnloadImage(*child); return DE_FORMAT;}
        loaded->Unload=unload_module;
    }
    return 0;
}
static int run_exec(EFI_HANDLE child,int driver,u32 entry,unsigned *result) {
    EFI_BOOT_SERVICES *bs=system_table->BootServices;
    EFI_HANDLE saved=current_image; current_image=child;
    EFI_STATUS e=driver?bs->StartImage(child,NULL,NULL):run_application(child,entry); current_image=saved;
    image_ended(child);
    /* Applications are unloaded once they end. A resident-capable image
     * stays loaded only after exit_resident registered it. */
    if(driver) {
        int resident=0;
        for(unsigned i=0;i<ARRAY_SIZE(modules);i++) if(modules[i].image==child && !modules[i].loaded) resident=1;
        EFI_LOADED_IMAGE *loaded=NULL;
        if(!resident && !EFI_ERROR(bs->HandleProtocol(child,&loaded_guid,(void **)&loaded))) bs->UnloadImage(child);
    } else bs->UnloadImage(child);
    if(EFI_ERROR(e)) return efi_dos_error(e);
    *result=(unsigned)e&255; return 0;
}
static int io_exec(void *ctx,const void *data,u32 size,const char *tail,unsigned *result) {
    (void)ctx; (void)tail; EFI_HANDLE child; int driver; u32 entry;
    int e=load_exec(data,size,0,&child,&driver,&entry); return e?e:run_exec(child,driver,entry,result);
}
static int io_image_load(void *ctx,const void *data,u32 size,u64 *token,u64 *base,u64 *bytes) {
    (void)ctx; if(!token || !base || !bytes) return DE_FUNCTION;
    *token=*base=*bytes=0; EFI_HANDLE child; int driver; u32 entry;
    int e=load_exec(data,size,1,&child,&driver,&entry); if(e) return e;
    EFI_LOADED_IMAGE *loaded=NULL; EFI_BOOT_SERVICES *bs=system_table->BootServices;
    if(EFI_ERROR(bs->HandleProtocol(child,&loaded_guid,(void **)&loaded)) || (e=new_module(child,1,driver,token))) {
        bs->UnloadImage(child); return e?e:DE_FORMAT;
    }
    module_for(*token)->entry=entry;
    *base=(uintptr_t)loaded->ImageBase; *bytes=loaded->ImageSize; return 0;
}
static int io_image_start(void *ctx,u64 token,const char *tail,unsigned *result) {
    (void)ctx; (void)tail; Module *m=module_for(token);
    if(!m || !m->loaded || !result) return DE_HANDLE;
    EFI_HANDLE child=m->image; int driver=m->driver; u32 entry=m->entry; memset(m,0,sizeof(*m));
    return run_exec(child,driver,entry,result);
}
static int io_image_discard(void *ctx,u64 token) {
    (void)ctx; Module *m=module_for(token); if(!m || !m->loaded) return DE_HANDLE;
    EFI_STATUS status=system_table->BootServices->UnloadImage(m->image);
    memset(m,0,sizeof(*m)); return efi_dos_error(status);
}
static void io_exit(void *ctx,unsigned code) {
    (void)ctx; system_table->BootServices->Exit(current_image,code,0,NULL);
}
/* A non-error exit status keeps a boot-services driver image loaded. */
static int io_exit_resident(void *ctx,unsigned code,u64 *token) {
    (void)ctx; EFI_LOADED_IMAGE *loaded=NULL; EFI_BOOT_SERVICES *bs=system_table->BootServices;
    if(!token || code>255) return DE_FUNCTION;
    if(current_image==io_image || EFI_ERROR(bs->HandleProtocol(current_image,&loaded_guid,(void **)&loaded)) ||
       loaded->ImageCodeType!=EfiBootServicesCode) return DE_FORMAT;
    int e=new_module(current_image,0,1,token); if(e) return e;
    bs->Exit(current_image,code,0,NULL);
    Module *m=module_for(*token); if(m) memset(m,0,sizeof(*m));
    *token=0; return DE_FUNCTION;
}
static int io_module_load(void *ctx,const void *data,u32 size,u64 *token) {
    (void)ctx;
    if(!data || !token || size<64) return DE_FORMAT;
    *token=0;
    if(pe_subsystem(data,size)!=11) return DE_FORMAT;
    unsigned slot=0; while(slot<ARRAY_SIZE(modules) && modules[slot].image) slot++;
    if(slot==ARRAY_SIZE(modules) || !next_module) return DE_NOMEM;
    EFI_BOOT_SERVICES *bs=system_table->BootServices; EFI_HANDLE child;
    EFI_STATUS status=bs->LoadImage(FALSE,current_image,NULL,(void *)data,size,&child);
    if(EFI_ERROR(status)) return efi_dos_error(status);
    EFI_LOADED_IMAGE *loaded=NULL;
    status=bs->HandleProtocol(child,&loaded_guid,(void **)&loaded);
    if(EFI_ERROR(status) || loaded->ImageCodeType!=EfiBootServicesCode) {bs->UnloadImage(child); return DE_FORMAT;}
    loaded->Unload=unload_module;
    EFI_HANDLE saved=current_image; current_image=child;
    status=bs->StartImage(child,NULL,NULL); current_image=saved;
    if(status!=EFI_SUCCESS) {image_ended(child); bs->UnloadImage(child); return EFI_ERROR(status)?efi_dos_error(status):DE_FORMAT;}
    /* An image may install its own Unload; IO.SYS's replaces it, since
     * resource cleanup is DOS FINISH, before this EFI unload callback. */
    loaded->Unload=unload_module;
    modules[slot]=(Module){child,next_module++,0,1,0}; *token=modules[slot].token; return 0;
}
static int io_module_unload(void *ctx,u64 token) {
    (void)ctx;
    for(unsigned i=0;i<ARRAY_SIZE(modules);i++) if(modules[i].image && modules[i].token==token) {
        image_ended(modules[i].image);
        EFI_STATUS status=system_table->BootServices->UnloadImage(modules[i].image);
        if(EFI_ERROR(status)) return efi_dos_error(status);
        memset(&modules[i],0,sizeof(modules[i])); return 0;
    }
    return DE_HANDLE;
}
/* Before a reset: each unit with media flushes its writes. */
static void flush_disks(void) {
    for(u32 i=0;i<efi_disk_count(NULL);i++) {
        IoDiskInfo info;
        if(!efi_disk_info(NULL,i,&info) && (info.flags&IO_DISK_PRESENT))
            info.disk.flush(info.disk.ctx);
    }
}
static void io_shutdown(void *ctx) {
    (void)ctx; flush_disks();
    system_table->RuntimeServices->ResetSystem(EfiResetShutdown,EFI_SUCCESS,0,NULL);
}
static void io_restart(void *ctx) {
    (void)ctx; flush_disks();
    system_table->RuntimeServices->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
}
static int io_publish(void *ctx,void *api) {
    (void)ctx; EFI_BOOT_SERVICES *bs=system_table->BootServices;
    if(api) {
        if(published_dos) return DE_ACCESS;
        dos_publisher=current_image;
        EFI_STATUS e=bs->InstallProtocolInterface(&dos_publisher,&api_guid,EFI_NATIVE_INTERFACE,api);
        if(EFI_ERROR(e)) return efi_dos_error(e);
        published_dos=api;
    } else if(published_dos) {
        EFI_STATUS e=bs->UninstallProtocolInterface(dos_publisher,&api_guid,published_dos);
        if(EFI_ERROR(e)) return efi_dos_error(e);
        published_dos=NULL;
    }
    return 0;
}
static void EFIAPI count_tick(EFI_EVENT event,void *context) {
    (void)event; (void)context;
    clock_ms+=10; /* No DOS, graphics, I/O or scheduling from the callback. */
}
/* Milliseconds come from the interval time counter, calibrated against
 * Stall at startup: the firmware runs timer callbacks only when it polls,
 * so counting them loses the time spent running code. */
static u64 itc_base,itc_per_ms;
static u64 read_itc(void) {u64 v; __asm__ volatile("mov %0=ar.itc" : "=r"(v)); return v;}
static u64 now_ms(void) {return itc_per_ms?(read_itc()-itc_base)/itc_per_ms:clock_ms;}
static u64 io_ticks(void *ctx) {(void)ctx; return now_ms();}
static int read_key(IoEvent *event) {
    if(!event) return DE_FUNCTION;
    memset(event,0,sizeof(*event)); event->time_ms=now_ms();
    EFI_INPUT_KEY key;
    EFI_STATUS e;
    if(keyboard_ex) {
        EFI_KEY_DATA data;
        e=keyboard_ex->ReadKeyStrokeEx(keyboard_ex,&data);
        if(!EFI_ERROR(e)) {
            key=data.Key;
            u32 state=data.KeyState.KeyShiftState;
            if(state&EFI_SHIFT_STATE_VALID) {
                event->flags|=IO_KEY_MODIFIERS_VALID;
                if(state&(EFI_LEFT_SHIFT_PRESSED|EFI_RIGHT_SHIFT_PRESSED)) event->modifiers|=IO_MOD_SHIFT;
                if(state&(EFI_LEFT_CONTROL_PRESSED|EFI_RIGHT_CONTROL_PRESSED)) event->modifiers|=IO_MOD_CONTROL;
                if(state&(EFI_LEFT_ALT_PRESSED|EFI_RIGHT_ALT_PRESSED)) event->modifiers|=IO_MOD_ALT;
                if(state&(EFI_LEFT_LOGO_PRESSED|EFI_RIGHT_LOGO_PRESSED)) event->modifiers|=IO_MOD_LOGO;
                if(state&EFI_RIGHT_CONTROL_PRESSED) event->modifiers|=IO_MOD_RIGHT_CONTROL;
                if(state&EFI_RIGHT_ALT_PRESSED) event->modifiers|=IO_MOD_RIGHT_ALT;
            }
            u8 toggles=data.KeyState.KeyToggleState;
            if(toggles&EFI_TOGGLE_STATE_VALID) {
                event->flags|=IO_KEY_TOGGLES_VALID;
                if(toggles&EFI_SCROLL_LOCK_ACTIVE) event->modifiers|=IO_MOD_SCROLL_LOCK;
                if(toggles&EFI_NUM_LOCK_ACTIVE) event->modifiers|=IO_MOD_NUM_LOCK;
                if(toggles&EFI_CAPS_LOCK_ACTIVE) event->modifiers|=IO_MOD_CAPS_LOCK;
            }
        }
    } else e=system_table->ConIn->ReadKeyStroke(system_table->ConIn,&key);
    if(!EFI_ERROR(e)) {
        static const u32 scan[]={IO_SCAN_NONE,IO_SCAN_UP,IO_SCAN_DOWN,IO_SCAN_RIGHT,IO_SCAN_LEFT,
            IO_SCAN_HOME,IO_SCAN_END,IO_SCAN_INSERT,IO_SCAN_DELETE,IO_SCAN_PAGE_UP,IO_SCAN_PAGE_DOWN,
            IO_SCAN_F1,IO_SCAN_F2,IO_SCAN_F3,IO_SCAN_F4,IO_SCAN_F5,IO_SCAN_F6,IO_SCAN_F7,IO_SCAN_F8,
            IO_SCAN_F9,IO_SCAN_F10,IO_SCAN_F11,IO_SCAN_F12,IO_SCAN_ESCAPE};
        event->type=IO_EVENT_KEY; event->unicode=key.UnicodeChar;
        event->scan=key.ScanCode<ARRAY_SIZE(scan)?scan[key.ScanCode]:key.ScanCode==0x48?IO_SCAN_PAUSE:IO_SCAN_UNKNOWN;
        return 0;
    }
    if(e!=EFI_NOT_READY) return efi_dos_error(e);
    return DE_NOTREADY;
}
static int io_console_key(void *ctx,IoEvent *event,unsigned flags) {
    (void)ctx;
    if(!event || (flags&~(IO_KEY_PEEK|IO_KEY_WAIT))) return DE_FUNCTION;
    while(!have_key) {
        int e=read_key(&pending_key);
        if(!e) {have_key=1; break;}
        if(e!=DE_NOTREADY || !(flags&IO_KEY_WAIT)) return e;
        EFI_EVENT input=keyboard_ex?keyboard_ex->WaitForKeyEx:system_table->ConIn->WaitForKey;
        UINTN index; EFI_STATUS status=system_table->BootServices->WaitForEvent(1,&input,&index);
        if(EFI_ERROR(status)) return efi_dos_error(status);
    }
    *event=pending_key; if(!(flags&IO_KEY_PEEK)) have_key=0;
    return 0;
}
static int io_poll(void *ctx,IoEvent *event) {
    int result=io_console_key(ctx,event,0);
    if(result!=DE_NOTREADY) return result;
    memset(event,0,sizeof(*event)); event->time_ms=now_ms();
    EFI_STATUS e;
    if(pointer) {
        EFI_SIMPLE_POINTER_STATE state;
        e=pointer->GetState(pointer,&state);
        if(!EFI_ERROR(e)) {
            event->type=IO_EVENT_POINTER;
            event->dx=state.RelativeMovementX; event->dy=state.RelativeMovementY;
            event->dz=state.RelativeMovementZ;
            event->buttons=(state.LeftButton?1:0)|(state.RightButton?2:0);
            return 0;
        }
        if(e!=EFI_NOT_READY) return efi_dos_error(e);
    }
    return DE_NOTREADY;
}
static int io_wait(void *ctx,u32 milliseconds) {
    (void)ctx; EFI_BOOT_SERVICES *bs=system_table->BootServices;
    if(!milliseconds || have_key) return 0;
    if(!idle_event) return DE_FUNCTION;
    EFI_STATUS e=bs->SetTimer(idle_event,TimerRelative,(u64)milliseconds*10000);
    if(EFI_ERROR(e)) return efi_dos_error(e);
    EFI_EVENT events[3]={idle_event,keyboard_ex?keyboard_ex->WaitForKeyEx:system_table->ConIn->WaitForKey,NULL};
    UINTN count=2,index;
    if(pointer) events[count++]=pointer->WaitForInput;
    e=bs->WaitForEvent(count,events,&index);
    bs->SetTimer(idle_event,TimerCancel,0);
    return efi_dos_error(e);
}
static int io_display_info(void *ctx,IoDisplay *out) {
    (void)ctx;
    if(!out) return DE_FUNCTION;
    if(!graphics || !graphics->Mode || !graphics->Mode->Info) return DE_FUNCTION;
    out->width=graphics->Mode->Info->HorizontalResolution;
    out->height=graphics->Mode->Info->VerticalResolution;
    return 0;
}
static int io_display_blt(void *ctx,IoPixel *pixels,u32 x,u32 y,u32 width,u32 height,
                          u32 stride,int readback) {
    (void)ctx; IoDisplay mode;
    if(vga_text()) return DE_ACCESS; /* the frame buffer is the VGA's memory */
    int e=io_display_info(NULL,&mode); if(e) return e;
    if(!pixels || !width || !height || stride<width || x>=mode.width || y>=mode.height ||
       width>mode.width-x || height>mode.height-y) return DE_FUNCTION;
    _Static_assert(sizeof(IoPixel)==sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL),"pixel ABI");
    EFI_STATUS status=graphics->Blt(graphics,(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)pixels,
        readback?EfiBltVideoToBltBuffer:EfiBltBufferToVideo,
        readback?x:0,readback?y:0,readback?0:x,readback?0:y,width,height,(UINTN)stride*4);
    return efi_dos_error(status);
}
static int io_display_mode(void *ctx,u32 index,IoDisplayMode *out) {
    (void)ctx;
    if(!out || !graphics || !graphics->Mode) return DE_FUNCTION;
    if(index>=graphics->Mode->MaxMode) return DE_NOMORE;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info=NULL; UINTN size=0;
    EFI_STATUS e=graphics->QueryMode(graphics,index,&size,&info);
    if(EFI_ERROR(e)) return efi_dos_error(e);
    if(!info) return DE_IO;
    *out=(IoDisplayMode){info->HorizontalResolution,info->VerticalResolution,
                         index==graphics->Mode->Mode?IO_DISPLAY_CURRENT:0,0};
    system_table->BootServices->FreePool(info); return 0;
}
static int io_display_claim(void *ctx,u32 mode,u64 *token) {
    (void)ctx;
    if(!token) return DE_FUNCTION;
    *token=0;
    if(!graphics || !graphics->Mode) return DE_FUNCTION;
    if(claim.token && (mode!=IO_DISPLAY_VGA_TEXT || !claim.text || nested_claims==CLAIM_NESTING)) return DE_ACCESS;
    if(claim.token) {
        if(!next_claim) return DE_FUNCTION;
        outer_claims[nested_claims++]=claim;
        claim=(DisplayClaim){next_claim++,current_image,claim.mode,1,NULL,NULL,NULL,NULL}; *token=claim.token; return 0;
    }
    u32 previous=graphics->Mode->Mode;
    if(mode==IO_DISPLAY_VGA_TEXT) {
        if(!(services.capabilities&IO_CAP_VGA_TEXT) || !next_claim) return DE_FUNCTION;
        if(!vga_console) {
            int e=efi_vga_text();
            if(e) return e;
            console_on_vga(1);
        }
        claim=(DisplayClaim){next_claim++,current_image,previous,1,NULL,NULL,NULL,NULL}; *token=claim.token; return 0;
    }
    if(mode==IO_DISPLAY_KEEP) mode=previous;
    if(mode>=graphics->Mode->MaxMode || !next_claim) return DE_FUNCTION;
    /* Setting a mode clears the screen, also when it is the current one. */
    if(vga_console) {efi_vga_leave(); console_on_vga(0);}
    EFI_STATUS e=graphics->SetMode(graphics,mode);
    if(EFI_ERROR(e)) {gop_console(previous); return efi_dos_error(e);}
    claim=(DisplayClaim){next_claim++,current_image,previous,0,NULL,NULL,NULL,NULL}; *token=claim.token; return 0;
}
static int io_vga_port(void *ctx,u64 token,u32 port,u8 *value,int write) {
    (void)ctx;
    if(!token || token!=claim.token || !claim.text) return DE_HANDLE;
    return efi_vga_port(port,value,write);
}
static int io_display_console(void *ctx,u64 token,void (*draw)(void *,const u16 *,size_t),void *context) {
    (void)ctx;
    if(!token || token!=claim.token) return DE_HANDLE;
    claim.draw=draw; claim.draw_context=context;
    if(!draw) claim.text_ops=NULL;
    return 0;
}
static int io_display_release(void *ctx,u64 token) {
    (void)ctx;
    if(!token || token!=claim.token) return DE_HANDLE;
    return release_display();
}
static void init_optional_services(void) {
    EFI_BOOT_SERVICES *bs=system_table->BootServices;
    EFI_GUID gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID pointer_guid=EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_GUID keyboard_guid=EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    if(!EFI_ERROR(bs->LocateProtocol(&gop_guid,NULL,(void **)&graphics)))
        services.capabilities|=IO_CAP_GRAPHICS;
    if(graphics && efi_vga_init(system_table)) services.capabilities|=IO_CAP_VGA_TEXT;
    if(!EFI_ERROR(bs->LocateProtocol(&pointer_guid,NULL,(void **)&pointer)))
        services.capabilities|=IO_CAP_POINTER;
    if(!EFI_ERROR(bs->HandleProtocol(system_table->ConsoleInHandle,&keyboard_guid,(void **)&keyboard_ex)))
        services.capabilities|=IO_CAP_KEY_MODIFIERS;
    EFI_STATUS e=bs->CreateEvent(EVT_TIMER,0,NULL,NULL,&idle_event);
    if(EFI_ERROR(e)) idle_event=NULL;
    itc_base=read_itc();
    if(!EFI_ERROR(bs->Stall(10000))) itc_per_ms=(read_itc()-itc_base)/10;
    e=bs->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,count_tick,NULL,&clock_event);
    if(!EFI_ERROR(e)) e=bs->SetTimer(clock_event,TimerPeriodic,100000);
    if(!EFI_ERROR(e) && idle_event) services.capabilities|=IO_CAP_TIMER;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    system_table=st; current_image=image; io_image=image;
    EFI_BOOT_SERVICES *bs=st->BootServices;
    void *running=NULL;
    if(!EFI_ERROR(bs->LocateProtocol(&io_guid,NULL,&running))) {con_puts("IO.SYS: DOS is already running\n"); return (EFI_STATUS)1;}
    /* A clean screen, the firmware's messages gone, then the banner. */
    io_console_clear(NULL);
    con_puts(DOS_PRODUCT "\n");
    EFI_LOADED_IMAGE *loaded=NULL;
    EFI_STATUS status=bs->HandleProtocol(image,&loaded_guid,(void **)&loaded);
    if(EFI_ERROR(status)) return status;
    Disk boot;
    int e=efi_disks_init(st,loaded->DeviceHandle,&boot);
    if(e) {efi_disks_close(); return EFI_LOAD_ERROR;}
    services=(IoServices){
        .version=IO_ABI_VERSION,.size=sizeof(IoServices),.capabilities=IO_CAP_FIRMWARE_BOOT|IO_CAP_DISKS|IO_CAP_CONSOLE_KEY|IO_CAP_MODULES,
        .boot_disk=boot,
        .console_write=io_console_write,.console_read=io_console_read,.console_clear=io_console_clear,
        .alloc_pages=io_alloc_pages,.free_pages=io_free_pages,
        .exec=io_exec,.exit_image=io_exit,.shutdown=io_shutdown,.publish_dos=io_publish,
        .poll_event=io_poll,.wait=io_wait,.ticks_ms=io_ticks,
        .display_info=io_display_info,.display_blt=io_display_blt,
        .display_mode=io_display_mode,.display_claim=io_display_claim,.display_release=io_display_release,.vga_port=io_vga_port,.display_console=io_display_console,
        .disk_count=efi_disk_count,.disk_info=efi_disk_info,.console_key=io_console_key,
        .module_load=io_module_load,.module_unload=io_module_unload,
        .console_write_text=io_console_write_text,
        .physical_count=efi_physical_count,.physical_info=efi_physical_info,.disk_location=efi_disk_location,
        .restart=io_restart,.exit_resident=io_exit_resident,
        .image_load=io_image_load,.image_start=io_image_start,.image_discard=io_image_discard,
        .image_switch=io_image_switch,.console_save=io_console_save,.console_restore=io_console_restore,
        .text_query=io_text_query,.text_locate=io_text_locate,.text_attribute=io_text_attribute,
        .text_erase=io_text_erase,.text_mode=io_text_mode,.display_text=io_display_text,
        .alloc_pages_range=io_alloc_pages_range,.range_free=io_range_free
    };
    services.capabilities|=IO_CAP_RESIDENT|IO_CAP_TEXT_SCREEN;
    if(efi_physical_count(NULL)) services.capabilities|=IO_CAP_PHYSICAL;
    init_optional_services();
    efi_clock_init(st->RuntimeServices,&services);
    efi_ports_init(st,&services);
    efi_serial_init(st,&services);
    efi_cdrom_init(st,&services);
    efi_ia32_init(st,&services,itc_per_ms);
    /* The high memory area for VDMs, when the firmware has it free. */
    EFI_PHYSICAL_ADDRESS hma=0x100000;
    if((services.capabilities&IO_CAP_IA32) && !EFI_ERROR(bs->AllocatePages(AllocateAddress,EfiLoaderData,16,&hma)))
        services.capabilities|=IO_CAP_HMA;
    else hma=0;
    status=bs->InstallProtocolInterface(&io_image,&io_guid,EFI_NATIVE_INTERFACE,&services);
    firmware_exit=bs->Exit; set_exit(exit_hook);
    if(!EFI_ERROR(status)) {
        EFI_HANDLE kernel;
        status=efi_load_sibling(st,image,(const CHAR16 *)u"\\MSDOS.SYS",&kernel);
        if(!EFI_ERROR(status)) {
            current_image=kernel;
            status=bs->StartImage(kernel,NULL,NULL);
            current_image=image;
        }
        io_publish(NULL,NULL);
        bs->UninstallProtocolInterface(io_image,&io_guid,&services);
    }
    set_exit(firmware_exit);
    if(vga_console) gop_console(graphics->Mode->Mode);
    efi_ia32_close(); efi_serial_close(); efi_cdrom_close(); efi_ports_close();
    if(clock_event) bs->CloseEvent(clock_event);
    if(idle_event) bs->CloseEvent(idle_event);
    if(hma) bs->FreePages(hma,16);
    efi_disks_close();
    if(EFI_ERROR(status)) print("IO.SYS: kernel failure EFI %x\n",(unsigned long long)status);
    return status;
}
