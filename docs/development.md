<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Development notes

## Layout

| Directory | Contents |
| --- | --- |
| `platform/` | Bootstrap and IO.SYS: EFI, disks, ports, VGA and IA-32 execution |
| `kernel/` | MSDOS.SYS |
| `command/` | COMMAND.COM |
| `apps/`, `drivers/` | Utilities, test programs and DEVICE= drivers |
| `vdm/` | VDM.EXE |
| `sdk/` | Native client library and fibers |
| `win/` | Interface Manager: host, API modules, accessories and help |
| `include/` | Native hardware, DOS, driver and task interfaces |
| `tests/` | Host, 8086, DOS extender and Win16 tests |
| `tools/` | Generators, image builder, launchers and QEMU tests |
| `vendor/` | Third-party sources (see the README) |

## Native services

- `include/io.h` provides display modes, VGA text, input events, a millisecond
  clock, pages, disks, ports and IA-32 execution. The clock uses the interval
  time counter, calibrated against firmware Stall. One program can claim the
  screen; console output goes to firmware serial until it releases the claim.
- `include/fiber.h` switches cooperative fibers, preserving IA-64 registers
  and the register stack backing store. `TASKHOST` runs each subsystem-11
  guest on its own fiber and DOS task (`include/taskhost.h`).
- Firmware pointers require PS/2: use `i8042=on` on itanium2-vpc.

## Interface Manager builds

Modules use the WDK's IA-64 `cl /X` and link with `/NODEFAULTLIB` against
`win/sdk`, which supplies memory functions, `_fltused`, `/GS` cookies and
`__chkstk`. `/QIPF_fr32` preserves f32-f127 as EFI requires; `/FIXED:NO`
retains relocations because preferred bases exceed guest RAM. Exports and
entry points use function descriptors, matching GCC function pointers.

`win/prebuilt/` contains WDK-built PE loader test modules from `win/tests/`,
so loader tests work without the WDK. Rebuild them with `make win-prebuilt`.

## Tests

```sh
make test-host                                    # host tests (ASan/UBSan)
python3 tools/test_qemu.py --machine all --fs both # both machines, FAT16 and FAT12
python3 tools/test_qemu.py --only user,samples     # selected tests; --list shows names
make test-qemu                                    # build the image and run QEMU tests
make test                                         # host and QEMU tests
```

Host tests cover MSDOS.SYS, Block I/O, serial, clock, NLS, the PE loader and
ISO 9660 using disk images and mocked devices. ISO tests require genisoimage,
mkisofs or xorriso.

QEMU tests use `tools/test_vm.sh` with disposable images and serial I/O.
`MACHINE` and `DOS_IMAGE` select the machine and disk; QMP supplies input and
screendumps. Logs go to `build/`. Tests run in separate processes, defaulting
to one per processor (`--jobs` overrides this). Previous timings in
`build/qemu-test-times.json` determine the order, longest first. Results appear
as tests finish, followed by a failure list.

`QEMU_TEST_SLOW` scales timeouts (default 3). Screen checks wait for expected
content; clicks and typing wait for a stable screen because firmware reports
mouse button states only when polled.

### Test programs and samples

Native DOS test programs are in `apps/`; 8086 assembly tests in `tests/dos16/`;
DOS extender tests in `tests/dos32/`; Win16 tests in `tests/win16/`; and
Interface Manager API tests in `win/tests/`. QEMU also tests each accessory.
DOS extender tests use Open Watcom with DOS/32A, PMODE/W, CauseWay and DOS/4GW.
The extenders are copied from Open Watcom into the test image only.

Open Watcom's Windows samples (`$(WATCOM)/samples/win`) build from a copy in
`build/samples/src` into three test-image directories:

| Directory | Build |
| --- | --- |
| `C:\SAMPLES` | Native |
| `C:\SAMPLES\WIN16` | Win16, using the samples' makefiles |
| `C:\SAMPLES\N16` | Native, using Win16 code paths and `WIN16_MESSAGES` |

`tools/hc.py` compiles helpex's help file. Sample sources are under the Open
Watcom Public License and stay out of the repository and distribution media.
The `samples`, `samples16`, `samplesn16`, `crt` and `packing` tests use 4 GiB
of RAM to exercise firmware allocations above 2 GiB.

### Requirements and limits

- Interface Manager tests require the WDK; samples, DOS extenders and
  `W16APP` require Open Watcom. Missing builds are skipped. `install` also
  requires the Interface Manager CD from `make dist`.
- 8086 and Win16 tests run on itanium-vpc (Merced) and itanium2-vpc with
  `-cpu madison-1500`, and check rejection on the default Montecito.
- VPC firmware exposes only the boot disk, so D: uses a second partition.
- Merced's LSI53C895A path fails subsequent commands after a read error;
  the bad-sector RECOVER test therefore runs only on itanium2-vpc.
- Media replacement during writes is tested only with host disk providers.
