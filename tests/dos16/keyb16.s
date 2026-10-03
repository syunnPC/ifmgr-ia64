# SPDX-License-Identifier: GPL-2.0-or-later
# KEYB16.COM: an 8086 program reading the keyboard through INT 16h with
# KEYB's German layout loaded. INT 2Fh AD80h finds KEYB (AL=FFh, version
# 1.0, ES:DI its shared data with ACTIVE_LANGUAGE "GR"); after "KEYB16:
# ready" it prints the INT 16h word of each of the next four keys
# ("KEYB16: key xxxx"); AD82h BL=0 gives the US layout for one key more and
# BL=FFh the German one back; AD82h BL=5 and AD81h for a code page KEYB has
# no table for are refused (CF, AX=1), 437 taken. Each check prints
# "KEYB16: <name> ok" or "FAILED"; the exit code is the failures.
	.code16
	.text
	.globl _start
_start:
	mov $0xad80,%ax
	int $0x2f
	cmp $0xff,%al
	jne 1f
	cmp $0x0100,%bx
	jne 1f
	cmpw $0x5247,%es:22(%di)	# "GR"
	jne 1f
	clc
	jmp 2f
1:	stc
2:	push %cs
	pop %es
	mov $n_inst,%dx
	call result
	mov $4,%cx
	call keys
	mov $0xad82,%ax			# US
	xor %bl,%bl
	int $0x2f
	mov $n_us,%dx
	call result
	mov $1,%cx
	call keys
	mov $0xad82,%ax			# German again
	mov $0xff,%bl
	int $0x2f
	mov $n_national,%dx
	call result
	mov $0xad82,%ax			# no such mode
	mov $5,%bl
	int $0x2f
	cmc
	jc 3f
	cmp $1,%ax
	je 3f
	stc
3:	mov $n_mode,%dx
	call result
	mov $0xad81,%ax			# no German table for 863
	mov $863,%bx
	int $0x2f
	cmc
	jc 4f
	cmp $1,%ax
	je 4f
	stc
4:	mov $n_page,%dx
	call result
	mov $0xad81,%ax
	mov $437,%bx
	int $0x2f
	mov $n_437,%dx
	call result
	mov failures,%al
	mov $0x4c,%ah
	int $0x21

# "KEYB16: ready", then CX keys from INT 16h, each as "KEYB16: key xxxx".
keys:	mov $ready,%dx
	mov $9,%ah
	int $0x21
5:	push %cx
	xor %ah,%ah
	int $0x16
	mov %ax,%bx
	mov $key,%dx
	mov $9,%ah
	int $0x21
	mov $4,%cx
6:	rol $4,%bx
	mov %bl,%dl
	and $15,%dl
	add $'0',%dl
	cmp $'9',%dl
	jbe 7f
	add $'a'-'0'-10,%dl
7:	mov $2,%ah
	int $0x21
	loop 6b
	mov $crlf,%dx
	mov $9,%ah
	int $0x21
	pop %cx
	loop 5b
	ret

# "KEYB16: <name at DX> ok" when CF is clear, else FAILED.
result:	pushf
	push %dx
	mov $prefix,%dx
	mov $9,%ah
	int $0x21
	pop %dx
	int $0x21
	popf
	mov $ok,%dx
	jnc 8f
	mov $failed,%dx
	incb failures
8:	mov $9,%ah
	int $0x21
	ret

	.data
prefix:	.ascii "KEYB16: $"
ok:	.ascii " ok\r\n$"
failed:	.ascii " FAILED\r\n$"
ready:	.ascii "KEYB16: ready\r\n$"
key:	.ascii "KEYB16: key $"
crlf:	.ascii "\r\n$"
failures: .byte 0
n_inst:	.ascii "installed$"
n_us:	.ascii "US mode$"
n_national: .ascii "national mode$"
n_mode:	.ascii "bad mode refused$"
n_page:	.ascii "page without a table refused$"
n_437:	.ascii "page 437$"
