#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later AND MIT
# MS-DOS 4.0 portions (the messages the tests expect): Copyright (c)
# Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
"""QEMU tests: disposable images driven over the serial console, and over QMP for the screen and input."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
# Every timeout stretches by QEMU_TEST_SLOW (default 3): guests run more
# slowly on a loaded or slower host, and a test that waits ends as soon as
# what it waits for comes.
SLOW = float(os.environ.get('QEMU_TEST_SLOW', '3'))

class Guest:
    def __init__(self, image, log, machine, ready=None, extra_args=()):
        self.log = open(log, 'wb')
        env = dict(os.environ, DOS_IMAGE=str(image), MACHINE=machine)
        self.p = subprocess.Popen([str(ROOT/'tools/test_vm.sh'), *extra_args], cwd=ROOT, env=env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.p.stdout, selectors.EVENT_READ)
        self.output = b''
        self.prompt = b'C:\\>'
        try:
            self.boot=self.expect(ready or self.prompt)
            if ready is None:
                assert 'IA-64/EFI DOS Version 4.00\n' in self.boot, self.boot
        except BaseException:
            self.close()
            raise

    def expect(self, text, timeout=45):
        deadline = time.monotonic()+timeout*SLOW
        while text not in self.output:
            if time.monotonic() > deadline:
                raise AssertionError(f'timed out waiting for {text!r}; tail={self.output[-2000:]!r}')
            self.receive(text)
        end = self.output.index(text)+len(text)
        result, self.output = self.output[:end], self.output[end:]
        return result.decode('ascii', 'replace')

    def expect_any(self, *texts, timeout=45):
        deadline = time.monotonic()+timeout*SLOW
        while True:
            hits=[(self.output.index(t),i) for i,t in enumerate(texts) if t in self.output]
            if hits:
                position,index=min(hits)
                return index,self.expect(texts[index],timeout)
            if time.monotonic() > deadline:
                raise AssertionError(f'timed out waiting for {texts!r}; tail={self.output[-2000:]!r}')
            self.receive(texts)

    def receive(self, waiting):
        """What QEMU writes within 0.2 s, logged and added to the output; waiting names what for."""
        for key, _ in self.selector.select(0.2):
            chunk = os.read(key.fileobj.fileno(), 65536)
            if not chunk:
                raise AssertionError(f'QEMU exited while waiting for {waiting!r}: {self.output[-2000:]!r}')
            self.log.write(chunk); self.log.flush()
            self.output += chunk.replace(b'\r', b'')

    def command(self, text, expected=None, prompt=None, error=False):
        self.p.stdin.write(text.encode('ascii')+b'\r'); self.p.stdin.flush()
        result = self.expect(prompt or self.prompt)
        # The first line is serial input echo, not evidence of command output.
        result = result.split('\n', 1)[-1]
        if expected is not None and expected not in result:
            raise AssertionError(f'{text}: missing {expected!r}: {result}')
        if not error and ('(DOS error ' in result or '[FAIL]' in result):
            raise AssertionError(f'{text}: {result}')
        print(f'PASS console: {text}', flush=True)
        return result

    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try: self.p.wait(timeout=5*SLOW)
            except subprocess.TimeoutExpired:
                self.p.kill(); self.p.wait()
        self.selector.close(); self.log.close()

def free_memory(guest):
    """MEM's largest executable program size: the DOS arena's largest free run."""
    result=guest.command('mem')
    found=re.search(r'(\d+) largest executable program size',result)
    assert found, result
    return int(found.group(1))

class Qmp:
    """QEMU machine protocol client for display and input checks."""
    def __init__(self, path, timeout=30):
        self.path = Path(path)
        timeout *= SLOW
        deadline = time.monotonic()+timeout
        while True:
            self.s = socket.socket(socket.AF_UNIX)
            try:
                self.s.connect(str(path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                self.s.close()
                if time.monotonic() > deadline: raise
                time.sleep(0.1)
        self.s.settimeout(timeout)
        self.f = self.s.makefile('rwb')
        self.read()
        self.execute('qmp_capabilities')

    def read(self):
        while True:
            line = self.f.readline()
            if not line: raise AssertionError('QMP connection closed')
            message = json.loads(line)
            if 'event' not in message: return message

    def execute(self, command, **arguments):
        request = {'execute': command}
        if arguments: request['arguments'] = arguments
        self.f.write(json.dumps(request).encode()+b'\n'); self.f.flush()
        reply = self.read()
        if 'error' in reply: raise AssertionError(f'QMP {command}: {reply["error"]}')
        return reply['return']

    def screen(self, path):
        """Screendump as (width, height, rgb bytes); PPM P6, maxval 255."""
        path = Path(path); path.unlink(missing_ok=True)
        self.execute('screendump', filename=str(path))
        data = path.read_bytes()
        fields, offset = [], 0
        while len(fields) < 4:
            while data[offset:offset+1].isspace(): offset += 1
            if data[offset:offset+1] == b'#':
                offset = data.index(b'\n', offset)+1; continue
            end = offset
            while not data[end:end+1].isspace(): end += 1
            fields.append(data[offset:end]); offset = end
        assert fields[0] == b'P6' and fields[3] == b'255', fields
        width, height = int(fields[1]), int(fields[2])
        return Screen(width, height, data[offset+1:offset+1+width*height*3])

    def pointer(self, dx=0, dy=0, button=None, down=True):
        events = []
        if dx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': dx}})
        if dy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': dy}})
        if button: events.append({'type': 'btn', 'data': {'down': down, 'button': button}})
        self.execute('input-send-event', events=events)

    def keys(self, *qcodes):
        """Press the keys in order, then release them in reverse order."""
        key = lambda code, down: {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}
        self.execute('input-send-event', events=[key(c, True) for c in qcodes])
        self.execute('input-send-event', events=[key(c, False) for c in reversed(qcodes)])

    def close(self):
        self.f.close(); self.s.close()

class Screen:
    def __init__(self, width, height, rgb):
        self.width, self.height, self.rgb = width, height, rgb
    def pixel(self, x, y):
        offset = (y*self.width+x)*3
        return tuple(self.rgb[offset:offset+3])
    def lit(self, x0, y0, x1, y1):
        """Any non-black pixel in the rectangle."""
        return any(self.pixel(x, y) != (0, 0, 0) for y in range(y0, y1) for x in range(x0, x1))

def run(image, prefix, machine):
    guest = Guest(image, f'{prefix}-boot.log', machine)
    try:
        guest.command('ver', 'IA-64/EFI DOS Version 4.00')
        guest.command('hello native-argument', 'Command tail: native-argument')
        guest.command('command /c ver', 'IA-64/EFI DOS Version 4.00')
        guest.command('command /c hello nested-shell', 'Command tail: nested-shell')
        guest.command('command /c exit37')
        guest.command('if errorlevel 37 echo SHELL-RETURN-PASS', 'SHELL-RETURN-PASS')
        guest.command('echo %COMSPEC%', 'C:\\COMMAND.COM')
        guest.command('set INHERIT_TEST=outer')
        result=guest.command('envtest', 'ENVTEST:')
        assert 'ENVTEST-CHILD: 5 passed, 0 failed' in result and '0 failed' in result, result
        guest.command('echo %INHERIT_TEST%', 'outer')
        guest.command('set INHERIT_TEST=')
        for _ in range(2):
            result=guest.command('filetest', 'FILETEST:')
            assert 'FILETEST-CHILD: 5 passed, 0 failed' in result and '0 failed' in result, result
        guest.command('command /c set PATH=C:\\CHILD')
        guest.command('path', 'PATH=C:\\')
        guest.command('exit', 'Permanent COMMAND.COM cannot exit.')
        # A second shell says which DOS it is, and does not run AUTOEXEC.BAT (whose
        # ECHO OFF would leave no prompt).
        guest.command('command', 'IA-64/EFI DOS Version 4.00')
        guest.command('set PRIVATE=child-only')
        guest.command('exit')
        guest.command('echo [%PRIVATE%]', '[]')
        guest.command('systest', '0 failed')
        guest.command('porttest absent', 'PORTTEST: 7 passed, 0 failed')
        guest.p.stdin.write(b'systest key\r'); guest.p.stdin.flush()
        guest.expect(b'I/O key test: press K\n')
        guest.p.stdin.write(b'K'); guest.p.stdin.flush()
        result=guest.expect(guest.prompt)
        assert '[PASS] io-key-event' in result and '0 failed' in result and '[FAIL]' not in result, result
        print('PASS console: SYSTEST input event and repeated context cleanup', flush=True)
        result = guest.command('apitest', 'APITEST:')
        match = re.search(r'APITEST: (\d+) passed, (\d+) failed', result)
        assert match and int(match[1])>=40 and int(match[2])==0, result
        guest.command('apitest', '0 failed') # cleanup and repeated image relocation
        guest.command('md lab')
        guest.command('cd lab', prompt=b'C:\\LAB>')
        guest.prompt=b'C:\\LAB>'
        guest.command('echo first-line > one.txt')
        guest.command('echo second-line >> one.txt')
        guest.command('copy one.txt two.txt', '1 file(s) copied')
        guest.command('ren two.txt saved.txt')
        guest.command('type saved.txt', 'first-line\nsecond-line\n')
        guest.command('copy saved.txt saved.txt', 'Access denied', error=True)
        guest.command('copy saved.txt c:/lab/./SAVED.TXT', 'Access denied', error=True)
        guest.command('copy saved.txt .', 'Access denied', error=True)
        guest.command('type saved.txt', 'first-line\nsecond-line\n')
        guest.command('dir /b > list.txt')
        guest.command('type list.txt', 'SAVED.TXT')
        guest.command('md box')
        guest.command('md box\\inner')
        guest.command('md dest')
        guest.command('echo MOVED-PERSIST > box\\inner\\note.txt')
        guest.command('ren box box\\inner\\loop', 'Access denied', error=True)
        guest.command('ren box\\inner dest\\moved')
        guest.command('type dest\\moved\\note.txt', 'MOVED-PERSIST')
        guest.command('ren dest\\moved\\note.txt .\\moved.txt')
        guest.command('type moved.txt', 'MOVED-PERSIST')
        guest.command('rd box')
        guest.command('hello redirected > hello.txt')
        guest.command('type hello.txt', 'Command tail: redirected')
        guest.command('set TESTVAR=environment-value')
        guest.command('echo %TESTVAR%', 'environment-value')
        guest.command('command /c set', 'TESTVAR=environment-value')
        guest.command('command /c set TESTVAR=child-value')
        guest.command('echo %TESTVAR%', 'environment-value')
        guest.command('copy c:\\hello.efi greet.efi', '1 file(s) copied')
        guest.command('path c:\\lab')
        guest.command('c:\\command.com /c greet inherited-path', 'Command tail: inherited-path')
        guest.command('path c:\\')
        guest.command('del greet.efi')
        guest.command('echo @echo off > flow.bat')
        guest.command('echo if exist saved.txt goto good >> flow.bat')
        guest.command('echo echo BATCH-FAIL >> flow.bat')
        guest.command('echo :good >> flow.bat')
        guest.command('echo echo BATCH-PASS >> flow.bat')
        result=guest.command('flow', 'BATCH-PASS')
        assert 'BATCH-FAIL' not in result, result
        guest.command('echo legacy > legacy.exe')
        guest.command('legacy.exe', 'Unsupported executable format', error=True)
        guest.command('del one.txt')
        guest.command('cd \\', prompt=b'C:\\>')
        guest.prompt=b'C:\\>'
        guest.command('rd lab', 'Access denied', error=True)
        # MEM as DOS 4's: the native arena (4 MiB), its blocks with /PROGRAM.
        result=guest.command('mem','   4194304 bytes total memory\n   4194304 bytes available\n')
        assert re.match(r'\n\n +\d+ bytes total memory\n +\d+ bytes available\n +\d+ largest executable program size\n',result), result
        assert 'EMS' not in result and 'extended' not in result, result
        result=guest.command('mem /program','  Address     Name          Size       Type \n')
        for line in ('  [0-9A-F]{6}      MEM          [0-9A-F]{6}     Program   ','  [0-9A-F]{6}      COMMAND      [0-9A-F]{6}     Program   ','  [0-9A-F]{6}                   [0-9A-F]{6}     -- Free --'):
            assert re.search('\n'+line+'\n',result), (line,result)
        guest.command('mem /debug','largest executable program size')
        guest.command('mem /program /debug','Too many parameters - /DEBUG')
        guest.command('mem /x','Invalid switch - /X')
        guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush()
        guest.p.wait(timeout=15*SLOW)
        assert guest.p.returncode==0
    finally:
        guest.close()
    guest=Guest(image, f'{prefix}-reboot.log', machine)
    try:
        guest.command('type c:\\lab\\saved.txt', 'first-line\nsecond-line\n')
        guest.command('type c:\\lab\\moved.txt', 'MOVED-PERSIST')
        guest.command('dir /b c:\\lab\\dest', 'MOVED')
        guest.command('dir c:\\lab', 'SAVED    TXT')
        guest.command('hello after-reboot', 'Command tail: after-reboot')
    finally:
        guest.close()
    from check_image import inspect
    files=inspect(image)
    assert files['LAB/SAVED.TXT']==b'first-line\nsecond-line\n'
    assert files['LAB/MOVED.TXT']==b'MOVED-PERSIST\n'
    assert 'LAB/ONE.TXT' not in files
    assert not any(name.startswith('APIWORK/') for name in files)
    assert 'FILETEST.DAT' not in files
    print(f'PASS: {machine} boot, native API, file persistence across reboot', flush=True)

def fcbs(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='fcb-test-') as temporary:
        image=Path(temporary)/'disk.img'
        build(image,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=fat12)
        prefix=ROOT/'build'/f'fcb-{machine}-fat{12 if fat12 else 16}'
        guest=Guest(image,Path(str(prefix)+'-0.log'),machine)
        try:
            guest.command('fcbtest','0 failed')
        finally:
            guest.close()
        guest=Guest(image,Path(str(prefix)+'-1.log'),machine)
        try:
            guest.command('fcbtest verify','0 failed')
        finally:
            guest.close()
        files=inspect(image)
        assert files['FCBKEEP.DAT']==b'fcb!!' and not any(name.startswith('FCBWORK/') for name in files)
        raw=image.read_bytes(); start=int.from_bytes(raw[454:458],'little')*512
        assert raw[start+43:start+54]==b'DOS4 IA64  '
        print(f'PASS FCB: {machine} FAT{12 if fat12 else 16} records, DTA, sharing, locks, labels, SDK and reboot',flush=True)

def nls(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='nls-test-') as temporary:
        work=Path(temporary); media=work/'media'
        shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nCOUNTRY=81,932,C:\\COUNTRY.SYS\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        image=work/'disk.img'
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        prefix=ROOT/'build'/f'nls-{machine}-fat{12 if fat12 else 16}'
        guest=Guest(image,Path(str(prefix)+'-0.log'),machine)
        try:
            guest.command('nlstest','0 failed')
        finally:
            guest.close()
        guest=Guest(image,Path(str(prefix)+'-1.log'),machine)
        try:
            guest.command('nlstest verify','0 failed')
        finally:
            guest.close()
        files=inspect(image)
        assert files['NLSKEEP.DAT']==b'NLS \x81\\\xe5a'
        assert not any(name.startswith('NLSWORK/') for name in files)
        print(f'PASS NLS: {machine} FAT{12 if fat12 else 16} CONFIG, native APIs, DBCS paths/FCBs and reboot',flush=True)

def clocks(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='clock-test-') as temporary:
        image=Path(temporary)/'disk.img'
        build(image,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=fat12)
        prefix=ROOT/'build'/f'clock-{machine}-fat{12 if fat12 else 16}'
        guest=Guest(image,Path(str(prefix)+'-0.log'),machine)
        try:
            guest.command('timetest','0 failed')
            guest.command('date 2000-02-29')
            guest.command('time 12:34:56.78')
            guest.command('date /t','2000-02-29')
            guest.command('time /t','.78')
            for bad in ('date 2001-02-29','date 2100-01-01','date 2000-04-31','date 2000-02-29 extra',
                        'time 24:00','time 12:60','time 12:34:60','time 12:34:56.100','time 12:34 garbage'):
                guest.command(bad,'Invalid ',error=True)
            guest.command('date /t','2000-02-29')
            guest.command('time /t','.78')
            prompt=b'or press Enter to keep it: '
            guest.p.stdin.write(b'date\r'); guest.p.stdin.flush(); guest.expect(prompt)
            guest.p.stdin.write(b'\r'); guest.p.stdin.flush(); guest.expect(guest.prompt)
            guest.command('date /t','2000-02-29')
            guest.p.stdin.write(b'date\r'); guest.p.stdin.flush(); guest.expect(prompt)
            guest.p.stdin.write(b'2001-02-29\r'); guest.p.stdin.flush()
            result=guest.expect(prompt); assert 'Invalid date' in result, result
            guest.p.stdin.write(b'2001-03-01\r'); guest.p.stdin.flush(); guest.expect(guest.prompt)
            guest.command('date /t','2001-03-01')
            guest.p.stdin.write(b'time\r'); guest.p.stdin.flush(); guest.expect(prompt)
            guest.p.stdin.write(b'0:1\r'); guest.p.stdin.flush(); guest.expect(guest.prompt)
            guest.command('time /t','00:01:')
            guest.command('echo 2004-02-29 > clock.in')
            guest.command('date < clock.in')
            guest.command('date /t','2004-02-29')
            guest.command('date < nul')
            guest.command('date /t > clock.out')
            guest.command('type clock.out','2004-02-29')
            guest.p.stdin.write(b'time\r'); guest.p.stdin.flush(); guest.expect(prompt)
            guest.p.stdin.write(b'\x03'); guest.p.stdin.flush(); guest.expect(guest.prompt)
            guest.command('date /t','2004-02-29')
        finally:
            guest.close()
        guest=Guest(image,Path(str(prefix)+'-1.log'),machine)
        try:
            guest.command('timetest verify','TIMETEST: 4 passed, 0 failed')
        finally:
            guest.close()
        files=inspect(image)
        assert files['CLOCK.TST']==b'clock' and files['CLOCK.OUT']==b'2004-02-29\n'
        print(f'PASS clock: {machine} FAT{12 if fat12 else 16} EFI/DOS setters, limits, DATE/TIME, input, Ctrl-C and persistent timestamps',flush=True)

def console(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='console-test-') as temporary:
        image=Path(temporary)/'disk.img'
        build(image,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'console-{machine}-fat{12 if fat12 else 16}.log',machine)
        def interactive(mode,keys,ready=None):
            guest.p.stdin.write(f'contest {mode}\r'.encode()); guest.p.stdin.flush()
            guest.expect(f'CONTEST: {ready or mode}-ready\n'.encode())
            guest.p.stdin.write(keys); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert '[FAIL]' not in result and '(DOS error ' not in result and '0 failed' in result, result
            print(f'PASS console API: {mode}',flush=True)
        try:
            guest.command('contest setup','0 failed')
            guest.command('contest redir < conin.tmp > conout.txt')
            guest.command('type conout.txt','CONTEST: 10 passed, 0 failed')
            interactive('queue',b'K')
            interactive('line',b'ab\bC\r')
            interactive('edit',b'\x1b[13~\r')
            interactive('raw',b'\x03\x1a\rZ')
            interactive('callback',b'\x03X')
            interactive('abort',b'\x03',ready='abort-child')
            guest.command('break','BREAK is off')
            guest.command('break on')
            guest.command('command /c break','BREAK is on')
            guest.command('command /c break off')
            guest.command('break','BREAK is on')
            guest.command('break off')
            guest.p.stdin.write(b'echo MUST-NOT-RUN\x03'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert '\nMUST-NOT-RUN\n' not in result and '^C' in result, result
            guest.command('ver','IA-64/EFI DOS Version 4.00')
            guest.command('echo @echo off > cbatch.bat')
            guest.command('echo echo BEFORE-BREAK >> cbatch.bat')
            guest.command('echo pause >> cbatch.bat')
            guest.command('echo echo AFTER-BREAK >> cbatch.bat')
            guest.p.stdin.write(b'cbatch\r'); guest.p.stdin.flush()
            result=guest.expect(b'Press any key to continue . . .')
            assert 'BEFORE-BREAK' in result, result
            guest.p.stdin.write(b'\x03'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert 'AFTER-BREAK' not in result and '^C' in result, result
            guest.command('echo RESUMED > resumed.txt')
            guest.command('type resumed.txt','RESUMED')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        files=inspect(image)
        assert b'CONTEST: 10 passed, 0 failed' in files['CONOUT.TXT']
        assert 'BREAK.TMP' not in files and files['RESUMED.TXT']==b'RESUMED\n'
        print(f'PASS console: {machine} native character/line input, redirection, raw mode, breaks, child cleanup, batch cancellation',flush=True)

SHELL_BATCHES={
    'ARGS.BAT':'@echo off\r\necho zero=%0 one=%1 two=%2 three=%3> ARGS.OUT\r\nshift\r\n'
               'echo shifted=%0 %1 %2 %3>> ARGS.OUT\r\nif "%1"=="B" echo quoted-equal>> ARGS.OUT\r\n'
               'if not %2==X echo not-equal>> ARGS.OUT\r\nif %2==c echo case-blind>> ARGS.OUT\r\n'
               'echo percent=%%>> ARGS.OUT\r\necho env=%COMSPEC%>> ARGS.OUT\r\n'
               'CHAIN %3 tail\r\necho NOT-REACHED>> ARGS.OUT\r\n',
    'CHAIN.BAT':'@echo off\r\necho chained=%0 %1 %2>> ARGS.OUT\r\ncall CALLEE.BAT nested\r\n'
                'echo returned>> ARGS.OUT\r\ngoto skip\r\necho SKIPPED>> ARGS.OUT\r\n'
                ':skip extra words\r\necho label-ok>> ARGS.OUT\r\n',
    'CALLEE.BAT':'@echo off\r\necho callee=%1>> ARGS.OUT\r\n',
    'FORS.BAT':'@echo off\r\nfor %%f in (C*.BAT) do echo bat %%f>> FORS.OUT\r\n'
               'for %%x in (1,2;3) do echo item %%x>> FORS.OUT\r\n'
               'for %%x in (a) do for %%y in (b) do echo nested>> FORS.OUT\r\n',
    'NOLABEL.BAT':'@echo off\r\ngoto nowhere\r\necho AFTER-MISSING-LABEL\r\n',
    'CTTYTEST.BAT':'@echo off\r\nctty nul\r\necho HIDDEN-OUTPUT\r\nctty con\r\necho VISIBLE-OUTPUT\r\n',
    'ECHOON.BAT':'echo on\r\necho VISIBLE\r\n',
}
# Shift-JIS: 835C and 837C end in the ASCII codes of '\' and '|'.
DBCS_BATCH=(b'@echo off\r\nmd \x83\x5c\r\necho A\x83\x7cB> \x83\x5c\\\x83\x7c.TXT\r\n'
            b'type \x83\x5c\\\x83\x7c.TXT > DBCS1.TXT\r\n'
            b'copy \x83\x5c\\\x83\x7c.TXT \x83\x5c\\\x83\x5c.TXT > NUL\r\n'
            b'ren \x83\x5c\\\x83\x5c.TXT \x83\x5cZ.TXT\r\n'
            b'for %%f in (\x83\x5c\\*.TXT) do echo %%f>> DBCS2.TXT\r\n'
            b'if exist \x83\x5c\\\x83\x7c.TXT echo YES> DBCS3.TXT\r\n'
            b'cd \x83\x5c\r\ntruename \x83\x7c.TXT > \\DBCS4.TXT\r\ncd \\\r\n'
            b'echo \x83\x7c| pipetest count > DBCS5.TXT\r\n'
            b'set K=\x83\x7c\r\necho [%K%]> DBCS6.TXT\r\n')

def shell(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    bits=12 if fat12 else 16
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='shell-test-') as temporary:
        work=Path(temporary); media=work/'media'; shutil.copytree(ROOT/'build/media',media)
        for name,text in SHELL_BATCHES.items(): (media/name).write_bytes(text.encode('ascii'))
        (media/'DBCS.BAT').write_bytes(DBCS_BATCH)
        image=work/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'shell-{machine}-fat{bits}-0.log',machine)
        try:
            guest.command('help','TRUENAME')
            guest.command('echo','ECHO is on')
            result=guest.command('echo.'); assert result=='\nC:\\>', result
            guest.command('xyzzy','Bad command or file name',error=True)
            # Pipes: intermediate AH=5Ah files in the root or %TEMP%, then deleted.
            guest.command('pipetest emit 5 | pipetest upper | pipetest count','PIPETEST: 5 lines, 35 bytes')
            guest.command('pipetest emit 3 | pipetest upper > up.txt')
            guest.command('type up.txt','LINE 1\nLINE 2\nLINE 3\n')
            guest.command('pipetest count < up.txt','PIPETEST: 3 lines, 21 bytes')
            guest.command('set TEMP=C:\\EFI')
            guest.command('pipetest emit 2 | pipetest count','PIPETEST: 2 lines, 14 bytes')
            guest.command('set TEMP=')
            guest.command('echo pipe-echo| pipetest upper','PIPE-ECHO')
            guest.command('echo a|','Syntax error',error=True)
            # Batch parameters, SHIFT, IF, chaining without CALL, CALL and GOTO.
            guest.command('args A B C D')
            assert 'AFTER-MISSING-LABEL' not in guest.command('nolabel','Label not found',error=True)
            guest.command('fors')
            assert 'HIDDEN' not in guest.command('cttytest','VISIBLE-OUTPUT')
            guest.command('command /c echoon > echoon.out')
            guest.command('for %v in (x y) do echo item %v > for.out')
            guest.command('for %v in (x) echo bad','Syntax error',error=True)
            guest.command('if a==a echo string-equal','string-equal')
            assert 'WRONG' not in guest.command('if not a==a echo WRONG')
            guest.command('if a echo x','Syntax error',error=True)
            guest.command('if exist hello.efi echo found-file','found-file')
            guest.command('if exist c:\\nul echo found-device','found-device')
            guest.command('exit37')
            guest.command('if errorlevel 37 echo level-37','level-37')
            assert 'level-38' not in guest.command('if errorlevel 38 echo level-38')
            # PROMPT metastrings, then the default $P$G.
            guest.prompt=b'$=<|>'; guest.command('prompt $$$q$l$b$g')
            guest.prompt=b'DOS Version 4.00]'; guest.command('prompt [$v]')
            guest.prompt=b']>'; result=guest.command('prompt [$t]$g')
            guest.p.stdin.write(b'ver\r'); guest.p.stdin.flush(); result=guest.expect(guest.prompt)
            assert re.search(r'\[[ \d]\d:\d\d:\d\d\.\d\d\]>$',result), result
            guest.prompt=b'C>'; guest.command('prompt $n$g')
            guest.prompt=b'C:\\>'; guest.command('prompt')
            guest.command('vol',' Volume in drive C is DOS4 IA64')
            guest.command('vol q:','Invalid drive specification',error=True)
            guest.command('verify','VERIFY is off')
            guest.command('verify on'); guest.command('verify','VERIFY is on')
            guest.command('verify maybe','Must specify ON or OFF',error=True)
            guest.command('verify off')
            guest.command('chcp','Active code page: 437')
            guest.command('chcp 437')
            guest.command('chcp 99','Parameter value not in allowed range',error=True)
            result=guest.command('chcp 999',error=True)
            assert 'Invalid code page' in result or 'Code page 999 not prepared' in result, result
            guest.command('truename hello.efi','C:\\HELLO.EFI')
            guest.command('set BAD','Syntax error',error=True)
            guest.command('path ;'); guest.command('path','No Path'); guest.command('path c:\\')
            result=guest.command('dir',' Directory of  C:\\\n')
            assert ' Volume in drive C is DOS4 IA64\n' in result and re.search(r'HELLO    EFI +\d+ ',result), result
            assert re.search(r'EFI          <DIR> ',result) and re.search(r'\d+ File\(s\) +\d+ bytes free',result), result
            result=guest.command('dir /w','HELLO    EFI')
            # Five names per row, no sizes; HELLO may end a row.
            assert '<DIR>' not in result and re.search(r'HELLO    EFI(?: +[A-Z]| *\n)',result), result
            assert re.search(r'\n(?:[A-Z0-9]+ +[A-Z]* +){4}[A-Z0-9]+',result), result
            guest.command('dir hello','HELLO    EFI')
            guest.command('dir nothing.zzz','File not found',error=True)
            guest.p.stdin.write(b'dir /p\r'); guest.p.stdin.flush(); pauses=0
            while not guest.expect_any(b'Press any key to continue . . .',guest.prompt)[0]:
                pauses+=1; guest.p.stdin.write(b' '); guest.p.stdin.flush()
            assert pauses, 'DIR /P did not pause'
            print('PASS console: dir /p',flush=True)
            # COPY: concatenation, /A and /B, wildcards, devices and verify.
            guest.command('md sh'); guest.command('cd sh',prompt=b'C:\\SH>'); guest.prompt=b'C:\\SH>'
            guest.command('echo ab > z1.txt')
            guest.command('copy /b z1.txt+z1.txt z2.txt','z1.txt\nz1.txt\n        1 file(s) copied.')
            guest.command('copy z2.txt z3.txt /a','        1 file(s) copied.')
            guest.command('copy /b z3.txt+z1.txt z4.txt')
            result=guest.command('type z4.txt'); assert result=='ab\nab\nC:\\SH>', result
            guest.command('copy z1.txt+z3.txt z5.txt')
            guest.command('copy z5.txt+z5.txt','Content of destination lost before copy')
            guest.command('copy /v z2.txt zv.txt','        1 file(s) copied.')
            guest.command('verify','VERIFY is off')
            guest.command('copy z*.txt *.cpy','Z1.TXT\n')
            guest.command('copy nofile.txt x.txt','File not found',error=True)
            guest.command('copy z1.txt','File cannot be copied onto itself',error=True)
            guest.p.stdin.write(b'copy con typed.txt\r'); guest.p.stdin.flush()
            guest.p.stdin.write(b'TYPED-LINE\r\x1a\r'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt); assert '1 file(s) copied' in result, result
            guest.command('ren *.cpy *.bak')
            guest.command('ren nofile.* x.*','Duplicate file name or file not found',error=True)
            guest.command('del','Required parameter missing',error=True)
            guest.p.stdin.write(b'del /p z?.bak\r'); guest.p.stdin.flush()
            for answer in (b'n',b'y',b'x',b'n',b'y',b'n',b'y'):
                guest.expect(b'Delete (Y/N)?'); guest.p.stdin.write(answer+b'\r'); guest.p.stdin.flush()
            guest.expect(guest.prompt)
            guest.p.stdin.write(b'del *.*\r'); guest.p.stdin.flush()
            guest.expect(b'Are you sure (Y/N)?'); guest.p.stdin.write(b'n\r'); guest.p.stdin.flush()
            guest.expect(guest.prompt)
            guest.command('dir /b','TYPED.TXT')
            guest.command('cd \\',prompt=b'C:\\>'); guest.prompt=b'C:\\>'
            guest.command('md sh\\gone'); guest.command('copy sh\\z1.txt sh\\gone')
            guest.p.stdin.write(b'del sh\\gone\r'); guest.p.stdin.flush()
            guest.expect(b'Are you sure (Y/N)?'); guest.p.stdin.write(b'y\r'); guest.p.stdin.flush()
            guest.expect(guest.prompt)
            guest.command('rd sh\\gone')
            guest.command('command /e:512 /c echo env-ok','env-ok')
            guest.command('command /e:99 /c echo env-bad','Parameter value not in allowed range')
            guest.command('command /msg /c ver','IA-64/EFI DOS Version 4.00')
            guest.command('command c:\\sh /c set','COMSPEC=C:\\SH\\COMMAND.COM')
            guest.command('command c:\\nodir /c ver','Specified COMMAND search directory bad')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        files=inspect(image)
        assert files['ARGS.OUT']==(b'zero=args one=A two=B three=C\nshifted=A B C D\nquoted-equal\nnot-equal\n'
                                   b'percent=%\nenv=C:\\COMMAND.COM\nchained=CHAIN D tail\ncallee=nested\n'
                                   b'returned\nlabel-ok\n'), files['ARGS.OUT']
        fors=files['FORS.OUT'].split(b'\n')
        assert {b'bat CHAIN.BAT',b'bat CALLEE.BAT',b'bat CTTYTEST.BAT'}<=set(fors), fors
        assert fors[-5:]==[b'item 1',b'item 2',b'item 3',b'FOR cannot be nested',b''], fors
        assert files['ECHOON.OUT']==b'C:\\>echo on\nC:\\>echo VISIBLE\nVISIBLE\n', files['ECHOON.OUT']
        assert files['FOR.OUT']==b'C:\\>echo item x\nitem x\nC:\\>echo item y\nitem y\n', files['FOR.OUT']
        sh={name[3:]:data for name,data in files.items() if name.startswith('SH/')}
        assert sh['Z1.TXT']==b'ab\n' and sh['Z2.TXT']==b'ab\nab\n' and sh['Z3.TXT']==b'ab\nab\n\x1a', sh
        assert sh['Z4.TXT']==b'ab\nab\n\x1aab\n' and sh['Z5.TXT']==b'ab\nab\nab\n\x1a' and sh['ZV.TXT']==sh['Z2.TXT'], sh
        assert sh['TYPED.TXT']==b'TYPED-LINE\r\n', sh
        assert set(sh)=={'Z1.TXT','Z2.TXT','Z3.TXT','Z4.TXT','Z5.TXT','ZV.TXT','TYPED.TXT','Z1.BAK','Z3.BAK','Z5.BAK'}, sorted(sh)
        assert not any(re.fullmatch(r'(EFI/)?[0-9A-F]{8}',name) for name in files), sorted(files)
        # DBCS parsing: trail bytes 5Ch/7Ch are characters, not separators or pipes.
        (media/'CONFIG.SYS').write_text('FILES=64\nCOUNTRY=81,932,C:\\COUNTRY.SYS\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'shell-{machine}-fat{bits}-1.log',machine)
        try:
            guest.command('chcp','Active code page: 932')
            guest.command('dbcs')
            guest.command('chcp 437'); guest.command('chcp','Active code page: 437')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        files=inspect(image)
        folder=b'\x83\x5c/'.decode('cp437')
        assert files[folder+b'\x83\x7c.TXT'.decode('cp437')]==b'A\x83\x7cB\n', sorted(files)
        assert files[folder+b'\x83\x5cZ.TXT'.decode('cp437')]==b'A\x83\x7cB\n', sorted(files)
        assert files['DBCS1.TXT']==b'A\x83\x7cB\n', files['DBCS1.TXT']
        assert files['DBCS2.TXT']==b'\x83\x5c\\\x83\x7c.TXT\n\x83\x5c\\\x83\x5cZ.TXT\n', files['DBCS2.TXT']
        assert files['DBCS3.TXT']==b'YES\n' and files['DBCS4.TXT']==b'C:\\\x83\x5c\\\x83\x7c.TXT\n', files['DBCS4.TXT']
        assert files['DBCS5.TXT']==b'PIPETEST: 1 lines, 3 bytes\n' and files['DBCS6.TXT']==b'[\x83\x7c]\n', files['DBCS6.TXT']
        print(f'PASS shell: {machine} FAT{bits} pipes, batch parameters, chaining, FOR, IF, PROMPT, COPY, DIR, DEL, REN, VOL, VERIFY, CHCP, CTTY, TRUENAME and DBCS parsing',flush=True)

def drives(machine):
    from mkimage import build
    from check_image import inspect, u32
    import struct
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='drive-test-') as temporary:
        fixture=Path(temporary); boot=fixture/'boot.img'; second=fixture/'second.img'
        shutil.copyfile(ROOT/'build/dos-ia64.img',boot)
        build(second,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=True)
        # The VPC firmware publishes only its selected physical disk.
        # Give it two partitions to exercise independent C:/D: volume handles.
        cimage=bytearray(boot.read_bytes()); dimage=bytearray(second.read_bytes())
        dstart=u32(dimage,454); dcount=u32(dimage,458); joined_start=len(cimage)//512
        entry=bytearray(dimage[446:462]); entry[0]=0; entry[4]=1
        struct.pack_into('<I',entry,8,joined_start); cimage[462:478]=entry
        partition=dimage[dstart*512:(dstart+dcount)*512]
        struct.pack_into('<I',partition,28,joined_start)
        cimage.extend(partition); boot.write_bytes(cimage)
        for reboot in range(2):
            guest=Guest(boot,ROOT/'build'/f'drives-{machine}-{reboot}.log',machine)
            try:
                if not reboot:
                    result=guest.command('drvtest','DRIVETEST:')
                    assert 'DRIVETEST-CHILD: 7 passed, 0 failed' in result and '0 failed' in result, result
                    guest.command('echo C-PERSIST > same.txt')
                    guest.command('echo D-PERSIST > d:\\same.txt')
                    guest.command('copy same.txt d:\\fromc.txt','1 file(s) copied')
                    guest.command('copy d:\\same.txt fromd.txt','1 file(s) copied')
                    guest.command('md d:\\work')
                    guest.command('cd d:\\work')
                    guest.command('cd d:','D:\\WORK')
                    guest.command('d:',prompt=b'D:\\WORK>'); guest.prompt=b'D:\\WORK>'
                    guest.command('echo SECOND-WORK > local.txt')
                    guest.command('type \\same.txt','D-PERSIST')
                    guest.command('copy c:\\hello.efi greet.efi','1 file(s) copied')
                    guest.command('c:\\command.com /c greet from-D','Command tail: from-D')
                    guest.command('echo @echo off > flow.bat')
                    guest.command('echo echo D-BATCH-PASS >> flow.bat')
                    guest.command('flow','D-BATCH-PASS')
                    guest.command('c:',prompt=b'C:\\>'); guest.prompt=b'C:\\>'
                    guest.command('path d:\\work')
                    guest.command('greet drive-path','Command tail: drive-path')
                    guest.command('path c:\\')
                guest.command('type c:\\same.txt','C-PERSIST')
                guest.command('type d:\\same.txt','D-PERSIST')
                guest.command('type c:\\fromd.txt','D-PERSIST')
                guest.command('type d:\\fromc.txt','C-PERSIST')
                guest.command('type d:\\work\\local.txt','SECOND-WORK')
                guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush()
                guest.p.wait(timeout=15*SLOW); assert guest.p.returncode==0
            finally:
                guest.close()
        combined=boot.read_bytes()
        dimage[dstart*512:(dstart+dcount)*512]=combined[joined_start*512:]
        struct.pack_into('<I',dimage,dstart*512+28,dstart); second.write_bytes(dimage)
        c=inspect(boot); d=inspect(second)
        assert c['SAME.TXT']==d['FROMC.TXT']==b'C-PERSIST\n'
        assert d['SAME.TXT']==c['FROMD.TXT']==b'D-PERSIST\n'
        assert d['WORK/LOCAL.TXT']==b'SECOND-WORK\n' and 'WORK/LOCAL.TXT' not in c
        print(f'PASS drives: {machine} FAT16/FAT12, separate volumes, native ABI, EXEC, batch, reboot',flush=True)

def fat_write_fault(image,config,fixture):
    """A blkdebug config failing the write of the second FAT copy's sector that holds the first
    free cluster's entry; the image's bytes and its FAT's width."""
    from check_image import u16, u32
    original=image.read_bytes(); start=u32(original,454); volume=original[start*512:]
    reserved=u16(volume,14); spf=u16(volume,22); spc=volume[13]
    data=reserved+volume[16]*spf+(u16(volume,17)*32+511)//512
    total=u16(volume,19) or u32(volume,32); clusters=(total-data)//spc
    bits=12 if clusters<4085 else 16
    table=volume[reserved*512:(reserved+spf)*512]
    for cluster in range(2,clusters+2):
        off=cluster+cluster//2 if bits==12 else cluster*2
        value=u16(table,off)
        if bits==12: value=(value>>4 if cluster&1 else value)&0xfff
        if not value: break
    else: raise AssertionError(f'{fixture} fixture has no free clusters')
    # Fail the second FAT copy after earlier sectors in the same transaction
    # have reached the actual emulated controller and EFI Block I/O driver.
    target=start+reserved+spf+off//512
    config.write_text('[inject-error]\nevent = "write_aio"\niotype = "write"\n'
                      f'sector = "{target}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n')
    return original,bits

