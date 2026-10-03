/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * RECOVER: native counterpart of MS-DOS 4 CMD/RECOVER.
 *   RECOVER [d:][path]file  reads every cluster of the file; unreadable
 *       clusters are marked bad in the FAT and dropped from its chain, and
 *       the file keeps the bytes that could be read.
 *   RECOVER d:  rebuilds the root directory as FILEnnnn.REC entries, one per
 *       allocation chain (directories included), as DOS 4 did. This discards
 *       every name on the drive; the volume label is kept.
 */
#include "maint.h"
static Volume vol;
static int valid(u32 c) {return c>=2 && c<vol.clusters+2;}
/* Locate a path's directory entry from raw sectors (8.3 components). */
static int find_entry(const char *path,u32 *lba,unsigned *offset,u8 entry[32]) {
    u32 dir=0; const char *p=path; if(p[0] && p[1]==':') p+=2; while(*p=='\\') p++;
    u8 sector[512];
    while(*p) {
        u8 name[11]; memset(name,' ',11); unsigned n=0,end=8;
        while(*p && *p!='\\') {
            if(*p=='.') {n=8; end=11; p++; continue;}
            if(n<end) name[n++]=(u8)upper(*p);
            p++;
        }
        while(*p=='\\') p++;
        int found=0; u32 c=dir;
        for(u32 budget=vol.clusters+1;budget-- && !found;) {
            u32 count=dir?vol.spc:vol.root_sectors;
            for(u32 s=0;s<count && !found;s++) {
                u32 at=dir?cluster_lba(&vol,c)+s:vol.root_start+s;
                int e=volume_read(&vol,at,1,sector); if(e) return e;
                for(unsigned off=0;off<512;off+=32) {
                    u8 *x=sector+off; if(!x[0]) return DE_NOFILE;
                    if(x[0]==0xe5 || (x[11]&FA_VOLUME) || memcmp(x,name,11)) continue;
                    found=1; *lba=at; *offset=off; memcpy(entry,x,32); break;
                }
            }
            if(!dir || found) break;
            u32 next=fat_value(&vol,c); if(fat_end(&vol,next) || !valid(next)) break;
            c=next;
        }
        if(!found) return DE_NOFILE;
        if(*p) {if(!(entry[11]&FA_DIR)) return DE_PATH; dir=rd16(entry+26); if(!valid(dir)) return DE_PATH;}
    }
    return 0;
}
static int recover_file(const char *path) {
    u32 lba=0; unsigned offset=0; u8 entry[32];
    int e=find_entry(path,&lba,&offset,entry);
    if(e) {print("\nFile not found\n"); return e;}
    if(entry[11]&FA_DIR) {print("\nInvalid drive or file name\n"); return DE_ACCESS;}
    u32 size=rd32(entry+28),unit=vol.spc*512,kept=0,previous=0,first=0,remaining=size;
    u8 *cluster; e=dos_alloc(unit/16,(void **)&cluster); if(e) return e;
    for(u32 c=rd16(entry+26),budget=vol.clusters;valid(c) && budget--;) {
        u32 next=fat_value(&vol,c),done,take=MIN(remaining,unit);
        int bad=dos_disk_read(vol.drive,cluster_lba(&vol,c),vol.spc,cluster,&done)!=0;
        if(bad) fat_store(&vol,c,fat_bad(&vol));
        else {
            if(previous) fat_store(&vol,previous,c); else first=c;
            previous=c; kept+=take;
        }
        remaining-=take;
        if(fat_end(&vol,next) || !valid(next)) break;
        c=next;
    }
    if(previous) fat_store(&vol,previous,fat_eof(&vol));
    dos_free(cluster);
    wr16(entry+26,(u16)first); wr32(entry+28,kept);
    u8 sector[512]; e=volume_read(&vol,lba,1,sector);
    if(!e) {memcpy(sector+offset,entry,32); e=volume_write(&vol,lba,1,sector);}
    if(!e) e=volume_save_fat(&vol);
    if(e) {print("\nCan not write file allocation table\n"); return e;}
    print("\n%u of %u bytes recovered\n",(unsigned long long)kept,(unsigned long long)size);
    return 0;
}
static int recover_drive(void) {
    u8 *referenced; int e=dos_alloc((vol.clusters+2+15)/16,(void **)&referenced); if(e) return e;
    memset(referenced,0,vol.clusters+2);
    for(u32 c=2;c<vol.clusters+2;c++) {u32 v=fat_value(&vol,c); if(valid(v)) referenced[v]=1;}
    u8 label[32]={0}; u8 sector[512];
    for(u32 s=0;s<vol.root_sectors && !label[0];s++) {
        if(volume_read(&vol,vol.root_start+s,1,sector)) break;
        for(unsigned off=0;off<512;off+=32) if(sector[off] && sector[off]!=0xe5 && (sector[off+11]&FA_VOLUME) && sector[off+11]!=0x0f) {memcpy(label,sector+off,32); break;}
    }
    u32 slot=label[0]?1:0,made=0,full=0,unit=vol.spc*512; u16 date,time; fat_timestamp(&date,&time);
    u8 *root; e=dos_alloc(vol.root_sectors*32,(void **)&root); if(e) {dos_free(referenced); return e;}
    memset(root,0,vol.root_sectors*512); if(label[0]) memcpy(root,label,32);
    for(u32 c=2;c<vol.clusters+2;c++) {
        u32 v=fat_value(&vol,c);
        if(!v || v==fat_bad(&vol) || referenced[c]) continue;
        if(slot==vol.root_entries) {full=1; break;}
        u32 length=0;
        for(u32 x=c,budget=vol.clusters;valid(x) && budget--;) {
            length++; u32 next=fat_value(&vol,x);
            if(fat_end(&vol,next) || !valid(next)) {if(!fat_end(&vol,next)) fat_store(&vol,x,fat_eof(&vol)); break;}
            x=next;
        }
        made++; u8 *x=root+slot*32; char name[12]="FILE0000REC"; u32 number=made;
        for(int i=7;i>=4;i--) {name[i]='0'+number%10; number/=10;}
        memcpy(x,name,11); x[11]=FA_ARCHIVE; wr16(x+22,time); wr16(x+24,date); wr16(x+26,(u16)c); wr32(x+28,length*unit);
        slot++;
    }
    e=volume_write(&vol,vol.root_start,vol.root_sectors,root);
    if(!e) e=volume_save_fat(&vol);
    dos_free(root); dos_free(referenced);
    if(e) {print("\nCan not write file allocation table\n"); return e;}
    if(full) print("\nWarning - directory full\n");
    print("\n%u file(s) recovered\n",(unsigned long long)made);
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256],*cursor=tail; strcopy(tail,sizeof(tail),app_dos->command_tail());
    char *target=next_word(&cursor);
    if(!target || next_word(&cursor)) {print("\nInvalid drive or file name\n"); return 1;}
    char full[DOS_PATH_MAX]; unsigned drive; int whole=strlen(target)==2 && target[1]==':';
    if(whole) {if(drive_argument(target,&drive)) {print("\nInvalid drive or file name\n"); return 1;}}
    else {
        if(dos_canonical(target,full)) {print("\nInvalid drive or file name\n"); return 1;}
        drive=(unsigned)(full[0]-'A');
    }
    if(refuse_mapped(drive,"RECOVER")) return 1;
    print("\nPress any key to begin recovery of the\nfile(s) on drive %c:\n\n",'A'+drive);
    DosRegs r={.ax=0x0800}; if(dos_call(&r)) return 1;
    int e=dos_volume_lock(drive,1);
    if(e) {print("\nDrive %c: is in use; close its files and try again (DOS error %u)\n",'A'+drive,(unsigned long long)e); return 1;}
    e=volume_boot(drive,&vol);
    if(!e) e=volume_fat(&vol,0);
    if(e) print("\nCan not read file allocation table\n");
    else e=whole?recover_drive():recover_file(full);
    volume_free(&vol);
    int unlocked=dos_volume_lock(drive,0);
    return e || unlocked?1:0;
}
