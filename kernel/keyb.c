/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * Resident KEYB state processor, based on DOS 4 KEYBI9.ASM, KEYBI9C.ASM
 * and KEYBI2F.ASM. KEYB loads language tables from KEYBOARD.SYS. Firmware
 * keys with shift-state data are mapped back to US PC scan codes/BIOS flags.
 * State logic emits CON-code-page characters, no output for dead keys, or
 * the firmware character. Ctrl+Alt+F1/F2 switches to US/back; scan codes
 * above 88 are excluded. BEEP and full-buffer sounds are silent without an
 * IO.SYS speaker.
 */
#include "keyb.h"

/* The BIOS's flags (POSTEQU.INC) and KEYB's own (KEYBSHAR.INC). */
#define RIGHT_SHIFT 0x01
#define LEFT_SHIFT 0x02
#define CTL_SHIFT 0x04
#define ALT_SHIFT 0x08
#define SCROLL_STATE 0x10
#define NUM_STATE 0x20
#define CAPS_STATE 0x40
#define L_CTL_SHIFT 0x01
#define L_ALT_SHIFT 0x02
#define LC_E0 0x02
#define R_CTL_SHIFT 0x04
#define R_ALT_SHIFT 0x08
#define KBX 0x10
#define EITHER_SHIFT 0x80
#define EITHER_CTL 0x40
#define EITHER_ALT 0x20
#define SCAN_MATCH 0x08
#define EXIT_IF_FOUND 0x80
#define TYPE_2_TAB 0x40
#define ASCII_ONLY 0x80
#define ZERO_SCAN 0x20
#define G_KB 0x1000 /* KEYB_TYPE: an enhanced keyboard on an AT */
#define DEL_KEY 83
#define HOT_KEY_US 59 /* Ctrl+Alt+F1 */
#define HOT_KEY_FOREIGN 60 /* Ctrl+Alt+F2 */
#define STEPS 4096 /* commands a key may run: a GOTO loop ends as an error does */
enum {KB_FLAG,KB_FLAG_1,KB_FLAG_2,KB_FLAG_3,EXT_KB_FLAG,NLS_FLAG_1,NLS_FLAG_2};

static struct {
    int installed,foreign;
    char language[2]; u16 id,code_page;
    u8 tables[DOS_KEYB_TABLE_MAX]; u32 size;
    u32 common,specific,active; /* the logic at 0, the common section, the first specific one, the active one */
    /* FLAGS_TO_TEST: the BIOS's four flags as a key found them, then
     * EXT_KB_FLAG and the NLS flags, which last from key to key. */
    u8 flags[8];
    int caps; /* Caps Lock as the characters typed show it */
} keyb;

void keyb_reset(void) {memset(&keyb,0,sizeof(keyb));}
static u16 word(const u8 *t,u32 n,u32 at) {return at+2<=n?(u16)(t[at]|t[at+1]<<8):0;}
static u16 rd(u32 at) {return word(keyb.tables,keyb.size,at);}
/* The specific section for a code page, 0 when none. */
static u32 section(u16 page) {
    for(u32 at=keyb.specific;at<keyb.size;at+=rd(at)) if(rd(at+2)==page) return at;
    return 0;
}

/* BUFFER_FILL: entries with a byte of 0FFh type nothing. */
typedef struct {u16 *keys; u32 count;} Typed;
static void fill(Typed *o,u8 c,u8 scan) {
    if(c==0xff || scan==0xff) return;
    if(o->count<DOS_KEYB_KEYS) o->keys[o->count++]=(u16)(scan<<8|c);
}
/* A section's state of that ID for this keyboard (TABLE_BUILD loads no
 * other): its offset, and its end; 0 when there is none. */
