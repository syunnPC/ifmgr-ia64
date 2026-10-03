/* SPDX-License-Identifier: GPL-2.0-or-later
 * Firmware-neutral keyboard translation, CON code-page transcoding and DOS
 * buffered line editing. DBCS characters are never split by editing.
 */
#include "dos.h"
#include "console.h"
#include "codepage.h"
#include "print.h"
#include "keyb.h"
static int extended=-1,lead=-1;
/* Keys typed through KEYB (scan code << 8 | character), with the BIOS's
 * translation of a key KEYB passes on after its own: the BIOS's buffer. */
#define TYPED_MAX 16U
static u16 typed[TYPED_MAX];
static unsigned typed_head,typed_count;
void console_reset(void) {extended=lead=-1; typed_head=typed_count=0; codepage_reset(); ansi_reset(); keyb_reset();}
void console_codepage_changed(void) {lead=-1;}
static void typed_put(u16 w) {if(typed_count<TYPED_MAX) typed[(typed_head+typed_count++)%TYPED_MAX]=w;}
static void typed_drop(void) {typed_head=(typed_head+1)%TYPED_MAX; typed_count--;}
static u8 scan_code(u32 scan) {
    static const u8 codes[]={0,72,80,77,75,71,79,82,83,73,81,
        59,60,61,62,63,64,65,66,67,68,133,134,1};
    return scan<ARRAY_SIZE(codes)?codes[scan]:0;
}
/* Alt+Tab, Alt+Esc and Ctrl+Esc: keys that may switch away from the program. */
static u32 switch_key(const IoEvent *k) {
    if(!(k->flags&IO_KEY_MODIFIERS_VALID)) return 0;
    if((k->modifiers&IO_MOD_ALT) && k->unicode=='\t') return DOS_SWITCH_ALT_TAB;
    if((k->modifiers&IO_MOD_ALT) && k->scan==IO_SCAN_ESCAPE) return DOS_SWITCH_ALT_ESC;
    if((k->modifiers&IO_MOD_CONTROL) && k->scan==IO_SCAN_ESCAPE) return DOS_SWITCH_CTRL_ESC;
    return 0;
}
/* A key; a wait gives PRINT its time (DOS's INT 28h) while it has
 * something to print. */
static int next_key(IoEvent *key,unsigned flags) {
    while(flags&IO_KEY_WAIT) {
        int e=platform_console_key(key,flags&~IO_KEY_WAIT); if(e!=DE_NOTREADY) return e;
        u32 ms=print_idle(); if(!ms) break;
        platform_wait(ms);
    }
    return platform_console_key(key,flags);
}
/* The BIOS's translation of a key: its bytes in the code page (a DBCS
 * character two), else the scan code of an extended key; neither for keys
 * the page cannot show. */
static unsigned bios_key(const IoEvent *key,u8 bytes[2],u8 *scan) {
    u32 c=key->unicode;
    int control=(key->flags&IO_KEY_MODIFIERS_VALID) && (key->modifiers&IO_MOD_CONTROL);
    if(control && key->scan==IO_SCAN_PAUSE) c=3;
    if(control && c>='@' && c<='_') c&=31;
    else if(control && c>='a' && c<='z') c&=31;
    *scan=0;
    if(!c && key->scan==IO_SCAN_ESCAPE) {bytes[0]=27; return 1;}
    unsigned n=c<32?(bytes[0]=(u8)c,c!=0):codepage_encode(c,bytes);
    if(!n) *scan=scan_code(key->scan);
    return n;
}
/* KEYB's words for a key it takes, then the BIOS's translation when it
 * leaves that, to the buffer. */
static void queue_keys(const IoEvent *key,u32 r,const u16 *words) {
    for(u32 i=0;i<(r&~DOS_KEYB_BIOS);i++) typed_put(words[i]);
    if(r&DOS_KEYB_BIOS) {
        u8 bytes[2],scan; unsigned n=bios_key(key,bytes,&scan);
        if(n==1) typed_put(bytes[0]); else if(!n && scan) typed_put((u16)(scan<<8));
    }
}
static int keyb_take(IoEvent *key) {
    u16 words[DOS_KEYB_KEYS]; int e=platform_console_key(key,0); if(e) return e;
    queue_keys(key,keyb_key(key,words),words); return 0;
}
/* Where the key looked at comes from: the firmware, untranslated or the
 * BIOS's; the buffer; or the firmware, KEYB's first word for it. */
