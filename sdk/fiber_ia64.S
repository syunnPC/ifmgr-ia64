// SPDX-License-Identifier: GPL-2.0-or-later
// IA-64 fiber switch. Saves the preserved registers of the software
// conventions and swaps the register stack engine's backing store:
// flushrs writes the dirty stacked registers, then (in enforced lazy mode)
// loadrs discards the physical ones and ar.bspstore/ar.rnat select the other
// fiber's store; br.ret refills the caller's frame from it. Offsets follow
// FiberContext in include/fiber.h.

	.text
	.align 32
	.global fiber_switch
	.proc fiber_switch
fiber_switch:
	alloc r16=ar.pfs,2,0,0,0
	mov r17=ar.unat
	mov r18=ar.lc
	mov r19=ar.fpsr
	mov r20=pr
	mov r21=b0
	;;
	mov r2=in0
	add r3=8,in0
	;;
	st8 [r2]=sp,16			// sp	@0
	st8 [r3]=gp,16			// gp	@8
	;;
	st8.spill [r2]=r4,16		// r4	@16
	st8.spill [r3]=r5,16		// r5	@24
	;;
	st8.spill [r2]=r6,16		// r6	@32
	st8.spill [r3]=r7,16		// r7	@40
	;;
	mov r22=ar.unat
	mov r23=b1
	mov r24=b2
	;;
	st8 [r2]=r22,16			// NaT bits of r4-r7	@48
	st8 [r3]=r17,16			// caller's ar.unat	@56
	mov r25=b3
	mov r26=b4
	mov r27=b5
	;;
	st8 [r2]=r21,16			// b0	@64
	st8 [r3]=r23,16			// b1	@72
	;;
	st8 [r2]=r24,16			// b2	@80
	st8 [r3]=r25,16			// b3	@88
	;;
	st8 [r2]=r26,16			// b4	@96
	st8 [r3]=r27,16			// b5	@104
	;;
	st8 [r2]=r16,16			// ar.pfs	@112
	st8 [r3]=r18,16			// ar.lc	@120
	;;
	st8 [r2]=r19,16			// ar.fpsr	@128
	st8 [r3]=r20,16			// pr	@136
	;;
	flushrs
	;;
	mov r28=ar.rsc
	;;
	mov ar.rsc=r0			// enforced lazy, loadrs field 0
	;;
	mov r29=ar.bspstore
	;;
	mov r30=ar.rnat
	;;
	st8 [r2]=r28,16			// ar.rsc	@144
	st8 [r3]=r29			// ar.bspstore	@152
	;;
	st8 [r2]=r30			// ar.rnat	@160
	add r2=176,in0
	add r3=192,in0
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
	add r2=176,in1
	add r3=192,in1
	;;
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
	add r2=16,in1
	add r3=24,in1
	add r8=48,in1
	;;
	ld8 r9=[r8],8			// NaT bits of r4-r7
	;;
	ld8 r10=[r8]			// caller's ar.unat
	mov ar.unat=r9
	;;
	ld8.fill r4=[r2],16
	ld8.fill r5=[r3],16
	;;
	ld8.fill r6=[r2]
	ld8.fill r7=[r3]
	;;
	mov ar.unat=r10
	add r2=64,in1
	add r3=72,in1
	;;
	ld8 r16=[r2],16			// b0
	ld8 r17=[r3],16			// b1
	;;
	ld8 r18=[r2],16			// b2
	ld8 r19=[r3],16			// b3
	;;
	ld8 r20=[r2],16			// b4
	ld8 r21=[r3],16			// b5
	;;
	ld8 r22=[r2],16			// ar.pfs
	ld8 r23=[r3],16			// ar.lc
	;;
	ld8 r24=[r2],16			// ar.fpsr
	ld8 r25=[r3],16			// pr
	;;
	ld8 r26=[r2],16			// ar.rsc
	ld8 r27=[r3]			// ar.bspstore
	add r30=8,in1
	;;
	ld8 r28=[r2]			// ar.rnat
	ld8 r29=[in1]			// sp
	;;
	ld8 r31=[r30]			// gp
	mov b0=r16
	mov b1=r17
	;;
	mov b2=r18
	mov b3=r19
	mov b4=r20
	;;
	mov b5=r21
	mov ar.pfs=r22
	mov ar.lc=r23
	;;
	mov ar.fpsr=r24
	mov pr=r25,-1
	;;
	loadrs				// discard stacked registers outside this frame
	;;
	mov ar.bspstore=r27
	;;
	mov ar.rnat=r28
	;;
	mov ar.rsc=r26
	mov sp=r29
	mov gp=r31
	;;
	br.ret.sptk.many b0
	;;
	.endp fiber_switch

// First code of a fiber: an empty register frame, r4=argument and
// r5=function descriptor of the entry. The entry must not return.
	.align 32
	.global fiber_trampoline
	.proc fiber_trampoline
fiber_trampoline:
	alloc r16=ar.pfs,0,0,1,0
	mov out0=r4
	ld8 r2=[r5],8
	;;
	ld8 gp=[r5]
	mov b6=r2
	;;
	br.call.sptk.many b0=b6
	;;
	break 0
	;;
	.endp fiber_trampoline

// u64 fiber_rsc(void): the caller's RSE configuration, for new fibers.
	.align 32
	.global fiber_rsc
	.proc fiber_rsc
fiber_rsc:
	mov r8=ar.rsc
	br.ret.sptk.many b0
	;;
	.endp fiber_rsc
