#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert 32x32 ASCII art to ICO, BMP or CUR.
VGA colors (one character per pixel):
  K black  R maroon  G green  Y olive  B navy  M purple  C teal  L silver
  D gray   r red     g lime   y yellow b blue  m fuchsia c aqua  W white
Space/dot is transparent; # lines are comments. BMP replaces transparency
with black. CUR is monochrome (light colors white) and reads # hotspot x y."""
import struct, sys

PALETTE = 'KRGYBMCLDrgybmcW'
VGA = [(0,0,0),(128,0,0),(0,128,0),(128,128,0),(0,0,128),(128,0,128),(0,128,128),(192,192,192),
       (128,128,128),(255,0,0),(0,255,0),(255,255,0),(0,0,255),(255,0,255),(0,255,255),(255,255,255)]

def cursor(rows, hotspot, output):
    xor, mask = b'', b''
    for row in reversed(rows):
        bits = solid = 0
        for c in row:
            r, g, b = VGA[PALETTE.index(c)] if c in PALETTE else (0, 0, 0)
            bits = bits*2+(1 if c in PALETTE and r+g+b >= 384 else 0)
            solid = solid*2+(0 if c in PALETTE else 1)
        xor += bits.to_bytes(4, 'big'); mask += solid.to_bytes(4, 'big')
    header = struct.pack('<IiiHHIIiiII', 40, 32, 64, 1, 1, 0, len(xor)+len(mask), 0, 0, 2, 0)
    image = header+struct.pack('<II', 0, 0xffffff)+xor+mask
    directory = struct.pack('<HHH', 0, 2, 1)+struct.pack('<BBBBHHII', 32, 32, 2, 0, hotspot[0], hotspot[1], len(image), 22)
    open(output, 'wb').write(directory+image)

def main(source, output):
    lines = open(source, encoding='ascii').read().splitlines()
    hotspot = next((tuple(int(v) for v in l.split()[2:4]) for l in lines if l.startswith('# hotspot ')), (0, 0))
    rows = [l.rstrip('\n') for l in lines if not l.startswith('#')]
    rows = [(r+' '*32)[:32] for r in rows[:32]] + [' '*32]*(32-len(rows[:32]))
    if output.lower().endswith('.cur'):
        return cursor(rows, hotspot, output)
    xor, mask = b'', b''
    for row in reversed(rows):           # bottom-up, as in a DIB
        pixels = [PALETTE.index(c) if c in PALETTE else 0 for c in row]
        xor += bytes(pixels[i]*16+pixels[i+1] for i in range(0, 32, 2))
        bits = 0
        for c in row:
            bits = bits*2+(0 if c in PALETTE else 1)
        mask += bits.to_bytes(4, 'big')
    palette = b''.join(struct.pack('<BBBB', b, g, r, 0) for r, g, b in VGA)
    if output.lower().endswith('.bmp'):
        info = struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 4, 0, len(xor), 0, 0, 16, 0)+palette
        open(output, 'wb').write(struct.pack('<2sIHHI', b'BM', 14+len(info)+len(xor), 0, 0, 14+len(info))+info+xor)
        return
    header = struct.pack('<IiiHHIIiiII', 40, 32, 64, 1, 4, 0, len(xor)+len(mask), 0, 0, 16, 0)
    image = header+palette+xor+mask
    directory = struct.pack('<HHH', 0, 1, 1)+struct.pack('<BBBBHHII', 32, 32, 16, 0, 1, 4, len(image), 22)
    open(output, 'wb').write(directory+image)

if __name__ == '__main__':
    main(*sys.argv[1:])
