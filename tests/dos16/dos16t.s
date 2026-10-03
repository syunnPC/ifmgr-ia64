# SPDX-License-Identifier: GPL-2.0-or-later
# DOS16T.EXE: an 8086 MZ program that checks the VDM. Each test prints
# "DOS16T: <name> ok" or "FAILED"; the exit code is the number of failures.
# Code, data and stack share one segment; code2 is a second segment reached
# through a relocated far call.
	.code16
	.section .hdr,"a"
	.ascii "MZ"
	.word _last,_pages,(relocs_end-relocs)/4,_header_paras
	.word 0x40,0xffff		# extra paragraphs: minimum, maximum
	.word 0,stack_top		# SS:SP
	.word 0,start,0			# checksum, IP, CS
	.word relocs,0
relocs:
	.word farcall+3,0
	.word segword,0
relocs_end:
	.balign 16

	.text
	.globl start,code2
start:
	push %cs
	pop %ds
	mov %es,psp
	push %ds
	pop %es
	mov $tests,%si
next:	cmp $tests_end,%si
	jae done
	push %si
	call *2(%si)
	pop %si
	pushf
	push %ds
	pop %es
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
	incw failures
1:	mov $9,%ah
	int $0x21
	add $4,%si
	jmp next
done:	mov $prefix,%dx
	mov $9,%ah
	int $0x21
	mov $all_ok,%dx
	cmpw $0,failures
	je 1f
	mov $some_failed,%dx
1:	mov $9,%ah
	int $0x21
	mov failures,%al
	mov $0x4c,%ah
	int $0x21

pass:	clc
	ret
fail:	push %ds
	pop %es
	stc
	ret

# A far call and a segment word, both relocated by the loader.
t_far:
farcall:
	lcall $_code2_seg,$(far_proc-code2)
	cmp $0x1234,%ax
	jne fail
	cmp segword,%bx
	jne fail
	jmp pass

# Shrink the program's block, allocate, fill and free memory.
t_mem:
	mov psp,%es
	mov $0x1000,%bx
	mov $0x4a,%ah
	int $0x21
	jc fail
	mov $0x100,%bx
	mov $0x48,%ah
	int $0x21
	jc fail
	mov %ax,%es
	xor %di,%di
	mov $0x800,%cx
	mov $0x55aa,%ax
	cld
	rep stosw
	mov $0x49,%ah
	int $0x21
	jc fail
	mov $0xffff,%bx
	mov $0x48,%ah
	int $0x21
	jnc fail
	cmp $8,%ax
	jne fail
	cmp $0x1000,%bx
	jb fail
	jmp pass

# Create, write, seek, read, search for and delete a file.
t_file:
	mov $fname,%dx
	xor %cx,%cx
	mov $0x3c,%ah
	int $0x21
	jc fail
	mov %ax,%bx
	mov $alpha,%dx
	mov $26,%cx
	mov $0x40,%ah
	int $0x21
	jc fail
	cmp $26,%ax
	jne fail
	mov $0x4200,%ax
	xor %cx,%cx
	mov $10,%dx
	int $0x21
	jc fail
	cmp $10,%ax
	jne fail
	mov $buf,%dx
	mov $5,%cx
	mov $0x3f,%ah
	int $0x21
	jc fail
	cmp $5,%ax
	jne fail
	cmpl $0x4e4d4c4b,buf
	jne fail
	mov $0x4202,%ax
	xor %cx,%cx
	xor %dx,%dx
	int $0x21
	cmp $26,%ax
	jne fail
	mov $0x3e,%ah
	int $0x21
	jc fail
	mov $mydta,%dx
	mov $0x1a,%ah
	int $0x21
	mov $fname,%dx
	xor %cx,%cx
	mov $0x4e,%ah
	int $0x21
	jc fail
	cmpw $26,mydta+26
	jne fail
	cmpl $0x31534f44,mydta+30	# "DOS1"
	jne fail
	mov $fname,%dx
	mov $0x41,%ah
	int $0x21
	jc fail
	mov $fname,%dx
	xor %cx,%cx
	mov $0x4e,%ah
	int $0x21
	jnc fail
	jmp pass

