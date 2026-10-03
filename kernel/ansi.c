/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * ANSI.SYS: the escape sequences MS-DOS 4's ANSI.SYS reads in CON output,
 * and its key reassignment, once DEVICE=ANSI.SYS has turned them on
 * (DOS_INSTALLED_ANSI); the screen is IO.SYS's text console
 * (IO_CAP_TEXT_SCREEN). As DOS 4's: ESC [, parameters (decimal bytes, or
 * quoted strings a byte per character, split by ';'; '=' and '?' are
 * passed over) and a letter:
 *   A B C D  the cursor up, down, forward, back (1 for 0), to the edge
 *   H f      the cursor to row;column (1 for 0); a row below the screen
 *            leaves it, a column beyond takes the last
 *   J        the screen erased, the cursor home (whatever the parameter)
 *   K        the rest of the line erased
 *   s u      the cursor's position kept, and taken again
 *   n        the cursor's position typed back: ESC [ rr ; cc R CR
 *   m        the attribute changed by each parameter's masks (GRMODE)
 *   h l      7 wraps text at the line's end (h) or keeps it in the last
 *            column (l); 0-6 and 13-19 set that video mode
 *   p        a key (its character, or 0;scan) types the rest instead
 *   q        0 and 1 clear and set /X
 *   R        nothing (the report n types back)
 * Any other letter is shown, as is a character after a lone ESC. Text is
 * written in the attribute; erasing and scrolling fill with it.
 */
#include "dos.h"
#include "console.h"
#define ASSIGN_MAX 400 /* bytes of key definitions (ASNMAX) */
enum {PLAIN,ESCAPE,SEQUENCE,QUOTED};
/* SGR parameter, AND and OR masks. */
static const u8 renditions[][3]={
    {0,0x00,0x07},{1,0xff,0x08},{4,0xf8,0x01},{5,0xff,0x80},{7,0xf8,0x70},{8,0x88,0x00},
    {30,0xf8,0x00},{31,0xf8,0x04},{32,0xf8,0x02},{33,0xf8,0x06},{34,0xf8,0x01},{35,0xf8,0x05},{36,0xf8,0x03},{37,0xf8,0x07},
    {40,0x8f,0x00},{41,0x8f,0x40},{42,0x8f,0x20},{43,0x8f,0x60},{44,0x8f,0x10},{45,0x8f,0x50},{46,0x8f,0x30},{47,0x8f,0x70}
};
static u32 options; /* DOS_ANSI_*, 0 while ANSI.SYS is not there */
static unsigned state,count; /* count: the parameter being read */
static u8 quote,attribute=0x07; static int no_wrap;
/* Definitions (a length byte counting itself, the key, what it types),
 * ended by a zero length, and the parameters after them, as one buffer. */
static u8 table[ASSIGN_MAX+8]; static unsigned used;
static u32 saved_column,saved_row;
/* Bytes typed ahead of the keyboard: a reassigned key's, a report. */
static u8 typed[256]; static unsigned typed_head,typed_count;