static u32 state(u32 at,u8 id,u32 *end) {
    if(!at) return 0;
    u32 last=at+rd(at);
    for(at+=4;at+2<=last;) {
        u32 n=rd(at);
        if(n<7 || at+n>last) return 0; /* 0: the last state */
        if(keyb.tables[at+2]==id && (rd(at+3)&G_KB)) {*end=at+n; return at;}
        at+=n;
    }
    return 0;
}
static int translate(u32 at,u8 id,u8 scan,Typed *o) {
    const u8 *b=keyb.tables; u32 end;
    if(!(at=state(at,id,&end))) return 0;
    keyb.flags[EXT_KB_FLAG]&=~SCAN_MATCH;
    for(u32 t=at+7;t+2<=end;) {
        u32 size=rd(t); if(size<4 || t+size>end) return 0; /* 0: the last table */
        u8 options=b[t+2],c=0,s=scan; int found=0,narrow=(options&(ASCII_ONLY|ZERO_SCAN))!=0;
        if(options&TYPE_2_TAB) {
            u32 step=narrow?2:3,e=t+4;
            for(unsigned i=0;i<b[t+3] && e+step<=t+size;i++,e+=step)
                if(b[e]==scan) {c=b[e+1]; if(!narrow) s=b[e+2]; found=1; break;}
        } else if(size>=5 && scan>=b[t+3] && scan<=b[t+4]) {
            u32 i=scan-b[t+3];
            if(narrow) {if(t+5+i<t+size) {c=b[t+5+i]; found=1;}}
            else if(t+7+2*i<=t+size) {c=b[t+5+2*i]; s=b[t+6+2*i]; found=1;}
        }
        if(found) {fill(o,c,options&ZERO_SCAN?0:s); return 1;}
        t+=size;
    }
    return 0;
}
static int put_error(u32 at,u8 id,Typed *o) {
    u32 end; if(!(at=state(at,id,&end))) return 0;
    fill(o,keyb.tables[at+5],keyb.tables[at+6]); return 1;
}
/* A SET_FLAG state: the scan code's flag, the NLS flags cleared first. */
static int set_flag(u8 id,u8 scan) {
    const u8 *b=keyb.tables; u32 end,at=state(keyb.common,id,&end); if(!at) return 0;
    keyb.flags[EXT_KB_FLAG]&=~SCAN_MATCH;
    u32 n=at+9<=end?rd(at+7):0;
    for(u32 e=at+9;n && e+3<=end;n--,e+=3) if(b[e]==scan) {
        keyb.flags[NLS_FLAG_1]=keyb.flags[NLS_FLAG_2]=0;
        keyb.flags[b[e+1]&7]|=b[e+2];
        keyb.flags[EXT_KB_FLAG]|=SCAN_MATCH;
        return 1;
    }
    return 0;
}
/* KEYB_STATE_PROCESSOR: 1 when the key is taken (EXIT), 0 when the BIOS
 * translates it (EXIT_STATE_LOGIC). A command it does not know, or a
 * logic it cannot follow, takes the key as DOS 4's FATAL_ERROR does. */
static int process(u8 scan,Typed *o) {
    const u8 *b=keyb.tables; u8 *f=keyb.flags,option=0;
    u32 end=rd(0),si=4; unsigned nest=0,level=0,steps=0; int take_else=0;
    for(;;) {
        if(si>=end || ++steps>STEPS) return 1;
        u8 c=b[si],command=c>>4;
        u32 length=command==2 || command==3 || command==10 || command==11?1:command==8 || command==9?3:2;
        if(si+length>end) return 1;
        u8 operand=b[si+length-1]; int here=nest==level;
        switch(command) {
        case 0: case 1: { /* IFF, ANDF */
            int match=(f[c&7]&operand)!=0; if(c&8) match=!match;
            if(command==0) {
                if(here) {if(match) {level++; take_else=0;} else take_else=1;}
                nest++;
            } else if(here && !match) {take_else=1; level--;}
            break;
        }
        case 2: /* ELSEF */
            if(level==nest) level--;
            else if(take_else) {nest--; if(level==nest) {level++; take_else=0;} nest++;}
            break;
        case 3: /* ENDIFF */
            if(level==nest) level--;
            nest--; break;
        case 4: /* XLATT: the active code page's section, then the common one */
            if(here && (translate(keyb.active,operand,scan,o) || translate(keyb.common,operand,scan,o))) {
                f[EXT_KB_FLAG]|=SCAN_MATCH;
                if(option&EXIT_IF_FOUND) return 1;
            }
            break;
        case 5: /* OPTION */
            if(here) {if(c&8) option&=(u8)~operand; else option|=operand;}
            break;
        case 6: /* SET_FLAG */
            if(here && set_flag(operand,scan) && (option&EXIT_IF_FOUND)) return 1;
            break;
        case 7: /* PUT_ERROR_CHAR */
            if(here && !put_error(keyb.active,operand,o)) put_error(keyb.common,operand,o);
            break;
        case 8: /* IFKBD */
            if(here) {if(rd(si+1)&G_KB) {level++; take_else=0;} else take_else=1;}
            nest++; break;
        case 9: /* GOTO, EXIT_INT_9, EXIT_STATE_LOGIC */
            if(!here) break;
            if(c&15) return (c&15)!=2;
            si+=(u32)(int16_t)rd(si+1); nest=level=0;
            break;
        case 10: break; /* BEEP */
        case 11: if(here) f[NLS_FLAG_1]=f[NLS_FLAG_2]=0; break; /* RESET_NLS */
        default: return 1;
        }
        si+=length;
    }
}

