/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * FORMAT: native reimplementation of MS-DOS 4 CMD/FORMAT for FAT12/16.
 * The BPB comes from AH=440Dh (MS-DOS 4 BIOS DiskTable2 and diskette rules),
 * the volume serial from MSFOR.ASM Create_Serial_ID. Fixed disks are verified
 * and keep their data area like DOS 4; removable media are zero-filled.
 * Exit codes follow DOS 4: 0 done, 3 interrupted, 4 fatal, 5 declined.
 * /BACKUP, as BACKUP /F runs it, formats the diskette in the drive without
 * asking for one first.
 */
#include "maint.h"
#define CHUNK 64U
typedef struct {const char *path; u8 attr; u8 *data; u32 size;} SystemFile;
static SystemFile system_files[]={
    {"\\EFI\\BOOT\\BOOTIA64.EFI",0,NULL,0},{"\\IO.SYS",FA_RDONLY|FA_HIDDEN|FA_SYSTEM,NULL,0},
    {"\\MSDOS.SYS",FA_RDONLY|FA_HIDDEN|FA_SYSTEM,NULL,0},{"\\COMMAND.COM",0,NULL,0}};
static u32 *bad; static unsigned bad_count,bad_capacity;
static int load_system(unsigned source) {
    for(unsigned i=0;i<ARRAY_SIZE(system_files);i++) {
        char path[DOS_PATH_MAX]={(char)('A'+source),':'}; strcopy(path+2,sizeof(path)-2,system_files[i].path);
        int e=load_file(path,&system_files[i].data,&system_files[i].size); if(e) return e;
    }
    return 0;
}
static int transfer_system(unsigned drive) {
    char path[DOS_PATH_MAX]={(char)('A'+drive),':','\\','E','F','I',0};
    int e=dos_mkdir(path); if(e && e!=DE_ACCESS && e!=DE_EXISTS) return e;
    e=strappend(path,sizeof(path),"\\BOOT"); if(e) return e;
    e=dos_mkdir(path); if(e && e!=DE_ACCESS && e!=DE_EXISTS) return e;
    for(unsigned i=0;i<ARRAY_SIZE(system_files);i++) {
        path[2]=0; strcopy(path+2,sizeof(path)-2,system_files[i].path);
        e=store_file(path,system_files[i].data,system_files[i].size,system_files[i].attr); if(e) return e;
    }
    return 0;
}
static int note_bad(u32 sector) {
    if(bad_count==bad_capacity) {
        u32 *next; int e=dos_alloc((bad_capacity*2+64)*4/16+1,(void **)&next); if(e) return e;
        if(bad) {memcpy(next,bad,bad_count*4); dos_free(bad);}
        bad=next; bad_capacity=bad_capacity*2+64;
    }
    bad[bad_count++]=sector; return 0;
}
/* Sectors zero-filled (removable) and verified. */
static int format_sectors(unsigned drive,u32 sector,u32 count,int removable) {
    DosSectorIo io={.size=sizeof(io),.sector=sector,.count=count};
    int e=removable?dos_sector_io(drive,0x42,&io):0;
    if(!e) {io=(DosSectorIo){.size=sizeof(io),.sector=sector,.count=count}; e=dos_sector_io(drive,0x62,&io);}
    return e;
}
/* Verify (fixed) or zero-fill and verify (removable) every sector. */
static int scan(unsigned drive,u32 total,int removable) {
    unsigned shown=101;
    for(u32 at=0;at<total;) {
        unsigned percent=(unsigned)((u64)at*100/total);
        if(percent!=shown) {print("\r%u percent of disk formatted",(unsigned long long)percent); shown=percent;}
        u32 count=MIN(CHUNK,total-at);
        int e=format_sectors(drive,at,count,removable);
        if(e==DE_BREAK) return e;
        if(e) {
            /* Retry one sector at a time to find exactly which are bad. */
            for(u32 s=0;s<count;s++) {
                int x=format_sectors(drive,at+s,1,removable);
                if(x && (x=note_bad(at+s))) return x;
            }
        }
        at+=count;
    }
    print("\r100 percent of disk formatted\n");
    return 0;
}
static int write_volume(unsigned drive,const DosDeviceParams *p,int removable,u32 serial,u32 *bad_clusters) {
    u32 total=(u32)p->sectors,spf=p->sectors_per_fat,root_sectors=(p->root_entries*32+511)/512;
    u32 root=p->reserved_sectors+p->fats*spf,data=root+root_sectors,clusters=(total-data)/p->sectors_per_cluster;
    for(unsigned i=0;i<bad_count;i++) if(bad[i]<data) {print("\nInvalid media or Track 0 bad - disk unusable\n"); return DE_IO;}
    u8 sector[512]; memset(sector,0,sizeof(sector)); u32 done;
    memcpy(sector,"\xeb\x3c\x90" "DOS4IA64",11); wr16(sector+11,512); sector[13]=(u8)p->sectors_per_cluster;
    wr16(sector+14,(u16)p->reserved_sectors); sector[16]=(u8)p->fats; wr16(sector+17,(u16)p->root_entries);
    if(total<65536) wr16(sector+19,(u16)total); else wr32(sector+32,total);
    sector[21]=(u8)p->media; wr16(sector+22,(u16)spf); wr16(sector+24,(u16)p->sectors_per_track); wr16(sector+26,(u16)p->heads);
    wr32(sector+28,p->hidden<=UINT32_MAX?(u32)p->hidden:0);
    sector[36]=removable?0:0x80; sector[38]=0x29; wr32(sector+39,serial);
    memcpy(sector+43,"NO NAME    ",11);
    memcpy(sector+54,clusters<4085?"FAT12   ":"FAT16   ",8);
    memcpy(sector+62,"\r\nNon-System disk or disk error\r\n",32);
    sector[510]=0x55; sector[511]=0xaa;
    int e=dos_disk_write(drive,0,1,sector,&done); if(e) {print("\nUnable to write BOOT\n"); return e;}
    memset(sector,0,sizeof(sector));
    for(u32 s=1;s<p->reserved_sectors;s++) {e=dos_disk_write(drive,s,1,sector,&done); if(e) return e;}
    Volume v={.drive=drive,.bits=clusters<4085?12:16,.clusters=clusters,.spf=spf,.fats=p->fats,.reserved=p->reserved_sectors};
    e=dos_alloc((spf*512+15)/16,(void **)&v.fat); if(e) return e;
    memset(v.fat,0,spf*512);
    fat_store(&v,0,0xff00|p->media); fat_store(&v,1,fat_eof(&v));
    if(v.bits==16) v.fat[0]=(u8)p->media;
    *bad_clusters=0;
    for(unsigned i=0;i<bad_count;i++) {
        u32 c=(bad[i]-data)/p->sectors_per_cluster+2;
        if(c<clusters+2 && fat_value(&v,c)!=fat_bad(&v)) {fat_store(&v,c,fat_bad(&v)); (*bad_clusters)++;}
    }
    e=volume_save_fat(&v); volume_free(&v);
    if(e) {print("\nError writing FAT\n"); return e;}
    memset(sector,0,sizeof(sector));
    for(u32 s=0;s<root_sectors && !e;s++) e=dos_disk_write(drive,root+s,1,sector,&done);
    if(e) print("\nError writing directory\n");
    return e;
}
static u32 floppy_sectors(unsigned kilobytes) {
    switch(kilobytes) {
    case 160: return 320; case 180: return 360; case 320: return 640; case 360: return 720;
    case 720: return 1440; case 1200: case 1210: return 2400; case 1440: return 2880; case 2880: return 5760;
    default: return 0;
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256]; strcopy(tail,sizeof(tail),app_dos->command_tail());
    unsigned drive=DOS_DRIVES; int system=0,have_label=0,for_backup=0; u8 label[11];
    u32 wanted=0,tracks=0,sides=2,per_track=0; int floppy_switch=0;
    char *cursor=tail;
    for(char *w;(w=next_word(&cursor));) {
        if(w[0]!='/') {
            if(drive!=DOS_DRIVES) {print("Too many parameters\n"); return 4;}
            if(drive_argument(w,&drive)) {print("Invalid drive specification\n"); return 4;}
            continue;
        }
        char c=upper(w[1]);
        if(!stricmp(w,"/BACKUP")) for_backup=1;
        else if(c=='S' && !w[2]) system=1;
        else if(c=='B' && !w[2]) {}
        else if(c=='V' && (!w[2] || w[2]==':')) {
            if(w[2]) {if(label_name(w+3,label)) {print("Invalid characters in volume label\n"); return 4;} have_label=1;}
        } else if(c=='1' && !w[2]) {sides=1; floppy_switch=1;}
        else if(c=='4' && !w[2]) {wanted=720; floppy_switch=1;}
        else if(c=='8' && !w[2]) {per_track=8; floppy_switch=1;}
        else if((c=='F' || c=='N' || c=='T') && w[2]==':') {
            u32 value=0; const char *p=w+3; if(!*p) {print("Invalid parameter\n"); return 4;}
            while(*p>='0' && *p<='9' && value<100000) value=value*10+(*p++-'0');
            if(*p) {print("Invalid parameter\n"); return 4;}
            if(c=='F') {wanted=floppy_sectors(value); if(!wanted) {print("Invalid parameter\n"); return 4;}}
            else if(c=='N') per_track=value; else tracks=value;
            floppy_switch=1;
        } else {print("Invalid parameter - %s\n",w); return 4;}
    }
    if(drive==DOS_DRIVES) {print("No target drive specified\n"); return 4;}
    if(refuse_mapped(drive,"FORMAT")) return 4;
    if((tracks && !per_track) || (per_track && !tracks && wanted)) {print("Must enter both /T and /N parameters\n"); return 4;}
    DosDeviceParams params={.size=sizeof(params)};
    int e=dos_device_params(drive,&params);
    if(e==DE_DRIVE) {print("Invalid drive specification\n"); return 4;}
    if(e) {print("Invalid device parameters from device driver\n"); return 4;}
    int removable=!(params.attributes&DOS_DEVICE_NONREMOVABLE);
    if(floppy_switch) {
        if(!removable) {print("Parameters not compatible\nwith fixed disk\n"); return 4;}
        if(tracks && per_track) wanted=tracks*per_track*sides;
        else if(per_track && !wanted) wanted=40*per_track*sides;
        if(wanted && wanted!=params.sectors) {print("Parameters not supported by drive\n"); return 4;}
    }
    if(system) {
        unsigned source=2; char comspec[DOS_PATH_MAX];
        if(!dos_env_get("COMSPEC",comspec,sizeof(comspec)) && comspec[1]==':') source=(unsigned)(upper(comspec[0])-'A');
        if(load_system(source)) {print("Cannot find System Files\n"); return 4;}
    }
    for(;;) {
        if(removable && !for_backup) {
            char dummy[4]; print("Insert new diskette for drive %c:\n",'A'+drive);
            if(read_text("and press ENTER when ready...",dummy,sizeof(dummy))) return 3;
        } else if(!removable) {
            char question[96]="\nWARNING, ALL DATA ON NON-REMOVABLE DISK\nDRIVE X: WILL BE LOST!\nProceed with Format (Y/N)?";
            *strchr(question,'X')=(char)('A'+drive);
            if(!ask_yes_no(question)) return 5;
        }
        e=dos_volume_lock(drive,1);
        if(e) {print("\nDrive %c: is in use; close its files and try again (DOS error %u)\n",'A'+drive,(unsigned long long)e); return 4;}
        params=(DosDeviceParams){.size=sizeof(params)};
        e=dos_device_params(drive,&params);
        if(e) {dos_volume_lock(drive,0); print("\nFormat not supported on drive %c:\n",'A'+drive); return 4;}
        bad_count=0;
        e=scan(drive,(u32)params.sectors,removable);
        u32 serial=new_serial(),bad_clusters=0;
        if(!e) e=write_volume(drive,&params,removable,serial,&bad_clusters);
        int unlocked=dos_volume_lock(drive,0); if(!e) e=unlocked;
        if(e==DE_BREAK) {print("\nFormat terminated\n"); return 3;}
        if(e) {print("\nFormat terminated\n"); return 4;}
        print("\rFormat complete                    \n");
        if(system) {
            e=transfer_system(drive);
            if(e) {print("Disk unsuitable for system disk\n"); return 4;}
            print("System transferred\n");
        }
        /* As in DOS 4 FORLABEL Volid, /V only supplies a label; FORMAT asks
         * otherwise, and the label is made with FCB delete and create. */
        int labelled=have_label;
        while(!labelled) {
            char text[16]; if(read_text("Volume label (11 characters, ENTER for none)? ",text,sizeof(text)) || !text[0]) break;
            if(label_name(text,label)) {print("Invalid characters in volume label\n"); continue;}
            labelled=1;
        }
        if(labelled && label_set(drive,label)) print("Invalid Volume ID\n");
        DosDriveInfo info; e=dos_drive_info(drive,&info);
        if(e) {print("Format terminated\n"); return 4;}
        u64 unit=(u64)info.sectors_per_cluster*512,total=(u64)info.total_clusters*unit,free=(u64)info.free_clusters*unit;
        u64 bad_bytes=(u64)bad_clusters*unit,used=total-free-bad_bytes;
        print("\n%10u bytes total disk space\n",(unsigned long long)total);
        if(system || used) print("%10u bytes used by system\n",(unsigned long long)used);
        if(bad_bytes) print("%10u bytes in bad sectors\n",(unsigned long long)bad_bytes);
        print("%10u bytes available on disk\n\n",(unsigned long long)free);
        print("%10u bytes in each allocation unit\n",(unsigned long long)unit);
        print("%10u allocation units available on disk\n\n",(unsigned long long)info.free_clusters);
        char text[10]; serial_number(serial,text); print("Volume Serial Number is %s\n",text);
        if(!removable || !ask_yes_no("\nFormat another (Y/N)?")) break;
    }
    if(bad) dos_free(bad);
    return 0;
}
