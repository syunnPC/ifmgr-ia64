# SPDX-License-Identifier: GPL-2.0-or-later
# DPMI16.COM: an 8086 program that switches to 16-bit protected mode
# through the VDM's DPMI host and checks INT 31h's services. Each test
# prints "DPMI16: <name> ok" or "FAILED" (in protected mode through
# INT 21h AH=02h, which the host reflects to real mode); the exit code is
# the number of failures.
	.code16
	.text
	.globl _start
_start:
	mov %cs,rm_seg
	mov $0x4a,%ah		# keep 64K, the rest for the host's data
	mov $0x1000,%bx
	int $0x21
	mov $0x1687,%ax
	int $0x2f
	or %ax,%ax
	jnz no_dpmi
	mov %di,entry
	mov %es,entry+2
	mov %si,host_paras
	mov $0x48,%ah
	mov host_paras,%bx
	int $0x21
	jc no_dpmi
	mov %ax,%es
	xor %ax,%ax		# a 16-bit program
	lcall *entry
	jc no_dpmi
	# Protected mode from here.
	mov %cs,pm_cs
	mov %ds,pm_ds
	mov %ds,pm_ds_copy
	mov $tests,%si
next:	cmp $tests_end,%si
	je done
	push %si
	call *2(%si)
	pop %si
	pushf
	mov $prefix,%bx
	call puts
	mov (%si),%bx
	call puts
	popf
	mov $ok,%bx
	jnc 1f
	mov $failed,%bx
	incb failures
1:	call puts
	add $4,%si
	jmp next
done:	mov failures,%al
	mov $0x4c,%ah
	int $0x21
no_dpmi:
	mov $nodpmi,%dx
	mov $9,%ah
	int $0x21
	mov $0x4cff,%ax
	int $0x21

# A $-terminated string at DS:BX, a character at a time through INT 21h AH=02h.
puts:	push %ax
	push %dx
1:	mov (%bx),%dl
	cmp $'$',%dl
	je 2f
	mov $2,%ah
	int $0x21
	inc %bx
	jmp 1b
2:	pop %dx
	pop %ax
	ret
pass:	clc
	ret
fail:	mov pm_ds,%ax
	mov %ax,%ds
	mov %ax,%es
	stc
	ret

t_switch:
	mov %cs,%ax
	and $7,%ax		# an LDT selector at privilege 3
	cmp $7,%ax
	jne fail
	mov $0x0400,%ax
	int $0x31
	jc fail
	cmp $0x005a,%ax
	jne fail
	mov $0x0006,%ax		# DS is the program's segment
	mov %ds,%bx
	int $0x31
	jc fail
	mov rm_seg,%ax
	mov %ax,%bx
	shl $4,%ax
	shr $12,%bx
	cmp %ax,%dx
	jne fail
	cmp %bx,%cx
	jne fail
	jmp pass
t_descriptor:
	mov $0x0000,%ax		# a descriptor for B800h's text page
	mov $1,%cx
	int $0x31
	jc fail
	mov %ax,sel
	mov $0x0007,%ax
	mov sel,%bx
	mov $0x000b,%cx
	mov $0x8000,%dx
	int $0x31
	jc fail
	mov $0x0008,%ax
	mov sel,%bx
	xor %cx,%cx
	mov $0xffff,%dx
	int $0x31
	jc fail
	mov $0x0002,%ax		# the same as 0002h's for B800h, twice
	mov $0xb800,%bx
	int $0x31
	jc fail
	mov %ax,%dx
	mov $0x0002,%ax
	mov $0xb800,%bx
	int $0x31
	jc fail
	cmp %ax,%dx
	jne fail
	mov $0x0006,%ax
	mov %dx,%bx
	int $0x31
	cmp $0x000b,%cx
	jne fail
	cmp $0x8000,%dx
	jne fail
	mov $0x0001,%ax
	mov sel,%bx
	int $0x31
	jc fail
	mov $0x0001,%ax		# not twice
	mov sel,%bx
	int $0x31
	jnc fail
	jmp pass