/* The key typing this character on a US keyboard; 0 for none. */
static u8 us_scan(u32 c) {
    static const char *const keys[4]={"1234567890-=","qwertyuiop[]","asdfghjkl;'`","\\zxcvbnm,./"};
    static const char *const shifted[4]={"!@#$%^&*()_+","QWERTYUIOP{}","ASDFGHJKL:\"~","|ZXCVBNM<>?"};
    static const u8 first[4]={0x02,0x10,0x1e,0x2b};
    if(c==' ') return 0x39;
    for(unsigned r=0;r<4;r++) for(unsigned i=0;keys[r][i];i++) if(c==(u8)keys[r][i] || c==(u8)shifted[r][i]) return (u8)(first[r]+i);
    return 0;
}
/* The scan code of a key event and whether it came after E0 (the keys of
 * an enhanced keyboard's own cursor pad); 0 when it has none. Control
 * characters stand for their letters, the firmware's Ctrl+letter. */
static u8 event_scan(const IoEvent *k,int *e0) {
    static const u8 edit[]={0,72,80,77,75,71,79,82,83,73,81};
    static const u8 function[]={59,60,61,62,63,64,65,66,67,68,87,88};
    u32 c=k->unicode; *e0=0;
    if(c) {
        if(c==8) return 0x0e;
        if(c==9) return 0x0f;
        if(c==13) return 0x1c;
        if(c==27) return 0x01;
        if(c<=26) return us_scan('a'+c-1);
        return c<127?us_scan(c):0;
    }
    if(k->scan>=IO_SCAN_UP && k->scan<=IO_SCAN_PAGE_DOWN) {*e0=1; return edit[k->scan];}
    if(k->scan>=IO_SCAN_F1 && k->scan<=IO_SCAN_F12) return function[k->scan-IO_SCAN_F1];
    if(k->scan==IO_SCAN_ESCAPE) return 0x01;
    return 0;
}
u32 keyb_key(const IoEvent *k,u16 keys[DOS_KEYB_KEYS]) {
    if(!keyb.installed || !k || !keys || k->type!=IO_EVENT_KEY || (k->flags&IO_KEY_RELEASE) || !(k->flags&IO_KEY_MODIFIERS_VALID)) return DOS_KEYB_BIOS;
    int e0; u8 scan=event_scan(k,&e0); if(!scan) return DOS_KEYB_BIOS;
    u32 m=k->modifiers; u8 kb=0,kb1=0,kb3=KBX;
    if(m&IO_MOD_SHIFT) kb|=LEFT_SHIFT;
    if(m&IO_MOD_CONTROL) {kb|=CTL_SHIFT; if(m&IO_MOD_RIGHT_CONTROL) kb3|=R_CTL_SHIFT; else kb1|=L_CTL_SHIFT;}
    if(m&IO_MOD_ALT) {kb|=ALT_SHIFT; if(m&IO_MOD_RIGHT_ALT) kb3|=R_ALT_SHIFT; else kb1|=L_ALT_SHIFT;}
    if(e0) kb3|=LC_E0;
    /* The lock states as the firmware tells them; else Caps Lock by the
     * case of a letter, and Num Lock on, as KEYB leaves it. */
    if(k->flags&IO_KEY_TOGGLES_VALID) {
        keyb.caps=(m&IO_MOD_CAPS_LOCK)!=0;
        if(m&IO_MOD_NUM_LOCK) kb|=NUM_STATE;
        if(m&IO_MOD_SCROLL_LOCK) kb|=SCROLL_STATE;
    } else {
        u32 c=k->unicode;
        if(c>='a' && c<='z') keyb.caps=(m&IO_MOD_SHIFT)!=0;
        else if(c>='A' && c<='Z') keyb.caps=!(m&IO_MOD_SHIFT);
        kb|=NUM_STATE;
    }
    if(keyb.caps) kb|=CAPS_STATE;
    /* Ctrl+Alt: Del is the BIOS's; F1 and F2 are the hot keys. */
    if((kb&(CTL_SHIFT|ALT_SHIFT))==(CTL_SHIFT|ALT_SHIFT)) {
        if(scan==DEL_KEY) return DOS_KEYB_BIOS;
        if(scan==HOT_KEY_US && !e0) {keyb.foreign=0; return 0;}
        if(scan==HOT_KEY_FOREIGN && !e0) {keyb.foreign=1; return 0;}
    }
    if(!keyb.foreign || scan>88) return DOS_KEYB_BIOS;
    u8 *f=keyb.flags;
    f[KB_FLAG]=kb; f[KB_FLAG_1]=kb1; f[KB_FLAG_2]=0; f[KB_FLAG_3]=kb3;
    f[EXT_KB_FLAG]&=~(EITHER_SHIFT|EITHER_CTL|EITHER_ALT);
    if(kb&(LEFT_SHIFT|RIGHT_SHIFT)) f[EXT_KB_FLAG]|=EITHER_SHIFT;
    if((kb&CTL_SHIFT) || (kb3&R_CTL_SHIFT)) f[EXT_KB_FLAG]|=EITHER_CTL;
    if((kb&ALT_SHIFT) || (kb3&R_ALT_SHIFT)) f[EXT_KB_FLAG]|=EITHER_ALT;
    Typed o={keys,0};
    int taken=process(scan,&o);
    return o.count|(taken?0:DOS_KEYB_BIOS);
}