def faults(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='fault-test-') as temporary:
        fixture=Path(temporary); image=fixture/'disk.img'; config=fixture/'blkdebug.conf'
        build(image,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=fat12)
        original,bits=fat_write_fault(image,config,'fault')
        log=ROOT/'build'/f'fault-{machine}-fat{bits}.log'
        guest=Guest(f'blkdebug:{config}:{image}',log,machine)
        try:
            guest.p.stdin.write(b'md txnfail\r'); guest.p.stdin.flush()
            result=guest.expect(b'Action: ')
            assert 'Critical disk error 23 on C: (write)' in result, result
            guest.p.stdin.write(b'F'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert '(DOS error 83)' in result, result
            assert image.read_bytes()==original, 'native rollback did not restore every disk byte'
            guest.command('md txnfail')
            guest.command('echo RECOVERED > txnfail\\ok.txt')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        files=inspect(image); assert files['TXNFAIL/OK.TXT']==b'RECOVERED\n'
        print(f'PASS faults: {machine} FAT{bits} EFI write failure, complete rollback and successful retry',flush=True)

def critical_errors(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='critical-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CRIT.TMP').write_bytes(b'held')
        image=fixture/'disk.img'; config=fixture/'blkdebug.conf'
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        original,bits=fat_write_fault(image,config,'critical')
        for mode in ('fail','retry','ignore-meta','abort','shell-retry','shell-abort'):
            image.write_bytes(original)
            log=ROOT/'build'/f'critical-{machine}-fat{bits}-{mode}.log'
            guest=Guest(f'blkdebug:{config}:{image}',log,machine)
            try:
                if mode.startswith('shell-'):
                    guest.p.stdin.write(b'md critdir\r'); guest.p.stdin.flush()
                    result=guest.expect(b'Action: ')
                    assert 'Critical disk error 23' in result and '[I]gnore' not in result, result
                    guest.p.stdin.write(b'R' if mode=='shell-retry' else b'A'); guest.p.stdin.flush()
                    result=guest.expect(guest.prompt)
                    assert '(DOS error ' not in result, result
                    guest.command('ver','IA-64/EFI DOS Version 4.00')
                else:
                    result=guest.command('crittest '+mode,'CRITTEST:')
                    assert '0 failed' in result and '[FAIL]' not in result, result
                if mode in ('fail','ignore-meta','abort','shell-abort'):
                    assert image.read_bytes()==original, f'{mode}: disk rollback changed the image'
                guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
                assert guest.p.returncode==0
            finally:
                guest.close()
            inspect(image)
            print(f'PASS critical: {machine} FAT{bits} {mode}',flush=True)

def devices(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='device-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        config=media/'CONFIG.SYS'; image=fixture/'disk.img'; bits=12 if fat12 else 16
        # Failed INIT must invoke FINISH, unload, and release its name before a
        # later DEVICE line loads another instance. A duplicate cannot replace it.
        config.write_text('DEVICE=LOOPDRV.SYS /FAIL\nDEVICE=LOOPDRV.SYS SAMPLE\nDEVICE=LOOPDRV.SYS duplicate\n'
                          'DEVICE=HELLO.EFI\nFILES=64\nSHELL=C:\\COMMAND.COM /P\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'devices-{machine}-fat{bits}.log',machine)
        try:
            assert 'CONFIG.SYS line 1: DEVICE C:\\LOOPDRV.SYS failed' in guest.boot, guest.boot
            assert 'LOOPDRV.SYS: finished' in guest.boot, guest.boot
            assert 'CONFIG.SYS line 3: DEVICE C:\\LOOPDRV.SYS failed (80)' in guest.boot, guest.boot
            assert 'CONFIG.SYS line 4: DEVICE C:\\HELLO.EFI failed (11)' in guest.boot, guest.boot
            guest.command('type loop','SAMPLE')
            guest.command('echo DEVICE-ROUNDTRIP > loop')
            guest.command('type loop > captured.txt')
            guest.command('type captured.txt','DEVICE-ROUNDTRIP')
            result=guest.command('devtest','DEVTEST:')
            assert '0 failed' in result and '[FAIL]' not in result, result
            guest.p.stdin.write(b'devtest abort\r'); guest.p.stdin.flush()
            guest.expect(b'DEVTEST: abort-ready\n')
            guest.p.stdin.write(b'\3'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert '0 failed' in result and '[PASS] aborted-cookie-released' in result and '[FAIL]' not in result, result
            guest.command('command /c echo SECOND-SHELL > loop')
            guest.command('type loop','SECOND-SHELL')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        assert inspect(image)['CAPTURED.TXT']==b'DEVICE-ROUNDTRIP\n'
        # A returning replacement shell closes all device handles and unloads
        # the driver before MSDOS.SYS and its API table disappear.
        config.write_text('DEVICE=LOOPDRV.SYS\nSHELL=C:\\DEVTEST.EFI\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'device-unload-{machine}-fat{bits}.log',machine,ready=b'Boot image returned successfully.')
        try:
            assert 'DEVTEST:' in guest.boot and '0 failed' in guest.boot and '[FAIL]' not in guest.boot, guest.boot
            assert 'LOOPDRV.SYS: finished' in guest.boot, guest.boot
        finally:
            guest.close()
        config.write_text('DEVICE=LOOPDRV.SYS /FAIL\nSHELL=C:\\DEVTEST.EFI absent\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'device-failed-{machine}-fat{bits}.log',machine,ready=b'DEVTEST: 3 passed, 0 failed\n')
        try:
            assert '[FAIL]' not in guest.boot, guest.boot
        finally:
            guest.close()
        print(f'PASS devices: {machine} FAT{bits} resident load, rollback, I/O, controls, inheritance and unload',flush=True)

def ports(machine,fat12=False):
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='ports-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        config=media/'CONFIG.SYS'; image=fixture/'disk.img'; bits=12 if fat12 else 16
        serial_path=fixture/'uart.sock'; printer_path=fixture/'printer.bin'
        extra=['-chardev',f'socket,id=dos_uart,path={serial_path},server=on,wait=off',
               '-device','isa-serial,index=1,iobase=0x2f8,irq=3,chardev=dos_uart',
               '-chardev',f'file,id=dos_printer,path={printer_path}',
               '-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer']
        # The VPC firmware's only EFI Serial I/O instance drives its console UART
        # (named by ConIn/ConOut), so IO.SYS publishes no unit: COM2=EFI0 fails
        # cleanly (invalid drive/unit) and leaves the COM names for line 3.
        config.write_text('DEVICE=PORTDRV.SYS COM2=EFI0\nDEVICE=PORTDRV.SYS COM1=3F8\nDEVICE=PORTDRV.SYS COM1=2F8 LPT1=378\n'
                          'DEVICE=PORTDRV.SYS COM1=2E8\nDEVICE=LOOPDRV.SYS\nSHELL=C:\\COMMAND.COM /P\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'ports-{machine}-fat{bits}.log',machine,extra_args=extra)
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as uart:
            try:
                uart.settimeout(5*SLOW); uart.connect(str(serial_path))
                assert 'CONFIG.SYS line 1: DEVICE C:\\PORTDRV.SYS failed (15)' in guest.boot, guest.boot
                assert 'CONFIG.SYS line 2: DEVICE C:\\PORTDRV.SYS failed (170)' in guest.boot, guest.boot
                assert 'CONFIG.SYS line 4: DEVICE C:\\PORTDRV.SYS failed (80)' in guest.boot, guest.boot
                result=guest.command('devtest','DEVTEST:')
                assert '0 failed' in result and '[FAIL]' not in result, result
                result=guest.command('porttest','PORTTEST:')
                assert '0 failed' in result and '[FAIL]' not in result, result
                expected=b'PORT-TX\n\0A\x80\xff#CHILD\n'
                output=b''
                while len(output)<len(expected):
                    part=uart.recv(1024); assert part, output; output+=part
                assert output==expected, output
                guest.p.stdin.write(b'porttest receive\r'); guest.p.stdin.flush()
                guest.expect(b'PORTTEST: receive-ready\n')
                uart.sendall(b'\0\3\x80\xffRZ')
                result=guest.expect(guest.prompt)
                assert '[PASS] external-binary-receive' in result and '0 failed' in result and '[FAIL]' not in result, result
                # Bytes sent while the shell idles at its prompt are sampled by
                # IO.SYS into its 4 KiB ring before any DOS read; a larger burst
                # fills the ring and the rest waits in the UART/chardev FIFO.
                for count in (300,5000):
                    uart.sendall(bytes((i*7+1)&255 for i in range(count))); time.sleep(1.5)
                    result=guest.command(f'porttest buffer {count}','PORTTEST:')
                    if count<4096: assert f'PORTTEST: {count} bytes buffered before any DOS read' in result, result
                    assert '[PASS] idle-sampling' in result and '[PASS] ring-filled' in result, result
                    assert '[PASS] buffered-order' in result and '0 failed' in result and '[FAIL]' not in result, result
                    print(f'PASS ports: {count} bytes timer-sampled while DOS idled, then read in order',flush=True)
                guest.command('echo REDIRECT > aux')
                output=b''
                while len(output)<9:
                    part=uart.recv(1024); assert part, output; output+=part
                assert output==b'REDIRECT\n', output
                guest.command('echo PRINT-REDIRECT > prn')
                guest.command('ver','IA-64/EFI DOS Version 4.00')
                guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
                assert guest.p.returncode==0
            finally:
                guest.close()
        assert printer_path.read_bytes()==b'PRINT\0\n!?PRINT-REDIRECT\n', printer_path.read_bytes()
        files=inspect(image)
        assert all(name not in files for name in ('COM1.TXT','AUX','PRN','LPT1'))
        # Native shell return exercises FINISH, register restoration, release
        # of IO.SYS resource tokens and actual EFI driver-image unload.
        config.write_text('DEVICE=PORTDRV.SYS COM1=2F8 LPT1=378\nSHELL=C:\\PORTTEST.EFI\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        unload_extra=list(extra)
        unload_extra[1]=f'file,id=dos_uart,path={fixture/"uart-unload.bin"}'
        unload_extra[5]=f'file,id=dos_printer,path={fixture/"printer-unload.bin"}'
        guest=Guest(image,ROOT/'build'/f'ports-unload-{machine}-fat{bits}.log',machine,
                    ready=b'Boot image returned successfully.',extra_args=unload_extra)
        try:
            assert '[FAIL]' not in guest.boot and '0 failed' in guest.boot, guest.boot
        finally:
            guest.close()
        assert (fixture/'uart-unload.bin').read_bytes()==expected
        assert (fixture/'printer-unload.bin').read_bytes()==b'PRINT\0\n!?'
        print(f'PASS ports: {machine} FAT{bits} dedicated UART/SPP, binary I/O, aliases, redirection, timeouts and unload',flush=True)

def ramdisks(machine,fat12=False):
    from mkimage import build
    from check_image import inspect,u32
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='ram-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        config=media/'CONFIG.SYS'; image=fixture/'disk.img'; second=fixture/'second.img'
        bits=12 if fat12 else 16; size=2048 if fat12 else 4096
        config.write_text(f'DEVICE=RAMDRV.SYS /SIZE:0\nDEVICE=RAMDRV.SYS /SIZE:{size} /UNITS:2\n'
                          'DEVICE=RAMDRV.SYS\nDEVICE=LOOPDRV.SYS\nSHELL=C:\\COMMAND.COM /P\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        build(second,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=True)
        # Keep firmware-backed D: alongside the native RAM units E:/F:.
        cimage=bytearray(image.read_bytes()); dimage=bytearray(second.read_bytes())
        start=u32(dimage,454); count=u32(dimage,458); joined=len(cimage)//512
        entry=bytearray(dimage[446:462]); entry[0]=0; entry[4]=1
        struct.pack_into('<I',entry,8,joined); cimage[462:478]=entry
        partition=dimage[start*512:(start+count)*512]; struct.pack_into('<I',partition,28,joined)
        cimage.extend(partition); image.write_bytes(cimage)
        for reboot in range(2):
            guest=Guest(image,ROOT/'build'/f'ram-{machine}-fat{bits}-{reboot}.log',machine)
            try:
                assert 'CONFIG.SYS line 1: DEVICE C:\\RAMDRV.SYS failed (1)' in guest.boot, guest.boot
                assert 'CONFIG.SYS line 3: DEVICE C:\\RAMDRV.SYS failed (80)' in guest.boot, guest.boot
                if not reboot:
                    result=guest.command('devtest','DEVTEST:')
                    assert '0 failed' in result and '[FAIL]' not in result, result
                    result=guest.command(f'ramtest {bits}','RAMTEST:')
                    assert 'RAMTEST drives: E: F:' in result and '0 failed' in result and '[FAIL]' not in result, result
                    guest.command('echo VOLATILE > e:\\temp.txt')
                    guest.command('echo PHYSICAL-D > d:\\physical.txt')
                else:
                    guest.command('type e:\\temp.txt','File not found',error=True)
                    guest.command('type d:\\physical.txt','PHYSICAL-D')
                    result=guest.command(f'ramtest {bits}','RAMTEST:')
                    assert 'RAMTEST drives: E: F:' in result and '0 failed' in result and '[FAIL]' not in result, result
                guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
                assert guest.p.returncode==0
            finally:
                guest.close()
        files=inspect(image)
        assert files['RAMBACK.BIN']==bytes((i*17+3)&255 for i in range(6000))
        assert 'RAMCTL' not in files
        config.write_text(f'DEVICE=RAMDRV.SYS /SIZE:{size} /UNITS:2\nSHELL=C:\\RAMTEST.EFI {bits}\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'ram-unload-{machine}-fat{bits}.log',machine,ready=b'Boot image returned successfully.')
        try:
            assert 'RAMTEST drives: D: E:' in guest.boot and '[FAIL]' not in guest.boot and '0 failed' in guest.boot, guest.boot
        finally:
            guest.close()
        config.write_text('DEVICE=RAMDRV.SYS /UNITS:0\nSHELL=C:\\RAMTEST.EFI absent\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        guest=Guest(image,ROOT/'build'/f'ram-failed-{machine}-fat{bits}.log',machine,ready=b'RAMTEST: 3 passed, 0 failed\n')
        try:
            assert '[FAIL]' not in guest.boot, guest.boot
        finally:
            guest.close()
        print(f'PASS RAM disk: {machine} FAT{bits} native blocks, firmware volumes, media changes, EXEC, batch, reboot and unload',flush=True)

def keyb(machine):
    """KEYB with keys from the firmware's PS/2 keyboard (QMP): German and
    Japanese layouts in COMMAND.COM's line input, a dead key and AltGr, the
    hot keys, CON's code pages with KEYB's tables, and an 8086 program's
    INT 16h and INT 2Fh AD8xh. Serial input has no shift state: it stays
    as typed."""
    name=f'keyb-{machine}'; cpu=None if machine=='itanium-vpc' else 'madison-1500'
    image=ROOT/'build'/f'{name}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    sock=ROOT/'build'/f'qmp-{name}.sock'; sock.unlink(missing_ok=True)
    guest=Guest(image,ROOT/'build'/f'{name}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off')+(('-cpu',cpu) if cpu else ()))
    qmp=None
    try:
        qmp=Qmp(sock)
        def strokes(*keys):
            for k in keys:
                qmp.keys(*(k if isinstance(k,tuple) else (k,))); time.sleep(0.15)
        def line(*keys):
            strokes('e','c','h','o','spc',*keys,'ret')
            return guest.expect(guest.prompt,timeout=30)
        guest.command('keyb','KEYB has not been installed\nCurrent CON code page: 437')
        guest.command('keyb xx','Invalid keyboard code specified')
        guest.command('if errorlevel 1 echo LEVEL1','LEVEL1')
        guest.command('keyb gr,850','Code page specified has not been prepared')
        guest.command('if errorlevel 5 echo LEVEL5','LEVEL5')
        guest.command('keyb gr,863','Invalid code page specified')
        guest.command('keyb 999','Invalid keyboard ID specified')
        guest.command('keyb be /id:120','Keyboard ID specified is inconsistent with the selected keyboard layout')
        guest.command('keyb gr,,c:\\nosuch.sys','Bad or missing Keyboard Definition File')
        guest.command('if errorlevel 2 echo LEVEL2','LEVEL2')
        guest.command('keyb gr /x','Invalid switch - /X')
        guest.command('keyb gr,437,c:\\keyboard.sys,x','Too many parameters - X')
        result=guest.command('keyb gr')
        assert 'KEYB' not in result and 'Invalid' not in result, result
        guest.command('keyb','Current keyboard code: GR  code page: 437\nCurrent CON code page: 437')
        guest.command('echo yz','yz')
        # Y and Z change places; AltGr+Q, Shift+2, the acute accent and a space.
        result=line('y','z',('alt_r','q'),('shift','2'),'equal','spc')
        assert '\nzy@"\'\n' in result, result
        # Ctrl+Alt+F1: the US layout; Ctrl+Alt+F2: back.
        result=line(('ctrl','alt','f1'),'y',('ctrl','alt','f2'),'y')
        assert '\nyz\n' in result, result
        # A page KEYB has no table for: CON switches, KEYB keeps its own
        # until KEYB is run again with the page prepared.
        guest.command('mode con cp prepare=((850) c:\\efi.cpi)','MODE prepare code page function completed')
        guest.command('mode con cp select=850','Current keyboard does not support this code page')
        guest.command('keyb','Current keyboard code: GR  code page: 437\nCurrent CON code page: 850')
        guest.command('keyb gr')
        guest.command('keyb','Current keyboard code: GR  code page: 850\nCurrent CON code page: 850')
        guest.command('mode con cp select=437','MODE select code page function completed')
        guest.command('keyb','Current keyboard code: GR  code page: 437\nCurrent CON code page: 437')
        guest.p.stdin.write(b'keyb16\r'); guest.p.stdin.flush()
        result=guest.expect(b'KEYB16: ready\n',timeout=60)
        # At once: keys wait in the firmware while the program prints
        # through INT 21h, whose look for ^C must leave them to INT 16h.
        for k in ('y',('alt_r','q'),'equal','e',('shift','2')): qmp.keys(*(k if isinstance(k,tuple) else (k,)))
        result+=guest.expect(b'KEYB16: ready\n',timeout=30); strokes('y')
        result+=guest.expect(guest.prompt,timeout=30)
        for want in ('installed ok','key 157a','key 1040','key 0082','key 0322','US mode ok','key 1579','national mode ok',
                     'bad mode refused ok','page without a table refused ok','page 437 ok'):
            assert 'KEYB16: '+want+'\n' in result, (want,result)
        assert 'FAILED' not in result, result
        # Japanese: the JIS symbols; CON's 850 has no table, so a warning.
        guest.command('keyb jp','One or more CON code pages invalid for given keyboard code')
        result=line('bracket_left','bracket_right','backslash','equal',('shift','2'),('shift','semicolon'),('shift','apostrophe'))
        assert '\n@[]^"+*\n' in result, result
        guest.command('keyb','Current keyboard code: JP  code page: 437')
        print(f'PASS keyb: {machine} German and Japanese layouts from KEYBOARD.SYS for PS/2 keys, dead key, AltGr, hot keys, code pages, VDM INT 16h/2Fh',flush=True)
    finally:
        if qmp: qmp.close()
        guest.close()

def codepages(machine,fat12=False):
    """MODE code-page/serial/printer commands. The firmware serial console
    drops CHAR16 above 7Fh and delivers input bytes as Latin-1 code units, so
    DBCS input and non-ASCII glyphs are host-tested; here an ASCII trail byte
    shows whether CON decoded a Shift-JIS pair."""
    from mkimage import build
    from check_image import inspect
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='codepage-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        bits=12 if fat12 else 16; image=fixture/'disk.img'; printer_path=fixture/'printer.bin'
        (media/'CONFIG.SYS').write_text('DEVICE=PORTDRV.SYS COM1=2F8 LPT1=378\nCOUNTRY=81,932,C:\\COUNTRY.SYS\n'
                                        'SHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        (media/'SJIS.TXT').write_bytes(b'[\x83\x5c]\r\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media,fat12=fat12)
        extra=['-chardev',f'file,id=dos_uart,path={fixture/"uart.bin"}',
               '-device','isa-serial,index=1,iobase=0x2f8,irq=3,chardev=dos_uart',
               '-chardev',f'file,id=dos_printer,path={printer_path}',
               '-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer']
        guest=Guest(image,ROOT/'build'/f'codepage-{machine}-fat{bits}.log',machine,extra_args=extra)
        try:
            result=guest.command('mode con cp /status','Active code page for device CON is 437')
            assert 'Hardware code pages:\n  code page 437\nPrepared code pages:\n  code page not prepared\n' in result, result
            guest.command('mode con cp select=850','Code page not prepared')
            guest.command('if errorlevel 1 echo MODE-FAILED','MODE-FAILED')
            guest.command('type sjis.txt','[\\]')
            guest.command('mode con codepage prepare=((850,932) c:\\efi.cpi)','MODE prepare code page function completed')
            result=guest.command('mode con: cp','  code page 850\n  code page 932\n  code page not prepared')
            guest.command('mode con cp prep=((,936) c:\\efi.cpi)','Device or code page missing from font file')
            guest.command('mode con cp prep=((850) c:\\missing.cpi)','Failure to access code page font file')
            guest.command('mode con cp prep=((850) c:\\country.sys)','Font file contents invalid')
            guest.command('mode con cp','  code page 850\n  code page 932\n')
            guest.command('mode con cp sel=932','MODE select code page function completed')
            guest.command('mode con cp /sta','Active code page for device CON is 932')
            guest.command('mode con lines=25','ANSI.SYS must be installed to perform requested function')
            result=guest.command('type sjis.txt')
            assert '[]\n' in result and '[\\]' not in result, result
            guest.command('mode con cp refresh','MODE refresh code page function completed')
            guest.command('mode con cp select=437','MODE select code page function completed')
            guest.command('type sjis.txt','[\\]')
            guest.command('mode com1 9600,n,8,1','COM1: 9600,n,8,1,-')
            result=guest.command('mode com1 /status','Status for device COM1:')
            assert 'COM1: 9600,n,8,1,-' in result, result
            guest.command('mode com1:96,e,7,1,p','COM1: 9600,e,7,1,p')
            guest.command('mode com1 baud=19200 parity=n data=8 stop=2','COM1: 19200,n,8,2,p')
            guest.command('mode com1 110','Invalid baud rate specified')
            guest.command('mode com1 /status','COM1: 19200,n,8,2,p')
            guest.command('mode com3 9600','Failure to access device: COM3')
            guest.command('mode lpt1 132','LPT1: set for 132')
            result=guest.command('mode lpt1 80,8,p','LPT1: set for 80')
            assert 'Printer lines per inch set' in result and 'Infinite retry on parallel printer time-out' in result, result
            guest.command('mode lpt1 /status','LPT1: not rerouted')
            guest.command('mode lpt1=com1','Function not supported on this computer - reroute')
            guest.command('mode lpt1 66','Invalid parameter')
            guest.command('mode xyz','Illegal device name')
            result=guest.command('mode','Status for device CON:')
            assert 'Status for device COM1:' in result and 'Status for device LPT1:' in result, result
            assert 'Active code page for device CON is 437' in result, result
            guest.command('nlstest','0 failed')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        assert printer_path.read_bytes()==b'\x0f\x12\x1b0', printer_path.read_bytes()
        files=inspect(image); assert files['EFI.CPI']==(ROOT/'build/EFI.CPI').read_bytes()
        print(f'PASS code pages: {machine} FAT{bits} MODE prepare/select/refresh/status, Shift-JIS CON output, COM/LPT setup',flush=True)

def modules(machine):
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='module-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; image=fixture/'disk.img'
        shutil.copytree(ROOT/'build/media',media)
        for name,expected in (('IO.SYS',b'IO.SYS boot failure: EFI '),
                              ('MSDOS.SYS',b'IO.SYS: kernel failure EFI '),
                              ('COMMAND.COM',b'MSDOS.SYS: C:\\COMMAND.COM: File not found (2)')):
            saved=(media/name).read_bytes(); (media/name).unlink()
            build(image,ROOT/'build/BOOTIA64.EFI',media)
            guest=Guest(image,ROOT/'build'/f'missing-{name}.log',machine,ready=expected)
            try:
                assert 'C:\\>' not in guest.boot, guest.boot
                print(f'PASS modules: missing {name} reported at its loading boundary',flush=True)
            finally:
                guest.close()
            (media/name).write_bytes(saved)
        # Replacing the shell requires no kernel or I/O rebuild.
        shutil.copyfile(ROOT/'build/hello.efi',media/'COMMAND.COM')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/replaced-command.log',machine,ready=b'Command tail: /P\n')
        try:
            assert 'Hello from an IA-64 program.' in guest.boot and 'C:\\>' not in guest.boot, guest.boot
            print('PASS modules: COMMAND.COM replaced by an ordinary native DOS app',flush=True)
        finally:
            guest.close()
        shutil.copyfile(ROOT/'build/COMMAND.COM',media/'COMMAND.COM')
        config=media/'CONFIG.SYS'
        config.unlink()
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/config-absent.log',machine)
        try:
            guest.command('echo %COMSPEC%', 'C:\\COMMAND.COM')
            print('PASS config: absent CONFIG.SYS uses the default permanent shell',flush=True)
        finally:
            guest.close()
        config.write_text('FILES=bad\nDEVICE=NOTYET.SYS\nFILES=64\nSHELL=C:\\COMMAND.COM /P\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/config-invalid.log',machine)
        try:
            assert 'CONFIG.SYS line 1:' in guest.boot and 'CONFIG.SYS line 2:' in guest.boot, guest.boot
            guest.command('ver', 'IA-64/EFI DOS Version 4.00')
            print('PASS config: invalid/unsupported lines are reported; valid settings still boot',flush=True)
        finally:
            guest.close()
        config.write_text('FILES=8\nSHELL=C:\\ENVTEST.EFI boot-config\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/config-custom-shell.log',machine,ready=b'BOOT-CONFIG: 8 passed, 0 failed\n')
        try:
            assert '[FAIL]' not in guest.boot, guest.boot
            print('PASS config: selected shell receives COMSPEC/tail and FILES=8 takes effect',flush=True)
        finally:
            guest.close()
        config.write_text('BREAK=ON\nSHELL=C:\\COMMAND.COM /C BREAK\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/config-break.log',machine,ready=b'BREAK is on\n')
        try:
            assert 'invalid or unsupported directive' not in guest.boot, guest.boot
            print('PASS config: BREAK=ON is inherited by the configured shell',flush=True)
        finally:
            guest.close()
        config.write_text('SHELL=C:\\MISSING.COM\n')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build/config-missing-shell.log',machine,ready=b'MSDOS.SYS: C:\\MISSING.COM: File not found (2)')
        try:
            print('PASS config: a missing configured shell is reported',flush=True)
        finally:
            guest.close()

def disk_fixture(path,fat12,corrupt,free_mb=0,media=None):
    """C: (from media when given) plus a FAT12 D: partition (optionally damaged) and free space."""
    from mkimage import build
    from check_image import u16, u32
    work=Path(path).parent
    boot=work/'c.img'; second=work/'d.img'
    if media: build(boot,ROOT/'build/BOOTIA64.EFI',media)
    elif fat12: build(boot,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=True)
    else: shutil.copyfile(ROOT/'build/dos-ia64.img',boot)
    build(second,ROOT/'build/BOOTIA64.EFI',ROOT/'build/media',fat12=True)
    c=bytearray(boot.read_bytes()); d=bytearray(second.read_bytes())
    start=u32(d,454); count=u32(d,458); joined=len(c)//512
    entry=bytearray(d[446:462]); entry[0]=0; entry[4]=1; struct.pack_into('<I',entry,8,joined); c[462:478]=entry
    part=bytearray(d[start*512:(start+count)*512]); struct.pack_into('<I',part,28,joined)
    if corrupt:
        reserved=u16(part,14); spf=u16(part,22); fats=part[16]
        def fat(copy): return (reserved+copy*spf)*512
        def get(cluster):
            v=u16(part,fat(0)+cluster+cluster//2); return (v>>4 if cluster&1 else v)&0xfff
        def put(cluster,value):
            for copy in range(fats):
                off=fat(copy)+cluster+cluster//2; v=u16(part,off)
                v=((v&15)|((value&0xfff)<<4)) if cluster&1 else ((v&0xf000)|(value&0xfff))
                struct.pack_into('<H',part,off,v)
        data=reserved+fats*spf+(u16(part,17)*32+511)//512; clusters=((u16(part,19) or u32(part,32))-data)//part[13]
        free=[x for x in range(2,clusters+2) if not get(x)]
        put(free[-2],free[-1]); put(free[-1],0xfff)          # a lost two-cluster chain
        root=(reserved+fats*spf)*512
        for off in range(root,root+512*32,32):
            if part[off:off+11]==b'HELLO   EFI':                # size beyond its chain
                struct.pack_into('<I',part,off+28,u32(part,off+28)+5000)
        part[fat(1)+spf*512-1]^=0x5a                          # FAT copies disagree
    c.extend(part); c.extend(bytes(free_mb*1024*1024)); Path(path).write_bytes(c)
    return joined,count

def partition_image(disk,start,count,out):
    """Wrap one partition of a disk image in a single-partition MBR for check_image."""
    raw=Path(disk).read_bytes(); image=bytearray(2048*512)
    image[446:462]=struct.pack('<B3sB3sII',0,b'\xfe\xff\xff',6,b'\xfe\xff\xff',2048,count); image[510:512]=b'\x55\xaa'
    part=bytearray(raw[start*512:(start+count)*512]); struct.pack_into('<I',part,28,2048)
    image.extend(part); Path(out).write_bytes(image)

def fat12_capacity(sectors,spc):
    """Data bytes of a FAT12 volume FORMAT builds: 1 reserved sector, 2 FATs, 512 root entries."""
    spf=1
    while True:
        clusters=(sectors-1-2*spf-32)//spc
        if ((clusters+2)*3+1)//2<=spf*512: return clusters*spc*512
        spf+=1

def answer(guest,text,prompt,reply):
    guest.p.stdin.write(text.encode()+b'\r'); guest.p.stdin.flush()
    first=guest.expect(prompt.encode(),timeout=120)
    guest.p.stdin.write(reply); guest.p.stdin.flush()
    return first+guest.expect(guest.prompt,timeout=180)

def maintenance(machine,fat12=False):
    from check_image import inspect
    bits=12 if fat12 else 16
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='maint-test-') as temporary:
        image=Path(temporary)/'disk.img'; dstart,dcount=disk_fixture(image,fat12,True)
        lost=2*image.read_bytes()[dstart*512+13]*512   # the fixture's lost chain is two clusters
        guest=Guest(image,ROOT/'build'/f'maint-{machine}-fat{bits}.log',machine)
        try:
            assert 'D: cannot mount media (23)' in guest.boot, guest.boot
            result=guest.command('chkdsk','bytes total disk space')
            assert 'Errors found' not in result and 'lost clusters' not in result, result
            result=answer(guest,'chkdsk d:','(Y/N)?',b'N\r')
            for text in ('Errors found, F parameter not specified','File allocation table 2 differs from table 1',
                         'D:\\HELLO.EFI\n   Allocation error, size adjusted','2 lost clusters found in 1 chains.',
                         f'{lost} bytes disk space would be freed','Volume Serial Number is 4941-3634'):
                assert text in result, result
            result=answer(guest,'chkdsk d: /f','(Y/N)?',b'Y\r')
            assert f'{lost} bytes in 1 recovered files' in result and 'Errors found, F' not in result, result
            result=guest.command('chkdsk d:','bytes total disk space')
            assert 'Errors found' not in result and 'Allocation error' not in result, result
            guest.command('dir d:\\*.chk','FILE0000 CHK')
            guest.command('label d: WORKDISK'); guest.command('vol d:','Volume in drive D is WORKDISK')
            guest.command('label d: bad*name','Invalid characters in volume label')
            guest.p.stdin.write(b'label d:\r'); guest.p.stdin.flush()
            result=guest.expect(b'ENTER for none)? ')
            assert 'Volume in drive D is WORKDISK' in result and 'Volume Serial Number is 4941-3634' in result, result
            result=answer(guest,'','(Y/N)?',b'Y\r')
            guest.command('vol d:','has no label')
            result=answer(guest,'format d: /v:fresh','Proceed with Format (Y/N)?',b'N\r')
            assert 'WARNING, ALL DATA ON NON-REMOVABLE DISK\nDRIVE D: WILL BE LOST!' in result, result
            guest.command('if errorlevel 5 echo FORMAT-DECLINED','FORMAT-DECLINED')
            guest.command('type d:\\readme.txt','IA-64/EFI DOS 4.0')
            guest.command('format d: /1','Parameters not compatible\nwith fixed disk',error=True)
            result=answer(guest,'format d: /v:fresh','(Y/N)?',b'Y\r')
            assert 'Format complete' in result and f'{fat12_capacity(dcount,8)} bytes total disk space' in result and 'Volume Serial Number is' in result, result
            guest.command('dir d:','Volume in drive D is FRESH')
            guest.command('echo NEWDATA > d:\\new.txt'); guest.command('type d:\\new.txt','NEWDATA')
            result=guest.command('chkdsk d:','4096 bytes in 1 user files'); assert 'Errors found' not in result, result
            guest.command('sys c:','Can not specify default drive',error=True)
            guest.command('sys d:','System transferred')
            result=guest.command('chkdsk d: /v','D:\\IO.SYS')
            assert 'in 2 hidden files' in result and 'D:\\EFI\\BOOT\\BOOTIA64.EFI' in result, result
            guest.p.stdin.write(b'format d: /s\r'); guest.p.stdin.flush(); guest.expect(b'(Y/N)?')
            guest.p.stdin.write(b'Y\r'); guest.p.stdin.flush()
            result=guest.expect(b'Volume label (11 characters, ENTER for none)? ',timeout=180)
            assert 'Format complete' in result and 'System transferred' in result, result
            guest.p.stdin.write(b'sysdisk\r'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt); assert 'bytes used by system' in result, result
            guest.command('vol d:','Volume in drive D is SYSDISK')
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        inspect(image)
        partition_image(image,dstart,dcount,Path(temporary)/'d-only.img')
        files=inspect(Path(temporary)/'d-only.img')
        assert all(name in files for name in ('IO.SYS','MSDOS.SYS','COMMAND.COM','EFI/BOOT/BOOTIA64.EFI'))
        raw=image.read_bytes()[dstart*512:]
        assert raw[43:54]==b'SYSDISK    ' and raw[38]==0x29 and raw[54:62]==b'FAT12   '
        print(f'PASS maintenance: {machine} FAT{bits} CHKDSK report and /F repair, LABEL, FORMAT /V /S, SYS',flush=True)

def fdisk(machine):
    from check_image import inspect, u32
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='fdisk-test-') as temporary:
        image=Path(temporary)/'disk.img'; disk_fixture(image,False,False,free_mb=16)
        guest=Guest(image,ROOT/'build'/f'fdisk-{machine}.log',machine)
        def keys(data,wait):
            guest.p.stdin.write(data); guest.p.stdin.flush(); return guest.expect(wait.encode(),timeout=60)
        try:
            result=guest.command('fdisk /status','Display Partition Information')
            assert ' C: 1        A   PRI DOS' in result and ' D: 2            PRI DOS' in result and 'EXT DOS' not in result, result
            keys(b'fdisk\r','Enter choice: ')
            total=image.stat().st_size//1048576
            result=keys(b'4\r','Enter choice: '); assert f'Total disk space is {total:4d} Mbytes' in result, result
            keys(b'1\r','Enter choice: '); keys(b'2\r','Extended DOS Partition....')
            result=keys(b'\r','Enter choice: '); assert 'Extended DOS Partition created' in result, result
            keys(b'1\r','Enter choice: '); keys(b'3\r','disk space (%)...')
            keys(b'8\r','disk space (%)...')
            result=keys(b'\r','Enter choice: ')
            assert 'All available space in the Extended DOS Partition' in result, result
            keys(b'\x1b','Press any key when ready . . .')
            guest.p.stdin.write(b'x'); guest.p.stdin.flush()
            boot=guest.expect(guest.prompt,timeout=90)
            assert 'E: cannot mount media (11)' in boot and 'F: cannot mount media (11)' in boot, boot
            result=guest.command('fdisk /status','EXT DOS')
            assert 'E:' in result and 'F:' in result and 'UNKNOWN' in result, result
            result=answer(guest,'format e: /v:logical1','(Y/N)?',b'Y\r'); assert 'Format complete' in result, result
            guest.command('echo LOGICAL > e:\\l.txt'); guest.command('type e:\\l.txt','LOGICAL')
            result=answer(guest,'chkdsk f:','Continue (Y/N)?',b'N\r'); assert 'Probable non-DOS disk' in result, result
            keys(b'fdisk\r','Enter choice: '); keys(b'3\r','Enter choice: ')
            result=keys(b'3\r','What drive do you want to delete')
            assert 'LOGICAL1' in result and 'FAT12' in result, result
            keys(b'F\r','Enter Volume Label'); keys(b'\r','Are you sure (Y/N)')
            result=keys(b'Y\r','Enter choice: '); assert 'Drive deleted' in result, result
            keys(b'\x1b','Press any key when ready . . .')
            guest.p.stdin.write(b'x'); guest.p.stdin.flush()
            boot=guest.expect(guest.prompt,timeout=90)
            assert 'F: cannot mount media' not in boot, boot
            guest.command('type e:\\l.txt','LOGICAL'); guest.command('dir f:','Invalid drive',error=True)
            raw=image.read_bytes()
            base=u32(raw,446+2*16+8); ebr=raw[base*512:(base+1)*512]
            assert ebr[510:512]==b'\x55\xaa' and ebr[446+4]==1 and u32(ebr,446+8)==63 and not ebr[462+4], ebr[446:478]
            # Active partition, the one-primary rule, and extended deletion.
            keys(b'fdisk\r','Enter choice: '); keys(b'2\r','make active')
            result=keys(b'2\r','Enter choice: '); assert 'Partition 2 made active' in result, result
            keys(b'1\r','Enter choice: ')
            result=keys(b'1\r','Enter choice: '); assert 'Primary DOS Partition already exists.' in result, result
            keys(b'3\r','Enter choice: ')
            result=keys(b'2\r','Enter choice: '); assert 'Cannot delete Extended DOS Partition while logical drives exist.' in result, result
            keys(b'3\r','Enter choice: '); keys(b'3\r','What drive do you want to delete')
            keys(b'E\r','Enter Volume Label'); keys(b'LOGICAL1\r','Are you sure (Y/N)')
            result=keys(b'Y\r','Enter choice: '); assert 'Drive deleted' in result, result
            keys(b'3\r','Enter choice: '); keys(b'2\r','Do you wish to continue')
            result=keys(b'Y\r','Enter choice: '); assert 'Extended DOS Partition deleted' in result, result
            keys(b'\x1b','Press any key when ready . . .')
            guest.p.stdin.write(b'x'); guest.p.stdin.flush(); guest.expect(guest.prompt,timeout=90)
            result=guest.command('fdisk /status','Display Partition Information')
            assert 'EXT DOS' not in result and ' D: 2        A   PRI DOS' in result and ' C: 1            PRI DOS' in result, result
            guest.command('dir e:','Invalid drive',error=True)
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        raw=image.read_bytes(); inspect(image)
        assert [raw[446+i*16+4] for i in range(4)]==[0xef,1,0,0] and [raw[446+i*16] for i in range(2)]==[0,0x80]
        print(f'PASS fdisk: {machine} extended/logical creation and deletion, restarts, FORMAT of a new drive, active partition, one-primary rule',flush=True)

def resident(machine):
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='tsr-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        image=fixture/'disk.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
        guest=Guest(image,ROOT/'build'/f'tsr-{machine}.log',machine)
        try:
            guest.command('tsrapp install','residency refused (11)',error=True)
            guest.command('tsrtest query','not resident')
            before=free_memory(guest)
            guest.command('tsrtest install','going resident')
            guest.command('if errorlevel 5 echo RESIDENT-EXIT-5','RESIDENT-EXIT-5')
            guest.command('tsrtest query','TSRTEST: query RESIDENT')
            guest.command('command /c tsrtest query','TSRTEST: query RESIDENT')
            assert free_memory(guest)<before, 'TSRTEST not resident'
            result=guest.command('tsrtest exec','TSRTEST: discarded (6)')
            assert 'Command tail: from-4B01' in result and 'entry inside image' in result and 'started, exit 0' in result, result
            result=guest.command('tsrtest overlay','TSRTEST: overlay '); assert 'bytes: IA-64/EFI DOS 4.0' in result, result
            guest.command('hello after-resident','Command tail: after-resident')
        finally:
            guest.close()
        (media/'CONFIG.SYS').write_text('SHELL=C:\\COMMAND.COM /C TSRTEST INSTALL\n',encoding='ascii')
        build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'tsr-unload-{machine}.log',machine,ready=b'Boot image returned successfully.')
        try:
            assert 'TSRTEST: going resident' in guest.boot and 'TSRTEST: unload hook' in guest.boot, guest.boot
        finally:
            guest.close()
        print(f'PASS resident: {machine} AH=31h residency and refusal, resident code and memory, 4B01h/4B80h/4B81h, 4B03h, unload at shell exit',flush=True)

def tasks(machine):
    image=ROOT/'build'/f'tasks-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    guest=Guest(image,ROOT/'build'/f'tasks-{machine}.log',machine)
    try:
        result=guest.command('fibrtest','FIBERTEST: 27 switches; 32 passed, 0 failed')
        before=free_memory(guest)
        for attempt in range(2):
            result=guest.command('taskhost taskapp.exe=A3 taskapp.exe=B2 taskapp.exe=C4','TASKHOST: all tasks finished')
            steps=re.findall(r'TASKAPP ([A-Z][0-9])',result)
            assert steps==['A0','B0','C0','A1','B1','C1','A2','C2','C3'], result
            for line in ('task 0 returned 3','task 1 returned 2','task 2 returned 4'): assert line in result, result
            assert free_memory(guest)==before, 'task memory not reclaimed'
        guest.command('dir \\task?','File not found',error=True)
        guest.command('taskhost hello.efi','hello.efi failed (11)',error=True)
        guest.command('taskhost tsrtest.exe','tsrtest.exe failed (11)',error=True)
        guest.command('taskapp.exe','TASKAPP: needs TASKHOST',error=True)
        assert free_memory(guest)==before, 'failed loads leaked memory'
        guest.command('taskhost taskapp.exe=D1','task 0 returned 1')
    finally:
        guest.close()
    print(f'PASS tasks: {machine} fiber switches, guest modules on fibers with DOS task contexts, interleaving, reaping, unload',flush=True)

def dos16(machine):
    # itanium2-vpc defaults to Montecito, which has no IA-32 instruction set;
    # with a Madison processor the same machine runs 16-bit programs.
    image=ROOT/'build'/f'dos16-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    for cpu in (None,) if machine=='itanium-vpc' else (None,'madison-1500'):
        sock=ROOT/'build'/f'qmp-dos16-{machine}.sock'; sock.unlink(missing_ok=True)
        guest=Guest(image,ROOT/'build'/f'dos16-{machine}{"-"+cpu if cpu else ""}.log',machine,
                    extra_args=(('-cpu',cpu) if cpu else ())+('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        try:
            if machine!='itanium-vpc' and not cpu:
                guest.command('ia32test','IA32TEST: this processor has no IA-32 instruction set')
                guest.command('hello16','VDM: 16-bit programs need a processor with the IA-32 instruction set')
                continue
            result=guest.command('ia32test','IA32TEST: exit code 42 after 6 exits, 3 checks')
            assert 'IA32TEST: protected mode ok' in result, result
            before=free_memory(guest)
            guest.command('hello16 abc def','HELLO16: DOS 4.00, tail [ abc def]')
            guest.command('if errorlevel 7 echo SEVEN','SEVEN')
            guest.command('tiny16','TINY16: ok')
            # A program that makes no calls still stops for the timer: Ctrl-C ends it.
            guest.p.stdin.write(b'hello16 /LOOP\r'); guest.p.stdin.flush()
            guest.expect(b'tail [ /LOOP]\n',timeout=30); time.sleep(2)
            guest.p.stdin.write(b'\x03'); guest.p.stdin.flush()
            guest.expect(b'^C',timeout=30); guest.expect(guest.prompt,timeout=30)
            result=guest.command('dos16t','DOS16T: all passed')
            assert result.count(' ok\n')==15 and 'HELLO16: DOS 4.00, tail [ child]' in result, result
            assert 'HELLO16: DOS 4.00, tail [ nested]' in result, result
            assert 'Command tail:  child' in result and 'BIOS teletype' in result, result
            guest.command('hello16 piped | pipetest upper','HELLO16: DOS 4.00, TAIL [ PIPED]')
            guest.command('hello16 file > out16.txt')
            guest.command('type out16.txt','HELLO16: DOS 4.00, tail [ file]')
            guest.command('dir dos16t.*','1 File(s)')
            # The first 16-bit program made the adapter a VGA in text mode 3
            # (720x400 with 9-dot characters) and the DOS console, showing
            # what the firmware's console showed. VGA16 finds its command line
            # there, writes the screen directly and through INT 10h and DOS,
            # and leaves it to the console.
            qmp=Qmp(sock); dump=ROOT/'build'/f'dos16-{machine}.ppm'
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.lit(0,384,720,400))
            guest.p.stdin.write(b'vga16\r'); guest.p.stdin.flush()
            index,_=guest.expect_any(b'VGA16: ok',b'VGA16: FAIL',timeout=60)
            assert index==0, 'VGA16: FAIL'+guest.expect(b'\n',timeout=10)
            blue,red,green,grey,magenta=(0,0,168),(168,0,0),(0,168,0),(168,168,168),(168,0,168)
            vga16=((364,4,blue),(90,32,red),(9,64,green),(0,160,grey),(364,196,magenta))
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.pixel(364,4)==blue)
            for x,y,colour in vga16:
                assert screen.pixel(x,y)==colour, (x,y,screen.pixel(x,y),colour)
            assert screen.lit(0,96,72,112), 'DOS text not on the text screen'
            assert any(r<120 and g>200 and b>200 for r,g,b in (screen.pixel(x,y) for y in range(128,144) for x in range(0,90))), 'box drawing'
            guest.p.stdin.write(b'k'); guest.p.stdin.flush(); guest.expect(guest.prompt)
            # The console goes on below on the same screen, as do programs that
            # only write text.
            guest.command('hello16 again','HELLO16: DOS 4.00, tail [ again]')
            screen=wait_screen(qmp,dump,lambda s: s.lit(0,272,72,288))
            for x,y,colour in vga16:
                assert screen.pixel(x,y)==colour, ('screen not kept',x,y,screen.pixel(x,y),colour)
            assert screen.lit(0,224,72,240), 'result line not kept'
            # Graphics: modes 13h, 12h and 4 drawn through INT 10h, memory and
            # the graphics controller, with BIOS and DOS text; mode 3 after.
            guest.p.stdin.write(b'gfx16\r'); guest.p.stdin.flush()
            white,yellow=(255,255,255),(255,255,87)
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(50,50)==red)
            for x,y,colour in ((250,20,(0,0,255)),(330,20,(0,255,0)),(420,200,yellow),(300,200,(0,0,0))):
                assert screen.pixel(x,y)==colour, ('mode 13h',x,y,screen.pixel(x,y),colour)
            assert screen.lit(0,320,48,336) and screen.lit(48,320,96,336), 'mode 13h text'
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,480))
            for x,y,colour in ((50,10,green),(100,120,red),(50,60,(0,0,0))):
                assert screen.pixel(x,y)==colour, ('mode 12h',x,y,screen.pixel(x,y),colour)
            assert screen.lit(0,320,48,336), 'mode 12h text'
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(20,100)==white)
            assert screen.pixel(20,102)==(87,255,255) and screen.lit(0,32,48,48), 'mode 4'
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt)
            assert 'GFX16: ok' in result and 'MODE13DOS13' in result, result
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.lit(0,0,72,16) and s.lit(0,16,32,32))
            # A graphics claim sets a GOP mode again; its release leaves the
            # firmware's console, which the next 16-bit program carries over.
            guest.command('gfxtest 1 keep','GFXTEST: 10 passed, 0 failed')
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400))
            guest.command('echo back on the firmware console','back on the firmware console')
            guest.command('hello16 vga','HELLO16: DOS 4.00, tail [ vga]')
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.lit(0,0,240,32))
            # An EFI program writing the firmware's console (the bootstrap,
            # refusing to start DOS twice) writes the VGA console instead.
            guest.command('cls')
            guest.command('\\efi\\boot\\bootia64','DOS bootstrap: DOS is already running')
            guest.command('if errorlevel 1 echo REFUSED','REFUSED')
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.lit(250,16,330,32))
            assert not screen.lit(250,0,330,16), 'bootstrap message not on its own line'
            assert free_memory(guest)==before, 'VDM memory not released'
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS dos16: {machine} IA-32 virtual-8086 mode, virtual ports, timer interrupt, COM/MZ loading, INT 21h/BIOS services, EXEC, redirection, VGA console with text and graphics modes via screendump, GOP and back, Montecito refusal',flush=True)

