# SPDX-License-Identifier: GPL-2.0-or-later
# VGA16.COM: a 16-bit full-screen program for the VDM's VGA text mode. It
# finds the screen it was started from (the line above the cursor begins
# with the prompt C:\>), sets mode 3 and fills the screen at B800h directly
# and through INT 10h (characters with attributes, a string, a window
# scroll, the cursor), prints with DOS, reads the screen, the BIOS state and
# the CRT controller back, reports on the console and waits for a key,
# leaving the screen as it is for checking:
#   row 0   "VGA16" yellow on blue across the row
#   row 2   five X white on red from column 10
#   row 4   "STRING" white on green
#   row 6   "DOS text" through DOS
#   row 8   a double-line box top in light cyan
#   row 10  "A" black on light grey, scrolled up from row 11
#   row 12  blank magenta, brought in by the scroll
#   row 14  the result
	.code16
	.text
	.globl _start
_start:
	mov $0x03,%ah
	xor %bh,%bh
	int $0x10
	mov $'0',%cl			# the command line on the screen
	or %dh,%dh
	jz 1f
	dec %dh
	mov $160,%al
	mul %dh
	mov %ax,%di
	mov $0xb800,%ax
	mov %ax,%es
	cmpb $'C',%es:(%di)
	jne fail
	cmpb $':',%es:2(%di)
	jne fail
	cmpb $'\\',%es:4(%di)
	jne fail
	cmpb $'>',%es:6(%di)
	jne fail
1:	mov $0x0003,%ax
	int $0x10
	mov $0xb800,%ax
	mov %ax,%es
	cld
	xor %di,%di
	mov $0x1e20,%ax
	mov $80,%cx
	rep stosw
	xor %di,%di
	mov $title,%si
	mov $0x1e,%ah
1:	lodsb
	or %al,%al
	jz 2f
	stosw
	jmp 1b
2:	mov $8*160,%di
	mov $0x0bc9,%ax
	stosw
	mov $0x0bcd,%ax
	mov $8,%cx
	rep stosw
	mov $0x0bbb,%ax
	stosw
	movw $0x7041,%es:11*160
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x020a,%dx
	int $0x10
	mov $0x0958,%ax
	mov $0x004f,%bx
	mov $5,%cx
	int $0x10
	push %ds
	pop %es
	mov $0x1301,%ax
	mov $0x002f,%bx
	mov $string_end-string,%cx
	mov $0x0400,%dx
	mov $string,%bp
	int $0x10
	mov $'1',%cl			# the string moved the cursor
	call cursor
	cmp $0x0406,%dx
	jne fail
	mov $0x0601,%ax
	mov $0x5000,%bx
	mov $0x0a00,%cx
	mov $0x0c4f,%dx
	int $0x10
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x0600,%dx
	int $0x10
	mov $dos_text,%dx
	mov $9,%ah
	int $0x21
	mov $'2',%cl			# DOS text moved it too
	call cursor
	cmp $0x0608,%dx
	jne fail
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x020a,%dx
	int $0x10
	mov $0x08,%ah
	xor %bh,%bh
	int $0x10
	mov $'3',%cl			# a character and attribute read back
	cmp $0x4f58,%ax
	jne fail
	mov $0x0f,%ah
	int $0x10
	mov $'4',%cl			# mode 3, 80 columns
	cmp $0x5003,%ax
	jne fail
	mov $0xb800,%ax
	mov %ax,%es
	mov $'5',%cl			# the screen memory
	cmpw $0x1e56,%es:0
	jne fail
	cmpw $0x7041,%es:10*160
	jne fail
	cmpw $0x5020,%es:12*160+158
	jne fail
	mov $'6',%cl			# the CRT controller's cursor start line
	mov $0x3d4,%dx
	mov $0x0a,%al
	out %al,%dx
	inc %dx
	in %dx,%al
	and $0x3f,%al
	cmp $0x0d,%al
	jne fail
	mov $ok,%dx
	jmp report
fail:
	mov %cl,which
	mov $failed,%dx
report:
	push %dx
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x0e00,%dx
	int $0x10
	pop %dx
	mov $9,%ah
	int $0x21
	xor %ah,%ah
	int $0x16
	mov $0x4c00,%ax
	int $0x21
cursor:
	mov $0x03,%ah
	xor %bh,%bh
	int $0x10
	ret
title:	.asciz "VGA16"
string:	.ascii "STRING"
string_end:
dos_text: .ascii "DOS text$"
ok:	.ascii "VGA16: ok\r\n$"
failed:	.ascii "VGA16: FAIL "
which:	.ascii "?\r\n$"
