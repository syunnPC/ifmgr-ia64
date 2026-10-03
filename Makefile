# SPDX-License-Identifier: GPL-2.0-or-later
# Tool overrides: command line, environment, then local.env (KEY=value).
# CROSS: IA-64 GCC prefix; WDK: WinDDK 7600.16385.1 root; MSRUN: Windows
# runner (e.g. wine); WATCOM: Open Watcom v2 root with Linux x64 tools.
# HOSTCC, X86_AS/X86_LD and MKISOFS select host/test/image tools.
# Without WDK, omit Windows modules but use prebuilt PE tests. Without
# Watcom, omit Win16 C, DOS extender and Windows sample tests.
local_env = $(shell sed -n 's/^$(1)=//p' local.env 2>/dev/null)
$(foreach v,CROSS WDK MSRUN WATCOM HOSTCC X86_AS X86_LD MKISOFS,$(if $(filter undefined default,$(origin $(v))),$(if $(call local_env,$(v)),$(eval $(v) := $(call local_env,$(v))))))
CROSS ?= ia64-linux-gnu-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
GNU_EFI := vendor/gnu-efi
CFLAGS := -std=c11 -O2 -g -fpic -ffreestanding -fno-builtin -fno-stack-protector -fno-common -fshort-wchar -mno-sdata -mfixed-range=f32-f127 -Wall -Wextra -Werror -Iinclude -MMD -MP
LDFLAGS := -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds
LIBGCC := $(shell $(CC) -print-libgcc-file-name)
EFI_CFLAGS := -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/ia64
COMMON := build/kernel/base.o build/platform/entry.o
CRT := build/crt0.o build/reloc.o
BOOT_OBJS := build/platform/boot.o build/platform/efi_support.o
IO_OBJS := build/platform/efi.o build/platform/efi_disk.o build/platform/efi_ports.o build/platform/efi_serial.o build/platform/efi_cdrom.o build/platform/efi_clock.o build/platform/efi_ia32.o build/platform/ia32_ia64.o build/platform/efi_vga.o build/platform/efi_support.o
DOS_OBJS := build/kernel/arena.o build/kernel/fat.o build/kernel/fat_io.o build/kernel/dos.o build/kernel/fcb.o build/kernel/nls.o build/kernel/nls_state.o build/kernel/codepage.o build/kernel/clock.o build/kernel/device.o build/kernel/block.o build/kernel/console.o build/kernel/ansi.o build/kernel/print.o build/kernel/keyb.o build/kernel/config.o build/kernel/io_adapter.o build/kernel/start.o build/platform/msdos_entry.o
CLIENT := build/sdk/client.o build/apps/runtime.o
COMMAND_OBJS := build/command/shell.o build/command/entry.o $(CLIENT)
.PHONY: all image run clean test test-host test-qemu dist
.SECONDARY:
.DELETE_ON_ERROR:
SYSTEM_BINARIES := build/BOOTIA64.EFI build/IO.SYS build/MSDOS.SYS build/COMMAND.COM
FILE_APPS := attrib find more sort tree comp xcopy replace edlin subst join assign append share fastopen nlsfunc graftabl backup restore print exe2bin keyb mem
# Their names on the DOS disk, as MS-DOS 4 shipped them.
FILE_UTILITIES := build/attrib.efi:ATTRIB.EXE build/find.efi:FIND.EXE build/more.efi:MORE.COM build/sort.efi:SORT.EXE build/tree.efi:TREE.COM build/comp.efi:COMP.COM build/xcopy.efi:XCOPY.EXE build/replace.efi:REPLACE.EXE build/edlin.efi:EDLIN.COM build/subst.efi:SUBST.EXE build/join.efi:JOIN.EXE build/assign.efi:ASSIGN.COM build/append.efi:APPEND.EXE build/share.efi:SHARE.EXE build/fastopen.efi:FASTOPEN.EXE build/nlsfunc.efi:NLSFUNC.EXE build/graftabl.efi:GRAFTABL.COM build/backup.efi:BACKUP.COM build/restore.efi:RESTORE.COM build/print.efi:PRINT.EXE build/exe2bin.efi:EXE2BIN.EXE build/keyb.efi:KEYB.COM build/mem.efi:MEM.EXE
APPS := hello apitest exit37 systest envtest filetest drivetest contest crittest devtest porttest ramtest timetest fcbtest nlstest mode pipetest chkdsk format sys label fdisk tsrtest diskcopy diskcomp recover fibertest taskhost gfxtest winsetup ia32test $(FILE_APPS)
BINARIES := $(SYSTEM_BINARIES) $(APPS:%=build/%.efi) build/LOOPDRV.SYS build/PORTDRV.SYS build/RAMDRV.SYS build/EFICD.SYS build/ANSI.SYS build/HIMEM.SYS build/EMM386.SYS build/MSCDEX.EXE build/TSRTEST.EXE build/TASKAPP.EXE build/peload.efi build/VDM.EXE
MEDIA_FILES := $(shell find media -print)
all: $(BINARIES)
# Only image entry shims, IO.SYS and application startup can include EFI headers.
build/platform/%.o build/apps/%.o build/drivers/%.o build/command/entry.o: CFLAGS += $(EFI_CFLAGS)
build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -fpic -c $< -o $@
FIBER := build/sdk/fiber.o build/sdk/fiber_ia64.o
build/fibertest.so build/taskhost.so: build/%.so: build/apps/%.o $(FIBER) $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
# Interface Manager: GCC host/PE loader; WDK IA-64 modules (LLP64) using
# only win/ headers and runtime (/X, /NODEFAULTLIB).
build/win/host/%.o: CFLAGS += $(EFI_CFLAGS) -Iapps -Iwin/host
WIN_HOST := build/win/host/env.o build/win/host/pe.o build/win/host/pe_ia64.o
build/peload.so: build/win/host/peload.o $(WIN_HOST) $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
# Fonts made from vendor/: GDI's, and WIN.COM's for its start-up screen.
build/win/font_%.h: vendor/misc-fixed/%.bdf tools/mkfont.py
	@mkdir -p $(@D)
	python3 tools/mkfont.py $< $@ FONT_$(shell echo $* | tr a-z A-Z)