void ansi_reset(void) {
    options=0; state=PLAIN; count=used=0; table[0]=0; attribute=0x07; no_wrap=0;
    saved_column=saved_row=0; typed_head=typed_count=0;
}
u32 ansi_options(void) {return options;}
void ansi_set_options(u32 value) {
    if(!options) {
        ansi_reset();
        platform_text_attribute(attribute,0);
    }
    options=value;
}
static u8 *parameter(void) {
    while(used+1+count>=sizeof(table)) count--;
    return &table[used+1+count];
}
static void type(const u8 *p,unsigned n) {
    for(unsigned i=0;i<n && typed_count<sizeof(typed);i++) typed[(typed_head+typed_count++)%sizeof(typed)]=p[i];
}
/* The definition of a key: its character, or 0 and its scan code. */
static unsigned definition(u8 first,u8 second) {
    for(unsigned at=0;at<used;at+=table[at]) if(table[at+1]==first && (first || table[at+2]==second)) return at;
    return used;
}
static void assign(void) {
    unsigned n=count+1,key=table[used+1]?1:2;
    unsigned at=definition(table[used+1],table[used+2]);
    if(at<used) { /* The old definition goes, the parameters moving down with the rest. */
        unsigned length=table[at];
        memmove(table+at,table+at+length,sizeof(table)-at-length); used-=length;
    }
    if(n>key && used+n+1<ASSIGN_MAX) {table[used]=(u8)(n+1); used+=n+1;}
    table[used]=0;
}
static void report(u32 column,u32 row) {
    u8 text[9]={27,'[',(u8)('0'+(row+1)/10%10),(u8)('0'+(row+1)%10),';',(u8)('0'+(column+1)/10%10),(u8)('0'+(column+1)%10),'R','\r'};
    typed_count=0; type(text,sizeof(text));
}
static void command(u8 c) {
    const u8 *p=&table[used+1]; unsigned first=p[0],times=first?first:1;
    IoTextScreen s={.size=sizeof(s)};
    switch(c) {
    case 'm':
        for(unsigned i=0;i<=count;i++) for(unsigned k=0;k<ARRAY_SIZE(renditions);k++)
            if(renditions[k][0]==p[i]) attribute=(u8)((attribute&renditions[k][1])|renditions[k][2]);
        platform_text_attribute(attribute,no_wrap?IO_TEXT_NOWRAP:0);
        return;
    case 'h': case 'l':
        if(first==7) {no_wrap=c=='l'; platform_text_attribute(attribute,no_wrap?IO_TEXT_NOWRAP:0);}
        else if(first<=6 || (first>=13 && first<=19)) platform_text_mode(first);
        return;
    case 'p': assign(); return;
    case 'q':
        if(first==0) options&=~DOS_ANSI_X; else if(first==1) options|=DOS_ANSI_X;
        return;
    case 'R': return;
    case 'A': case 'B': case 'C': case 'D': case 'H': case 'f': case 'J': case 'K': case 'n': case 's': case 'u': break;
    default: console_write_plain(&c,1); return;
    }
    if(platform_text_query(&s) || !s.columns || !s.rows) return;
    u32 column=MIN(s.column,s.columns-1),row=MIN(s.row,s.rows-1);
    switch(c) {
    case 'A': row=row>times?row-times:0; break;
    case 'B': row=MIN(row+times,s.rows-1); break;
    case 'C': column=MIN(column+times,s.columns-1); break;
    case 'D': column=column>times?column-times:0; break;
    case 'H': case 'f':
        if(times>s.rows) return;
        row=times-1; column=count && p[1]?MIN(p[1]-1u,s.columns-1):0;
        break;
    case 'J': platform_text_erase(0,0,s.columns*s.rows); column=row=0; break;
    case 'K': platform_text_erase(column,row,s.columns-column); return;
    case 'n': report(column,row); return;
    case 's': saved_column=column; saved_row=row; return;
    case 'u': column=MIN(saved_column,s.columns-1); row=MIN(saved_row,s.rows-1); break;
    }
    platform_text_locate(column,row);
}
static void byte(u8 c) {
    switch(state) {
    case ESCAPE:
        if(c=='[') {state=SEQUENCE; count=0; *parameter()=0; return;}
        state=PLAIN;
        if(c==27) state=ESCAPE; else console_write_plain(&c,1);
        return;
    case SEQUENCE:
        if(c==';') {count++; *parameter()=0; return;}
        if(c>='0' && c<='9') {u8 *p=parameter(); *p=(u8)(*p*10+c-'0'); return;}
        if(c=='=' || c=='?') return;
        if(c=='"' || c=='\'') {state=QUOTED; quote=c; return;}
        state=PLAIN; command(c);
        return;
    case QUOTED:
        if(c==quote) {state=SEQUENCE; if(count) count--; return;}
        *parameter()=c; count++; *parameter()=0;
        return;
    default:
        if(c==27) state=ESCAPE; else console_write_plain(&c,1);
    }
}
/* CON output: runs of text as they are, sequences acted on. A sequence
 * may arrive a byte at a time (AH=02h). */
void ansi_write(const u8 *p,u32 n) {
    u32 start=0;
    for(u32 i=0;i<n;i++) {
        if(state==PLAIN && p[i]!=27) continue;
        if(i>start) console_write_plain(p+start,i-start);
        start=i+1;
        byte(p[i]);
    }
    if(n>start) console_write_plain(p+start,n-start);
}
int ansi_typed(u8 *value,int peek) {
    if(!typed_count) return 0;
    *value=typed[typed_head];
    if(!peek) {typed_head=(typed_head+1)%sizeof(typed); typed_count--;}
    return 1;
}
int ansi_key(u8 first,u8 second) {
    if(!options) return 0;
    unsigned at=definition(first,second); if(at>=used) return 0;
    unsigned key=first?1:2; type(&table[at+1+key],table[at]-1u-key);
    return 1;
}
void ansi_flush(void) {typed_count=0;}
/* IOCTL 440Ch for CON, category 3: display information (7Fh) and its
 * setting (5Fh), the text screen as it is the only one there is. */
int ansi_request(unsigned function,void *buffer,u32 size,u32 *transferred) {
    u8 *packet=buffer; IoTextScreen s={.size=sizeof(s)};
    if(!options || (function!=0x7f && function!=0x5f)) return DE_FUNCTION;
    if(!packet || size<18 || packet[0] || rd16(packet+2)<14) return DE_FUNCTION;
    if(platform_text_query(&s)) return DE_FUNCTION;
    if(function==0x5f) {
        if(rd16(packet+2)!=14 || (rd16(packet+4)&~1u) || packet[6]!=1 || rd16(packet+8)!=16 ||
           rd16(packet+14)!=s.columns || rd16(packet+16)!=s.rows || (rd16(packet+4)&1)!=(s.flags&IO_TEXT_INTENSITY?1u:0u))
            return DE_FUNCTION;
        *transferred=0; return 0;
    }
    wr16(packet+2,14); wr16(packet+4,s.flags&IO_TEXT_INTENSITY?1:0); packet[6]=1; packet[7]=0; wr16(packet+8,16);
    wr16(packet+10,0xffff); wr16(packet+12,0xffff); wr16(packet+14,(u16)s.columns); wr16(packet+16,(u16)s.rows);
    *transferred=18; return 0;
}
