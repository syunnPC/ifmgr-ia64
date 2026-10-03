#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build an MBR/FAT16 (or, with --fat12, FAT12) EFI disk, with deterministic DOS 8.3 names. No root/tools."""
from pathlib import Path
import struct
import sys

def name83(name):
    bits = name.upper().split('.')
    if len(bits) > 2 or not 1 <= len(bits[0]) <= 8 or (len(bits) == 2 and len(bits[1]) > 3):
        raise ValueError(f'not an 8.3 name: {name}')
    return (bits[0].ljust(8) + (bits[1] if len(bits) == 2 else '').ljust(3)).encode('ascii')

def build(output, boot, media, fat12=False):
    start = 2048
    sectors, spc, spf = (16384, 4, 12) if fat12 else (65536, 2, 128)
    unit = spc * 512
    root_entries = 512
    root_start = 1 + 2 * spf
    data_start = root_start + 32
    image = bytearray((start + sectors) * 512)
    image[446:462] = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', 0xef, b'\xfe\xff\xff', start, sectors)
    image[510:512] = b'\x55\xaa'
    volume = memoryview(image)[start * 512:]
    volume[:11] = b'\xeb\x3c\x90DOS4IA64'
    struct.pack_into('<HBHBHHBHHHII', volume, 11, 512, spc, 1, 2, root_entries,
                     sectors if sectors < 65536 else 0, 0xf8, spf, 63, 255, start,
                     sectors if sectors >= 65536 else 0)
    struct.pack_into('<BBBI11s8s', volume, 36, 0x80, 0, 0x29, 0x49413634, b'DOS4 IA64  ', b'FAT12   ' if fat12 else b'FAT16   ')
    volume[510:512] = b'\x55\xaa'
    fat = bytearray(spf * 512)
    def setfat(c, value):
        if fat12:
            pos = c + c//2
            old = struct.unpack_from('<H', fat, pos)[0]
            value = ((old & 15) | ((value & 0xfff) << 4)) if c & 1 else ((old & 0xf000) | (value & 0xfff))
            struct.pack_into('<H', fat, pos, value)
        else:
            struct.pack_into('<H', fat, c*2, value)
    setfat(0, 0xfff8)
    setfat(1, 0xffff)
    next_cluster = 2
    def allocate(data):
        nonlocal next_cluster
        if not data:
            return 0
        first = next_cluster
        count = (len(data) + unit - 1)//unit
        if next_cluster + count - 2 > (sectors-data_start)//spc:
            raise ValueError('image full')
        for i in range(count):
            c = next_cluster
            next_cluster += 1
            setfat(c, c+1 if i+1 < count else 0xffff)
            offset = (data_start + (c-2)*spc)*512
            chunk = data[i*unit:(i+1)*unit]
            volume[offset:offset+len(chunk)] = chunk
        return first
    def entry(name, attr, cluster, size=0):
        e = bytearray(32)
        e[:11] = name
        e[11] = attr
        struct.pack_into('<HHHI', e, 22, 0, ((2026-1980)<<9)|(9<<5)|29, cluster, size)
        return e
    tree = {'EFI': {'BOOT': {'BOOTIA64.EFI': Path(boot).read_bytes()}}}
    if media:
        def read_tree(path):
            return {p.name.upper(): read_tree(p) if p.is_dir() else p.read_bytes()
                    for p in sorted(path.iterdir()) if not p.name.startswith('.')}
        for name, data in read_tree(Path(media)).items():
            if name == 'EFI': raise ValueError('media cannot replace EFI boot directory')
            # The 8 MiB FAT12 volume holds DOS alone; Interface Manager needs a FAT16 one.
            if fat12 and name in ('WINDOWS', 'WINTEST', 'SAMPLES'): continue
            tree[name] = data
    def directory(items, parent, root=False):
        slots = len(items)+(1 if root else 2)+1
        data = bytearray((root_entries*32) if root else ((slots*32+unit-1)//unit)*unit)
        cluster = 0 if root else allocate(data)
        if root:
            data[:32] = entry(b'DOS4 IA64  ', 8, 0)
            pos = 32
        else:
            data[:32] = entry(b'.          ', 16, cluster)
            data[32:64] = entry(b'..         ', 16, parent)
            pos = 64
        for name, value in items.items():
            if isinstance(value, dict):
                c = directory(value, cluster)
                e = entry(name83(name), 16, c)
            else:
                c = allocate(value)
                e = entry(name83(name), 32, c, len(value))
            data[pos:pos+32] = e
            pos += 32
        offset = root_start*512 if root else (data_start+(cluster-2)*spc)*512
        volume[offset:offset+len(data)] = data
        return cluster
    directory(tree, 0, True)
    for i in range(2):
        volume[(1+i*spf)*512:(1+(i+1)*spf)*512] = fat
    Path(output).write_bytes(image)
    print(f'{output}: FAT{12 if fat12 else 16}, {(len(image)//1024)} KiB, {next_cluster-2} clusters used')

if __name__ == '__main__':
    build(*sys.argv[1:4], fat12='--fat12' in sys.argv[4:])
