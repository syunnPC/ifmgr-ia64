# SPDX-License-Identifier: GPL-2.0-or-later
# TINY16.COM: an 8086 program of a few bytes (DOS runs .COM files of any
# size): a line through INT 21h AH=09h, then RET to the INT 20h at PSP:0.
	.code16
	.text
	.globl _start
_start:
	mov $line,%dx
	mov $9,%ah
	int $0x21
	ret
line:	.ascii "TINY16: ok\r\n$"