# Wildcard search of the root: at least two .COM files.
t_find:
	mov $mydta,%dx
	mov $0x1a,%ah
	int $0x21
	mov $pattern,%dx
	xor %cx,%cx
	mov $0x4e,%ah
	int $0x21
	jc fail
	mov $1,%di
1:	mov $0x4f,%ah
	int $0x21
	jc 2f
	inc %di
	jmp 1b
2:	cmp $2,%di
	jb fail
	jmp pass

# The environment holds COMSPEC and, after it, this program's path.
t_env:
	mov psp,%es
	mov %es:0x2c,%ax
	mov %ax,%es
	xor %di,%di
	xor %bx,%bx
1:	cmpb $0,%es:(%di)
	je 4f
	push %di
	mov $comspec,%si
	mov $8,%cx
	repe cmpsb
	pop %di
	jne 2f
	inc %bx
2:	cmpb $0,%es:(%di)
	je 3f
	inc %di
	jmp 2b
3:	inc %di
	jmp 1b
4:	or %bx,%bx
	jz fail
	cmpw $1,%es:1(%di)
	jne fail
	add $3,%di
5:	cmpb $0,%es:(%di)
	je 6f
	inc %di
	jmp 5b
6:	cmpl $0x4558452e,%es:-4(%di)	# ".EXE"
	jne fail
	jmp pass

# A hooked INT 21h vector that chains to the previous handler.
t_hook:
	mov $0x3521,%ax
	int $0x21
	mov %bx,old21
	mov %es,old21+2
	mov $0x2521,%ax
	mov $hook21,%dx
	int $0x21
	movw $0,hits
	mov $0x30,%ah
	int $0x21
	cmp $4,%al
	jne 1f
	mov $0x30,%ah
	int $0x21
	cmpw $2,hits
	jne 1f
	clc
	jmp 2f
1:	stc
2:	pushf
	push %ds
	lds old21,%dx
	mov $0x2521,%ax
	int $0x21
	pop %ds
	popf
	push %ds
	pop %es
	ret
hook21:
	incw %cs:hits
	ljmp *%cs:old21

# A divide error reaches a hooked INT 0 vector, which skips the DIV.
t_div:
	mov $0x3500,%ax
	int $0x21
	mov %bx,old0
	mov %es,old0+2
	mov $0x2500,%ax
	mov $div_handler,%dx
	int $0x21
	movb $0,divflag
	mov $1,%ax
	xor %dx,%dx
	xor %bx,%bx
	div %bx
	push %ds
	lds old0,%dx
	mov $0x2500,%ax
	int $0x21
	pop %ds
	cmpb $1,divflag
	jne fail
	jmp pass
div_handler:
	push %bp
	mov %sp,%bp
	addw $2,2(%bp)
	movb $1,%cs:divflag
	pop %bp
	iret

# The x87 stack survives a DOS call between instructions.
t_fpu:
	fninit
	filds three
	mov $0x30,%ah
	int $0x21
	filds four
	fmulp
	fistps result
	cmpw $12,result
	jne fail
	jmp pass

# EXEC of a 16-bit child; its exit code comes back through AH=4Dh.
t_exec:
	call exec_hello
	jc fail
	mov $0x4d,%ah
	int $0x21
	cmp $7,%ax
	jne fail
	jmp pass
exec_hello:
	mov %ds,pb_tail+2
	mov %ds,pb_fcb1+2
	mov %ds,pb_fcb2+2
	push %ds
	pop %es
	mov $hello_path,%dx
	mov $paramblk,%bx
	mov $0x4b00,%ax
	int $0x21
	ret

