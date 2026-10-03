/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows help reader. Format reference: Manfred Winterhoff's GPL
 * helpdeco/helpfile.txt. Load the file into memory, index the topic chain
 * once, and decode topics on demand.
 *
 * Internal files use a B+ tree directory: |SYSTEM holds version/title/
 * compression (15=3.0, 21=3.1); |Phrases and |FONT hold text phrases/fonts.
 * |TOPIC has 2 KiB blocks in 3.0, or 4 KiB LZ77 blocks expanding to 16 KiB
 * in 3.1. Topic links contain headers, paragraph/format records and
 * NUL-separated text. |TOMAP (3.0)/|CONTEXT (3.1) resolve jumps, |CTXOMAP
 * resolves [MAP] IDs, and |KWBTREE/|KWDATA index keywords. bmN pictures are
 * SHG DIBs, DDBs or metafiles, optionally RLE/LZ77-compressed.
 */
#include <windows.h>
#include <string.h>
#include "winapp.h"
#include "hlpfile.h"

typedef struct {DWORD stream,offset,previous,next; char *title;} TopicEntry;
struct HelpFile {
    BYTE *data; DWORD size; char path[MAX_PATH],title[128];
    int minor; BOOL compressed; DWORD block_size,decompress_size;
    BYTE *stream; DWORD stream_size,*block_start,blocks;  /* |TOPIC's blocks, expanded, one after another */
    BYTE *phrase_text; const BYTE *phrase_table; int phrase_count; WORD phrase_base;
    HelpFont *fonts; int font_count;
    TopicEntry *topics; DWORD topic_count;
    const BYTE *tomap; DWORD tomap_count;
    DWORD contents; BOOL has_contents;
    const BYTE *context,*ctxomap,*kwdata; DWORD context_size,ctxomap_size,kwdata_size;
    char **keywords; WORD *keyword_counts; DWORD *keyword_data; int keyword_count;
};

static WORD get16(const BYTE *p) {return (WORD)(p[0]|p[1]<<8);}
static DWORD get32(const BYTE *p) {return get16(p)|(DWORD)get16(p+2)<<16;}
/* A bigger copy of an array (the old one goes); NULL, the old one kept, when there is no memory. */
static void *grow(void *old,DWORD old_bytes,DWORD bytes) {
    void *n=(void *)GlobalAlloc(GPTR,bytes);
    if(n && old) {memcpy(n,old,(size_t)min(old_bytes,bytes)); GlobalFree((HGLOBAL)old);}
    return n;
}
static void release(void *p) {if(p) GlobalFree((HGLOBAL)p);}

/* --- compression --------------------------------------------------------------------------- */
/* LZ77: a byte of flags, the low bit first; a set one is a word of a 12-bit
 * distance (back from the end, less one) and a 4-bit length (less three),
 * a clear one a byte as it is. At most max bytes out; how many came. */
static DWORD lz77(const BYTE *in,DWORD n,BYTE *out,DWORD max) {
    DWORD i=0,o=0;
    while(i<n && o<max) {
        BYTE flags=in[i++]; int bit;
        for(bit=0;bit<8 && i<n && o<max;bit++) {
            if(flags&(1<<bit)) {
                WORD w; DWORD back; int length;
                if(i+2>n) return o;
                w=get16(in+i); i+=2; back=(DWORD)(w&0xfff)+1; length=(w>>12)+3;
                while(length-- && o<max) {out[o]=o>=back?out[o-back]:0; o++;}
            } else out[o++]=in[i++];
        }
    }
    return o;
}
/* Run lengths: a count with the top bit set copies that many bytes, without it repeats the next one. */
static DWORD unrun(const BYTE *in,DWORD n,BYTE *out,DWORD max) {
    DWORD i=0,o=0;
    while(i<n && o<max) {
        BYTE count=in[i++];
        if(count&0x80) {count&=0x7f; while(count-- && i<n && o<max) out[o++]=in[i++];}
        else {if(i>=n) break; while(count-- && o<max) out[o++]=in[i];  i++;}
    }
    return o;
}

/* --- the file system --------------------------------------------------------------------- */
/* Every leaf entry of a B+ tree in order (the first leaf is under the first
 * child of each index page); entry() reads one, returning what follows or NULL to stop. */
