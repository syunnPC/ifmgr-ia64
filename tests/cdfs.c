/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host tests for the ISO 9660 reader against a genisoimage-built image
 * (tools/cdfs_fixture.py), including damaged copies.
 *   cdfs-test image.iso
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iso9660.h"
static unsigned checks;
#define CHECK(x) do {checks++; if(!(x)) {fprintf(stderr,"%s:%d: CHECK(%s)\n",__FILE__,__LINE__,#x); exit(1);}} while(0)
void con_write(const void *p,size_t n) {fwrite(p,1,n,stdout);}
static u8 *image; static size_t image_size; static unsigned reads;
static int read_image(void *ctx,u32 sector,u32 count,void *buffer) {
    (void)ctx; reads++;
    if((size_t)(sector+count)*ISO_SECTOR>image_size) return DE_SEEK;
    memcpy(buffer,image+(size_t)sector*ISO_SECTOR,(size_t)count*ISO_SECTOR); return 0;
}
static IsoVolume v;
static void test_image(void) {
    IsoEntry e; u8 buf[6000]; u32 n;
    CHECK(!iso_mount(&v,read_image,NULL) && !strcmp(v.label,"WIN30"));
    CHECK(!iso_lookup(&v,"\\",&e) && (e.attributes&0x10));
    CHECK(!iso_lookup(&v,"\\README.TXT",&e) && e.size==18 && e.attributes==0x01);
    CHECK(!iso_read(&v,&e,0,buf,100,&n) && n==18 && !memcmp(buf,"WINDOWS SETUP CD\r\n",18));
    CHECK(!iso_read(&v,&e,10,buf,100,&n) && n==8 && !iso_read(&v,&e,18,buf,10,&n) && !n);
    CHECK(!iso_lookup(&v,"\\WIN\\SETUP.EXE",&e) && e.size==5000);
    CHECK(!iso_read(&v,&e,0,buf,5000,&n) && n==5000);
    for(unsigned i=0;i<5000;i++) CHECK(buf[i]==(u8)(i*7+3));
    CHECK(!iso_read(&v,&e,2040,buf,20,&n) && n==20 && buf[0]==(u8)(2040*7+3) && buf[19]==(u8)(2059*7+3));
    CHECK(!iso_read(&v,&e,4096,buf,5000,&n) && n==904 && buf[0]==(u8)(4096*7+3));
    CHECK(!iso_lookup(&v,"\\WIN\\SUB\\DEEP\\FILE.TXT",&e) && e.size==9);
    CHECK(!iso_lookup(&v,"\\LOWER.TXT",&e));
    CHECK(iso_lookup(&v,"\\NONE.TXT",&e)==DE_NOFILE && iso_lookup(&v,"\\NONE\\X.TXT",&e)==DE_PATH);
    CHECK(iso_lookup(&v,"\\README.TXT\\X",&e)==DE_PATH);
    /* A directory spanning several sectors, and an empty one. */
    CHECK(!iso_lookup(&v,"\\MANY",&e) && e.size>ISO_SECTOR);
    u32 offset=0,count=0; IsoEntry f; int err;
    while(!(err=iso_next(&v,e.sector,e.size,&offset,&f))) {
        char want[13]; snprintf(want,sizeof(want),"F%03u.TXT",count);
        CHECK(!strcmp(f.name,want)); count++;
    }
    CHECK(err==DE_NOMORE && count==120);
    CHECK(!iso_lookup(&v,"\\EMPTY",&e) && (offset=0,iso_next(&v,e.sector,e.size,&offset,&f)==DE_NOMORE));
    offset=0; count=0;
    CHECK(!iso_lookup(&v,"\\",&e));
    while(!iso_next(&v,e.sector,e.size,&offset,&f)) count++;
    CHECK(count==5); /* EMPTY LOWER.TXT MANY README.TXT WIN */
}
int main(int argc,char **argv) {
    if(argc!=2) {fprintf(stderr,"usage: %s image.iso\n",argv[0]); return 2;}
    FILE *fp=fopen(argv[1],"rb"); if(!fp) {perror(argv[1]); return 1;}
    fseek(fp,0,SEEK_END); image_size=(size_t)ftell(fp); fseek(fp,0,SEEK_SET);
    u8 *original=malloc(image_size); CHECK(fread(original,1,image_size,fp)==image_size); fclose(fp);
    image=malloc(image_size); memcpy(image,original,image_size);
    test_image();
    /* Damaged descriptors and records fail cleanly. */
    IsoEntry e;
    image[16*ISO_SECTOR+1]='X'; CHECK(iso_mount(&v,read_image,NULL)==DE_FORMAT);
    memcpy(image,original,image_size); image[16*ISO_SECTOR+129]=4; CHECK(iso_mount(&v,read_image,NULL)==DE_FORMAT);
    memcpy(image,original,image_size); CHECK(!iso_mount(&v,read_image,NULL));
    image[(size_t)v.root_sector*ISO_SECTOR+68]=20; CHECK(iso_lookup(&v,"\\WIN\\SETUP.EXE",&e)==DE_FORMAT);
    memcpy(image,original,image_size); size_t full=image_size; image_size=17*ISO_SECTOR;
    CHECK(!iso_mount(&v,read_image,NULL) && iso_lookup(&v,"\\WIN",&e)==DE_SEEK);
    image_size=full;
    /* Random damage: clean failure or valid results. */
    srand(1);
    for(unsigned round=0;round<2000;round++) {
        memcpy(image,original,image_size);
        for(unsigned k=0;k<1+(unsigned)rand()%6;k++) {
            size_t at=(size_t)(16+rand()%24)*ISO_SECTOR+(size_t)(rand()%ISO_SECTOR);
            if(at<image_size) image[at]^=(u8)(1u<<(rand()%8));
        }
        if(iso_mount(&v,read_image,NULL)) continue;
        u8 buf[256]; u32 n;
        if(!iso_lookup(&v,"\\WIN\\SETUP.EXE",&e)) iso_read(&v,&e,(u32)(rand()%6000),buf,sizeof(buf),&n);
        if(!iso_lookup(&v,"\\MANY",&e)) {u32 off=0; IsoEntry f; unsigned guard=0; while(!iso_next(&v,e.sector,e.size,&off,&f) && guard++<10000) {}}
    }
    free(image); free(original);
    printf("PASS ISO 9660: %u assertions, %u sector reads (genisoimage image, nested and multi-sector directories, boundaries, damage)\n",checks,reads);
    return 0;
}