/* What a key would type, KEYB's state left as it was: a look at a key
 * that stays the firmware's until it is read. */
u32 keyb_look(const IoEvent *k,u16 keys[DOS_KEYB_KEYS]) {
    u8 flags[sizeof(keyb.flags)]; int caps=keyb.caps,foreign=keyb.foreign;
    memcpy(flags,keyb.flags,sizeof(flags));
    u32 r=keyb_key(k,keys);
    memcpy(keyb.flags,flags,sizeof(flags)); keyb.caps=caps; keyb.foreign=foreign;
    return r;
}
int keyb_code_page(u16 page) {
    if(!keyb.installed) return 0;
    u32 at=section(page); if(!at) return DE_NOFILE;
    keyb.active=at; keyb.code_page=page; return 0;
}
/* The tables as KEYB lays them out: each section within them, the logic at
 * least 5 bytes, the common and specific sections at least 4. */
static int check(const u8 *t,u32 n,u32 *common,u32 *specific) {
    if(!t || n>DOS_KEYB_TABLE_MAX) return DE_NOMEM;
    u32 logic=word(t,n,0);
    if(logic<5 || logic+4>n) return DE_FORMAT;
    u32 shared=word(t,n,logic);
    if(shared<4 || logic+shared>n) return DE_FORMAT;
    *common=logic; *specific=logic+shared;
    for(u32 at=*specific;at<n;) {
        u32 size=word(t,n,at);
        if(size<4 || at+size>n) return DE_FORMAT;
        at+=size;
    }
    return 0;
}
int keyb_request(u32 function,DosKeybRequest *r) {
    if(!r || r->size<sizeof(*r)) return DE_FUNCTION;
    switch(function) {
    case DOS_KEYB_QUERY:
        if(!keyb.installed) return DE_FUNCTION;
        r->flags=keyb.foreign?DOS_KEYB_FOREIGN:0;
        memcpy(r->language,keyb.language,2); r->id=keyb.id; r->code_page=keyb.code_page;
        return 0;
    case DOS_KEYB_LOAD: {
        u32 common,specific; int e=check(r->tables,r->table_size,&common,&specific); if(e) return e;
        u32 at=specific; const u8 *t=r->tables;
        while(at<r->table_size && word(t,r->table_size,at+2)!=r->code_page) at+=word(t,r->table_size,at);
        if(at>=r->table_size) return DE_NOFILE;
        memcpy(keyb.tables,t,r->table_size); keyb.size=r->table_size;
        keyb.common=common; keyb.specific=specific; keyb.active=at;
        memcpy(keyb.language,r->language,2); keyb.id=r->id; keyb.code_page=r->code_page;
        memset(keyb.flags,0,sizeof(keyb.flags));
        keyb.installed=keyb.foreign=1;
        return 0;
    }
    case DOS_KEYB_MODE:
        if(!keyb.installed) return DE_FUNCTION;
        keyb.foreign=(r->flags&DOS_KEYB_FOREIGN)!=0; return 0;
    case DOS_KEYB_CODE_PAGE:
        if(!keyb.installed) return DE_FUNCTION;
        return keyb_code_page(r->code_page);
    default: return DE_FUNCTION;
    }
}