enum {FROM_FIRMWARE,FROM_BUFFER,FROM_KEYB};
static int take_looked(int from,IoEvent *key) {
    if(from==FROM_BUFFER) {typed_drop(); return 0;}
    if(from==FROM_KEYB) {int e=keyb_take(key); if(!e) typed_drop(); return e;}
    return platform_console_key(key,0);
}
int console_byte(int wait,int peek,u8 *value) {
    if(ansi_typed(value,peek)) return 0;
    if(extended>=0) {*value=extended; if(!peek) extended=-1; return 0;}
    for(;;) {
        IoEvent key; u8 bytes[2],scan; unsigned n; int e,from=FROM_FIRMWARE;
        if(typed_count) {
            /* KEYB's characters are the code page's already. */
            u16 w=typed[typed_head];
            bytes[0]=(u8)w; n=bytes[0]!=0; scan=n?0:(u8)(w>>8); from=FROM_BUFFER;
        } else {
            e=next_key(&key,(wait?IO_KEY_WAIT:0)|(peek?IO_KEY_PEEK:0)); if(e) return e;
            /* A switching key the switch hook takes is consumed, waits and
             * polls alike; the program goes on when it is switched back to. */
            u32 to=switch_key(&key);
            if(to && dos_switch_away(to|DOS_SWITCH_QUERY)) {
                if(peek) {e=platform_console_key(&key,0); if(e) return e;}
                dos_switch_away(to);
                continue;
            }
            /* A key KEYB translates: a read queues its words (and what the
             * BIOS types for it after them); a look leaves it to whoever
             * reads it next (VDM's INT 16h reads the firmware itself) and
             * shows the first, but takes one that types nothing, a dead or
             * hot key, which changes KEYB's state alone. */
            u16 words[DOS_KEYB_KEYS]; u32 r=peek?keyb_look(&key,words):keyb_key(&key,words);
            if(r==DOS_KEYB_BIOS) n=bios_key(&key,bytes,&scan);
            else if(!peek) {queue_keys(&key,r,words); continue;}
            else if(!r) {e=keyb_take(&key); if(e) return e; continue;}
            else {
                from=FROM_KEYB;
                if(r&~DOS_KEYB_BIOS) {bytes[0]=(u8)words[0]; n=bytes[0]!=0; scan=n?0:(u8)(words[0]>>8);}
                else n=bios_key(&key,bytes,&scan);
            }
        }
        /* A key ANSI.SYS reassigns types its definition instead. */
        if(n<2 && (n || scan) && ansi_key(n?bytes[0]:0,scan)) {
            if(from==FROM_BUFFER || peek) {e=take_looked(from,&key); if(e) return e;}
            if(ansi_typed(value,peek)) return 0;
            continue;
        }
        /* A DBCS key yields its lead byte now and queues the trail, like the
         * scan code after an extended key's zero byte. */
        if(n || scan) {
            *value=n?bytes[0]:0;
            if(!peek) {if(from==FROM_BUFFER) typed_drop(); if(n==2) extended=bytes[1]; else if(!n) extended=scan;}
            return 0;
        }
        /* Unsupported Unicode/scans cannot be represented in this DOS code
         * page. Consume them even while peeking, without eating pointer input. */
        if(from==FROM_BUFFER || peek) {e=take_looked(from,&key); if(e) return e;}
    }
}
int console_flush(void) {
    extended=-1; typed_head=typed_count=0; ansi_flush();
    for(;;) {
        IoEvent key; int e=platform_console_key(&key,0);
        if(e==DE_NOTREADY || e==DE_EOF) return 0;
        if(e) return e;
    }
}
/* CON output, through ANSI.SYS when it is there. */
void console_write(const void *data,u32 count) {
    if(ansi_options()) ansi_write(data,count); else console_write_plain(data,count);
}
/* CON output in the selected code page. A lead byte waits for its trail even
 * across separate writes (AH=02h); an orphan lead or unmapped byte shows '?'. */
void console_write_plain(const void *data,u32 count) {
    const u8 *p=data; u16 text[64]; unsigned used=0;
    for(u32 i=0;i<count;i++) {
        u8 c=p[i]; u16 unit;
        if(lead>=0) {
            u8 first=(u8)lead; lead=-1;
            if(c>=0x40 && c!=0x7f) {unit=codepage_unicode(first,1,c); text[used++]=unit?unit:'?'; goto next;}
            text[used++]='?';
            if(used==ARRAY_SIZE(text)) {platform_console_text(text,used); used=0;}
        }
        if(codepage_lead(c)) {lead=c; continue;}
        unit=codepage_unicode(c,0,0); text[used++]=unit||!c?unit:'?';
next:
        if(used==ARRAY_SIZE(text)) {platform_console_text(text,used); used=0;}
    }
    if(used) platform_console_text(text,used);
}
static int emit(const ConsoleLineIo *io,const void *p,u32 n) {return io->write(io->context,p,n);}
static unsigned width(u8 c,unsigned column) {return c==9?8-(column&7):c<32?2:1;}
static int echo(const ConsoleLineIo *io,u8 c) {
    if(c<32 && c!=9) {u8 text[2]={'^',(u8)(c+'@')}; return emit(io,text,2);}
    return emit(io,&c,1);
}
/* Character sizes within a buffer of n bytes; a final lone lead is one byte. */
static unsigned char_size(const u8 *s,unsigned at,unsigned n) {return at+1<n && codepage_lead(s[at])?2:1;}
static unsigned char_before(const u8 *s,unsigned at) {
    unsigned i=0,size=0;
    while(i<at) {size=char_size(s,i,at); i+=size;}
    return size;
}
/* A failed read ends the line: at the end of input with what was typed
 * (DE_EOF only when nothing was), otherwise empty. */