t_memory:
	mov $0x0501,%ax		# 64K of memory
	mov $1,%bx
	xor %cx,%cx
	int $0x31
	jc fail
	mov %si,handle
	mov %di,handle+2
	mov %bx,linear+2
	mov %cx,linear
	call map_block
	jc fail
	mov sel,%es
	movw $0x1234,%es:0
	movw $0x5678,%es:0xfffe
	push %ds
	pop %es
	mov $0x0503,%ax		# grown to 128K, the data kept
	mov $2,%bx
	xor %cx,%cx
	mov handle,%si
	mov handle+2,%di
	int $0x31
	jc fail
	mov %si,handle
	mov %di,handle+2
	mov %bx,linear+2
	mov %cx,linear
	call map_block
	jc fail
	mov sel,%es
	cmpw $0x1234,%es:0
	jne fail
	cmpw $0x5678,%es:0xfffe
	jne fail
	push %ds
	pop %es
	mov $0x0001,%ax
	mov sel,%bx
	int $0x31
	mov $0x0502,%ax
	mov handle,%si
	mov handle+2,%di
	int $0x31
	jc fail
	mov $0x0502,%ax		# not twice
	mov handle,%si
	mov handle+2,%di
	int $0x31
	jnc fail
	jmp pass
# A new selector for the block at linear, 64K.
map_block:
	mov $0x0000,%ax
	mov $1,%cx
	int $0x31
	jc 1f
	mov %ax,sel
	mov $0x0007,%ax
	mov sel,%bx
	mov linear+2,%cx
	mov linear,%dx
	int $0x31
	jc 1f
	mov $0x0008,%ax
	mov sel,%bx
	xor %cx,%cx
	mov $0xffff,%dx
	int $0x31
1:	ret
t_dos_memory:
	mov $0x0100,%ax
	mov $0x100,%bx
	int $0x31
	jc fail
	mov %ax,dos_seg
	mov %dx,dos_sel
	mov $0x0006,%ax
	mov dos_sel,%bx
	int $0x31
	mov dos_seg,%ax
	mov %ax,%bx
	shl $4,%ax
	shr $12,%bx
	cmp %ax,%dx
	jne fail
	cmp %bx,%cx
	jne fail
	mov dos_sel,%es
	movw $0x4321,%es:0
	push %ds
	pop %es
	mov $0x0101,%ax
	mov dos_sel,%dx
	int $0x31
	jc fail
	# 96K: two selectors 64K apart; shrunk to 32K, the second is freed.
	mov $0x0100,%ax
	mov $0x1800,%bx
	int $0x31
	jc fail
	mov %ax,dos_seg
	mov %dx,dos_sel
	mov $0x0006,%ax
	mov dos_sel,%bx
	add $8,%bx
	int $0x31
	jc fail
	mov dos_seg,%ax
	mov %ax,%bx
	shl $4,%ax
	shr $12,%bx
	inc %bx			# 10000h above
	cmp %ax,%dx
	jne fail
	cmp %bx,%cx
	jne fail
	mov $0x0102,%ax
	mov $0x0800,%bx
	mov dos_sel,%dx
	int $0x31
	jc fail
	mov $0x0006,%ax
	mov dos_sel,%bx
	add $8,%bx
	int $0x31
	jnc fail
	mov $0x0101,%ax
	mov dos_sel,%dx
	int $0x31
	jc fail
	jmp pass
t_interrupts:
	mov $0x0204,%ax		# INT 60h's handler, then ours
	mov $0x60,%bl
	int $0x31
	jc fail
	mov %cx,old60+2
	mov %dx,old60
	mov $0x0205,%ax
	mov $0x60,%bl
	mov %cs,%cx
	mov $handler60,%dx
	int $0x31
	jc fail
	movb $0,hit
	mov $0x1111,%ax
	int $0x60
	cmpb $1,hit
	jne fail
	cmp $0x2222,%ax
	jne fail
	mov $0x0205,%ax
	mov $0x60,%bl
	mov old60+2,%cx
	mov old60,%dx
	int $0x31
	# INT 21h AH=30h, reflected to real mode with its registers.
	mov $0x3000,%ax
	int $0x21
	cmp $4,%al
	jne fail
	jmp pass
handler60:
	movb $1,hit		# DS is still the program's
	mov $0x2222,%ax
	iret
t_translate:
	# AH=09h in real mode through 0300h, the string in the program's segment.
	mov $regs,%di
	push %ds
	pop %es
	mov $0x32/2,%cx
	xor %ax,%ax
	cld
	rep stosw
	movw $0x0900,regs+0x1c
	movw $line,regs+0x14
	mov rm_seg,%ax
	mov %ax,regs+0x24
	mov $0x0300,%ax
	mov $0x21,%bl
	xor %bh,%bh
	xor %cx,%cx
	mov $regs,%di
	int $0x31
	jc fail
	cmpb $'$',regs+0x1c	# AL comes back as AH=09h leaves it: "$"
	jne fail
	jmp pass