def win16(machine):
    # A Windows 3.0 (NE) program through WOW.DLL. From DOS its MZ stub runs in
    # VDM; under WIN.COM it shows a message box. Montecito has no IA-32.
    if not (ROOT/'build/media/WINDOWS/SYSTEM/WOW.DLL').exists():
        print(f'SKIP win16: {machine} (Interface Manager needs the WDK)',flush=True); return
    for cpu in (None,) if machine=='itanium-vpc' else (None,'madison-1500'):
        name=f'win16-{machine}{"-"+cpu if cpu else ""}'
        image=ROOT/'build'/f'{name}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
        sock=ROOT/'build'/f'qmp-{name}.sock'; sock.unlink(missing_ok=True)
        dump=ROOT/'build'/f'{name}.ppm'
        guest=Guest(image,ROOT/'build'/f'{name}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off')+(('-cpu',cpu) if cpu else ()))
        qmp=None
        try:
            qmp=Qmp(sock)
            ia32=machine=='itanium-vpc' or cpu
            if ia32: guest.command('c:\\windows\\w16test','This program requires Interface Manager.')
            before=free_memory(guest)
            guest.p.stdin.write(b'c:\\windows\\win w16test\r'); guest.p.stdin.flush()
            if ia32: guest.expect(b'W16TEST: started\n',timeout=90)
            caption,white=(0,0,128),(255,255,255)
            red=lambda s: any(s.pixel(x,y)==(255,0,0) for y in range(195,230) for x in range(140,200))
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(400,172)==caption and s.pixel(400,195)==white)
            assert red(screen)!=bool(ia32), 'message box icon'
            qmp.keys('ret')
            result=guest.expect(guest.prompt,timeout=60)
            if ia32: assert 'W16TEST: version 0003, message box 1' in result, result
            else: assert 'W16TEST' not in result, result
            assert 'leaked' not in result and 'WOW:' not in result, result
            assert free_memory(guest)==before, 'WIN leaked DOS memory'
            if ia32 and (ROOT/'build/media/WINDOWS/W16APP.EXE').exists():
                # W16APP, a C program built with Open Watcom: its window class,
                # menu, accelerator and dialog come from its NE resources.
                guest.p.stdin.write(b'c:\\windows\\win w16app\r'); guest.p.stdin.flush()
                guest.expect(b'W16APP: run-time library ok\n',timeout=90)
                guest.expect(b'W16APP: WM_CREATE "Win16 Test" 320x200\n',timeout=30)
                guest.expect(b'W16APP: painted\n',timeout=30)
                # Controls, subclassing, enumeration, a modeless dialog, timers
                # and an owner-drawn list box; GDI's structures; its library.
                index,result=guest.expect_any(b'W16APP: windows ok\n',b'FAILED\n',timeout=30)
                assert index==0, result
                for line in (b'W16APP: gdi ok\n',b'W16APP: library ok\n',b'W16APP: more ok\n',b'W16APP: hooks ok\n',b'W16APP: lzexpand ok\n',
                             b'W16APP: printing ok\n'):
                    index,more=guest.expect_any(line,b'FAILED\n',timeout=30)
                    result+=more; assert index==0, result
                for line in (b'W16APP: timer proc\n',b'W16APP: WM_TIMER\n',b'W16APP: WM_DRAWITEM 1234\n'):
                    if line.decode() not in result: result+=guest.expect(line,timeout=30)
                # A function WOW has no implementation of: said once, and the program goes on.
                stub='WOW: USER.GETSYSTEMDEBUGSTATE is not implemented; it returns 0\n'
                assert stub in result and 'WOW:' not in result.replace(stub,''), result
                screen=wait_screen(qmp,dump,lambda s: s.pixel(200,52)==caption and s.pixel(100,140)==(255,0,0))
                menu_text=sum(screen.pixel(x,y)==(0,0,0) for y in range(64,78) for x in range(48,136))
                assert screen.pixel(300,200)==white and menu_text>20, ('W16APP window',menu_text)
                qmp.keys('alt','a')
                guest.expect(b'W16APP: about\n',timeout=30)
                screen=wait_screen(qmp,dump,lambda s: s.pixel(110,106)==caption)
                assert any(screen.pixel(x,y)==(255,0,0) for y in range(118,146) for x in range(86,118)), 'dialog icon'
                qmp.keys('ret'); time.sleep(1)
                # The common dialogs from the File menu, through COMMDLG.
                desk=Desk(qmp)
                desk.keys('alt','f',pause=1); desk.keys('o',pause=3); desk.type('win.ini\n')
                guest.expect(b'W16APP: open c:\\windows\\win.ini win.ini 11\n',timeout=30)
                desk.keys('alt','f',pause=1); desk.keys('f',pause=3); desk.keys('ret')
                guest.expect(b'W16APP: font Helv 100 -13\n',timeout=30)
                desk.keys('alt','f',pause=1); desk.keys('c',pause=3); desk.keys('ret')
                guest.expect(b'W16APP: color ff0000\n',timeout=30)
                desk.keys('alt','f',pause=1); desk.keys('n',pause=1)
                guest.expect(b'W16APP: find dialog\n',timeout=30); time.sleep(2)
                desk.keys('ret'); guest.expect(b'W16APP: find hello down\n',timeout=30)
                desk.keys('esc'); guest.expect(b'W16APP: find closed\n',timeout=30); time.sleep(1)
                # Print: two copies of pages 2 to 3, to a file; DEVNAMES, DEVMODE and a DC come back.
                desk.keys('alt','f',pause=1); desk.keys('p',pause=3)
                desk.keys('alt','c'); desk.type('2'); desk.keys('alt','l'); desk.keys('ret')
                guest.expect(b'W16APP: print PSCRIPT, PostScript Printer, FILE: 2-3 x2 to file paper 1 dc\n',timeout=30)
                qmp.keys('alt','f'); time.sleep(1); qmp.keys('x')
                result=guest.expect(guest.prompt,timeout=60)
                assert 'W16APP: WM_DESTROY' in result and 'W16APP: exit 7' in result and 'W16DLL: WEP' in result, result
                assert 'leaked' not in result and 'WOW:' not in result, result
                assert free_memory(guest)==before, 'WIN leaked DOS memory'
                # LZCopy expanded LZTEST.TX_ (made by tools/mkszdd.py) to the original.
                expected=(ROOT/'tests/win16/lztest.txt').read_bytes().decode('ascii').replace('\r','')
                result=guest.command('type c:\\windows\\lztest.txt','End.')
                assert expected.strip() in result.replace('\r',''), result
        finally:
            if qmp: qmp.close()
            guest.close()
        if (machine=='itanium-vpc' or cpu) and (ROOT/'build/media/WINDOWS/W16APP.EXE').exists():
            # W16APP's page, printed through escapes in bands: two copies.
            from check_image import inspect
            data=inspect(image)['WINDOWS/W16APP.PS']
            assert b'/NumCopies 2 ' in data, data[:400]
            page,=printed(data,'W16APP',1)
            green,black=(0,255,0),(0,0,0)
            assert page.palette and page.pixel(250,175)==green and page.pixel(100,100)==black and page.pixel(50,50)==(255,255,255)
            assert page.count(black,100,300,200,316)>30
    print(f'PASS win16: {machine} NE programs under WOW.DLL: start-up, C run-time library, local and global memory, files, Catch/Throw, MessageBox, wsprintf, window class, CreateWindow, painting, menu, accelerator, dialog with icon, controls, subclassing, enumeration, modeless dialog, timers, owner-draw, GDI structures, mapping modes, DIBs and metafiles (a picture on the clipboard), a library (imports, LoadLibrary, callbacks, WEP), resources, atoms, properties, clipboard, superclass, MDI, hooks, LZEXPAND, printing through escapes, common dialogs (Open, Font, Color, Find, Print), an unimplemented function; MZ stub from DOS; Montecito refusal',flush=True)

def graphics(machine):
    image=ROOT/'build'/f'gfx-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    sock=ROOT/'build'/f'qmp-gfx-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'gfx-{machine}.ppm'
    # Firmware pointers are PS/2 only; itanium2-vpc defaults to USB input.
    guest=Guest(image,ROOT/'build'/f'gfx-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    try:
        qmp=Qmp(sock)
        result=guest.command('gfxtest','GFXTEST: 4 modes, pointer yes')
        modes=re.findall(r'GFXTEST: mode (\d) (\d+)x(\d+)( current)?',result)
        assert [(m[1],m[2]) for m in modes]==[('640','400'),('800','600'),('1024','768'),('1280','1024')], result
        assert modes[0][3]==' current', result
        wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.lit(0,0,640,400),what='the text console')
        guest.p.stdin.write(b'gfxtest 1 hold\r'); guest.p.stdin.flush()
        result=guest.expect(b'GFXTEST: holding\n')
        assert 'GFXTEST: drawn 800x600' in result and '[FAIL]' not in result, result
        back,red,green,white=(0,0,96),(255,0,0),(0,255,0),(255,255,255)
        screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(20,20)==red,what='GFXTEST\'s picture')
        for x,y,colour in ((20,20,red),(780,20,green),(400,300,white),(0,0,back),(8,8,back),(300,30,back),(4,592,back)):
            assert screen.pixel(x,y)==colour, (x,y,screen.pixel(x,y),colour)
        # Console text while claimed reaches the serial console only.
        assert all(screen.pixel(x,y)==back for y in range(100,400) for x in range(0,640,4) if not 384<=x<416 or not 284<=y<316)
        qmp.pointer(dx=12,dy=-7)
        result=guest.expect(b'buttons 0\n')
        assert re.search(r'GFXTEST: pointer \+\d+ -\d+ buttons 0',result), result
        qmp.pointer(button='left'); guest.expect(b'buttons 1\n')
        qmp.pointer(button='left',down=False); guest.expect(b'buttons 0\n')
        guest.p.stdin.write(b'x'); guest.p.stdin.flush(); guest.expect(b'GFXTEST: key 120 scan 0\n')
        guest.p.stdin.write(b'q'); guest.p.stdin.flush()
        result=guest.expect(guest.prompt); assert 'GFXTEST: released; 13 passed, 0 failed' in result, result
        wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.lit(0,0,64,16) and not s.lit(0,100,640,400),what='the text console restored')
        result=guest.command('gfxtest 3 keep','GFXTEST: 10 passed, 0 failed')
        wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400),what='the claim released at the image\'s end')
        result=guest.command('gfxtest','GFXTEST: mode 0 640x400 current')
        guest.command('taskhost taskapp.exe=E1','task 0 returned 1')
    finally:
        if qmp: qmp.close()
        guest.close()
    print(f'PASS graphics: {machine} modes, claim/release, pattern via screendump, text kept off the screen, pointer and keys, release at image end',flush=True)

def pe_modules(machine):
    image=ROOT/'build'/f'pe-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    guest=Guest(image,ROOT/'build'/f'pe-{machine}.log',machine)
    try:
        before=free_memory(guest)
        result=guest.command('peload','PELOAD: 0 pages and 0 files outstanding')
        lines=[l for l in result.splitlines() if l.startswith(('PETRACE:','PELOAD:'))]
        assert lines==['PETRACE: PEDLL2 attach','PETRACE: PEDLL attach','PETRACE: PETEST start',
                       'PETRACE: PEDYN attach','PETRACE: PEDYN detach',
                       'PELOAD: LoadLibrary NOSUCH failed (2) at NOSUCH.DLL ',
                       'PETRACE: PEFAIL attach refused','PELOAD: LoadLibrary PEFAIL failed (5) at PEFAIL.DLL DllMain',
                       'PETRACE: PETEST passed','PELOAD: exit 0','PELOAD: free 0, 3 modules remain',
                       'PETRACE: PEDLL detach','PETRACE: PEDLL2 detach','PELOAD: 0 pages and 0 files outstanding'], lines
        guest.command('peload c:\\wintest\\pebad.exe','failed (2) at PEDLL.DLL pedll_missing',error=True)
        guest.command('peload c:\\wintest\\pestrip.exe','failed (11) at PESTRIP.EXE',error=True)
        guest.command('peload c:\\wintest\\pedll.dll','failed (11) at PEDLL.DLL',error=True)
        guest.command('peload c:\\wintest\\nothing.exe','failed (2) at NOTHING.EXE',error=True)
        result=guest.command('peload','PELOAD: exit 0')
        assert free_memory(guest)==before, 'PE loads leaked DOS memory'
    finally:
        guest.close()
    print(f'PASS pe: {machine} WDK-built EXE/DLLs with imports by name/ordinal/data/forwarder, import cycle, DllMain order, LoadLibrary/GetProcAddress/FreeLibrary, resources, failures unwound',flush=True)

def make_iso(path, label, files):
    """ISO 9660 image from {relative path: bytes} with genisoimage (or xorriso)."""
    from cdfs_fixture import mkisofs
    tree=Path(str(path)+'.tree')
    if tree.exists(): shutil.rmtree(tree)
    for name,data in files.items():
        (tree/name).parent.mkdir(parents=True,exist_ok=True); (tree/name).write_bytes(data)
    mkisofs(tree,path,label)

