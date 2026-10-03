#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the ISO 9660 test image for tests/cdfs.c with genisoimage (or
xorriso); mkisofs() also makes the QEMU tests' CD images."""
import shutil, subprocess, sys
from pathlib import Path

def mkisofs(tree, output, label):
    """An ISO 9660 image of a directory, by genisoimage or mkisofs, else xorriso."""
    tool = shutil.which('genisoimage') or shutil.which('mkisofs')
    command = [tool] if tool else ['xorriso', '-as', 'mkisofs']
    subprocess.run(command+['-quiet', '-V', label, '-o', str(output), str(tree)], check=True)

def main(output):
    tree = Path(output).with_suffix('.tree')
    if tree.exists(): shutil.rmtree(tree)
    for d in ('WIN/SUB/DEEP', 'MANY', 'EMPTY'):
        (tree/d).mkdir(parents=True)
    (tree/'README.TXT').write_bytes(b'WINDOWS SETUP CD\r\n')
    (tree/'WIN/SETUP.EXE').write_bytes(bytes((i*7+3) & 255 for i in range(5000)))
    (tree/'WIN/SUB/DEEP/FILE.TXT').write_bytes(b'deep file')
    (tree/'lower.txt').write_bytes(b'lower')
    for i in range(120):
        (tree/f'MANY/F{i:03d}.TXT').write_bytes(b'%d' % i)
    mkisofs(tree, output, 'WIN30')

if __name__ == '__main__':
    main(sys.argv[1])