typedef const BYTE *(*LeafEntry)(const BYTE *entry,const BYTE *end,void *data);
static void btree(const BYTE *tree,DWORD size,LeafEntry entry,void *data) {
    WORD page_size,levels,pages,level; short page; DWORD walked=0;
    if(!tree || size<38 || get16(tree)!=0x293b) return;
    page_size=get16(tree+4); page=(short)get16(tree+26); pages=get16(tree+30); levels=get16(tree+32);
    if(page_size<8 || 38+(DWORD)pages*page_size>size) return;
    for(level=1;level<levels;level++) {
        if(page<0 || page>=(short)pages) return;
        page=(short)get16(tree+38+(DWORD)page*page_size+4);
    }
    while(page>=0 && page<(short)pages && walked++<=pages) {
        const BYTE *p=tree+38+(DWORD)page*page_size,*end=p+page_size,*e=p+8; int n=(short)get16(p+2),i;
        for(i=0;i<n && e && e<end;i++) if(!(e=entry(e,end,data))) return;
        page=(short)get16(p+6);
    }
}
typedef struct {LPCSTR name; DWORD offset; BOOL found;} Lookup;
static const BYTE *directory_entry(const BYTE *e,const BYTE *end,void *data) {
    Lookup *l=(Lookup *)data; const BYTE *z=e;
    while(z<end && *z) z++;
    if(z+5>end) return NULL;
    if(!lstrcmp((LPCSTR)e,l->name)) {l->offset=get32(z+1); l->found=TRUE; return NULL;}
    return z+5;
}
/* An internal file's contents (after its 9-byte header) and size; NULL if there is none. */
static const BYTE *subfile(const HelpFile *h,LPCSTR name,DWORD *size) {
    Lookup l; DWORD dir=get32(h->data+4),used;
    l.name=name; l.found=FALSE;
    if(dir+9>h->size) return NULL;
    btree(h->data+dir+9,min(get32(h->data+dir+4),h->size-dir-9),directory_entry,&l);
    if(!l.found && name[0]=='|') {l.name=name+1; btree(h->data+dir+9,min(get32(h->data+dir+4),h->size-dir-9),directory_entry,&l);}
    if(!l.found || l.offset+9>h->size) return NULL;
    used=get32(h->data+l.offset+4);
    if(used>h->size-l.offset-9) used=h->size-l.offset-9;
    *size=used;
    return h->data+l.offset+9;
}

/* --- |SYSTEM, |Phrases, |FONT ------------------------------------------------------------ */
static BOOL read_system(HelpFile *h) {
    DWORD size; const BYTE *p=subfile(h,"|SYSTEM",&size),*end; WORD flags;
    if(!p || size<12 || get16(p)!=0x036c) return FALSE;
    h->minor=get16(p+2); flags=get16(p+10); end=p+size;
    h->compressed=h->minor>16 && (flags==4 || flags==8);
    h->block_size=h->minor<=16 || flags==8?2048:4096;
    h->decompress_size=h->compressed?16384:h->block_size-12;
    if(h->minor<=16) lstrcpyn(h->title,(LPCSTR)p+12,(int)min(size-12+1,sizeof(h->title)));
    else for(p+=12;p+4<=end;p+=4+get16(p+2)) {
        WORD type=get16(p),length=get16(p+2);
        if(p+4+length>end) break;
        if(type==1) lstrcpyn(h->title,(LPCSTR)p+4,(int)min(length+1,sizeof(h->title)));
        if(type==3 && length>=4) {h->contents=get32(p+4); h->has_contents=TRUE;}
    }
    return TRUE;
}
/* The phrases: a count, 0x0100, (3.1: the expanded size,) offsets from the
 * offset table, then the phrases, LZ77 packed in 3.1's files. */
static void read_phrases(HelpFile *h) {
    DWORD size,expanded; const BYTE *p=subfile(h,"|Phrases",&size); int n; DWORD table;
    if(!p || size<4) return;
    n=get16(p); table=h->minor<=16?4:8;
    if(!n || table+2*(DWORD)(n+1)>size) return;
    h->phrase_table=p+table; h->phrase_count=n; h->phrase_base=get16(p+table);
    if(h->minor<=16) {
        expanded=size-table-h->phrase_base;
        if(!(h->phrase_text=(BYTE *)GlobalAlloc(GPTR,expanded+1))) {h->phrase_count=0; return;}
        memcpy(h->phrase_text,p+table+h->phrase_base,expanded);
    } else {
        expanded=get32(p+4);
        if(expanded>0x1000000 || !(h->phrase_text=(BYTE *)GlobalAlloc(GPTR,expanded+1))) {h->phrase_count=0; return;}
        lz77(p+table+2*(n+1),size-table-2*(n+1),h->phrase_text,expanded);
    }
}
/* Fonts: face names, then 11-byte descriptors (attributes, half points,
 * family, face, foreground and background colours). Later formats are
 * not read: their text comes out in the first face. */
static void read_fonts(HelpFile *h) {
    DWORD size; const BYTE *p=subfile(h,"|FONT",&size); int faces,count,i,face_length; WORD face_at,desc_at;
    if(!p || size<8) return;
    faces=get16(p); count=get16(p+2); face_at=get16(p+4); desc_at=get16(p+6);
    if(!faces || face_at>=12 || desc_at<face_at || desc_at+(DWORD)count*11>size) return;
    face_length=(desc_at-face_at)/faces;
    if(!(h->fonts=(HelpFont *)GlobalAlloc(GPTR,(DWORD)(count+1)*sizeof(HelpFont)))) return;
    for(i=0;i<count;i++) {
        const BYTE *d=p+desc_at+i*11; HelpFont *f=&h->fonts[i]; WORD face=get16(d+3);
        f->attributes=d[0]; f->half_points=d[1]; f->family=d[2]; f->color=RGB(d[5],d[6],d[7]);
        if(face<faces) lstrcpyn(f->face,(LPCSTR)p+face_at+face*face_length,min(face_length+1,(int)sizeof(f->face)));
    }
    h->font_count=count;
}

