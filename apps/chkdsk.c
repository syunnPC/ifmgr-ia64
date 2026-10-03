/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * CHKDSK: native reimplementation of MS-DOS 4 CMD/CHKDSK checks and repairs.
 * Works from raw sectors, so it can repair volumes MSDOS.SYS refuses to mount.
 * /F holds the volume lock; MSDOS.SYS remounts the drive when it is released.
 */
#include "maint.h"
static Volume vol;
static u8 *used;
static int fix,verbose,errors;
static u32 dirs,files,hidden,dir_units,file_units,hidden_units,bad_units,label_date,label_time;
static char label[12],spec_dir[DOS_PATH_MAX]; static u8 spec_mask[11];
static int have_spec; static u32 fragmented;
static int failed;
static void error_found(void) {
    if(!errors++ && !fix) print("Errors found, F parameter not specified\nCorrections will not be written to disk\n\n");
}
static void report(const char *path,const char *message) {
    error_found(); print("%s\n%s\n",path,message);
}
static int fat_changed;
static void set_fat(u32 cluster,u32 value) {if(fix) {fat_store(&vol,cluster,value); fat_changed=1;}}
static int write_entry(u32 lba,u8 *sector) {
    if(!fix) return 0;
    int e=volume_write(&vol,lba,1,sector); if(e) {print("   Disk error writing directory\n"); failed=e;}
    return e;
}
static int valid_cluster(u32 c) {return c>=2 && c<vol.clusters+2;}
static void entry_name(const u8 *e,char *out) {
    unsigned n=0;
    for(unsigned i=0;i<8 && e[i]!=' ';i++) out[n++]=(char)(!i && e[0]==5?0xe5:e[i]);
    if(e[8]!=' ') {out[n++]='.'; for(unsigned i=8;i<11 && e[i]!=' ';i++) out[n++]=(char)e[i];}
    out[n]=0;
}
static int matches(const u8 *e) {
    for(unsigned i=0;i<11;i++) if(spec_mask[i]!='?' && spec_mask[i]!=e[i]) return 0;
    return 1;
}
/* Follow a chain like CHKPROC CHASELOOP: mark clusters, stop on a cross
 * link, and end an invalid link with EOF. Returns the cluster count. */
