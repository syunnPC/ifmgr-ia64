// SPDX-License-Identifier: GPL-2.0-or-later
// Entry to and return from the IA-32 instruction set (platform/efi_ia32.c).
//
// ia32_enter(context, frame, ipsr, iip) saves the preserved registers of the
// software conventions in frame, flushes the register stack, loads the
// IA-32 register image from context (IoIa32Context in include/io.h) and
// enters it with rfi. IA-32 execution leaves GR4-GR7, the branch registers
// b0-b5 and the register stack pointers unmodified; the stacked registers are
// lost, but the flushed frames of the callers stay in the backing store.
//
// An IA-32 interruption reaches the Debug Support callback, which stores the
// IA-32 state and resumes at ia32_landing in IA-64 code with an empty frame.
// GR4 still holds the frame: the landing pad restores what ia32_enter saved,
// as fiber_switch does when it switches back, and returns to ia32_enter's
// caller.
//
// Frame offsets: sp 0, gp 8, r4-r7 16-40, their NaT bits 48, caller's
// ar.unat 56, b0-b5 64-104, ar.pfs 112, ar.lc 120, ar.fpsr 128, pr 136,
// ar.rsc 144, ar.bspstore 152, ar.rnat 160, f2-f5 and f16-f31 from 176.

	.text
	.align 32
	.global ia32_enter
	.proc ia32_enter
ia32_enter:
	alloc r16=ar.pfs,4,0,0,0
	mov r17=ar.unat
	mov r18=ar.lc
	mov r19=ar.fpsr
	mov r20=pr
	mov r21=b0
	;;
	mov r2=in1
	add r3=8,in1
	;;
	st8 [r2]=sp,16
	st8 [r3]=gp,16
	;;
	st8.spill [r2]=r4,16
	st8.spill [r3]=r5,16
	;;
	st8.spill [r2]=r6,16
	st8.spill [r3]=r7,16
	;;
	mov r22=ar.unat
	mov r23=b1
	mov r24=b2
	;;
	st8 [r2]=r22,16
	st8 [r3]=r17,16
	mov r25=b3
	mov r26=b4
	mov r27=b5
	;;
	st8 [r2]=r21,16
	st8 [r3]=r23,16
	;;
	st8 [r2]=r24,16
	st8 [r3]=r25,16
	;;
	st8 [r2]=r26,16
	st8 [r3]=r27,16
	;;
	st8 [r2]=r16,16
	st8 [r3]=r18,16
	;;
	st8 [r2]=r19,16
	st8 [r3]=r20,16
	;;
	flushrs
	;;
	mov r28=ar.rsc
	;;
	mov ar.rsc=r0
	;;
	mov r29=ar.bspstore
	;;
	mov r30=ar.rnat
	;;
	st8 [r2]=r28,16
	st8 [r3]=r29
	;;
	st8 [r2]=r30
	add r2=176,in1
	add r3=192,in1
	;;
	stf.spill [r2]=f2,32
	stf.spill [r3]=f3,32
	;;
	stf.spill [r2]=f4,32
	stf.spill [r3]=f5,32
	;;
	stf.spill [r2]=f16,32
	stf.spill [r3]=f17,32
	;;
	stf.spill [r2]=f18,32
	stf.spill [r3]=f19,32
	;;
	stf.spill [r2]=f20,32
	stf.spill [r3]=f21,32
	;;
	stf.spill [r2]=f22,32
	stf.spill [r3]=f23,32
	;;
	stf.spill [r2]=f24,32
	stf.spill [r3]=f25,32
	;;
	stf.spill [r2]=f26,32
	stf.spill [r3]=f27,32
	;;
	stf.spill [r2]=f28,32
	stf.spill [r3]=f29,32
	;;
	stf.spill [r2]=f30
	stf.spill [r3]=f31
	// The IA-32 image: FR8-FR31, then the application registers. The RSE
	// stays in enforced lazy mode until the landing pad.
	mov r2=in0
	add r3=16,in0
	;;
	ldf.fill f8=[r2],32
	ldf.fill f9=[r3],32
	;;
	ldf.fill f10=[r2],32
	ldf.fill f11=[r3],32
	;;
	ldf.fill f12=[r2],32
	ldf.fill f13=[r3],32
	;;
	ldf.fill f14=[r2],32
	ldf.fill f15=[r3],32
	;;
	ldf.fill f16=[r2],32
	ldf.fill f17=[r3],32
	;;
	ldf.fill f18=[r2],32
	ldf.fill f19=[r3],32
	;;
	ldf.fill f20=[r2],32
	ldf.fill f21=[r3],32
	;;
	ldf.fill f22=[r2],32
	ldf.fill f23=[r3],32
	;;
	ldf.fill f24=[r2],32
	ldf.fill f25=[r3],32
	;;
	ldf.fill f26=[r2],32
	ldf.fill f27=[r3],32
	;;
	ldf.fill f28=[r2],32
	ldf.fill f29=[r3],32
	;;
	ldf.fill f30=[r2]
	ldf.fill f31=[r3]
	add r2=544,in0			// cflg
	add r3=552,in0			// fsr
	;;
	ld8 r8=[r2],16
	ld8 r9=[r3],16
	;;
	ld8 r10=[r2]			// fcr
	ld8 r11=[r3]			// fir
	add r2=576,in0			// fdr
	add r3=456,in0			// eflags
	;;
	ld8 r14=[r2]
	ld8 r15=[r3]
	add r2=528,in0			// cs
	add r3=536,in0			// ss
	;;
	ld8 r16=[r2]
	ld8 r17=[r3]
	;;
	mov ar.cflg=r8
	mov ar.fsr=r9
	mov ar.fcr=r10
	mov ar.fir=r11
	mov ar.fdr=r14
	mov ar.eflag=r15
	mov ar.csd=r16
	mov ar.ssd=r17
	add r2=464,in0			// data_sel
	add r3=472,in0			// sys_sel
	;;
	ld8 r16=[r2],16
	ld8 r17=[r3],16
	;;
	ld8 r24=[r2],16			// es
	ld8 r27=[r3],16			// ds
	;;
	ld8 r28=[r2],16			// fs
	ld8 r29=[r3],16			// gs
	;;
	ld8 r30=[r2]			// ldt
	ld8 r31=[r3]			// gdt
	add r2=384,in0
	add r3=392,in0
	;;
	ld8 r8=[r2],16			// eax
	ld8 r9=[r3],16			// ecx
	;;
	ld8 r10=[r2],16			// edx
	ld8 r11=[r3],16			// ebx
	;;
	ld8 r12=[r2],16			// esp
	ld8 r13=[r3],16			// ebp
	;;
	ld8 r14=[r2]			// esi
	ld8 r15=[r3]			// edi
	mov r4=in1
	;;
	rsm psr.i|psr.ic
	;;
	srlz.d
	;;
	mov cr.ipsr=in2
	mov cr.iip=in3
	mov cr.ifs=r0
	;;
	rfi
	;;
	.endp ia32_enter

	.align 32
	.global ia32_landing
	.proc ia32_landing
