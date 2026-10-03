#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent, read-only FAT12/16 allocation and directory consistency check."""
from pathlib import Path
import struct
import sys

def u16(b, off): return struct.unpack_from('<H', b, off)[0]
def u32(b, off): return struct.unpack_from('<I', b, off)[0]

def inspect(path):
    disk=Path(path).read_bytes()
    assert disk[510:512]==b'\x55\xaa', 'invalid MBR'
    start=u32(disk, 454)
    partition_size=u32(disk, 458)
    b=disk[start*512:(start+partition_size)*512]
    assert len(b)==partition_size*512, 'partition outside disk'
    assert b[510:512]==b'\x55\xaa' and u16(b,11)==512
    spc=b[13]; reserved=u16(b,14); copies=b[16]; entries=u16(b,17); spf=u16(b,22)
    total=u16(b,19) or u32(b,32)
    root=reserved+copies*spf
    data=root+(entries*32+511)//512
    assert spc and not (spc & (spc-1)) and total<=partition_size and total>data
    clusters=(total-data)//spc
    assert 0<clusters<65525
    bits=12 if clusters<4085 else 16
    fat=b[reserved*512:(reserved+spf)*512]
    for i in range(1,copies):
        assert fat==b[(reserved+i*spf)*512:(reserved+(i+1)*spf)*512], 'FAT copies differ'
    def link(c):
        if bits==16: return u16(fat,c*2)
        v=u16(fat,c+c//2)
        return (v>>4 if c&1 else v)&0xfff
    def eof(c): return (c==0xff0 or c>=0xff8) if bits==12 else c>=0xfff8
    owners={}
    files={}
    dirs=[]
    def chain(first,owner):
        result=[]; c=first
        while c:
            assert 2<=c<clusters+2, f'out of range cluster in {owner}: {c}'
            assert c not in owners, f'loop/cross-link in {owner}: {c}, also {owners.get(c)}'
            owners[c]=owner; result.append(c)
            nxt=link(c)
            assert nxt, f'free cluster inside chain {owner}: {c}'
            if eof(nxt): return result
            c=nxt
        return result
    def read_chain(chain):
        return b''.join(b[(data+(c-2)*spc)*512:(data+(c-1)*spc)*512] for c in chain)
    def walk(raw,prefix,own,parent):
        dirs.append(prefix)
        if own:
            assert raw[:11]==b'.          ' and u16(raw,26)==own, f'bad dot entry: {prefix}'
            assert raw[32:43]==b'..         ' and u16(raw,58)==parent, f'bad parent: {prefix}'
        names=set()
        for off in range(0,len(raw),32):
            e=raw[off:off+32]
            if not e[0]: break
            if e[0]==0xe5 or e[11]==15: continue
            if e[11]&8 or e[0]==ord('.'): continue
            name=e[:8].decode('cp437').rstrip()
            ext=e[8:11].decode('cp437').rstrip()
            if ext: name+='.'+ext
            assert name not in names, f'duplicate name {prefix}{name}'
            names.add(name)
            path=prefix+name; first=u16(e,26); size=u32(e,28)
            allocated=chain(first,path)
            contents=read_chain(allocated)
            if e[11]&16:
                assert first and size==0, f'invalid directory {path}'
                walk(contents,path+'/',first,own)
            else:
                assert len(allocated)==(size+spc*512-1)//(spc*512), f'wrong chain length {path}'
                files[path]=contents[:size]
    walk(b[root*512:root*512+entries*32],'',0,0)
    bad=0xff7 if bits==12 else 0xfff7 # Bad-cluster marks (FORMAT, RECOVER) are owned by nothing.
    lost=[c for c in range(2,clusters+2) if link(c) and link(c)!=bad and c not in owners]
    assert not lost, f'unreferenced clusters: {lost[:20]}'
    for name in ('EFI/BOOT/BOOTIA64.EFI', 'IO.SYS', 'MSDOS.SYS', 'COMMAND.COM'):
        module=files[name]
        assert module[:2]==b'MZ', name
        pe=u32(module,60)
        assert module[pe:pe+4]==b'PE\0\0' and u16(module,pe+4)==0x200, name
        assert u16(module,pe+24)==0x20b and u16(module,pe+24+68)==10, name
    for name in ('LOOPDRV.SYS','PORTDRV.SYS','RAMDRV.SYS'):
        if name not in files:
            continue
        module=files[name]; pe=u32(module,60)
        assert module[:2]==b'MZ' and module[pe:pe+4]==b'PE\0\0', name
        assert u16(module,pe+4)==0x200 and u16(module,pe+24)==0x20b and u16(module,pe+24+68)==11, name
    print(f'PASS image: FAT{bits}, {len(files)} files, {len(dirs)} directories, '
          f'{len(owners)} allocated clusters, no cross-links/orphans; IA-64 PE32+')
    return files

if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('usage: check_image.py IMAGE')
    inspect(sys.argv[1])