t_virtual_if:
	mov $0x0900,%ax
	int $0x31
	mov %al,was_if
	mov $0x0902,%ax
	int $0x31
	or %al,%al
	jnz fail
	mov $0x0901,%ax
	int $0x31
	mov $0x0902,%ax
	int $0x31
	cmp $1,%al
	jne fail
	jmp pass
t_exception:
	mov $0x0202,%ax		# a divide error handler that steps over the DIV
	mov $0,%bl
	int $0x31
	jc fail
	mov %cx,old0+2
	mov %dx,old0
	mov $0x0203,%ax
	mov $0,%bl
	mov %cs,%cx
	mov $handler0,%dx
	int $0x31
	jc fail
	movb $0,hit
	xor %dx,%dx
	mov $100,%ax
	xor %cx,%cx
divide:	div %cx
	cmpb $1,hit
	jne fail
	mov $0x0203,%ax
	mov $0,%bl
	mov old0+2,%cx
	mov old0,%dx
	int $0x31
	jmp pass
# A protected-mode INT 8 handler counts ticks that come while the program's
# real-mode code (0301h) waits for three of them.
t_timer:
	mov $0x0204,%ax
	mov $8,%bl
	int $0x31
	jc fail
	mov %cx,old8+2
	mov %dx,old8
	mov $0x0205,%ax
	mov $8,%bl
	mov %cs,%cx
	mov $handler8,%dx
	int $0x31
	jc fail
	movw $0,ticks
	mov $regs,%di		# far call to wait_ticks in real mode
	push %ds
	pop %es
	mov $0x32/2,%cx
	xor %ax,%ax
	cld
	rep stosw
	movw $wait_ticks,regs+0x2a
	mov rm_seg,%ax
	mov %ax,regs+0x2c
	mov %ax,regs+0x24
	movw $0x0200,regs+0x20	# interrupts on
	mov $0x0301,%ax
	xor %bx,%bx
	xor %cx,%cx
	mov $regs,%di
	int $0x31
	pushf
	mov $0x0205,%ax
	mov $8,%bl
	mov old8+2,%cx
	mov old8,%dx
	int $0x31
	popf
	jc fail
	cmpw $3,ticks
	jb fail
	jmp pass
handler8:
	push %ds
	push %ax
	mov %cs:pm_ds_copy,%ds
	incw ticks
	mov $0x20,%al		# end of interrupt
	out %al,$0x20
	pop %ax
	pop %ds
	iret
	.code16
wait_ticks:			# real mode, DS the program's segment
	mov $0x8000,%cx
1:	cmpw $3,ticks
	jae 2f
	push %cx
	mov $0x2c00,%ax		# a DOS call, then a while in a loop
	int $0x21
	pop %cx
	mov $2000,%dx
3:	dec %dx
	jnz 3b
	loop 1b
2:	lret
# The frame: return IP and CS, error code, IP, CS, flags, SP, SS.
handler0:
	push %bp
	mov %sp,%bp
	addw $2,%ss:8(%bp)	# DIV CX is two bytes
	movb $1,hit		# DS is still the program's
	pop %bp
	lret

	.data
prefix:	.ascii "DPMI16: $"
ok:	.ascii " ok\r\n$"
failed:	.ascii " FAILED\r\n$"
nodpmi:	.ascii "DPMI16: no DPMI host\r\n$"
line:	.ascii "DPMI16: a line from real mode\r\n$"
failures: .byte 0
hit:	.byte 0
was_if:	.byte 0
entry:	.word 0,0
host_paras: .word 0
rm_seg:	.word 0
pm_cs:	.word 0
pm_ds:	.word 0
pm_ds_copy: .word 0	# read through CS by handler8
sel:	.word 0
handle:	.word 0,0
linear:	.word 0,0
dos_seg: .word 0
dos_sel: .word 0
old60:	.word 0,0
old0:	.word 0,0
old8:	.word 0,0
ticks:	.word 0
regs:	.space 0x32
n_switch: .ascii "switch and version$"
n_desc:	.ascii "descriptors$"
n_mem:	.ascii "memory blocks$"
n_dos:	.ascii "DOS memory$"
n_int:	.ascii "interrupts$"
n_tr:	.ascii "real-mode call$"
n_vif:	.ascii "virtual interrupts$"
n_exc:	.ascii "exception handler$"
n_timer: .ascii "timer in real mode$"
tests:	.word n_switch,t_switch,n_desc,t_descriptor,n_mem,t_memory,n_dos,t_dos_memory
	.word n_int,t_interrupts,n_tr,t_translate,n_vif,t_virtual_if,n_exc,t_exception,n_timer,t_timer
tests_end:
