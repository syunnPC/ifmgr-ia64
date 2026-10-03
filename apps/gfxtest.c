/* SPDX-License-Identifier: GPL-2.0-or-later
 * Display ownership exerciser:
 *   GFXTEST            list the graphics modes
 *   GFXTEST n [hold] [keep]
 *       claim mode n, draw a test pattern and check it; with hold, report
 *       pointer events and keys until q; with keep, end without releasing
 *       (IO.SYS releases the claim when the image ends)
 * Pattern: background (0,0,96); red 64x64 at (16,16); green 64x64 at the
 * top right corner inset 16; white 32x32 in the middle.
 */
#include "runtime.h"
static const IoServices *io;
static unsigned passed,failed;
static void check(const char *name,int ok) {
    if(ok) passed++; else {failed++; print("[FAIL] %s\n",name);}
}
static IoPixel row[2048];
static int fill(u32 x,u32 y,u32 w,u32 h,IoPixel color) {
    for(u32 i=0;i<w;i++) row[i]=color;
    for(u32 j=0;j<h;j++) {int e=io->display_blt(io->context,row,x,y+j,w,1,w,0); if(e) return e;}
    return 0;
}
static int same(IoPixel a,IoPixel b) {return a.red==b.red && a.green==b.green && a.blue==b.blue;}
static int pixel_is(u32 x,u32 y,IoPixel want) {
    IoPixel got; return !io->display_blt(io->context,&got,x,y,1,1,1,1) && same(got,want);
}
static int has_word(const char *text,const char *word) {
    for(size_t n=strlen(word);*text;text++) if(!memcmp(text,word,n)) return 1;
    return 0;
}
static char sign(int32_t v) {return v<0?'-':'+';}
static unsigned long long magnitude(int32_t v) {return v<0?(unsigned long long)-(long long)v:(unsigned long long)v;}
static void hold(void) {
    print("GFXTEST: holding\n");
    for(;;) {
        IoEvent ev; int e=io->poll_event(io->context,&ev);
        if(e==DE_NOTREADY) {io->wait(io->context,100); continue;}
        if(e) {print("GFXTEST: poll error %u\n",(unsigned long long)e); return;}
        if(ev.type==IO_EVENT_POINTER)
            print("GFXTEST: pointer %c%u %c%u buttons %u\n",sign(ev.dx),magnitude(ev.dx),sign(ev.dy),magnitude(ev.dy),
                  (unsigned long long)ev.buttons);
        else if(ev.type==IO_EVENT_KEY) {
            print("GFXTEST: key %u scan %u\n",(unsigned long long)ev.unicode,(unsigned long long)ev.scan);
            if(ev.unicode=='q') return;
        }
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    io=dos_io_services();
    if(!io || io->size<offsetof(IoServices,display_release)+sizeof(io->display_release) || !(io->capabilities&IO_CAP_GRAPHICS)) {
        print("GFXTEST: no display services\n"); return 1;
    }
    char tail[64],*args=tail; strcopy(tail,sizeof(tail),app_dos->command_tail());
    while(*args==' ') args++;
    IoDisplayMode m; u32 count=0,current=0;
    for(;;count++) {
        int e=io->display_mode(io->context,count,&m); if(e) {check("mode-end",e==DE_NOMORE); break;}
        if(m.flags&IO_DISPLAY_CURRENT) current=count;
        if(!*args) print("GFXTEST: mode %u %ux%u%s\n",(unsigned long long)count,(unsigned long long)m.width,
                         (unsigned long long)m.height,m.flags&IO_DISPLAY_CURRENT?" current":"");
    }
    if(!*args) {print("GFXTEST: %u modes, pointer %s\n",(unsigned long long)count,io->capabilities&IO_CAP_POINTER?"yes":"no"); return failed?1:0;}
    u32 mode=(u32)(*args-'0'); int want_hold=has_word(args,"hold"),keep=has_word(args,"keep");
    if(mode>=count || io->display_mode(io->context,mode,&m)) {print("GFXTEST: bad mode\n"); return 1;}
    u64 token=0,other=0;
    check("claim-bad-mode",io->display_claim(io->context,count,&token)==DE_FUNCTION && !token);
    int e=io->display_claim(io->context,mode,&token);
    if(e) {print("GFXTEST: claim failed (%u)\n",(unsigned long long)e); return 1;}
    check("claim-exclusive",io->display_claim(io->context,IO_DISPLAY_KEEP,&other)==DE_ACCESS && !other);
    check("release-wrong-token",io->display_release(io->context,token+1)==DE_HANDLE);
    IoDisplay d; check("info",!io->display_info(io->context,&d) && d.width==m.width && d.height==m.height);
    IoDisplayMode now; check("mode-current",!io->display_mode(io->context,mode,&now) && (now.flags&IO_DISPLAY_CURRENT));
    check("cleared",pixel_is(d.width/2,d.height/2,(IoPixel){0,0,0,0}));
    const IoPixel back={96,0,0,0},red={0,0,255,0},green={0,255,0,0},white={255,255,255,0};
    check("draw",!fill(0,0,d.width,d.height,back) && !fill(16,16,64,64,red) &&
          !fill(d.width-80,16,64,64,green) && !fill(d.width/2-16,d.height/2-16,32,32,white));
    /* Console text goes around the screen while it is claimed. */
    print("GFXTEST: drawn %ux%u\n",(unsigned long long)d.width,(unsigned long long)d.height);
    check("text-bypasses-screen",pixel_is(0,0,back) && pixel_is(8,8,back) && pixel_is(4,d.height-8,back));
    check("pattern",pixel_is(20,20,red) && pixel_is(d.width-20,20,green) && pixel_is(d.width/2,d.height/2,white));
    if(want_hold) hold();
    print("GFXTEST: %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    if(keep) return failed?1:0;
    check("release",!io->display_release(io->context,token));
    check("release-twice",io->display_release(io->context,token)==DE_HANDLE);
    check("restored",!io->display_mode(io->context,current,&now) && (now.flags&IO_DISPLAY_CURRENT));
    print("GFXTEST: released; %u passed, %u failed\n",(unsigned long long)passed,(unsigned long long)failed);
    return failed?1:0;
}
