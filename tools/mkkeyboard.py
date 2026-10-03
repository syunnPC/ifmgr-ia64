#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble KEYBOARD.SYS from MIT-licensed DOS 4 keyboard sources.
Support DB/DW/DD, labels, EQUs, PUBLIC/EXTRN and KEYBMAC.INC state macros.
Link modules in KEYBOARD.LNK order, paragraph-aligned, matching LINK/EXE2BIN.

Usage: mkkeyboard.py output.sys include_dir module.asm... [--add CODE:[ID]:module.asm]...
--add extends KDFNOW.ASM's language/ID tables via CODE_LANG_ENT and inserts
the module before KDFEOF.ASM."""
import re, sys
from pathlib import Path

FLAG_IDS = {}
for name in ('SCAN_MATCH', 'EITHER_SHIFT', 'EITHER_CTL', 'EITHER_ALT'): FLAG_IDS[name] = 'EXT_KB_FLAG_ID'
for name in ('CAPS_STATE', 'NUM_STATE', 'LEFT_SHIFT', 'RIGHT_SHIFT', 'ALT_SHIFT', 'CTL_SHIFT'): FLAG_IDS[name] = 'KB_FLAG_ID'
for name in ('R_ALT_SHIFT', 'R_CTL_SHIFT', 'LC_E0'): FLAG_IDS[name] = 'KB_FLAG_3_ID'
for name in ('TILDE', 'ACUTE', 'GRAVE', 'DIARESIS', 'CEDILLA', 'CIRCUMFLEX'): FLAG_IDS[name] = 'NLS_FLAG_1_ID'


class Error(Exception):
    pass


def strip_comment(line):
    out, quote = [], None
    for c in line:
        if quote:
            out.append(c)
            if c == quote: quote = None
        elif c in '\'"':
            quote = c; out.append(c)
        elif c == ';':
            break
        else:
            out.append(c)
    return ''.join(out).rstrip()


def split_operands(text):
    """Comma-separated operands, commas inside quotes or parentheses kept."""
    parts, cur, quote, depth = [], [], None, 0
    for c in text:
        if quote:
            cur.append(c)
            if c == quote: quote = None
        elif c in '\'"':
            quote = c; cur.append(c)
        elif c == '(':
            depth += 1; cur.append(c)
        elif c == ')':
            depth -= 1; cur.append(c)
        elif c == ',' and depth == 0:
            parts.append(''.join(cur).strip()); cur = []
        else:
            cur.append(c)
    if cur or parts: parts.append(''.join(cur).strip())
    return [p for p in parts]


TOKEN = re.compile(r"\s*(?:(?P<num>[0-9][0-9A-Fa-f]*[HhBb]?)|(?P<str>'[^']*'|\"[^\"]*\")|(?P<name>[A-Za-z_$?@][A-Za-z0-9_$?@]*)|(?P<op>[-+*/()]))")


class Module:
    def __init__(self, path, include_dir, equs):
        self.path, self.include_dir, self.equs = path, include_dir, equs
        self.labels, self.local_equs, self.publics, self.externs = {}, {}, set(), set()
        self.items = []   # (offset, kind, payload, line)
        self.size = 0

    # --- source ---------------------------------------------------------
    def lines(self, path=None, depth=0):
        path = path or self.path
        text = Path(path).read_bytes().decode('latin-1').replace('\r', '')
        in_macro = in_struc = False
        for number, raw in enumerate(text.split('\n'), 1):
            line = strip_comment(raw.replace('\x1a', ''))
            if not line.strip(): continue
            words = line.split()
            upper = [w.upper() for w in words]
            if in_macro:
                if upper[0] == 'ENDM': in_macro = False
                continue
            if in_struc:
                if len(upper) > 1 and upper[1] == 'ENDS': in_struc = False
                continue
            if len(upper) > 1 and upper[1] == 'MACRO': in_macro = True; continue
            if len(upper) > 1 and upper[1] == 'STRUC': in_struc = True; continue
            if upper[0] == 'INCLUDE':
                name = words[1]
                found = [p for d in (Path(self.include_dir), Path(self.include_dir).parent)
                         for p in d.iterdir() if p.name.upper() == name.upper()]
                if not found: raise Error(f'{path}:{number}: no include {name}')
                yield from self.lines(found[0], depth + 1)
                continue
            yield path, number, line

    # --- expressions ----------------------------------------------------
    def value(self, text, here, symbols, where):
        tokens, pos = [], 0
        text = text.strip()
        if text.upper().startswith('OFFSET '): text = text[7:]
        while pos < len(text):
            m = TOKEN.match(text, pos)
            if not m or m.end() == pos: raise Error(f'{where}: cannot read {text!r}')
            pos = m.end()
            if m.group('num') is not None:
                n = m.group('num')
                if n[-1] in 'Hh': v = int(n[:-1], 16)
                elif n[-1] in 'Bb' and re.fullmatch(r'[01]+[Bb]', n): v = int(n[:-1], 2)
                else: v = int(n, 10)
                tokens.append(v)
            elif m.group('str') is not None:
                s = m.group('str')[1:-1]
                if len(s) != 1: raise Error(f'{where}: string {s!r} in an expression')
                tokens.append(ord(s))
            elif m.group('name') is not None:
                name = m.group('name').upper()
                if name == '$': tokens.append(here)
                elif name == 'OFFSET': continue
                else: tokens.append(('sym', name))
            else:
                tokens.append(m.group('op'))
        def resolve(t):
            if isinstance(t, tuple):
                v = symbols(t[1])
                if v is None: raise Error(f'{where}: unknown symbol {t[1]}')
                return v
            return t
        # Recursive descent: sum of products of unary terms.
        def term(i):
            t = tokens[i]
            if t == '-': v, i = term(i + 1); return -v, i
            if t == '+': return term(i + 1)
            if t == '(':
                v, i = expr(i + 1)
                if tokens[i] != ')': raise Error(f'{where}: ) expected')
                return v, i + 1
            return resolve(t), i + 1
        def product(i):
            v, i = term(i)
            while i < len(tokens) and tokens[i] in ('*', '/'):
                w, j = term(i + 1)
                v = v * w if tokens[i] == '*' else v // w
                i = j
            return v, i
        def expr(i):
            v, i = product(i)
            while i < len(tokens) and tokens[i] in ('+', '-'):
                w, j = product(i + 1)
                v = v + w if tokens[i] == '+' else v - w
                i = j
            return v, i
        v, i = expr(0)
        if i != len(tokens): raise Error(f'{where}: extra text in {text!r}')
        return v

    # --- statements -----------------------------------------------------
    def data_items(self, kind, operands, where):
        """Sizes of DB/DW/DD operands: [(size, text)] with strings split."""
        out = []
        width = {'DB': 1, 'DW': 2, 'DD': 4}[kind]
        for op in split_operands(operands):
            m = re.fullmatch(r'(.+?)\s+DUP\s*\((.*)\)', op, re.I)
            if m:
                count = self.value(m.group(1), 0, lambda n: self.local_equs.get(n, self.equs.get(n)), where)
                for _ in range(count): out.append((width, m.group(2)))
                continue
            if width == 1 and len(op) >= 2 and op[0] in '\'"' and op[-1] == op[0] and len(op) > 3:
                for c in op[1:-1]: out.append((1, repr(c) if c != "'" else '"\'"'))
                continue
            out.append((width, op))
        return out

    def statement(self, line, where, offset):
        """(size, payload) for one statement; labels recorded."""
        m = re.match(r'\s*([A-Za-z_$?@][A-Za-z0-9_$?@]*)\s*:\s*(.*)$', line)
        if m:
            self.labels[m.group(1).upper()] = offset
            line = m.group(2)
            if not line.strip(): return 0, None
        line = re.sub(r'^(\s*[A-Za-z_][A-Za-z0-9_]*)\s*=\s*', r'\1 = ', line)
        words = line.split(None, 1)
        head = words[0].upper()
        rest = words[1] if len(words) > 1 else ''
        if len(words) > 1:
            second = rest.split(None, 1)
            if second[0].upper() in ('EQU', '='):
                self.local_equs[head] = ('expr', second[1] if len(second) > 1 else '0', where)
                return 0, None
            if second[0].upper() in ('SEGMENT', 'ENDS', 'LABEL', 'PROC', 'ENDP'):
                if second[0].upper() == 'LABEL': self.labels[head] = offset
                return 0, None
        if head in ('PAGE', 'TITLE', 'ASSUME', 'END', '.XLIST', '.LIST', 'EVEN', 'NAME'):
            return 0, None
        if head == 'PUBLIC':
            for n in split_operands(rest): self.publics.add(n.split(':')[0].strip().upper())
            return 0, None
        if head == 'EXTRN':
            for n in split_operands(rest): self.externs.add(n.split(':')[0].strip().upper())
            return 0, None
        if head in ('DB', 'DW', 'DD'):
            items = self.data_items(head, rest, where)
            return sum(s for s, _ in items), ('data', items)
        ops = split_operands(rest) if rest else []
        if head in ('IFF', 'ANDF', 'OPTION', 'FLAG'):
            return 2, ('macro', head, ops)
        if head in ('XLATT', 'PUT_ERROR_CHAR', 'SET_FLAG'):
            return 2, ('macro', head, ops)
        if head in ('ELSEF', 'ENDIFF', 'RESET_NLS', 'BEEP', 'CHECK_FOR_CORE_KEY'):
            return 1, ('macro', head, ops)
        if head in ('IFKBD', 'GOTO', 'EXIT_INT_9', 'EXIT_STATE_LOGIC'):
            return 3, ('macro', head, ops)
        raise Error(f'{where}: unknown statement {line.strip()!r}')

    def scan(self):
        offset = 0
        for path, number, line in self.lines():
            where = f'{Path(path).name}:{number}'
            size, payload = self.statement(line, where, offset)
            if payload: self.items.append((offset, payload, where))
            offset += size
        self.size = offset


def equ_table(include_dir):
    """The EQUs of the include files the modules share."""
    table = {}
    for name in ('POSTEQU.INC', 'KEYBSHAR.INC', 'KEYBMAC.INC'):
        found = [p for p in Path(include_dir).iterdir() if p.name.upper() == name]
        text = found[0].read_bytes().decode('latin-1').replace('\r', '')
        in_struc = in_macro = False
        for raw in text.split('\n'):
            line = strip_comment(raw)
            words = line.split()
            if not words: continue
            u = [w.upper() for w in words]
            if in_macro:
                if u[0] == 'ENDM': in_macro = False
                continue
            if in_struc:
                if len(u) > 1 and u[1] == 'ENDS': in_struc = False
                continue
            if len(u) > 1 and u[1] == 'MACRO': in_macro = True; continue
            if len(u) > 1 and u[1] == 'STRUC': in_struc = True; continue
            m = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s+(EQU|=)\s+(.*)$', line, re.I)
            if m: table[m.group(1).upper()] = ('expr', m.group(3), name)
    return table


def assemble(include_dir, paths, additions=()):
    equs = equ_table(include_dir)
    modules = [Module(p, include_dir, equs) for p in paths]
    for module in modules: module.scan()
    if additions:
        add_languages(modules[0], additions)
    # Each module at a paragraph, in link order.
    bases, at = [], 0
    for module in modules:
        at = (at + 15) & ~15
        bases.append(at); at += module.size
    publics = {}
    for module, base in zip(modules, bases):
        for name in module.publics:
            if name not in module.labels: raise Error(f'{module.path}: PUBLIC {name} undefined')
            publics[name] = base + module.labels[name]
    image = bytearray(at)
    for module, base in zip(modules, bases):
        cache = {}
        def symbols(name, module=module, base=base, cache=cache):
            if name in module.labels: return base + module.labels[name]
            if name in module.externs or (name in publics and name not in module.local_equs and name not in equs):
                return publics.get(name)
            for table in (module.local_equs, equs):
                if name in table:
                    if name not in cache:
                        kind, text, where = table[name]
                        cache[name] = module.value(text, 0, symbols, where)
                    return cache[name]
            return None
        def put(offset, size, value, where):
            if size == 1 and not -128 <= value <= 255: raise Error(f'{where}: byte {value} out of range')
            if size == 2 and not -32768 <= value <= 65535: raise Error(f'{where}: word {value} out of range')
            image[base + offset:base + offset + size] = (value & ((1 << 8 * size) - 1)).to_bytes(size, 'little')
        for offset, payload, where in module.items:
            here = base + offset
            if payload[0] == 'data':
                for size, text in payload[1]:
                    if text.strip() == '?': value = 0
                    else: value = module.value(text, here, symbols, where)
                    put(offset, size, value, where); offset += size; here += size
                continue
            _, head, ops = payload
            v = lambda t: module.value(t, here, symbols, where)
            def flag(mask):
                mask = mask.strip().upper()
                if mask not in FLAG_IDS: raise Error(f'{where}: unknown flag {mask}')
                return symbols(FLAG_IDS[mask]), v(mask)
            def negate(rest):
                if not rest: return 0
                if rest[0].strip().upper() != 'NOT': raise Error(f'{where}: {rest[0]!r} for NOT')
                return 8
            if head in ('IFF', 'ANDF'):
                fid, mask = flag(ops[0])
                command = symbols('IFF_COMMAND' if head == 'IFF' else 'ANDF_COMMAND')
                put(offset, 1, command + negate(ops[1:]) + fid, where); put(offset + 1, 1, mask, where)
            elif head == 'FLAG':
                fid, mask = flag(ops[0]); put(offset, 1, fid, where); put(offset + 1, 1, mask, where)
            elif head == 'OPTION':
                put(offset, 1, symbols('OPTION_COMMAND') + negate(ops[1:]), where); put(offset + 1, 1, v(ops[0]), where)
            elif head in ('XLATT', 'PUT_ERROR_CHAR', 'SET_FLAG'):
                name = {'XLATT': 'XLATT_COMMAND', 'PUT_ERROR_CHAR': 'PUT_ERROR_COMMAND', 'SET_FLAG': 'SET_FLAG_COMMAND'}[head]
                put(offset, 1, symbols(name), where); put(offset + 1, 1, v(ops[0]), where)
            elif head in ('ELSEF', 'ENDIFF', 'RESET_NLS', 'BEEP', 'CHECK_FOR_CORE_KEY'):
                name = {'ELSEF': 'ELSEF_COMMAND', 'ENDIFF': 'ENDIFF_COMMAND', 'RESET_NLS': 'RESET_NLS_COMMAND',
                        'BEEP': 'BEEP_COMMAND', 'CHECK_FOR_CORE_KEY': 'CHECK_CORE_COMMAND'}[head]
                put(offset, 1, symbols(name), where)
            elif head == 'IFKBD':
                put(offset, 1, symbols('IFKBD_COMMAND'), where); put(offset + 1, 2, v(ops[0]), where)
            elif head == 'GOTO':
                # DW GOTO_OFFSET-$-2 with $ at the word: the target less the command's end.
                put(offset, 1, symbols('GOTO_COMMAND'), where); put(offset + 1, 2, v(ops[0]) - (here + 1) - 2, where)
            elif head == 'EXIT_INT_9':
                put(offset, 1, symbols('GOTO_COMMAND') + symbols('EXIT_INT_9_FLAG'), where); put(offset + 1, 2, 0, where)
            elif head == 'EXIT_STATE_LOGIC':
                put(offset, 1, symbols('GOTO_COMMAND') + symbols('EXIT_STATE_LOGIC_FLAG'), where); put(offset + 1, 2, 0, where)
    return bytes(image)


def add_languages(directory, additions):
    """The added languages in the directory module (KDFNOW.ASM): an entry in its
    language table, before 'US', and in its ID table for one with an ID,
    and the counts of both raised. additions: (code, id or None, label)."""
    items = directory.items
    def words(i):
        p = items[i][1]
        return [t.strip().upper() for _, t in p[1]] if p[0] == 'data' else None
    def insert(at, new):
        start = items[at][0]
        size = sum(n for n, _ in new)
        for name, value in directory.labels.items():
            if value >= start: directory.labels[name] = value + size
        items[at:] = [(start, ('data', new), 'added')] + [(o + size, p, w) for o, p, w in items[at:]]
        directory.size += size
    # The counts, IDs then languages: the two words before the first language code.
    def code(i):
        w = words(i)
        return w and len(w) == 2 and all(len(x) == 3 and x[0] == "'" for x in w)
    first = next(i for i in range(len(items)) if code(i))
    ids_at, langs_at = first - 2, first - 1
    assert words(ids_at)[0].isdigit() and words(langs_at)[0].isdigit()
    us = next(i for i in range(len(items)) if words(i) == ["'U'", "'S'"])
    insert(us, [x for code, _, label in additions
                for x in ((1, repr(code[0])), (1, repr(code[1])), (2, 'OFFSET ' + label), (2, '0'))])
    with_id = [(ident, label) for _, ident, label in additions if ident is not None]
    if with_id:
        last = max(i for i in range(len(items)) if words(i) == ['OFFSET DUMMY_ENT', '0'])
        insert(last - 1, [x for ident, label in with_id for x in ((2, str(ident)), (2, 'OFFSET ' + label), (2, '0'))])
    for at, more in ((ids_at, len(with_id)), (langs_at, len(additions))):
        offset, payload, _ = items[at]
        items[at] = (offset, ('data', [(2, str(int(words(at)[0]) + more))]), 'added')
    for _, _, label in additions: directory.externs.add(label.upper())


def main(argv):
    additions, args = [], []
    i = 0
    while i < len(argv):
        if argv[i] == '--add':
            code, ident, path = argv[i + 1].split(':', 2)
            additions.append((code, int(ident) if ident else None, path)); i += 2
        else:
            args.append(argv[i]); i += 1
    output, include_dir, paths = args[0], args[1], args[2:]
    # Added modules go before the end module (KDFEOF), labels CODE_LANG_ENT.
    extra = [(code, ident, f'{code}_LANG_ENT') for code, ident, _ in additions]
    paths = paths[:-1] + [p for _, _, p in additions] + paths[-1:]
    image = assemble(include_dir, paths, extra)
    Path(output).write_bytes(image)


if __name__ == '__main__':
    try: main(sys.argv[1:])
    except Error as e: sys.exit(f'mkkeyboard: {e}')
