# SPDX-License-Identifier: GPL-2.0-or-later
# PRINT16.COM: an 8086 program using PRINT's INT 2Fh (AH=01h) with the
# resident part installed: it holds the queue (0104h), queues QA.PRN and
# QB.PRN (0101h), finds both in the queue, cancels QB.PRN (0102h), finds
# QA.PRN alone, reads the list device's header (0106h) and releases the
# queue (0105h), so that QA.PRN alone is printed afterwards. Each check
# prints "PRINT16: <name> ok" or "FAILED"; the exit code is the failures.
	.code16
	.text
	.globl _start
_start:
	mov $0x4a,%ah		# keep 64 KiB, so that AH=48h has memory to give
	mov $0x1000,%bx
	int $0x21
	mov %cs,seg_a
	mov %cs,seg_b
	mov %cs,seg_c
	mov $tests,%si
next:	cmp $tests_end,%si
	je done
	push %si
	call *2(%si)
	push %cs		# 0104h and 0106h leave DS at PRINT's queue
	pop %ds
	pop %si
	pushf
	mov $prefix,%dx
	mov $9,%ah
	int $0x21
	mov (%si),%dx
	int $0x21
	popf
	mov $ok,%dx
	jnc 1f
	mov $failed,%dx
	incb failures
1:	mov $9,%ah
	int $0x21
	add $4,%si
	jmp next
done:	mov $0x0105,%ax		# never leave the queue held
	int $0x2f
	mov failures,%al
	mov $0x4c,%ah
	int $0x21

pass:	clc
	ret
fail:	push %cs
	pop %ds
	push %cs
	pop %es
	stc
	ret

t_installed:
	mov $0x0100,%ax
	int $0x2f
	cmp $0xff,%al
	jne fail
	jmp pass
t_queue:
	mov $0x0104,%ax		# held from here
	int $0x2f
	jc fail
	push %cs
	pop %ds
	mov $packet_a,%dx
	mov $0x0101,%ax
	int $0x2f
	jc fail
	mov $packet_b,%dx
	mov $0x0101,%ax
	int $0x2f
	jc fail
	mov $0x0104,%ax
	int $0x2f
	jc fail
	mov $name_a,%di		# DS:SI the queue: QA.PRN, QB.PRN, then an empty entry
	call entry
	jne fail
	add $64,%si
	mov $name_b,%di
	call entry
	jne fail
	cmpb $0,64(%si)
	jne fail
	jmp pass
t_cancel:
	push %cs
	pop %ds
	mov $name_b,%dx
	mov $0x0102,%ax
	int $0x2f
	jc fail
	mov $0x0102,%ax		# not twice
	int $0x2f
	jnc fail
	mov $0x0104,%ax
	int $0x2f
	jc fail
	mov $name_a,%di
	call entry
	jne fail
	cmpb $0,64(%si)
	jne fail
	jmp pass
t_missing:			# a file that does not open is not queued
	push %cs
	pop %ds
	mov $packet_c,%dx
	mov $0x0101,%ax
	int $0x2f
	jnc fail
	cmp $2,%ax
	jne fail
	jmp pass
t_device:
	mov $0x0106,%ax		# files queued: CF, DS:SI the device's header
	int $0x2f
	jnc fail
	cmpw $0xffff,(%si)
	jne fail
	cmpl $0x3154504c,10(%si) # "LPT1"
	jne fail
	push %cs		# memory blocks still whole after it
	pop %ds
	mov $0x48,%ah
	mov $0x10,%bx
	int $0x21
	jc fail
	mov %ax,%es
	mov $0x49,%ah
	int $0x21
	jc fail
	push %cs
	pop %es
	jmp pass

# Whether the queue entry at DS:SI ends with the name at CS:DI: ZF.
entry:	push %si
	push %di
	push %cx
	push %bx
	mov %si,%bx
1:	cmpb $0,(%bx)
	je 2f
	inc %bx
	jmp 1b
2:	sub %si,%bx		# the entry's length
	mov %di,%cx
3:	cmpb $0,%cs:(%di)
	je 4f
	inc %di
	jmp 3b
4:	sub %cx,%di
	xchg %di,%cx		# CX the name's length, DI the name
	cmp %cx,%bx
	jb 6f
	add %bx,%si
	sub %cx,%si
5:	mov (%si),%al
	cmp %cs:(%di),%al
	jne 6f
	inc %si
	inc %di
	loop 5b
	xor %al,%al
	jmp 7f
6:	or $1,%al
7:	pop %bx
	pop %cx
	pop %di
	pop %si
	ret

	.data
prefix:	.ascii "PRINT16: $"
ok:	.ascii " ok\r\n$"
failed:	.ascii " FAILED\r\n$"
failures: .byte 0
name_a:	.asciz "QA.PRN"
name_b:	.asciz "QB.PRN"
packet_a: .byte 0
	.word name_a
seg_a:	.word 0
packet_b: .byte 0
	.word name_b
seg_b:	.word 0
name_c:	.asciz "NOSUCH.PRN"
packet_c: .byte 0
	.word name_c
seg_c:	.word 0
n_inst:	.ascii "installed$"
n_queue: .ascii "queue held$"
n_cancel: .ascii "cancel$"
n_device: .ascii "list device$"
n_missing: .ascii "missing file$"
tests:	.word n_inst,t_installed,n_queue,t_queue,n_cancel,t_cancel,n_missing,t_missing,n_device,t_device
tests_end:
