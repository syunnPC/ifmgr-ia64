# Interface Manager 3.0 and DOS 4 for IA-64 EFI

> [!NOTE]
> This codebase is written using LLMs.

An experimental DOS 4–compatible system and graphical environment with the Windows 3.0 API, running as native IA-64 code on EFI. EFI boot services provide the hardware layer; 8086 DOS and Win16 programs use the processor’s IA-32 instruction set where available.

**Tested only in QEMU (`itanium-vpc` and `itanium2-vpc`), not on real Itanium hardware.**

[DOS](docs/dos.md) · [8086 programs](docs/vdm.md) · [Interface Manager](docs/interface-manager.md) · [Development and tests](docs/development.md)

## Building

- **Required:** GNU make, Python 3, an IA-64 GCC/binutils cross toolchain, a host C compiler and x86 binutils.
- **For Interface Manager:** WinDDK 7600.16385.1, plus Wine or another runner where its Windows tools cannot run directly. Without the WDK, Interface Manager is omitted.
- **Optional:** Open Watcom v2 with Linux x64 tools for additional tests; genisoimage for distribution CD images.

Set variables on the command line, in the environment or in an untracked `local.env` file (`KEY=value`). Main settings are `CROSS` (default: `ia64-linux-gnu-`), `WDK`, `MSRUN` (e.g. `wine`), `WATCOM` and `MKISOFS`.

```sh
make image    # build/dos-ia64.img: DOS, utilities, tests and Interface Manager
make dist     # build/dist/dos.img: DOS; build/dist/win30.iso: Interface Manager CD
```

GNU-EFI headers and startup code are bundled. No network access is needed to build.

## Running

[The IA-64-capable QEMU and its firmware](https://github.com/syunnPC/qemu-system-ia64) are required. Set `QEMU_ROOT` to a built QEMU tree, or set `QEMU` and `FIRMWARE` separately.

```sh
tools/run.sh             # Window with PS/2 keyboard and mouse
tools/run.sh --terminal  # Serial console in the terminal
tools/run.sh --dist      # Distribution disk and Interface Manager CD
tools/run.sh --help      # All options
make run                 # Build and run; accepts RUN_FLAGS
```

The default is `itanium2-vpc` with a Madison CPU and 256 MiB RAM. Use `--machine itanium-vpc` for Merced.

Disk changes persist in `build/interactive.img`, a working copy of the build image. Use `--fresh` to reset it.

## Testing

```sh
make test-host
python3 tools/test_qemu.py --machine all --fs both
```

QEMU tests cover both machines with FAT16 and FAT12. See [test documentation](docs/development.md#tests).

## Limitations

- **8086 / Win16:** Requires an IA-32-capable CPU: Itanium or Itanium 2 before the 9000 series. These programs have no IA-32 paging or memory protection; EMS mapping uses copying.
- **Input and interrupts:** EFI owns interrupts; input is polled. Busy GUI programs may lose keystrokes. 8086 programs receive a simulated timer interrupt but no keyboard interrupt.
- **DOS:** No on-disk journal; interrupted writes require `CHKDSK /F`. `DEBUG`, `DOSSHELL`, `SELECT` and `GRAPHICS` are unimplemented. `KEYB` translates only keys reported with shift state by US-layout firmware.
- **Interface Manager:** Windows 3.0 standard mode only, raster fonts and US keyboard layout only, no sound. Printing produces PostScript page images at 150 dpi.

## License

Original code is [GPL-2.0-or-later](LICENSE), without warranty. Third-party material retains its own licenses and notices.

| Material | License |
| --- | --- |
| MS-DOS 4.0 (`vendor/msdos4`) | MIT |
| GNU-EFI (`vendor/gnu-efi`) | BSD-3-Clause, GPL-2.0-or-later |
| Wine 9.0 raster fonts and `.spec` data | LGPL-2.1-or-later; fonts used under the GPL |
| misc-fixed fonts (`vendor/misc-fixed`) | Public domain |
| Liberation Serif (rendered text only, no font data) | OFL-1.1 |

Files translating or quoting MS-DOS sources are marked `GPL-2.0-or-later AND MIT` and retain Microsoft’s notice.

The WDK is used only for building, under Microsoft’s terms; none of it is linked in. Open Watcom’s DOS extenders and Windows samples appear only in test images, not in the repository or `make dist` media.