<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# 8086 programs (VDM)

EXEC sends `.COM` files and non-IA-64 MZ programs to `C:\VDM.EXE`, which uses
hardware IA-32 virtual-8086 mode. Itanium and pre-9000-series Itanium 2
processors support IA-32; Montecito and later processors do not. VDM reports
missing support. IA-32 interruptions return to VDM's IA-64 handlers through
firmware Debug Support.

## Memory and DOS

- Conventional memory occupies physical memory below 640 KiB. The largest
  block is 544 KiB, or 480 KiB with EMM386.SYS's page frame. DOS reserves the
  firmware vector table at 10000h-17FFFh. Memory is saved and restored around
  each run, allowing several suspended VDMs.
- INT 21h implements DOS 4 over the native API: per-program file tables,
  memory control blocks, PSPs, environments, FCBs, 16-bit and native EXEC,
  overlays and chainable vector hooks. Unused vectors point to a shared IRET.
- `HIMEM.SYS [/HMAMIN=m] [/NUMHANDLES=n]` provides XMS 3.0 and the high memory
  area. XMS and DPMI share 16 MiB of pages below 4 GiB. A20 stays enabled;
  upper memory blocks are unavailable.
- `EMM386.SYS [kilobytes]` provides LIM EMS 4.0 with a page frame at 9000h;
  Itanium has no memory at C0000h-EFFFFh. Without IA-32 paging, mappings use
  copies: mapping a page twice creates two independent copies.
- DPMI 0.9 is always available (INT 2Fh 1687h). It supports one 16- or 32-bit
  client at privilege 3 with an LDT below 4 GiB. Services include descriptors, DOS and
  linear memory, interrupts, exceptions, real-mode calls and callbacks, and
  the raw mode switch. Linear addresses are physical: memory is unprotected
  and locking has no effect.

## BIOS and devices

- IO.SYS hands over the VGA adapter. VDM programs text mode 3 on first use,
  then uses VGA as the DOS console. INT 10h implements IBM VGA modes 0-7,
  0Dh, 0Eh and 10h-13h, fonts, palettes and the DAC; VBE and option ROMs are
  unsupported. Without VGA, output uses the native console. Console text is
  also sent to firmware serial.
- INT 16h supplies PC scan codes and KEYB translations. INT 1Ah and 40:6Ch
  supply time. INT 13h disks and the mouse report no device.
- Port I/O emulates VGA, the PIT, port 61h and the CMOS clock. The PIT uses
  the millisecond clock; other ports read as all ones. Guest port I/O never
  accesses hardware.
- Runs yield at the PIT period, with about 10 ms as the shortest interval
  through firmware callbacks. This simulates hooked INT 8 and 1Ch handlers
  and allows Ctrl-C to stop programs that make no calls. INT 9 is not simulated.
- VDM emulates IRET, HLT, MOV SS and SMSW, and supplies V86-monitor values
  for control register reads. Divide errors reach hooked INT 0 handlers;
  other protected-mode instructions or faults terminate the program with a
  message.
