# SPDX-License-Identifier: GPL-2.0-or-later
# MEM16.COM: an 8086 program that checks the VDM's XMS and EMS. Each test
# prints "MEM16: <name> ok" or "FAILED"; with HIMEM.SYS and EMM386.SYS
# loaded the drivers' services are checked, without them ("MEM16 NONE")
# that they are absent. The exit code is the number of failures.
	.code16
	.text
	.globl _start
_start:
	mov $tests,%si
	movw $tests_end,end_ptr
	mov 0x82,%al		# "MEM16 NONE": the tests of absence
	and $0xdf,%al
	cmp $'N',%al
	jne next
	mov $none_tests,%si
	movw $none_end,end_ptr
next:	cmp end_ptr,%si
	je done
	push %si
	call *2(%si)
	pop %si
	pushf
	mov $prefix,%dx
	mov $9,%ah
	int $0x21
	mov (%si),%dx
	mov $9,%ah
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
done:	mov failures,%al
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
xms:	lcall *xms_entry
	ret

# --- XMS -----------------------------------------------------------------
t_xms_found:
	mov $0x4300,%ax
	int $0x2f
	cmp $0x80,%al
	jne fail
	mov $0x4310,%ax
	int $0x2f
	mov %bx,xms_entry
	mov %es,xms_entry+2
	push %cs
	pop %es
	les xms_entry,%di	# the entry: a short jump over three NOPs
	cmpw $0x03eb,%es:(%di)
	jne fail
	cmpb $0x90,%es:4(%di)
	jne fail
	push %cs
	pop %es
	mov $0,%ah
	call xms
	cmp $0x0300,%ax
	jne fail
	cmp $1,%dx		# the HMA, there or not
	ja fail
	mov %dl,have_hma
	jmp pass
t_xms_alloc:
	mov $8,%ah
	call xms
	cmp $256,%ax		# at least 256K largest
	jb fail
	mov $9,%ah
	mov $64,%dx
	call xms
	cmp $1,%ax
	jne fail
	mov %dx,handle
	# 16 bytes there and back, through the block.
	movw $16,move_len
	movw $0,move_len+2
	movw $0,move_src
	mov $text,%ax
	mov %ax,move_soff
	mov %cs,move_soff+2
	mov handle,%ax
	mov %ax,move_dst
	movl $100,move_doff
	mov $8,%ah
	call move
	jc fail
	mov handle,%ax
	mov %ax,move_src
	movl $100,move_soff
	movw $0,move_dst
	mov $copy,%ax
	mov %ax,move_doff
	mov %cs,move_doff+2
	mov $8,%ah
	call move
	jc fail
	mov $text,%si
	mov $copy,%di
	mov $16,%cx
	cld
	repe cmpsb
	jne fail
	# An odd length is refused.
	movw $15,move_len
	call move
	jnc fail
	cmp $0xa7,%bl
	jne fail
	jmp pass
move:	mov $move_struct,%si
	mov $0x0b,%ah
	call xms
	cmp $1,%ax
	je 1f
	stc
	ret
1:	clc
	ret
t_xms_lock:
	mov $0x0c,%ah
	mov handle,%dx
	call xms
	cmp $1,%ax
	jne fail
	or %bx,%dx		# a 32-bit address
	jz fail
	mov $0x0f,%ah		# a locked block cannot grow
	mov $128,%bx
	mov handle,%dx
	call xms
	cmp $0xab,%bl
	jne fail
	mov $0x0d,%ah
	mov handle,%dx
	call xms
	cmp $1,%ax
	jne fail
	mov $0x0f,%ah
	mov $128,%bx
	mov handle,%dx
	call xms
	cmp $1,%ax
	jne fail
	mov $0x0e,%ah
	mov handle,%dx
	call xms
	cmp $128,%dx
	jne fail
	# The 16 bytes moved with the block.
	mov handle,%ax
	mov %ax,move_src
	movl $100,move_soff
	movw $0,move_dst
	mov $copy2,%ax
	mov %ax,move_doff
	mov %cs,move_doff+2
	movw $16,move_len
	call move
	jc fail
	mov $text,%si
	mov $copy2,%di
	mov $16,%cx
	repe cmpsb
	jne fail
	mov $0x0a,%ah
	mov handle,%dx
	call xms
	cmp $1,%ax
	jne fail
	mov $0x0a,%ah
	mov handle,%dx
	call xms
	cmp $0xa2,%bl
	jne fail
	jmp pass
t_xms_hma:
	cmpb $0,have_hma
	jne 1f
	mov $1,%ah		# no HMA: 90h
	mov $0xffff,%dx
	call xms
	cmp $0x90,%bl
	jne fail
	or %ax,%ax
	jnz fail
	jmp a20
