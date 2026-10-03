# SPDX-License-Identifier: GPL-2.0-or-later
# W16TEST.EXE: a Windows 3.0 program (NE format) for the Win16 tests. It
# starts as Windows programs do (InitTask, WaitEvent, InitApp), reports
# through OutputDebugString, formats with wsprintf (C calling convention),
# shows a message box and ends with INT 21h AH=4Ch. From DOS, its MZ stub
# says that it needs Windows.
	.code16

	.section .mz,"a"
	.ascii "MZ"
	.word _mz_last,_mz_pages,0,4,0,0xffff,0,0x100,0,stub-0x40,0,0x40,0
	.fill 0x3c-0x1c,1,0
	.long _ne_file
stub:	push %cs
	pop %ds
	mov $(stub_msg-0x40),%dx
	mov $9,%ah
	int $0x21
	mov $0x4c01,%ax
	int $0x21
stub_msg: .ascii "This program requires Interface Manager.\r\n$"
	.balign 16

	.section .ne,"a"
	.ascii "NE"
	.byte 5,10
	.word entries,entries_end-entries
	.long 0
	.word 0x0302			# instance data, Windows application
	.word 2				# automatic data segment
	.word 1024,4096			# local heap, stack
	.word start,1			# CS:IP
	.word 0,2			# SS:SP (top of DGROUP)
	.word 2,2			# segments, module references
	.word _nonres_size
	.word segments,resident,resident,modules,imported
	.long _nonres_file
	.word 0,4,0			# movable entries, alignment shift, resource segments
	.byte 2,0			# Windows
	.word 0,0,0
	.word 0x0300
segments:
	.word _code_sector,_code_size,0x0140,_code_size	# code: relocations, preload
	.word _data_sector,_data_size,0x0041,_data_size	# data, preload
resident:
	.byte 7
	.ascii "W16TEST"
	.word 0
	.byte 0
modules: .word kernel-imported,user-imported
imported: .byte 0
kernel:	.byte 6
	.ascii "KERNEL"
user:	.byte 4
	.ascii "USER"
entries: .byte 0
entries_end:

	.section .code,"ax"
	.globl start
start:
inittask: .byte 0x9a
	.word 0xffff,0			# KERNEL.91 InitTask
	or %ax,%ax
	jz fail
	mov %di,hinstance
	push $0
waitevent: .byte 0x9a
	.word 0xffff,0			# KERNEL.30 WaitEvent
	push hinstance
initapp: .byte 0x9a
	.word 0xffff,0			# USER.5 InitApp
	or %ax,%ax
	jz fail
	push %ds
	push $started
trace1: .byte 0x9a
	.word 0xffff,0			# KERNEL.115 OutputDebugString
getversion: .byte 0x9a
	.word 0xffff,0			# KERNEL.3 GetVersion
	mov %ax,version
	push $0
	push %ds
	push $text
	push %ds
	push $caption
	push $0x40			# MB_ICONINFORMATION
msgbox: .byte 0x9a
	.word 0xffff,0			# USER.1 MessageBox
	push %ax
	push version
	push %ds
	push $format
	push %ds
	push $buffer
wsprintf: .byte 0x9a
	.word 0xffff,0			# USER.420 _wsprintf (cdecl)
	add $12,%sp
	push %ds
	push $buffer
trace2: .byte 0x9a
	.word 0xffff,0			# KERNEL.115 OutputDebugString
	mov $0x4c00,%ax
	int $0x21
fail:	mov $0x4c01,%ax
	int $0x21

	.section .coderel,"a"
	.word 8
	.byte 3,1
	.word inittask+1,1,91
	.byte 3,1
	.word waitevent+1,1,30
	.byte 3,1
	.word initapp+1,2,5
	.byte 3,1
	.word trace1+1,1,115
	.byte 3,1
	.word getversion+1,1,3
	.byte 3,1
	.word msgbox+1,2,1
	.byte 3,1
	.word wsprintf+1,2,420
	.byte 3,1
	.word trace2+1,1,115

	.section .data,"aw"
	.fill 16,1,0			# instance data
hinstance: .word 0
version: .word 0
started: .asciz "W16TEST: started"
caption: .asciz "W16TEST"
text:	.asciz "A 16-bit (NE) program on IA-64"
format:	.asciz "W16TEST: version %04X, message box %d"
buffer:	.fill 80,1,0

	.section .nonres,"a"
	.byte 22
	.ascii "Win16 test application"
	.word 0
	.byte 0