/* --- |TOPIC -------------------------------------------------------------------------------- */
/* The blocks one after another: 3.0's as stored, 3.1's expanded. */
static BOOL read_stream(HelpFile *h) {
    DWORD size,i; const BYTE *p=subfile(h,"|TOPIC",&size);
    if(!p) return FALSE;
    h->blocks=(size+h->block_size-1)/h->block_size;
    h->stream=(BYTE *)GlobalAlloc(GPTR,h->blocks*h->decompress_size+64);
    h->block_start=(DWORD *)GlobalAlloc(GPTR,(h->blocks+1)*sizeof(DWORD));
    if(!h->stream || !h->block_start) return FALSE;
    for(i=0;i<h->blocks;i++) {
        DWORD start=i*h->block_size,n;
        h->block_start[i]=h->stream_size;
        if(start+12>size) continue;
        n=min(h->block_size,size-start)-12;
        if(h->compressed) h->stream_size+=lz77(p+start+12,n,h->stream+h->stream_size,h->decompress_size);
        else {memcpy(h->stream+h->stream_size,p+start+12,n); h->stream_size+=n;}
    }
    h->block_start[h->blocks]=h->stream_size;
    return TRUE;
}
/* Where a link is in the stream: a 3.0 position counts the blocks' headers
 * (the 12 bytes before each block's data), a 3.1 TOPICPOS the expanded
 * blocks after the first header. */
static BOOL stream_at(const HelpFile *h,DWORD pos,DWORD *at) {
    DWORD block,offset;
    if(h->minor<=16) {block=pos/h->block_size; offset=pos%h->block_size; if(offset<12) return FALSE; offset-=12;}
    else {if(pos<12) return FALSE; block=(pos-12)/h->decompress_size; offset=(pos-12)%h->decompress_size;}
    if(block>=h->blocks) return FALSE;
    *at=h->block_start[block]+offset;
    return *at+21<=h->stream_size;
}
/* A link's text: its second part with the phrases put back (a byte below
 * 16 other than 0 and the next make twice a phrase's number, odd for a
 * space after it). A new buffer of the expanded size, NUL-terminated. */
static char *link_text(const HelpFile *h,const BYTE *link,DWORD *length) {
    DWORD block=get32(link),expanded=get32(link+4),first=get32(link+16),o=0;
    const BYTE *end=h->stream+h->stream_size,*in=link+first,*stop=link+block; char *out;
    if(stop>end || stop<link) stop=end;
    if(in>stop) in=stop;
    if(expanded>0x100000) expanded=0x100000;
    if(!(out=(char *)GlobalAlloc(GPTR,expanded+2))) return NULL;
    if(expanded>(DWORD)(stop-in) && h->phrase_count) {
        while(o<expanded && in<stop) {
            BYTE c=*in++;
            if(!c || c>15) out[o++]=(char)c;
            else {
                WORD code; int n; DWORD from,to;
                if(in>=stop) break;
                code=(WORD)((c-1)*256+*in++); n=code/2;
                if(n<h->phrase_count) {
                    from=get16(h->phrase_table+2*n)-h->phrase_base; to=get16(h->phrase_table+2*n+2)-h->phrase_base;
                    while(from<to && o<expanded) out[o++]=(char)h->phrase_text[from++];
                }
                if((code&1) && o<expanded) out[o++]=' ';
            }
        }
    } else {o=min(expanded,(DWORD)(stop-in)); memcpy(out,in,o);}
    *length=o;
    return out;
}
/* Compressed numbers in a record's first part. */
typedef struct {const BYTE *p,*end;} Cursor;
static BYTE c_byte(Cursor *c) {return c->p<c->end?*c->p++:0;}
static WORD c_word(Cursor *c) {WORD v=c->p+2<=c->end?get16(c->p):0; c->p+=2; return v;}
static DWORD c_dword(Cursor *c) {DWORD v=c->p+4<=c->end?get32(c->p):0; c->p+=4; return v;}
static WORD c_cushort(Cursor *c) {BYTE b=c_byte(c); return (WORD)(b&1?(b>>1)+c_byte(c)*128:b>>1);}
static short c_cshort(Cursor *c) {BYTE b=c_byte(c); return (short)(b&1?(b>>1)+c_byte(c)*128-16384:(b>>1)-64);}
static DWORD c_culong(Cursor *c) {WORD w=c_word(c); return w&1?(w>>1)+(DWORD)c_word(c)*32768:w>>1;}
static long c_clong(Cursor *c) {WORD w=c_word(c); return w&1?(long)((w>>1)+(DWORD)c_word(c)*32768)-67108864L:(long)(w>>1)-16384;}
/* The characters a display record counts toward 3.1's topic offsets. */
static DWORD record_length(const BYTE *link) {
    Cursor c; c.p=link+21; c.end=link+get32(link+16);
    c_clong(&c);
    return c_cushort(&c);
}
/* The topics, by walking the links: 3.0 adds each link's distance to the
 * next (headers counted), 3.1 follows its TOPICPOS; the last link only ends
 * the chain. A topic's offset is its position (3.0) or its block and the
 * characters before it there (3.1). */