def cdrom(machine):
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='cd-test-') as temporary:
        work=Path(temporary); media=work/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('DEVICE=C:\\EFICD.SYS /D:MSCD001\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        image=work/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        first,second=work/'first.iso',work/'second.iso'
        make_iso(first,'WIN30',{'README.TXT':b'WINDOWS SETUP CD\r\n','PROGRAMS/HELLO.EFI':(ROOT/'build/hello.efi').read_bytes(),
                                'DATA/BIG.BIN':bytes((i*7+3)&255 for i in range(70000))})
        make_iso(second,'SECOND',{'OTHER.TXT':b'second disc\r\n'})
        sock=work/'q.sock'; iface='scsi' if machine=='itanium-vpc' else 'ide'
        guest=Guest(image,ROOT/'build'/f'cd-{machine}.log',machine,
                    extra_args=('-drive',f'file={first},media=cdrom,if={iface},index=1,id=cd0','-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        try:
            qmp=Qmp(sock)
            guest.command('mscdex','usage: MSCDEX',error=True)
            guest.command('mscdex /d:nul','is not a CD-ROM driver',error=True)
            guest.command('mscdex /d:mscd001','Drive D: = Driver MSCD001 unit 0')
            guest.command('mscdex /d:mscd001','already installed',error=True)
            result=guest.command('dir d:\\','Volume in drive D is WIN30')
            assert 'README   TXT' in result and 'PROGRAMS     <DIR>' in result and 'DATA         <DIR>' in result, result
            guest.command('type d:\\readme.txt','WINDOWS SETUP CD')
            guest.command('d:\\programs\\hello from-cd','Command tail: from-cd')
            guest.command('copy d:\\data\\big.bin c:\\big.bin','1 file(s) copied.')
            result=guest.command('dir c:\\big.bin','BIG      BIN'); assert '70000' in result, result
            guest.command('echo x > d:\\new.txt','Access denied',error=True)
            guest.command('md d:\\newdir','Access denied',error=True)
            guest.command('del d:\\readme.txt','Access denied',error=True)
            guest.command('d:',prompt=b'D:\\>'); guest.command('cd programs',prompt=b'D:\\PROGRAMS>')
            guest.command('dir','HELLO    EFI',prompt=b'D:\\PROGRAMS>')
            guest.command('c:',prompt=b'C:\\>')
            qmp.execute('blockdev-change-medium',device='cd0',filename=str(second),format='raw'); time.sleep(1)
            result=guest.command('dir d:\\','Volume in drive D is SECOND'); assert 'OTHER    TXT' in result, result
            guest.command('type d:\\readme.txt','File not found',error=True)
            qmp.execute('eject',device='cd0',force=True); time.sleep(1)
            # Each failing call in DIR (label, listing) asks; fail them all.
            guest.p.stdin.write(b'dir d:\\\r'); guest.p.stdin.flush()
            result=guest.expect(b'Action: ',timeout=60)
            assert 'Critical disk error 21 on D:' in result, result
            for _ in range(4):
                guest.p.stdin.write(b'F'); guest.p.stdin.flush()
                index,text=guest.expect_any(b'Action: ',guest.prompt,timeout=60)
                if index==1: break
            else: raise AssertionError('DIR kept asking')
            qmp.execute('blockdev-change-medium',device='cd0',filename=str(first),format='raw'); time.sleep(1)
            guest.command('type d:\\readme.txt','WINDOWS SETUP CD')
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS cdrom: {machine} EFICD.SYS and MSCDEX, ISO 9660 drive D:, DIR/TYPE/COPY/EXEC from CD, read-only, disc change and eject',flush=True)

def install(machine):
    """The distribution: a DOS-only hard disk, then Interface Manager from its CD."""
    iso=ROOT/'build/dist/win30.iso'
    if not iso.exists():
        print(f'SKIP install: {machine} (make dist with the WDK builds the Interface Manager CD)',flush=True); return
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='install-test-') as temporary:
        work=Path(temporary); hdd=work/'hdd.img'; shutil.copyfile(ROOT/'build/dist/dos.img',hdd)
        iface='scsi' if machine=='itanium-vpc' else 'ide'
        cd=('-drive',f'file={iso},media=cdrom,if={iface},index=1,id=cd0')
        guest=Guest(hdd,ROOT/'build'/f'install-{machine}.log',machine,ready=b'C:\\>',extra_args=cd)
        try:
            assert 'Drive D: = Driver MSCD001 unit 0' in guest.boot, guest.boot
            guest.command('path','PATH=C:\\DOS')
            guest.command('dir c:\\windows','File not found',error=True)
            result=guest.command('d:\\setup /q','Interface Manager is installed in C:\\WINDOWS.')
            assert 'C:\\WINDOWS\\SYSTEM\\USER.DLL' in result, result
            guest.command('type c:\\autoexec.bat','PATH C:\\DOS;C:\\WINDOWS')
            guest.command('type c:\\windows\\system.ini','shell=progman.exe')
            guest.command('type c:\\windows\\win.ini','device=PostScript Printer,PSCRIPT,LPT1:')
            guest.command('dir c:\\windows\\system','KERNEL   DLL')
            guest.p.stdin.write(b'd:\\setup\r'); guest.p.stdin.flush()
            guest.expect(b'Interface Manager is already there. Replace it (Y/N)? ')
            guest.p.stdin.write(b'n'); guest.p.stdin.flush()
            guest.expect(b'Setup was cancelled.')
            guest.expect(guest.prompt)
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=30*SLOW)
        finally:
            guest.close()
        sock=work/'q.sock'
        guest=Guest(hdd,ROOT/'build'/f'install-boot-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    ready=b'C:\\>',extra_args=cd+('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        try:
            qmp=Qmp(sock)
            guest.command('path','PATH=C:\\DOS;C:\\WINDOWS')
            guest.p.stdin.write(b'win hellowin\r'); guest.p.stdin.flush()
            wait_screen(qmp,work/'screen.ppm',lambda s: (s.width,s.height)==(800,600) and s.pixel(300,32)==(0,0,128))
            qmp.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60); assert 'WIN:' not in result, result
            # WIN alone: the SYSTEM.INI shell, Program Manager; Alt+F4 and Enter end the session.
            guest.p.stdin.write(b'win\r'); guest.p.stdin.flush()
            wait_screen(qmp,work/'screen.ppm',lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==(0,0,128),timeout=180)
            time.sleep(2); qmp.keys('alt','f4'); time.sleep(1.5); qmp.keys('ret')
            result=guest.expect(guest.prompt,timeout=60); assert 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS install: {machine} DOS-only disk with CD-ROM, Interface Manager SETUP from the CD, PATH, restart, WIN and Program Manager',flush=True)

def wait_screen(qmp, path, ready, timeout=60, what='the expected screen'):
    """The screen once ready(screen) holds: what the guest draws, it draws in its own time."""
    deadline = time.monotonic()+timeout*SLOW
    while True:
        screen = qmp.screen(path)
        if ready(screen): return screen
        if time.monotonic() > deadline: raise AssertionError(f'the screen never showed {what}')
        time.sleep(0.5)

def settled(qmp, path, quiet=0.5, timeout=30):
    """The screen once it stops changing (a blinking caret aside), for what is measured on it."""
    deadline = time.monotonic()+timeout*SLOW
    last = qmp.screen(path)
    while True:
        time.sleep(quiet)
        screen = qmp.screen(path)
        if (screen.width, screen.height) == (last.width, last.height):
            row = screen.width*3
            changed = [y for y in range(screen.height) if screen.rgb[y*row:(y+1)*row] != last.rgb[y*row:(y+1)*row]]
            if len(changed) <= 32 and sum(screen.rgb[y*row+x:y*row+x+3] != last.rgb[y*row+x:y*row+x+3]
                                          for y in changed for x in range(0, row, 3)) <= 64:
                return screen
        if time.monotonic() > deadline: return screen
        last = screen

def windows(machine):
    image=ROOT/'build'/f'windows-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/WIN.COM').exists():
        print(f'SKIP windows: {machine} (Interface Manager needs the WDK)',flush=True); return
    sock=ROOT/'build'/f'qmp-windows-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'windows-{machine}.ppm'
    guest=Guest(image,ROOT/'build'/f'windows-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    try:
        qmp=Qmp(sock)
        before=free_memory(guest)
        guest.p.stdin.write(b'c:\\windows\\win hellowin\r'); guest.p.stdin.flush()
        # The start-up screen while the system loads: the blue-violet ground, the name in
        # white with its gray shadow, the credits at the bottom.
        ground,white,gray=(84,84,168),(255,255,255),(128,128,128)
        splash=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(5,5)==ground,timeout=60)
        assert splash.pixel(400,60)==ground and splash.pixel(700,400)==ground, 'start-up screen'
        name=[splash.pixel(x,y) for y in range(186,276) for x in range(100,700)]
        assert name.count(white)>1500 and name.count(gray)>300, (name.count(white),name.count(gray))
        assert sum(splash.pixel(x,y)==white for y in range(530,570) for x in range(100,700))>200, 'credits'
        # The default window is 600x450 at (24,24) on 800x600; its caption starts below a 4-pixel frame.
        caption=(0,0,128)
        screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(300,32)==caption and s.lit(240,240,400,280))
        desktop,frame,white,face=(192,192,192),(0,0,0),(255,255,255),(192,192,192)
        expect={(5,5):desktop,(700,300):desktop,(24,200):frame,(26,200):face,(27,200):frame,(623,200):frame,
                (100,30):caption,(29,30):face,(604,30):face,(36,37):white,(300,46):frame,(60,100):white,(500,400):white,(300,473):frame}
        for (x,y),colour in expect.items(): assert screen.pixel(x,y)==colour, (x,y,screen.pixel(x,y),colour)
        text=[(x,y) for y in range(240,280) for x in range(240,400) if screen.pixel(x,y)==(0,0,0)]
        assert len(text)>100 and min(x for x,_ in text)>250 and max(x for x,_ in text)<390, len(text)
        title=[(x,y) for y in range(28,48) for x in range(200,450) if screen.pixel(x,y)==white]
        assert len(title)>50, len(title)
        qmp.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400),what='the text console')
        assert free_memory(guest)==before, 'WIN leaked DOS memory'
        guest.command('c:\\windows\\win nothing','cannot start nothing (2)',error=True)
        assert free_memory(guest)==before
        # "WIN :" starts without the start-up screen.
        guest.p.stdin.write(b'c:\\windows\\win : hellowin\r'); guest.p.stdin.flush()
        deadline=time.monotonic()+120
        while True:
            screen=qmp.screen(dump)
            assert screen.pixel(5,5)!=ground, 'the start-up screen with WIN :'
            if (screen.width,screen.height)==(800,600) and screen.pixel(300,32)==caption: break
            assert time.monotonic()<deadline, 'HELLOWIN after WIN :'
        qmp.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        if qmp: qmp.close()
        guest.close()
    print(f'PASS windows: {machine} WIN.COM and its start-up screen (none with :), KERNEL/USER/GDI from the WDK, HELLOWIN window frame, caption and text via screendump, Alt+F4 exit, memory returned',flush=True)

class PrintedPage:
    """One page PSCRIPT printed: an image, ASCII85 then run-length encoded,
    of 1-bit gray, 8-bit indexes into RGB colors, or 8-bit RGB samples."""
    def __init__(self, text):
        head, body = text.split('image\n', 1)
        self.width, self.height, self.bits = (int(v) for v in re.search(
            r'/Width (\d+) /Height (\d+) /BitsPerComponent (\d+)', head).groups())
        indexed = re.search(r'\[/Indexed /DeviceRGB (\d+) <([0-9A-F\s]+)>\]', head)
        self.palette = None
        if indexed:
            colors = bytes.fromhex(''.join(indexed.group(2).split()))
            self.palette = [tuple(colors[i:i+3]) for i in range(0, len(colors), 3)]
            assert len(self.palette) == int(indexed.group(1))+1
        self.components = 3 if '/DeviceRGB setcolorspace' in head else 1
        raw, samples, i = base64.a85decode(body.split('~>', 1)[0]), bytearray(), 0
        while raw[i] != 128:
            if raw[i] < 128: samples += raw[i+1:i+2+raw[i]]; i += raw[i]+2
            else: samples += raw[i+1:i+2]*(257-raw[i]); i += 2
        self.samples = bytes(samples)
        self.row = (self.width+7)//8 if self.bits == 1 else self.width*self.components
        assert len(self.samples) == self.row*self.height, (len(self.samples), self.row, self.height)
    def pixel(self, x, y):
        if self.bits == 1:
            v = 255 if self.samples[y*self.row+x//8] & (0x80 >> (x%8)) else 0
            return (v, v, v)
        if self.palette: return self.palette[self.samples[y*self.row+x]]
        return tuple(self.samples[y*self.row+x*3:y*self.row+x*3+3])
    def count(self, colour, x0, y0, x1, y1):
        return sum(self.pixel(x, y) == colour for y in range(y0, y1) for x in range(x0, x1))

def printed(data, title, pages):
    """A PSCRIPT document's pages, after its structure is checked."""
    text = data.decode('ascii')
    assert text.startswith('%!PS-Adobe-3.0\n') and f'%%Title: {title}\n' in text, text[:300]
    assert text.endswith(f'%%Trailer\n%%Pages: {pages}\n%%EOF\n'), text[-100:]
    result = [PrintedPage(part) for part in text.split('%%Page: ')[1:]]
    assert len(result) == pages, len(result)
    return result

def gdi(machine):
    """GDITEST checks GDI on memory DCs itself, then draws a window read back here."""
    image=ROOT/'build'/f'gdi-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/GDITEST.EXE').exists():
        print(f'SKIP gdi: {machine} (Interface Manager needs the WDK)',flush=True); return
    sock=ROOT/'build'/f'qmp-gdi-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'gdi-{machine}.ppm'
    guest=Guest(image,ROOT/'build'/f'gdi-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    try:
        qmp=Qmp(sock)
        guest.p.stdin.write(b'c:\\windows\\win gditest\r'); guest.p.stdin.flush()
        result=guest.expect(b'GDITEST: ',timeout=90)+guest.expect(b'\n')
        assert 'checks passed' in result, result
        # The window is at (0,0); its client area starts below the 4-pixel frame and the caption.
        def at(x,y): return screen.pixel(x+4,y+23)
        red,white,blue,green,yellow,black,magenta=(255,0,0),(255,255,255),(0,0,255),(0,255,0),(255,255,0),(0,0,0),(255,0,255)
        screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(4+120,23+100)==white and s.pixel(4+95,23+75)==magenta)
        expect={(35,35):red,(11,11):white,(85,25):blue,(115,25):green,(129,39):green,(131,25):white,
                (140,10):yellow,(141,10):red,(140,11):red,(20,80):black,(60,80):red,(20,120):green,(60,120):blue,
                (95,75):magenta,(120,100):white,(155,135):white,(149,129):magenta}
        for (x,y),colour in expect.items(): assert at(x,y)==colour, (x,y,at(x,y),colour)
        text=[(x,y) for y in range(150,166) for x in range(10,160) if at(x,y)==black]
        assert len(text)>100 and max(x for x,_ in text)<10+15*9+2, len(text)
        qmp.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        if qmp: qmp.close()
        guest.close()
    # What GDITEST printed: four Letter pages at 150 dots per inch, the first
    # two with 3 and 2 colors, the third with more than 256, the last black and white.
    from check_image import inspect
    files=inspect(image)
    assert 'WINDOWS/GDIABORT.PS' not in files, 'an aborted document was kept'
    pages=printed(files['WINDOWS/GDITEST.PS'],'GDITEST',4)
    kinds=[(p.width,p.height,len(p.palette) if p.palette else p.components,p.bits) for p in pages]
    assert kinds==[(1200,1575,3,8),(1200,1575,2,8),(1200,1575,3,8),(1200,1575,1,1)], kinds
    assert not pages[2].palette, 'the third page is RGB'
    first,second,third,last=pages
    assert first.pixel(300,200)==red and first.pixel(150,150)==black and first.pixel(449,299)==black and first.pixel(100,100)==white
    assert first.count(black,150,400,300,416)>40 and first.count(red,150,400,300,416)==0
    assert second.pixel(600,30)==blue and second.pixel(600,80)==white
    assert third.pixel(200,150)==(128,128,128) and third.pixel(50,50)==white and third.pixel(300,200)==white
    assert third.pixel(100,400)==(0,0,0) and third.pixel(300,400)==(200,0,0) and third.pixel(399,400)==(43,0,255)
    assert last.count(black,150,150,300,166)>40 and last.pixel(600,800)==white
    # The metafile GDITEST copied to a file: a disk metafile's header, then records
    # (sizes in words) ending with an empty one, as Windows writes them.
    wmf=files['WINDOWS/GDITEST.WMF']
    kind,header,version,size,objects,largest,_=struct.unpack_from('<HHHIHIH',wmf,0)
    assert (kind,header,version,size*2)==(2,9,0x300,len(wmf)), (kind,header,version,size,len(wmf))
    functions,at=[],18
    while at<len(wmf):
        words,function=struct.unpack_from('<IH',wmf,at); functions.append(function); at+=words*2
        if not function: break
    assert at==len(wmf) and functions[-1]==0 and max(objects,1)<=8, (at,functions,objects)
    for function in (0x02fc,0x02fa,0x012d,0x041b,0x0418,0x0521,0x06ff,0x0228,0x0940,0x0b41,0x001e,0x0127,0x01f0):
        assert function in functions, (hex(function),functions)
    print(f'PASS gdi: {machine} shapes, ROPs, clipping, regions, bitmaps, mono/color BitBlt, StretchBlt, DIBs, fonts, mapping modes, arcs, flood fills, palettes, metafiles (recorded, played, enumerated, a file); window read back; printing through PSCRIPT (escapes, StartDoc, bands, abort) decoded',flush=True)

class Desk:
    """Keyboard and mouse for an Interface Manager test: the pointer starts at the screen's centre.
    Keys wait in the firmware's buffer, but the mouse buttons' state is polled: a click made while
    the guest is busy can pass unseen, so a click, and typed text, wait first for the screen to
    stop changing."""
    def __init__(self, qmp, x=400, y=300):
        self.qmp, self.x, self.y = qmp, x, y
        self.dump = Path(str(qmp.path)+'.ppm')
    def idle(self):
        settled(self.qmp, self.dump, quiet=0.25, timeout=3)
    def keys(self, *codes, pause=0.4):
        self.qmp.keys(*codes); time.sleep(pause)
    def type(self, text):
        self.idle()
        names = {' ': 'spc', '.': 'dot', '\\': 'backslash', '/': 'slash', '\n': 'ret', '-': 'minus', ',': 'comma', ':': ('shift', 'semicolon')}
        for c in text:
            code = ('shift', c.lower()) if c.isupper() else c if c.isalnum() else names[c]
            self.keys(*(code if isinstance(code, tuple) else (code,)), pause=0.2)
    def move(self, x, y):
        self.qmp.pointer(dx=x-self.x, dy=y-self.y); self.x, self.y = x, y; time.sleep(0.3)
    def click(self, x, y, button='left', double=False):
        # The firmware reports the buttons' state when polled, so each
        # transition needs a moment of its own; a double click's must fit
        # in the double-click time.
        self.move(x, y); self.idle()
        hold = 0.12 if double else 0.25
        for _ in range(2 if double else 1):
            self.qmp.pointer(button=button, down=True); time.sleep(hold)
            self.qmp.pointer(button=button, down=False); time.sleep(hold)
        time.sleep(0.5)
    def double_click(self, x, y, done, after=None):
        """A double click, then done(), which raises AssertionError when its effect does not
        come: on a host so loaded that the guest missed a button, it saw single clicks, and
        after Esc closes what one opened (an icon's system menu) the double click is made again."""
        for attempt in range(3):
            self.click(x, y, double=True)
            if after: after()
            try: return done()
            except AssertionError:
                if attempt == 2: raise
                self.keys('esc'); self.keys('esc')

def crt(machine):
    """CRTTEST checks the C run-time library programs link with: formatting and scanning
    (floating point too), conversions, mathematics, the heap, streams and handles in text
    and binary modes, time and TZ's zones, strings, sorting, paths, the environment and
    directories. With 4 GiB of memory, the firmware's pages are above 2 GiB, and a program's
    must not be."""
    image=ROOT/'build'/f'crt-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/CRTTEST.EXE').exists():
        print(f'SKIP crt: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest=Guest(image,ROOT/'build'/f'crt-{machine}.log',machine,extra_args=('-m','4G'))
    try:
        guest.p.stdin.write(b'c:\\windows\\win crttest\r'); guest.p.stdin.flush()
        result=guest.expect(b'CRTTEST: ',timeout=90)+guest.expect(b' failures',timeout=60)
        checks=result.count(' ok')
        assert 'FAILED' not in result and 'CRTTEST: 0 failures' in result and checks>=45, result
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        guest.close()
    print(f'PASS crt: {machine} CRTTEST {checks} checks of the C run-time library, memory below 2 GiB with 4 GiB',flush=True)

def packing(machine):
    """PACKTEST, written as for Windows 3.0 and built with WIN16_MESSAGES: what USER sends its
    procedures, and what they send and pass on, packed as Windows 3.0 packs it; memory handles
    in a WORD; DDE with a native server and a native client; message hooks; the message loop."""
    image=ROOT/'build'/f'packing-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/PACKTEST.EXE').exists():
        print(f'SKIP packing: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest=Guest(image,ROOT/'build'/f'packing-{machine}.log',machine,extra_args=('-m','4G'))
    try:
        guest.p.stdin.write(b'c:\\windows\\win packtest\r'); guest.p.stdin.flush()
        result=guest.expect(b'PACKTEST: ',timeout=90)+guest.expect(b' failures',timeout=60)
        checks=result.count(' ok')
        assert 'FAILED' not in result and 'PACKTEST: 0 failures' in result and checks>=22, result
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        guest.close()
    print(f'PASS packing: {machine} PACKTEST {checks} checks of Windows 3.0 message packing',flush=True)

def samples(machine,win16=False,packed16=False):
    """Run Open Watcom samples as native, Win16 or WIN16_MESSAGES builds.
    Cover generic message/About boxes, edit fonts/file dialogs, datactl controls,
    life bitmap menus/LIF files, WATZEE dialogs and helpex help. Use 4 GiB to
    check pointers stored in LONGs; Win16 requires an IA-32-capable CPU."""
    kind='16' if win16 else 'n16' if packed16 else ''
    name=f'samples{kind}-{machine}'; folder='c:\\samples'+({'16':'\\win16','n16':'\\n16'}.get(kind,''))
    image=ROOT/'build'/f'{name}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/SAMPLES'/(('WIN16/' if win16 else 'N16/' if packed16 else '')+'GENERIC.EXE')).exists():
        print(f'SKIP {name.split("-")[0]}: {machine} (they need the WDK and Open Watcom)',flush=True); return
    sock=ROOT/'build'/f'qmp-{name}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'{name}.ppm'
    cpu=('-cpu','madison-1500') if win16 and machine=='itanium2-vpc' else ()
    guest=Guest(image,ROOT/'build'/f'{name}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off','-m','4G')+cpu)
    qmp=None
    navy,white,gray,black,green=(0,0,128),(255,255,255),(192,192,192),(0,0,0),(0,128,0)
    try:
        qmp=Qmp(sock); d=Desk(qmp)
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        def start(program,ready=lambda s: s.pixel(60,37)==navy):
            guest.p.stdin.write(f'c:\\windows\\win {folder}\\{program}\r'.encode()); guest.p.stdin.flush()
            shows(ready,timeout=120); time.sleep(1)
        def leave():
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result and 'WOW:' not in result, result
        start('generic')
        d.keys('alt','i'); d.keys('ret')
        shows(lambda s: s.pixel(400,173)==navy and not any(s.pixel(x,y)==black for y in range(216,229) for x in range(310,490)))
        d.keys('ret'); shows(lambda s: s.pixel(400,173)==gray)
        d.keys('alt','h'); d.keys('ret'); shows(lambda s: s.pixel(250,75)==navy)
        d.keys('ret'); shows(lambda s: s.pixel(250,75)==white)
        leave()
        start('edit')
        d.keys('alt','o'); d.keys('c'); shows(lambda s: s.pixel(300,108)==navy and s.pixel(100,166)==navy)
        d.keys('esc'); shows(lambda s: s.pixel(300,108)==white)
        d.keys('alt','f'); d.keys('o'); shows(lambda s: s.pixel(300,98)==navy)
        d.keys('esc'); shows(lambda s: s.pixel(300,98)!=navy)
        leave()
        start('datactl')
        d.keys('alt','i'); d.keys('d'); shows(lambda s: s.pixel(500,153)==navy)
        d.keys('esc'); shows(lambda s: s.pixel(500,153)!=navy)
        leave()
        # The second column's first pattern, selected (inverted), past a bar.
        start('life',lambda s: s.pixel(150,88)==navy)
        d.keys('alt','p'); d.keys('down'); d.keys('down'); d.keys('right')
        shows(lambda s: s.pixel(306,130)==black and s.pixel(310,155)==black and s.pixel(301,300)==black and s.pixel(300,130)==white)
        d.keys('esc'); d.keys('esc')
        leave()
        # One player, A, then the game's score sheet.
        start('watzee',lambda s: s.pixel(200,178)==navy)
        d.keys('ret'); shows(lambda s: s.pixel(140,168)==navy)
        d.click(147,262); d.click(380,350); shows(lambda s: s.pixel(170,100)==black and s.pixel(140,168)!=navy)
        leave()
        # Help Index: WinHelp shows HELPEX.HLP's contents, its links green.
        start('helpex')
        d.keys('alt','h'); d.keys('i')
        shows(lambda s: sum(s.pixel(x,y)==green for y in range(120,340,2) for x in range(40,340,2))>100)
        d.keys('alt','f4'); shows(lambda s: s.pixel(60,37)==navy)
        leave()
        for program in ('iconview','shootgal','alarm'):
            start(program); leave()
    finally:
        if qmp: qmp.close()
        guest.close()
    kind='Win16, through WOW' if win16 else 'native from their Win16 code, with Windows 3.0 packing' if packed16 else 'native'
    print(f'PASS {name.split("-")[0]}: {machine} Open Watcom generic, edit, datactl, life, watzee, helpex (WinHelp), iconview, shootgal, alarm, {kind}, with 4 GiB',flush=True)

def samples16(machine): samples(machine,win16=True)
def samplesn16(machine): samples(machine,packed16=True)