# The child writes to a redirected standard output.
t_redirect:
	mov $outname,%dx
	xor %cx,%cx
	mov $0x3c,%ah
	int $0x21
	jc fail
	mov %ax,fh
	mov $1,%bx
	mov $0x45,%ah
	int $0x21
	jc fail
	mov %ax,saved1
	mov fh,%bx
	mov $1,%cx
	mov $0x46,%ah
	int $0x21
	jc fail
	call exec_hello
	pushf
	mov saved1,%bx
	mov $1,%cx
	mov $0x46,%ah
	int $0x21
	mov saved1,%bx
	mov $0x3e,%ah
	int $0x21
	popf
	jc fail
	mov fh,%bx
	mov $0x4200,%ax
	xor %cx,%cx
	xor %dx,%dx
	int $0x21
	mov $buf,%dx
	mov $64,%cx
	mov $0x3f,%ah
	int $0x21
	jc fail
	cmp $20,%ax
	jb fail
	mov fh,%bx
	mov $0x3e,%ah
	int $0x21
	mov $outname,%dx
	mov $0x41,%ah
	int $0x21
	cmpl $0x4c4c4548,buf		# "HELL"
	jne fail
	jmp pass

# BIOS teletype output and the timer tick count.
t_bios:
	mov $bios_text,%si
1:	lodsb
	or %al,%al
	jz 2f
	mov $0x0e,%ah
	xor %bx,%bx
	int $0x10
	jmp 1b
2:	xor %ah,%ah
	int $0x1a
	mov %dx,%bx
	mov $0x86,%ah
	mov $0x0003,%cx
	mov $0x0d40,%dx		# 200 ms
	int $0x15
	xor %ah,%ah
	int $0x1a
	sub %bx,%dx
	cmp $2,%dx
	jb fail
	cmp $10,%dx
	ja fail
	jmp pass

# A native program started from this one.
t_native:
	mov %ds,pb_tail+2
	mov %ds,pb_fcb1+2
	mov %ds,pb_fcb2+2
	push %ds
	pop %es
	mov $native_path,%dx
	mov $paramblk,%bx
	mov $0x4b00,%ax
	int $0x21
	jc fail
	mov $0x4d,%ah
	int $0x21
	cmp $0,%ax
	jne fail
	jmp pass

# A 16-bit program started by a native child (COMMAND.COM /C) shares the
# screen.
t_nested:
	mov %ds,pb_tail+2
	mov %ds,pb_fcb1+2
	mov %ds,pb_fcb2+2
	movw $nested_tail,pb_tail
	push %ds
	pop %es
	mov $command_path,%dx
	mov $paramblk,%bx
	mov $0x4b00,%ax
	int $0x21
	movw $ctail,pb_tail
	jc fail
	jmp pass

# Port I/O is virtual: the CMOS clock and VGA status answer, and a reset
# request through port CF9h never reaches the machine.
t_ports:
	mov $0x0b,%al
	out %al,$0x70
	in $0x71,%al
	cmp $0x02,%al
	jne fail
	mov $0x3da,%dx
	in %dx,%al
	mov %al,%bl
	in %dx,%al
	xor %bl,%al
	test $8,%al
	jz fail
	mov $0xcf9,%dx
	mov $0x06,%al
	out %al,%dx
	hlt
	jmp pass

# The timer interrupt: hooked INT 8 and INT 1Ch handlers run while the
# program polls the BIOS tick count without making any call, and counter 0
# reprogrammed to four times the rate brings INT 8 that much more often.
t_timer:
	movw $0,count8
	movw $0,count1c
	mov $0x3508,%ax
	int $0x21
	mov %bx,old8
	mov %es,old8+2
	mov $0x351c,%ax
	int $0x21
	mov %bx,old1c
	mov %es,old1c+2
	mov $0x2508,%ax
	mov $timer8,%dx
	int $0x21
	mov $0x251c,%ax
	mov $timer1c,%dx
	int $0x21
	sti
	call wait_ticks
	mov count8,%cx
	mov count1c,%di
	mov $0x36,%al			# counter 0, low then high byte, mode 3
	out %al,$0x43
	xor %al,%al
	out %al,$0x40
	mov $0x40,%al			# 4000h: 72.8 interrupts a second
	out %al,$0x40
	movw $0,count8
	call wait_ticks
	mov count8,%bp
	mov $0x36,%al
	out %al,$0x43
	xor %al,%al
	out %al,$0x40
	out %al,$0x40
	cli
	lds old8,%dx
	mov $0x2508,%ax
	int $0x21
	push %cs
	pop %ds
	lds old1c,%dx
	mov $0x251c,%ax
	int $0x21
	push %cs
	pop %ds
	sti
	cmp $3,%cx			# about four of each in four ticks
	jb fail
	cmp $3,%di
	jb fail
	cmp $10,%bp			# about sixteen at the faster rate
	jb fail
	jmp pass