static BOOL find_topics(HelpFile *h) {
    DWORD pos=12,at,walked=0,block=0xffffffffUL,chars=0,cap=0;
    while(walked++<0x100000 && stream_at(h,pos,&at)) {
        const BYTE *link=h->stream+at; DWORD next=get32(link+12),this_block=h->minor<=16?0:(pos-12)/h->decompress_size;
        BYTE type=link[20]; DWORD beyond;
        BOOL last=h->minor<=16?!next || !stream_at(h,pos+next,&beyond):next==0xffffffffUL || next<=pos || !stream_at(h,next,&beyond);
        if(h->minor>16 && this_block!=block) {block=this_block; chars=0;}
        if(type==2 && !last) {
            TopicEntry *t; DWORD length; char *title;
            if(h->topic_count==cap) {
                TopicEntry *n=(TopicEntry *)grow(h->topics,cap*sizeof(TopicEntry),(cap?cap*2:64)*sizeof(TopicEntry));
                cap=cap?cap*2:64;
                if(!n) return FALSE;
                h->topics=n;
            }
            t=&h->topics[h->topic_count++];
            t->stream=at; t->offset=h->minor<=16?pos:block*0x8000+chars;
            t->previous=get32(link+0x19); t->next=get32(link+0x1d);
            title=link_text(h,link,&length);
            t->title=title;
        } else if(h->minor>16 && (type==0x20 || type==0x23)) chars+=record_length(link);
        if(last) break;
        pos=h->minor<=16?pos+next:next;
    }
    return h->topic_count>0;
}
/* The topic an offset is in: the last one starting at or before it (-1 is none). */
static DWORD by_offset(const HelpFile *h,DWORD offset) {
    DWORD i,found=HELP_NO_TOPIC;
    if(offset==0xffffffffUL) return HELP_NO_TOPIC;
    for(i=0;i<h->topic_count;i++) if(h->topics[i].offset<=offset && (found==HELP_NO_TOPIC || h->topics[found].offset<=h->topics[i].offset)) found=i;
    return found;
}
typedef struct {DWORD key,value; BOOL found;} Hashed;
static const BYTE *context_entry(const BYTE *e,const BYTE *end,void *data) {
    Hashed *k=(Hashed *)data;
    if(e+8>end) return NULL;
    if(get32(e)==k->key) {k->value=get32(e+4); k->found=TRUE; return NULL;}
    return e+8;
}
/* A jump's target: 3.0's are topic numbers (|TOMAP's positions), 3.1's context hashes (|CONTEXT). */
static DWORD by_target(const HelpFile *h,DWORD target) {
    Hashed k;
    if(h->minor<=16) return target<h->tomap_count?by_offset(h,get32(h->tomap+4*target)):HELP_NO_TOPIC;
    k.key=target; k.found=FALSE;
    btree(h->context,h->context_size,context_entry,&k);
    return k.found?by_offset(h,k.value):HELP_NO_TOPIC;
}

/* --- keywords ----------------------------------------------------------------------------- */
typedef struct {HelpFile *h; int cap;} Keywords;
static const BYTE *keyword_entry(const BYTE *e,const BYTE *end,void *data) {
    Keywords *k=(Keywords *)data; HelpFile *h=k->h; const BYTE *z=e; int n=h->keyword_count; char *word;
    while(z<end && *z) z++;
    if(z+7>end) return NULL;
    if(n==k->cap) {
        int cap=k->cap?k->cap*2:64; char **w=(char **)grow(h->keywords,n*sizeof(char *),cap*sizeof(char *));
        WORD *c=(WORD *)grow(h->keyword_counts,n*sizeof(WORD),cap*sizeof(WORD)); DWORD *d=(DWORD *)grow(h->keyword_data,n*sizeof(DWORD),cap*sizeof(DWORD));
        if(w) h->keywords=w;
        if(c) h->keyword_counts=c;
        if(d) h->keyword_data=d;
        if(!w || !c || !d) return NULL;
        k->cap=cap;
    }
    if(!(word=(char *)GlobalAlloc(GPTR,(DWORD)(z-e)+1))) return NULL;
    memcpy(word,e,(size_t)(z-e));
    h->keywords[n]=word; h->keyword_counts[n]=get16(z+1); h->keyword_data[n]=get32(z+3);
    h->keyword_count++;
    return z+7;
}
static void read_keywords(HelpFile *h) {
    DWORD size; const BYTE *tree=subfile(h,"|KWBTREE",&size); Keywords k;
    k.h=h; k.cap=0;
    if(!tree || !(h->kwdata=subfile(h,"|KWDATA",&h->kwdata_size))) return;
    btree(tree,size,keyword_entry,&k);
}