def user(machine):
    """USERTEST checks USER and KERNEL itself, then menus, a dialog, a combo box and message boxes
    are driven with the keyboard and the mouse."""
    image=ROOT/'build'/f'user-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/USERTEST.EXE').exists():
        print(f'SKIP user: {machine} (Interface Manager needs the WDK)',flush=True); return
    sock=ROOT/'build'/f'qmp-user-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'user-{machine}.ppm'
    guest=Guest(image,ROOT/'build'/f'user-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    try:
        qmp=Qmp(sock); d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win usertest\r'); guest.p.stdin.flush()
        result=guest.expect(b'USERTEST: ready',timeout=180)
        assert 'checks passed' in result and 'FAIL' not in result, result
        time.sleep(1)
        # Alt+F opens File, D its Dialog item; type into the edit, tab to the check box.
        d.keys('alt','f'); wait_screen(qmp,dump,lambda s: s.pixel(30,50)==(0,0,128) and s.pixel(150,100)==(255,255,255),what='the File menu')
        d.keys('d'); time.sleep(1.5)
        d.type('abc'); d.keys('tab'); d.keys('spc'); d.keys('ret')
        guest.expect(b'USERTEST: dialog 1 abc checked',timeout=30)
        # The accelerator, the combo box's list by mouse, then Escape.
        d.keys('ctrl','d'); time.sleep(1.5)
        d.click(110,183); wait_screen(qmp,dump,lambda s: s.pixel(100,206)==(255,255,255) and s.pixel(36,200)==(0,0,0),what='the dropped list')
        d.click(80,202); d.keys('esc')
        guest.expect(b'USERTEST: dialog 2  unchecked',timeout=30)
        # A message box answered with its mnemonic.
        d.keys('alt','f'); d.keys('m'); time.sleep(1.5)
        wait_screen(qmp,dump,lambda s: s.pixel(330,170)==(0,0,128),what='the message box')
        d.keys('n'); guest.expect(b'USERTEST: message box 7',timeout=30)
        # Help, About with the mouse, then Enter and Alt+F4.
        d.click(75,31); d.click(90,50); time.sleep(1.5)
        guest.expect(b'USERTEST: command 105',timeout=30)
        d.keys('ret'); time.sleep(1)
        # The journal: keys recorded up to Enter (Shift with the first),
        # then played back into an edit control.
        d.keys('ctrl','j'); guest.expect(b'USERTEST: recording',timeout=30)
        d.keys('shift','h'); d.keys('i'); d.keys('ret')
        result=guest.expect(b'USERTEST: journal',timeout=30)+guest.expect(b'\n',timeout=30)
        assert 'played Hi' in result, result
        # The Owner menu: two items USERTEST measures and draws (red, blue; green
        # selected), a bar, then a column with a magenta bitmap (inverted selected).
        d.keys('alt','o')
        wait_screen(qmp,dump,lambda s: (s.pixel(100,50),s.pixel(100,75),s.pixel(183,70),s.pixel(210,50))==((0,255,0),(0,0,255),(0,0,0),(255,0,255)),what='the owner-drawn menu')
        d.keys('right')
        wait_screen(qmp,dump,lambda s: (s.pixel(100,50),s.pixel(210,50))==((255,0,0),(0,255,0)),what='the bitmap item selected')
        d.keys('ret'); guest.expect(b'USERTEST: command 110',timeout=30)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        if qmp: qmp.close()
        guest.close()
    print(f'PASS user: {machine} USERTEST checks (hooks among them), menus by keyboard and mouse, accelerators, dialog, combo box, message box, journal record and playback, owner-drawn and bitmap menu items in columns, exit',flush=True)

def progman(machine):
    """WIN alone starts Program Manager: run items by double-click, File Run and a new item, then
    Exit Interface Manager saves PROGMAN.INI and returns to DOS."""
    image=ROOT/'build'/f'progman-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    windows=ROOT/'build/media/WINDOWS'
    if not (windows/'PROGMAN.EXE').exists():
        print(f'SKIP progman: {machine} (Interface Manager needs the WDK)',flush=True); return
    sock=ROOT/'build'/f'qmp-progman-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'progman-{machine}.ppm'
    guest=Guest(image,ROOT/'build'/f'progman-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    blue=(0,0,128)
    def shows(test, timeout=90):
        return wait_screen(qmp,dump,test,timeout)
    # A program started: Program Manager inactive, and another window's caption active
    # (Program Manager's goes inactive before the program's window is up).
    # File Run's dialog, whose caption is at y=75, must have gone first.
    def launched(s): return (s.pixel(60,10)!=blue and sum(s.pixel(x,75)==blue for x in range(60,480,4))<60 and
                             any(sum(s.pixel(x,y)==blue for x in range(0,800,4))>40 for y in range(20,560,2)))
    try:
        qmp=Qmp(sock); d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
        # Program Manager, with Main active (pixels left of the titles); Clipboard follows File Manager,
        # Control Panel and Print Manager in Main.
        shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==blue and s.pixel(60,62)==blue,timeout=180)
        time.sleep(2)
        index=sum((windows/f).exists() for f in ('WINFILE.EXE','CONTROL.EXE','PRINTMAN.EXE'))
        d.double_click(58+84*index,92,lambda: shows(launched,timeout=20))
        d.keys('alt','f4'); shows(lambda s: s.pixel(60,10)==blue)
        d.keys('alt','f'); d.keys('r'); time.sleep(1.5)
        d.type('clipbrd'); d.keys('ret')
        shows(launched)
        d.keys('alt','f4'); shows(lambda s: s.pixel(60,10)==blue)
        # A new item in Main, then run it.
        d.keys('alt','f'); d.keys('n'); time.sleep(1.5); d.keys('ret'); time.sleep(1.5)
        d.type('hello'); d.keys('tab'); d.type('hellowin.exe'); d.keys('ret'); time.sleep(1.5)
        d.keys('end'); d.keys('ret')
        shows(launched)
        d.keys('alt','f4'); shows(lambda s: s.pixel(60,10)==blue)
        # Overlapping windows: Calculator over Main, then Program Manager
        # brought forward repaints Main and its icons where Calculator was;
        # moved, it repaints its minimized groups too.
        d.keys('alt','f'); d.keys('r'); time.sleep(1.5)
        d.type('calc'); d.keys('ret')
        shows(lambda s: launched(s) and s.pixel(100,180)==(192,192,192))
        d.click(500,300)
        shows(lambda s: s.pixel(60,10)==blue and s.pixel(100,180)==(255,255,255) and s.pixel(200,150)==(255,255,255))
        d.move(320,12); qmp.pointer(button='left'); time.sleep(0.3)
        for i in range(1,6): d.move(320+150*i//5,12+120*i//5)
        qmp.pointer(button='left',down=False)
        shows(lambda s: s.pixel(210,130)==blue and s.pixel(240,585)==(255,255,232) and
              any(s.pixel(x,y) not in ((255,255,232),(192,192,192)) for y in range(530,555) for x in range(185,215)))
        d.click(300,105); shows(lambda s: s.pixel(210,130)!=blue) # Calculator's caption, cascaded
        d.keys('alt','f4'); shows(lambda s: s.pixel(210,130)==blue)
        # Accessories' icon double-clicked opens the group (the system menu the first click
        # brings up goes): the icon's place is bare.
        d.double_click(198,542,lambda: shows(lambda s: all(s.pixel(x,y)==(255,255,232) for y in range(530,555,2) for x in range(185,215,2)),timeout=20),
                       after=lambda: d.move(500,300))
        if machine=='itanium-vpc':
            # A Win16 program makes a group and an item through DDE, as a
            # setup program does, and asks for the group's items.
            d.keys('alt','f'); d.keys('r'); time.sleep(1.5)
            d.type('w16app dde'); d.keys('ret')
            index,result=guest.expect_any(b'W16APP: dde ok',b'FAILED',timeout=90)
            assert index==0, result+guest.expect(b'\n',timeout=10)
        # Exit Interface Manager, saving the groups.
        d.keys('alt','f4'); time.sleep(1.5); d.keys('ret')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('type c:\\windows\\progman.ini','Group1=Main')
        assert 'hello,hellowin.exe' in result and 'Group2=Accessories' in result, result
        if machine=='itanium-vpc': assert 'Win16 Group' in result and 'Notes (16-bit),notepad.exe' in result, result
    finally:
        if qmp: qmp.close()
        guest.close()
    dde=', DDE from a Win16 program' if machine=='itanium-vpc' else ''
    print(f'PASS progman: {machine} Program Manager as the shell: default groups, run by double-click, Run dialog, new item, repainting under other windows and after a move, a group opened from its icon{dde}, Exit Interface Manager, PROGMAN.INI',flush=True)

def notepad(machine):
    """NOTEPAD: typing, Time/Date, Save As, Find down and up, Word Wrap, the save prompt, Open and Save,
    then the file from DOS; a file named on the command line, and changes discarded."""
    image=ROOT/'build'/f'notepad-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    if not (ROOT/'build/media/WINDOWS/NOTEPAD.EXE').exists():
        print(f'SKIP notepad: {machine} (Interface Manager needs the WDK)',flush=True); return
    sock=ROOT/'build'/f'qmp-notepad-{machine}.sock'; sock.unlink(missing_ok=True)
    dump=ROOT/'build'/f'notepad-{machine}.ppm'
    guest=Guest(image,ROOT/'build'/f'notepad-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    qmp=None
    # The window is at (24,24) and 600x450: caption, then the menu bar; the horizontal
    # scroll bar's shaft is at y=462 while Word Wrap is off.
    active=lambda s: (s.width,s.height)==(800,600) and s.pixel(100,36)==(0,0,128)
    try:
        qmp=Qmp(sock); d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win notepad\r'); guest.p.stdin.flush()
        screen=wait_screen(qmp,dump,active,timeout=120); time.sleep(1)
        assert screen.pixel(100,462)==(192,192,192), 'horizontal scroll bar'
        d.type('hello world\nsecond line\n'); d.keys('f5')
        wait_screen(qmp,dump,lambda s: any(s.pixel(x,y)==(0,0,0) for y in range(106,118) for x in range(40,160,2)),what='the time and date')
        d.keys('alt','f'); d.keys('a'); time.sleep(1.5)
        d.type('C:\\NOTE.TXT\n'); time.sleep(2)
        # Find "second" from the top and type over it; then find "hello" upwards from the end.
        d.keys('ctrl','home'); d.keys('alt','s'); d.keys('f'); time.sleep(1.5)
        d.type('second\n'); time.sleep(1); d.type('x')
        d.keys('ctrl','end'); d.keys('alt','s'); d.keys('f'); time.sleep(1.5)
        d.click(227,185)   # Up, inside the Direction group box
        d.keys('alt','n'); d.type('hello\n'); time.sleep(1); d.type('Hello')
        d.keys('alt','e'); d.keys('w'); time.sleep(1.5)
        wait_screen(qmp,dump,lambda s: s.pixel(100,462)==(255,255,255),what='no horizontal scroll bar with Word Wrap')
        # New asks to save the changes; Yes saves. Then open the file again, add to it and save.
        d.keys('alt','f'); d.keys('n'); time.sleep(1.5)
        wait_screen(qmp,dump,lambda s: s.pixel(330,169)==(0,0,128),what='the save changes prompt')
        d.keys('y'); time.sleep(2)
        d.keys('alt','f'); d.keys('o'); time.sleep(1.5)
        d.type('C:\\NOTE.TXT\n'); time.sleep(2)
        d.keys('ctrl','end'); d.type('end'); d.keys('alt','f'); d.keys('s'); time.sleep(2)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('type c:\\note.txt','Hello world')
        assert 'x line' in result and 'end' in result and ('AM' in result or 'PM' in result), result
        # The command line names the file; changes are discarded with No.
        guest.p.stdin.write(b'c:\\windows\\win notepad c:\\note.txt\r'); guest.p.stdin.flush()
        wait_screen(qmp,dump,active,timeout=120); time.sleep(1)
        d.type('zzz'); d.keys('alt','f4'); time.sleep(1.5)
        d.keys('n')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('type c:\\note.txt','Hello world'); assert 'zzz' not in result, result
        # Printing: Page Setup as it is; Print Setup picks the printer's FILE: port (kept in
        # WIN.INI), so Print asks for the file's name; the Cancel box is up while it prints.
        guest.p.stdin.write(b'c:\\windows\\win notepad c:\\note.txt\r'); guest.p.stdin.flush()
        wait_screen(qmp,dump,active,timeout=120); time.sleep(1)
        d.keys('alt','f'); d.keys('t'); time.sleep(1.5); d.keys('ret'); time.sleep(1)
        d.keys('alt','f'); d.keys('r'); time.sleep(2)
        d.keys('alt','p'); d.keys('tab'); d.keys('down'); time.sleep(0.5); d.keys('ret'); time.sleep(1.5)
        d.keys('alt','f'); d.keys('p'); time.sleep(2)
        wait_screen(qmp,dump,lambda s: s.pixel(100,36)!=(0,0,128),what='the Print To File prompt')
        d.type('c:\\windows\\notepad.ps\n'); time.sleep(1)
        wait_screen(qmp,dump,active,timeout=90); time.sleep(1)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        guest.command('type c:\\windows\\win.ini','[PSCRIPT,FILE]')
    finally:
        if qmp: qmp.close()
        guest.close()
    # One Letter page: the header (the file's name) and footer (Page 1) centred,
    # the text in Courier from the 3/4-inch left and 1-inch top margins.
    from check_image import inspect
    page,=printed(inspect(image)['WINDOWS/NOTEPAD.PS'],'Notepad - NOTE.TXT',1)
    black=(0,0,0)
    assert page.count(black,0,0,70,page.height)==0, 'left margin'
    rows=[y for y in range(135,200) if page.count(black,75,y,400,y+1)]
    assert rows and max(rows)-min(rows)>=15 and page.count(black,75,135,400,200)>30, ('first line',rows)
    assert page.count(black,450,113,750,128)>20 and page.count(black,75,113,400,128)==0, 'header'
    assert page.count(black,450,1448,750,1463)>20, 'footer'
    print(f'PASS notepad: {machine} typing, Time/Date, Save As, Find down and up, Word Wrap, save prompt, Open, Save, command line, Page Setup, Print Setup, printing to a file',flush=True)

def windows_guest(machine, name):
    """A fresh image booted for an Interface Manager test, with QMP; None without the WDK build."""
    image=ROOT/'build'/f'{name}-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    sock=ROOT/'build'/f'qmp-{name}-{machine}.sock'; sock.unlink(missing_ok=True)
    guest=Guest(image,ROOT/'build'/f'{name}-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
    try:
        return guest,Qmp(sock)
    except BaseException:
        guest.close(); raise

def taskman(machine):
    """TASKMAN, the Task List, over Program Manager and Notepad: Ctrl+Esc, Tile, Switch To,
    Cascade, a double click on the desktop, End Task."""
    if not (ROOT/'build/media/WINDOWS/TASKMAN.EXE').exists():
        print(f'SKIP taskman: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'taskman'); dump=ROOT/'build'/f'taskman-{machine}.ppm'
    navy,white,desktop=(0,0,128),(255,255,255),(192,192,192)
    def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
    # The Task List is centred, its caption across y=132 (left of the title at x=280).
    task_list=lambda s: s.pixel(280,132)==navy
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
        shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(1)
        d.keys('alt','f'); d.keys('r'); time.sleep(1.5); d.type('notepad\n')
        shows(lambda s: s.pixel(100,36)==navy)
        # Tile: Notepad (on top, so first) on the left, Program Manager on the right (captions
        # probed left of their titles).
        d.keys('ctrl','esc'); shows(task_list); d.keys('alt','t')
        shows(lambda s: s.pixel(50,10)==navy and s.pixel(450,10)==white)
        # Switch To the second in the list, Program Manager.
        d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret')
        shows(lambda s: s.pixel(50,10)==white and s.pixel(450,10)==navy)
        # Cascade: Notepad, now the bottom window, at the top left, Program Manager a caption lower.
        d.keys('ctrl','esc'); shows(task_list); d.keys('alt','c')
        shows(lambda s: s.pixel(60,12)==white and s.pixel(80,36)==navy and s.pixel(790,10)==desktop)
        # A double click on the desktop; then End Task for Notepad, second in the list.
        d.double_click(790,10,lambda: shows(task_list,timeout=20))
        d.keys('down'); d.keys('alt','e')
        shows(lambda s: s.pixel(10,12)==desktop and s.pixel(80,36)==navy)
        d.keys('alt','f4'); time.sleep(1.5); d.keys('ret')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS taskman: {machine} Task List from Ctrl+Esc and the desktop, Tile, Switch To, Cascade, End Task',flush=True)

def recorder(machine):
    """RECORDER: a macro recorded in Notepad (stopped by a click on the Recorder's icon) and played
    back by its shortcut key; the macros saved in a .REC file."""
    if not (ROOT/'build/media/WINDOWS/RECORDER.EXE').exists():
        print(f'SKIP recorder: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'recorder'); dump=ROOT/'build'/f'recorder-{machine}.ppm'
    navy=(0,0,128)
    def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
    task_list=lambda s: s.pixel(280,132)==navy
    # Active captions, left of their titles: Notepad's (it opens at (48,48)), the Record Macro
    # dialog's, and that of the Recorder's dialog when a recording stops.
    notepad=lambda s: s.pixel(100,60)==navy
    record_dialog=lambda s: s.pixel(100,78)==navy
    stopped=lambda s: s.pixel(150,460)==navy
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
        shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(2)
        # The Recorder, then Notepad, from Program Manager; back to the Recorder from Notepad,
        # so Notepad is the window it records in.
        d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('recorder\n'); time.sleep(4)
        d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret'); time.sleep(2)
        d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('notepad\n'); time.sleep(4)
        d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('down'); d.keys('ret'); time.sleep(2)
        # Record Greeting on Ctrl+K: "hello" typed in Notepad, then a click on the Recorder's
        # icon (bottom left) and Save Macro.
        d.keys('alt','m'); d.keys('c'); shows(record_dialog)
        d.type('Greeting'); d.keys('tab'); d.type('k'); d.keys('alt','c'); d.keys('ret'); shows(notepad); time.sleep(1)
        d.type('hello'); time.sleep(1)
        d.click(40,540); shows(stopped); d.keys('ret'); shows(notepad); time.sleep(1)
        # Ctrl+K types it again; Notepad keeps the text.
        d.keys('ctrl','k'); time.sleep(4)
        d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\HELLO.TXT\n'); time.sleep(3)
        # The Recorder (picked in the Task List by its initial) saves its macros.
        d.keys('ctrl','esc'); shows(task_list); d.type('r'); d.keys('ret'); time.sleep(2)
        d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\TEST.REC\n'); time.sleep(3)
        d.keys('alt','f4'); time.sleep(2)
        # Exit Interface Manager from Program Manager.
        d.keys('ctrl','esc'); shows(task_list); d.type('p'); d.keys('ret'); time.sleep(2)
        d.keys('alt','f4'); time.sleep(2); d.keys('ret')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        guest.command('type c:\\hello.txt','hellohello')
    finally:
        qmp.close(); guest.close()
    from check_image import inspect
    rec=inspect(ROOT/'build'/f'recorder-{machine}.img')['TEST.REC']
    assert rec.startswith(b'IM RECORDER 1\r\n\0\x01\x00Greeting'), rec[:40]
    print(f'PASS recorder: {machine} a macro recorded in Notepad, stopped from the icon, played by its shortcut key, saved',flush=True)

def terminal(machine):
    """TERMINAL on COM1, a UART on a socket: what the other end sends shown (a VT-100 sequence
    among it) and captured to a file, what is typed sent; then Notepad prints to LPT1:, a
    parallel port into a file."""
    from mkimage import build
    if not (ROOT/'build/media/WINDOWS/TERMINAL.EXE').exists():
        print(f'SKIP terminal: {machine} (Interface Manager needs the WDK)',flush=True); return
    navy,black=(0,0,128),(0,0,0)
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='terminal-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=PORTDRV.SYS COM1=2F8 LPT1=378\nSHELL=C:\\COMMAND.COM /P\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        serial_path=fixture/'uart.sock'; printer_path=fixture/'printer.bin'; sock=fixture/'qmp.sock'
        dump=ROOT/'build'/f'terminal-{machine}.ppm'
        extra=('-chardev',f'socket,id=dos_uart,path={serial_path},server=on,wait=off',
               '-device','isa-serial,index=1,iobase=0x2f8,irq=3,chardev=dos_uart',
               '-chardev',f'file,id=dos_printer,path={printer_path}',
               '-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer',
               '-qmp',f'unix:{sock},server=on,wait=off')
        guest=Guest(image,ROOT/'build'/f'terminal-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',extra_args=extra)
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        # The window opens at (24,24); its first row of text is at y=69, below the menu bar.
        first_row=lambda s,x0,x1: sum(s.pixel(x,y)==black for y in range(70,85) for x in range(x0,x1))
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as uart:
            try:
                uart.settimeout(10*SLOW); uart.connect(str(serial_path))
                qmp=Qmp(sock); d=Desk(qmp)
                guest.p.stdin.write(b'c:\\windows\\win terminal\r'); guest.p.stdin.flush()
                shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(100,36)==navy,timeout=120); time.sleep(2)
                uart.sendall(b'HELLO FROM HOST\r\n')
                shows(lambda s: first_row(s,30,180)>60)
                # Typed text goes out as typed, Enter as CR.
                d.type('abc\n')
                received=b''
                while not received.endswith(b'\r'):
                    part=uart.recv(64); assert part, received; received+=part
                assert received==b'abc\r', received
                # A VT-100 clear and home, then text at the top left (the cursor after it).
                uart.sendall(b'\x1b[2J\x1b[HTOP')
                shows(lambda s: 0<first_row(s,30,55) and first_row(s,70,180)==0)
                # Receive Text File keeps what arrives until Stop.
                d.keys('alt','t'); d.keys('r'); time.sleep(2); d.type('C:\\RECV.TXT\n'); time.sleep(2)
                uart.sendall(b'captured line\r\n'); time.sleep(2)
                d.keys('alt','t'); d.keys('t'); time.sleep(1)
                d.keys('alt','f4')
                result=guest.expect(guest.prompt,timeout=60)
                assert 'leaked' not in result and 'WIN:' not in result, result
                guest.command('type c:\\recv.txt','captured line')
                # Notepad prints to the default printer, PostScript Printer on LPT1:.
                guest.p.stdin.write(b'c:\\windows\\win notepad c:\\recv.txt\r'); guest.p.stdin.flush()
                shows(lambda s: s.pixel(100,36)==navy,timeout=120); time.sleep(1)
                d.keys('alt','f'); d.keys('p')
                # Printing is done when the document's end has reached LPT1:.
                deadline=time.monotonic()+90*SLOW
                while not printer_path.read_bytes().endswith(b'%%EOF\n'):
                    assert time.monotonic()<deadline, printer_path.read_bytes()[-200:]
                    time.sleep(0.5)
                shows(lambda s: s.pixel(100,36)==navy,timeout=90); time.sleep(1)
                d.keys('alt','f4')
                result=guest.expect(guest.prompt,timeout=60)
                assert 'leaked' not in result and 'WIN:' not in result, result
            finally:
                if qmp: qmp.close()
                guest.close()
        page,=printed(printer_path.read_bytes(),'Notepad - RECV.TXT',1)
        # 10-point Courier at 150 dpi: the 10-pixel face drawn twice its size.
        rows=[y for y in range(135,200) if page.count(black,75,y,400,y+1)]
        assert page.count(black,75,135,400,200)>30 and rows and max(rows)-min(rows)>=15, ('the printed text',rows)
    print(f'PASS terminal: {machine} COM1 through PORTDRV.SYS: text shown, a VT-100 clear, typed text sent, a text file received; Notepad printing to LPT1:',flush=True)

def write_document(data):
    """A Write file's text and its character and paragraph runs: (start, end, property bytes)."""
    word=lambda o: struct.unpack_from('<H',data,o)[0]
    assert word(0) in (0xbe31,0xbe32) and word(4)==0xab00, data[:8]
    fc_mac=struct.unpack_from('<I',data,14)[0]; assert word(96)*128==len(data), (word(96),len(data))
    def runs(pn):
        result,fc=[],128
        while fc<fc_mac:
            page=data[pn*128:(pn+1)*128]; assert struct.unpack_from('<I',page,0)[0]==fc
            for i in range(page[127]):
                lim,bf=struct.unpack_from('<IH',page,4+6*i)
                result.append((fc,lim,b'' if bf==0xffff else page[5+bf:5+bf+page[4+bf]])); fc=lim
            pn+=1
        return result
    return data[128:fc_mac],runs((fc_mac+127)//128),runs(word(18))

def write(machine):
    """WRITE: a heading made bold, larger and centred, saved as a .WRI file (checked here),
    opened again, and printed to a file."""
    if not (ROOT/'build/media/WINDOWS/WRITE.EXE').exists():
        print(f'SKIP write: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'write'); dump=ROOT/'build'/f'write-{machine}.ppm'
    navy,black=(0,0,128),(0,0,0)
    def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
    # The window opens at (24,24): its text's first line is at y=70, below the menu bar's
    # line at y=68, and centred on x=328.
    blacks=lambda s,x0,x1,y0,y1: sum(s.pixel(x,y)==black for y in range(y0,y1) for x in range(x0,x1))
    centred=lambda s: blacks(s,40,120,70,88)==0 and blacks(s,280,380,70,88)>40
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win write\r'); guest.p.stdin.flush()
        shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(100,36)==navy,timeout=120); time.sleep(1)
        d.type('A Title Line\nThe second paragraph has ordinary text that goes on long enough to wrap onto the next line.\n')
        d.keys('ctrl','home'); d.keys('shift','end'); d.keys('ctrl','b')
        d.keys('alt','p'); d.keys('c'); time.sleep(1)
        for _ in range(2): d.keys('alt','c'); d.keys('e'); time.sleep(1)
        shows(centred)
        d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\TEST.WRI\n'); time.sleep(3)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        # Opened again; then File Print, to a file.
        guest.p.stdin.write(b'c:\\windows\\win write c:\\test.wri\r'); guest.p.stdin.flush()
        shows(lambda s: s.pixel(100,36)==navy and centred(s),timeout=120)
        d.keys('alt','f'); d.keys('p'); time.sleep(3); d.keys('alt','l'); d.keys('ret'); time.sleep(2)
        d.type('c:\\windows\\write.ps\n'); time.sleep(1)
        shows(lambda s: s.pixel(100,36)==navy,timeout=90); time.sleep(1)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    from check_image import inspect
    files=inspect(ROOT/'build'/f'write-{machine}.img')
    text,chps,paps=write_document(files['TEST.WRI'])
    assert text.startswith(b'A Title Line\r\nThe second paragraph') and text.endswith(b'line.\r\n'), text
    # The heading: bold (byte 1, bit 0) at 16 points (32 half points), centred (justification 1).
    assert chps[0][:2]==(128,140) and chps[0][2][1]&1 and chps[0][2][2]==32, chps
    assert paps[0][1]==142 and paps[0][2][1]==1 and all(len(p[2])<2 or p[2][1]==0 for p in paps[1:]), paps
    # Printed: the heading centred in the 6-inch column from the 1.25-inch margin, one inch down
    # (y=113); the raster fonts' largest sizes make the lines about 20 dots high.
    page,=printed(files['WINDOWS/WRITE.PS'],'Write - TEST.WRI',1)
    assert page.count(black,150,113,450,134)==0 and page.count(black,450,113,750,134)>60, 'heading'
    assert page.count(black,150,134,450,160)>60, 'second paragraph'
    print(f'PASS write: {machine} formatting (bold, larger, centred), a .WRI file written and read back, printing',flush=True)

def wmf_rectangle(colour):
    """A metafile in memory: window extent 100 x 100, a solid brush, no pen, Rectangle(0,0,100,100)."""
    records=[struct.pack('<IHhh',5,0x020c,100,100),                                  # SETWINDOWEXT y, x
             struct.pack('<IHHIH',7,0x02fc,0,colour[0]|colour[1]<<8|colour[2]<<16,0),   # CREATEBRUSHINDIRECT
             struct.pack('<IHH',4,0x012d,0),                                          # SELECTOBJECT
             struct.pack('<IHHhhI',8,0x02fa,5,0,0,0),                                 # CREATEPENINDIRECT PS_NULL
             struct.pack('<IHH',4,0x012d,1),
             struct.pack('<IHhhhh',7,0x041b,100,100,0,0),                             # RECTANGLE bottom, right, top, left
             struct.pack('<IH',3,0)]
    body=b''.join(records)
    return struct.pack('<HHHIHIH',1,9,0x300,(18+len(body))//2,2,max(len(r) for r in records)//2,0)+body

def wri_metafile_picture(metafile, width, height):
    """A Write picture of a metafile (anisotropic) width x height twips: the 40-byte header, then the metafile."""
    return struct.pack('<HhhHHHH',0x88,2540,2540,0,0,width,height)+bytes(16)+struct.pack('<HIHH',40,len(metafile),1000,1000)+metafile

def wri_bitmap_picture(width, height, pixel):
    """A Write picture of a 24-bit bitmap, rows from the top; pixel(x, y) gives (r, g, b)."""
    row=(width*3+1)//2*2; bits=bytearray(row*height)
    for y in range(height):
        for x in range(width):
            r,g,b=pixel(x,y); bits[y*row+x*3:y*row+x*3+3]=bytes((b,g,r))
    return (struct.pack('<HhhHHHHH',0xe3,0,0,0,0,width*15,height*15,0)+struct.pack('<HHHHBBI',0,width,height,row,1,24,0)+
            struct.pack('<HIHH',40,len(bits),1000,1000)+bytes(bits))

def wri_file(paragraphs):
    """A Write file: each paragraph text ending CR LF (default formats), or (picture bytes, alignment)."""
    text,paps=b'',[]
    for p in paragraphs:
        if isinstance(p,tuple):
            text+=p[0]; prop=bytearray(17); prop[0],prop[1],prop[2],prop[10],prop[16]=61,p[1],30,240,0x10   # a picture's (0x10)
            paps.append((128+len(text),bytes(prop)))
        else:
            text+=p; paps.append((128+len(text),None))
    def fkps(runs):
        pages,fc,i=[],128,0
        while i<len(runs):
            page=bytearray(128); struct.pack_into('<I',page,0,fc); n,back=0,127
            while i<len(runs) and 4+6*(n+1)<=back-(len(runs[i][1])+1 if runs[i][1] else 0):
                lim,prop=runs[i]
                if prop: back-=len(prop)+1; page[back]=len(prop); page[back+1:back+1+len(prop)]=prop
                struct.pack_into('<IH',page,4+6*n,lim,back-4 if prop else 0xffff); n,fc,i=n+1,lim,i+1
            page[127]=n; pages.append(bytes(page))
        return pages
    fc_mac=128+len(text); text_pages=(len(text)+127)//128
    chps,pap_pages=fkps([(fc_mac,None)]),fkps(paps)
    pn_para=1+text_pages+len(chps); pn_mac=pn_para+len(pap_pages)
    head=bytearray(128); struct.pack_into('<HHHI',head,0,0xbe31,0,0xab00,0); struct.pack_into('<I',head,14,fc_mac)
    struct.pack_into('<6H',head,18,pn_para,pn_mac,pn_mac,pn_mac,pn_mac,pn_mac); struct.pack_into('<H',head,96,pn_mac)
    return bytes(head)+text+bytes(text_pages*128-len(text))+b''.join(chps)+b''.join(pap_pages)

def write_pictures(machine):
    """WRITE with pictures, in a .WRI file made here: a metafile (a red rectangle, an inch by half an
    inch) and a bitmap (32 x 16 pixels, blue and green halves) centred. Write shows them; a click selects
    the metafile, Size Picture makes it twice as wide, Copy puts it on the clipboard, which the Clipboard
    viewer shows, and Paste adds it at the end; the file saved keeps the pictures, and printing draws them."""
    from mkimage import build
    from check_image import inspect
    if not (ROOT/'build/media/WINDOWS/WRITE.EXE').exists():
        print(f'SKIP write_pictures: {machine} (Interface Manager needs the WDK)',flush=True); return
    red,cyan,blue,green,navy=(255,0,0),(0,255,255),(0,0,255),(0,255,0),(0,0,128)
    metafile=wri_metafile_picture(wmf_rectangle(red),1440,720)
    bitmap=wri_bitmap_picture(32,16,lambda x,y: blue if x<16 else green)
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='wrpic-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'PICS.WRI').write_bytes(wri_file([b'Pictures\r\n',(metafile,0),b'Between\r\n',(bitmap,1),b'The end\r\n']))
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'wrpic-{machine}.ppm'
        guest=Guest(image,ROOT/'build'/f'wrpic-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        def reds(s,x0,y0,x1,y1): return sum(s.pixel(x,y)==red for y in range(y0,y1,4) for x in range(x0,x1,4))
        # Write opens at (24,24): the metafile at (40,89), 96 x 48 pixels; the bitmap centred at (312,157).
        task_list=lambda s: s.pixel(280,132)==navy
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(2)
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('write c:\\pics.wri\n')
            shows(lambda s: s.pixel(100,36)==navy and s.pixel(80,110)==red and s.pixel(136,110)!=red)
            shows(lambda s: s.pixel(318,165)==blue and s.pixel(338,165)==green)
            settled(qmp,dump)
            # The horizontal scroll bar: the column (with its margins) is 24 pixels wider than the
            # window; two steps right move it all the way, and two steps left bring it back.
            d.click(596,464); time.sleep(0.5); d.click(596,464)
            shows(lambda s: s.pixel(30,110)==red and s.pixel(120,110)!=red)
            d.click(32,464); time.sleep(0.5); d.click(32,464)
            shows(lambda s: s.pixel(30,110)!=red and s.pixel(80,110)==red and s.pixel(136,110)!=red)
            d.click(80,110); shows(lambda s: s.pixel(60,100)==cyan); time.sleep(1)
            # Size Picture: eight steps of an eighth of an inch to the right.
            d.keys('alt','e'); d.keys('z'); time.sleep(1)
            for _ in range(8): d.keys('right',pause=0.2)
            d.keys('ret'); shows(lambda s: s.pixel(200,110)==cyan and s.pixel(240,110)!=cyan)
            d.keys('ctrl','c'); time.sleep(1)
            # The Clipboard viewer, from Program Manager, shows the picture at its size.
            d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret'); time.sleep(2)
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('clipbrd\n')
            shows(lambda s: s.pixel(150,115)==red and s.pixel(240,115)==red and s.pixel(250,115)!=red)
            d.keys('alt','f4'); time.sleep(2)
            # Back to Write (second in the Task List, after Program Manager): the picture pasted at the end.
            d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret'); time.sleep(2)
            d.keys('ctrl','end'); time.sleep(1); d.keys('ctrl','v')
            shows(lambda s: reds(s,30,70,600,450)>2*48*12*0.9)
            d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\PICS2.WRI\n'); time.sleep(3)
            d.keys('alt','f'); d.keys('p'); time.sleep(3); d.keys('alt','l'); d.keys('ret'); time.sleep(2)
            d.type('c:\\pics.ps\n'); time.sleep(2)
            shows(lambda s: s.pixel(100,36)==navy,timeout=120); time.sleep(2)
            d.keys('alt','f4'); time.sleep(2)
            d.keys('alt','f4'); time.sleep(2); d.keys('ret')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
        files=inspect(image)
    text,chps,paps=write_document(files['PICS2.WRI'])
    pictures=[text[a-128:b-128] for a,b,prop in paps if len(prop)>16 and prop[16]&0x10]
    assert len(pictures)==3 and text.startswith(b'Pictures\r\n'+pictures[0]+b'Between\r\n'+bitmap+b'The end\r\n'), [len(p) for p in pictures]
    # The sized metafile: twice as wide (2000 thousandths); the pasted one two inches by half an inch.
    assert pictures[0][40:]==metafile[40:] and struct.unpack_from('<HH',pictures[0],36)==(2000,1000), pictures[0][:40]
    assert pictures[2][40:]==metafile[40:] and struct.unpack_from('<HH',pictures[2],10)==(2880,720), pictures[2][:40]
    page,=printed(files['PICS.PS'],'Write - PICS2.WRI',1)
    assert page.count(red,0,0,page.width,400)>2*300*75*0.9, page.count(red,0,0,page.width,400)
    assert page.count(blue,0,0,page.width,400)>500 and page.count(green,0,0,page.width,400)>500, 'the bitmap printed'
    print(f'PASS write_pictures: {machine} pictures in a .WRI file (a metafile and a bitmap) shown, scrolled sideways, selected, sized, copied to the Clipboard viewer, pasted, saved and printed',flush=True)

def pif_file(base, extensions):
    """A PIF: the basic part (0x171 bytes, its checksum made) and, when there are extensions,
    the MICROSOFT PIFEX header and each (name, data) behind a header of its own, in order."""
    data=bytearray(base)
    if extensions:
        chain=[(b'MICROSOFT PIFEX',b'')]+list(extensions)
        for i,(name,body) in enumerate(chain):
            at=len(data)
            following=0xffff if i==len(chain)-1 else at+0x16+len(body)
            data+=name.ljust(16,b'\0')+struct.pack('<HHH',following,at+0x16 if i else 0,len(body) if i else 0x171)+body
    data[1]=sum(data[2:0x171])&0xff
    return bytes(data)

def filecmds(machine):
    """The file utilities after MS-DOS 4's: ATTRIB, FIND, MORE, SORT, TREE, COMP, XCOPY and
    REPLACE on a tree of text files, their output and prompts checked."""
    from mkimage import build
    fruit=b'apple\r\nBanana\r\ncherry\r\napple pie\r\n'
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='filecmds-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        for name,data in (('FT/A.TXT',fruit),('FT/B.TXT',fruit.replace(b'cherry',b'cHerry')),('FT/C.TXT',b'short\r\n'),
                          ('FT/SUB/D.TXT',b'deep one\r\n'),('FT/SUB/DEEP/E.TXT',b'deeper\r\n'),
                          ('FT/LONG.TXT',b''.join(b'line %d\r\n'%i for i in range(1,31)))):
            (media/name).parent.mkdir(parents=True,exist_ok=True); (media/name).write_bytes(data)
        (media/'FT/EMPTY').mkdir()
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'filecmds-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on')
        try:
            # Prompts read their key with AH=0Ch, which first flushes what was typed ahead.
            def answer(key,expected,timeout=30):
                time.sleep(0.5); guest.p.stdin.write(key); guest.p.stdin.flush(); return guest.expect(expected,timeout=timeout)
            # ATTRIB: archive set on files made by the image; R set and cleared; /S lists below first.
            guest.command('attrib ft\\a.txt','  A                 C:\\FT\\A.TXT')
            guest.command('attrib +r ft\\a.txt')
            guest.command('attrib ft\\a.txt','  A    R            C:\\FT\\A.TXT')
            guest.command('attrib -r -a ft\\a.txt')
            guest.command('attrib ft\\a.txt','                    C:\\FT\\A.TXT')
            result=guest.command('attrib ft\\*.txt /s')
            assert result.index('C:\\FT\\SUB\\DEEP\\E.TXT')<result.index('C:\\FT\\SUB\\D.TXT')<result.index('C:\\FT\\A.TXT'), result
            guest.command('attrib +r +r ft\\a.txt','Parameter format not correct - +r',error=True)
            guest.command('attrib','Required parameter missing',error=True)
            guest.command('attrib ft\\none.txt','File not found - ft\\none.txt',error=True)
            # FIND: lines, a count, numbered lines without the text, as a filter, a missing file.
            result=guest.command('find "apple" ft\\a.txt ft\\c.txt','---------- FT\\A.TXT')
            assert 'apple\n' in result and 'apple pie' in result and 'Banana' not in result and '---------- FT\\C.TXT' in result, result
            guest.command('find /c "apple" ft\\a.txt','---------- FT\\A.TXT: 2')
            result=guest.command('find /v /n "apple" ft\\a.txt','[2]Banana')
            assert '[3]cherry' in result and '[1]' not in result, result
            result=guest.command('type ft\\a.txt | find "pie"','apple pie')
            assert '----------' not in result, result
            guest.command('find "x" ft\\none.txt','File not found - FT\\NONE.TXT')
            guest.command('find apple ft\\a.txt','FIND: Parameter format not correct',error=True)
            # SORT by the collating sequence (letter case aside), backwards, from column 2.
            result=guest.command('sort < ft\\a.txt')
            assert result.index('apple\n')<result.index('apple pie')<result.index('Banana')<result.index('cherry'), result
            result=guest.command('sort /r < ft\\a.txt')
            assert result.index('cherry')<result.index('Banana')<result.index('apple pie')<result.index('apple\n'), result
            result=guest.command('sort /+2 < ft\\a.txt')
            assert result.index('Banana')<result.index('cherry')<result.index('apple'), result
            guest.command('sort /x','SORT: Invalid switch - /x',error=True)
            # TREE with ASCII lines, then files too.
            result=guest.command('tree ft /a','Directory PATH listing')
            assert 'C:\\FT\n' in result and '+---EMPTY' in result and '\\---SUB' in result and '    \\---DEEP' in result, result
            result=guest.command('tree ft /f /a')
            assert '|   A.TXT' in result and '    \\---DEEP' in result and '            E.TXT' in result, result
            guest.command('tree ft\\empty','No sub-directories exist')
            # COMP: equal files, then different ones, and "Compare more files".
            guest.p.stdin.write(b'comp ft\\a.txt ft\\a.txt\r'); guest.p.stdin.flush()
            result=guest.expect(b'Compare more files (Y/N) ?')
            assert 'C:\\FT\\A.TXT and C:\\FT\\A.TXT' in result and 'Files compare ok' in result and 'Eof mark not found' in result, result
            answer(b'n',guest.prompt)
            guest.p.stdin.write(b'comp ft\\a.txt ft\\b.txt\r'); guest.p.stdin.flush()
            result=guest.expect(b'Compare more files (Y/N) ?')
            assert 'Compare error at OFFSET 10\nFile 1 = 68\nFile 2 = 48' in result, result
            answer(b'n',guest.prompt)
            guest.p.stdin.write(b'comp ft\\a.txt ft\\c.txt\r'); guest.p.stdin.flush()
            result=guest.expect(b'Compare more files (Y/N) ?')
            assert 'Files are different sizes' in result, result
            answer(b'n',guest.prompt)
            # XCOPY: a tree, empty directories with /E, a target asked about, only archive files.
            result=guest.command('xcopy ft c:\\xt\\ /s /e','Reading source file(s)...')
            assert '6 File(s) copied' in result and 'C:\\FT\\SUB\\DEEP\\E.TXT' in result, result
            guest.command('dir c:\\xt\\empty','2 File(s)')
            guest.command('type c:\\xt\\sub\\deep\\e.txt','deeper')
            guest.p.stdin.write(b'xcopy ft\\c.txt c:\\one\r'); guest.p.stdin.flush()
            guest.expect(b'(F = file, D = directory)?')
            result=answer(b'f',guest.prompt)
            assert '1 File(s) copied' in result, result
            guest.command('type c:\\one','short')
            guest.command('attrib -a ft\\b.txt')
            result=guest.command('xcopy ft\\*.txt c:\\arc\\ /m')
            # A.TXT lost its archive attribute above, B.TXT here.
            assert 'C:\\FT\\A.TXT' not in result and 'C:\\FT\\B.TXT' not in result and '2 File(s) copied' in result, result
            guest.command('xcopy ft\\*.txt c:\\arc\\ /m','0 File(s) copied')
            guest.command('xcopy ft c:\\ft\\in\\ /s','Cannot perform a cyclic copy')
            # REPLACE: the files of a name below a target, then additions.
            guest.command('echo changed> c:\\new.txt')
            guest.command('copy c:\\new.txt c:\\xt\\sub\\d.txt')
            guest.command('copy ft\\sub\\d.txt c:\\d.txt')
            result=guest.command('replace c:\\d.txt c:\\xt /s','Replacing C:\\XT\\SUB\\D.TXT')
            assert '1 file(s) replaced' in result, result
            guest.command('type c:\\xt\\sub\\d.txt','deep one')
            result=guest.command('replace c:\\new.txt c:\\xt /a','Adding C:\\XT\\NEW.TXT')
            assert '1 file(s) added' in result, result
            guest.command('replace c:\\new.txt c:\\xt /a','No files added')
            guest.command('replace c:\\none.txt c:\\xt','No files found - c:\\none.txt',error=True)
            guest.command('replace c:\\d.txt c:\\xt /a /s','Invalid parameter combination',error=True)
            # MORE: a screenful (24 lines), then the rest after a key.
            guest.p.stdin.write(b'more < ft\\long.txt\r'); guest.p.stdin.flush()
            result=guest.expect(b'-- More --')
            assert 'line 24' in result and 'line 25' not in result, result
            result=answer(b' ',guest.prompt)
            assert 'line 30' in result, result
            # EDLIN: a new file, lines inserted until Ctrl+C, listed, one edited, found, saved; then
            # one deleted, the old file kept as .BAK. Its prompt is a "*" at the start of a line.
            star=b'\n*'
            guest.p.stdin.write(b'edlin c:\\ed.txt\r'); guest.p.stdin.flush()
            guest.expect(b'New file'); guest.expect(star)
            answer(b'i\r',b'       1:*'); answer(b'first\r',b'       2:*'); answer(b'second\r',b'       3:*')
            answer(b'\x03',star)
            result=answer(b'l\r',star)
            assert '       1: first' in result and '       2: second' in result, result
            answer(b'1\r',b'       1:*first'); guest.expect(b'1:*')
            answer(b'uno\r',star)
            result=answer(b'1,2ssec\r',star)
            assert '       2:*second' in result, result
            answer(b'e\r',guest.prompt)
            guest.command('type c:\\ed.txt','uno')
            guest.p.stdin.write(b'edlin c:\\ed.txt\r'); guest.p.stdin.flush()
            guest.expect(b'End of input file'); guest.expect(star)
            answer(b'2d\r',star); answer(b'e\r',guest.prompt)
            result=guest.command('type c:\\ed.txt','uno')
            assert 'second' not in result, result
            guest.command('type c:\\ed.bak','second')
        finally:
            guest.close()
    print(f'PASS filecmds: {machine} ATTRIB, FIND, MORE, SORT, TREE, COMP, XCOPY, REPLACE and EDLIN as in MS-DOS 4',flush=True)

def drivemaps(machine):
    """SUBST, JOIN and ASSIGN: a letter for a directory (its own current directory, DIR and the
    prompt in its terms, TRUENAME through it, removal of a directory in use refused, CHKDSK
    refused), a RAM drive joined to a directory of C: and parted again, a JOIN directory with
    files refused, and ASSIGN's letters with /STATUS."""
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='drivemaps-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=RAMDRV.SYS /SIZE:512\nSHELL=C:\\COMMAND.COM /P\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'drivemaps-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on')
        try:
            guest.command('md c:\\sb'); guest.command('echo hi> c:\\sb\\x.txt')
            guest.command('subst e: c:\\sb')
            guest.command('subst','E: => C:\\SB')
            guest.command('type e:x.txt','hi')
            result=guest.command('dir e:','Directory of  E:\\')
            assert 'X        TXT' in result, result
            guest.command('e:',prompt=b'E:\\>')
            guest.command('md in',prompt=b'E:\\>')
            guest.command('cd in',prompt=b'E:\\IN>')
            guest.command('truename .','C:\\SB\\IN',prompt=b'E:\\IN>')
            guest.command('c:')
            guest.command('rd c:\\sb\\in','(DOS error 16)',error=True)
            guest.command('chkdsk e:','Cannot CHKDSK a SUBSTed or ASSIGNed drive')
            guest.command('subst e: /d')
            result=guest.command('subst')
            assert 'E:' not in result.split('\n',1)[-1], result
            guest.command('rd c:\\sb\\in')
            guest.command('type e:x.txt','(DOS error 15)',error=True)
            # JOIN: D:, the RAM drive, as C:\DJ (made for it); then parted.
            guest.command('echo dd> d:\\dd.txt')
            guest.command('join d: c:\\dj')
            guest.command('join','D: => C:\\DJ')
            guest.command('type c:\\dj\\dd.txt','dd')
            guest.command('dir d:','(DOS error 15)',error=True)
            guest.command('join d: /d')
            guest.command('type d:\\dd.txt','dd')
            guest.command('md c:\\full'); guest.command('echo f> c:\\full\\f.txt')
            guest.command('join d: c:\\full','Directory not empty - c:\\full')
            # ASSIGN: D: sent to C:.
            guest.command('assign d=c')
            guest.command('type d:\\sb\\x.txt','hi')
            guest.command('assign /sta','Original D: set to C:')
            guest.command('chkdsk d:','Cannot CHKDSK a SUBSTed or ASSIGNed drive')
            guest.command('assign')
            guest.command('type d:\\dd.txt','dd')
            # APPEND: C:\\AP searched for files opened by name; searches and ATTRIB only with /X
            # (ATTRIB looks at files itself and leaves /X off).
            guest.command('md c:\\ap'); guest.command('echo ap> c:\\ap\\ap.txt')
            guest.command('type ap.txt','(DOS error 2)',error=True)
            guest.command('append')
            guest.command('append','No Append')
            guest.command('append c:\\ap')
            guest.command('append','APPEND=C:\\AP')
            guest.command('type ap.txt','ap')
            guest.command('dir ap.txt','File not found',error=True)
            guest.command('append /x:on')
            guest.command('dir ap.txt','AP       TXT')
            guest.command('attrib ap.txt','File not found - ap.txt')
            guest.command('append /x:off')
            guest.command('append ;')
            guest.command('append','No Append')
            guest.command('type ap.txt','(DOS error 2)',error=True)
        finally:
            guest.close()
    print(f'PASS drivemaps: {machine} SUBST (its letter\'s own directories, TRUENAME, a directory in use, CHKDSK refused), JOIN of a RAM drive and a non-empty directory refused, ASSIGN with /STATUS, APPEND with and without /X',flush=True)

def installables(machine):
    """CONFIG.SYS INSTALL= (SHARE run before the shell) and the DOS 4 programs whose work MSDOS.SYS
    and VDM do themselves: SHARE, FASTOPEN, NLSFUNC and GRAFTABL check their parameters and say
    when they are already installed, as DOS 4's did."""
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='installables-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nINSTALL=C:\\SHARE.EXE /F:2048 /L:20\nSHELL=C:\\COMMAND.COM /P\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'installables-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on')
        try:
            assert 'INSTALL' not in guest.boot, guest.boot
            guest.command('share','SHARE already installed')
            guest.command('share /l:0','Parameter value not in allowed range - /l:0')
            guest.command('fastopen c:=5','Invalid number of file/directory entries')
            guest.command('fastopen c:=100','FASTOPEN installed')
            guest.command('fastopen c:','FASTOPEN already installed')
            guest.command('nlsfunc c:\\none.sys','File not found - c:\\none.sys')
            result=guest.command('nlsfunc c:\\country.sys')
            assert 'installed' not in result.split('\n',1)[-1], result
            guest.command('nlsfunc','NLSFUNC already installed')
            guest.command('graftabl /sta','Active Code Page: None')
            guest.command('graftabl 850','Active Code Page: 850')
            result=guest.command('graftabl 865','Previous Code Page: 850')
            assert 'Active Code Page: 865' in result, result
            guest.command('graftabl 999','Parameter value not allowed - 999')
            guest.command('if errorlevel 3 echo THREE','THREE')
        finally:
            guest.close()
    print(f'PASS installables: {machine} INSTALL=SHARE.EXE, SHARE, FASTOPEN, NLSFUNC and GRAFTABL as DOS 4\'s',flush=True)

def backups(machine):
    """BACKUP and RESTORE as DOS 4's: a tree to a fixed disk's \\BACKUP with a log and back after
    deletion, a changed file added with /M /A and the questions /P asks, a diskette labelled
    BACKUP 001, and a file spanning two diskettes (the drive is one RAM diskette, so RESTORE finds
    the second out of sequence and the file's first part missing)."""
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='backup-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=C:\\RAMDRV.SYS /SIZE:360 /REMOVABLE\nSHELL=C:\\COMMAND.COM /P\n')
        tree=media/'BK'; (tree/'SUB').mkdir(parents=True)
        (tree/'A.TXT').write_bytes(b'alpha\r\n'); (tree/'SUB'/'B.TXT').write_bytes(b'beta\r\n')
        big=bytes((i*13+7)&255 for i in range(500000)); (tree/'SUB'/'BIG.BIN').write_bytes(big)
        (media/'REF').mkdir(); (media/'REF'/'BIG.BIN').write_bytes(big)
        # D:, a second partition: a fixed disk.
        image=fixture/'disk.img'; disk_fixture(image,False,False,media=media)
        guest=Guest(image,ROOT/'build'/f'backup-{machine}.log',machine)
        def run(command,steps,expected=()):
            guest.p.stdin.write(command.encode()+b'\r'); guest.p.stdin.flush(); out=''
            for wait,reply in steps:
                # Keys are read with AH=0Ch, which first flushes what was typed ahead.
                out+=guest.expect(wait.encode(),timeout=120); time.sleep(0.5); guest.p.stdin.write(reply); guest.p.stdin.flush()
            out+=guest.expect(guest.prompt,timeout=120)
            assert all(text in out for text in expected), out
            print(f'PASS console: {command}',flush=True)
            return out
        try:
            # A fixed disk: \BACKUP made, the log in the source's root.
            run('backup c:\\bk d: /s /l',[],['Logging to file C:\\BACKUP.LOG','*** Backing up files to drive D: ***',
                'Diskette Number: 01','\\BK\\A.TXT','\\BK\\SUB\\B.TXT','\\BK\\SUB\\BIG.BIN'])
            guest.command('type c:\\backup.log','001  \\BK\\SUB\\BIG.BIN')
            out=guest.command('attrib d:\\backup\\*.*','A    R            D:\\BACKUP\\CONTROL.001')
            assert 'A    R            D:\\BACKUP\\BACKUP.001' in out, out
            guest.command('attrib c:\\bk\\a.txt',' '*20+'C:\\BK\\A.TXT')
            for name in ('a.txt','sub\\b.txt','sub\\big.bin'): guest.command(f'del c:\\bk\\{name}')
            guest.command('rd c:\\bk\\sub')
            run('restore d: c:\\bk\\*.* /s',[],['*** Files were backed up ','*** Restoring files from drive D: ***',
                '\\BK\\A.TXT','\\BK\\SUB\\BIG.BIN'])
            run('comp c:\\bk\\sub\\big.bin c:\\ref\\big.bin',[('Compare more files (Y/N) ?',b'N')],['Files compare ok'])
            guest.command('type c:\\bk\\a.txt','alpha')
            # /M with /A: only the changed file, added to the backup there. (RESTORE
            # gave the files back their attributes, archive among them.)
            guest.command('attrib -a c:\\bk\\*.* /s')
            guest.command('echo gamma>> c:\\bk\\a.txt')
            out=run('backup c:\\bk d: /s /m /a',[],['\\BK\\A.TXT']); assert 'B.TXT' not in out, out
            guest.command('echo delta>> c:\\bk\\a.txt')
            out=run('restore d: c:\\bk\\a.txt /p',[('Replace the file (Y/N)?',b'N'),('Replace the file (Y/N)?',b'N')],
                ['Warning! File A.TXT\nwas changed after it was backed up'])
            guest.command('type c:\\bk\\a.txt','delta')
            guest.command('restore d: c:\\bk\\a.txt /n','Warning! No files were found to restore')
            guest.command('restore d: c:\\bk\\a.txt','\\BK\\A.TXT')
            out=guest.command('type c:\\bk\\a.txt','gamma'); assert 'delta' not in out, out
            # A diskette: its root erased, the files read-only, the label BACKUP 001.
            run('backup c:\\bk\\*.txt a: /s',[('Press any key to continue . . .',b'x')],['Insert backup diskette 01 in drive A:',
                'A:\\ root directory will be erased','*** Backing up files to drive A: ***','\\BK\\SUB\\B.TXT'])
            guest.command('vol a:','Volume in drive A is BACKUP  001')
            guest.command('del c:\\bk\\sub\\b.txt')
            run('restore a: c:\\bk\\sub\\b.txt',[('Press any key to continue . . .',b'x')],['Diskette: 01','\\BK\\SUB\\B.TXT'])
            guest.command('type c:\\bk\\sub\\b.txt','beta')
            # A file too big for one diskette goes on to the next.
            out=run('backup c:\\bk\\sub\\big.bin a:',[('Press any key to continue . . .',b'x'),('Insert backup diskette 02 in drive A:',b''),
                ('Press any key to continue . . .',b'x')],['Diskette Number: 02'])
            assert out.count('\\BK\\SUB\\BIG.BIN')==2, out
            guest.command('vol a:','Volume in drive A is BACKUP  002')
            guest.command('dir a:','CONTROL  002')
            run('restore a: c:\\bk\\sub\\big.bin',[('Press any key to continue . . .',b'x'),('Diskette is out of sequence',b''),
                ('Press any key to continue . . .',b'x')],['Warning! No files were found to restore'])
            guest.command('if errorlevel 1 echo NONE','NONE')
            run('comp c:\\bk\\sub\\big.bin c:\\ref\\big.bin',[('Compare more files (Y/N) ?',b'N')],['Files compare ok'])
        finally:
            guest.close()
    print(f'PASS backups: {machine} BACKUP and RESTORE to and from a fixed disk and diskettes, log, /M /A /P /N, spanning',flush=True)

def ansi(machine):
    """DEVICE=ANSI.SYS: escape sequences in CON output (TYPE, PROMPT $e) placing and coloring text on
    the firmware's console, the cursor's position typed back, a key reassigned, CLS in the
    attribute, and the attribute kept on the VGA screen an 8086 program takes."""
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='ansi-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=C:\\ANSI.SYS /X\nSHELL=C:\\COMMAND.COM /P\n')
        # Cleared, HELLO at row 5 column 10 in bright yellow on red, the cursor's place asked.
        (media/'PAINT.TXT').write_bytes(b'\x1b[2J\x1b[5;10H\x1b[1;33;41mHELLO\x1b[0m\x1b[6n')
        (media/'KEYS.TXT').write_bytes(b'\x1b["Q";"echo REMAPPED";13p')
        (media/'RESET.TXT').write_bytes(b'\x1b[0m')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'ansi-{machine}.ppm'
        # itanium2-vpc's default Montecito has no IA-32 instruction set for VDM.
        guest=Guest(image,ROOT/'build'/f'ansi-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off')+(() if machine=='itanium-vpc' else ('-cpu','madison-1500')))
        qmp=None
        red,yellow,blue,white=(170,0,0),(255,255,85),(0,0,170),(255,255,255)
        cells=lambda s,x0,y0,x1,y1: [s.pixel(x,y) for y in range(y0,y1) for x in range(x0,x1)]
        try:
            qmp=Qmp(sock)
            assert 'CONFIG.SYS line' not in guest.boot, guest.boot
            # The report comes back as typed: ESC cancels the line, the rest is a command.
            guest.p.stdin.write(b'type paint.txt\r'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt,timeout=30)+guest.expect(guest.prompt,timeout=30)
            assert '[05;15R' in result and 'Bad command' in result, result
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and set(cells(s,72,64,112,80))<={red,yellow},what='the painted text')
            screen=settled(qmp,dump)
            hello=cells(screen,72,64,112,80)
            assert hello.count(red)>300 and hello.count(yellow)>100 and set(hello)<={red,yellow}, set(hello)
            assert set(cells(screen,0,0,640,48))<={(0,0,0)}, 'erased'
            # MODE shows and sets the screen's columns and lines through ANSI.SYS.
            result=guest.command('mode con','COLUMNS=80\nLINES=25\n')
            guest.command('mode con lines=43','Function not supported on this computer - LINES=43')
            result=guest.command('mode con cols=80 lines=25')
            assert 'not supported' not in result and 'Invalid' not in result, result
            guest.command('type keys.txt')
            guest.p.stdin.write(b'Q'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt,timeout=30)
            assert '\nREMAPPED' in result, result
            # CLS fills with the attribute PROMPT's sequence set.
            guest.command('prompt $e[1;37;44m$p$g')
            guest.command('cls')
            screen=wait_screen(qmp,dump,lambda s: s.pixel(600,380)==blue and s.pixel(4,4)==blue)
            assert white in cells(screen,0,0,32,16), 'prompt'
            # An 8086 program's screen (the VGA) takes the attribute, and keeps it after.
            guest.command('hello16 colors','HELLO16: DOS 4.00, tail [ colors]')
            vga_blue=(0,0,168)
            screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(720,400) and s.pixel(700,390)==vga_blue)
            line=cells(screen,0,16,240,32)
            assert line.count(vga_blue)>2000 and line.count(white)>200, set(line)
            # The prompt's sequence would set it again: the prompt first, then the reset.
            guest.command('prompt $p$g')
            guest.command('type reset.txt')
            guest.command('cls')
            screen=wait_screen(qmp,dump,lambda s: s.pixel(700,390)==(0,0,0) and s.lit(0,0,36,16))
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS ansi: {machine} ANSI.SYS cursor, colors, cursor report, key reassignment, CLS and the VGA screen',flush=True)

def xmsems(machine):
    """XMS and EMS for 8086 programs: with DEVICE=HIMEM.SYS and EMM386.SYS, MEM16 checks XMS 3.0
    (blocks, moves, locks, the high memory area, A20) and LIM EMS 4.0 (the frame at 9000h, mapping,
    moves, names, the EMMXXXX0 device); without them, and with EMM386.SYS refusing a frame in
    upper memory, that neither is there."""
    from mkimage import build
    cpu=() if machine=='itanium-vpc' else ('-cpu','madison-1500')
    for drivers in (True,False):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='xmsems-') as temporary:
            fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
            lines='DEVICE=C:\\HIMEM.SYS /NUMHANDLES=16\nDEVICE=C:\\EMM386.SYS 1024\n' if drivers else 'DEVICE=C:\\EMM386.SYS FRAME=D000\n'
            (media/'CONFIG.SYS').write_text('FILES=64\n'+lines+'SHELL=C:\\COMMAND.COM /P\n')
            image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
            guest=Guest(image,ROOT/'build'/f'xmsems-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',extra_args=cpu)
            try:
                if drivers:
                    assert 'HIMEM: XMS 3.0 for 8086 programs, 16 handles' in guest.boot and 'EMM386: LIM EMS 4.0' in guest.boot, guest.boot
                    result=guest.command('mem16','MEM16: EMS free ok')
                    assert 'FAILED' not in result, result
                    for name in ('XMS found','XMS blocks and moves','XMS lock, resize, free','XMS high memory area and A20',
                                 'XMS 3.0 functions','EMS found','EMS pages mapped','EMS move and names','EMS device',
                                 'EMS map and call'):
                        assert f'MEM16: {name} ok' in result, (name,result)
                    assert 'MEM16: EMS frame below 640K ok' in result, result
                    result=guest.command('mem','\n   1048576 bytes total EMS memory\n   1048576 bytes free EMS memory\n')
                    assert '\n  16777216 bytes total extended memory\n         0 bytes available extended memory\n' in result, result
                else:
                    assert 'EMM386: Invalid parameter - FRAME=D000' in guest.boot, guest.boot
                    guest.command('mem16 none','MEM16: no XMS or EMS ok')
                    result=guest.command('mem','largest executable program size')
                    assert 'EMS' not in result and 'extended' not in result, result
                guest.command('if errorlevel 1 echo BAD')
                assert 'BAD' not in guest.output.decode('latin-1').split('if errorlevel 1 echo BAD')[-1], 'exit code'
            finally:
                guest.close()
    print(f'PASS xmsems: {machine} HIMEM.SYS and EMM386.SYS: XMS 3.0 and LIM EMS 4.0 for 8086 programs, and neither without them',flush=True)

