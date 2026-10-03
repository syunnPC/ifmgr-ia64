#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# QEMU as the tests (tools/test_qemu.py) start it: no display or monitor,
# the serial console on standard input and output, DOS_IMAGE (default
# build/dos-ia64.img) on the machine MACHINE (default itanium2-vpc), the
# arguments passed on to QEMU. tools/run.sh is for use by hand. QEMU and
# FIRMWARE name the emulator and its IA-64 firmware; QEMU_ROOT, a built
# QEMU IA-64 tree, supplies both. The environment wins over the untracked
# local.env (KEY=value lines).
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -f local.env ]]; then
    while IFS='=' read -r key value; do
        if [[ $key =~ ^(QEMU_ROOT|QEMU|FIRMWARE)$ && -z "${!key:-}" ]]; then printf -v "$key" '%s' "$value"; fi
    done < local.env
fi
qemu_root="${QEMU_ROOT:-}"
qemu="${QEMU:-${qemu_root:+$qemu_root/build/qemu-system-ia64}}"
firmware="${FIRMWARE:-${qemu_root:+$qemu_root/build/roms/ia64-firmware/ia64-firmware.bin}}"
if [[ -z "$qemu" || -z "$firmware" ]]; then
    echo "test_vm.sh: set QEMU_ROOT to a built QEMU IA-64 tree, or QEMU and FIRMWARE" >&2
    exit 2
fi
machine="${MACHINE:-itanium2-vpc}"
disk_interface=ide
if [[ "$machine" == itanium-vpc ]]; then disk_interface=scsi; fi
exec "$qemu" \
    -machine "$machine,firmware-console=serial,nvram=none" \
    -bios "$firmware" \
    -m 256M -smp 1 -nic none -display none -monitor none -serial stdio \
    -drive "file=${DOS_IMAGE:-build/dos-ia64.img},format=raw,if=$disk_interface,index=0" "$@"