/* --- opening and closing ------------------------------------------------------------------ */
void HelpClose(HelpFile *h) {
    DWORD i; int k;
    if(!h) return;
    for(i=0;i<h->topic_count;i++) release(h->topics[i].title);
    for(k=0;k<h->keyword_count;k++) release(h->keywords[k]);
    release(h->topics); release(h->keywords); release(h->keyword_counts); release(h->keyword_data);
    release(h->fonts); release(h->phrase_text); release(h->stream); release(h->block_start); release(h->data);
    GlobalFree((HGLOBAL)h);
}
HelpFile *HelpOpen(LPCSTR path) {
    HelpFile *h=(HelpFile *)GlobalAlloc(GPTR,sizeof(HelpFile)); DWORD size;
    if(!h) return NULL;
    lstrcpyn(h->path,path,sizeof(h->path));
    if(!(h->data=(BYTE *)ReadWholeFile(path,&h->size)) || h->size<16 || get32(h->data)!=0x00035f3fUL ||
       !read_system(h) || !read_stream(h)) {HelpClose(h); return NULL;}
    read_phrases(h);
    read_fonts(h);
    if(!find_topics(h)) {HelpClose(h); return NULL;}
    if((h->tomap=subfile(h,"|TOMAP",&size))!=NULL) h->tomap_count=size/4;
    h->context=subfile(h,"|CONTEXT",&h->context_size);
    h->ctxomap=subfile(h,"|CTXOMAP",&h->ctxomap_size);
    read_keywords(h);
    return h;
}
LPCSTR HelpTitle(const HelpFile *h) {return h->title;}
LPCSTR HelpPath(const HelpFile *h) {return h->path;}
int HelpFontCount(const HelpFile *h) {return h->font_count;}
const HelpFont *HelpFontAt(const HelpFile *h,int i) {return i>=0 && i<h->font_count?&h->fonts[i]:NULL;}
DWORD HelpTopicCount(const HelpFile *h) {return h->topic_count;}
LPCSTR HelpTopicTitle(const HelpFile *h,DWORD n) {return n<h->topic_count && h->topics[n].title?h->topics[n].title:"";}
/* The contents: 3.0's first |TOMAP entry (the index), 3.1's |SYSTEM record, else the first topic. */
DWORD HelpContents(const HelpFile *h) {
    DWORD n=HELP_NO_TOPIC;
    if(h->minor<=16 && h->tomap_count) n=by_offset(h,get32(h->tomap));
    if(h->minor>16 && h->has_contents) n=by_offset(h,h->contents);
    return n==HELP_NO_TOPIC?0:n;
}
DWORD HelpByTarget(const HelpFile *h,DWORD target) {return by_target(h,target);}
DWORD HelpMapped(const HelpFile *h,DWORD map) {
    DWORD n=h->ctxomap_size>=2?get16(h->ctxomap):0,i;
    for(i=0;i<n && 2+8*(i+1)<=h->ctxomap_size;i++) if(get32(h->ctxomap+2+8*i)==map) return by_offset(h,get32(h->ctxomap+6+8*i));
    return HELP_NO_TOPIC;
}
/* 3.1's hash of a context name; 3.0 has only numbers. */
DWORD HelpNamed(const HelpFile *h,LPCSTR name) {
    static const signed char table[256]={
        0,-47,-46,-45,-44,-43,-42,-41,-40,-39,-38,-37,-36,-35,-34,-33,-32,-31,-30,-29,-28,-27,-26,-25,-24,-23,-22,-21,-20,-19,-18,-17,
        -16,11,-14,-13,-12,-11,-10,-9,-8,-7,-6,-5,-4,-3,12,-1,10,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,11,12,13,14,13,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
        80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
        112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,-128,-127,-126,-125,11,-123,-122,-121,-120,-119,-118,-117,-116,-115,-114,-113,
        -112,-111,-110,-109,-108,-107,-106,-105,-104,-103,-102,-101,-100,-99,-98,-97,-96,-95,-94,-93,-92,-91,-90,-89,-88,-87,-86,-85,-84,-83,-82,-81,
        -80,-79,-78,-77,-76,-75,-74,-73,-72,-71,-70,-69,-68,-67,-66,-65,-64,-63,-62,-61,-60,-59,-58,-57,-56,-55,-54,-53,-52,-51,-50,-49};
    DWORD hash=0;
    if(h->minor<=16) return HELP_NO_TOPIC;
    for(;*name;name++) hash=hash*43+(DWORD)(long)table[(BYTE)*name];
    return by_target(h,*name || hash?hash:1);
}
int HelpKeywordCount(const HelpFile *h) {return h->keyword_count;}
LPCSTR HelpKeyword(const HelpFile *h,int i) {return i>=0 && i<h->keyword_count?h->keywords[i]:"";}
int HelpKeywordTopics(const HelpFile *h,int k,DWORD *topics,int max) {
    int i,n=0; DWORD at;
    if(k<0 || k>=h->keyword_count) return 0;
    for(i=0,at=h->keyword_data[k];i<h->keyword_counts[k] && n<max && at+4<=h->kwdata_size;i++,at+=4) {
        DWORD t=by_offset(h,get32(h->kwdata+at)),j;
        for(j=0;j<(DWORD)n && topics[j]!=t;j++) {}
        if(t!=HELP_NO_TOPIC && j==(DWORD)n) topics[n++]=t;
    }
    return n;
}
int HelpFindKeyword(const HelpFile *h,LPCSTR prefix) {
    int i,n=lstrlen(prefix);
    for(i=0;i<h->keyword_count;i++) {
        char start[256];
        lstrcpyn(start,h->keywords[i],min(n+1,(int)sizeof(start)));
        if(!lstrcmpi(start,prefix)) return i;
    }
    return -1;
}

/* --- pictures ------------------------------------------------------------------------------ */
/* A picture's packed data: stored, run-length (1), LZ77 (2) or both (3). */
static BYTE *unpack(const BYTE *in,DWORD n,DWORD size,BYTE packing) {
    BYTE *out=(BYTE *)GlobalAlloc(GPTR,size+4),*middle;
    if(!out) return NULL;
    switch(packing) {
    case 0: memcpy(out,in,min(n,size)); break;
    case 1: unrun(in,n,out,size); break;
    case 2: lz77(in,n,out,size); break;
    case 3:
        if((middle=(BYTE *)GlobalAlloc(GPTR,size*2+0x10000))!=NULL) {
            DWORD m=lz77(in,n,middle,size*2+0x10000); unrun(middle,m,out,size); GlobalFree((HGLOBAL)middle);
        }
        break;
    }
    return out;
}
/* The first of an SHG or MRB's pictures: a DIB (with its colours), a DDB
 * (rows top-down, a word each; the colours VGA's) or a metafile. */