def mz_file(image,ip=0,cs=0,ss=0,sp=0,relocations=()):
    """An 8086 MZ program: a 512-byte header, its relocation table at 1Ch."""
    size=512+len(image); pages=(size+511)//512
    header=bytearray(512)
    struct.pack_into('<2sHHHHHHHHHHHHH',header,0,b'MZ',size%512,pages,len(relocations),32,0,0xffff,ss,sp,0,ip,cs,0x1c,0)
    for i,(seg,off) in enumerate(relocations): struct.pack_into('<HH',header,0x1c+4*i,off,seg)
    return bytes(header)+image

def exe2bin(machine):
    """EXE2BIN: a program set up for CS:100h becomes a memory image without its first 100h bytes,
    named from the input (.BIN in the current directory, into a directory given, or as named);
    a program at IP 0 gets its relocations applied to a base segment asked for in hex (asked again
    after a bad answer); others, and files that are no MZ program, cannot be converted."""
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='exe2bin-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'COMIMG.EXE').write_bytes(mz_file(b'\0'*0x100+b'COM PAYLOAD OK\r\n',ip=0x100))
        fixed=bytearray(b'FX&G-relocated-.....&G-end\r\n'); assert fixed[0x14:0x16]==b'&G'
        (media/'FIXIMG.EXE').write_bytes(mz_file(bytes(fixed),relocations=((0,2),(1,4))))
        (media/'CSIMG.EXE').write_bytes(mz_file(b'x'*32,cs=1))
        (media/'PLAIN.EXE').write_bytes(b'not a program\r\n')
        # A last page of FFFFh bytes: DOS 4 counts the size in 16 bits.
        odd=bytearray(mz_file(b'\0'*0x100+b'odd\r\n',ip=0x100)); struct.pack_into('<H',odd,2,0xffff)
        (media/'ODD.EXE').write_bytes(bytes(odd))
        (media/'OUTDIR').mkdir()
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'exe2bin-{machine}.log',machine)
        try:
            guest.command('exe2bin','File name must be specified')
            guest.command('exe2bin comimg')
            guest.command('type comimg.bin','COM PAYLOAD OK')
            guest.command('exe2bin comimg.exe outdir')
            guest.command('type outdir\\comimg.bin','COM PAYLOAD OK')
            guest.command('exe2bin comimg named.com')
            guest.command('type named.com','COM PAYLOAD OK')
            guest.p.stdin.write(b'exe2bin fiximg\r'); guest.p.stdin.flush()
            guest.expect(b'Fix-ups needed - base segment (hex):',timeout=60); time.sleep(0.5)
            guest.p.stdin.write(b'12z4\r'); guest.p.stdin.flush()
            guest.expect(b'Fix-ups needed - base segment (hex):',timeout=60); time.sleep(0.5)
            guest.p.stdin.write(b'1234\r'); guest.p.stdin.flush()
            guest.expect(guest.prompt,timeout=60)
            guest.command('type fiximg.bin','FXZY-relocated-.....ZY-end')
            guest.command('exe2bin csimg','File cannot be converted')
            guest.command('exe2bin plain','File cannot be converted')
            guest.command('exe2bin odd')
            guest.command('dir odd.bin','65279')
            guest.command('exe2bin missing','File not found')
            guest.command('exe2bin comimg a b','Too many parameters - b')
        finally:
            guest.close()
    print(f'PASS exe2bin: {machine} .COM images, output names, relocations with a base segment, refused programs',flush=True)

def spooler(machine):
    """PRINT: the resident part MSDOS.SYS keeps, installed once with its list device (LPT1 through
    PORTDRV.SYS, a file on the host), files printed while the shell waits for a command with tabs
    expanded, the end at ^Z and a form feed after each, wildcards, the queue shown, an 8086 program
    holding, queueing and canceling through INT 2Fh 0100h-0106h, and the installation switches
    refused once installed."""
    from mkimage import build
    cpu=() if machine=='itanium-vpc' else ('-cpu','madison-1500')
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='spooler-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('DEVICE=PORTDRV.SYS LPT1=378\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        (media/'TAB.PRN').write_bytes(b'one\ttwo\r\nthree\r\n\x1aafter the end')
        (media/'W1.PRN').write_bytes(b'wild one\r\n'); (media/'W2.PRN').write_bytes(b'wild two\r\n')
        (media/'QA.PRN').write_bytes(b'QA line\r\n'); (media/'QB.PRN').write_bytes(b'QB line\r\n')
        big=b''.join(b'%05d the quick brown fox jumps over the lazy dog 0123456789\r\n'%i for i in range(4000))
        (media/'BIG.PRN').write_bytes(big)
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        printer=fixture/'printer.bin'
        extra=cpu+('-chardev',f'file,id=dos_printer,path={printer}',
                   '-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer')
        guest=Guest(image,ROOT/'build'/f'spooler-{machine}.log',machine,extra_args=extra)
        try:
            guest.command('print /d:nodevice','List output is not assigned to a device')
            # Without /D the list device is asked for; a trailing colon is dropped.
            guest.p.stdin.write(b'print tab.prn big.prn\r'); guest.p.stdin.flush()
            guest.expect(b'Name of list device [PRN]: ',timeout=60); time.sleep(0.5)
            guest.p.stdin.write(b'lpt1:\r'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt,timeout=60)
            assert 'Resident part of PRINT installed' in result, result
            assert 'C:\\BIG.PRN is currently being printed' in result or 'C:\\BIG.PRN is in queue' in result, result
            def drained():
                # The shell waits for a key meanwhile: PRINT prints.
                for _ in range(60):
                    time.sleep(1)
                    if 'PRINT queue is empty' in guest.command('print'): return
                raise AssertionError('the queue did not empty')
            drained()
            guest.command('print /d:lpt1','Invalid switch - /d:lpt1')
            result=guest.command('print w?.prn')
            # Both are queued; the first may be printed already (the printer's bytes show both).
            assert 'C:\\W2.PRN' in result or 'PRINT queue is empty' in result, result
            drained()
            guest.command('print nofile.prn','File not found - C:\\NOFILE.PRN')
            if machine=='itanium-vpc' or cpu:
                result=guest.command('print16')
                for name in ('installed','queue held','cancel','missing file','list device'):
                    assert f'PRINT16: {name} ok' in result, (name,result)
                guest.command('if errorlevel 1 echo BAD')
                assert 'BAD' not in guest.output.decode('latin-1').split('if errorlevel 1 echo BAD')[-1], 'exit code'
            drained()
        finally:
            guest.close()
        data=printer.read_bytes()
        head=b'one     two\r\nthree\r\n\x0c'+big+b'\x0c'
        assert data.startswith(head), data[:200]
        rest=data[len(head):]
        assert sorted(rest[:2*len(b'wild one\r\n\x0c')].split(b'\x0c')[:2])==[b'wild one\r\n',b'wild two\r\n'], data
        assert rest[2*len(b'wild one\r\n\x0c'):]==b'QA line\r\n\x0c', data
    print(f'PASS spooler: {machine} PRINT installed on LPT1, background printing with tabs, ^Z and form feeds, wildcards, the queue, INT 2Fh from an 8086 program',flush=True)

def dpmi(machine):
    """Check DPMI16 host discovery, descriptors, DOS/linear memory, interrupt
    reflection/handlers, real-mode calls, virtual interrupts and exception cleanup.
    Run Watcom DPMI32 under DOS/32A, PMODE/W, CauseWay and DOS/4GW. The latter
    two require QEMU fixes for POP destination segments and flat-code CS overrides."""
    cpu=() if machine=='itanium-vpc' else ('-cpu','madison-1500')
    image=ROOT/'build'/f'dpmi-{machine}.img'; shutil.copyfile(ROOT/'build/dos-ia64.img',image)
    guest=Guest(image,ROOT/'build'/f'dpmi-{machine}.log',machine,extra_args=cpu)
    exit_code=lambda: 'BAD' not in guest.command('if errorlevel 1 echo BAD')
    tested=['DPMI16']
    try:
        before=free_memory(guest)
        for run in range(2):
            result=guest.command('dpmi16','DPMI16: exception handler')
            assert 'FAILED' not in result and 'VDM:' not in result, result
            for name in ('switch and version','descriptors','memory blocks','DOS memory','interrupts',
                         'real-mode call','virtual interrupts','exception handler','timer in real mode'):
                assert f'DPMI16: {name} ok' in result, (name,result)
            assert 'DPMI16: a line from real mode' in result, result
            assert exit_code(), 'DPMI16 exit code'
        if not (ROOT/'build/DPMI32A.EXE').exists():
            print(f'SKIP dpmi: {machine} DOS extenders (DPMI32 needs Open Watcom)',flush=True)
        else:
            for program,extender in (('dpmi32a','DOS/32A'),('dpmi32p','PMODE/W'),('dpmi32c','CauseWay'),('dpmi32g','DOS/4GW')):
                result=guest.command(program)
                good=all(f'DPMI32: {name} ok' in result for name in
                         ('DPMI version','4 MiB of memory','file write and read','real-mode call','timer ticks'))
                good=good and 'FAILED' not in result and 'VDM:' not in result and exit_code()
                if not good: raise AssertionError((extender,result))
                tested.append(extender)
        assert free_memory(guest)==before, 'memory kept'
    finally:
        guest.close()
    print(f'PASS dpmi: {machine} DPMI 0.9 for 8086 programs: {", ".join(tested)}',flush=True)

def dossessions(machine):
    """Several DOS sessions: two DOS Prompts set aside at once, each an icon of its own, taken
    up again in turn with their command lines waiting; a PIF that reserves Alt+Tab keeps that
    key for its program, while Alt+Esc still sets it aside."""
    from mkimage import build
    if not (ROOT/'build/media/WINDOWS/PROGMAN.EXE').exists():
        print(f'SKIP dossessions: {machine} (Interface Manager needs the WDK)',flush=True); return
    blue=(0,0,128)
    base=bytearray(0x171); base[2:0x20]=b'Keys'.ljust(30); struct.pack_into('<HH',base,0x20,640,128)
    base[0x24:0x32]=b'C:\\COMMAND.COM'; base[0x63]=0x10
    keys=pif_file(base,[(b'WINDOWS 286 3.0',struct.pack('<HHH',0,0,0x0001))])
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='dossessions-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'KEYS.PIF').write_bytes(keys)
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'dossessions-{machine}.ppm'
        guest=Guest(image,ROOT/'build'/f'dossessions-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
            ready=lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==blue and s.pixel(60,62)==blue
            wait_screen(qmp,dump,ready,timeout=180); time.sleep(2)
            # The icons along the bottom of the screen, 88 pixels apart.
            icon=lambda s,slot: sum(s.pixel(x,y)==(0,0,0) for y in range(532,566,2) for x in range(26+88*slot,62+88*slot,2))
            dos=lambda s: (s.width,s.height)!=(800,600)
            index=sum((media/'WINDOWS'/f).exists() for f in ('WINFILE.EXE','CONTROL.EXE','PRINTMAN.EXE','CLIPBRD.EXE'))
            # A word echoed: its output line (the screen drawn again on return goes to the
            # serial line too, so each word is new).
            def say(word):
                guest.p.stdin.write(f'echo {word}\r'.encode()); guest.p.stdin.flush()
                guest.expect(b'\n'+word.encode(),timeout=30); guest.expect(guest.prompt,timeout=30)
            def aside(key,shown):
                qmp.keys('alt',key)
                wait_screen(qmp,dump,lambda s: ready(s) and shown(s),timeout=60); time.sleep(1)
            def back(slot):
                d.double_click(43+88*slot,548,lambda: wait_screen(qmp,dump,dos,timeout=20),after=lambda: d.move(700,300)); time.sleep(2)
            def prompt():
                d.double_click(58+84*index,92,lambda: guest.expect(guest.prompt,timeout=40)); time.sleep(1)
            prompt(); say('alpha')
            aside('tab',lambda s: icon(s,0)>30)
            prompt(); say('beta')
            aside('tab',lambda s: icon(s,0)>30 and icon(s,1)>30)
            # The first again, then the second, each ending with EXIT.
            back(0); say('gamma')
            guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
            # The other's icon is the active window now.
            wait_screen(qmp,dump,lambda s: not dos(s) and not icon(s,0) and icon(s,1)>30,timeout=60); time.sleep(1)
            back(1); say('delta')
            guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
            wait_screen(qmp,dump,lambda s: ready(s) and not icon(s,0) and not icon(s,1),timeout=60); time.sleep(1)
            # KEYS.PIF reserves Alt+Tab: COMMAND.COM gets it (a tab, rubbed out); Alt+Esc switches.
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('c:\\keys.pif\n')
            guest.expect(guest.prompt,timeout=120); time.sleep(1)
            qmp.keys('alt','tab'); time.sleep(3)
            assert dos(qmp.screen(dump)), 'Alt+Tab was reserved'
            guest.p.stdin.write(b'\x08'); guest.p.stdin.flush(); say('epsilon')
            aside('esc',lambda s: icon(s,0)>30)
            back(0); say('zeta')
            guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
            wait_screen(qmp,dump,lambda s: ready(s) and not icon(s,0),timeout=60); time.sleep(1)
            if machine=='itanium-vpc':
                # An 8086 program and another DOS Prompt set aside at once: VGA16
                # taken up again and ended while the other COMMAND.COM is away, then GFX16 run
                # in that one, its VDM finding conventional memory as the first left it.
                red16,blue16,green16=(168,0,0),(0,0,168),(0,168,0)
                vga16=lambda s: (s.width,s.height)==(720,400) and s.pixel(364,4)==blue16 and s.pixel(90,32)==red16 and s.pixel(9,64)==green16
                prompt(); guest.p.stdin.write(b'vga16\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,vga16,timeout=60); time.sleep(1)
                aside('tab',lambda s: icon(s,0)>30)
                prompt(); say('eta')
                aside('tab',lambda s: icon(s,0)>30 and icon(s,1)>30)
                back(0); wait_screen(qmp,dump,vga16,timeout=60)
                guest.p.stdin.write(b'k'); guest.p.stdin.flush(); time.sleep(2)
                guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: not dos(s) and not icon(s,0) and icon(s,1)>30,timeout=60); time.sleep(1)
                back(1); say('theta')
                guest.p.stdin.write(b'gfx16\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(50,50)==red16,timeout=60)
                guest.p.stdin.write(b'k'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,480),timeout=60)
                guest.p.stdin.write(b'k'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(20,100)==(255,255,255),timeout=60)
                guest.p.stdin.write(b'k'); guest.p.stdin.flush()
                guest.expect(b'GFX16: ok',timeout=30); guest.expect(guest.prompt,timeout=30)
                guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: ready(s) and not icon(s,0) and not icon(s,1),timeout=60); time.sleep(1)
                # An 8086 program ending (AH=4Ch, code 7) after a later session started keeps
                # its exit code: IO.SYS, not the firmware, gives each program its way out.
                prompt(); guest.p.stdin.write(b'hello16 /KEY\r'); guest.p.stdin.flush()
                guest.expect(b'tail [ /KEY]',timeout=60); time.sleep(1)
                aside('tab',lambda s: icon(s,0)>30)
                prompt(); say('iota')
                aside('tab',lambda s: icon(s,0)>30 and icon(s,1)>30)
                back(0); guest.p.stdin.write(b'k'); guest.p.stdin.flush(); guest.expect(guest.prompt,timeout=30)
                guest.p.stdin.write(b'if errorlevel 7 echo SEVEN\r'); guest.p.stdin.flush()
                guest.expect(b'\nSEVEN',timeout=30); guest.expect(guest.prompt,timeout=30)
                guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: not dos(s) and not icon(s,0) and icon(s,1)>30,timeout=60); time.sleep(1)
                back(1); say('kappa')
                guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
                wait_screen(qmp,dump,lambda s: ready(s) and not icon(s,0) and not icon(s,1),timeout=60); time.sleep(1)
            d.keys('alt','f4'); time.sleep(1.5); d.keys('ret')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS dossessions: {machine} two DOS Prompts set aside at once as icons of their own and taken up again in turn'+(', VGA16 under VDM with them' if machine=='itanium-vpc' else '')+'; a PIF reserving Alt+Tab keeps it for COMMAND.COM while Alt+Esc switches',flush=True)