static u32 chain(const char *path,u32 first,int *cross,u32 *pieces) {
    u32 count=0,c=first; *cross=0; *pieces=1;
    for(u32 budget=vol.clusters;budget--;) {
        if(used[c]) {
            error_found(); print("%s\n   Is cross linked on cluster %u\n",path,(unsigned long long)c);
            *cross=1; return count;
        }
        used[c]=1; count++;
        u32 next=fat_value(&vol,c);
        if(fat_end(&vol,next)) return count;
        if(!valid_cluster(next)) {report(path,"   Has invalid cluster, file truncated"); set_fat(c,fat_eof(&vol)); return count;}
        if(next!=c+1) (*pieces)++;
        c=next;
    }
    report(path,"   Has invalid cluster, file truncated"); set_fat(c,fat_eof(&vol)); return count;
}
static int walk(const char *path,u32 dir,u32 parent,unsigned depth);
static int entry(const char *dirpath,u32 dir,u8 *e,u32 lba,u8 *sector,unsigned depth) {
    char name[13],path[DOS_PATH_MAX];
    entry_name(e,name);
    unsigned n=strlen(dirpath); memcpy(path,dirpath,n); if(n>3) path[n++]='\\';
    if(n+strlen(name)>=sizeof(path)) return 0;
    strcopy(path+n,sizeof(path)-n,name);
    u8 attr=e[11];
    if(attr==0x0f) return 0;
    if(attr&FA_VOLUME) {
        if(!dir && !(attr&FA_DIR) && !memcmp(label,e,11)) return 0;
        report(path,"   Entry has a bad attribute");
        if(fix) {e[0]=0xe5; return write_entry(lba,sector);}
        return 0;
    }
    u32 first=rd16(e+26),size=rd32(e+28),unit=vol.spc*512;
    if(verbose) print("%s\n",path);
    if(attr&FA_DIR) {
        if(!valid_cluster(first) || size) {
            report(path,"   Invalid sub-directory entry");
            if(fix) {e[0]=0xe5; return write_entry(lba,sector);}
            return 0;
        }
        int cross; u32 pieces,count=chain(path,first,&cross,&pieces);
        dirs++; dir_units+=count;
        if(cross || depth>=32) return 0;
        return walk(path,first,dir,depth+1);
    }
    u32 count=0,pieces=0; int cross=0;
    if(!first) {
        if(size) {
            report(path,"   Allocation error, size adjusted");
            if(fix) {wr32(e+28,0); return write_entry(lba,sector);}
        }
    } else if(!valid_cluster(first)) {
        report(path,"   First cluster number is invalid, entry truncated");
        if(fix) {wr16(e+26,0); wr32(e+28,0); return write_entry(lba,sector);}
    } else {
        count=chain(path,first,&cross,&pieces);
        u64 allocated=(u64)count*unit;
        if(!cross && (size>allocated || allocated-size>=unit)) {
            report(path,"   Allocation error, size adjusted");
            if(fix) {wr32(e+28,(u32)MIN(allocated,(u64)UINT32_MAX)); int x=write_entry(lba,sector); if(x) return x;}
        }
    }
    if(attr&(FA_HIDDEN|FA_SYSTEM)) {hidden++; hidden_units+=count;} else {files++; file_units+=count;}
    if(have_spec && !stricmp(dirpath,spec_dir) && matches(e)) {
        if(pieces>1) {print("%s Contains %u non-contiguous blocks\n",path,(unsigned long long)pieces); fragmented++;}
    }
    return 0;
}
/* Root is a fixed area; subdirectories follow their (already marked) chain. */
static int walk(const char *path,u32 dir,u32 parent,unsigned depth) {
    u8 sector[512]; u32 index=0,c=dir;
    if(verbose && dir) print("Directory %s\n",path);
    for(u32 budget=vol.clusters+1;budget--;) {
        u32 count=dir?vol.spc:vol.root_sectors;
        for(u32 s=0;s<count;s++) {
            u32 lba=dir?cluster_lba(&vol,c)+s:vol.root_start+s;
            int e=volume_read(&vol,lba,1,sector); if(e) {print("   Disk error reading directory %s\n",path); return e;}
            for(unsigned off=0;off<512;off+=32,index++) {
                u8 *x=sector+off;
                if(!x[0]) return 0;
                if(x[0]==0xe5) continue;
                if(dir && index<2) {
                    /* "." and ".." must name this directory and its parent. */
                    int ok=x[0]=='.' && (x[11]&FA_DIR) && !memcmp(x,index?"..         ":".          ",11) &&
                        rd16(x+26)==(index?parent:dir);
                    if(!ok) {
                        report(path,"   Invalid sub-directory entry");
                        if(fix) {
                            memcpy(x,index?"..         ":".          ",11); x[11]=FA_DIR;
                            wr16(x+26,(u16)(index?parent:dir)); wr32(x+28,0);
                            e=write_entry(lba,sector); if(e) return e;
                        }
                    }
                    continue;
                }
                e=entry(path,dir,x,lba,sector,depth); if(e) return e;
            }
        }
        if(!dir) return 0;
        u32 next=fat_value(&vol,c);
        if(fat_end(&vol,next) || !valid_cluster(next)) return 0;
        c=next;
    }
    return 0;
}
static int recover_lost(void) {
    u32 lost=0,chains=0; u8 *head;
    int e=dos_alloc((vol.clusters+2+15)/16,(void **)&head); if(e) return e;
    memset(head,0,vol.clusters+2);
    for(u32 c=2;c<vol.clusters+2;c++) {
        u32 v=fat_value(&vol,c);
        if(!v || v==fat_bad(&vol) || used[c]) continue;
        lost++; head[c]=1;
    }
    for(u32 c=2;c<vol.clusters+2;c++) if(head[c]==1 || head[c]==2) {
        u32 next=fat_value(&vol,c);
        if(valid_cluster(next) && next!=c && head[next]) head[next]=2; /* Referenced: not a chain head. */
    }
    for(u32 c=2;c<vol.clusters+2;c++) if(head[c]==1) chains++;
    if(!lost) {dos_free(head); return 0;}
    /* A lost cycle has no head; adopt one cluster of each remaining cycle. */
    for(u32 c=2;c<vol.clusters+2;c++) if(head[c]==2) {
        int reached=0;
        for(u32 h=2;h<vol.clusters+2 && !reached;h++) if(head[h]==1) {
            u32 x=h; for(u32 budget=vol.clusters;budget-- && valid_cluster(x) && head[x];x=fat_value(&vol,x)) if(x==c) {reached=1; break;}
        }
        if(!reached) {head[c]=1; chains++;}
    }
    error_found();
    print("   %u lost clusters found in %u chains.\n",(unsigned long long)lost,(unsigned long long)chains);
    int convert=ask_yes_no("Convert lost chains to files (Y/N)?");
    u64 unit=(u64)vol.spc*512; u32 made=0;
    if(!convert) {
        for(u32 c=2;c<vol.clusters+2;c++) if(head[c]) set_fat(c,0);
        print("%10u bytes disk space %s\n",(unsigned long long)(lost*unit),fix?"freed":"would be freed");
        dos_free(head); return 0;
    }
    u8 sector[512]; u32 lba=vol.root_start,slot=0; e=0; u16 date,time; fat_timestamp(&date,&time);
    for(u32 c=2;c<vol.clusters+2 && !e;c++) if(head[c]==1) {
        u32 length=0,x=c,last=c;
        for(u32 budget=vol.clusters;budget-- && valid_cluster(x) && head[x] && !used[x];x=fat_value(&vol,x)) {
            used[x]=1; length++; last=x;
        }
        set_fat(last,fat_eof(&vol)); /* Close cycles and runs. */
        made++;
        if(!fix) continue;
        /* Find a free root slot for FILEnnnn.CHK. */
        int placed=0;
        while(!placed && slot<vol.root_entries) {
            u32 want=vol.root_start+slot/16;
            if(want!=lba || slot%16==0) {lba=want; e=volume_read(&vol,lba,1,sector); if(e) break;}
            u8 *x2=sector+(slot%16)*32;
            if(!x2[0] || x2[0]==0xe5) {
                char name[12]; memcpy(name,"FILE0000CHK",11); u32 number=made-1;
                for(unsigned i=7;i>=4;i--) {name[i]='0'+number%10; number/=10;}
                memset(x2,0,32); memcpy(x2,name,11); x2[11]=FA_ARCHIVE;
                wr16(x2+22,time); wr16(x2+24,date); wr16(x2+26,(u16)c); wr32(x2+28,(u32)(length*unit));
                e=write_entry(lba,sector); placed=1;
            }
            slot++;
        }
        if(!placed && !e) {print("   Insufficient room in root directory\n   Move files from root directory and repeat CHKDSK\n"); break;}
    }
    if(fix) print("%10u bytes in %u recovered files\n",(unsigned long long)(lost*unit),(unsigned long long)made);
    else print("%10u bytes would be in %u recovered files\n",(unsigned long long)(lost*unit),(unsigned long long)made);
    dos_free(head); return e;
}
static int parse(char *tail,unsigned *drive) {
    *drive=dos_current_drive();
    for(char *w;(w=next_word(&tail));) {
        if(w[0]=='/') {
            for(char *s=w;*s;) {
                if(*s!='/') return DE_FUNCTION;
                char c=upper(s[1]);
                if(c=='F') fix=1; else if(c=='V') verbose=1; else return DE_FUNCTION;
                s+=2;
            }
            continue;
        }
        char full[DOS_PATH_MAX];
        if(w[0] && w[1]==':' && !w[2]) {if(drive_argument(w,drive)) return DE_DRIVE; continue;}
        int e=dos_canonical(w,full); if(e) return e;
        *drive=(unsigned)(full[0]-'A');
        char *leaf=full; for(char *p=full;*p;p++) if(*p=='\\') leaf=p;
        memcpy(spec_dir,full,leaf-full); spec_dir[leaf-full]=0; if(leaf-full==2) strcopy(spec_dir+2,sizeof(spec_dir)-2,"\\");
        leaf++; memset(spec_mask,' ',11);
        unsigned pos=0;
        for(char *p=leaf;*p;p++) {
            if(*p=='.') {pos=8; continue;}
            if(*p=='*') {unsigned end=pos<8?8:11; while(pos<end) spec_mask[pos++]='?'; continue;}
            if(pos<11) spec_mask[pos++]=upper(*p);
        }
        have_spec=1;
    }
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256]; strcopy(tail,sizeof(tail),app_dos->command_tail());
    unsigned drive; int e=parse(tail,&drive);
    if(e==DE_DRIVE) {print("Invalid drive specification\n"); return 1;}
    if(e) {print("Invalid parameter\n"); return 1;}
    if(refuse_mapped(drive,"CHKDSK")) return 1;
    if(fix) {
        e=dos_volume_lock(drive,1);
        if(e) {print("Cannot CHKDSK /F drive %c: while files are open on it (DOS error %u)\n",'A'+drive,(unsigned long long)e); return 1;}
    }
    e=volume_boot(drive,&vol);
    if(e==DE_FORMAT) {
        if(!ask_yes_no("Probable non-DOS disk\nContinue (Y/N)?")) {if(fix) dos_volume_lock(drive,0); return 1;}
        print("   File allocation table bad, drive %c:\n",'A'+drive); if(fix) dos_volume_lock(drive,0); return 1;
    }
    if(e) {print("Invalid drive specification\n"); if(fix) dos_volume_lock(drive,0); return 1;}
    e=volume_fat(&vol,0);
    if(e) {print("Disk error reading FAT 1\n"); goto out;}
    if((fat_value(&vol,0)&0xff)!=vol.media) {
        if(!ask_yes_no("Probable non-DOS disk\nContinue (Y/N)?")) goto out;
    }
    e=dos_alloc((vol.clusters+2+15)/16,(void **)&used); if(e) goto out;
    memset(used,0,vol.clusters+2);
    /* Compare FAT copies with the first; MSDOS.SYS refuses mismatches. */
    u8 *other; e=dos_alloc((vol.spf*512+15)/16,(void **)&other); if(e) goto out;
    for(unsigned copy=1;copy<vol.fats;copy++) {
        e=volume_read(&vol,vol.reserved+copy*vol.spf,vol.spf,other);
        if(e) {print("Disk error reading FAT %u\n",(unsigned long long)copy+1); continue;}
        if(memcmp(other,vol.fat,vol.spf*512)) {
            error_found(); print("   File allocation table %u differs from table 1\n",(unsigned long long)copy+1);
            if(fix) fat_changed=1;
        }
    }
    dos_free(other); e=0;
    /* DOS 4 identifies the volume before reporting errors. */
    u8 sector[512];
    for(u32 s=0;s<vol.root_sectors && !label[0];s++) {
        if(volume_read(&vol,vol.root_start+s,1,sector)) break;
        for(unsigned off=0;off<512 && !label[0];off+=32) {
            const u8 *x=sector+off; if(!x[0]) {s=vol.root_sectors; break;}
            if(x[0]!=0xe5 && (x[11]&FA_VOLUME) && !(x[11]&FA_DIR) && x[11]!=0x0f) {
                memcpy(label,x,11); label[11]=0; label_time=rd16(x+22); label_date=rd16(x+24);
            }
        }
    }
    DosMediaId id={.size=sizeof(id)};
    if(label[0]) {char when[24]; date_text((u16)label_date,(u16)label_time,when); print("Volume %s created %s\n",label,when);}
    if(!dos_media_id(drive,&id,0)) {char text[10]; serial_number(id.serial,text); print("Volume Serial Number is %s\n",text);}
    print("\n");
    char root[4]={(char)('A'+drive),':','\\',0};
    e=walk(root,0,0,0);
    for(u32 c=2;c<vol.clusters+2;c++) if(fat_value(&vol,c)==fat_bad(&vol)) bad_units++;
    if(!e) e=recover_lost();
    if(fix && fat_changed && !failed) {e=volume_save_fat(&vol); if(e) print("   Disk error writing FAT 1\n");}
    if(have_spec && !fragmented) print("All specified file(s) are contiguous\n");
    u64 unit=(u64)vol.spc*512; u32 free=0;
    for(u32 c=2;c<vol.clusters+2;c++) if(!fat_value(&vol,c)) free++;
    print("%s%10u bytes total disk space\n",errors?"\n":"",(unsigned long long)(vol.clusters*unit));
    if(bad_units) print("%10u bytes in bad sectors\n",(unsigned long long)(bad_units*unit));
    if(hidden) print("%10u bytes in %u hidden files\n",(unsigned long long)(hidden_units*unit),(unsigned long long)hidden);
    if(dirs) print("%10u bytes in %u directories\n",(unsigned long long)(dir_units*unit),(unsigned long long)dirs);
    print("%10u bytes in %u user files\n",(unsigned long long)(file_units*unit),(unsigned long long)files);
    print("%10u bytes available on disk\n\n",(unsigned long long)(free*unit));
    print("%10u bytes in each allocation unit\n",(unsigned long long)unit);
    print("%10u total allocation units on disk\n",(unsigned long long)vol.clusters);
    print("%10u available allocation units on disk\n\n",(unsigned long long)free);
    DosInfo info; if(!dos_query(&info)) print("%10u bytes free\n",(unsigned long long)info.largest_paragraphs*16);
out:
    if(used) dos_free(used);
    volume_free(&vol);
    if(fix) {int x=dos_volume_lock(drive,0); if(!e) e=x;}
    if(failed && !e) e=failed;
    return e?1:0;
}
