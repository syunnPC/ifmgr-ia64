# SPDX-License-Identifier: GPL-2.0-or-later
# GFX16.COM: a 16-bit program for the VDM's VGA graphics modes. It draws in
# modes 13h, 12h and 4 through INT 10h, in video memory and through the
# graphics controller, reads pixels back and writes text with the BIOS and
# DOS, waiting for a key after each mode so that the screen can be checked:
#   13h  a red 100x50 block at 0,0 (memory), blue and green 20x20 blocks of
#        the default palette (20h, 30h) at x=120 and 160, a yellow line at
#        y=100 from x=200 to 219 (AH=0Ch), "MODE13" in white (AH=0Eh) and
#        "DOS13" through DOS on text row 20
#   12h  a green line at y=10 from x=0 to 99 (AH=0Ch), a red 80x50 block at
#        x=80, y=100 (set/reset), "MODE12" in yellow (AH=13h) on row 20
#   4    palette 1, a white line at y=50 and a cyan one at y=51 from x=0 to
#        99, "CGA" in magenta on row 2
# then mode 3 and the result.
	.code16
	.text
	.globl _start
_start:
	cld
	mov $0x0013,%ax
	int $0x10
	movb $'1',code			# mode 13h
	mov $0x0f,%ah
	int $0x10
	cmp $0x13,%al
	jne fail
	mov $0xa000,%ax
	mov %ax,%es
	xor %di,%di
	mov $50,%dx
1:	mov $100,%cx
	mov $0x04,%al
	rep stosb
	add $220,%di
	dec %dx
	jnz 1b
	mov $120,%di
	mov $0x20,%al
	call block
	mov $160,%di
	mov $0x30,%al
	call block
	mov $200,%si
2:	mov $0x0c0e,%ax
	xor %bh,%bh
	mov %si,%cx
	mov $100,%dx
	int $0x10
	inc %si
	cmp $220,%si
	jb 2b
	movb $'2',code			# the pixel back, also from memory
	mov $0x0d00,%ax
	xor %bh,%bh
	mov $210,%cx
	mov $100,%dx
	int $0x10
	cmp $0x0e,%al
	jne fail
	cmpb $0x0e,%es:100*320+210
	jne fail
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x1400,%dx
	int $0x10
	mov $mode13,%si
	mov $0x0f,%bl
	call bios_text
	mov $dos13,%dx
	mov $9,%ah
	int $0x21
	xor %ah,%ah
	int $0x16

	mov $0x0012,%ax
	int $0x10
	movb $'3',code			# mode 12h
	mov $0x0f,%ah
	int $0x10
	cmp $0x12,%al
	jne fail
	xor %si,%si
3:	mov $0x0c02,%ax
	xor %bh,%bh
	mov %si,%cx
	mov $10,%dx
	int $0x10
	inc %si
	cmp $100,%si
	jb 3b
	movb $'4',code
	mov $0x0d00,%ax
	xor %bh,%bh
	mov $50,%cx
	mov $10,%dx
	int $0x10
	cmp $2,%al
	jne fail
	mov $0x3ce,%dx			# set/reset red, in all planes
	mov $0x0400,%ax
	out %ax,%dx
	mov $0x0f01,%ax
	out %ax,%dx
	mov $0xa000,%ax
	mov %ax,%es
	mov $100*80+10,%di
	mov $50,%bx
4:	mov $10,%cx
	mov $0xff,%al
	rep stosb
	add $70,%di
	dec %bx
	jnz 4b
	mov $0x3ce,%dx
	mov $0x0001,%ax
	out %ax,%dx
	xor %ax,%ax
	out %ax,%dx
	movb $'5',code
	mov $0x0d00,%ax
	xor %bh,%bh
	mov $100,%cx
	mov $120,%dx
	int $0x10
	cmp $4,%al
	jne fail
	push %ds
	pop %es
	mov $0x1301,%ax
	mov $0x000e,%bx
	mov $mode12_end-mode12,%cx
	mov $0x1400,%dx
	mov $mode12,%bp
	int $0x10
	xor %ah,%ah
	int $0x16

	mov $0x0004,%ax
	int $0x10
	mov $0x0b00,%ax
	mov $0x0101,%bx
	int $0x10
	xor %si,%si
5:	mov $0x0c03,%ax
	xor %bh,%bh
	mov %si,%cx
	mov $50,%dx
	int $0x10
	mov $0x0c01,%ax
	mov $51,%dx
	int $0x10
	inc %si
	cmp $100,%si
	jb 5b
	movb $'6',code			# the odd line's pixel, also in its bank
	mov $0x0d00,%ax
	xor %bh,%bh
	mov $10,%cx
	mov $51,%dx
	int $0x10
	cmp $1,%al
	jne fail
	mov $0xb800,%ax
	mov %ax,%es
	cmpb $0x55,%es:0x2000+25*80+2
	jne fail
	mov $0x0200,%ax
	xor %bx,%bx
	mov $0x0200,%dx
	int $0x10
	mov $cga,%si
	mov $0x02,%bl
	call bios_text
	xor %ah,%ah
	int $0x16
	mov $ok,%dx
	jmp report
fail:
	mov $failed,%dx
report:
	push %dx
	mov $0x0003,%ax
	int $0x10
	pop %dx
	mov $9,%ah
	int $0x21
	mov $0x4c00,%ax
	int $0x21
# A 20x20 block of color AL at ES:DI in mode 13h.
block:
	mov $20,%dx
1:	mov $20,%cx
	rep stosb
	add $300,%di
	dec %dx
	jnz 1b
	ret
# The string at SI through the BIOS teletype in color BL.
bios_text:
	lodsb
	or %al,%al
	jz 1f
	mov $0x0e,%ah
	xor %bh,%bh
	int $0x10
	jmp bios_text
1:	ret
mode13:	.asciz "MODE13"
dos13:	.ascii "DOS13$"
mode12:	.ascii "MODE12"
mode12_end:
cga:	.asciz "CGA"
ok:	.ascii "GFX16: ok\r\n$"
failed:	.ascii "GFX16: FAIL "
code:	.ascii "?\r\n$"
