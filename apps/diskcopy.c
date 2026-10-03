/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT
 * MS-DOS 4.0 portions (messages, data or translated code): Copyright (c)
 * Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
 * DISKCOPY and DISKCOMP: native counterparts of MS-DOS 4 CMD/DISKCOPY and
 * CMD/DISKCOMP for removable media, through native INT 25h/26h. Built twice;
 * COMPARE selects DISKCOMP. Media must have the same size: there is no
 * format-while-copying for LBA media. Like DOS 4, DISKCOPY gives the copy a
 * new volume serial number and DISKCOMP ignores that field.
 * Exit codes follow DOS 4: 0 done/compare OK, 1 errors or differences,
 * 2 interrupted, 3 fatal, 4 initialization error.
 */
#include "maint.h"
static void geometry(const u8 *boot,u32 sectors,u32 *spt,u32 *heads) {
    *spt=rd16(boot+24); *heads=rd16(boot+26);
    if(!*spt || !*heads || *spt>255 || *heads>255) {*spt=63; *heads=sectors>2880?255:2;}
}
static void place(u32 sector,u32 spt,u32 heads,u32 *side,u32 *track) {*side=(sector/spt)%heads; *track=sector/(spt*heads);}
static int insert(const char *which,unsigned drive) {
    print("\nInsert %s diskette in drive %c:\n",which,'A'+drive);
    print("Press any key to continue . . .");
    DosRegs r={.ax=0x0800}; int e=dos_call(&r); print("\n"); return e;
}
static int removable_drive(const char *text,unsigned *drive) {
    DosDeviceParams p={.size=sizeof(p)};
    if(drive_argument(text,drive) || dos_device_params(*drive,&p) || (p.attributes&DOS_DEVICE_NONREMOVABLE)) return DE_DRIVE;
    return 0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    (void)image; EFI_STATUS status=app_init(st); if(EFI_ERROR(status)) return status;
    char tail[256],*cursor=tail,*names[2]; unsigned count=0;
    strcopy(tail,sizeof(tail),app_dos->command_tail());
    for(char *w;(w=next_word(&cursor));) {
        if(w[0]=='/') {
            char c=upper(w[1]);
            if(w[2] || (c!='1'
#ifdef COMPARE
               && c!='8'
#endif
               )) {print("\nInvalid parameter\n"); return 4;}
            continue;
        }
        if(count==2) {print("\nInvalid parameter\n"); return 4;}
        names[count++]=w;
    }
    unsigned first=dos_current_drive(),second=first;
    if(count) {
        for(unsigned i=0;i<count;i++) if(strlen(names[i])!=2 || names[i][1]!=':') {
#ifdef COMPARE
            print("Do not specify filename(s)\nCommand format: DISKCOMP d: d: [/1][/8]\n");
#else
            print("Do not specify filename(s)\nCommand Format: DISKCOPY d: d: [/1]\n");
#endif
            return 4;
        }
        if(removable_drive(names[0],&first) || (count==2 && removable_drive(names[1],&second)) ||
           (count==1 && (second=first,0))) {
            print("\nInvalid drive specification\nSpecified drive does not exist\nor is non-removable\n"); return 4;
        }
    } else {
        unsigned d; char text[3]={(char)('A'+first),':',0};
        if(removable_drive(text,&d)) {print("\nInvalid drive specification\nSpecified drive does not exist\nor is non-removable\n"); return 4;}
    }
    int one_drive=first==second,result=0;
    for(;;) {
#ifdef COMPARE
        if(insert("FIRST",first)) return 2;
        if(!one_drive && insert("SECOND",second)) return 2;
#else
        if(insert("SOURCE",first)) return 2;
        if(!one_drive && insert("TARGET",second)) return 2;
#endif
        DosDeviceParams a={.size=sizeof(a)},b={.size=sizeof(b)};
        u8 boot[512]; u32 n;
        int e=dos_device_params(first,&a);
        if(!e) e=dos_disk_read(first,0,1,boot,&n);
        if(e) {
            print(e==DE_NOTREADY?"\nDrive not ready\nMake sure a diskette is inserted into\nthe drive and the door is closed\n":
#ifdef COMPARE
                  "\nFIRST diskette bad or incompatible\n");
#else
                  "\nSOURCE diskette bad or incompatible\n");
#endif
            return 3;
        }
        u32 sectors=(u32)a.sectors,spt,heads; geometry(boot,sectors,&spt,&heads);
        u32 tracks=(sectors+spt*heads-1)/(spt*heads);
#ifdef COMPARE
        print("\nComparing %u tracks\n%u sectors per track, %u side(s)\n",(unsigned long long)tracks,(unsigned long long)spt,(unsigned long long)heads);
#else
        print("\nCopying %u tracks\n%u Sectors/Track, %u Side(s)\n",(unsigned long long)tracks,(unsigned long long)spt,(unsigned long long)heads);
#endif
        if(!one_drive) {
            e=dos_device_params(second,&b);
            if(e || b.sectors!=a.sectors) {print("\nDrive types or diskette types\nnot compatible\n"); return 3;}
        }
        DosInfo info; if(dos_query(&info)) return 3;
        u32 room=info.largest_paragraphs*16/512; if(room>2) room-=2;
        if(!one_drive && room>128) room=128;
        if(!room) {print("Insufficient memory\n"); return 4;}
        u8 *buffer; e=dos_alloc(room*32,(void **)&buffer); if(e) {print("Insufficient memory\n"); return 4;}
        u8 *other=NULL;
#ifdef COMPARE
        /* A single drive compares a buffered pass of FIRST against SECOND. */
        u32 half=room/2; if(!half) half=1;
        other=buffer+(u64)half*512; room=half;
        u32 failed_track=UINT32_MAX; int differs=0;
#else
        int locked=0,extended=0; u32 serial=new_serial();
#endif
        for(u32 at=0;at<sectors && !e;) {
            u32 take=MIN(room,sectors-at);
            if(one_drive && at) {
#ifdef COMPARE
                if(insert("FIRST",first)) {e=DE_BREAK; break;}
#else
                if(insert("SOURCE",first)) {e=DE_BREAK; break;}
#endif
            }
            u32 done; e=dos_disk_read(first,at,take,buffer,&done);
            if(e) {
                u32 side,track; place(at+done,spt,heads,&side,&track);
                print("\nUnrecoverable read error on drive %c\nSide %u, track %u\n",'A'+first,(unsigned long long)side,(unsigned long long)track);
                result=1; e=0; memset(buffer+(u64)done*512,0,(u64)(take-done)*512);
            }
#ifdef COMPARE
            if(one_drive && insert("SECOND",second)) {e=DE_BREAK; break;}
            e=dos_disk_read(second,at,take,other,&done);
            if(e) {print("\nSECOND diskette bad or incompatible\n"); break;}
            for(u32 s=0;s<take;s++) {
                u8 *x=buffer+(u64)s*512,*y=other+(u64)s*512;
                if(at+s==0 && x[38]==0x29 && y[38]==0x29) {memcpy(y+39,x+39,4);}
                if(memcmp(x,y,512)) {
                    u32 side,track; place(at+s,spt,heads,&side,&track);
                    if(track*heads+side!=failed_track) {
                        print("\nCompare error on\nside %u, track %u\n",(unsigned long long)side,(unsigned long long)track);
                        failed_track=track*heads+side;
                    }
                    differs=1;
                }
            }
#else
            if(one_drive && insert("TARGET",second)) {e=DE_BREAK; break;}
            if(!locked) {
                e=dos_volume_lock(second,1);
                if(e) {print("\nTarget diskette may be unusable\n"); break;}
                locked=1;
            }
            if(!at && buffer[38]==0x29 && rd16(buffer+510)==0xaa55) {wr32(buffer+39,serial); extended=1;}
            e=dos_disk_write(second,at,take,buffer,&done);
            if(e) {
                u32 side,track; place(at+done,spt,heads,&side,&track);
                print(e==DE_READONLY?"\nAttempt to write to write-protected diskette\n":
                      "\nUnrecoverable write error on drive %c\nSide %u, track %u\n",'A'+second,(unsigned long long)side,(unsigned long long)track);
                print("\nTarget diskette unusable\n");
                break;
            }
            if(one_drive) {int x=dos_volume_lock(second,0); locked=0; if(x && !e) e=x;}
#endif
            at+=take;
        }
#ifdef COMPARE
        if(!e && !differs) print("\nCompare OK\n");
        if(differs) result=1;
#else
        if(locked) {int x=dos_volume_lock(second,0); if(!e) e=x;}
        if(!e && extended) {char text[10]; serial_number(serial,text); print("\nVolume Serial Number is %s\n",text);}
#endif
        (void)other;
        dos_free(buffer);
        if(e==DE_BREAK) return 2;
        if(e) {
#ifdef COMPARE
            print("\nCompare process ended\n");
#else
            print("\nCopy process ended\n");
#endif
            return 3;
        }
#ifdef COMPARE
        if(!ask_yes_no("\nCompare another diskette (Y/N) ?")) break;
#else
        if(!ask_yes_no("\nCopy another diskette (Y/N)? ")) break;
#endif
    }
    return result;
}
