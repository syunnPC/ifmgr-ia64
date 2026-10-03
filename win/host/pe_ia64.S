// SPDX-License-Identifier: GPL-2.0-or-later
// void pe_flush_code(void *start, u64 bytes): make stores of freshly loaded
// code visible to instruction fetch. fc works on at least 32-byte lines on
// every IA-64 implementation (fc.i is not on Merced); sync.i and srlz.i
// then order the flushes before later fetches.
	.text
	.align 32
	.global pe_flush_code
	.proc pe_flush_code
pe_flush_code:
	alloc r16=ar.pfs,2,0,0,0
	mov r17=ar.lc
	and r14=-32,r32
	add r15=r32,r33
	;;
	sub r18=r15,r14
	;;
	adds r18=31,r18
	;;
	shr.u r18=r18,5
	;;
	cmp.eq p6,p0=0,r18
	adds r18=-1,r18
(p6)	br.cond.spnt 2f
	;;
	mov ar.lc=r18
	;;
1:	fc r14
	adds r14=32,r14
	br.cloop.sptk.few 1b
	;;
2:	sync.i
	;;
	srlz.i
	;;
	mov ar.lc=r17
	br.ret.sptk.many b0
	;;
	.endp pe_flush_code