1:	mov $1,%ah
	mov $0xffff,%dx
	call xms
	cmp $1,%ax
	jne fail
	mov $1,%ah
	mov $0xffff,%dx
	call xms
	cmp $0x91,%bl
	jne fail
	mov $0xffff,%ax		# FFFF:0010 is 100000h, past 1 MiB: A20 is on
	mov %ax,%es
	movw $0x1234,%es:0x10
	xor %ax,%ax
	mov %ax,%es
	cmpw $0x1234,%es:0
	je fail
	mov $0xffff,%ax
	mov %ax,%es
	cmpw $0x1234,%es:0x10
	jne fail
	push %cs
	pop %es
	call a20
	jc fail
	mov $2,%ah
	call xms
	cmp $1,%ax
	jne fail
	mov $2,%ah
	call xms
	cmp $0x93,%bl
	jne fail
	jmp pass
a20:	mov $7,%ah
	call xms
	cmp $1,%ax
	jne fail
	mov $4,%ah		# A20 stays on
	call xms
	cmp $0x94,%bl
	jne fail
	jmp pass
t_xms_386:
	mov $0x88,%ah
	call xms
	cmp $0,%bl
	jne fail
	cmp $256,%eax
	jb fail
	mov $0x89,%ah
	mov $300,%edx
	call xms
	cmp $1,%ax
	jne fail
	mov %dx,handle
	mov $0x8e,%ah
	call xms
	cmp $300,%edx
	jne fail
	mov $0x0a,%ah
	mov handle,%dx
	call xms
	cmp $1,%ax
	jne fail
	jmp pass

# --- EMS -----------------------------------------------------------------
ems_name: .ascii "EMMXXXX0"
t_ems_found:
	xor %ax,%ax
	mov %ax,%es
	mov %es:0x19e,%es	# INT 67h's segment
	mov $0x0a,%di
	mov $ems_name,%si
	mov $8,%cx
	cld
	repe cmpsb
	jne fail
	push %cs
	pop %es
	mov $0x40,%ah
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x46,%ah
	int $0x67
	cmp $0x0040,%ax
	jne fail
	mov $0x41,%ah
	int $0x67
	or %ah,%ah
	jnz fail
	cmp $0x9000,%bx
	jne fail
	mov $0x42,%ah
	int $0x67
	cmp $64,%dx		# 1024K: 64 pages
	jne fail
	cmp $64,%bx
	jne fail
	jmp pass
# The frame is the top 64K of conventional memory: 576K, the arena below it.
t_ems_frame:
	int $0x12
	cmp $576,%ax
	jne fail
	mov $0x4a,%ah		# the program's block down to 64K, then the rest
	mov $0x1000,%bx
	int $0x21
	jc fail
	mov $0x48,%ah
	mov $0xffff,%bx
	int $0x21
	jnc fail
	mov %cs,%ax		# the free block after the program ends at 9000h
	add $0x1001,%ax
	add %bx,%ax
	cmp $0x9000,%ax
	jne fail
	jmp pass
t_ems_map:
	mov $0x43,%ah
	mov $4,%bx
	int $0x67
	or %ah,%ah
	jnz fail
	mov %dx,ems_handle
	mov $0x4b,%ah		# handle 0 and this one
	int $0x67
	cmp $2,%bx
	jne fail
	# Logical page 0 in physical page 0, written; page 1 there; 0 again.
	mov $0x4400,%ax
	xor %bx,%bx
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x9000,%ax
	mov %ax,%es
	movw $0xbeef,%es:0
	mov $0x4400,%ax
	mov $1,%bx
	mov ems_handle,%dx
	int $0x67
	cmpw $0xbeef,%es:0
	je fail
	mov $0x4401,%ax		# page 0 in physical page 1, at 9400h
	xor %bx,%bx
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x9400,%ax
	mov %ax,%es
	cmpw $0xbeef,%es:0
	jne fail
	push %cs
	pop %es
	mov $0x4400,%ax		# a logical page out of range
	mov $9,%bx
	mov ems_handle,%dx
	int $0x67
	cmp $0x8a,%ah
	jne fail
	mov $0x4404,%ax		# a physical page out of range
	xor %bx,%bx
	mov ems_handle,%dx
	int $0x67
	cmp $0x8b,%ah
	jne fail
	mov $0x43,%ah		# no pages
	xor %bx,%bx
	int $0x67
	cmp $0x89,%ah
	jne fail
	jmp pass
t_ems_move:
	# 16 bytes into logical page 2 at 100 and back (57h).
	movl $16,ems_len
	movb $0,ems_stype
	mov $text,%ax
	mov %ax,ems_soff
	mov %cs,ems_sseg
	movb $1,ems_dtype
	mov ems_handle,%ax
	mov %ax,ems_dhandle
	movw $100,ems_doff
	movw $2,ems_dseg
	mov $ems_struct,%si
	mov $0x5700,%ax
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x4402,%ax		# page 2 in physical page 2, at 9800h
	mov $2,%bx
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x9800,%ax
	mov %ax,%es
	mov $100,%di
	mov $text,%si
	mov $16,%cx
	cld
	repe cmpsb
	jne fail
	push %cs
	pop %es
	# A name, found again.
	mov $0x5301,%ax
	mov ems_handle,%dx
	mov $hname,%si
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x5401,%ax
	mov $hname,%si
	int $0x67
	or %ah,%ah
	jnz fail
	cmp ems_handle,%dx
	jne fail
	jmp pass