static BOOL read_picture(const BYTE *base,DWORD size,HelpPicture *out) {
    const BYTE *p; Cursor c; BYTE type,packing; DWORD at;
    if(size<8) return FALSE;
    at=get32(base+4);
    if(at+2>size) return FALSE;
    p=base+at; type=p[0]; packing=p[1]; c.p=p+2; c.end=base+size;
    memset(out,0,sizeof(*out));
    if(type==5 || type==6) {
        DWORD width,height,colors,packed,image,table,i,row; WORD planes,bpp; BITMAPINFOHEADER *bh; BYTE *bits,*dib;
        c_culong(&c); c_culong(&c); planes=c_cushort(&c); bpp=c_cushort(&c);
        width=c_culong(&c); height=c_culong(&c); colors=c_culong(&c); c_culong(&c);
        packed=c_culong(&c); c_culong(&c); at=c_dword(&c); c_dword(&c);
        if(!width || !height || width>4096 || height>4096 || planes!=1 || (bpp!=1 && bpp!=4 && bpp!=8 && bpp!=24) || at>size-(DWORD)(p-base)) return FALSE;
        if(type==5) colors=bpp==1?2:bpp<=8?1u<<bpp:0;
        else if(!colors && bpp<=8) colors=1u<<bpp;
        row=((width*bpp+31)/32)*4; image=row*height; table=sizeof(BITMAPINFOHEADER)+colors*4;
        if(!(dib=(BYTE *)GlobalAlloc(GPTR,table+image))) return FALSE;
        bh=(BITMAPINFOHEADER *)dib;
        bh->biSize=sizeof(*bh); bh->biWidth=(LONG)width; bh->biHeight=(LONG)height; bh->biPlanes=1; bh->biBitCount=bpp; bh->biClrUsed=colors;
        if(type==6) {
            for(i=0;i<colors && c.p+4<=c.end;i++,c.p+=4) {dib[sizeof(*bh)+i*4]=c.p[0]; dib[sizeof(*bh)+i*4+1]=c.p[1]; dib[sizeof(*bh)+i*4+2]=c.p[2];}
            if(!(bits=unpack(p+at,min(packed,size-(DWORD)(p-base)-at),image,packing))) {GlobalFree((HGLOBAL)dib); return FALSE;}
            memcpy(dib+table,bits,image);
        } else {
            DWORD stored=((width*bpp+15)/16)*2;
            static const BYTE vga[16][3]={{0,0,0},{0,0,128},{0,128,0},{0,128,128},{128,0,0},{128,0,128},{128,128,0},{192,192,192},
                {128,128,128},{0,0,255},{0,255,0},{0,255,255},{255,0,0},{255,0,255},{255,255,0},{255,255,255}};
            if(colors==2) memset(dib+sizeof(*bh)+4,0xff,3);
            else for(i=0;i<colors && i<16;i++) memcpy(dib+sizeof(*bh)+i*4,vga[i],3);
            if(!(bits=unpack(p+at,min(packed,size-(DWORD)(p-base)-at),stored*height,packing))) {GlobalFree((HGLOBAL)dib); return FALSE;}
            for(i=0;i<height;i++) memcpy(dib+table+(height-1-i)*row,bits+i*stored,min(stored,row));
        }
        GlobalFree((HGLOBAL)bits);
        out->data=dib; out->size=table+image; out->width=(int)width; out->height=(int)height;
        return TRUE;
    }
    if(type==8) {
        DWORD expanded,packed;
        out->metafile=TRUE; out->mm=c_cushort(&c); out->width=(short)c_word(&c); out->height=(short)c_word(&c);
        expanded=c_culong(&c); packed=c_culong(&c); c_culong(&c); at=c_dword(&c); c_dword(&c);
        if(!expanded || expanded>0x1000000 || at>size-(DWORD)(p-base)) return FALSE;
        if(!(out->data=unpack(p+at,min(packed,size-(DWORD)(p-base)-at),expanded,packing))) return FALSE;
        out->size=expanded;
        return TRUE;
    }
    return FALSE;
}

