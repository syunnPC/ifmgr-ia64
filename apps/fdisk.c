/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * FDISK: native counterpart of MS-DOS 4 CMD/FDISK menus for MBR disks.
 * Tables are edited in memory and written through the physical-disk API on
 * exit; IO.SYS refuses writes inside published volumes, and the machine
 * restarts so firmware and DOS see the new layout. Disks are LBA-addressed:
 * partitions start on 1 MiB boundaries, logical drives one track after their
 * EBR, and CHS fields use a 255-head/63-sector translation.
 * /STATUS (a later-DOS extension) prints the tables without changing them.
 */
#include "maint.h"
#define MB 2048U
#define PART_ALIGN 2048U
#define MAX_LOGICAL 23U
typedef struct {u8 type,active; u32 start,length;} Part;
typedef struct {u32 ebr; Part part;} Logical;
static DosPhysicalInfo disk;
static unsigned disk_index,disk_total;
static u8 mbr[512];
static Part primary[4];
static Logical logical[MAX_LOGICAL];
static unsigned logical_count;
static int changed,extended_slot;
static int fat_type(u8 t) {return t==1 || t==4 || t==6 || t==0x0e || t==0xef;}
static int extended_type(u8 t) {return t==5 || t==0x0f;}
static u32 megabytes(u64 sectors) {return (u32)((sectors+MB/2)/MB);}
/* Keys: digits/letters echo, Backspace edits, Enter accepts, Esc cancels. */
static int field(const char *prompt,const char *preset,char *out,unsigned capacity) {
    print("%s[%s]",prompt,preset); for(size_t i=0;i<=strlen(preset);i++) print("\b");
    unsigned n=0; out[0]=0;
    for(;;) {
        DosRegs r={.ax=0x0800}; if(dos_call(&r)) return -1;
        u8 c=(u8)r.ax;
        if(c==27) {print("\n"); return -1;}
        if(c==0) {r=(DosRegs){.ax=0x0800}; dos_call(&r); continue;}
        if(c=='\r') {print("\n"); if(!n) strcopy(out,capacity,preset); return 0;}
        if(c==8) {if(n) {n--; out[n]=0; print("\b \b");} continue;}
        if(c<32 || n+1>=capacity) continue;
        out[n++]=(char)c; out[n]=0; print("%c",c);
    }
}
static int number(const char *prompt,unsigned preset,unsigned *out) {
    char text[16],initial[16]; unsigned n=0;
    do initial[n++]='0'+preset%10; while(preset/=10);
    for(unsigned i=0;i<n/2;i++) {char t=initial[i]; initial[i]=initial[n-1-i]; initial[n-1-i]=t;}
    initial[n]=0;
    for(;;) {
        if(field(prompt,initial,text,sizeof(text))) return -1;
        unsigned value=0,digits=0; char *p=text;
        while(*p>='0' && *p<='9' && value<100000000) {value=value*10+(*p++-'0'); digits++;}
        if(digits && (!*p || (*p=='%' && !p[1]))) {*out=value|(*p=='%'?0x80000000U:0); return 0;}
        print("Invalid entry, please enter a number\n");
    }
}
static int yes_no(const char *prompt,int preset) {
    for(;;) {
        char text[4]; if(field(prompt,preset?"Y":"N",text,sizeof(text))) return -1;
        if(upper(text[0])=='Y') return 1;
        if(upper(text[0])=='N') return 0;
        print("Invalid entry, please enter Y or N\n");
    }
}
static void chs(u8 *p,u32 lba) {
    u32 c=lba/(255*63),h=(lba/63)%255,s=lba%63+1;
    if(c>1023) {p[0]=254; p[1]=255; p[2]=255; return;}
    p[0]=(u8)h; p[1]=(u8)(s|((c>>2)&0xc0)); p[2]=(u8)c;
}
static void put_entry(u8 *e,const Part *part,u32 base) {
    memset(e,0,16); if(!part->type) return;
    e[0]=part->active?0x80:0; chs(e+1,part->start); e[4]=part->type; chs(e+5,part->start+part->length-1);
    wr32(e+8,part->start-base); wr32(e+12,part->length);
}
static int load(unsigned index) {
    disk_index=index; logical_count=0; extended_slot=-1; changed=0;
    memset(primary,0,sizeof(primary));
    int e=dos_physical_info(index,&disk); if(e) return e;
    u32 n; e=dos_physical_read(index,0,1,mbr,&n); if(e) return e;
    if(rd16(mbr+510)!=0xaa55) {memset(mbr,0,sizeof(mbr)); return 0;}
    for(unsigned i=0;i<4;i++) {
        const u8 *p=mbr+446+i*16;
        primary[i]=(Part){p[4],p[0]==0x80,rd32(p+8),rd32(p+12)};
        if(!primary[i].length) primary[i].type=0;
        if(extended_type(primary[i].type) && extended_slot<0) extended_slot=(int)i;
    }
    if(extended_slot<0) return 0;
    Part *x=&primary[extended_slot]; u32 ebr=x->start; u8 sector[512];
    for(unsigned guard=0;guard<MAX_LOGICAL;guard++) {
        e=dos_physical_read(index,ebr,1,sector,&n); if(e) return e;
        if(rd16(sector+510)!=0xaa55 || !sector[446+4] || !rd32(sector+446+12)) break;
        logical[logical_count++]=(Logical){ebr,{sector[446+4],0,ebr+rd32(sector+446+8),rd32(sector+446+12)}};
        const u8 *link=sector+462; u32 next=x->start+rd32(link+8);
        if(!extended_type(link[4]) || !rd32(link+12) || next<=ebr || next>=x->start+x->length) break;
        ebr=next;
    }
    return 0;
}
static int save(void) {
    if(!rd16(mbr+510)) wr32(mbr+440,new_serial());
    for(unsigned i=0;i<4;i++) put_entry(mbr+446+i*16,&primary[i],0);
    mbr[510]=0x55; mbr[511]=0xaa;
    u32 n; int e=dos_physical_write(disk_index,0,1,mbr,&n); if(e) return e;
    if(extended_slot<0) return 0;
    const Part *x=&primary[extended_slot];
    if(!logical_count) {
        u8 empty[512]={0}; empty[510]=0x55; empty[511]=0xaa;
        return dos_physical_write(disk_index,x->start,1,empty,&n);
    }
    for(unsigned i=0;i<logical_count;i++) {
        u8 sector[512]; memset(sector,0,sizeof(sector));
        put_entry(sector+446,&logical[i].part,logical[i].ebr);
        if(i+1<logical_count) {
            const Logical *next=&logical[i+1];
            Part link={5,0,next->ebr,next->part.start+next->part.length-next->ebr};
            put_entry(sector+462,&link,x->start);
        }
        sector[510]=0x55; sector[511]=0xaa;
        e=dos_physical_write(disk_index,logical[i].ebr,1,sector,&n); if(e) return e;
    }
    return 0;
}
static u8 type_for(u32 sectors) {return sectors<=32680?1:sectors<65536?4:6;}
static const char *type_name(u8 t) {return extended_type(t)?"EXT DOS":fat_type(t)?"PRI DOS":"NON-DOS";}
/* Largest aligned free run in [low,high) avoiding the given extents. */
static u32 largest_gap(u32 low,u32 high,const u32 *starts,const u32 *ends,unsigned count,u32 align,u32 *at) {
    u32 best=0; *at=0;
    for(u32 cursor=low;cursor<high;) {
        u32 start=(cursor+align-1)/align*align,limit=high;
        int moved=0;
        for(unsigned i=0;i<count;i++) {
            if(start>=starts[i] && start<ends[i]) {cursor=ends[i]; moved=1; break;}
            if(starts[i]>start && starts[i]<limit) limit=starts[i];
        }
        if(moved) continue;
        if(start<limit && limit-start>best) {best=limit-start; *at=start;}
        if(limit>=high) break;
        cursor=limit;
    }
    return best;
}
static u32 primary_gap(u32 *at) {
    u32 starts[4],ends[4]; unsigned n=0;
    for(unsigned i=0;i<4;i++) if(primary[i].type) {starts[n]=primary[i].start; ends[n++]=primary[i].start+primary[i].length;}
    u64 total=MIN(disk.sectors,(u64)UINT32_MAX);
    return largest_gap(PART_ALIGN,(u32)total,starts,ends,n,PART_ALIGN,at);
}
static u32 logical_gap(u32 *at) {
    u32 starts[MAX_LOGICAL],ends[MAX_LOGICAL];
    for(unsigned i=0;i<logical_count;i++) {starts[i]=logical[i].ebr; ends[i]=logical[i].part.start+logical[i].part.length;}
    const Part *x=&primary[extended_slot];
    return largest_gap(x->start,x->start+x->length,starts,ends,logical_count,1,at);
}
static unsigned drive_of(u32 start) {
    for(unsigned d=0;d<DOS_DRIVES;d++) if(disk.drives&(1U<<d)) {
        DosDeviceParams p={.size=sizeof(p)};
        if(!dos_device_params(d,&p) && p.hidden==start) return d;
    }
    return DOS_DRIVES;
}
static void volume_info(u32 start,char label[12],char system[9]) {
    u8 boot[512]; u32 n; strcopy(label,12,""); strcopy(system,9,"UNKNOWN");
    if(dos_physical_read(disk_index,start,1,boot,&n) || rd16(boot+510)!=0xaa55 || rd16(boot+11)!=512) return;
    if(boot[38]==0x29) {memcpy(label,boot+43,11); label[11]=0; memcpy(system,boot+54,8); system[8]=0;}
    else strcopy(system,9,"FAT");
    for(int i=10;i>=0 && label[i]==' ';i--) label[i]=0;
    for(int i=7;i>=0 && system[i]==' ';i--) system[i]=0;
}
static void header(const char *title) {
    print("\n%s\n\nCurrent fixed disk drive: %u\n\n",title,(unsigned long long)disk_index+1);
}
static void show_primary(void) {
    print("Partition Status   Type    Size in Mbytes   Percentage of Disk Used\n");
    unsigned shown=0;
    for(unsigned i=0;i<4;i++) if(primary[i].type) {
        unsigned d=fat_type(primary[i].type)?drive_of(primary[i].start):DOS_DRIVES;
        print(" %c%c %u        %c   %s       %4u         %3u%%\n",d<DOS_DRIVES?'A'+d:' ',d<DOS_DRIVES?':':' ',
              (unsigned long long)i+1,primary[i].active?'A':' ',type_name(primary[i].type),
              (unsigned long long)megabytes(primary[i].length),(unsigned long long)((u64)primary[i].length*100/disk.sectors));
        shown++;
    }
    if(!shown) print("No partitions defined\n");
    print("\nTotal disk space is %4u Mbytes (1 Mbyte = 1048576 bytes)\n",(unsigned long long)megabytes(disk.sectors));
}
static void show_logical(void) {
    print("Drv Volume Label  Mbytes  System  Usage\n");
    for(unsigned i=0;i<logical_count;i++) {
        char label[12],system[9]; volume_info(logical[i].part.start,label,system);
        unsigned d=drive_of(logical[i].part.start),pct=(unsigned)((u64)logical[i].part.length*100/primary[extended_slot].length);
        if(d<DOS_DRIVES) print("%c:  ",'A'+d); else print("#%u  ",(unsigned long long)i+1);
        print("%s",label); for(size_t k=strlen(label);k<13;k++) print(" ");
        print("  %4u  %s",(unsigned long long)megabytes(logical[i].part.length),system);
        for(size_t k=strlen(system);k<8;k++) print(" ");
        print("  %3u%%\n",(unsigned long long)pct);
    }
    print("\nTotal Extended DOS Partition size is %4u Mbytes (1 Mbyte = 1048576 bytes)\n",
          (unsigned long long)megabytes(primary[extended_slot].length));
}
static int dos_primary(void) {for(unsigned i=0;i<4;i++) if(fat_type(primary[i].type)) return (int)i; return -1;}
static int free_slot(void) {for(unsigned i=0;i<4;i++) if(!primary[i].type) return (int)i; return -1;}
static u32 requested(unsigned value,u64 whole,u32 maximum) {
    u64 sectors=value&0x80000000U?whole*(value&0x7fffffffU)/100:(u64)value*MB;
    return sectors>maximum?UINT32_MAX:(u32)sectors;
}
static void create_primary(void) {
    header("Create Primary DOS Partition");
    if(dos_primary()>=0) {print("Primary DOS Partition already exists.\n"); return;}
    int slot=free_slot(); u32 at,gap=primary_gap(&at);
    if(slot<0 || gap<MB) {print("No space to create a DOS partition.\n"); return;}
    int all=yes_no("Do you wish to use the maximum available size for a Primary DOS Partition\nand make the partition active (Y/N).....................? ",1);
    if(all<0) return;
    u32 size=gap;
    if(!all) {
        show_primary();
        print("Maximum space available for partition is %4u Mbytes (%u%%)\n\n",(unsigned long long)megabytes(gap),(unsigned long long)((u64)gap*100/disk.sectors));
        unsigned value;
        for(;;) {
            if(number("Enter partition size in Mbytes or percent of disk space (%) to\ncreate a Primary DOS Partition..................................",megabytes(gap),&value)) return;
            size=requested(value,disk.sectors,gap);
            if(size && size!=UINT32_MAX) break;
            print("Requested partition size exceeds the maximum available space\n");
        }
    }
    if(all) for(unsigned i=0;i<4;i++) primary[i].active=0;
    primary[slot]=(Part){type_for(size),(u8)all,at,size}; changed=1;
    print("Primary DOS Partition created\n");
}
static void create_extended(void) {
    header("Create Extended DOS Partition");
    if(extended_slot>=0) {print("Extended DOS Partition already exists.\n"); return;}
    if(dos_primary()<0 && !disk_index) {print("Cannot create Extended DOS Partition without\nPrimary DOS Partition on disk 1.\n"); return;}
    int slot=free_slot(); u32 at,gap=primary_gap(&at);
    if(slot<0 || gap<MB) {print("No space for an Extended DOS Partition\n"); return;}
    show_primary();
    print("Maximum space available for partition is %4u Mbytes (%u%%)\n\n",(unsigned long long)megabytes(gap),(unsigned long long)((u64)gap*100/disk.sectors));
    unsigned value; u32 size;
    for(;;) {
        if(number("Enter partition size in Mbytes or percent of disk space (%) to\ncreate an Extended DOS Partition................................",megabytes(gap),&value)) return;
        size=requested(value,disk.sectors,gap);
        if(size>=MB && size!=UINT32_MAX) break;
        print("Requested partition size exceeds the maximum available space\n");
    }
    primary[slot]=(Part){5,0,at,size}; extended_slot=slot; changed=1;
    print("Extended DOS Partition created\n");
}
static void create_logical(void) {
    print("\nCreate Logical DOS Drive(s) in the Extended DOS Partition\n\n");
    if(extended_slot<0) {print("Cannot create Logical DOS Drive without\nan Extended DOS Partition on the current drive.\n"); return;}
    for(;;) {
        if(logical_count) show_logical();
        u32 at,gap=logical_gap(&at);
        if(gap<=63+MB/2 || logical_count==MAX_LOGICAL) {print("All available space in the Extended DOS Partition\nis assigned to logical drives.\n"); return;}
        print("Maximum space available for logical drive is %4u Mbytes (%u%%)\n\n",(unsigned long long)megabytes(gap),
              (unsigned long long)((u64)gap*100/primary[extended_slot].length));
        unsigned value; u32 size;
        for(;;) {
            if(number("Enter logical drive size in Mbytes or percent of disk space (%)...",megabytes(gap),&value)) return;
            size=requested(value,primary[extended_slot].length,gap);
            if(size>63+MB/2 && size!=UINT32_MAX) break;
            print("Requested logical drive size exceeds the maximum available space\n");
        }
        unsigned i=logical_count; while(i && logical[i-1].ebr>at) {logical[i]=logical[i-1]; i--;}
        logical[i]=(Logical){at,{type_for(size-63),0,at+63,size-63}}; logical_count++; changed=1;
        print("Logical DOS Drive created, drive letters changed or added\n");
    }
}
static void set_active(void) {
    header("Set Active Partition");
    show_primary();
    unsigned value;
    if(number("Enter the number of the partition you want to make active............:",0,&value)) return;
    if(!value || value>4 || !primary[value-1].type) {print("Invalid entry, please enter 1-4\n"); return;}
    if(!fat_type(primary[value-1].type)) {print("Partition selected (%u) is not startable, active partition not changed.\n",(unsigned long long)value); return;}
    for(unsigned i=0;i<4;i++) primary[i].active=i==value-1;
    changed=1; print("Partition %u made active\n",(unsigned long long)value);
}
static void delete_partition(void) {
    print("\nDelete DOS Partition or Logical DOS Drive\n\nCurrent fixed disk drive: %u\n\n"
          "    1.  Delete Primary DOS Partition\n    2.  Delete Extended DOS Partition\n"
          "    3.  Delete Logical DOS Drive(s) in the Extended DOS Partition\n\n",(unsigned long long)disk_index+1);
    unsigned choice; if(number("Enter choice: ",0,&choice)) return;
    if(choice==1) {
        int slot=dos_primary(); if(slot<0) {print("No Primary DOS Partition to delete.\n"); return;}
        show_primary(); print("Warning! Data in the deleted Primary DOS Partition will be lost.\n");
        if(yes_no("Do you wish to continue (Y/N).................? ",0)!=1) return;
        primary[slot]=(Part){0}; changed=1; print("Primary DOS Partition deleted\n");
    } else if(choice==2) {
        if(extended_slot<0) {print("No Extended DOS Partition to delete.\n"); return;}
        if(logical_count) {print("Cannot delete Extended DOS Partition while logical drives exist.\n"); return;}
        show_primary(); print("Warning! Data in the deleted Extended DOS Partition will be lost.\n");
        if(yes_no("Do you wish to continue (Y/N).................? ",0)!=1) return;
        primary[extended_slot]=(Part){0}; extended_slot=-1; changed=1; print("Extended DOS Partition deleted\n");
    } else if(choice==3) {
        if(extended_slot<0 || !logical_count) {print("No Logical DOS Drive(s) to delete.\n"); return;}
        show_logical(); print("Warning! Data in a deleted Logical DOS Drive will be lost.\n");
        char which[8]; if(field("What drive do you want to delete...........................? ","",which,sizeof(which))) return;
        unsigned index=MAX_LOGICAL;
        for(unsigned i=0;i<logical_count;i++) {
            unsigned d=drive_of(logical[i].part.start);
            if((which[0]=='#' && (unsigned)(which[1]-'0')==i+1 && !which[2]) || (d<DOS_DRIVES && upper(which[0])=='A'+(char)d && (!which[1] || which[1]==':'))) index=i;
        }
        if(index==MAX_LOGICAL) {print("Invalid entry, please enter a listed drive\n"); return;}
        char label[12],system[9],typed[16]; volume_info(logical[index].part.start,label,system);
        if(field("Enter Volume Label.............................? ","",typed,sizeof(typed))) return;
        for(char *p=typed;*p;p++) *p=upper(*p);
        if(strcmp(typed,label)) {print("Volume label does not match.\n"); return;}
        if(yes_no("Are you sure (Y/N).............................? ",0)!=1) return;
        for(unsigned i=index;i+1<logical_count;i++) logical[i]=logical[i+1];
        logical_count--; changed=1; print("Drive deleted\n");
    }
}
static void display(void) {
    header("Display Partition Information");
    show_primary();
    if(extended_slot>=0 && logical_count) {
        print("\nThe Extended DOS Partition contains Logical DOS Drives.\n");
        if(yes_no("Do you want to display the logical drive information (Y/N)......? ",1)==1) {
            print("\nDisplay Logical DOS Drive Information\n\n"); show_logical();
        }
    }
}
static void status_report(void) {
    for(unsigned i=0;i<disk_total;i++) {
        if(load(i)) continue;
        if(disk.flags&DOS_DRIVE_REMOVABLE) continue;
        header("Display Partition Information"); show_primary();
        if(extended_slot>=0) {print("\n"); show_logical();}
    }
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[64],*cursor=tail; strcopy(tail,sizeof(tail),app_dos->command_tail());
    char *word=next_word(&cursor); int report=0;
    if(word && same_text(word,"/STATUS")) report=1;
    else if(word) {print("Invalid parameter\n"); return 1;}
    disk_total=0;
    for(unsigned i=0;i<32;i++) {DosPhysicalInfo p; if(dos_physical_info(i,&p)) break; disk_total++;}
    if(!disk_total) {print("No fixed disks present\n"); return 1;}
    if(report) {status_report(); return 0;}
    unsigned current=0;
    while(current<disk_total && (load(current) || (disk.flags&DOS_DRIVE_REMOVABLE) || !(disk.flags&DOS_DRIVE_PRESENT))) current++;
    if(current==disk_total) {print("No fixed disks present\n"); return 1;}
    unsigned fixed=0; for(unsigned i=0;i<disk_total;i++) {DosPhysicalInfo p; if(!dos_physical_info(i,&p) && !(p.flags&DOS_DRIVE_REMOVABLE)) fixed++;}
    int modified=0;
    for(;;) {
        print("\nFixed Disk Setup Program\n\n                                FDISK Options\n\n"
              "Current fixed disk drive: %u\n\nChoose one of the following:\n\n"
              "    1.  Create DOS Partition or Logical DOS Drive\n    2.  Set active partition\n"
              "    3.  Delete DOS Partition or Logical DOS Drive\n    4.  Display partition information\n",(unsigned long long)current+1);
        if(fixed>1) print("    5.  Select next fixed disk drive\n");
        int active=0; for(unsigned i=0;i<4;i++) if(primary[i].active) active=1;
        if(!active && dos_primary()>=0) print("\nWarning! No partitions are set active - disk %u is not startable unless\na partition is set active.\n",(unsigned long long)current+1);
        print("\nPress ESC to exit FDISK\n\n");
        unsigned choice; if(number("Enter choice: ",1,&choice)) break;
        if(choice==1) {
            print("\nCreate DOS Partition or Logical DOS Drive\n\nCurrent fixed disk drive: %u\n\n"
                  "    1.  Create Primary DOS Partition\n    2.  Create Extended DOS Partition\n"
                  "    3.  Create Logical DOS Drive(s) in the Extended DOS Partition\n\n",(unsigned long long)current+1);
            unsigned which; if(number("Enter choice: ",1,&which)) continue;
            if(which==1) create_primary(); else if(which==2) create_extended(); else if(which==3) create_logical();
        } else if(choice==2) set_active();
        else if(choice==3) delete_partition();
        else if(choice==4) display();
        else if(choice==5 && fixed>1) {
            if(changed) {int e=save(); if(e) {print("Error writing fixed disk\n"); return 1;} modified=1;}
            do current=(current+1)%disk_total; while(load(current) || (disk.flags&DOS_DRIVE_REMOVABLE));
        } else print("Invalid entry, please enter 1-%u.\n",(unsigned long long)(fixed>1?5:4));
        if(changed && choice!=5) modified=1;
    }
    if(changed) {
        int e=save(); if(e) {print("Error writing fixed disk (DOS error %u)\n",(unsigned long long)e); return 1;}
        modified=1;
    }
    if(!modified) return 0;
    print("\nSystem will now restart\n\nPress any key when ready . . .");
    DosRegs r={.ax=0x0800}; dos_call(&r); print("\n");
    dos_restart();
    return 0;
}
