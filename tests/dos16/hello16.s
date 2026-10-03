# SPDX-License-Identifier: GPL-2.0-or-later
# HELLO16.COM: an 8086 DOS program for the VDM tests. Prints the DOS
# version and its command tail, then exits with code 7; with the tail
# " /LOOP" it then loops without a call until Ctrl-C ends it, with " /KEY"
# it first waits for a key (INT 16h).
	.code16
	.text
	.globl _start
_start:
	mov $0x30,%ah
	int $0x21
	mov %ax,%bx
	mov $msg,%dx
	mov $9,%ah
	int $0x21
	mov %bl,%al
	call digit
	mov $'.',%dl
	mov $2,%ah
	int $0x21
	mov %bh,%al
	aam
	xchg %ah,%al
	push %ax
	call digit
	pop %ax
	mov %ah,%al
	call digit
	mov $tail,%dx
	mov $9,%ah
	int $0x21
	mov $0x80,%si
	mov (%si),%cl
	xor %ch,%ch
	inc %si
1:	jcxz 2f
	mov (%si),%dl
	mov $2,%ah
	int $0x21
	inc %si
	dec %cx
	jmp 1b
2:	mov $close,%dx
	mov $9,%ah
	int $0x21
	cmpw $0x2f20,0x81		# " /LOOP" or " /KEY"
	jne 3f
	cmpw $0x4f4c,0x83
	jne 4f
	jmp .
4:	cmpw $0x454b,0x83
	jne 3f
	xor %ah,%ah
	int $0x16
3:	mov $0x4c07,%ax
	int $0x21
digit:
	mov %al,%dl
	add $'0',%dl
	mov $2,%ah
	int $0x21
	ret
msg:	.ascii "HELLO16: DOS $"
tail:	.ascii ", tail [$"
close:	.ascii "]\r\n$"