# Waits for four BIOS ticks, reading 40:6Ch only.
wait_ticks:
	push %es
	mov $0x40,%ax
	mov %ax,%es
	mov %es:0x6c,%bx
	add $4,%bx
1:	mov %es:0x6c,%ax
	cmp %bx,%ax
	jb 1b
	pop %es
	ret
timer8:	incw %cs:count8
	ljmp *%cs:old8
timer1c: incw %cs:count1c
	iret

	.balign 2
tests:	.word n_far,t_far,n_mem,t_mem,n_file,t_file,n_find,t_find,n_env,t_env
	.word n_hook,t_hook,n_div,t_div,n_fpu,t_fpu,n_exec,t_exec,n_redirect,t_redirect
	.word n_bios,t_bios,n_native,t_native,n_nested,t_nested,n_ports,t_ports,n_timer,t_timer
tests_end:
n_far:	.ascii "far call and relocations$"
n_mem:	.ascii "memory blocks$"
n_file:	.ascii "file I/O and search$"
n_find:	.ascii "wildcard search$"
n_env:	.ascii "environment$"
n_hook:	.ascii "hooked INT 21h$"
n_div:	.ascii "divide error handler$"
n_fpu:	.ascii "x87 state$"
n_exec:	.ascii "EXEC and exit code$"
n_redirect: .ascii "redirected child output$"
n_bios:	.ascii "BIOS teletype and timer$"
n_native: .ascii "native child$"
n_nested: .ascii "16-bit grandchild$"
n_ports: .ascii "virtual ports$"
n_timer: .ascii "timer interrupt$"
prefix:	.ascii "DOS16T: $"
ok:	.ascii " ok\r\n$"
failed:	.ascii " FAILED\r\n$"
all_ok:	.ascii "all passed\r\n$"
some_failed: .ascii "some tests failed\r\n$"
fname:	.asciz "DOS16T.TMP"
outname: .asciz "DOS16T.OUT"
pattern: .asciz "C:\\*.COM"
hello_path: .asciz "C:\\HELLO16.COM"
native_path: .asciz "C:\\HELLO.EFI"
command_path: .asciz "C:\\COMMAND.COM"
comspec: .ascii "COMSPEC="
alpha:	.ascii "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
bios_text: .asciz "BIOS teletype\r\n"
ctail:	.byte 6
	.ascii " child\r"
nested_tail: .byte 18
	.ascii " /C HELLO16 nested\r"
	.balign 2
paramblk: .word 0
pb_tail: .word ctail,0
pb_fcb1: .word fcb,0
pb_fcb2: .word fcb,0
fcb:	.fill 16,1,0
three:	.word 3
four:	.word 4
result:	.word 0
psp:	.word 0
failures: .word 0
hits:	.word 0
old21:	.word 0,0
old0:	.word 0,0
old8:	.word 0,0
old1c:	.word 0,0
count8:	.word 0
count1c: .word 0
fh:	.word 0
saved1:	.word 0
divflag: .byte 0
	.balign 2
buf:	.fill 64,1,0
mydta:	.fill 64,1,0
segword: .word _code2_seg
	.balign 2
	.fill 2048,1,0
stack_top:
	.balign 16
code2:
far_proc:
	push %cs
	pop %bx
	mov $0x1234,%ax
	lret
