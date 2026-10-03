#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compress to Microsoft COMPRESS's SZDD format.
Header: signature, A, original last filename character, expanded size.
LZSS uses a 4096-byte space-filled window starting at 4080. Flag bits are
LSB first: 1=literal, 0=12-bit position plus 4-bit length (3-18 bytes).
Matches may overlap their own output.

Usage: mkszdd.py input output"""
import os
import struct
import sys

def compress(data):
    window = bytearray(b' ' * 4096)
    pos = 4096 - 16
    out = bytearray()
    i = 0
    while i < len(data):
        flags, items = 0, bytearray()
        for bit in range(8):
            if i >= len(data):
                break
            best_len, best_at = 0, 0
            for at in range(4096):
                if window[at] != data[i]:
                    continue
                written, w, n = {}, pos, 0
                while n < 18 and i + n < len(data):
                    b = written.get((at + n) & 4095, window[(at + n) & 4095])
                    if b != data[i + n]:
                        break
                    written[w] = b
                    w = (w + 1) & 4095
                    n += 1
                if n > best_len:
                    best_len, best_at = n, at
                    if n == 18:
                        break
            if best_len >= 3:
                items += bytes([best_at & 0xff, (best_at >> 4) & 0xf0 | (best_len - 3)])
                count = best_len
            else:
                flags |= 1 << bit
                items.append(data[i])
                count = 1
            for k in range(count):
                window[pos] = data[i + k]
                pos = (pos + 1) & 4095
            i += count
        out.append(flags)
        out += items
    return bytes(out)

def main(source, output):
    data = open(source, 'rb').read()
    last = os.path.basename(source).upper()[-1:].encode('ascii') or b'\0'
    with open(output, 'wb') as f:
        f.write(b'SZDD\x88\xf0\x27\x33A' + last + struct.pack('<I', len(data)) + compress(data))

if __name__ == '__main__':
    main(*sys.argv[1:])