# Wine's raster fonts: System, Fixedsys, Courier, MS Sans Serif and Small Fonts.
WINE_FONT = mkdir -p $(@D) && python3 tools/mkfont.py $< $@ $(1) $(2)
build/win/font_terminal.h: vendor/misc-fixed/9x15.bdf tools/mkfont.py
	python3 tools/mkfont.py $< $@ FONT_TERMINAL 0 cp437
build/win/font_system.h: vendor/wine-fonts/system.sfd tools/mkfont.py
	$(call WINE_FONT,FONT_SYSTEM,16)
build/win/font_fixedsys.h: vendor/wine-fonts/fixedsys.sfd tools/mkfont.py
	$(call WINE_FONT,FONT_FIXEDSYS,15)
build/win/font_courier.h: vendor/wine-fonts/courier.sfd tools/mkfont.py
	$(call WINE_FONT,FONT_COURIER,13)
build/win/font_sans%.h: vendor/wine-fonts/ms_sans_serif.sfd tools/mkfont.py
	$(call WINE_FONT,FONT_SANS$*,$*)
build/win/font_small.h: vendor/wine-fonts/small_fonts.sfd tools/mkfont.py
	$(call WINE_FONT,FONT_SMALL,11)
ifneq ($(WDK),)
ifeq ($(wildcard $(WDK)/bin/x86/ia64/cl.exe),)
$(error WDK=$(WDK) has no bin/x86/ia64/cl.exe)
endif
endif
MSCL := $(MSRUN) $(WDK)/bin/x86/ia64/cl.exe
MSLINK := $(MSRUN) $(WDK)/bin/x86/ia64/link.exe
MSIAS := $(MSRUN) $(WDK)/bin/x86/ia64/ias.exe
MSRC := $(MSRUN) $(WDK)/bin/x86/rc.exe
MSCFLAGS := /nologo /c /O2 /GS- /Zl /QIPF_fr32 /W3 /WX /X /Iwin/include /Iwin/tests /Iwin/kernel
MSLDFLAGS := /NOLOGO /NODEFAULTLIB /MACHINE:IA64 /FIXED:NO /INCREMENTAL:NO
WIN_SDK := build/win/crt.obj build/win/chkstk.obj
WIN_FIXTURES := petest.exe pedll.dll pedll2.dll pedyn.dll pefail.dll pebad.exe pestrip.exe
ifneq ($(WDK),)
build/win/%.obj: win/sdk/%.c
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Fo$@ $<
build/win/%.obj: win/tests/%.c win/tests/wintest.h
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Fo$@ $<
build/win/%.obj: win/sdk/%.s
	@mkdir -p $(@D)
	$(MSIAS) -o $@ $<
build/win/%.lib: win/tests/%.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
build/win/windefs.h: win/include/windows.h tools/mkwindefs.py
	@mkdir -p $(@D)
	python3 tools/mkwindefs.py $< $@
build/win/%.res: win/tests/%.rc build/win/windefs.h
	@mkdir -p $(@D)
	$(MSRC) /x /i build/win /i win/tests /fo $@ $<
build/win/%.dll: build/win/%.obj win/tests/%.def $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/tests/$*.def /IMPLIB:build/win/$*.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
build/win/pedll.dll: build/win/hosttest.lib build/win/pedll2.lib
build/win/pedll2.dll: build/win/hosttest.lib build/win/pedll.lib
build/win/pedyn.dll build/win/pefail.dll: build/win/hosttest.lib
build/win/petest.exe: build/win/petest.obj build/win/petest.res build/win/pedll.lib build/win/hosttest.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:petest_main /OUT:$@ $^
build/win/pebad.exe: build/win/pebad.obj build/win/pebad.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:pebad_main /OUT:$@ $^
# Linked without relocations, as cl's EXE default; the loader must refuse it.
build/win/pestrip.exe: build/win/pestrip.obj $(WIN_SDK)
	$(MSLINK) $(filter-out /FIXED:NO,$(MSLDFLAGS)) /SUBSYSTEM:WINDOWS /ENTRY:pestrip_main /OUT:$@ $^
# The Windows system: KERNEL/USER/GDI.DLL, the SDK and sample programs.
build/win/kernel/%.obj: win/kernel/%.c win/kernel/kernel.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Fo$@ $<
build/win/winhost.lib: win/host/winhost.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
build/win/kernel.lib: win/kernel/kernel.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
build/win/kernel.dll: build/win/kernel/kernel.obj build/win/kernel/memory.obj build/win/kernel/file.obj build/win/winhost.lib build/win/user.lib $(WIN_SDK) win/kernel/kernel.def
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/kernel/kernel.def /IMPLIB:build/win/kernel.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
GDI_FONTS := 6x10 9x15 9x15B terminal system fixedsys courier sans13 sans16 sans20 small
GDI_OBJS := $(patsubst %,build/win/gdi/%.obj,object region map draw bitmap text print metafile fontfile)
build/win/gdi/text.obj: $(GDI_FONTS:%=build/win/font_%.h)
build/win/gdi/text.obj build/win/gdi/fontfile.obj: win/gdi/fontfile.h
build/win/gdi/%.obj: win/gdi/%.c win/gdi/gdi.h win/gdi/gdip.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Iwin/gdi /Ibuild/win /Fo$@ $<
build/win/gdi.lib: win/gdi/gdi.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
build/win/gdi.dll: $(GDI_OBJS) build/win/winhost.lib build/win/kernel.lib $(WIN_SDK) win/gdi/gdi.def
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/gdi/gdi.def /IMPLIB:build/win/gdi.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
build/win/user/%.obj: win/user/%.c win/user/user.h win/gdi/gdi.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Iwin/gdi /Fo$@ $<
build/win/user.lib: win/user/user.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
USER_OBJS := $(patsubst %,build/win/user/%.obj,wnd msg draw nc menu resource dialog controls edit listbox mdi clip hook dde comm help dosaway pack16)
build/win/user.dll: $(USER_OBJS) build/win/winhost.lib build/win/kernel.lib build/win/gdi.lib $(WIN_SDK) win/user/user.def
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/user/user.def /IMPLIB:build/win/user.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
# COMMDLG.DLL: the common dialogs (Windows 3.1's, for Win16 programs too).
build/win/commdlg/%.obj: win/commdlg/%.c win/include/commdlg.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Fo$@ $<
build/win/commdlg.lib: win/commdlg/commdlg.def
	@mkdir -p $(@D)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /DEF:$< /OUT:$@