t_ems_device:
	mov $emmname,%dx
	mov $0x3d00,%ax
	int $0x21
	jc fail
	mov %ax,%bx
	mov $0x4400,%ax
	int $0x21
	jc fail
	test $0x80,%dl
	jz fail
	mov $0x4407,%ax
	int $0x21
	jc fail
	cmp $0xff,%al
	jne fail
	mov $0x3e,%ah
	int $0x21
	jc fail
	jmp pass
# 56h: logical page 3 in physical page 0 for a far call, and the
# structure's old map (logical page 1 there) once it returns.
t_ems_call:
	mov $0x4400,%ax		# page 1 marked, then page 0 in physical page 0
	mov $1,%bx
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x9000,%ax
	mov %ax,%es
	movw $0x1234,%es:0
	mov $0x4400,%ax
	xor %bx,%bx
	mov ems_handle,%dx
	int $0x67
	cmpw $0xbeef,%es:0
	jne fail
	mov %cs,call_target+2
	mov %cs,call_new+2
	mov %cs,call_old+2
	movw $0,callee_saw
	push %cs
	pop %ds
	mov $call_struct,%si
	mov $0x5600,%ax
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	cmpw $1,callee_saw	# page 3, neither 0 nor 1, during the call
	jne fail
	cmpw $0x1234,%es:0	# the old map: page 1
	jne fail
	mov $0x4400,%ax		# what the call wrote is in page 3
	mov $3,%bx
	mov ems_handle,%dx
	int $0x67
	cmpw $0x3333,%es:0
	jne fail
	push %cs
	pop %es
	jmp pass
callee:	push %es
	mov $0x9000,%ax
	mov %ax,%es
	cmpw $0xbeef,%es:0
	je 1f
	cmpw $0x1234,%es:0
	je 1f
	movw $1,%cs:callee_saw
1:	movw $0x3333,%es:0
	pop %es
	lret
t_ems_free:
	mov $0x45,%ah
	mov ems_handle,%dx
	int $0x67
	or %ah,%ah
	jnz fail
	mov $0x45,%ah
	mov ems_handle,%dx
	int $0x67
	cmp $0x83,%ah
	jne fail
	mov $0x42,%ah
	int $0x67
	cmp $64,%bx
	jne fail
	jmp pass

# --- without the drivers --------------------------------------------------
t_none:
	mov $0x4300,%ax
	int $0x2f
	cmp $0x80,%al
	je fail
	xor %ax,%ax		# INT 67h leads to a bare IRET
	mov %ax,%es
	les %es:0x19c,%di
	cmpb $0xcf,%es:(%di)
	jne fail
	mov $0x0a,%di
	mov $ems_name,%si
	mov $8,%cx
	cld
	repe cmpsb
	je fail
	push %cs
	pop %es
	jmp pass

	.data
prefix:	.ascii "MEM16: $"
ok:	.ascii " ok\r\n$"
failed:	.ascii " FAILED\r\n$"
emmname: .asciz "EMMXXXX0"
hname:	.ascii "MEM16TST"
text:	.ascii "0123456789ABCDEF"
copy:	.space 16
copy2:	.space 16
failures: .byte 0
have_hma: .byte 0
end_ptr: .word 0
xms_entry: .word 0,0
handle:	.word 0
ems_handle: .word 0
move_struct:
move_len: .long 0
move_src: .word 0
move_soff: .long 0
move_dst: .word 0
move_doff: .long 0
ems_struct:
ems_len: .long 0
ems_stype: .byte 0
ems_shandle: .word 0
ems_soff: .word 0
ems_sseg: .word 0
ems_dtype: .byte 0
ems_dhandle: .word 0
ems_doff: .word 0
ems_dseg: .word 0
call_struct:
call_target: .word callee,0
	.byte 1
call_new: .word new_map,0
	.byte 1
call_old: .word old_map,0
	.space 8
new_map: .word 3,0		# logical page 3 in physical page 0
old_map: .word 1,0		# logical page 1 there after the call
callee_saw: .word 0
n_found: .ascii "XMS found$"
n_alloc: .ascii "XMS blocks and moves$"
n_lock:	.ascii "XMS lock, resize, free$"
n_hma:	.ascii "XMS high memory area and A20$"
n_386:	.ascii "XMS 3.0 functions$"
n_efound: .ascii "EMS found$"
n_eframe: .ascii "EMS frame below 640K$"
n_emap:	.ascii "EMS pages mapped$"
n_emove: .ascii "EMS move and names$"
n_edev:	.ascii "EMS device$"
n_efree: .ascii "EMS free$"
n_ecall: .ascii "EMS map and call$"
n_none:	.ascii "no XMS or EMS$"
tests:	.word n_found,t_xms_found,n_alloc,t_xms_alloc,n_lock,t_xms_lock,n_hma,t_xms_hma,n_386,t_xms_386
	.word n_efound,t_ems_found,n_eframe,t_ems_frame,n_emap,t_ems_map,n_emove,t_ems_move,n_edev,t_ems_device,n_ecall,t_ems_call,n_efree,t_ems_free
tests_end:
none_tests: .word n_none,t_none
none_end:
