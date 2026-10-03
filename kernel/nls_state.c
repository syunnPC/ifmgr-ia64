/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dos.h"
#include "nls.h"
#include "codepage.h"
#include "../build/country_data.h"
#define COUNTRY_LIMIT 65536U
static u8 country_data[2][COUNTRY_LIMIT],lead_ranges[512];
static NlsDatabase database;
static NlsCountry active;
static unsigned bank,lead_size;
static u16 default_page;
static void select_country(const NlsCountry *country) {
    active=*country;
    lead_size=active.dbcs.size?active.dbcs.size:2;
    memset(lead_ranges,0,sizeof(lead_ranges));
    memcpy(lead_ranges,active.dbcs.data,lead_size);
}
int nls_reset(void) {
    NlsDatabase db; NlsCountry country;
    int e=nls_database_open(&db,builtin_country,sizeof(builtin_country));
    if(!e) e=nls_database_country(&db,1,437,&country);
    if(e) return e;
    database=db; bank=0; default_page=437; select_country(&country); return 0;
}
static void initialize(void) {if(!active.info.data) nls_reset();}
int nls_lead(u8 c) {initialize(); return nls_country_lead(&active,c);}
u8 nls_upper(u8 c,int filename) {initialize(); return nls_country_upper(&active,c,filename);}
int nls_file_char(u8 c) {initialize(); return nls_country_file_char(&active,c);}
unsigned nls_char_size(const char *s) {return !*s?0:nls_lead((u8)*s)?(s[1]?2:0):1;}
char *nls_last_sep(char *s,const char *separators) {
    char *last=NULL;
    while(*s) {
        unsigned n=nls_char_size(s); if(!n) return NULL;
        if(n==1 && strchr(separators,*s)) last=s;
        s+=n;
    }
    return last;
}
/* Stage, validate and close before publishing. Failed I/O, malformed data and
 * unsupported pairs leave both the current database and default page intact. */
int nls_load(u16 country,u16 page,const char *path) {
    if(!country || country==DOS_NLS_CURRENT || page==DOS_NLS_CURRENT || !path) return DE_FUNCTION;
    unsigned h; int e=dos_open(path,DOS_OPEN_READ|DOS_SHARE_DENY_WRITE,0,&h); if(e) return e;
    u8 *data=country_data[bank^1]; u32 used=0;
    while(!e && used<COUNTRY_LIMIT) {
        u32 got=0; e=dos_read(h,data+used,COUNTRY_LIMIT-used,&got);
        used+=got; if(!got) break;
    }
    if(!e && used==COUNTRY_LIMIT) {
        u8 extra; u32 got; e=dos_read(h,&extra,1,&got); if(!e && got) e=DE_DATA;
    }
    NlsDatabase db; NlsCountry next;
    if(!e) {e=nls_database_open(&db,data,used); if(e==DE_FORMAT) e=DE_DATA;}
    if(!e) e=nls_database_country(&db,country,page,&next);
    if(e) dos_preserve_error(e);
    int closed=dos_close(h); if(!e) e=closed;
    if(e) return e;
    database=db; bank^=1; default_page=next.code_page; select_country(&next); return 0;
}
static int lookup(u64 country,u64 page,NlsCountry *out) {
    if(country==UINT64_MAX) country=DOS_NLS_CURRENT;
    if(page==UINT64_MAX) page=DOS_NLS_CURRENT;
    if(country>65535 || page>65535) return DE_FUNCTION;
    if(country==DOS_NLS_CURRENT) country=active.country;
    if(page==DOS_NLS_CURRENT) page=active.code_page;
    if(!country || !page) return DE_NOFILE;
    return nls_database_country(&database,country,page,out);
}
static int copy_info(const NlsCountry *country,DosRegs *r,u64 address) {
    if(!address || r->cx<sizeof(DosCountryInfo)) {r->cx=sizeof(DosCountryInfo); return DE_FUNCTION;}
    const u8 *p=country->info.data;
    DosCountryInfo info={.size=sizeof(info),.country=country->country,.code_page=country->code_page,
        .date_order=rd16(p+4),.currency_flags=p[19],.currency_digits=p[20],.time_format=p[21]};
    memcpy(info.currency,p+6,5); memcpy(info.thousands,p+11,2); memcpy(info.decimal,p+13,2);
    memcpy(info.date_separator,p+15,2); memcpy(info.time_separator,p+17,2);
    memcpy(info.list_separator,p+26,2);
    memcpy((void *)(uintptr_t)address,&info,sizeof(info)); r->cx=sizeof(info); return 0;
}
static int case_call(DosRegs *r,unsigned sub) {
    int filename=(sub&0x80)!=0; sub&=0x7f;
    if(sub==0x20) {
        u8 c=r->dx; if(nls_lead(c)) return DE_FUNCTION;
        r->dx=(r->dx&~255ULL)|nls_country_upper(&active,c,filename); return 0;
    }
    if(sub!=0x21 && sub!=0x22) return DE_FUNCTION;
    if(r->cx>UINT32_MAX || (!r->dx && r->cx)) return DE_FUNCTION;
    u8 *data=(u8 *)(uintptr_t)r->dx; u32 count=r->cx;
    if(sub==0x22) {
        u32 length=0; while(length<count && data[length]) length++;
        if(length==count) return DE_FUNCTION;
        count=length;
    }
    return nls_country_case(&active,data,count,filename);
}
int nls_dispatch(DosRegs *r) {
    initialize();
    unsigned fn=(r->ax>>8)&255,sub=r->ax&255;
    NlsCountry country; int e;
    switch(fn) {
    case 0x38: {
        u64 id=!sub?active.country:sub==255?r->bx:sub;
        e=lookup(id,active.code_page,&country); if(e) return e;
        if(r->dx==65535 || r->dx==UINT64_MAX) select_country(&country);
        else {e=copy_info(&country,r,r->dx); if(e) return e;}
        r->bx=country.country; return 0;
    }
    case 0x63:
        if(sub) return DE_FUNCTION;
        r->si=(uintptr_t)lead_ranges; r->cx=lead_size; r->ax=0; return 0;
    case 0x65: {
        if(sub>=0x20) return case_call(r,sub);
        if(sub!=1 && sub!=2 && sub!=4 && sub!=5 && sub!=6 && sub!=7) return DE_FUNCTION;
        e=lookup(r->dx,r->bx,&country); if(e) return e;
        if(sub==1) return copy_info(&country,r,r->di);
        NlsTable table=sub==2?country.upper:sub==4?country.file_upper:sub==5?country.file_chars:
            sub==6?country.collate:country.dbcs;
        u32 size=table.size?table.size:2;
        if(!r->di || r->cx<size) {r->cx=size; return DE_FUNCTION;}
        memcpy((void *)(uintptr_t)r->di,table.data,size); r->cx=size; return 0;
    }
    case 0x66:
        if(sub==1) {r->bx=active.code_page; r->dx=default_page; return 0;}
        if(sub!=2 || !r->bx || r->bx>=65535) return DE_FUNCTION;
        e=lookup(active.country,r->bx,&country); if(e) return e;
        /* CON must accept the page first; NLS and CON then switch together. */
        e=codepage_check(country.code_page); if(e) return e;
        /* As NLSFUNC's, the country's data stays switched when a device
         * (KEYB through CON) refuses the page. */
        select_country(&country);
        return codepage_select(country.code_page)?DOS_CP_SYSTEM_NOT_PREPARED:0;
    default: return DE_FUNCTION;
    }
}