static int read_failed(u8 *buffer,const u8 *line,unsigned used,int e) {
    if(e!=DE_EOF) {buffer[1]=0; return e;}
    buffer[1]=used; memcpy(buffer+2,line,used); buffer[used+2]='\r';
    return used?0:e;
}
int console_line(u8 *buffer,const ConsoleLineIo *io) {
    unsigned maximum=buffer[0]; if(!maximum) return 0;
    unsigned old=buffer[1],cursor=0,used=0,column=io->column;
    u8 line[255],template[255]; unsigned widths[255]; int insert=0,first=1;
    if(old>=maximum || buffer[old+2]!='\r') old=0;
    memcpy(template,buffer+2,old);
    for(;;) {
        u8 c; int e=io->read(io->context,&c);
        if(e) return read_failed(buffer,line,used,e);
        if(first && c=='\n') {first=0; continue;} first=0;
        if(!c) {
            e=io->read(io->context,&c); if(e) return e;
            unsigned take=0;
            if(c==59 || c==77) take=1; /* F1/right: copy one template character. */
            else if(c==61) take=old-cursor; /* F3: copy remainder. */
            else if(c==60 || c==62) {
                u8 wanted,trail=0; e=io->read(io->context,&wanted); if(e) return e;
                if(!wanted) {e=io->read(io->context,&wanted); if(e) return e; continue;}
                if(codepage_lead(wanted)) {e=io->read(io->context,&trail); if(e) return e;}
                if(cursor>=old) continue;
                unsigned end=cursor+char_size(template,cursor,old);
                while(end<old && (template[end]!=wanted || (trail && (end+1>=old || template[end+1]!=trail))))
                    end+=char_size(template,end,old);
                if(end>=old) continue;
                if(c==62) {cursor=end; continue;} /* F4: skip to character. */
                take=end-cursor;
            } else if(c==83) {if(cursor<old) cursor+=char_size(template,cursor,old); continue;} /* Delete template character. */
            else if(c==82) {insert=!insert; continue;}
            else if(c==75) c=8;
            else if(c==63) { /* F5 makes the edited line the new template. */
                memcpy(template,line,used); old=used;
                e=emit(io,"@\r\n",3); if(e) return e;
                used=cursor=0; insert=0; column=io->column;
                for(unsigned i=0;i<column;i++) {e=emit(io," ",1); if(e) return e;}
                continue;
            } else if(c==64) c=26;
            else continue;
            if(take) {
                insert=0;
                while(take && cursor<old) {
                    unsigned size=char_size(template,cursor,old);
                    if(used+size>maximum-1) break;
                    for(unsigned i=0;i<size;i++) {
                        u8 byte=template[cursor++]; widths[used]=width(byte,column); column+=widths[used]; line[used++]=byte;
                        e=echo(io,byte); if(e) return e;
                    }
                    take=take>size?take-size:0;
                }
                continue;
            }
            if(c!=8 && c!=26) continue;
        }
        if(c=='\r') {
            buffer[1]=used; memcpy(buffer+2,line,used); buffer[used+2]='\r';
            return emit(io,"\r",1);
        }
        if(c==8 || c==127) {
            if(used) {
                unsigned size=char_before(line,used),cells=0;
                while(size--) cells+=widths[--used];
                column=column>=cells?column-cells:0;
                while(cells--) {e=emit(io,"\b \b",3); if(e) return e;}
            }
            if(!insert && cursor) cursor-=char_before(template,cursor);
            continue;
        }
        if(c==27) {
            e=emit(io,"\\\r\n",3); if(e) return e;
            used=cursor=0; insert=0; column=io->column;
            for(unsigned i=0;i<column;i++) {e=emit(io," ",1); if(e) return e;}
            continue;
        }
        if(c=='\n') {e=emit(io,"\r\n",2); if(e) return e; column=0; continue;}
        if(c==6) continue;
        u8 pair[2]={c,0}; unsigned size=1;
        if(codepage_lead(c)) {
            e=io->read(io->context,&pair[1]);
            if(e) return read_failed(buffer,line,used,e);
            if(pair[1]<0x40 || pair[1]==0x7f) continue; /* Drop a lead without a trail. */
            size=2;
        }
        if(used+size>maximum-1) {e=emit(io,"\a",1); if(e) return e; continue;}
        for(unsigned i=0;i<size;i++) {
            widths[used]=width(pair[i],column); column+=widths[used]; line[used++]=pair[i];
            e=echo(io,pair[i]); if(e) return e;
        }
        if(!insert && cursor<old) cursor+=char_size(template,cursor,old);
    }
}
