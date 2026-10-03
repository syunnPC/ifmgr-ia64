/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "nls.h"
static unsigned checks;
void con_write(const void *data,size_t count) {(void)fwrite(data,1,count,stdout);}
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"FAIL NLS:%u: %s\n",__LINE__,#x); exit(1);}} while(0)
static void rejected(const u8 *data,u32 size) {
    NlsDatabase db={.size=99,.count=88},saved=db;
    CHECK(nls_database_open(&db,data,size)==DE_FORMAT);
    CHECK(!memcmp(&db,&saved,sizeof(db)));
}
int main(int argc,char **argv) {
    CHECK(argc==2); FILE *file=fopen(argv[1],"rb"); CHECK(file!=NULL);
    CHECK(!fseek(file,0,SEEK_END)); long length=ftell(file); CHECK(length>0 && length<65536); rewind(file);
    u8 *data=malloc(length),*bad=malloc(length); CHECK(data && bad);
    CHECK(fread(data,1,length,file)==(size_t)length && !fclose(file));
    NlsDatabase db; CHECK(!nls_database_open(&db,data,length) && db.count==46);
    NlsCountry us,german,japan,portugal;
    CHECK(!nls_database_country(&db,1,0,&us) && us.code_page==437);
    CHECK(rd16(us.info.data+4)==0 && us.info.data[6]=='$' && us.info.data[11]==',' && us.info.data[13]=='.');
    CHECK(us.upper.size==128 && us.collate.size==256 && !us.dbcs.size);
    CHECK(nls_country_upper(&us,'a',0)=='A' && nls_country_upper(&us,0x81,0)==0x9a);
    CHECK(nls_country_upper(&us,0x82,0)=='E' && nls_country_upper(&us,0xa0,1)=='A');
    CHECK(!nls_database_country(&db,49,437,&german) && rd16(german.info.data+4)==1);
    CHECK(german.info.data[6]=='D' && german.info.data[7]=='M' && german.info.data[13]==',');
    CHECK(!nls_database_country(&db,351,0,&portugal) && portugal.code_page==850);
    CHECK(!nls_database_country(&db,81,0,&japan) && japan.code_page==932 && rd16(japan.info.data+4)==2);
    CHECK(nls_country_lead(&japan,0x81) && nls_country_lead(&japan,0x9f) && !nls_country_lead(&japan,0xa0));
    CHECK(nls_country_lead(&japan,0xe0) && nls_country_lead(&japan,0xfc) && !nls_country_lead(&japan,0xfd));
    for(unsigned byte=0;byte<256;byte++) CHECK(!nls_country_lead(&us,byte));
    u8 name[]={'a',0x81,'a','z',0x81,'\\',0xe5,'b'};
    CHECK(!nls_country_case(&japan,name,sizeof(name),1));
    const u8 wanted[]={'A',0x81,'a','Z',0x81,'\\',0xe5,'b'}; CHECK(!memcmp(name,wanted,sizeof(name)));
    u8 partial[]={'a',0x81},zero[]={'a',0x81,0};
    CHECK(nls_country_case(&japan,partial,sizeof(partial),0)==DE_FUNCTION && partial[0]=='a');
    CHECK(nls_country_case(&japan,zero,sizeof(zero),0)==DE_FUNCTION && zero[0]=='a');
    CHECK(!nls_country_case(&us,NULL,0,0) && nls_country_case(&us,NULL,1,0)==DE_FUNCTION);
    CHECK(nls_country_file_char(&us,0x81) && nls_country_file_char(&us,'A') && nls_country_file_char(&us,'?'));
    CHECK(!nls_country_file_char(&us,'\\') && !nls_country_file_char(&us,'.') && !nls_country_file_char(&us,0) && !nls_country_file_char(&us,' '));
    NlsCountry old=japan;
    CHECK(nls_database_country(&db,81,850,&japan)==DE_NOFILE && !memcmp(&old,&japan,sizeof(old)));
    CHECK(nls_database_country(&db,999,0,&japan)==DE_NOFILE);
    /* All original country/code-page combinations expose complete tables. */
    u32 directory=rd32(data+19),position=directory+2,region_count=0; u16 regions[46];
    for(u32 i=0;i<db.count;i++) {
        u16 country=rd16(data+position+2),page=rd16(data+position+4); NlsCountry entry;
        CHECK(!nls_database_country(&db,country,page,&entry) && entry.country==country && entry.code_page==page);
        int found=0; for(unsigned j=0;j<region_count;j++) if(regions[j]==country) found=1;
        if(!found) regions[region_count++]=country;
        for(unsigned b=0;b<256;b++) {
            u8 text=b;
            if(nls_country_lead(&entry,text)) CHECK(nls_country_case(&entry,&text,1,0)==DE_FUNCTION && text==b);
            else CHECK(!nls_country_case(&entry,&text,1,0) && text==nls_country_upper(&entry,b,0));
        }
        position+=2+rd16(data+position);
    }
    CHECK(region_count==23);
    for(unsigned i=0;i<23;i++) rejected(data,i);
    memcpy(bad,data,length); bad[0]=0; rejected(bad,length);
    memcpy(bad,data,length); wr32(bad+19,UINT32_MAX); rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+directory,65535); rejected(bad,length);
    u32 entry=directory+2,tables=rd32(data+entry+10);
    memcpy(bad,data,length); wr16(bad+entry,11); rejected(bad,length);
    memcpy(bad,data,length); wr32(bad+entry+10,UINT32_MAX); rejected(bad,length);
    memcpy(bad,data,length); memcpy(bad+entry+14+2,bad+entry+2,4); rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+tables,65535); rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+tables+2,5); rejected(bad,length);
    memcpy(bad,data,length); bad[tables+4]=3; rejected(bad,length);
    memcpy(bad,data,length); wr32(bad+tables+6,UINT32_MAX); rejected(bad,length);
    memcpy(bad,data,length); bad[tables+12]=bad[tables+4]; rejected(bad,length);
    u32 info=(u32)(us.info.data-data),upper=(u32)(us.upper.data-data),chars=(u32)(us.file_chars.data-data),dbcs=(u32)(japan.dbcs.data-data);
    memcpy(bad,data,length); bad[info-10]=0; rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+info-2,37); rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+info+4,3); rejected(bad,length);
    memcpy(bad,data,length); bad[info+21]=2; rejected(bad,length);
    memcpy(bad,data,length); bad[info+19]=8; rejected(bad,length);
    memcpy(bad,data,length); memset(bad+info+6,'X',5); rejected(bad,length);
    memcpy(bad,data,length); bad[info+12]=1; rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+upper-2,127); rejected(bad,length);
    memcpy(bad,data,length); bad[chars+7]++; rejected(bad,length);
    memcpy(bad,data,length); wr16(bad+dbcs-2,5); rejected(bad,length);
    memcpy(bad,data,length); bad[dbcs+5]=1; rejected(bad,length);
    memcpy(bad,data,length); bad[dbcs+2]=0x90; rejected(bad,length);
    memcpy(bad,data,length); bad[dbcs]=0x7f; rejected(bad,length);
    memcpy(bad,data,length); bad[us.dbcs.data-data]=1; rejected(bad,length);
    free(bad); free(data);
    printf("PASS NLS data: %u assertions (46 profiles, country defaults, case, DBCS, bounds and malformed tables)\n",checks);
    return 0;
}