def pifedit(machine):
    """PIFEDIT, the PIF Editor: a PIF made in standard mode's form (program, title, parameters,
    start-up directory, XMS memory, a reserved key, not closing on exit) and 386 enhanced mode's
    (windowed, in the background, a foreground priority in Advanced), saved and checked byte by
    byte; Program Manager runs it (the batch file in the start-up directory with the parameters,
    a key waited for at the end); a PIF with bits and an extension the editor does not know
    keeps them when its title is changed."""
    from mkimage import build
    from check_image import inspect
    if not (ROOT/'build/media/WINDOWS/PIFEDIT.EXE').exists():
        print(f'SKIP pifedit: {machine} (Interface Manager needs the WDK)',flush=True); return
    navy=(0,0,128)
    base=bytearray(0x171); base[2:0x20]=b'Keep'.ljust(30); struct.pack_into('<HH',base,0x20,640,128)
    base[0x24:0x2f]=b'C:\\KEEP.EXE'; base[0x63]=0x30; base[0xe5:0xef]=bytes([0x7f,1,0,0xff,25,80,0,0,7,0])
    base[0x16f]=0x13; base[0x170]=0x60
    enhanced=bytearray(0x68); struct.pack_into('<8HII',enhanced,0,640,128,100,50,1024,0,1024,0,0x80021018,0xab000017)
    enhanced[0x18:0x20]=bytes([0x19,0,0x08,0,0x0f,0,0x01,0x5a]); enhanced[0x28:0x2f]=b'keep386'
    keep=pif_file(base,[(b'WINDOWS 386 3.0',bytes(enhanced)),(b'WINDOWS NT  3.1',bytes(range(1,9))),
                        (b'WINDOWS 286 3.0',struct.pack('<HHH',0,0,0x8001))])
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='pifedit-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'KEEP.PIF').write_bytes(keep)
        (media/'PIFTEST.BAT').write_bytes(b'@echo off\r\necho %1> RESULT.TXT\r\ncd >> RESULT.TXT\r\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'pifedit-{machine}.ppm'
        guest=Guest(image,ROOT/'build'/f'pifedit-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        # The PIF Editor's caption is at the top of the screen.
        editor=lambda s: (s.width,s.height)==(800,600) and s.pixel(300,8)==navy
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win pifedit\r'); guest.p.stdin.flush()
            shows(editor,timeout=180); time.sleep(2)
            d.type('C:\\PIFTEST.BAT'); d.keys('tab'); d.type('Pif Test'); d.keys('tab'); d.type('hello'); d.keys('tab'); d.type('C:\\WINDOWS')
            d.keys('alt','q'); d.type('256'); d.keys('alt','l'); d.type('2048')
            d.keys('alt','c'); d.keys('alt','s'); d.keys('spc')
            # 386 enhanced mode: windowed, in the background; Advanced: a foreground priority of 200.
            d.keys('alt','m'); d.keys('3'); time.sleep(2)
            d.keys('alt','w'); d.keys('alt','b')
            d.keys('alt','a'); time.sleep(2); d.keys('alt','f'); d.type('200'); d.keys('ret'); time.sleep(1)
            d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\TEST.PIF\n'); time.sleep(2)
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            # The other PIF: only its title changes.
            guest.p.stdin.write(b'c:\\windows\\win pifedit c:\\keep.pif\r'); guest.p.stdin.flush()
            shows(editor,timeout=120); time.sleep(2)
            d.keys('alt','t'); d.type('Kept'); d.keys('alt','f'); d.keys('s'); time.sleep(2)
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            # Program Manager runs the PIF: DOS has the screen until a key is pressed.
            guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(2)
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('c:\\test.pif\n')
            guest.expect(b'Press any key to return to Interface Manager.',timeout=120)
            guest.p.stdin.write(b' '); guest.p.stdin.flush()
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=120); time.sleep(2)
            d.keys('alt','f4'); time.sleep(1.5); d.keys('ret')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            result=guest.command('type c:\\windows\\result.txt','hello')
            assert 'C:\\WINDOWS' in result.upper(), result
            guest.command('cd','C:\\')
        finally:
            if qmp: qmp.close()
            guest.close()
        files=inspect(image)
    pif=files['TEST.PIF']
    assert len(pif)==0x221 and pif[1]==sum(pif[2:0x171])&0xff, (len(pif),pif[1])
    assert pif[0x24:0x33]==b'C:\\PIFTEST.BAT\0' and pif[2:0x20]==b'Pif Test'.ljust(30), pif[:0x40]
    assert pif[0xa5:0xab]==b'hello\0' and pif[0x65:0x70]==b'C:\\WINDOWS\0' and pif[0x63]==0x00, (pif[0x63],pif[0x65:0x70])
    assert pif[0x171:0x180]==b'MICROSOFT PIFEX' and pif[0x187:0x196]==b'WINDOWS 286 3.0' and pif[0x1a3:0x1b2]==b'WINDOWS 386 3.0'
    assert struct.unpack_from('<HHH',pif,0x19d)==(2048,256,0x0001), struct.unpack_from('<HHH',pif,0x19d)
    fg,bg=struct.unpack_from('<HH',pif,0x1b9+4); flags=struct.unpack_from('<I',pif,0x1b9+0x10)[0]
    assert (fg,bg,flags)==(200,50,0x00021002), (fg,bg,hex(flags))
    expected=bytearray(keep); expected[2:0x20]=b'Kept'.ljust(30); expected[1]=sum(expected[2:0x171])&0xff
    assert files['KEEP.PIF']==bytes(expected), [i for i,(x,y) in enumerate(zip(files['KEEP.PIF'],expected)) if x!=y][:10]
    print(f'PASS pifedit: {machine} PIF Editor: standard and 386 enhanced settings saved as Windows 3.0 lays them out, the checksum; a PIF run from Program Manager (program, parameters, start-up directory, a key at the end); unknown bits and extensions kept',flush=True)

from hlpwrite import hlp_picture_dib, hlp_file

def helpmenus(machine):
    """Check Notepad F1, Help Keyboard and Using Help, then open every
    accessory's compiled help index in WINHELP."""
    if not (ROOT/'build/media/WINDOWS/NOTEPAD.HLP').exists():
        print(f'SKIP helpmenus: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'helpmenus'); dump=ROOT/'build'/f'helpmenus-{machine}.ppm'
    green,navy,black=(0,128,0),(0,0,128),(0,0,0)
    # Notepad's window is at (24,24) and Help's over it (cascaded when opened again); its topic is
    # below the buttons, within the area counted.
    area=lambda s: [s.pixel(x,y) for y in range(102,470,2) for x in range(28,604,2)]
    count=lambda s,c: sum(p==c for p in area(s))
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win notepad\r'); guest.p.stdin.flush()
        wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(100,36)==navy,timeout=180); time.sleep(1)
        d.move(790,590)  # the pointer out of the way
        d.keys('f1'); wait_screen(qmp,dump,lambda s: count(s,green)>40,timeout=90); time.sleep(1)
        d.keys('alt','f4'); wait_screen(qmp,dump,lambda s: not count(s,green),timeout=30); time.sleep(1)
        d.keys('alt','h'); d.keys('k')
        wait_screen(qmp,dump,lambda s: not count(s,green) and count(s,black)>200,timeout=60); time.sleep(1)
        # Notepad's empty edit area again, above its scroll bar.
        d.keys('alt','f4'); wait_screen(qmp,dump,lambda s: not any(s.pixel(x,y)==black for y in range(102,440,2) for x in range(40,600,2)),timeout=30); time.sleep(1)
        d.keys('alt','h'); d.keys('u'); screen=wait_screen(qmp,dump,lambda s: count(s,green)>40,timeout=60); time.sleep(1)
        seen={tuple(area(settled(qmp,dump)))}
        for name in ('CALC','CALENDAR','CARDFILE','CLIPBRD','CONTROL','PBRUSH','PIFEDIT','PRINTMAN','PROGMAN',
                     'RECORDER','REVERSI','SETUP','SOL','TERMINAL','WINFILE','WRITE'):
            d.keys('alt','f'); d.keys('o'); time.sleep(1.5); d.type(f'C:\\WINDOWS\\{name}.HLP\n')
            screen=wait_screen(qmp,dump,lambda s: count(s,green)>40 and tuple(area(s)) not in seen,timeout=60)
            seen.add(tuple(area(screen))); time.sleep(1)
        d.keys('alt','f4'); time.sleep(1.5); d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS helpmenus: {machine} Notepad\'s F1, Help Keyboard and Using Help, and every accessory\'s help file at its index',flush=True)

def winhelp(machine):
    """Exercise generated Windows 3.0 help: links/popups, formatted topics,
    pictures, history/browse, keyword search, bookmarks and printing. HELPTEST
    checks WinHelp context/keyword requests and HELP_QUIT."""
    from mkimage import build
    from check_image import inspect
    if not (ROOT/'build/media/WINDOWS/WINHELP.EXE').exists():
        print(f'SKIP winhelp: {machine} (Interface Manager needs the WDK)',flush=True); return
    red,blue,green,navy,magenta,black=(255,0,0),(0,0,255),(0,128,0),(0,0,128),(255,0,255),(0,0,0)
    fonts=[('Helv',20,0,3,(0,0,0)),('Helv',24,1,3,(0,0,0)),('Helv',20,0,3,blue)]
    topics=[('Test Help Index',None,1,[
                ({'below':12,'left':12},[('font',1),b'Test Help Index']),
                ({'left':12},[('font',0),b'Read the ',('jump',1),b'second topic',('end',),b' or a ',('popup',2),b'defined term',('end',),b'.']),
                ({'above':12,'left':12,'border':8,'tabs':[144]},[('font',0),b'Name',('tab',),b'Value'])]),
            ('Second Topic',0,None,[
                ({'below':12,'left':12},[('font',1),b'Second Topic']),
                ({'left':12},[('font',0),('picture',0),b' A picture in the line.'])]),
            ('',None,None,[({'left':12},[('font',2),b'A word explained in a popup.'])])]
    palette=[(0,0,0),red]+[(255,255,255)]*14
    help_file=hlp_file('Test Help',fonts,topics,keywords=[('index',[0]),('second',[1]),('testing',[1])],maps=[(100,1)],
                       pictures=[hlp_picture_dib(16,16,palette,lambda x,y: 1)])
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='winhelp-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'TEST.HLP').write_bytes(help_file)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=PORTDRV.SYS LPT1=378\nSHELL=C:\\COMMAND.COM /P\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        printer_path=fixture/'printer.bin'; sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'winhelp-{machine}.ppm'
        extra=('-chardev',f'file,id=dos_printer,path={printer_path}',
               '-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer',
               '-qmp',f'unix:{sock},server=on,wait=off')
        guest=Guest(image,ROOT/'build'/f'winhelp-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',extra_args=extra)
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        # Help's window opens at (24,24); its topic below the buttons, from y=102.
        def where(s,colour,x0=28,y0=102,x1=604,y1=470): return [(x,y) for y in range(y0,y1,2) for x in range(x0,x1) if s.pixel(x,y)==colour]
        def count(s,colour,x0=0,y0=0,x1=800,y1=480): return sum(s.pixel(x,y)==colour for y in range(y0,y1,2) for x in range(x0,x1,2))
        index=lambda s: count(s,green)>40 and not count(s,red)
        second=lambda s: count(s,red)>40 and not count(s,green)
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win winhelp c:\\test.hlp\r'); guest.p.stdin.flush()
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(100,36)==navy and index(s),timeout=180)
            screen=settled(qmp,dump)
            # The hotspots: green, on one line, the jump first and the popup after a gap.
            spots=sorted(where(screen,green)); xs=[x for x,_ in spots]
            gap=max(range(1,len(xs)),key=lambda i: xs[i]-xs[i-1])
            jump,popup=spots[:gap],spots[gap:]
            centre=lambda points: (sum(x for x,_ in points)//len(points),min(y for _,y in points)+4)
            # The border under "Name<tab>Value", across the window; "Value" at the tab stop, an inch
            # (144 half points) from the column's left (x=36), "Name" ending well before.
            rule=[y for y in range(102,470) if sum(screen.pixel(x,y)==black for x in range(40,590))>400]
            assert rule, 'the border'
            line=range(rule[0]-16,rule[0]-2)
            assert any(screen.pixel(x,y)==black for y in line for x in range(133,170)), 'the text at the tab stop'
            assert not any(screen.pixel(x,y)==black for y in line for x in range(92,131)), 'nothing before the tab stop'
            d.click(*centre(jump)); shows(second); time.sleep(1)
            d.keys('alt','b'); shows(index); time.sleep(1)
            d.click(*centre(popup)); shows(lambda s: count(s,blue)>20); time.sleep(1)
            d.click(300,440); shows(lambda s: not count(s,blue)); time.sleep(1)
            # Browse >> and << (the buttons).
            d.click(320,85); shows(second); time.sleep(1)
            d.click(217,85); shows(index); time.sleep(1)
            d.keys('alt','s'); time.sleep(2); d.type('testing'); d.keys('ret'); time.sleep(1); d.keys('ret')
            shows(second); time.sleep(1)
            d.keys('alt','m'); d.keys('d'); time.sleep(2); d.keys('ret'); time.sleep(1)
            d.keys('alt','i'); shows(index); time.sleep(1)
            d.keys('alt','m'); d.keys('1'); shows(second); time.sleep(1)
            d.keys('alt','f'); d.keys('p'); time.sleep(3)
            shows(lambda s: s.pixel(100,36)==navy and second(s),timeout=120)
            # Print Manager, done, ends (its HELP_QUIT does not end Help, which it never asked):
            # the topic's end has reached LPT1:.
            deadline=time.monotonic()+120*SLOW
            while not printer_path.read_bytes().endswith(b'%%EOF\n'):
                assert time.monotonic()<deadline, printer_path.read_bytes()[-200:]
                time.sleep(0.5)
            time.sleep(2)
            shows(lambda s: s.pixel(100,36)==navy and second(s))
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            # A program's requests: HELPTEST's window (magenta) is clicked for each. The
            # pointer starts at the centre again.
            guest.p.stdin.write(b'c:\\windows\\win helptest\r'); guest.p.stdin.flush(); d=Desk(qmp)
            shows(lambda s: s.pixel(650,510)==magenta,timeout=180); time.sleep(1)
            for step,shown in ((1,second),(2,index)):
                d.click(715,545)
                result=guest.expect(f'HELPTEST: {step} '.encode(),timeout=60)+guest.expect(b'\n')
                assert 'ok' in result.split('HELPTEST:')[-1], result
                shows(lambda s: s.pixel(650,510)==magenta and shown(s)); time.sleep(2)
            d.click(715,545)
            result=guest.expect(b'HELPTEST: 3 ',timeout=60)+guest.expect(guest.prompt,timeout=60)
            assert 'HELPTEST: 3 ok' in result and 'leaked' not in result and 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
        files=inspect(image); output=printer_path.read_bytes()
    ini=files['WINDOWS/WINHELP.INI'].decode()
    assert '[TEST.HLP]' in ini and 'Second Topic=1' in ini, ini
    page,=printed(output,'Help - Second Topic',1)
    assert page.count(red,0,0,page.width,page.height)>100, 'the picture printed'
    print(f'PASS winhelp: {machine} a Windows 3.0 help file: its contents topic, a jump and Back, a popup, Browse, Search, a bookmark, the topic printed; WinHelp from a program: a [MAP] number, a keyword, HELP_QUIT',flush=True)

def printman(machine):
    """PRINTMAN, Print Manager, with LPT1 a parallel port into a file: the printer paused, a
    document printed from Notepad waits in the queue, then goes out when it is resumed; its
    spool file is gone afterwards."""
    from mkimage import build
    from check_image import inspect
    if not (ROOT/'build/media/WINDOWS/PRINTMAN.EXE').exists():
        print(f'SKIP printman: {machine} (Interface Manager needs the WDK)',flush=True); return
    navy,black=(0,0,128),(0,0,0)
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='printman-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=PORTDRV.SYS COM1=2F8 LPT1=378\nSHELL=C:\\COMMAND.COM /P\n')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        printer_path=fixture/'printer.bin'; sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'printman-{machine}.ppm'
        extra=('-chardev',f'file,id=dos_uart,path={fixture}/uart.bin','-device','isa-serial,index=1,iobase=0x2f8,irq=3,chardev=dos_uart',
               '-chardev',f'file,id=dos_printer,path={printer_path}','-device','isa-parallel,index=0,iobase=0x378,irq=7,chardev=dos_printer',
               '-qmp',f'unix:{sock},server=on,wait=off')
        guest=Guest(image,ROOT/'build'/f'printman-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',extra_args=extra)
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        # Print Manager opens at (24,24): its list's first line, the printer, at y=118; a document under it at y=134.
        task_list=lambda s: s.pixel(280,132)==navy
        document=lambda s: sum(s.pixel(x,y)==black for y in range(128,142) for x in range(40,300))>30
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(2)
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('printman /trace\n')
            shows(lambda s: s.pixel(100,36)==navy); time.sleep(1)
            d.keys('alt','p'); time.sleep(1)
            # Notepad, from Program Manager (second in the Task List), prints a file.
            d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret'); time.sleep(2)
            d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('notepad c:\\config.sys\n'); time.sleep(4)
            d.keys('alt','f'); d.keys('p')
            guest.expect(b'PRINTMAN: queued Notepad - CONFIG.SYS',timeout=90); time.sleep(1)
            d.keys('alt','f4'); time.sleep(2)
            assert printer_path.stat().st_size==0, 'printed while paused'
            # Back to Print Manager: the document waits; Resume sends it.
            d.keys('ctrl','esc'); shows(task_list); d.keys('down'); d.keys('ret')
            shows(document)
            d.keys('alt','r')
            guest.expect(b'PRINTMAN: sent Notepad - CONFIG.SYS',timeout=90)
            shows(lambda s: not document(s),timeout=90)
            d.keys('alt','v'); d.keys('x'); time.sleep(2)
            d.keys('alt','f4'); time.sleep(2); d.keys('ret')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
        page,=printed(printer_path.read_bytes(),'Notepad - CONFIG.SYS',1)
        assert page.count(black,75,135,400,200)>30, 'the printed text'
        files=inspect(image)
        assert not [name for name in files if name.endswith('.TMP')], [name for name in files if name.endswith('.TMP')]
    print(f'PASS printman: {machine} Print Manager: a paused printer, a document waiting in the queue, resumed and sent to LPT1:; no spool file left',flush=True)

def winsetup(machine):
    """Setup, from Program Manager: the display set to 1024 x 768 (SYSTEM.INI, which WIN.COM
    takes the next time), and Set Up Applications adding what it finds on C: to Program Manager."""
    if not (ROOT/'build/media/WINDOWS/SETUP.EXE').exists():
        print(f'SKIP winsetup: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'winsetup'); dump=ROOT/'build'/f'winsetup-{machine}.ppm'
    navy=(0,0,128)
    def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
        shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==navy,timeout=180); time.sleep(2)
        d.keys('alt','f'); d.keys('r'); time.sleep(2); d.type('setup\n')
        shows(lambda s: s.pixel(100,36)==navy); time.sleep(1)
        # Change System Settings (its caption at y=95): the display one down the list,
        # 1024 x 768; a message (caption at y=168) says when.
        setup_active=lambda s: s.pixel(120,34)==navy
        d.keys('alt','o'); d.keys('c'); shows(lambda s: s.pixel(150,95)==navy)
        d.keys('down'); d.keys('ret'); shows(lambda s: s.pixel(300,168)==navy)
        d.keys('ret'); shows(setup_active)
        # Set Up Applications on C: (the search's dialog, caption at y=115), all of them
        # (the list of what it found, caption at y=96, emptied by Add All). Setup is at
        # (24,24); its caption is active again once Program Manager has taken them.
        d.keys('alt','o'); d.keys('u'); shows(lambda s: s.pixel(150,115)==navy)
        d.keys('ret'); shows(lambda s: s.pixel(150,96)==navy and not setup_active(s),timeout=180)
        d.keys('alt','d'); shows(lambda s: not any(s.pixel(x,y)==(0,0,0) for y in range(140,322,3) for x in range(84,310,3)))
        d.keys('ret'); shows(setup_active,timeout=180); time.sleep(1)
        d.keys('alt','o'); d.keys('x'); time.sleep(2)
        d.keys('alt','f4'); time.sleep(2); d.keys('ret')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        guest.command('type c:\\windows\\system.ini','resolution=1024x768')
        result=guest.command('type c:\\windows\\progman.ini','DOS Applications')
        assert 'HELLO16.COM' in result.upper() and re.search(r'^Group\d+=Applications\r?$',result,re.M), result
        # Windows programs in one group, DOS programs (EFI applications, PE files too) in the other.
        def group(name):
            n=re.search(rf'^Group(\d+)={name}\r?$',result,re.M).group(1)
            m=re.search(rf'^\[Group{n}\]\r?\n(.*?)(?=^\[|\Z)',result,re.M|re.S)
            return m.group(1).upper() if m else ''
        assert 'XCOPY.EXE' in group('DOS Applications') and 'XCOPY.EXE' not in group('Applications'), result
        assert 'SAMPLES\\GENERIC.EXE' in group('Applications'), result
        # The next start is at 1024 x 768.
        guest.p.stdin.write(b'c:\\windows\\win hellowin\r'); guest.p.stdin.flush()
        shows(lambda s: (s.width,s.height)==(1024,768),timeout=120); time.sleep(2)
        qmp.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS winsetup: {machine} Setup: the display resolution kept in SYSTEM.INI and taken by WIN.COM, applications found and added to Program Manager',flush=True)

