#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert BDF or FontForge SFD bitmap strikes to GDI C tables.
Output: 256 glyphs, 32-bit rows (MSB at pen position), advances and metrics.
Decode Wine SFD BDFChar Ascii85 data; map cp1252 (or OEM cp437) to Unicode,
using the default glyph for missing characters. SFD average/default metrics
follow Wine's '#pragma makedep font'; leading is ascent minus M height
(sfnt2fon convention, with Courier=0 and Fixedsys=3).

Usage: mkfont.py source output NAME [pixel-size [code-page]]"""
import base64
import re
import sys

def parse(path):
    """A BDF font: its properties and glyphs by encoding."""
    props, glyphs, glyph = {}, {}, None
    with open(path, encoding='ascii') as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        key, _, rest = line.partition(' ')
        if key in ('FONT_ASCENT', 'FONT_DESCENT'):
            props[key] = int(rest)
        elif key == 'FONTBOUNDINGBOX':
            props[key] = [int(v) for v in rest.split()]
        elif key == 'STARTCHAR':
            glyph = {}
        elif key == 'ENCODING':
            glyph['code'] = int(rest.split()[0])
        elif key == 'BBX':
            glyph['bbx'] = [int(v) for v in rest.split()]
        elif key == 'BITMAP':
            rows = []
            for row in lines:
                if row == 'ENDCHAR':
                    break
                rows.append(int(row, 16))
            glyph['rows'] = rows
            glyphs[glyph['code']] = glyph
    return props, glyphs

def bdf_font(path):
    props, glyphs = parse(path)
    width, height = props['FONTBOUNDINGBOX'][:2]
    ascent, descent = props['FONT_ASCENT'], props['FONT_DESCENT']
    assert width <= 32 and ascent + descent == height, (width, ascent, descent)
    out = {}
    for code, g in glyphs.items():
        w, h, x, y = g['bbx']
        row_bytes = (w + 7) // 8
        rows = {}
        for i, bits in enumerate(g['rows']):
            rows[ascent - (y + h) + i] = (bits << (32 - 8 * row_bytes)) >> x & 0xffffffff
        out[code] = (width, rows)
    return {'height': height, 'ascent': ascent, 'leading': 0, 'avg': width, 'default': ord('?'),
            'glyphs': out, 'source': path.split('/')[-1], 'licence': 'public domain glyphs'}

def sfd_font(path, size):
    text = open(path, encoding='latin-1').read()
    family = re.search(r'^FamilyName: (.*)$', text, re.M).group(1)
    m = re.search(r'^BitmapFont: %d \d+ (\d+) (\d+) 1' % size, text, re.M)
    assert m, (path, size)
    ascent, descent = int(m.group(1)), int(m.group(2))
    strike = text[m.end():text.index('EndBitmapFont', m.end())]
    avg, default = None, ord('?')
    for pragma in re.findall(r'#pragma makedep font [^"+]*', text):
        p = re.search(r'\b%d,1252,(\d+)' % size, pragma)
        if p:
            avg = int(p.group(1))
            d = re.search(r'-d (\d+)', pragma)
            if d:
                default = ord(bytes([int(d.group(1))]).decode('cp1252', 'replace'))
            break
    glyphs = {}
    for record in re.split(r'\nBDFChar: ', strike)[1:]:
        head, _, data = record.partition('\n')
        _, code, width, xmin, xmax, ymin, ymax = [int(v) for v in head.split()[:7]]
        row_bytes = (xmax - xmin + 1 + 7) // 8
        count = ymax - ymin + 1
        raw = base64.a85decode(''.join(data.split()).encode('ascii'))[:row_bytes * count]
        rows = {}
        for i in range(count):
            value = int.from_bytes(raw[i * row_bytes:(i + 1) * row_bytes].ljust(row_bytes, b'\0'), 'big')
            rows[ascent - 1 - (ymax - i)] = (value << (32 - 8 * row_bytes)) >> xmin & 0xffffffff
        glyphs[code] = (width, rows)
    if family == 'Courier':
        leading = 0
    elif family == 'Fixedsys':
        leading = 3
    else:
        mrows = [r for r, v in glyphs[ord('M')][1].items() if v]
        leading = ascent - (ascent - min(mrows))
    return {'height': ascent + descent, 'ascent': ascent, 'leading': leading, 'avg': avg, 'default': default,
            'glyphs': glyphs, 'source': path.split('/')[-1], 'licence': 'LGPL 2.1 or later, from Wine'}

def main(source, output, name, size=None, codepage='cp1252'):
    font = sfd_font(source, int(size)) if source.endswith('.sfd') else bdf_font(source)
    height, glyphs = font['height'], font['glyphs']
    fallback = glyphs.get(font['default']) or glyphs.get(ord('?'))
    cells, widths = [], []
    for code in range(256):
        try:
            point = ord(bytes([code]).decode(codepage))
        except UnicodeDecodeError:
            point = None
        glyph = glyphs.get(point) if point is not None and (code >= 0x20 or codepage != 'cp1252') else None
        if not glyph and code >= 0x20:
            glyph = fallback
        width, rows = glyph if glyph else (font['avg'] or fallback[0], {})
        cells.append([rows.get(r, 0) for r in range(height)])
        widths.append(width)
    avg = font['avg'] or widths[ord('x')]
    with open(output, 'w', encoding='ascii') as out:
        out.write('/* Generated by tools/mkfont.py from %s (%s). */\n' % (font['source'], font['licence']))
        out.write('#define %s_HEIGHT %d\n#define %s_ASCENT %d\n#define %s_LEADING %d\n#define %s_AVG %d\n#define %s_MAX %d\n'
                  % (name, height, name, font['ascent'], name, font['leading'], name, avg, name, max(widths)))
        out.write('static const unsigned char %s_widths[256]={\n' % name.lower())
        for base in range(0, 256, 16):
            out.write('    %s,\n' % ','.join(str(w) for w in widths[base:base + 16]))
        out.write('};\n')
        out.write('static const unsigned int %s_bits[256][%d]={\n' % (name.lower(), height))
        for code, cell in enumerate(cells):
            out.write('    {%s}, /* %02x */\n' % (','.join('0x%08x' % v for v in cell), code))
        out.write('};\n')

if __name__ == '__main__':
    main(*sys.argv[1:])