ia32_landing:
	mov r2=r4
	add r3=8,r4
	add r14=144,r4
	add r15=152,r4
	;;
	ld8 sp=[r2]
	ld8 gp=[r3]
	add r2=160,r4
	;;
	ld8 r16=[r14]			// ar.rsc
	ld8 r17=[r15]			// ar.bspstore
	ld8 r18=[r2]			// ar.rnat
	mov ar.rsc=r0
	;;
	loadrs
	;;
	mov ar.bspstore=r17
	;;
	mov ar.rnat=r18
	add r2=48,r4
	;;
	mov ar.rsc=r16
	ld8 r19=[r2]
	;;
	mov ar.unat=r19
	add r2=24,r4
	add r3=32,r4
	;;
	ld8.fill r5=[r2],16
	ld8.fill r6=[r3]
	;;
	ld8.fill r7=[r2]
	add r2=56,r4
	add r3=64,r4
	;;
	ld8 r20=[r2],16			// caller's ar.unat
	ld8 r21=[r3],16			// b0
	;;
	ld8 r22=[r2],16			// b1
	ld8 r23=[r3],16			// b2
	;;
	ld8 r24=[r2],16			// b3
	ld8 r25=[r3],16			// b4
	;;
	ld8 r26=[r2],16			// b5
	ld8 r27=[r3],16			// ar.pfs
	;;
	ld8 r28=[r2],16			// ar.lc
	ld8 r29=[r3]			// ar.fpsr
	;;
	ld8 r30=[r2]			// pr
	mov b0=r21
	mov b1=r22
	mov b2=r23
	mov b3=r24
	mov b4=r25
	mov b5=r26
	mov ar.pfs=r27
	mov ar.lc=r28
	mov ar.fpsr=r29
	add r2=176,r4
	add r3=192,r4
	;;
	mov pr=r30,-1
	ldf.fill f2=[r2],32
	ldf.fill f3=[r3],32
	;;
	ldf.fill f4=[r2],32
	ldf.fill f5=[r3],32
	;;
	ldf.fill f16=[r2],32
	ldf.fill f17=[r3],32
	;;
	ldf.fill f18=[r2],32
	ldf.fill f19=[r3],32
	;;
	ldf.fill f20=[r2],32
	ldf.fill f21=[r3],32
	;;
	ldf.fill f22=[r2],32
	ldf.fill f23=[r3],32
	;;
	ldf.fill f24=[r2],32
	ldf.fill f25=[r3],32
	;;
	ldf.fill f26=[r2],32
	ldf.fill f27=[r3],32
	;;
	ldf.fill f28=[r2],32
	ldf.fill f29=[r3],32
	;;
	ldf.fill f30=[r2]
	ldf.fill f31=[r3]
	add r2=16,r4
	;;
	ld8.fill r4=[r2]
	;;
	mov ar.unat=r20
	br.ret.sptk.many b0
	;;
	.endp ia32_landing
