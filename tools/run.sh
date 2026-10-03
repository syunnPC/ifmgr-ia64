#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Starts the system in QEMU for use by hand: a window with the screen, the
# PS/2 keyboard and mouse; the serial console and QEMU's monitor are tabs of
# it. "tools/run.sh --help" lists the options. QEMU and FIRMWARE name the
# emulator and its IA-64 firmware; QEMU_ROOT, a built QEMU IA-64 tree,
# supplies both. The environment wins over the untracked local.env
# (KEY=value lines). The tests start QEMU with tools/test_vm.sh instead.
set -euo pipefail
cd "$(dirname "$0")/.."

usage() {
    cat <<'EOF'
usage: tools/run.sh [options] [-- QEMU arguments...]
  --terminal      no window: the serial console on this terminal (Ctrl-C
                  goes to DOS, Ctrl-A X quits QEMU, Ctrl-A C switches to
                  QEMU's monitor and back)
  --sdl           an SDL window instead of GTK's (the serial console and
                  the monitor on this terminal, as with --terminal)
  --gtk           a GTK window (the default), the serial console and the
                  monitor as tabs of it
  --machine NAME  itanium2-vpc (the default) or itanium-vpc (Merced, SCSI);
                  machine properties may follow (NAME,prop=value)
  --cpu NAME      the processor (default madison-1500 on itanium2-vpc,
                  which has the IA-32 instruction set 8086 programs need;
                  e.g. montecito-9050 for the machine's own)
  --image FILE    the hard disk image to boot, used as it is
  --fresh         make the working copy again (see below)
  --snapshot      keep the disk as it is: writes go to a temporary file
  --cd FILE       an ISO 9660 image as the CD-ROM drive (D: with EFICD.SYS
                  and MSCDEX)
  --dist          the distribution: build/dist/dos.img with
                  build/dist/win30.iso as the CD (make dist)
  --memory SIZE   RAM, e.g. 512M (default 256M)
  --qmp SOCKET    a QMP server on that UNIX socket
  --dry-run       show the QEMU command instead of running it
  -h, --help      this text
Without --image the disk is a working copy, build/interactive.img
(build/interactive-dist.img with --dist), made from build/dos-ia64.img
(build/dist/dos.img) when it is missing or with --fresh: what is done on
it lasts from one run to the next and stays apart from the tests' image.
In the window, Ctrl+Alt+G releases the mouse and the keyboard.
EOF
}

if [[ -f local.env ]]; then
    while IFS='=' read -r key value; do
        if [[ $key =~ ^(QEMU_ROOT|QEMU|FIRMWARE)$ && -z "${!key:-}" ]]; then printf -v "$key" '%s' "$value"; fi
    done < local.env
fi

display=gtk machine=itanium2-vpc cpu="" image="" fresh=0 snapshot=0 cd_image="" dist=0
memory=256M qmp="" dry_run=0
need() { [[ $# -ge 2 && -n "$2" ]] || { echo "run.sh: $1 needs a value" >&2; exit 2; }; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --terminal) display=none ;;
        --sdl) display=sdl ;;
        --gtk) display=gtk ;;
        --machine) need "$@"; machine="$2"; shift ;;
        --cpu) need "$@"; cpu="$2"; shift ;;
        --image) need "$@"; image="$2"; shift ;;
        --fresh) fresh=1 ;;
        --snapshot) snapshot=1 ;;
        --cd) need "$@"; cd_image="$2"; shift ;;
        --dist) dist=1 ;;
        --memory) need "$@"; memory="$2"; shift ;;
        --qmp) need "$@"; qmp="$2"; shift ;;
        --dry-run) dry_run=1 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) echo "run.sh: unknown option $1 (QEMU's own go after --; see --help)" >&2; exit 2 ;;
    esac
    shift
done

qemu_root="${QEMU_ROOT:-}"
qemu="${QEMU:-${qemu_root:+$qemu_root/build/qemu-system-ia64}}"
firmware="${FIRMWARE:-${qemu_root:+$qemu_root/build/roms/ia64-firmware/ia64-firmware.bin}}"
if [[ -z "$qemu" || -z "$firmware" ]]; then
    echo "run.sh: set QEMU_ROOT to a built QEMU IA-64 tree, or QEMU and FIRMWARE" >&2
    exit 2
fi
base="${machine%%,*}"
case "$base" in
    itanium2-vpc|ia64-vpc)
        if [[ -z "$cpu" ]]; then cpu=madison-1500; fi
        # The keyboard and the mouse the firmware drives are PS/2's.
        if [[ "$machine" != *i8042=* ]]; then machine="$machine,i8042=on"; fi ;;
    itanium-vpc) ;;
    *) echo "run.sh: unknown machine $base (itanium2-vpc or itanium-vpc)" >&2; exit 2 ;;
esac

# The disk: the one named, else a working copy of the system's or the
# distribution's (whose CD comes with it).
source_image=build/dos-ia64.img work=build/interactive.img
if [[ $dist -eq 1 ]]; then
    source_image=build/dist/dos.img work=build/interactive-dist.img
    if [[ -z "$cd_image" ]]; then cd_image=build/dist/win30.iso; fi
fi
if [[ -n "$image" ]]; then
    disk="$image"
else
    disk="$work"
    if [[ ! -f "$source_image" ]]; then
        echo "run.sh: $source_image is missing (make $([[ $dist -eq 1 ]] && echo dist || echo image))" >&2; exit 2
    fi
    if [[ $fresh -eq 1 || ! -f "$work" ]]; then
        if [[ $dry_run -eq 0 ]]; then cp "$source_image" "$work"; echo "run.sh: $work made from $source_image" >&2; fi
    elif [[ "$source_image" -nt "$work" ]]; then
        echo "run.sh: $work is older than $source_image; --fresh makes it again" >&2
    fi
fi
for f in "$disk" ${cd_image:+"$cd_image"}; do
    if [[ ! -f "$f" && $dry_run -eq 0 ]]; then echo "run.sh: $f is missing" >&2; exit 2; fi
done

disk_interface=ide
if [[ "$base" == itanium-vpc ]]; then disk_interface=scsi; fi
args=()
if [[ "$display" == none ]]; then
    console=serial; args+=(-display none)
else
    console=vga; args+=(-display "$display")
fi
if [[ "$display" == gtk ]]; then
    args+=(-serial vc -monitor vc)
else
    args+=(-chardev stdio,id=term,mux=on,signal=off -serial chardev:term -mon chardev=term,mode=readline)
fi
if [[ -n "$cpu" ]]; then args+=(-cpu "$cpu"); fi
if [[ $snapshot -eq 1 ]]; then args+=(-snapshot); fi
if [[ -n "$cd_image" ]]; then args+=(-drive "file=$cd_image,media=cdrom,if=$disk_interface,index=1"); fi
if [[ -n "$qmp" ]]; then args+=(-qmp "unix:$qmp,server=on,wait=off"); fi

command=("$qemu"
    -machine "$machine,firmware-console=$console,nvram=none"
    -bios "$firmware"
    -m "$memory" -smp 1 -nic none "${args[@]}"
    -drive "file=$disk,format=raw,if=$disk_interface,index=0" "$@")
if [[ $dry_run -eq 1 ]]; then printf '%q ' "${command[@]}"; echo; exit 0; fi
if [[ "$display" != gtk ]]; then echo "run.sh: Ctrl-A X quits, Ctrl-A C switches to QEMU's monitor" >&2; fi
exec "${command[@]}"