build/win/commdlg.dll: build/win/commdlg/commdlg.obj build/win/kernel.lib build/win/user.lib build/win/gdi.lib $(WIN_SDK) win/commdlg/commdlg.def
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/commdlg/commdlg.def /IMPLIB:build/win/commdlg.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
# libw.lib imports from all three modules, as in the Windows 3.0 SDK.
build/win/libw.lib: build/win/user.lib build/win/gdi.lib build/win/kernel.lib
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /OUT:$@ $^
# The C run-time library programs link (win/crt, its headers in win/include).
CRT_OBJS := $(patsubst win/crt/%.c,build/win/crt/%.obj,$(wildcard win/crt/*.c))
build/win/crt/%.obj: win/crt/%.c win/crt/crtp.h win/crt/fp.h $(wildcard win/include/*.h win/include/sys/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Fo$@ $<
build/win/libc.lib: $(CRT_OBJS)
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /OUT:$@ $^
# Accessories: win/apps/NAME.c, NAME.rc and icons/NAME.txt (ASCII art).
build/win/apps/%.obj: win/apps/%.c win/sdk/winapp.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Iwin/sdk /Fo$@ $<
build/win/icons/%.ico: win/apps/icons/%.txt tools/mkicon.py
	@mkdir -p $(@D)
	python3 tools/mkicon.py $< $@
build/win/icons/%.cur: win/apps/icons/%.txt tools/mkicon.py
	@mkdir -p $(@D)
	python3 tools/mkicon.py $< $@
build/win/apps/%.res: win/apps/%.rc build/win/windefs.h build/win/icons/%.ico
	@mkdir -p $(@D)
	$(MSRC) /x /i build/win /i win/apps /i win/sdk /fo $@ $<
build/win/winapp.lib: build/win/filedlg.obj build/win/printer.obj build/win/help.obj
	$(MSLINK) /LIB /NOLOGO /MACHINE:IA64 /OUT:$@ $^
build/win/filedlg.obj build/win/printer.obj build/win/help.obj: win/sdk/winapp.h win/sdk/helpmenu.h
build/win/%.exe: build/win/apps/%.obj build/win/winstart.obj build/win/libc.lib build/win/libw.lib build/win/winapp.lib build/win/commdlg.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
build/win/gditest.exe: build/win/gditest.obj build/win/winstart.obj build/win/libc.lib build/win/libw.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
# PACKTEST is written as a Win16 source is, casts and all: warnings stay warnings.
build/win/packtest.obj: win/tests/packtest.c $(wildcard win/include/*.h)
	$(MSCL) $(filter-out /W3 /WX,$(MSCFLAGS)) /W1 /Fo$@ $<
build/win/packtest.exe: build/win/packtest.obj build/win/winstart.obj build/win/libc.lib build/win/libw.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
build/win/crttest.exe: build/win/crttest.obj build/win/winstart.obj build/win/libc.lib build/win/libw.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
build/win/helptest.exe: build/win/helptest.obj build/win/winstart.obj build/win/libc.lib build/win/libw.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
build/win/usertest.obj: win/tests/usertest.h
build/win/usertest.res: win/tests/usertest.h
build/win/usertest.exe: build/win/usertest.obj build/win/usertest.res build/win/winstart.obj build/win/libc.lib build/win/libw.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $^
WOW_OBJS := $(patsubst %,build/win/wow/%.obj,ldt ne task thunk msg16 convert int21 memory16 kernel16 user16 gdi16 comm16 system16 api16)
build/win/wow/api16.c: win/wow/api16.txt win/wow/stubs16.txt tools/mkwow.py
	@mkdir -p $(@D)
	python3 tools/mkwow.py win/wow/api16.txt win/wow/stubs16.txt $@
build/win/wow/api16.obj: build/win/wow/api16.c win/wow/wow.h win/wow/api.h $(wildcard win/include/*.h)
	$(MSCL) $(MSCFLAGS) /Iwin/wow /Fo$@ $<
build/win/wow/%.obj: win/wow/%.c win/wow/wow.h win/wow/api.h $(wildcard win/include/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Iwin/wow /Fo$@ $<
build/win/wow.dll: $(WOW_OBJS) build/win/winhost.lib build/win/kernel.lib build/win/user.lib build/win/gdi.lib build/win/commdlg.lib $(WIN_SDK) win/wow/wow.def
	$(MSLINK) $(MSLDFLAGS) /DLL /ENTRY:DllMain /DEF:win/wow/wow.def /IMPLIB:build/win/wow.out.lib /OUT:$@ $(filter %.obj %.lib,$^)
WIN_SYSTEM := kernel.dll gdi.dll user.dll commdlg.dll wow.dll
# Help files: win/help/NAME.HPJ with its RTF topics, compiled by tools/hc.py.
WIN_HELP := NOTEPAD CALC CALENDAR CARDFILE CLIPBRD CONTROL PBRUSH PIFEDIT PRINTMAN PROGMAN RECORDER REVERSI SETUP SOL TERMINAL WINFILE WRITE WINHELP
build/win/help/%.HLP: win/help/%.HPJ $(wildcard win/help/*.RTF) tools/hc.py tools/hlpwrite.py
	@mkdir -p $(@D)
	python3 tools/hc.py $< $@
WIN_ACCESSORIES := clipbrd progman notepad calc clock reversi control calendar sol pbrush cardfile winfile taskman recorder terminal write printman setup pifedit winhelp
build/win/apps/progman.res: build/win/icons/group.ico build/win/icons/dos.ico
build/win/apps/control.res: $(patsubst %,build/win/icons/%.ico,ctlcolor ctldate ctlmouse ctldesk ctlfonts ctlports ctlprint ctlintl ctlkeyb ctlsound)
# Control Panel reads font files as GDI does.
build/win/apps/fontfile.obj: win/gdi/fontfile.c win/gdi/fontfile.h
	@mkdir -p $(@D)
	$(MSCL) $(MSCFLAGS) /Iwin/sdk /Fo$@ $<
build/win/apps/control.obj: win/gdi/fontfile.h
build/win/control.exe: build/win/apps/fontfile.obj
build/win/apps/winhelp.res: build/win/icons/winhand.cur
build/win/apps/winhelp.obj build/win/apps/hlpfile.obj: win/apps/hlpfile.h
build/win/winhelp.exe: build/win/apps/hlpfile.obj
WIN_APPS := hellowin.exe $(WIN_ACCESSORIES:%=%.exe)
$(foreach a,$(WIN_ACCESSORIES),$(eval build/win/$(a).exe: build/win/apps/$(a).res))
WIN_TESTS := gditest.exe usertest.exe helptest.exe crttest.exe packtest.exe
else
build/win/%: win/prebuilt/%
	@mkdir -p $(@D)
	cp $< $@
WIN_SYSTEM :=
WIN_APPS :=
WIN_TESTS :=
endif
build/win.so: build/win/host/win.o build/win/host/splash.o $(WIN_HOST) $(FIBER) $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/win/host/win.o: CFLAGS += -iquote win/include
build/win/host/splash.o: CFLAGS += -iquote build/win
build/win/host/splash.o: build/win/font_sans13.h
.PHONY: win-prebuilt
win-prebuilt: $(WIN_FIXTURES:%=build/win/%)
	mkdir -p win/prebuilt
	cp $^ win/prebuilt/
build/crt0.o: $(GNU_EFI)/crt0-efi-ia64.S
	@mkdir -p $(@D)
	$(CC) -fpic -c $< -o $@
build/reloc.o: $(GNU_EFI)/reloc_ia64.S
	@mkdir -p $(@D)
	$(CC) -fpic -c $< -o $@
build/boot.so: $(CRT) $(COMMON) $(BOOT_OBJS)
# The VGA's code page 437 fonts, from public-domain misc-fixed faces: 8x16
# for IO.SYS, 8x8, 8x14 and 8x16 for VDM's video BIOS.
VGA_FONTS := vendor/misc-fixed/9x15.bdf vendor/misc-fixed/6x10.bdf tools/mkvgafont.py tools/mkfont.py
build/vga_font.h: $(VGA_FONTS)
	@mkdir -p $(@D)
	python3 tools/mkvgafont.py $@ vendor/misc-fixed/9x15.bdf vendor/misc-fixed/6x10.bdf 16
build/vdm_fonts.h: $(VGA_FONTS)
	@mkdir -p $(@D)
	python3 tools/mkvgafont.py $@ vendor/misc-fixed/9x15.bdf vendor/misc-fixed/6x10.bdf 8 14 16 graftabl
build/platform/efi_vga.o: build/vga_font.h
build/platform/efi_vga.o: CFLAGS += -Ibuild
build/vdm/vga.o: build/vdm_fonts.h
build/vdm/vga.o: CFLAGS += -Ibuild
build/io.so: $(CRT) $(COMMON) $(IO_OBJS)
build/msdos.so: $(CRT) $(COMMON) $(DOS_OBJS)
build/command.so: $(CRT) $(COMMON) $(COMMAND_OBJS)
build/boot.so build/io.so build/msdos.so build/command.so:
	$(LD) $(LDFLAGS) -Map=$(@:.so=.map) $^ $(LIBGCC) -o $@
build/BOOTIA64.EFI: build/boot.so
build/IO.SYS: build/io.so
build/MSDOS.SYS: build/msdos.so
build/COMMAND.COM: build/command.so
$(SYSTEM_BINARIES):
	$(OBJCOPY) $(PEFLAGS) $< $@
PEFLAGS := -j .hash -j .gnu.hash -j .text -j .sdata -j .data -j .dynamic -j .rela -j .rela.plt -j .reloc -j .dynsym -j .dynstr -O pei-ia64 --image-base=0 --subsystem=10
# 8086 test programs for the VDM, built with the host's x86 binutils.
X86_AS ?= as --32 --divide
X86_LD ?= ld -m elf_i386
MKISOFS ?= genisoimage
build/dos16/%.o: tests/dos16/%.s
	@mkdir -p $(@D)
	$(X86_AS) $< -o $@
build/HELLO16.COM: build/dos16/hello16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/TINY16.COM: build/dos16/tiny16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/PRINT16.COM: build/dos16/print16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/VGA16.COM: build/dos16/vga16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/GFX16.COM: build/dos16/gfx16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/MEM16.COM: build/dos16/mem16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/DPMI16.COM: build/dos16/dpmi16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/KEYB16.COM: build/dos16/keyb16.o
	$(X86_LD) -Ttext=0x100 -e _start --oformat binary -o $@ $<
build/DOS16T.EXE: build/dos16/dos16t.o tests/dos16/exe.ld
	$(X86_LD) -T tests/dos16/exe.ld -o $@ $<
build/win16/%.o: tests/win16/%.s
	@mkdir -p $(@D)
	$(X86_AS) $< -o $@
build/W16TEST.EXE: build/win16/w16test.o tests/win16/ne.ld
	$(X86_LD) -T tests/win16/ne.ld -o $@ $<
# Open Watcom builds the Win16 test program in C (W16APP), its library
# (W16DLL) and DPMI32 under its DOS extenders.
ifneq ($(WATCOM),)
W16_ENV := env WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h:$(WATCOM)/h/win PATH=$(WATCOM)/binl64:$$PATH
build/win16/w16app.obj: tests/win16/w16app.c tests/win16/w16app.h
	@mkdir -p $(@D)
	$(W16_ENV) wcc -q -bt=windows -ms -zW -oxs -wx -fo=$@ $<
build/win16/w16app.ico build/win16/w16app.bmp: build/win16/w16app.%: tests/win16/w16app.txt tools/mkicon.py
	@mkdir -p $(@D)
	python3 tools/mkicon.py $< $@
build/win16/w16app.res: tests/win16/w16app.rc tests/win16/w16app.h build/win16/w16app.ico build/win16/w16app.bmp
	cd build/win16 && $(W16_ENV) wrc -q -r -bt=windows -i=$(CURDIR)/tests/win16 -fo=w16app.res $(CURDIR)/tests/win16/w16app.rc
build/win16/w16dll.obj: tests/win16/w16dll.c
	@mkdir -p $(@D)
	$(W16_ENV) wcc -q -bt=windows -bd -ml -zu -zW -oxs -wx -fo=$@ $<
build/W16DLL.DLL: build/win16/w16dll.obj
	$(W16_ENV) wlink option quiet system windows_dll name $@ file $<
build/win16/w16dll.lib: build/W16DLL.DLL
	rm -f $@; $(W16_ENV) wlib -q -n $@ +$<
build/W16APP.EXE: build/win16/w16app.obj build/win16/w16app.res build/win16/w16dll.lib
	$(W16_ENV) wlink option quiet system windows name $@ file build/win16/w16app.obj library build/win16/w16dll.lib library commdlg library shell library mmsystem library toolhelp library lzexpand option heapsize=1024 option stack=4096
	$(W16_ENV) wrc -q build/win16/w16app.res $@
W16_APPS := build/W16APP.EXE build/W16DLL.DLL build/LZTEST.TX_
# DPMI32 bound to the DOS extenders Open Watcom has, each a client of VDM's
# DPMI host (DOS/4GW's stub runs DOS4GW.EXE, copied beside it).
build/dos32/dpmi32.obj: tests/dos32/dpmi32.c
	@mkdir -p $(@D)
	$(W16_ENV) wcc386 -q -bt=dos -mf -oxs -wx -fo=$@ $<
build/DPMI32A.EXE build/DPMI32P.EXE build/DPMI32C.EXE build/DPMI32G.EXE: build/DPMI32%.EXE: build/dos32/dpmi32.obj
	$(W16_ENV) PATH=$(WATCOM)/binl64:$(WATCOM)/binw:$$PATH wlink option quiet system $(word $(if $(filter A,$*),1,$(if $(filter P,$*),2,$(if $(filter C,$*),3,4))),dos32a pmodew causeway dos4g) name $@ file $<
build/DOS4GW.EXE: $(WATCOM)/binw/dos4gw.exe
	cp $< $@
DOS32_APPS := build/DPMI32A.EXE build/DPMI32P.EXE build/DPMI32C.EXE build/DPMI32G.EXE build/DOS4GW.EXE
# A file as COMPRESS makes them, for LZEXPAND.
build/LZTEST.TX_: tests/win16/lztest.txt tools/mkszdd.py
	python3 tools/mkszdd.py $< $@
# Open Watcom's Windows samples, compiled from $(WATCOM)/samples/win as
# native programs (with Win32's message packing, __NT__) into the test image
# only: their sources stay Open Watcom's and out of the tree.
ifneq ($(and $(WIN_SYSTEM),$(WATCOM)),)
SAMPLES := generic iconview shootgal watzee datactl life alarm edit helpex
SAMPLE_CFLAGS := $(filter-out /W3 /WX,$(MSCFLAGS)) /W1 /D__NT__ /Ibuild/samples/src
build/samples/src/.copied: $(wildcard $(WATCOM)/samples/win/*.h $(SAMPLES:%=$(WATCOM)/samples/win/%/*))
	rm -rf build/samples/src
	mkdir -p build/samples/src
	cp -R $(WATCOM)/samples/win/. build/samples/src/
	touch $@
build/samples/%.obj: build/samples/src/.copied $(wildcard win/include/*.h win/include/sys/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(SAMPLE_CFLAGS) /Ibuild/samples/src/$(*D) /Fo$@ build/samples/src/$*.c
sample_rc = $(notdir $(firstword $(wildcard $(WATCOM)/samples/win/$(1)/*.rc)))
sample_objs = $(patsubst $(WATCOM)/samples/win/%.c,build/samples/%.obj,$(wildcard $(WATCOM)/samples/win/$(1)/*.c))
build/samples/%.res: build/samples/src/.copied $(wildcard win/include/*.h)
	cd build/samples/src/$* && $(MSRC) /x /i ../../../../win/include /i .. /fo ../../$*.res $(call sample_rc,$*)
$(foreach n,$(SAMPLES),$(eval build/samples/$(n).exe: $(call sample_objs,$(n)) build/samples/$(n).res))
$(SAMPLES:%=build/samples/%.exe): build/win/winstart.obj build/win/libc.lib build/win/libw.lib build/win/commdlg.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $(filter %.obj %.res %.lib,$^)
# Natively once more, down their Win16 code (no __NT__), with Windows 3.0's
# message packing (WIN16_MESSAGES).
SAMPLE16_CFLAGS := $(filter-out /D__NT__,$(SAMPLE_CFLAGS)) /DWIN16_MESSAGES
build/samples/n16/%.obj: build/samples/src/.copied $(wildcard win/include/*.h win/include/sys/*.h)
	@mkdir -p $(@D)
	$(MSCL) $(SAMPLE16_CFLAGS) /Ibuild/samples/src/$(*D) /Fo$@ build/samples/src/$*.c
$(foreach n,$(SAMPLES),$(eval build/samples/n16/$(n).exe: $(patsubst build/samples/%,build/samples/n16/%,$(call sample_objs,$(n))) build/samples/$(n).res))
$(SAMPLES:%=build/samples/n16/%.exe): build/win/winstart.obj build/win/libc.lib build/win/libw.lib build/win/commdlg.lib $(WIN_SDK)
	$(MSLINK) $(MSLDFLAGS) /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OUT:$@ $(filter %.obj %.res %.lib,$^)
# The same samples as Win16 programs, built by their own makefiles, for
# WOW; helpex's help file from its project by tools/hc.py.
build/samples/src/helpex/helpex.hlp: build/samples/src/.copied tools/hc.py tools/hlpwrite.py
	python3 tools/hc.py build/samples/src/helpex/helpex.hpj $@
	cp $@ build/samples/src/helpex/win16/helpex.hlp
build/samples/win16.stamp: build/samples/src/.copied build/samples/src/helpex/helpex.hlp
	for n in $(SAMPLES); do (cd build/samples/src/$$n/win16 && $(W16_ENV) wmake -h) || exit 1; done
	touch $@
endif
endif
VDM_OBJS := build/vdm/main.o build/vdm/dos.o build/vdm/bios.o build/vdm/ports.o build/vdm/vga.o build/vdm/xms.o build/vdm/ems.o build/vdm/dpmi.o
build/vdm/main.o: CFLAGS += $(EFI_CFLAGS) -Iapps
build/vdm.so: $(VDM_OBJS) $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/VDM.EXE: build/vdm.so
	$(OBJCOPY) $(PEFLAGS) $< $@
build/%.so: build/apps/%.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
MAINT_APPS := chkdsk format sys label fdisk diskcopy diskcomp recover
$(MAINT_APPS:%=build/%.so): build/%.so: build/apps/%.o build/apps/maint.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
# The file utilities, with their shared parser messages and directory walks.
$(FILE_APPS:%=build/%.so): build/%.so: build/apps/%.o build/apps/util.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/tree.so build/backup.so: build/apps/maint.o
build/apps/join.o: apps/subst.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DJOIN -c $< -o $@
build/%.efi: build/%.so
	$(OBJCOPY) $(PEFLAGS) $< $@
build/loop.so: build/drivers/loop.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/apps/diskcomp.o: apps/diskcopy.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DCOMPARE -c $< -o $@
build/TSRTEST.EXE: build/tsrtest.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/TASKAPP.EXE: build/taskapp.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/LOOPDRV.SYS: build/loop.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/ports.so: build/drivers/ports.o build/drivers/ports_entry.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/PORTDRV.SYS: build/ports.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/ramdisk.so: build/drivers/ramdisk.o build/drivers/ramdisk_entry.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/mscdex.so: build/apps/mscdex.o build/sdk/iso9660.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/MSCDEX.EXE: build/mscdex.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/eficd.so: build/drivers/eficd.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/EFICD.SYS: build/eficd.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/RAMDRV.SYS: build/ramdisk.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/ansi.so: build/drivers/ansi.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/ANSI.SYS: build/ansi.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/himem.so: build/drivers/himem.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/HIMEM.SYS: build/himem.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/emm386.so: build/drivers/emm386.o $(CLIENT) $(COMMON) $(CRT)
	$(LD) $(LDFLAGS) $(CRT) $(filter-out $(CRT),$^) $(LIBGCC) -o $@
build/EMM386.SYS: build/emm386.so
	$(OBJCOPY) $(filter-out --subsystem=10,$(PEFLAGS)) --subsystem=11 $< $@
build/COUNTRY.SYS build/country_data.h &: tools/mkcountry.py vendor/msdos4/MKCNTRY.ASM vendor/msdos4/MKCNTRY.INC vendor/msdos4/COPYRIGH.INC
	@mkdir -p $(@D)
	python3 tools/mkcountry.py build/COUNTRY.SYS build/country_data.h
build/kernel/nls_state.o: build/country_data.h
build/EFI.CPI build/codepage_data.h &: tools/mkcodepage.py tools/mkcountry.py vendor/msdos4/MKCNTRY.ASM vendor/msdos4/MKCNTRY.INC vendor/msdos4/COPYRIGH.INC
	@mkdir -p $(@D)
	python3 tools/mkcodepage.py build/EFI.CPI build/codepage_data.h
build/kernel/codepage.o: build/codepage_data.h
# KEYBOARD.SYS: DOS 4's layouts in KEYBOARD.LNK's order, then Japanese.
KDF_MODULES := KDFNOW KDFSP KDFPO KDFFR120 KDFFR189 KDFDK KDFSG KDFGE KDFIT141 KDFIT142 KDFUK166 KDFUK168 \
    KDFSF KDFBE KDFNL KDFNO KDFCF KDFSV KDFLA KDFEOF
build/KEYBOARD.SYS: tools/mkkeyboard.py tools/kdf/KDFJP.ASM $(KDF_MODULES:%=vendor/msdos4/keyboard/%.ASM) $(wildcard vendor/msdos4/keyboard/*.INC) vendor/msdos4/COPYRIGH.INC
	@mkdir -p $(@D)
	python3 tools/mkkeyboard.py $@ vendor/msdos4/keyboard $(KDF_MODULES:%=vendor/msdos4/keyboard/%.ASM) --add JP::tools/kdf/KDFJP.ASM
# License texts use DOS 8.3 names on both disk and CD.
LICENSE_FILES := LICENSE:GPL2.TXT vendor/msdos4/LICENSE:MSDOS4.TXT dist/EFI-IA64.TXT:EFI64.TXT \
    vendor/gnu-efi/LICENSE.BSD-3-Clause:EFIBSD.TXT vendor/gnu-efi/LICENSE.efilib:EFILIB.TXT \
    vendor/wine-fonts/COPYING.LIB:WINE.TXT vendor/misc-fixed/COPYING:FIXED.TXT
LICENSE_INPUTS := dist/NOTICE.TXT $(foreach f,$(LICENSE_FILES),$(word 1,$(subst :, ,$(f))))
define copy_licenses
mkdir -p $(1)/LICENSES
$(foreach f,$(LICENSE_FILES),cp $(word 1,$(subst :, ,$(f))) $(1)/LICENSES/$(word 2,$(subst :, ,$(f)));)
cp dist/NOTICE.TXT $(1)/NOTICE.TXT
endef
build/dos-ia64.img: $(BINARIES) build/HELLO16.COM build/TINY16.COM build/PRINT16.COM build/VGA16.COM build/GFX16.COM build/MEM16.COM build/DPMI16.COM build/KEYB16.COM build/DOS16T.EXE build/W16TEST.EXE $(W16_APPS) $(DOS32_APPS) build/COUNTRY.SYS build/EFI.CPI build/KEYBOARD.SYS $(WIN_FIXTURES:%=build/win/%) $(WIN_SYSTEM:%=build/win/%) $(WIN_APPS:%=build/win/%) $(WIN_TESTS:%=build/win/%) $(SAMPLES:%=build/samples/%.exe) $(SAMPLES:%=build/samples/n16/%.exe) $(if $(SAMPLES),build/samples/win16.stamp) $(if $(WIN_SYSTEM),build/win.efi) $(MEDIA_FILES) $(if $(WIN_SYSTEM),dist/win/WIN.INI $(WIN_HELP:%=build/win/help/%.HLP)) tools/mkimage.py Makefile $(LICENSE_INPUTS)
	rm -rf build/media
	mkdir -p build/media
	cp -R media/. build/media/
	$(call copy_licenses,build/media)
	cp build/IO.SYS build/MSDOS.SYS build/COMMAND.COM build/media/
	cp build/hello.efi build/media/HELLO.EFI
	cp build/apitest.efi build/media/APITEST.EFI
	cp build/exit37.efi build/media/EXIT37.EFI
	cp build/systest.efi build/media/SYSTEST.EFI
	cp build/envtest.efi build/media/ENVTEST.EFI
	cp build/filetest.efi build/media/FILETEST.EFI
	cp build/drivetest.efi build/media/DRVTEST.EFI
	cp build/contest.efi build/media/CONTEST.EFI
	cp build/crittest.efi build/media/CRITTEST.EFI
	cp build/devtest.efi build/media/DEVTEST.EFI
	cp build/porttest.efi build/media/PORTTEST.EFI
	cp build/ramtest.efi build/media/RAMTEST.EFI
	cp build/timetest.efi build/media/TIMETEST.EFI
	cp build/fcbtest.efi build/media/FCBTEST.EFI
	cp build/nlstest.efi build/media/NLSTEST.EFI
	cp build/mode.efi build/media/MODE.COM
	cp build/pipetest.efi build/media/PIPETEST.EFI
	cp build/fibertest.efi build/media/FIBRTEST.EFI
	cp build/taskhost.efi build/media/TASKHOST.EFI
	cp build/gfxtest.efi build/media/GFXTEST.EFI
	cp build/ia32test.efi build/media/IA32TEST.EFI
	cp build/VDM.EXE build/HELLO16.COM build/TINY16.COM build/PRINT16.COM build/VGA16.COM build/GFX16.COM build/MEM16.COM build/DPMI16.COM build/KEYB16.COM build/DOS16T.EXE $(DOS32_APPS) build/media/
	cp build/peload.efi build/media/PELOAD.EFI
	mkdir -p build/media/WINTEST
	for f in $(WIN_FIXTURES); do cp build/win/$$f build/media/WINTEST/$$(echo $$f | tr a-z A-Z); done
ifneq ($(WIN_SYSTEM),)
	mkdir -p build/media/WINDOWS/SYSTEM
	cp build/win.efi build/media/WINDOWS/WIN.COM
	for f in $(WIN_SYSTEM); do cp build/win/$$f build/media/WINDOWS/SYSTEM/$$(echo $$f | tr a-z A-Z); done
	for f in $(WIN_APPS) $(WIN_TESTS); do cp build/win/$$f build/media/WINDOWS/$$(echo $$f | tr a-z A-Z); done
	cp build/W16TEST.EXE $(W16_APPS) build/media/WINDOWS/
	cp dist/win/WIN.INI build/media/WINDOWS/WIN.INI
	cp $(WIN_HELP:%=build/win/help/%.HLP) build/media/WINDOWS/
endif
	cp build/TASKAPP.EXE build/media/TASKAPP.EXE
	cp build/tsrtest.efi build/media/TSRAPP.EFI
	cp build/TSRTEST.EXE build/media/TSRTEST.EXE
	cp build/chkdsk.efi build/media/CHKDSK.COM
	cp build/format.efi build/media/FORMAT.COM
	cp build/sys.efi build/media/SYS.COM
	cp build/label.efi build/media/LABEL.COM
	cp build/fdisk.efi build/media/FDISK.EXE
	cp build/diskcopy.efi build/media/DISKCOPY.COM
	cp build/diskcomp.efi build/media/DISKCOMP.COM
	cp build/recover.efi build/media/RECOVER.COM
	$(foreach f,$(FILE_UTILITIES),cp $(word 1,$(subst :, ,$(f))) build/media/$(word 2,$(subst :, ,$(f)));)
	cp build/LOOPDRV.SYS build/PORTDRV.SYS build/RAMDRV.SYS build/EFICD.SYS build/ANSI.SYS build/HIMEM.SYS build/EMM386.SYS build/MSCDEX.EXE build/media/
	cp build/COUNTRY.SYS build/EFI.CPI build/KEYBOARD.SYS build/media/
ifneq ($(SAMPLES),)
	mkdir -p build/media/SAMPLES
	mkdir -p build/media/SAMPLES/WIN16
	for f in $(SAMPLES:%=build/samples/%.exe) build/samples/src/life/*.lif build/samples/src/helpex/helpex.hlp; do cp $$f build/media/SAMPLES/$$(basename $$f | tr a-z A-Z); done
	for n in $(SAMPLES); do cp build/samples/src/$$n/win16/*.exe build/media/SAMPLES/WIN16/$$(echo $$n | tr a-z A-Z).EXE; done
	cp build/media/SAMPLES/*.LIF build/media/SAMPLES/HELPEX.HLP build/media/SAMPLES/WIN16/
	mkdir -p build/media/SAMPLES/N16
	for n in $(SAMPLES); do cp build/samples/n16/$$n.exe build/media/SAMPLES/N16/$$(echo $$n | tr a-z A-Z).EXE; done
	cp build/media/SAMPLES/*.LIF build/media/SAMPLES/HELPEX.HLP build/media/SAMPLES/N16/
endif
	python3 tools/mkimage.py $@ build/BOOTIA64.EFI build/media
image: build/dos-ia64.img
# Distribution: a DOS-only hard disk and the Windows CD (dist/README).
DOS_UTILITIES := build/chkdsk.efi:CHKDSK.COM build/format.efi:FORMAT.COM build/sys.efi:SYS.COM build/label.efi:LABEL.COM \
    build/fdisk.efi:FDISK.EXE build/diskcopy.efi:DISKCOPY.COM build/diskcomp.efi:DISKCOMP.COM build/recover.efi:RECOVER.COM \
    build/mode.efi:MODE.COM build/EFICD.SYS:EFICD.SYS build/MSCDEX.EXE:MSCDEX.EXE build/RAMDRV.SYS:RAMDRV.SYS build/ANSI.SYS:ANSI.SYS build/HIMEM.SYS:HIMEM.SYS build/EMM386.SYS:EMM386.SYS \
    build/COUNTRY.SYS:COUNTRY.SYS build/EFI.CPI:EFI.CPI build/KEYBOARD.SYS:KEYBOARD.SYS $(FILE_UTILITIES)
dist: build/dist/dos.img $(if $(WIN_SYSTEM),build/dist/win30.iso)
build/dist/dos.img: $(SYSTEM_BINARIES) build/VDM.EXE $(foreach f,$(DOS_UTILITIES),$(word 1,$(subst :, ,$(f)))) dist/dos/CONFIG.SYS dist/dos/AUTOEXEC.BAT tools/mkimage.py $(LICENSE_INPUTS) Makefile
	rm -rf build/dist/dos && mkdir -p build/dist/dos/DOS
	cp build/IO.SYS build/MSDOS.SYS build/COMMAND.COM build/VDM.EXE dist/dos/CONFIG.SYS dist/dos/AUTOEXEC.BAT build/dist/dos/
	$(call copy_licenses,build/dist/dos)
	$(foreach f,$(DOS_UTILITIES),cp $(word 1,$(subst :, ,$(f))) build/dist/dos/DOS/$(word 2,$(subst :, ,$(f)));)
	python3 tools/mkimage.py $@ build/BOOTIA64.EFI build/dist/dos
build/SETUP.EXE: build/winsetup.efi
	cp $< $@
build/dist/win30.iso: build/SETUP.EXE build/win.efi $(WIN_SYSTEM:%=build/win/%) $(WIN_APPS:%=build/win/%) $(WIN_HELP:%=build/win/help/%.HLP) dist/win/SETUP.INF dist/win/WIN.INI $(LICENSE_INPUTS) Makefile
	rm -rf build/dist/wincd && mkdir -p build/dist/wincd/WINDOWS/SYSTEM
	cp build/SETUP.EXE dist/win/SETUP.INF dist/win/WIN.INI build/dist/wincd/
	$(call copy_licenses,build/dist/wincd)
	cp build/win.efi build/dist/wincd/WINDOWS/WIN.COM
	for f in $(WIN_SYSTEM); do cp build/win/$$f build/dist/wincd/WINDOWS/SYSTEM/$$(echo $$f | tr a-z A-Z); done
	for f in $(WIN_APPS); do cp build/win/$$f build/dist/wincd/WINDOWS/$$(echo $$f | tr a-z A-Z); done
	cp $(WIN_HELP:%=build/win/help/%.HLP) build/dist/wincd/WINDOWS/
	$(MKISOFS) -quiet -V WIN30 -o $@ build/dist/wincd
run: image
	./tools/run.sh $(RUN_FLAGS)
build/host-test: tests/host.c tests/fcb_cases.h tests/nls_cases.h tests/maint_cases.h drivers/ports.c drivers/ports.h drivers/ramdisk.c drivers/ramdisk.h kernel/base.c kernel/arena.c kernel/fat.c kernel/fat_io.c kernel/dos.c kernel/fcb.c kernel/nls.c kernel/nls_state.c kernel/codepage.c kernel/clock.c kernel/device.c kernel/block.c kernel/console.c kernel/ansi.c kernel/print.c kernel/keyb.c kernel/config.c build/country_data.h build/codepage_data.h $(wildcard include/*.h)
	$(HOSTCC) -std=c11 -O1 -g -fno-builtin -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude $(filter %.c,$^) -o $@
HOSTCC ?= gcc
build/cdfs.iso: tools/cdfs_fixture.py
	@mkdir -p $(@D)
	python3 tools/cdfs_fixture.py $@
build/cdfs-test: tests/cdfs.c sdk/iso9660.c include/iso9660.h kernel/base.c include/base.h
	$(HOSTCC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude tests/cdfs.c sdk/iso9660.c kernel/base.c -o $@
build/pe-test: tests/pe.c win/host/pe.c win/host/pe.h kernel/base.c include/base.h $(WIN_FIXTURES:%=build/win/%)
	$(HOSTCC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude -Iwin/host tests/pe.c win/host/pe.c kernel/base.c -o $@
build/clock-test: tests/clock.c platform/efi_clock.c platform/efi_clock.h sdk/client.c kernel/base.c $(wildcard include/*.h)
	$(HOSTCC) -std=c11 -O1 -g -fno-builtin -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/$(shell uname -m) $(filter %.c,$^) -o $@
build/disk-test: tests/disk.c platform/efi_disk.c platform/efi_disk.h platform/efi_support.c kernel/base.c $(wildcard include/*.h)
	$(HOSTCC) -std=c11 -O1 -g -fno-builtin -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/$(shell uname -m) $(filter %.c,$^) -o $@
build/serial-test: tests/serial.c platform/efi_serial.c platform/efi_serial.h platform/efi_ports.c platform/efi_ports.h platform/efi_support.c platform/efi_support.h kernel/base.c $(wildcard include/*.h)
	$(HOSTCC) -std=c11 -O1 -g -fno-builtin -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/$(shell uname -m) $(filter %.c,$^) -o $@
build/nls-test: tests/nls.c kernel/nls.c kernel/base.c include/nls.h include/base.h
	$(HOSTCC) -std=c11 -O1 -g -fno-builtin -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude $(filter %.c,$^) -o $@
test-host: image build/host-test build/clock-test build/disk-test build/serial-test build/nls-test build/pe-test build/cdfs-test build/cdfs.iso
	./build/clock-test
	./build/disk-test
	./build/serial-test
	./build/nls-test build/COUNTRY.SYS
	./build/pe-test build/win
	./build/cdfs-test build/cdfs.iso
	python3 tools/mkimage.py build/host-fat12.img build/BOOTIA64.EFI build/media --fat12
	./build/host-test build/dos-ia64.img build/host-verified-fat16.img
	./build/host-test build/host-fat12.img build/host-verified-fat12.img
	python3 tools/check_image.py build/host-verified-fat16.img
	python3 tools/check_image.py build/host-verified-fat12.img
test-qemu: image
	python3 tools/test_qemu.py --machine all --fs both
test: test-host test-qemu
clean:
	rm -rf build
-include $(wildcard build/kernel/*.d build/platform/*.d build/apps/*.d build/sdk/*.d build/command/*.d build/drivers/*.d build/win/host/*.d build/vdm/*.d)
