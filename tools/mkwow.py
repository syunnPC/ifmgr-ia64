#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate WOW.DLL API tables from win/wow/api16.txt.

Format: MODULE ORDINAL NAME RETURN [ARGUMENT ...] [=NATIVE] [!cdecl]
[!regs] [!custom]. Arguments use declaration order and the types below.
Default wrappers convert arguments/results around NAME or =NATIVE calls.
!custom uses DWORD W16_NAME(Args16 *), or W16_NATIVE for shared handlers.
Unimplemented stubs16.txt entries call W16_Unimplemented (return 0).
Emit ordinal-sorted kernel_api, user_api, gdi_api, etc. for MODULES."""
import sys

# type: (stack kind, native expression of argument v)
ARGS = {
    'word': ('w', '(UINT){v}'), 'uint': ('w', '(UINT){v}'), 'int': ('s', '(int)(LONG){v}'),
    'bool': ('w', '(BOOL){v}'), 'char': ('w', '(char){v}'), 'byte': ('w', '(BYTE){v}'),
    'hfile': ('w', '(HFILE)(short){v}'), 'handle16': ('w', '(HANDLE)(ULONG_PTR){v}'),
    'long': ('l', '(LONG){v}'), 'dword': ('l', '(DWORD){v}'), 'colorref': ('l', '(COLORREF){v}'),
    'hwnd': ('w', 'HWND32({v})'), 'hdc': ('w', '(HDC)HGDI32({v})'), 'hgdi': ('w', 'HGDI32({v})'),
    'hpen': ('w', '(HPEN)HGDI32({v})'), 'hbrush': ('w', 'Brush32((WORD){v})'), 'hfont': ('w', '(HFONT)HGDI32({v})'),
    'hbitmap': ('w', '(HBITMAP)HGDI32({v})'), 'hrgn': ('w', '(HRGN)HGDI32({v})'), 'hpalette': ('w', '(HPALETTE)HGDI32({v})'),
    'hmenu': ('w', 'HMENU32({v})'), 'hicon': ('w', 'HICON32({v})'), 'hcursor': ('w', '(HCURSOR)HICON32({v})'),
    'haccel': ('w', 'HACCEL32({v})'), 'hinstance': ('w', 'Instance32((WORD){v})'),
    'str': ('p', '(LPCSTR)PTR({v})'), 'buf': ('p', '(LPSTR)PTR({v})'), 'ptr': ('p', 'PTR({v})'),
    'resname': ('R', '(LPCSTR)PTR({v})'), 'segptr': ('P', '{v}'),
    'rect_in': ('p', None), 'rect_out': ('p', None), 'rect_io': ('p', None),
    'point_in': ('p', None), 'point_out': ('p', None), 'point_io': ('p', None), 'size_out': ('p', None),
}
# return type: (16-bit result, conversion of native result c)
RETURNS = {
    'void': (True, None), 'bool': (True, '(DWORD)(BOOL)({c})'), 'int': (True, '(DWORD)(int)({c})'),
    'word': (True, '(DWORD)(UINT)({c})'), 'uint': (True, '(DWORD)(UINT)({c})'), 'hfile': (True, '(DWORD)(int)({c})'),
    'handle16': (True, '(DWORD)(WORD)(ULONG_PTR)({c})'),
    'byte': (True, '(DWORD)(BYTE)({c})'),
    'long': (False, '(DWORD)(LONG)({c})'), 'dword': (False, '(DWORD)({c})'), 'colorref': (False, '(DWORD)({c})'),
    'hwnd': (True, 'HWND16({c})'), 'hdc': (True, 'HGDI16({c})'), 'hgdi': (True, 'HGDI16({c})'),
    'hpen': (True, 'HGDI16({c})'), 'hbrush': (True, 'HGDI16({c})'), 'hfont': (True, 'HGDI16({c})'),
    'hbitmap': (True, 'HGDI16({c})'), 'hrgn': (True, 'HGDI16({c})'), 'hpalette': (True, 'HGDI16({c})'),
    'hmenu': (True, 'HMENU16({c})'), 'hicon': (True, 'HICON16({c})'), 'hcursor': (True, 'HICON16({c})'),
    'haccel': (True, 'HACCEL16({c})'),
    'arg0': (False, None),  # the first argument's far pointer (lstrcpy and the like)
}
MODULES = {'KERNEL': 'kernel', 'USER': 'user', 'GDI': 'gdi', 'COMMDLG': 'commdlg', 'SHELL': 'shell',
           'MMSYSTEM': 'mmsystem', 'LZEXPAND': 'lzexpand', 'VER': 'ver', 'WIN87EM': 'win87em',
           'KEYBOARD': 'keyboard', 'SOUND': 'sound', 'SYSTEM': 'system', 'TOOLHELP': 'toolhelp',
           'PSCRIPT': 'pscript'}

def parse(path):
    entries = []
    for number, line in enumerate(open(path, encoding='ascii'), 1):
        line = line.split('#', 1)[0].split()
        if not line:
            continue
        module, ordinal, name, ret, *rest = line
        flags = {w[1:] for w in rest if w.startswith('!')}
        native = next((w[1:] for w in rest if w.startswith('=')), name)
        args = [w for w in rest if w[0] not in '!=']
        if module not in MODULES or ret not in RETURNS or any(a not in ARGS for a in args):
            sys.exit(f'{path}:{number}: bad entry')
        entries.append((module, int(ordinal), name, ret, args, native, flags))
    return entries

def wrapper(name, ret, args, native):
    decls, pre, post, call = [], [], [], []
    for i, a in enumerate(args):
        v = f'a->a[{i}]'
        expr = ARGS[a][1]
        if expr is not None:
            call.append(expr.format(v=v))
            continue
        kind, way = a.split('_')
        decls.append({'rect': 'RECT', 'point': 'POINT', 'size': 'SIZE'}[kind] + f' v{i};')
        if way == 'in':
            decls.append(f'BOOL in{i};')
            pre.append(f'in{i}={kind.capitalize()}In16({v},&v{i});')
            call.append(f'in{i}?&v{i}:NULL')
        elif way == 'io':
            pre.append(f'if(!{kind.capitalize()}In16({v},&v{i})) memset(&v{i},0,sizeof(v{i}));')
            call.append(f'&v{i}')
        else:
            pre.append(f'memset(&v{i},0,sizeof(v{i}));')
            call.append(f'&v{i}')
        if way in ('out', 'io'):
            post.append(f'{kind.capitalize()}Out16({v},&v{i});')
    c = f'{native}({",".join(call)})'
    convert = RETURNS[ret][1]
    if ret in ('void', 'arg0'):
        pre.append(f'{c};')
        result = 'a->raw[0]' if ret == 'arg0' else '0'
    else:
        decls.append('DWORD r;')
        pre.append(f'r={convert.format(c=c)};')
        result = 'r'
    body = decls+pre+post+[f'return {result};']
    if ret == 'void' and not post and not decls and not pre[:-1]:
        body = [pre[-1], 'return 0;']
    return f'static DWORD g_{name}(Args16 *a) {{{" ".join(body)}}}'

def stubs(path, entries):
    have = {(e[0], e[1]) for e in entries}
    out = []
    for line in open(path, encoding='ascii'):
        line = line.split('#', 1)[0].split()
        if not line:
            continue
        module, ordinal, name, kinds, flags = line
        if module in MODULES and (module, int(ordinal)) not in have:
            out.append((module, int(ordinal), name, '' if kinds == '-' else kinds, flags.split(',')))
    return out

def main(source, stub_list, output):
    entries = parse(source)
    unimplemented = stubs(stub_list, entries)
    out = ['/* Generated by tools/mkwow.py from win/wow/api16.txt; do not edit. */', '#include "api.h"']
    declared = set()
    for module, ordinal, name, ret, args, native, flags in entries:
        if 'custom' in flags or 'regs' in flags:
            fn = native.lstrip('_')
            if fn not in declared:
                out.append(f'DWORD W16_{fn}(Args16 *);')
                declared.add(fn)
        else:
            out.append(wrapper(name, ret, args, native))
    for module, prefix in MODULES.items():
        rows = []
        for _, ordinal, name, ret, args, native, flags in (e for e in entries if e[0] == module):
            kinds = ''.join(ARGS[a][0] for a in args)
            bits = []
            if RETURNS[ret][0]:
                bits.append('A_RET16')
            if 'cdecl' in flags:
                bits.append('A_CDECL')
            if 'regs' in flags:
                bits.append('A_REGS')
            fn = f'W16_{native.lstrip("_")}' if 'custom' in flags or 'regs' in flags else f'g_{name}'
            rows.append((ordinal, name, kinds, bits, fn))
        for _, ordinal, name, kinds, flags in (u for u in unimplemented if u[0] == module):
            bits = (['A_RET16'] if 'ret16' in flags else []) + (['A_CDECL'] if 'cdecl' in flags else [])
            rows.append((ordinal, name, kinds, bits, 'W16_Unimplemented'))
        out.append(f'const Api16 {prefix}_api[]={{')
        for ordinal, name, kinds, bits, fn in sorted(rows):
            out.append(f'    {{{ordinal},"{name.upper()}","{kinds}",{"|".join(bits) or 0},{fn}}},')
        out.append('};')
        out.append(f'const unsigned {prefix}_count=sizeof({prefix}_api)/sizeof({prefix}_api[0]);')
    open(output, 'w').write('\n'.join(out)+'\n')

if __name__ == '__main__':
    main(*sys.argv[1:])