/* --- reading a topic ----------------------------------------------------------------------- */
typedef struct {
    HelpTopic *t; int para_cap,run_cap,spot_cap,picture_cap; DWORD text_cap;
    WORD font; short spot; BOOL failed;
} Builder;
static BOOL room(Builder *b,void **array,int *cap,int count,DWORD element) {
    if(count<*cap) return TRUE;
    {
        int n=*cap?*cap*2:16; void *a=grow(*array,(DWORD)count*element,(DWORD)n*element);
        if(!a) {b->failed=TRUE; return FALSE;}
        *array=a; *cap=n;
    }
    return TRUE;
}
static HelpRun *new_run(Builder *b,BYTE kind) {
    HelpTopic *t=b->t; HelpRun *r;
    if(!t->para_count || !room(b,(void **)&t->runs,&b->run_cap,t->run_count,sizeof(HelpRun))) return NULL;
    r=&t->runs[t->run_count++]; memset(r,0,sizeof(*r));
    r->kind=kind; r->font=b->font; r->spot=b->spot;
    t->paras[t->para_count-1].runs++;
    return r;
}
static void add_text(Builder *b,const char *s,DWORD n) {
    HelpTopic *t=b->t; HelpRun *last=t->run_count && t->para_count && t->paras[t->para_count-1].runs?&t->runs[t->run_count-1]:NULL;
    if(!n) return;
    if(t->text_length+n+1>b->text_cap) {
        DWORD cap=max(b->text_cap*2,t->text_length+n+256); char *x=(char *)grow(t->text,t->text_length,cap);
        if(!x) {b->failed=TRUE; return;}
        t->text=x; b->text_cap=cap;
    }
    if(last && last->kind==RUN_TEXT && last->font==b->font && last->spot==b->spot && last->at+last->length==t->text_length) last->length+=n;
    else {HelpRun *r=new_run(b,RUN_TEXT); if(!r) return; r->at=t->text_length; r->length=n;}
    memcpy(t->text+t->text_length,s,n); t->text_length+=n;
}
static HelpPara *new_para(Builder *b,const HelpPara *like) {
    HelpTopic *t=b->t; HelpPara *p;
    if(!room(b,(void **)&t->paras,&b->para_cap,t->para_count,sizeof(HelpPara))) return NULL;
    p=&t->paras[t->para_count++];
    if(like) *p=*like; else memset(p,0,sizeof(*p));
    p->first_run=t->run_count; p->runs=0;
    return p;
}
static void add_picture(Builder *b,HelpFile *h,const BYTE *base,DWORD size,int number,BYTE place) {
    HelpTopic *t=b->t; HelpPicture pic; HelpRun *r;
    if(!base) {
        char name[16]; DWORD n;
        wsprintf(name,"|bm%d",number);
        if(!(base=subfile(h,name,&n))) return;
        size=n;
    }
    if(!read_picture(base,size,&pic)) return;
    if(!room(b,(void **)&t->pictures,&b->picture_cap,t->picture_count,sizeof(HelpPicture))) {GlobalFree((HGLOBAL)pic.data); return;}
    t->pictures[t->picture_count]=pic;
    if((r=new_run(b,RUN_PICTURE))!=NULL) {r->at=(DWORD)t->picture_count; r->place=place;}
    t->picture_count++;
}
static void add_spot(Builder *b,BYTE kind,BOOL visible,DWORD topic,LPCSTR file,LPCSTR macro,int macro_length) {
    HelpTopic *t=b->t; HelpSpot *s;
    if(!room(b,(void **)&t->spots,&b->spot_cap,t->spot_count,sizeof(HelpSpot))) return;
    s=&t->spots[t->spot_count]; memset(s,0,sizeof(*s));
    s->kind=kind; s->visible=visible; s->topic=topic;
    if(file) lstrcpyn(s->file,file,sizeof(s->file));
    if(macro) lstrcpyn(s->macro,macro,min(macro_length+1,(int)sizeof(s->macro)));
    b->spot=(short)t->spot_count++;
}
/* A display record: paragraph information, then the text's strings and the
 * formatting commands between them, to 0xFF (each cell's, in a table). */