def calc(machine):
    """CALC with the keyboard and the mouse; "/trace" reports each display."""
    if not (ROOT/'build/media/WINDOWS/CALC.EXE').exists():
        print(f'SKIP calc: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'calc')
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win calc /trace\r'); guest.p.stdin.flush()
        guest.expect(b'CALC: client ',timeout=120)
        x0,y0=map(int,guest.expect(b'\n').split())
        def shows(text): guest.expect(f'CALC: {text}\n'.encode(),timeout=30)
        def key(col,row):
            """A button's centre: the memory column is -1, the C/CE/Back row 0."""
            x=x0+(10 if col<0 else 62+col*46)+20
            return x,y0+(40 if row==0 else 74+(row-1)*30)+12
        time.sleep(1)
        for k in ('1','2'): d.keys(k)
        d.keys('shift','8'); d.keys('3'); d.keys('ret'); shows('36.')
        d.keys('r'); shows('0.02777777777778')
        d.keys('2'); d.keys('shift','2'); shows('1.414213562373')
        d.keys('5'); d.keys('slash'); d.keys('0'); d.keys('ret'); shows('Cannot divide by zero')
        d.keys('esc'); shows('0.')
        # 7 + 8 = with the mouse, then MS, C, MR.
        for c,r in ((0,1),(3,4),(1,1),(4,4)): d.click(*key(c,r))
        shows('15.')
        d.click(*key(-1,3)); d.click(*key(4,0)); shows('0.')
        d.click(*key(-1,2)); shows('15.')
        indicator=lambda s: [s.pixel(x,y) for y in range(y0+42,y0+62) for x in range(x0+12,x0+48)].count((0,0,0))>5
        wait_screen(qmp,ROOT/'build'/f'calc-{machine}.ppm',indicator,what='the memory indicator')
        # Copy, clear, paste: the display comes back.
        d.keys('ctrl','c'); d.keys('esc'); shows('0.')
        d.keys('ctrl','v'); shows('15.')
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS calc: {machine} keyboard and mouse arithmetic, 1/x, sqrt, divide by zero, memory, clipboard, exit',flush=True)

def clock(machine):
    """CLOCK's analog face moves, the digital face shows, and minimized it draws its icon."""
    if not (ROOT/'build/media/WINDOWS/CLOCK.EXE').exists():
        print(f'SKIP clock: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'clock')
    dump=ROOT/'build'/f'clock-{machine}.ppm'
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win clock\r'); guest.p.stdin.flush()
        # The window is 200x220 at (24,24); its client area is below the frame, caption and menu bar.
        face=lambda s: [s.pixel(x,y) for y in range(80,230,2) for x in range(40,210,2)]
        dark=lambda s: sum(1 for p in face(s) if p==(0,0,0))
        first=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(36,230)==(255,255,255) and dark(s)>150,timeout=120)
        assert first.pixel(124,79)==(0,0,0), 'the 12 o\'clock mark'
        wait_screen(qmp,dump,lambda s: face(s)!=face(first),what='the second hand moving')
        d.keys('alt','s'); d.keys('d')
        wait_screen(qmp,dump,lambda s: s.pixel(124,79)==(255,255,255) and dark(s)>100,what='the digital face')
        d.keys('alt','spc'); d.keys('n')
        square=lambda s: [s.pixel(x,y) for y in range(534,564) for x in range(29,60)]
        # The highlighted title: digits vary with the time, so count the highlight around them.
        title=lambda s: [s.pixel(x,y) for y in range(566,582) for x in range(20,70)]
        wait_screen(qmp,dump,lambda s: square(s).count((255,255,255))>200 and square(s).count((0,0,0))>20 and title(s).count((0,0,128))>200,
                    what='the icon drawing the clock, its active title showing the time')
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS clock: {machine} analog face with moving hands, digital face, clock drawn as its icon, exit',flush=True)

def reversi(machine):
    """REVERSI: a move by mouse, the computer's reply, Hint and New, read from the screen."""
    if not (ROOT/'build/media/WINDOWS/REVERSI.EXE').exists():
        print(f'SKIP reversi: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'reversi')
    dump=ROOT/'build'/f'reversi-{machine}.ppm'
    green,red=(0,128,0),(255,0,0)
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win reversi\r'); guest.p.stdin.flush()
        screen=wait_screen(qmp,dump,lambda s: (s.width,s.height)==(800,600) and s.pixel(300,200)==green,timeout=120)
        # The board: the green square's left and top edges, eight squares across.
        top=next(y for y in range(40,400) if screen.pixel(300,y)==green)-1
        left=next(x for x in range(0,400) if screen.pixel(x,top+6)==green)-1
        right=left+1
        while screen.pixel(right+1,top+6)!=(192,192,192): right+=1
        sq=(right-left+1)//8
        def board(s):
            names={(0,0,0):'B',(255,255,255):'W',green:'.'}
            return ''.join(names.get(s.pixel(left+c*sq+sq//2,top+r*sq+sq//2),'?') for r in range(8) for c in range(8))
        start='.'*27+'WB......BW'+'.'*27
        assert board(screen)==start, board(screen)
        # Black plays d3 (row 2, column 3); white answers at once.
        d.click(left+3*sq+sq//2,top+2*sq+sq//2); d.move(760,560)
        screen=wait_screen(qmp,dump,lambda s: board(s).count('B')+board(s).count('W')==6,timeout=60)
        b=board(screen)
        assert b[2*8+3] in 'BW' and b.count('W')>=3 and b.count('B')>=1, b
        # Hint frames a legal square in red; New starts again.
        d.keys('alt','g'); d.keys('h')
        wait_screen(qmp,dump,lambda s: any(s.pixel(x,y)==red for y in range(top,top+8*sq,3) for x in range(left,left+8*sq,3)),what='the hint')
        d.keys('alt','g'); d.keys('n'); d.move(760,560)
        screen=wait_screen(qmp,dump,lambda s: board(s)==start,timeout=30)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS reversi: {machine} board, a move by mouse and the reply, Hint, New, exit',flush=True)

def fnt_font(face, points, height, ascent, glyph, first=32, last=255, default=ord('?')):
    """A raster font in the FNT 2.0 format: glyph(c) gives a character's width and its rows (ints,
    the leftmost pixel the highest bit of the width)."""
    count=last-first+1
    header_size=118; table=header_size; table_size=(count+1)*4
    face_at=table+table_size; bits_at=face_at+len(face)+1
    widths=[]; bitmaps=b''; offsets=[]
    for c in range(first,last+1):
        width,rows=glyph(c)
        widths.append(width); offsets.append(bits_at+len(bitmaps))
        columns=(width+7)//8
        for col in range(columns):
            for row in rows:
                bitmaps+=bytes([(row<<(columns*8-width))>>((columns-1-col)*8)&0xff])
    widths.append(widths[0]); offsets.append(offsets[0])   # the absolute space
    avg=sum(widths)//len(widths)
    header=struct.pack('<HI60sHHHHHHHBBBHBHHBHHBBBBHIIIIB',0x200,bits_at+len(bitmaps),b'Test font',0,points,96,96,ascent,0,0,
                       0,0,0,400,0,0,height,0x21,avg,max(widths),first,last,default-first,0,(sum((w+7)//8 for w in widths)+1)&~1,
                       0,face_at,0,bits_at,0)
    assert len(header)==header_size
    return header+b''.join(struct.pack('<HH',w,o) for w,o in zip(widths,offsets))+face.encode()+b'\0'+bitmaps

def fon_file(module, description, fonts):
    """A .FON: an NE module with no code whose resources are a FONTDIR and the FONT resources (FNT
    data), its description ("FONTRES 100,96,96 : ...") first in the non-resident names."""
    ne=0x40; shift=4
    names=bytes([len(module)])+module.encode()+b'\0\0'+b'\0'
    nonresident=bytes([len(description)])+description.encode()+b'\0\0'+b'\0'
    fontdir=struct.pack('<H',len(fonts))+b''.join(struct.pack('<H',i+1)+f[:113]+b'\0'+f[105:105] for i,f in enumerate(fonts))
    resources=[(0x8007,[fontdir]),(0x8008,fonts)]
    table_size=2+sum(8+12*len(items) for _,items in resources)+2+1
    resident_at=0x40+table_size; module_ref_at=resident_at+len(names); imported_at=module_ref_at; entry_at=imported_at+1
    nonresident_at=ne+entry_at+2
    data_at=(nonresident_at+len(nonresident)+15)//16*16
    table=struct.pack('<H',shift); blobs=b''; at=data_at
    for kind,items in resources:
        table+=struct.pack('<HHI',kind,len(items),0)
        for i,item in enumerate(items):
            size=(len(item)+15)//16*16
            table+=struct.pack('<HHHHI',at>>shift,size>>shift,0x0c50,0x8001+i,0)
            blobs+=item+bytes(size-len(item)); at+=size
    table+=struct.pack('<H',0)+b'\0'
    assert len(table)==table_size
    header=struct.pack('<2sBBHHIHHHHIIHHHHHHHHIHHHBB8x',b'NE',5,10,entry_at,2,0,0x8000,0,0,0,0,0,0,0,len(nonresident),
                       0x40,0x40,resident_at,module_ref_at,imported_at,nonresident_at,0,shift,len(fonts)+1,2,0)
    header=header[:62]+struct.pack('<H',0x300)
    mz=b'MZ'+bytes(0x3a)+struct.pack('<I',ne)
    body=mz+header+table+names+b'\0'+b'\0\0'
    assert len(body)==nonresident_at, (len(body),nonresident_at)
    body+=nonresident
    return body+bytes(data_at-len(body))+blobs

def block_font():
    """TEST.FON: "Test Blocks", solid blocks for characters, 12 pixels high (widths 5 to 7) and 30
    (widths 30 to 40, wider than a 32-bit row)."""
    def blocks(height,base,spread):
        def glyph(c):
            width=base+c%spread
            if c==32: return width,[0]*height
            full=(1<<width)-2
            return width,[0]+[full]*(height-2)+[0]
        return glyph
    small=fnt_font('Test Blocks',9,12,10,blocks(12,5,3)); large=fnt_font('Test Blocks',22,30,25,blocks(30,30,11))
    return fon_file('TESTFONT','FONTRES 100,96,96 : Test Blocks 9,22 (test)',[small,large])

def control(machine):
    """CONTROL: a color scheme applied and kept in WIN.INI (and read again when Interface Manager starts),
    the date set from Date/Time, the Mouse dialog; Fonts: a font file (made here: solid blocks, one
    size wider than 32 pixels) added, copied to the system directory and entered in [fonts], its
    sample drawn in it, loaded again at the next start, then removed; COM1's rate (Ports), the printer's
    port and Print Manager (Printers), the country (International), the repeat rate (Keyboard) and the
    beep (Sound), each found in WIN.INI. Settings are chosen from the Settings menu."""
    from mkimage import build
    if not (ROOT/'build/media/WINDOWS/CONTROL.EXE').exists():
        print(f'SKIP control: {machine} (Interface Manager needs the WDK)',flush=True); return
    navy,maroon,black=(0,0,128),(128,0,0),(0,0,0)
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='control-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'TEST.FON').write_bytes(block_font())
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        sock=fixture/'qmp.sock'; dump=ROOT/'build'/f'control-{machine}.ppm'
        guest=Guest(image,ROOT/'build'/f'control-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        def shows(test,timeout=60): return wait_screen(qmp,dump,test,timeout)
        def blacks(s): return sum(s.pixel(x,y)==black for y in range(0,600,2) for x in range(0,800,2))
        def setting(key): d.keys('alt','s'); d.keys(key); time.sleep(2)
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.p.stdin.write(b'c:\\windows\\win control\r'); guest.p.stdin.flush()
            # The window opens at (24,24): its caption is active.
            shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(100,32)==navy,timeout=120)
            time.sleep(1)
            # Color (selected first): the next scheme, Arizona, has a maroon active caption.
            d.keys('ret'); time.sleep(1.5)
            d.keys('down'); d.keys('ret')
            shows(lambda s: s.pixel(100,32)==maroon,timeout=30)
            # Date/Time: year, month and day.
            setting('d')
            d.type('1999'); d.keys('tab'); d.type('12'); d.keys('tab'); d.type('31'); d.keys('ret'); time.sleep(1)
            # Mouse opens and cancels.
            setting('m'); d.keys('esc'); time.sleep(1)
            # Fonts: a built-in face's sample, then TEST.FON added; its sample is mostly black.
            setting('f'); time.sleep(1)
            before=blacks(settled(qmp,dump))
            d.keys('alt','a'); time.sleep(2); d.type('C:\\TEST.FON\n')
            shows(lambda s: blacks(s)>before+1500,timeout=30); time.sleep(1)
            d.keys('ret'); time.sleep(1)
            # Ports: COM1 at 2400 baud (two up from 9600).
            setting('o'); d.keys('ret'); time.sleep(2); d.keys('up'); d.keys('up'); d.keys('ret'); time.sleep(1); d.keys('esc'); time.sleep(1)
            # Printers: the printer moved to LPT2 (Configure), Print Manager not used.
            setting('p'); d.keys('alt','c'); time.sleep(2); d.keys('down'); d.keys('ret'); time.sleep(1)
            d.keys('alt','u'); d.keys('ret'); time.sleep(1)
            # International: United Kingdom (one up from United States, the last).
            setting('i'); d.keys('up'); time.sleep(1); d.keys('ret'); time.sleep(1)
            # Keyboard: a slower repeat; Sound: no warning beeps.
            setting('k'); [d.keys('left') for _ in range(5)]; d.keys('ret'); time.sleep(1)
            setting('u'); d.keys('spc'); d.keys('ret'); time.sleep(1)
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            result=guest.command('type c:\\windows\\win.ini','[colors]')
            assert 'ActiveTitle=128 0 0' in result and 'Background=0 128 128' in result, result
            assert 'Test Blocks 9,22 (test)=TEST.FON' in result, result
            for line in ('COM1:=2400,n,8,1','PostScript Printer=PSCRIPT,LPT2:','device=PostScript Printer,PSCRIPT,LPT2:','spooler=no',
                         'iCountry=44','sCountry=United Kingdom','iDate=1','sShortDate=dd/MM/yyyy','KeyboardSpeed=26','Beep=no'):
                assert line in result, (line,result)
            guest.command('dir c:\\windows\\system\\test.fon','TEST     FON')
            guest.command('date /t','1999-12-31')
            # Interface Manager starts with the saved colors and the font.
            guest.p.stdin.write(b'c:\\windows\\win control\r'); guest.p.stdin.flush()
            screen=shows(lambda s: (s.width,s.height)==(800,600) and s.pixel(100,32)==maroon,timeout=120)
            assert screen.pixel(700,500)==(0,128,128), screen.pixel(700,500)
            time.sleep(1)
            setting('f'); time.sleep(1)
            with_font=blacks(settled(qmp,dump))
            # Removed: the list's first entry is a built-in face again.
            d.keys('alt','r'); time.sleep(1.5); d.keys('ret')
            shows(lambda s: blacks(s)<with_font-1500,timeout=30); time.sleep(1)
            d.keys('ret'); time.sleep(1)
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
            result=guest.command('type c:\\windows\\win.ini','[colors]')
            assert 'Test Blocks' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS control: {machine} color scheme applied, saved to WIN.INI and read at start-up, date set, mouse dialog; a font file added (copied, [fonts], its sample), loaded at the next start, removed; ports, printers, international, keyboard and sound settings in WIN.INI',flush=True)

def calendar(machine):
    """CALENDAR: an appointment typed in the Day view, the Month view, the next day and back, a note,
    Save As, New and Open (traced), the save prompt on exit, and the file from DOS."""
    if not (ROOT/'build/media/WINDOWS/CALENDAR.EXE').exists():
        print(f'SKIP calendar: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'calendar')
    dump=ROOT/'build'/f'calendar-{machine}.ppm'
    try:
        d=Desk(qmp)
        def traced(text,timeout=30): return guest.expect(f'CALENDAR: {text}'.encode(),timeout=timeout)
        guest.p.stdin.write(b'c:\\windows\\win calendar /trace\r'); guest.p.stdin.flush()
        traced('day ',timeout=120); shown=guest.expect(b'\n').strip()
        time.sleep(1.5)
        # The 7:00 AM appointment has the focus.
        d.type('Meeting at noon'); d.keys('down')
        d.keys('f9'); traced('view month')
        wait_screen(qmp,dump,lambda s: sum(s.pixel(x,y)==(0,0,128) for y in range(110,420,4) for x in range(30,610,4))>20,what='the month with the day selected')
        d.keys('f8'); traced('view day'); traced('at 0700 Meeting at noon')
        d.keys('ctrl','pgdn'); traced('day '); assert guest.expect(b'\n').strip()!=shown
        d.keys('ctrl','pgup'); traced('day '+shown); traced('at 0700 Meeting at noon')
        # A note in the scratch pad, then Save As.
        d.keys('tab'); d.type('note one'); d.keys('tab')
        d.keys('alt','f'); d.keys('a'); time.sleep(1.5)
        d.type('C:\\TEST.CAL\n'); traced('saved C:\\TEST.CAL')
        d.keys('alt','f'); d.keys('n'); traced('day ')
        d.keys('alt','f'); d.keys('o'); time.sleep(1.5)
        d.type('C:\\TEST.CAL\n'); traced('opened C:\\TEST.CAL'); traced('at 0700 Meeting at noon')
        # A change, then Alt+F4: the save prompt, answered No.
        d.type('x'); d.keys('alt','f4')
        wait_screen(qmp,dump,lambda s: any(s.pixel(x,y)==(0,0,128) for y in range(150,450,3) for x in range(150,650,3)),what='the save prompt')
        d.keys('n')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('type c:\\test.cal','CALENDAR 60 7')
        assert '0700 Meeting at noon' in result and 'NOTE note one' in result and 'Meeting at noonx' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS calendar: {machine} Day and Month views, appointment, next and previous day, note, Save As, New, Open, save prompt',flush=True)

def sol(machine):
    """SOL with seed 10 (tableau tops 7H 9S 6C JH 3S AH 5D): drag 6C onto 7H, Undo, double-click AH
    to a foundation, deal from the deck, exit."""
    if not (ROOT/'build/media/WINDOWS/SOL.EXE').exists():
        print(f'SKIP sol: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'sol')
    dump=ROOT/'build'/f'sol-{machine}.ppm'
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win sol /seed:10 /trace\r'); guest.p.stdin.flush()
        guest.expect(b'SOL: client ',timeout=120)
        x0,y0=map(int,guest.expect(b'\n').split())
        def table(text): guest.expect(f'SOL: {text}\n'.encode(),timeout=30)
        table('T 7H 9S 6C JH 3S AH 5D | W - | F - - - - | D 24')
        time.sleep(1.5)
        # Pile n's top card: n cards face down above it, 5 pixels each, from y=116; piles 81 apart.
        def top(n): return x0+8+81*n+35,y0+116+5*n+30
        start=top(2); end=(x0+8+35+5,y0+116+30+18)
        d.move(*start); qmp.pointer(button='left',down=True); time.sleep(0.2)
        d.move((start[0]+end[0])//2,(start[1]+end[1])//2); d.move(*end); time.sleep(0.2)
        qmp.pointer(button='left',down=False)
        table('T 6C 9S - JH 3S AH 5D | W - | F - - - - | D 24')
        d.keys('backspace'); table('undo'); table('T 7H 9S 6C JH 3S AH 5D | W - | F - - - - | D 24')
        d.click(*top(5),double=True)
        table('T 7H 9S 6C JH 3S - 5D | W - | F AH - - - | D 24')
        d.click(x0+8+35,y0+8+48); table('T 7H 9S 6C JH 3S - 5D | W 7D | F AH - - - | D 23')
        wait_screen(qmp,dump,lambda s: s.pixel(x0+8+3*81+60,y0+8+12)==(255,255,255) and s.pixel(x0+8+3*81+35,y0+8+50)==(255,0,0),what='AH on the foundation')
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS sol: {machine} cards drawn, drag between piles, Undo, double-click to a foundation, deal from the deck',flush=True)

def pbrush(machine):
    """PBRUSH: a filled box and a line drawn with the mouse in chosen colors, the paint roller and
    Undo, Save As a .BMP, New and Open bringing the picture back, the save prompt; the file from DOS."""
    if not (ROOT/'build/media/WINDOWS/PBRUSH.EXE').exists():
        print(f'SKIP pbrush: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'pbrush')
    dump=ROOT/'build'/f'pbrush-{machine}.ppm'
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win pbrush /trace\r'); guest.p.stdin.flush()
        guest.expect(b'PBRUSH: client ',timeout=120)
        cx,cy,cw,ch=map(int,guest.expect(b'\n').split())
        guest.expect(b'PBRUSH: view ')
        vx,vy=map(int,guest.expect(b'\n').split())
        swatch=lambda i: (cx+48+(i%8)*24+11,cy+ch-40+4+(i//8)*18+8)
        tool=lambda i: (cx+4+(i%2)*26+12,cy+4+(i//2)*26+12)
        def drag(x0,y0,x1,y1):
            d.move(x0,y0); qmp.pointer(button='left',down=True); time.sleep(0.3)
            d.move((x0+x1)//2,(y0+y1)//2); d.move(x1,y1); time.sleep(0.3)
            qmp.pointer(button='left',down=False); time.sleep(0.8)
        def color(i):
            d.click(*swatch(i)); guest.expect(f'PBRUSH: foreground {i}\n'.encode(),timeout=30)
        red,blue,yellow,white=(255,0,0),(0,0,255),(255,255,0),(255,255,255)
        box=lambda s: s.pixel(vx+40,vy+30)   # up and left of the clicks: the cursor is not there
        line=lambda s: s.pixel(vx+100,vy+120)
        time.sleep(1)
        color(9); d.click(*tool(4)); drag(vx+20,vy+20,vx+120,vy+80)
        color(12); d.click(*tool(2)); drag(vx+20,vy+120,vx+200,vy+120)
        screen=wait_screen(qmp,dump,lambda s: box(s)==red and line(s)==blue,timeout=30)
        assert screen.pixel(vx+300,vy+200)==white and screen.pixel(vx+130,vy+50)==white
        # The paint roller fills the box; Undo takes it back.
        color(11); d.click(*tool(8)); d.click(vx+70,vy+50)
        wait_screen(qmp,dump,lambda s: box(s)==yellow and line(s)==blue,timeout=60)
        d.keys('ctrl','z'); wait_screen(qmp,dump,lambda s: box(s)==red,timeout=30)
        # Save As, New, then Open; the file dialog's caption (left of its title) is active before typing.
        file_dialog=lambda s: s.pixel(150,79)==(0,0,128)
        d.keys('alt','f'); d.keys('a'); wait_screen(qmp,dump,file_dialog,timeout=30); d.type('C:\\TEST.BMP\n')
        wait_screen(qmp,dump,lambda s: not file_dialog(s) and box(s)==red,timeout=60); time.sleep(1)
        d.keys('alt','f'); d.keys('n'); wait_screen(qmp,dump,lambda s: box(s)==white and line(s)==white,timeout=30)
        d.keys('alt','f'); d.keys('o'); wait_screen(qmp,dump,file_dialog,timeout=30); d.type('C:\\TEST.BMP\n')
        wait_screen(qmp,dump,lambda s: box(s)==red and line(s)==blue,timeout=60)
        # A change, then closing asks to save it; No leaves the file as saved.
        d.click(*tool(3)); drag(vx+300,vy+200,vx+350,vy+250)
        d.keys('alt','f4'); time.sleep(1.5); d.keys('n')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('dir c:\\test.bmp','TEST     BMP')
        assert '921654' in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS pbrush: {machine} filled box and line by mouse, paint roller and Undo, Save As, New, Open, save prompt, 24-bit BMP',flush=True)

def cardfile(machine):
    """CARDFILE: three cards kept in index order with text, Go To, Find, a card behind brought
    forward by mouse, the list view, Save As, New and Open; the file from DOS."""
    if not (ROOT/'build/media/WINDOWS/CARDFILE.EXE').exists():
        print(f'SKIP cardfile: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'cardfile')
    dump=ROOT/'build'/f'cardfile-{machine}.ppm'
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win cardfile /trace\r'); guest.p.stdin.flush()
        guest.expect(b'CARDFILE: client ',timeout=120)
        cx,cy=map(int,guest.expect(b'\n').split())
        def cards(text): guest.expect(f'CARDFILE: cards {text}\n'.encode(),timeout=30)
        cards(' front ')
        time.sleep(1)
        d.keys('f6'); time.sleep(1.5); d.type('pear\n'); cards('pear front pear')
        d.type('green')
        d.keys('f7'); time.sleep(1.5); d.type('apple\n'); cards('apple,pear front apple')
        d.type('red fruit')
        d.keys('f7'); time.sleep(1.5); d.type('fig\n'); cards('apple,fig,pear front fig')
        d.type('sweet')
        d.keys('f4'); time.sleep(1.5); d.type('pea\n'); cards('apple,fig,pear front pear')
        d.keys('alt','s'); d.keys('f'); time.sleep(1.5); d.type('sweet\n'); cards('apple,fig,pear front fig')
        # Behind fig are pear, then apple: a click on apple's index line brings it forward.
        d.click(cx+92,cy+32); cards('apple,fig,pear front apple')
        d.keys('alt','v'); d.keys('l')
        wait_screen(qmp,dump,lambda s: s.pixel(cx+300,cy+25)==(0,0,128),what='the list view\'s selection')
        d.keys('alt','v'); d.keys('c'); time.sleep(1)
        d.keys('alt','f'); d.keys('a'); time.sleep(2); d.type('C:\\TEST.CRD\n'); time.sleep(4)
        d.keys('alt','f'); d.keys('n'); cards(' front ')
        d.keys('alt','f'); d.keys('o'); time.sleep(2); d.type('C:\\TEST.CRD\n'); cards('apple,fig,pear front apple')
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('type c:\\test.crd','CARDFILE')
        for line in ('I:apple\nB:red fruit\nE','I:fig\nB:sweet\nE','I:pear\nB:green\nE'): assert line in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS cardfile: {machine} cards in index order, text, Go To, Find, card brought forward by mouse, list view, Save As, New, Open',flush=True)

def winfile(machine):
    """WINFILE: the tree of C:, directories created, a file copied, renamed and copied again, an error
    reported, a directory deleted with its files, a directory window, a document opened in Notepad
    and a double-click on ".."; "/trace" reports what the windows show and do. Then the disk from DOS."""
    if not (ROOT/'build/media/WINDOWS/WINFILE.EXE').exists():
        print(f'SKIP winfile: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'winfile')
    dump=ROOT/'build'/f'winfile-{machine}.ppm'
    blue=(0,0,128)
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win winfile /trace\r'); guest.p.stdin.flush()
        result=guest.expect(b'WINFILE: ready',timeout=150)
        assert 'WINFILE: tree C:\\ ' in result and ' WINDOWS ' in result, result
        time.sleep(2)
        def shows(text): return guest.expect(text.encode(),timeout=60)
        # Each operation refreshes the tree; nothing reads keys meanwhile, so wait for it.
        def op(item,fields,done,refresh=True):
            d.keys('alt','f'); d.keys(item); time.sleep(1.5)
            for i,f in enumerate(fields):
                if i: d.keys('tab')
                d.type(f)
            d.keys('ret'); shows(done)
            if refresh: shows('WINFILE: tree')
            time.sleep(1)
        op('e',['C:\\KEEP'],'WINFILE: created C:\\KEEP')
        op('e',['C:\\TESTDIR'],'WINFILE: created C:\\TESTDIR')
        op('c',['C:\\WINDOWS\\WIN.COM','C:\\TESTDIR'],'WINFILE: copied C:\\WINDOWS\\WIN.COM to C:\\TESTDIR\\WIN.COM')
        op('n',['C:\\TESTDIR\\WIN.COM','RENAMED.COM'],'WINFILE: renamed C:\\TESTDIR\\WIN.COM to C:\\TESTDIR\\RENAMED.COM')
        op('c',['C:\\TESTDIR\\RENAMED.COM','C:\\KEEP'],'WINFILE: copied C:\\TESTDIR\\RENAMED.COM to C:\\KEEP\\RENAMED.COM')
        op('c',['C:\\README.TXT','C:\\KEEP'],'WINFILE: copied C:\\README.TXT to C:\\KEEP\\README.TXT')
        # An existing directory cannot be created: a message box says so.
        op('e',['C:\\KEEP'],'WINFILE: error Cannot create C:\\KEEP',refresh=False)
        wait_screen(qmp,dump,lambda s: sum(s.pixel(x,170)==blue for x in range(200,600))>100,what='the error message box')
        d.keys('ret'); time.sleep(1)
        # Delete asks before removing a directory and its files.
        d.keys('alt','f'); d.keys('d'); time.sleep(1.5); d.type('C:\\TESTDIR'); d.keys('ret'); time.sleep(1.5)
        wait_screen(qmp,dump,lambda s: sum(s.pixel(x,170)==blue for x in range(200,600))>100,what='the delete confirmation')
        d.keys('y'); shows('WINFILE: deleted C:\\TESTDIR\n')
        shows('WINFILE: tree'); result=shows('\n')
        assert ' KEEP ' in result and 'TESTDIR' not in result, result
        time.sleep(1)
        # KEEP in the tree opens a directory window; its document opens in Notepad.
        d.keys('k'); d.keys('ret'); shows('WINFILE: dir C:\\KEEP\\*.*'); result=shows('\n')
        assert result.split()==['..','README.TXT','RENAMED.COM'], result
        time.sleep(1)
        d.keys('down'); d.keys('ret'); shows('WINFILE: run notepad.exe C:\\KEEP\\README.TXT')
        # Notepad is at (24,24): its caption, left of the title.
        wait_screen(qmp,dump,lambda s: s.pixel(100,36)==blue,timeout=120)
        d.keys('alt','f4'); wait_screen(qmp,dump,lambda s: s.pixel(60,10)==blue)
        time.sleep(1)
        # A double-click on ".." goes up.
        d.double_click(300,73,lambda: shows('WINFILE: dir C:\\*.*'))
        time.sleep(1)
        d.keys('alt','f4')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
        result=guest.command('dir c:\\keep','README   TXT')
        assert 'RENAMED  COM' in result, result
        guest.command('dir c:\\testdir','File not found',error=True)
    finally:
        qmp.close(); guest.close()
    # The CD (MSCDEX's D:) is a drive to choose, and writing to it fails with a message.
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='winfile-cd-') as temporary:
        work=Path(temporary); media=work/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('FILES=64\nDEVICE=C:\\EFICD.SYS /D:MSCD001\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        image=work/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        iso=work/'cd.iso'; make_iso(iso,'DISC',{'README.TXT':b'on the disc\r\n','SUB/FILE.TXT':b'x\r\n'})
        sock=work/'q.sock'; iface='scsi' if machine=='itanium-vpc' else 'ide'
        guest=Guest(image,ROOT/'build'/f'winfile-cd-{machine}.log',machine if machine=='itanium-vpc' else machine+',i8042=on',
                    extra_args=('-drive',f'file={iso},media=cdrom,if={iface},index=1,id=cd0','-qmp',f'unix:{sock},server=on,wait=off'))
        qmp=None
        try:
            qmp=Qmp(sock); d=Desk(qmp)
            guest.command('mscdex /d:mscd001','Drive D:')
            guest.p.stdin.write(b'c:\\windows\\win winfile /trace\r'); guest.p.stdin.flush()
            guest.expect(b'WINFILE: ready',timeout=150); time.sleep(2)
            d.keys('alt','d'); d.keys('s'); time.sleep(1.5); d.keys('end'); d.keys('ret')
            guest.expect(b'WINFILE: tree D:\\ SUB\n',timeout=60); time.sleep(2)
            d.keys('alt','f'); d.keys('e'); time.sleep(1.5); d.type('D:\\NEW'); d.keys('ret')
            guest.expect(b'WINFILE: error Cannot create D:\\NEW: Access denied.',timeout=60); time.sleep(1)
            d.keys('ret'); time.sleep(1)
            d.keys('alt','f'); d.keys('c'); time.sleep(1.5); d.type('C:\\README.TXT'); d.keys('tab'); d.type('D:\\'); d.keys('ret')
            guest.expect(b'WINFILE: error Cannot copy C:\\README.TXT: Access denied.',timeout=60); time.sleep(1)
            d.keys('ret'); time.sleep(1)
            d.keys('alt','f4')
            result=guest.expect(guest.prompt,timeout=60)
            assert 'leaked' not in result and 'WIN:' not in result, result
        finally:
            if qmp: qmp.close()
            guest.close()
    print(f'PASS winfile: {machine} tree, create/copy/rename/delete with dialogs, error message, directory windows, Notepad for a .TXT, "..", the CD read-only',flush=True)

def dosprompt(machine):
    """Program Manager's DOS Prompt: COMMAND.COM runs full screen while Interface Manager
    waits, as in Windows 3.0's standard mode; Alt+Tab at its prompt sets it aside as an icon (and
    Interface Manager will not end meanwhile), a double click brings it back with its screen, and
    EXIT returns to Interface Manager, whose screen comes back whole."""
    if not (ROOT/'build/media/WINDOWS/PROGMAN.EXE').exists():
        print(f'SKIP dosprompt: {machine} (Interface Manager needs the WDK)',flush=True); return
    guest,qmp=windows_guest(machine,'dosprompt')
    dump=ROOT/'build'/f'dosprompt-{machine}.ppm'; blue=(0,0,128)
    windows=ROOT/'build/media/WINDOWS'
    try:
        d=Desk(qmp)
        guest.p.stdin.write(b'c:\\windows\\win\r'); guest.p.stdin.flush()
        ready=lambda s: (s.width,s.height)==(800,600) and s.pixel(60,10)==blue and s.pixel(60,62)==blue
        before=wait_screen(qmp,dump,ready,timeout=180); time.sleep(2)
        # Main: File Manager, Control Panel, Print Manager, Clipboard, then DOS Prompt.
        index=sum((windows/f).exists() for f in ('WINFILE.EXE','CONTROL.EXE','PRINTMAN.EXE','CLIPBRD.EXE'))
        d.double_click(58+84*index,92,lambda: guest.expect(guest.prompt,timeout=40))
        wait_screen(qmp,dump,lambda s: (s.width,s.height)!=(800,600),what='DOS with the screen')
        guest.command('ver','IA-64/EFI DOS Version 4.00')
        # Alt+Tab while it waits for a command: Program Manager again, COMMAND an icon on the desktop.
        icon=lambda s: sum(s.pixel(x,y)==(0,0,0) for y in range(532,566,2) for x in range(26,62,2))
        text=lambda s: sum(s.pixel(x,y)!=(0,0,0) for y in range(0,s.height,2) for x in range(0,s.width,2))
        qmp.keys('alt','tab')
        away=wait_screen(qmp,dump,lambda s: ready(s) and icon(s)>30,timeout=60); time.sleep(1)
        assert away.pixel(300,100)==before.pixel(300,100), 'the screen came back'
        # Exiting is refused while it is there.
        d.keys('alt','f4'); time.sleep(1.5); d.keys('ret'); time.sleep(2); d.keys('ret')
        wait_screen(qmp,dump,lambda s: ready(s) and icon(s)>30,timeout=30); time.sleep(1)
        # A double click on the icon: its screen again, its text kept, the command line still waiting.
        d.double_click(43,548,lambda: wait_screen(qmp,dump,lambda s: (s.width,s.height)!=(800,600) and text(s)>300,timeout=20),
                       after=lambda: d.move(700,300))
        # The screen is drawn again through the firmware console, which the serial line echoes;
        # the command's output line is what shows the prompt took it.
        time.sleep(2); guest.p.stdin.write(b'echo back again\r'); guest.p.stdin.flush()
        guest.expect(b'\nback again',timeout=30)
        if machine=='itanium-vpc':
            # A 16-bit program is set aside the same way, VDM keeping its conventional memory
            # and the adapter (registers, palette and planes): VGA16 waiting for a key in text
            # mode, then GFX16 in mode 12h.
            def aside_and_back(shown):
                wait_screen(qmp,dump,shown,timeout=60); time.sleep(1)
                qmp.keys('alt','tab')
                wait_screen(qmp,dump,lambda s: ready(s) and icon(s)>30,timeout=60); time.sleep(1)
                d.double_click(43,548,lambda: wait_screen(qmp,dump,shown,timeout=20),after=lambda: d.move(700,300)); time.sleep(1)
            blue16,red16,green16=(0,0,168),(168,0,0),(0,168,0)
            guest.p.stdin.write(b'vga16\r'); guest.p.stdin.flush()
            index,_=guest.expect_any(b'VGA16: ok',b'VGA16: FAIL',timeout=60)
            assert index==0, 'VGA16: FAIL'+guest.expect(b'\n',timeout=10)
            aside_and_back(lambda s: (s.width,s.height)==(720,400) and s.pixel(364,4)==blue16 and s.pixel(90,32)==red16 and s.pixel(9,64)==green16)
            guest.p.stdin.write(b'k'); guest.p.stdin.flush(); guest.expect(guest.prompt,timeout=30)
            guest.p.stdin.write(b'gfx16\r'); guest.p.stdin.flush()
            wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(50,50)==red16,timeout=60)
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            aside_and_back(lambda s: (s.width,s.height)==(640,480) and s.pixel(50,10)==green16 and s.pixel(100,120)==red16 and s.pixel(50,60)==(0,0,0) and s.lit(0,320,48,336))
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            wait_screen(qmp,dump,lambda s: (s.width,s.height)==(640,400) and s.pixel(20,100)==(255,255,255),timeout=60)
            guest.p.stdin.write(b'k'); guest.p.stdin.flush()
            result=guest.expect(guest.prompt,timeout=30)
            assert 'GFX16: ok' in result, result
        guest.p.stdin.write(b'exit\r'); guest.p.stdin.flush()
        after=wait_screen(qmp,dump,lambda s: ready(s) and not icon(s),timeout=120)
        assert after.pixel(300,100)==before.pixel(300,100) and after.pixel(500,300)==before.pixel(500,300), 'the screen came back'
        d.keys('alt','f4'); time.sleep(1.5); d.keys('ret')
        result=guest.expect(guest.prompt,timeout=60)
        assert 'leaked' not in result and 'WIN:' not in result, result
    finally:
        qmp.close(); guest.close()
    print(f'PASS dosprompt: {machine} DOS Prompt from Program Manager runs COMMAND.COM full screen; Alt+Tab sets it aside as an icon, exiting is refused meanwhile, a double click brings it back with its screen'+(', as for VGA16 in text mode and GFX16 in mode 12h' if machine=='itanium-vpc' else '')+'; EXIT returns',flush=True)

def floppies(machine):
    from mkimage import build
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='floppy-test-') as temporary:
        fixture=Path(temporary); media=fixture/'media'; shutil.copytree(ROOT/'build/media',media)
        (media/'CONFIG.SYS').write_text('DEVICE=C:\\RAMDRV.SYS /SIZE:1440 /UNITS:2 /REMOVABLE\nSHELL=C:\\COMMAND.COM /P\n',encoding='ascii')
        image=fixture/'disk.img'; build(image,ROOT/'build/BOOTIA64.EFI',media)
        guest=Guest(image,ROOT/'build'/f'floppy-{machine}.log',machine)
        def run(command,steps,expected=None):
            guest.p.stdin.write(command.encode()+b'\r'); guest.p.stdin.flush(); out=''
            for wait,reply in steps:
                out+=guest.expect(wait.encode(),timeout=120); guest.p.stdin.write(reply); guest.p.stdin.flush()
            out+=guest.expect(guest.prompt,timeout=120)
            if expected: assert all(text in out for text in expected), out
            return out
        try:
            run('format a: /v:source',[('and press ENTER when ready...',b'\r'),('Format another (Y/N)?',b'N\r')],
                ['Insert new diskette for drive A:','1457664 bytes total disk space','512 bytes in each allocation unit'])
            guest.command('echo FLOPPY-DATA > a:\\file.txt'); guest.command('md a:\\sub'); guest.command('echo INNER > a:\\sub\\in.txt')
            out=run('diskcopy a: b:',[('Press any key to continue . . .',b'x'),('Press any key to continue . . .',b'x'),('Copy another diskette (Y/N)? ',b'N\r')],
                ['Insert SOURCE diskette in drive A:','Insert TARGET diskette in drive B:','Copying 80 tracks\n18 Sectors/Track, 2 Side(s)'])
            serial=out.split('Volume Serial Number is ')[1][:9]
            guest.command('type b:\\sub\\in.txt','INNER'); guest.command('vol b:',f'Volume Serial Number is {serial}')
            run('diskcomp a: b:',[('Press any key to continue . . .',b'x'),('Press any key to continue . . .',b'x'),('Compare another diskette (Y/N) ?',b'N\r')],
                ['Insert FIRST diskette in drive A:','Comparing 80 tracks','Compare OK'])
            guest.command('echo CHANGED > b:\\x.txt')
            run('diskcomp a: b:',[('Press any key to continue . . .',b'x'),('Press any key to continue . . .',b'x'),('Compare another diskette (Y/N) ?',b'N\r')],
                ['Compare error on\nside 0, track 0'])
            guest.command('if errorlevel 1 echo DISKS-DIFFER','DISKS-DIFFER')
            run('diskcopy a: a:',[('Insert SOURCE diskette in drive A:',b'x'),('Insert TARGET diskette in drive A:',b'x'),('Copy another diskette (Y/N)? ',b'N\r')],['Copying 80 tracks'])
            guest.command('diskcopy c: a:','or is non-removable',error=True)
            guest.command('diskcomp a: b: file','Invalid parameter',error=True)
            run('recover a:\\file.txt',[('file(s) on drive A:',b'x')],['12 of 12 bytes recovered'])
            guest.command('type a:\\file.txt','FLOPPY-DATA')
            run('recover b:',[('file(s) on drive B:',b'x')],['4 file(s) recovered'])
            out=guest.command('dir b:','FILE0001 REC'); assert 'Volume in drive B is SOURCE' in out and 'FILE     TXT' not in out, out
            out=guest.command('chkdsk b:','in 4 user files'); assert 'Errors found' not in out, out
            guest.command('type b:\\file0001.rec','FLOPPY-DATA')
        finally:
            guest.close()
        print(f'PASS floppies: {machine} removable FORMAT, DISKCOPY (two drives and one), DISKCOMP, RECOVER file and drive modes',flush=True)

def recover_bad(machine):
    from check_image import inspect, u16, u32
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='recover-test-') as temporary:
        image=Path(temporary)/'disk.img'; dstart,dcount=disk_fixture(image,False,False)
        part=image.read_bytes()[dstart*512:(dstart+dcount)*512]
        reserved=u16(part,14); spf=u16(part,22); fats=part[16]; spc=part[13]
        root=(reserved+fats*spf)*512; data=reserved+fats*spf+(u16(part,17)*32+511)//512
        entry=next(off for off in range(root,root+512*32,32) if part[off:off+11]==b'HELLO   EFI')
        size=u32(part,entry+28); first=u16(part,entry+26)
        fat=lambda c:(u16(part,reserved*512+c+c//2)>>(4 if c&1 else 0))&0xfff
        second=fat(first); unit=spc*512
        assert size>3*unit and 2<=second<0xff0
        original=Path(ROOT/'build/media/HELLO.EFI').read_bytes()
        config=Path(temporary)/'blkdebug.conf'
        config.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{dstart+data+(second-2)*spc+1}"\nerrno = "5"\nonce = "off"\nimmediately = "on"\n')
        guest=Guest(f'blkdebug:{config}:{image}',ROOT/'build'/f'recover-{machine}.log',machine)
        try:
            guest.p.stdin.write(b'recover d:\\hello.efi\r'); guest.p.stdin.flush()
            guest.expect(b'file(s) on drive D:'); guest.p.stdin.write(b'x'); guest.p.stdin.flush()
            out=guest.expect(guest.prompt,timeout=120)
            assert f'{size-unit} of {size} bytes recovered' in out, out
            out=guest.command('chkdsk d:',f'{unit} bytes in bad sectors'); assert 'Errors found' not in out, out
            guest.p.stdin.write(b'shutdown\r'); guest.p.stdin.flush(); guest.p.wait(timeout=15*SLOW)
            assert guest.p.returncode==0
        finally:
            guest.close()
        partition_image(image,dstart,dcount,Path(temporary)/'d-only.img')
        part=image.read_bytes()[dstart*512:(dstart+dcount)*512]
        assert fat(second)==0xff7 and u32(part,entry+28)==size-unit
        files=inspect(Path(temporary)/'d-only.img')
        assert files['HELLO.EFI']==original[:unit]+original[2*unit:]
        print(f'PASS recover: {machine} FAT12 unreadable cluster marked bad, chain relinked, readable bytes kept',flush=True)

# The tests: those that run on both file systems, those on FAT16 alone
# (Interface Manager and the rest), and those on itanium2-vpc's FAT16 alone.
# Each boots its own copies of the images, so any number run at once.
def boot(machine,fat12=False):
    prefix=ROOT/'build'/('qemu-'+machine+('-fat12' if fat12 else '-fat16'))
    image=Path(str(prefix)+'.img')
    if fat12:
        from mkimage import build
        build(image, ROOT/'build/BOOTIA64.EFI', ROOT/'build/media', fat12=True)
    else:
        shutil.copyfile(ROOT/'build/dos-ia64.img', image)
    run(image,prefix,machine)
BOTH=(boot,shell,faults,critical_errors,devices,ports,ramdisks,clocks,fcbs,nls,console,codepages,maintenance)
FAT16=(keyb,filecmds,drivemaps,installables,ansi,backups,fdisk,resident,floppies,tasks,dos16,graphics,xmsems,dpmi,
       spooler,exe2bin,pe_modules,windows,gdi,user,crt,packing,samples,samples16,samplesn16,progman,notepad,taskman,recorder,terminal,write,
       write_pictures,printman,winsetup,pifedit,winhelp,helpmenus,calc,clock,reversi,control,calendar,sol,pbrush,
       cardfile,winfile,dosprompt,dossessions,win16,cdrom,install,drives)
# Merced's LSI53C895A firmware path fails every later command after one
# injected read error (even after a Block I/O reset), so only AHCI runs
# recover_bad.
ITANIUM2=(recover_bad,modules)
MACHINES=('itanium-vpc','itanium2-vpc')

def tests(machine,fat12):
    """The tests for one machine and file system: (name, function, arguments)."""
    result=[(f.__name__,f,(machine,fat12)) for f in BOTH]
    if not fat12:
        result+=[(f.__name__,f,(machine,)) for f in FAT16]
        if machine=='itanium2-vpc': result+=[(f.__name__,f,(machine,)) for f in ITANIUM2]
    return result

def run_all(work,jobs):
    """Each test in a process of its own, jobs at a time, the longest (by the
    last run's times) first; each one's output when it ends, then the failures."""
    from concurrent.futures import ThreadPoolExecutor, as_completed
    times_file=ROOT/'build/qemu-test-times.json'
    try: times=json.loads(times_file.read_text())
    except (OSError,ValueError): times={}
    key=lambda w: f'{w[0]} {"fat12" if w[1] else "fat16"} {w[2]}'
    work=sorted(work,key=lambda w: -times.get(key(w),3600))
    def child(w):
        machine,fat12,name=w; started=time.monotonic()
        command=[sys.executable,__file__,'--machine',machine,'--only',name]+(['--fs','fat12'] if fat12 else [])
        p=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        try: output,_=p.communicate(timeout=3600)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid,9); output,_=p.communicate(); output+=b'\ntimed out after an hour\n'
        return p.returncode,output.decode('utf-8','replace'),time.monotonic()-started
    failed=[]; started=time.monotonic()
    with ThreadPoolExecutor(jobs) as pool:
        futures={pool.submit(child,w):w for w in work}
        for done,future in enumerate(as_completed(futures),1):
            w=futures[future]; code,output,seconds=future.result()
            times[key(w)]=round(seconds,1)
            sys.stdout.write(output)
            print(f'[{done}/{len(work)}] {"PASS" if code==0 else "FAIL"} {key(w)} ({seconds:.0f} s)',flush=True)
            if code: failed.append(key(w))
    times_file.write_text(json.dumps(times,indent=1,sort_keys=True))
    minutes,seconds=divmod(int(time.monotonic()-started),60)
    print(f'QEMU tests: {len(work)-len(failed)} passed, {len(failed)} failed in {minutes} min {seconds} s ({jobs} at a time)',flush=True)
    for f in failed: print(f'FAILED: {f}',flush=True)
    return 1 if failed else 0

if __name__=='__main__':
    parser=argparse.ArgumentParser(description='QEMU tests, run in parallel.')
    parser.add_argument('--machine',default='itanium2-vpc',help='itanium-vpc, itanium2-vpc, or all')
    parser.add_argument('--fs',choices=('fat16','fat12','both'),default='fat16',help='the boot disk\'s file system')
    parser.add_argument('--fat12',action='store_true',help='the same as --fs fat12')
    parser.add_argument('--only',help='these tests alone (names separated by commas)')
    parser.add_argument('--jobs',type=int,default=len(os.sched_getaffinity(0)),help='virtual machines at a time (default: the processors, as nproc counts them)')
    parser.add_argument('--list',action='store_true',help='list the tests')
    args=parser.parse_args()
    machines=MACHINES if args.machine=='all' else tuple(args.machine.split(','))
    systems=(False,True) if args.fs=='both' else (args.fat12 or args.fs=='fat12',)
    only=set(args.only.split(',')) if args.only else None
    work=[(m,f,name) for m in machines for f in systems for name,_,_ in tests(m,f) if not only or name in only]
    if only and only-{name for _,_,name in work}: parser.error(f'no such test: {", ".join(sorted(only-{n for _,_,n in work}))}')
    if args.list:
        for m,f,name in work: print(m,'fat12' if f else 'fat16',name)
        sys.exit(0)
    if len(work)==1:
        machine,fat12,name=work[0]
        _,function,arguments=next(t for t in tests(machine,fat12) if t[0]==name)
        function(*arguments)
        sys.exit(0)
    sys.exit(run_all(work,args.jobs))
