<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# DOS

The boot chain is `BOOTIA64.EFI` → `IO.SYS` → `MSDOS.SYS` → `COMMAND.COM`,
all native IA-64 PE32+ images. IO.SYS provides EFI hardware services
(`include/io.h`); MSDOS.SYS manages DOS state; COMMAND.COM is a replaceable
DOS program. Native programs use `include/dos_api.h` and `include/dos_client.h`
(see `apps/hello.c`).

Drivers and utilities live in `C:\` on `build/dos-ia64.img`, or `C:\DOS` on
the distribution disk.

## Kernel

- FAT12/16 supports sharing, byte-range locks, timestamps, cross-directory
  rename and extended open/create. Each firmware FAT volume gets a drive
  letter; the boot volume is C:. Media changes invalidate handles and searches.
- Reported I/O failures roll back metadata operations or sector writes.
  Mismatched FAT copies prevent mounting. Without journaling, interrupted
  writes may still require `CHKDSK /F`.
- The 4 MiB arena uses DOS 4 allocation algorithms. Tasks have 20-255 handles
  (AH=67h); EXEC inherits the first 20. Other services include native EXEC,
  batch files, redirection, pipes, FCB records/wildcards/labels, NLS
  (23 countries, 46 profiles, DBCS paths), critical errors and Ctrl-C handlers.
  Critical errors support Retry/Fail/Abort; Ignore applies only to zero-filled
  reads.
- Tasks isolate handles, 4 KiB environments, DTA, directories and memory
  ownership. There is no scheduler or memory protection; DOS calls are not
  reentrant.
- AH=31h keeps subsystem-11 images and their memory resident; subsystem-10
  applications cannot stay resident. IO.SYS uses firmware LoadImage but enters
  applications directly, allowing fibers to finish in any order. Drivers use
  StartImage.

Additional INT 21h calls: 1Bh/1Ch/1Fh/32h (native DPB snapshots), 34h, 37h,
50h/51h/62h (task ID as PSP), 5Dh, 5Eh00h/01h, 5Fh07h/08h, 4404h/4405h,
440Bh, 440Eh/440Fh, 4B01h with native 4B80h/4B81h, and 4B03h. Without a
redirector, other network calls return error 1. Vector calls 25h/35h and PSP
creation 26h have no native meaning.

## CONFIG.SYS

Supported directives: `SHELL`, `FILES` (8-255, default 64), `BREAK`, `COUNTRY`,
`DEVICE`, `INSTALL` and `REM`. Other lines report errors. The default shell
is `C:\COMMAND.COM /P`, which runs AUTOEXEC.BAT. INSTALL runs after drivers
and before the shell.

| Driver | Purpose |
| --- | --- |
| `PORTDRV.SYS COM1=2F8 LPT1=378` | 16550 serial and SPP printer ports; `COMn=EFIx` binds firmware Serial I/O unit x |
| `RAMDRV.SYS /SIZE:k /UNITS:n [/REMOVABLE]` | RAM drives: 128-32768 KiB each, up to 8 units and 64 MiB total |
| `ANSI.SYS [/X] [/K] [/L]` | DOS 4 CON escape sequences and key reassignment |
| `HIMEM.SYS`, `EMM386.SYS` | XMS and EMS for [8086 programs](vdm.md) |
| `EFICD.SYS` | Firmware CD-ROM drives as MSCD001 for MSCDEX |
| `LOOPDRV.SYS` | Sample resident character driver |

Serial ports use 1 ms polling and a 4 KiB ring buffer. A 10 ms firmware tick
can overrun above about 14400 baud. Firmware console ports are excluded;
AUX/PRN and handles 3/4 use COM1/LPT1.

## Console and code pages

CON transcodes output and keys between the selected code page and UTF-16.
DBCS characters survive split writes and remain intact during line editing.
CON has hardware page 437 and eight prepared slots using DOS 4 DISPLAY.SYS
IOCTLs:

```bat
MODE CON CP PREPARE=((850,932) C:\EFI.CPI)
MODE CON CP SELECT=932
```

`EFI.CPI` contains pages 437, 850, 860, 862, 863, 864, 865 and 932, generated
from Python codecs. Pages 934, 936 and 938 are NLS only. `COUNTRY=` sets only
the NLS page. CHCP (AH=6602h) switches NLS and CON together; if CON lacks the
page, error 65 leaves both unchanged. KEYB may reject the page after both
have switched. Firmware serial consoles drop non-ASCII output.

MSDOS.SYS interprets ANSI.SYS cursor movement/reports, erasing, attributes,
modes and key reassignment. `MODE CON` reads and sets COLUMNS/LINES through
IOCTL 440Ch. Firmware console colors have eight backgrounds and no blinking;
partial-screen erases leave the bottom-right cell.

## COMMAND.COM

The DOS 4 command language includes temporary-file pipes, `%0`-`%9`, SHIFT,
CALL, GOTO, single-level FOR, IF, PROMPT, VOL, VERIFY, CHCP, CTTY and TRUENAME;
COPY supports `+`, `/A`, `/B`, `/V`, and DIR supports `/W /P`.

Extensions are `%NAME%` at the prompt, DIR `/B`, EXIT `/B`, HELP and SHUTDOWN.
`/E:n` is accepted, but environments remain 4 KiB. Ctrl-C ends a batch file
without prompting. DATE accepts `YYYY-MM-DD`; TIME accepts `HH:MM[:SS[.hh]]`.

## Utilities

Utilities are native programs with DOS 4 options and messages.

| Area | Commands |
| --- | --- |
| Disks | CHKDSK, FORMAT, SYS, LABEL, FDISK, RECOVER, DISKCOPY, DISKCOMP |
| Files | ATTRIB, FIND, MORE, SORT, TREE, COMP, XCOPY, REPLACE, EDLIN, BACKUP, RESTORE, EXE2BIN |
| Drive mappings | SUBST, JOIN, ASSIGN, APPEND |

- `CHKDSK [/F /V]` repairs lost chains as FILEnnnn.CHK and truncates invalid
  chains; it reports cross-links.
- `FORMAT [/V[:label] /S /B /1 /4 /8 /N:n /T:n /F:size]` supports DOS 4
  formatting options. `/S` and SYS copy `\EFI\BOOT\BOOTIA64.EFI` in place
  of a DOS boot record.
  Firmware blocks of 1, 2 or 4 KiB are exposed as 512-byte sectors using
  read-modify-write; power loss can tear a block.
- FDISK supports primary, extended and logical partitions aligned to 1 MiB.
  DISKCOPY and DISKCOMP require removable media of equal size.
- BACKUP/RESTORE use DOS 3.3 and 4 formats; RESTORE also reads the older format.
- Drive mappings are system-wide MSDOS.SYS tables. Disk utilities reject
  mapped drives.
- SHARE, FASTOPEN and NLSFUNC validate parameters and report installation;
  sharing, caching and code page switching are built in. GRAFTABL selects
  characters for 8086 CGA graphics.
- MODE controls code pages, COM line settings, LPT columns/lines/retry and
  CON size.
- PRINT's DOS 4 queue resides in MSDOS.SYS and advances during DOS calls and
  keyboard waits (INT 2Fh 0100h-0106h for 8086 programs). Computation without
  DOS calls stalls printing because there is no timer interrupt.
- `MEM [/PROGRAM | /DEBUG]` reports the 4 MiB arena and lists its blocks with
  either option. It also reports EMS/XMS when EMM386.SYS/HIMEM.SYS is loaded.

### Keyboard layouts

`KEYB [xx[,[yyy][,[d:][path]KEYBOARD.SYS]]] [/ID:nnn]` supports DOS 4 layouts
(GR SP PO FR DK SG IT UK SF BE NL NO CF SV SU LA US) and a Japanese 106/109
layout without an input method. `tools/mkkeyboard.py` builds KEYBOARD.SYS.
MSDOS.SYS applies DOS 4 dead-key and AltGr logic; Ctrl+Alt+F1/F2 switches to
US and back. 8086 programs use INT 16h and INT 2Fh AD80h-AD82h.

Translation requires a US firmware layout and keys with shift-state data;
serial terminal input is not translated. Firmware cannot distinguish numeric
pad, 102nd, Japanese Ro and Yen keys. Caps Lock uses firmware toggle state or
letter case; beeps are unavailable. Interface Manager uses the US layout.

## CD-ROM

`DEVICE=EFICD.SYS` exposes firmware CD-ROM drives as MSCD001.
`MSCDEX /D:MSCD001 [/L:x]` mounts ISO 9660 as a read-only drive through
`include/dos_redir.h`. Names are 8.3; Joliet and Rock Ridge are unsupported.
Media checks resume half a second after a read, using a sector read to detect
changes.

## Distribution disk

`make dist` builds `build/dist/dos.img` with utilities in `C:\DOS` and CD-ROM
startup configuration. With the WDK, it also builds `build/dist/win30.iso`.

`tools/run.sh --dist` boots a working disk copy with the CD as D:.
Run `D:\SETUP` to install Interface Manager in `C:\WINDOWS` and add it to
PATH. Restart, then run `WIN` to start Program Manager.
