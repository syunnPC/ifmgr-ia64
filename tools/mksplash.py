#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rasterize TrueType text to RLE coverage masks for WIN.COM's startup.
Each byte stores a 16-level coverage value (high nibble) and run length
minus one (low nibble). Flatten simple/composite glyf quadratic outlines;
fill by the nonzero rule with four subrows and exact horizontal coverage.
Use hmtx advances without kerning/hinting. The checked-in header contains
rendered text, not font data, so builds need no font.

Usage: mksplash.py font.ttf output.h NAME=pixels:text..."""
import struct
import sys


class Font:
    def __init__(self, path):
        self.data = data = open(path, 'rb').read()
        count = struct.unpack_from('>H', data, 4)[0]
        self.tables = {}
        for i in range(count):
            tag, _, offset, length = struct.unpack_from('>4sIII', data, 12 + 16 * i)
            self.tables[tag.decode('latin-1')] = (offset, length)
        head = self.tables['head'][0]
        self.units = struct.unpack_from('>H', data, head + 18)[0]
        self.long_loca = struct.unpack_from('>h', data, head + 50)[0] == 1
        self.glyphs = struct.unpack_from('>H', data, self.tables['maxp'][0] + 4)[0]
        hhea = self.tables['hhea'][0]
        self.ascent, self.descent = struct.unpack_from('>hh', data, hhea + 4)
        self.metrics = struct.unpack_from('>H', data, hhea + 34)[0]
        self.cmap = self.read_cmap()
        self.name = self.read_name()

    def read_name(self):
        offset = self.tables['name'][0]
        _, count, strings = struct.unpack_from('>HHH', self.data, offset)
        for i in range(count):
            platform, encoding, _, name_id, length, start = struct.unpack_from('>HHHHHH', self.data, offset + 6 + 12 * i)
            if name_id == 4 and platform == 3 and encoding == 1:
                return self.data[offset + strings + start:offset + strings + start + length].decode('utf-16-be')
        return 'unknown'

    def read_cmap(self):
        base = self.tables['cmap'][0]
        count = struct.unpack_from('>H', self.data, base + 2)[0]
        for i in range(count):
            platform, encoding, offset = struct.unpack_from('>HHI', self.data, base + 4 + 8 * i)
            table = base + offset
            if (platform, encoding) in ((3, 1), (0, 3)) and struct.unpack_from('>H', self.data, table)[0] == 4:
                break
        else:
            sys.exit('no Unicode cmap of format 4')
        segments = struct.unpack_from('>H', self.data, table + 6)[0] // 2
        ends = struct.unpack_from('>%dH' % segments, self.data, table + 14)
        starts = struct.unpack_from('>%dH' % segments, self.data, table + 16 + 2 * segments)
        deltas = struct.unpack_from('>%dh' % segments, self.data, table + 16 + 4 * segments)
        ranges_at = table + 16 + 6 * segments
        ranges = struct.unpack_from('>%dH' % segments, self.data, ranges_at)
        cmap = {}
        for s in range(segments):
            for code in range(starts[s], min(ends[s], 0xfffe) + 1):
                if code > 0x24f:
                    break
                if ranges[s]:
                    at = ranges_at + 2 * s + ranges[s] + 2 * (code - starts[s])
                    glyph = struct.unpack_from('>H', self.data, at)[0]
                    glyph = (glyph + deltas[s]) & 0xffff if glyph else 0
                else:
                    glyph = (code + deltas[s]) & 0xffff
                cmap[code] = glyph
        return cmap

    def advance(self, glyph):
        hmtx = self.tables['hmtx'][0]
        return struct.unpack_from('>H', self.data, hmtx + 4 * min(glyph, self.metrics - 1))[0]

    def location(self, glyph):
        loca = self.tables['loca'][0]
        if self.long_loca:
            return struct.unpack_from('>II', self.data, loca + 4 * glyph)
        a, b = struct.unpack_from('>HH', self.data, loca + 2 * glyph)
        return a * 2, b * 2

    def contours(self, glyph):
        """The glyph's contours: lists of (x, y, on-curve) in font units."""
        start, end = self.location(glyph)
        if start == end:
            return []
        at = self.tables['glyf'][0] + start
        count = struct.unpack_from('>h', self.data, at)[0]
        if count < 0:
            return self.composite(at + 10)
        ends = struct.unpack_from('>%dH' % count, self.data, at + 10)
        p = at + 10 + 2 * count
        p += 2 + struct.unpack_from('>H', self.data, p)[0]
        points = ends[-1] + 1
        flags = []
        while len(flags) < points:
            f = self.data[p]; p += 1
            flags.append(f)
            if f & 8:
                flags.extend([f] * self.data[p]); p += 1
        coords = []
        for short, same in ((2, 16), (4, 32)):
            v, values = 0, []
            for f in flags:
                if f & short:
                    d = self.data[p]; p += 1
                    v += d if f & same else -d
                elif not f & same:
                    v += struct.unpack_from('>h', self.data, p)[0]; p += 2
                values.append(v)
            coords.append(values)
        result, first = [], 0
        for last in ends:
            result.append([(coords[0][i], coords[1][i], flags[i] & 1) for i in range(first, last + 1)])
            first = last + 1
        return result

    def composite(self, p):
        result = []
        while True:
            flags, glyph = struct.unpack_from('>HH', self.data, p); p += 4
            if flags & 1:
                dx, dy = struct.unpack_from('>hh', self.data, p); p += 4
            else:
                dx, dy = struct.unpack_from('>bb', self.data, p); p += 2
            a, b, c, d = 1.0, 0.0, 0.0, 1.0
            if flags & 8:
                a = d = struct.unpack_from('>h', self.data, p)[0] / 16384; p += 2
            elif flags & 0x40:
                a, d = (v / 16384 for v in struct.unpack_from('>hh', self.data, p)); p += 4
            elif flags & 0x80:
                a, b, c, d = (v / 16384 for v in struct.unpack_from('>hhhh', self.data, p)); p += 8
            if not flags & 2:
                dx = dy = 0  # point matching: not used by the faces this renders
            for contour in self.contours(glyph):
                result.append([(x * a + y * c + dx, x * b + y * d + dy, on) for x, y, on in contour])
            if not flags & 0x20:
                return result


