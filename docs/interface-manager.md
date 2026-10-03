<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Interface Manager 3.0

Interface Manager provides the Windows 3.0 API. Start it with:

```text
C:\WINDOWS\WIN [/M:n] [:] [program [arguments]]
```

WIN claims the screen, shows the startup screen (unless `:` is given), loads
KERNEL, USER and GDI from `%WINDIR%\SYSTEM`, and runs the requested program
(default: Program Manager). Display selection uses `/M:n`, then SYSTEM.INI's
`[display] resolution=`, then 800x600.

Each task has a WIN.COM fiber and a DOS context. Input is polled when programs
request messages; a busy program can lose keys if the firmware buffer fills.

## Native programs

Programs are IA-64 PE32+ modules built with the WDK and this tree's headers
and runtime. They include `win/include/windows.h`, start through
`win/sdk/winstart.c`, and link `libc.lib` and `libw.lib`. The PE loader
(`win/host/pe.c`) supports relocations, named and ordinal imports, forwarders,
import cycles and DllMain calls in dependency order.

The API uses Windows 3.0 names and module placement with Win64 types and
Win32 message packing. KERNEL32, USER32, GDI32 and Win16 module names are
aliases; Win32 `A` names are aliases or forwarders.

### Source compatibility

Windows 3.0 sources need few changes:

- Headers accept `far`, `near`, `huge`, `_export`, `_loadds` and `pascal`,
  provide `MAKEPOINT`, and include `commdlg.h`. `windowsx.h` supplies message
  crackers and control macros.
- Window, menu, accelerator, icon, cursor, hook, GDI and moveable memory
  handles are 16-bit values. Win16 sees the same window and GDI values.
  Fixed memory handles are addresses.
- LLP64 keeps `long` at 32 bits. Images, stacks, heaps, global memory and
  modules stay below 2 GiB so pointers stored in `LONG` remain valid.
  WIN.COM acquires low memory in 4 MiB pools when firmware pages lie higher.

Define `WIN16_MESSAGES` before `windows.h` to use Windows 3.0 message packing.
USER tracks window, dialog and hook procedures registered through
RegisterClass, SetWindowLong, dialog functions and SetWindowsHook. Their
messages, hook MSG/CWPSTRUCT data and GetMessage/PeekMessage results use
Win16 packing. SendMessage, CallWindowProc, DispatchMessage, Def*Proc,
CallNextHookEx and DefHookProc convert outgoing messages; `windowsx.h`
crackers follow the selected packing. GetWindowWord(GWW_HINSTANCE) returns
the full instance.

Message numbers remain Win32's: EM_, LB_ and CB_ constants written as
`WM_USER+n` can differ. Win16-packed DDE uses two words instead of native
blocks managed by USER; DDE memory must be moveable.

### C runtime

`libc.lib` (`win/crt/`) provides C89 plus Microsoft C extensions: stdio,
floating-point formatting/scanning, math, `_heapchk`, `_heapwalk`, time,
file/directory APIs (`io.h`, `_dos_*`, `direct.h`, `sys/stat.h`, `_splitpath`) and Open
Watcom's `dirent.h`. Stdio uses KERNEL files; text mode handles CR LF and ^Z,
and stdout/stderr use OutputDebugString. `exit` and return from WinMain run
`atexit` handlers and flush streams.

Time follows Microsoft C conventions. The clock is local time in `SET TZ=`
(e.g. `JST-9`, `EST5EDT`), defaulting to `PST8PDT`. Zones with a daylight-saving
name use US daylight-saving rules.

## Win16 programs

WOW.DLL runs Windows 3.0 NE programs and libraries using hardware IA-32
(see [VDM](vdm.md) for supported processors). Segments use 16-bit protected
mode at privilege 3. Import thunks convert arguments, structures and messages
for native calls; native code can call back into 16-bit window, dialog, hook
and enumeration procedures. USER and GDI handle values are shared.

About 700 functions are implemented across KERNEL, USER, GDI, COMMDLG, SHELL,
MMSYSTEM (timers only), LZEXPAND, VER, TOOLHELP and other modules
(`win/wow/api16.txt`). Unimplemented functions in `win/wow/stubs16.txt`
return 0 and are reported once. Global memory handles are selectors; the
local heap uses the automatic data segment; INT 21h uses the task's DOS context.
Self-loading programs and whole-class subclassing of native classes through
SetClassLong are unsupported.

## DOS programs

DOS programs and batch files run full screen while Interface Manager waits,
as in Windows 3.0 standard mode. Program Manager's DOS Prompt starts
COMMAND.COM; PIF files supply parameters, startup directory and options.

While a DOS program waits for a key, Alt+Tab, Alt+Esc or Ctrl+Esc suspends it
as an icon and preserves its screen. For 8086 programs, memory below 640 KiB
and VGA state are also saved. Multiple programs can be suspended. PIF's
Prevent Program Switch and Reserve Shortcut Keys control switching.
Firmware limits loaded images; exceeding the limit makes EXEC report
insufficient memory.

## GDI

GDI renders in memory and sends the result to the display with a software
cursor. It supports mapping modes, pens, brushes, shapes, flood fills,
regions, clipping, monochrome/32-bit bitmaps, all 256 raster operations,
1-32-bit DIBs with RLE, logical palettes on a true-color display, and Windows
metafiles in memory or files.

Raster fonts include Wine's System, MS Sans Serif, Courier, Small Fonts and
Fixedsys, misc-fixed Terminal faces and extra Courier sizes. .FON files
support FNT 2.0/3.0; vector fonts are unsupported. Sizes scale by integer
multiples, with synthetic bold and italic where needed. EnumFontFamilies
lists the same fonts as EnumFonts.

GDI's PSCRIPT driver renders pages at 150 dpi as PostScript Level 2 images,
then sends them to a port or file, normally through the spooler and Print
Manager. It accepts Windows 3.0 escapes and Windows 3.1 StartDoc functions.

## USER and KERNEL

USER supplies Windows 3.0 windows, menus (bitmap/owner-drawn items and
columns), dialogs, standard controls, MDI, timers, caret, clipboard, icons,
cursors, cross-task SendMessage, hooks (including journal recording/playback),
DDE, communications over PORTDRV.SYS and WinHelp. KERNEL supplies atoms,
.INI profiles, OpenFile and Win32 file/time functions.

## Accessories

Program Manager (including Setup DDE commands), File Manager, Control Panel,
Clipboard, Notepad, Write (.WRI with pictures), Paintbrush (.BMP), Calculator,
Clock, Calendar, Cardfile, Reversi, Solitaire, Task List, Recorder, Print
Manager, Setup, PIF Editor, Terminal (TTY/VT-100) and Help are included.

Help sources in `win/help/` compile with `tools/hc.py` to Windows 3.0 format.
Help also reads Windows 3.1 files, though 3.1 tables are untested with real
files. Icons are text drawings converted by `tools/mkicon.py`.
Control Panel saves Keyboard and Sound settings, but they have no effect:
firmware key repeat cannot be controlled and no sound device is available.