static void read_record(Builder *b,HelpFile *h,const BYTE *link) {
    BYTE type=link[20]; DWORD length; char *text=link_text(h,link,&length),*s=text,*s_end; Cursor c;
    int columns=1,i; short gap[32],width[32]; BOOL table=type==0x23,relative=FALSE,done=FALSE;
    if(!text) {b->failed=TRUE; return;}
    s_end=text+length;
    c.p=link+21; c.end=link+min(get32(link+16),get32(link));
    if(c.end>h->stream+h->stream_size) c.end=h->stream+h->stream_size;
    c_clong(&c);
    if(type!=1) c_cushort(&c);
    if(table) {
        BYTE kind; columns=c_byte(&c); kind=c_byte(&c);
        if(kind==0 || kind==2) {c_word(&c); relative=TRUE;}
        for(i=0;i<columns;i++) {short g=(short)c_word(&c),w=(short)c_word(&c); if(i<32) {gap[i]=g; width[i]=w;}}
        if(columns>32) columns=32;
    }
    while(!done && c.p<c.end && !b->failed) {
        HelpPara info,*p; WORD bits; short column=-1;
        memset(&info,0,sizeof(info)); info.column=-1;
        if(table) {
            column=(short)c_word(&c);
            if(column==-1) break;
            c_word(&c); c_byte(&c);
            if(column>=0 && column<columns) {
                int x=0,k;
                for(k=0;k<column;k++) x+=gap[k]+width[k];
                info.column=column; info.new_cell=1; info.relative=(BYTE)relative;
                info.cell_left=(short)(x+gap[column]); info.cell_width=width[column];
            }
        }
        c.p+=4;
        bits=c_word(&c);
        if(bits&0x0001) c_clong(&c);
        if(bits&0x0002) info.above=c_cshort(&c);
        if(bits&0x0004) info.below=c_cshort(&c);
        if(bits&0x0008) info.lines=c_cshort(&c);
        if(bits&0x0010) info.left=c_cshort(&c);
        if(bits&0x0020) info.right=c_cshort(&c);
        if(bits&0x0040) info.first=c_cshort(&c);
        if(bits&0x0100) {info.border=c_byte(&c); c_word(&c);}
        if(bits&0x0200) {
            int n=c_cshort(&c),k;
            for(k=0;k<n && c.p<c.end;k++) {
                WORD tab=c_cushort(&c),kind=(WORD)(tab&0x4000?c_cushort(&c):0);
                if(info.tabs<HELP_TABS) {info.tab[info.tabs]=(WORD)(tab&0x3fff); info.tab_kind[info.tabs]=(BYTE)(kind==1?1:kind==2?2:0); info.tabs++;}
            }
        }
        info.align=(BYTE)(bits&0x0800?HELP_CENTER:bits&0x0400?HELP_RIGHT:HELP_LEFT);
        if(!(p=new_para(b,&info))) break;
        /* The strings and commands. */
        for(;;) {
            const BYTE *command;
            if(s<s_end) {DWORD n=0; while(s+n<s_end && s[n]) n++; add_text(b,s,n); s+=n+1;}
            if(c.p>=c.end) {done=TRUE; break;}
            command=c.p++;
            switch(*command) {
            case 0xff: if(!table) done=TRUE; goto next_paragraph;
            case 0x20: c.p+=4; break;
            case 0x21: c.p+=2; break;
            case 0x80: b->font=c_word(&c); break;
            case 0x81: new_run(b,RUN_BREAK); break;
            case 0x82:
                /* A new paragraph like this one, unless the cell or record ends here. */
                if(c.p<c.end && *c.p!=0xff) {info.new_cell=0; p=new_para(b,&info);}
                break;
            case 0x83: new_run(b,RUN_TAB); break;
            case 0x86: case 0x87: case 0x88: {
                BYTE kind=c_byte(&c); long size=c_clong(&c); const BYTE *start;
                if(kind==0x22) c_cushort(&c);
                start=c.p;
                if(size<0 || start+size>c.end) {done=TRUE; goto next_paragraph;}
                if((kind==3 || kind==0x22) && size>=4) {
                    WORD embedded=get16(start);
                    if(embedded==0) add_picture(b,h,NULL,0,get16(start+2),(BYTE)(*command-0x86));
                    else if(embedded==1) add_picture(b,h,start+2,(DWORD)size-2,0,(BYTE)(*command-0x86));
                }
                c.p=start+size;
                break;
            }
            case 0x89: b->spot=-1; break;
            case 0x8b: add_text(b," ",1); break;
            case 0x8c: break;
            case 0xc8: case 0xcc: {
                WORD n=c_word(&c);
                add_spot(b,SPOT_MACRO,*command==0xc8,HELP_NO_TOPIC,NULL,(LPCSTR)c.p,n);
                c.p+=n;
                break;
            }
            case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe6: case 0xe7:
                add_spot(b,(BYTE)(*command&1?SPOT_JUMP:SPOT_POPUP),!(*command&4),by_target(h,c_dword(&c)),NULL,NULL,0);
                break;
            case 0xea: case 0xeb: case 0xee: case 0xef: {
                WORD n=c_word(&c); const BYTE *start=c.p; BYTE kind=c_byte(&c); DWORD target=c_dword(&c); LPCSTR file=NULL;
                if(kind==1) c_byte(&c);
                if(kind==6) {while(c.p<c.end && *c.p) c.p++; c.p++;}
                if(kind==4 || kind==6) file=(LPCSTR)c.p;
                add_spot(b,(BYTE)(*command&1?SPOT_JUMP:SPOT_POPUP),!(*command&4),file?target:by_target(h,target),file,NULL,0);
                c.p=start+n;
                break;
            }
            default: done=TRUE; goto next_paragraph;
            }
        }
    next_paragraph: ;
    }
    GlobalFree((HGLOBAL)text);
}
HelpTopic *HelpReadTopic(HelpFile *h,DWORD number) {
    Builder b; DWORD pos,at,walked=0; const BYTE *link; BOOL first=TRUE;
    if(number>=h->topic_count) return NULL;
    memset(&b,0,sizeof(b)); b.spot=-1;
    if(!(b.t=(HelpTopic *)GlobalAlloc(GPTR,sizeof(HelpTopic)))) return NULL;
    lstrcpyn(b.t->title,HelpTopicTitle(h,number),sizeof(b.t->title));
    b.t->number=number;
    link=h->stream+h->topics[number].stream;
    b.t->previous=h->minor<=16?by_target(h,h->topics[number].previous):by_offset(h,h->topics[number].previous);
    b.t->next=h->minor<=16?by_target(h,h->topics[number].next):by_offset(h,h->topics[number].next);
    if(h->topics[number].previous==0xffffffffUL || (h->minor<=16 && (h->topics[number].previous&0xffff)==0xffff)) b.t->previous=HELP_NO_TOPIC;
    if(h->topics[number].next==0xffffffffUL || (h->minor<=16 && (h->topics[number].next&0xffff)==0xffff)) b.t->next=HELP_NO_TOPIC;
    /* The links after the header, to the next topic's. */
    for(at=h->topics[number].stream;walked++<0x10000 && !b.failed;) {
        DWORD next;
        link=h->stream+at;
        if(!first && link[20]==2) break;
        if(link[20]==1 || link[20]==0x20 || link[20]==0x23) read_record(&b,h,link);
        first=FALSE;
        next=get32(link+12);
        if(h->minor<=16) {
            /* From the stream back to the position, then on by the distance. */
            DWORD block=0;
            while(block+1<h->blocks && h->block_start[block+1]<=at) block++;
            pos=block*h->block_size+12+(at-h->block_start[block]);
            if(!next || !stream_at(h,pos+next,&at)) break;
        } else if(next==0xffffffffUL || !stream_at(h,next,&at)) break;
    }
    if(b.failed && !b.t->para_count) {HelpFreeTopic(b.t); return NULL;}
    return b.t;
}
void HelpFreeTopic(HelpTopic *t) {
    int i;
    if(!t) return;
    for(i=0;i<t->picture_count;i++) release(t->pictures[i].data);
    release(t->paras); release(t->runs); release(t->text); release(t->spots); release(t->pictures);
    GlobalFree((HGLOBAL)t);
}
