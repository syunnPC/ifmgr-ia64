/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounds-checked reader for the DOS 4 COUNTRY.SYS data format.
 */
#include "nls.h"
static int contains(const NlsDatabase *db,u32 at,u32 count) {
    return at<=db->size && count<=db->size-at;
}
static int record(const NlsDatabase *db,u32 at,u32 minimum,u32 *next) {
    if(!contains(db,at,2)) return DE_FORMAT;
    u32 size=rd16(db->data+at);
    if(size<minimum || !contains(db,at+2,size)) return DE_FORMAT;
    *next=at+2+size; return 0;
}
static int read_table(const NlsDatabase *db,u32 at,unsigned kind,NlsTable *out) {
    static const char signatures[8][8]={"","CTYINFO","UCASE  ","","FUCASE ","FCHAR  ","COLLATE","DBCS   "};
    if(!contains(db,at,10) || db->data[at]!=255 ||
       (memcmp(db->data+at+1,signatures[kind],7) &&
        !(kind==4 && !memcmp(db->data+at+1,signatures[2],7)))) return DE_FORMAT;
    u32 size=rd16(db->data+at+8); if(!contains(db,at+10,size)) return DE_FORMAT;
    const u8 *data=db->data+at+10;
    if((kind==1 && size!=38) || ((kind==2 || kind==4) && size!=128) || (kind==6 && size!=256)) return DE_FORMAT;
    if(kind==1) {
        int currency_end=0; for(unsigned i=6;i<11;i++) if(!data[i]) currency_end=1;
        if(!currency_end || rd16(data+4)>2 || data[21]>1 || (data[19]&~7U) ||
           data[12] || data[14] || data[16] || data[18] || data[27]) return DE_FORMAT;
    }
    if(kind==5 && (size<8 || data[0]!=1 || data[1]>data[2] || data[3]!=0 || data[4]>data[5] ||
        data[6]!=2 || size!=8U+data[7])) return DE_FORMAT;
    if(kind==7) {
        if(size%2 || (size && (size<2 || data[size-2] || data[size-1]))) return DE_FORMAT;
        /* DOS 4's empty SBCS table declares zero bytes, followed by 00,00. */
        if(!size && (!contains(db,at+10,2) || data[0] || data[1])) return DE_FORMAT;
        unsigned last=0;
        for(unsigned i=0;i+2<size;i+=2) {
            if(data[i]<128 || data[i]<=last || data[i]>data[i+1]) return DE_FORMAT;
            last=data[i+1];
        }
    }
    *out=(NlsTable){data,size}; return 0;
}
static int country_at(const NlsDatabase *db,u32 at,NlsCountry *out,u32 *next) {
    int e=record(db,at,12,next); if(e) return e;
    NlsCountry country={.country=rd16(db->data+at+2),.code_page=rd16(db->data+at+4)};
    u32 tables=rd32(db->data+at+10);
    if(!contains(db,tables,2)) return DE_FORMAT;
    u32 count=rd16(db->data+tables),position=tables+2; unsigned seen=0;
    if(count>(db->size-position)/8) return DE_FORMAT;
    for(u32 i=0;i<count;i++) {
        u32 after; e=record(db,position,6,&after); if(e) return e;
        unsigned kind=db->data[position+2];
        NlsTable *table;
        switch(kind) {
        case 1: table=&country.info; break;
        case 2: table=&country.upper; break;
        case 4: table=&country.file_upper; break;
        case 5: table=&country.file_chars; break;
        case 6: table=&country.collate; break;
        case 7: table=&country.dbcs; break;
        default: return DE_FORMAT;
        }
        if(seen&(1U<<kind)) return DE_FORMAT;
        seen|=1U<<kind;
        e=read_table(db,rd32(db->data+position+4),kind,table); if(e) return e;
        position=after;
    }
    if(seen!=0xf6) return DE_FORMAT;
    *out=country; return 0;
}
int nls_database_open(NlsDatabase *out,const void *data,u32 size) {
    if(!out || !data) return DE_FUNCTION;
    NlsDatabase db={.data=data,.size=size};
    if(!contains(&db,0,18) || memcmp(data,"\xff" "COUNTRY",8)) return DE_FORMAT;
    u32 pointers=rd16(db.data+16),position=18; int found=0;
    if(pointers>(size-position)/5) return DE_FORMAT;
    for(u32 i=0;i<pointers;i++,position+=5) {
        if(db.data[position]==1) {
            if(found) return DE_FORMAT;
            found=1; db.directory=rd32(db.data+position+1);
        }
    }
    if(!found || !contains(&db,db.directory,2)) return DE_FORMAT;
    db.count=rd16(db.data+db.directory); position=db.directory+2;
    if(!db.count || db.count>(size-position)/14) return DE_FORMAT;
    for(u32 i=0;i<db.count;i++) {
        NlsCountry country; u32 after;
        int e=country_at(&db,position,&country,&after); if(e) return e;
        u32 earlier=db.directory+2;
        for(u32 j=0;j<i;j++) {
            if(rd16(db.data+earlier+2)==country.country && rd16(db.data+earlier+4)==country.code_page) return DE_FORMAT;
            earlier+=2+rd16(db.data+earlier);
        }
        position=after;
    }
    *out=db; return 0;
}
int nls_database_country(const NlsDatabase *db,u16 country,u16 page,NlsCountry *out) {
    if(!db || !out || !db->data) return DE_FUNCTION;
    u32 position=db->directory+2;
    for(u32 i=0;i<db->count;i++) {
        NlsCountry result; u32 after;
        int e=country_at(db,position,&result,&after); if(e) return e;
        if(result.country==country && (!page || result.code_page==page)) {*out=result; return 0;}
        position=after;
    }
    return DE_NOFILE;
}
int nls_country_lead(const NlsCountry *country,u8 byte) {
    const NlsTable *t=&country->dbcs;
    for(u32 i=0;i+2<t->size;i+=2) if(byte>=t->data[i] && byte<=t->data[i+1]) return 1;
    return 0;
}
u8 nls_country_upper(const NlsCountry *country,u8 byte,int filename) {
    if(byte<128) return byte>='a' && byte<='z'?byte-32:byte;
    return (filename?country->file_upper.data:country->upper.data)[byte-128];
}
int nls_country_case(const NlsCountry *country,u8 *data,u32 count,int filename) {
    if(!country || (count && !data)) return DE_FUNCTION;
    /* Validate boundaries before changing any byte. A DBCS trail byte can be
     * an ASCII letter or separator and must never be case-mapped on its own. */
    for(u32 i=0;i<count;i++) if(nls_country_lead(country,data[i])) {
        if(i==count-1 || !data[i+1]) return DE_FUNCTION;
        i++;
    }
    for(u32 i=0;i<count;i++) {
        if(nls_country_lead(country,data[i])) i++;
        else data[i]=nls_country_upper(country,data[i],filename);
    }
    return 0;
}
int nls_country_file_char(const NlsCountry *country,u8 byte) {
    const u8 *p=country->file_chars.data;
    if(byte<p[1] || byte>p[2] || (byte>=p[4] && byte<=p[5])) return 0;
    for(unsigned i=0;i<p[7];i++) if(byte==p[8+i]) return 0;
    return 1;
}