def outline(contour, steps=8):
    """A closed contour as a polygon: quadratic curves flattened."""
    n = len(contour)
    start = next((i for i in range(n) if contour[i][2]), None)
    if start is None:  # all off-curve: begin between the first two
        x0, y0 = (contour[0][0] + contour[1][0]) / 2, (contour[0][1] + contour[1][1]) / 2
        pts = [(x0, y0, 1)] + contour[1:] + contour[:1]
    else:
        pts = contour[start:] + contour[:start]
    pts = pts + [pts[0]]
    poly, current, control = [], (pts[0][0], pts[0][1]), None
    poly.append(current)
    for x, y, on in pts[1:]:
        if on:
            if control is None:
                poly.append((x, y))
            else:
                poly.extend(curve(current, control, (x, y), steps))
                control = None
            current = (x, y)
        else:
            if control is not None:
                middle = ((control[0] + x) / 2, (control[1] + y) / 2)
                poly.extend(curve(current, control, middle, steps))
                current = middle
            control = (x, y)
    return poly


def curve(a, b, c, steps):
    return [((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0],
             (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1])
            for t in (i / steps for i in range(1, steps + 1))]


def render(font, text, pixels):
    """The line's coverage (rows of 0..1), its width and height."""
    scale = pixels / font.units
    ascent, descent = font.ascent * scale, -font.descent * scale
    height = int(ascent + descent + 2)
    edges, pen = [], 1.0
    for ch in text:
        glyph = font.cmap.get(ord(ch), 0)
        for contour in font.contours(glyph):
            poly = [(pen + x * scale, ascent - y * scale) for x, y in outline(contour)]
            for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
                if y0 != y1:
                    edges.append((x0, y0, x1, y1))
        pen += font.advance(glyph) * scale
    width = int(pen + 2)
    rows = [[0.0] * width for _ in range(height)]
    for sub in range(height * 4):
        y = (sub + 0.5) / 4
        crossings = []
        for x0, y0, x1, y1 in edges:
            if min(y0, y1) <= y < max(y0, y1):
                crossings.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
        crossings.sort()
        winding, row = 0, rows[sub // 4]
        for i, (x, direction) in enumerate(crossings):
            before = winding
            winding += direction
            if before == 0 and winding != 0:
                left = x
            elif before != 0 and winding == 0:
                for px in range(max(0, int(left)), min(width, int(x) + 1)):
                    overlap = min(x, px + 1) - max(left, px)
                    if overlap > 0:
                        row[px] += overlap / 4
    return rows, width, height


def runs(rows):
    out = bytearray()
    flat = [min(15, int(v * 15 + 0.5)) for row in rows for v in row]
    i = 0
    while i < len(flat):
        n = 1
        while i + n < len(flat) and n < 16 and flat[i + n] == flat[i]:
            n += 1
        out.append(flat[i] << 4 | (n - 1))
        i += n
    return out


def main(font_path, output, *lines):
    font = Font(font_path)
    with open(output, 'w', encoding='ascii') as out:
        out.write('/* SPDX-License-Identifier: GPL-2.0-or-later\n'
                  ' * Generated by tools/mksplash.py from %s (SIL Open Font License 1.1):\n' % font.name)
        out.write(' * rendered lines of text, no font data. Each is a width, a height and\n'
                  ' * runs of 16-level coverage (the level in the high nibble, the run less\n'
                  ' * one in the low), row by row. To change one, run the tool again:\n'
                  ' *   tools/mksplash.py LiberationSerif-Regular.ttf %s %s */\n'
                  % (output, ' '.join("'%s'" % l for l in lines)))
        for line in lines:
            name, _, rest = line.partition('=')
            size, _, text = rest.partition(':')
            rows, width, height = render(font, text, int(size))
            data = runs(rows)
            out.write('/* "%s" at %s pixels */\n' % (text, size))
            out.write('static const unsigned char %s_runs[%d]={\n' % (name, len(data)))
            for i in range(0, len(data), 24):
                out.write('    %s,\n' % ','.join('0x%02x' % b for b in data[i:i + 24]))
            out.write('};\n')
            out.write('#define %s_WIDTH %d\n#define %s_HEIGHT %d\n' % (name.upper(), width, name.upper(), height))


if __name__ == '__main__':
    main(*sys.argv[1:])
