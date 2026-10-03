#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later AND MIT
"""Build DOS 4 COUNTRY.SYS from Microsoft's vendored data declarations.

The generated data is Copyright (c) Microsoft Corporation, MIT License
(vendor/msdos4/LICENSE); this generator is GPL-2.0-or-later."""
import ast
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'vendor/msdos4'


def strip_comment(line):
    quote = None
    for i, char in enumerate(line):
        if quote:
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == ';':
            return line[:i].strip()
    if quote:
        raise ValueError(f'unterminated string: {line}')
    return line.strip()


def fields(text):
    result, start, quote, depth = [], 0, None, 0
    for i, char in enumerate(text):
        if quote:
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
        elif char == ',' and not depth:
            result.append(text[start:i].strip())
            start = i + 1
    if quote or depth:
        raise ValueError(f'unbalanced initializer: {text}')
    return result + [text[start:].strip()]


def literal(text):
    if len(text) >= 2 and text[0] in "\"'" and text[-1] == text[0]:
        return text[1:-1].encode('ascii')
    return None


class DataAssembler:
    """Only the data syntax used in MKCNTRY is accepted; no x86 code runs."""
    def __init__(self):
        self.constants = {}
        self.structures = {}
        self.labels = {}
        self.fixups = []
        self.output = bytearray()
        self.countries = 0

    def value(self, text, here=0, active=()):
        text = text.lower().strip()
        text = re.sub(r'\boffset\s+', '', text)
        text = re.sub(r'\btype\s+(\w+)', lambda m: str(sum(f[1] for f in self.structures[m[1]])), text)
        text = text.replace('$', str(here))
        text = re.sub(r'\b([0-9][0-9a-f]*)h\b', lambda m: str(int(m[1], 16)), text)
        text = re.sub(r'\b[0-9]+\b', lambda m: str(int(m[0], 10)), text)

        def evaluate(node):
            if isinstance(node, ast.Constant) and type(node.value) is int:
                return node.value
            if isinstance(node, ast.Name):
                if node.id in self.labels:
                    return self.labels[node.id]
                if node.id in active:
                    raise ValueError(f'cyclic constant: {node.id}')
                return self.value(self.constants[node.id], here, active + (node.id,))
            if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub)):
                a, b = evaluate(node.left), evaluate(node.right)
                return a + b if isinstance(node.op, ast.Add) else a - b
            if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
                return -evaluate(node.operand)
            raise ValueError(f'unsupported data expression: {text}')

        return evaluate(ast.parse(text, mode='eval').body)

    def declarations(self, path):
        current = None
        for raw in path.read_text().splitlines():
            line = strip_comment(raw)
            match = re.fullmatch(r'(\w+)\s+struc', line, re.I)
            if match:
                current = []
                self.structures[match[1].lower()] = current
            elif re.fullmatch(r'\w+\s+ends', line, re.I):
                current = None
            elif match := re.fullmatch(r'(\w+)\s+equ\s+(.+)', line, re.I):
                self.constants[match[1].lower()] = match[2]
            elif current is not None and line:
                match = re.fullmatch(r'(?:\w+\s+)?(db|dw|dd)\s+(.+)', line, re.I)
                if not match:
                    raise ValueError(f'unsupported structure field: {line}')
                unit = {'db': 1, 'dw': 2, 'dd': 4}[match[1].lower()]
                default = match[2]
                if repeat := re.fullmatch(r'(\w+)\s+dup\s*\((.+)\)', default, re.I):
                    width, default = unit * self.value(repeat[1]), repeat[2]
                else:
                    data = literal(default)
                    width = len(data) if data is not None else unit
                current.append((unit, width, default))

    def emit(self, expression, unit, width=None):
        data = literal(expression)
        if width is None:
            width = len(data) if data is not None else unit
        if data is not None:
            if len(data) > width:
                raise ValueError(f'oversized string: {expression}')
            self.output.extend(data.ljust(width, b'\0'))
        else:
            if width % unit:
                raise ValueError('unaligned structure field')
            for _ in range(width // unit):
                self.fixups.append((len(self.output), unit, expression))
                self.output.extend(b'\0' * unit)

    def directive(self, operation, arguments):
        if operation in ('db', 'dw', 'dd'):
            unit = {'db': 1, 'dw': 2, 'dd': 4}[operation]
            for item in fields(arguments):
                if repeat := re.fullmatch(r'(\w+)\s+dup\s*\((.+)\)', item, re.I):
                    for _ in range(self.value(repeat[1])):
                        self.emit(repeat[2], unit)
                else:
                    self.emit(item, unit)
        elif operation in self.structures or operation == 'ctryent':
            name = 'ctrystr' if operation == 'ctryent' else operation
            if not arguments.startswith('<') or not arguments.endswith('>'):
                raise ValueError(f'invalid structure initializer: {arguments}')
            values = fields(arguments[1:-1])
            description = self.structures[name]
            if len(values) > len(description):
                raise ValueError(f'too many fields in {name}')
            for i, (unit, width, default) in enumerate(description):
                self.emit(values[i] if i < len(values) and values[i] else default, unit, width)
            if operation == 'ctryent':
                self.countries += 1
        else:
            raise ValueError(f'unsupported data directive: {operation}')

    def assemble(self, source):
        self.declarations(source / 'MKCNTRY.INC')
        active, macro = False, False
        for raw in (source / 'MKCNTRY.ASM').read_text().splitlines():
            line = strip_comment(raw)
            if re.fullmatch(r'cdinfo\s+label\s+word', line, re.I):
                active = True
            if not active or not line:
                continue
            if re.fullmatch(r'\w+\s+macro\s+.*', line, re.I):
                macro = True
                continue
            if line.lower() == 'endm':
                macro = False
                continue
            if macro or line.lower() in ('page', 'cntrycnt=0'):
                continue
            if re.fullmatch(r'dummy\s+%cntrycnt', line, re.I):
                self.constants['finalcnt'] = str(self.countries)
                continue
            if line.lower() == 'include copyrigh.inc':
                for copyright_line in (source / 'COPYRIGH.INC').read_text().splitlines():
                    op, args = strip_comment(copyright_line).split(None, 1)
                    self.directive(op.lower(), args)
                break
            if match := re.fullmatch(r'(\w+)\s+(?:label\s+\w+|equ\s+\$)', line, re.I):
                self.labels[match[1].lower()] = len(self.output)
                continue
            if match := re.fullmatch(r'(\w+)\s+(db|dw|dd)\s+(.+)', line, re.I):
                self.labels[match[1].lower()] = len(self.output)
                self.directive(match[2].lower(), match[3])
            else:
                op, args = line.split(None, 1)
                self.directive(op.lower(), args)
        for offset, width, expression in self.fixups:
            value = self.value(expression, offset)
            if not 0 <= value < 1 << (8 * width):
                raise ValueError(f'data overflow: {expression} = {value}')
            self.output[offset:offset + width] = value.to_bytes(width, 'little')
        return bytes(self.output)


def country_records(data):
    """Validate the emitted directory and every referenced country table."""
    if data[:8] != b'\xffCOUNTRY' or len(data) > 65535:
        raise ValueError('invalid country header')
    def word(offset):
        return struct.unpack_from('<H', data, offset)[0]
    def pointer(offset):
        return struct.unpack_from('<I', data, offset)[0]
    if word(16) != 1 or data[18] != 1:
        raise ValueError('unsupported country directory')
    directory = pointer(19)
    count = word(directory)
    records = []
    tags = {1: b'CTYINFO', 2: b'UCASE  ', 4: b'FUCASE ', 5: b'FCHAR  ', 6: b'COLLATE', 7: b'DBCS   '}
    for i in range(count):
        entry = directory + 2 + i * 14
        if word(entry) != 12:
            raise ValueError('invalid country entry')
        country, page = word(entry + 2), word(entry + 4)
        tables_at = pointer(entry + 10)
        tables = {}
        for j in range(word(tables_at)):
            item = tables_at + 2 + j * 8
            kind, target = data[item + 2], pointer(item + 4)
            if word(item) != 6 or kind not in tags or kind in tables:
                raise ValueError('invalid table entry')
            tag = data[target:target + 8]
            allowed = [b'\xff' + tags[kind]]
            if kind == 4:
                allowed.append(b'\xffUCASE  ')
            if tag not in allowed:
                raise ValueError(f'invalid table signature: {tag!r}')
            length = word(target + 8)
            if target + 10 + length > len(data):
                raise ValueError('table outside file')
            table = data[target + 10:target + 10 + length]
            if kind in (1, 2, 4, 6) and length != {1: 38, 2: 128, 4: 128, 6: 256}[kind]:
                raise ValueError(f'invalid table size: {kind}, {length}')
            if kind == 7 and (length % 2 or (length and table[-2:] != b'\0\0')):
                raise ValueError('invalid DBCS table')
            tables[kind] = table
        if set(tables) != set(tags):
            raise ValueError('incomplete country tables')
        records.append((country, page, tables))
    if len({(c, p) for c, p, _ in records}) != count:
        raise ValueError('duplicate country/code page')
    return records


def build(destination, header=None):
    data = DataAssembler().assemble(SOURCE)
    records = country_records(data)
    Path(destination).write_bytes(data)
    if header:
        lines = ['/* Generated from the MIT-licensed DOS 4 country data. */',
                 'static const u8 builtin_country[] = {']
        lines += ['    ' + ','.join(str(b) for b in data[i:i + 24]) + ','
                  for i in range(0, len(data), 24)]
        Path(header).write_text('\n'.join(lines + ['};', '']), encoding='ascii')
    print(f'{destination}: {len(data)} bytes, {len(records)} country/code-page pairs')


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
