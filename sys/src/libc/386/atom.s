/* 386 atomics */
/*
 * is atomicity enough, or must competing aincs return in order of their
 * completed incs?  do the losers have to keep trying until they win?
 */

TEXT ainc(SB), 1, $-4				/* long ainc(long*); */
	MOVL	arg+0(FP), BX
	MOVL	$1, AX
	LOCK; BYTE $0x0f; BYTE $0xc1; BYTE $0x03/* XADDL AX, (BX) */
	ADDL	$1, AX				/* overflow if -ve or 0 */
	RET

/*
 * adec is typically used to release a lock.  on 386 and amd64, writes are
 * never re-ordered, so a fence isn't needed to ensure that previous stores
 * complete before the count is decremented.
 */
TEXT adec(SB), 1, $-4				/* long adec(long*); */
	MOVL	arg+0(FP), BX
	MOVL	$-1, AX
	LOCK; BYTE $0x0f; BYTE $0xc1; BYTE $0x03/* XADDL AX, (BX) */
	SUBL	$1, AX				/* underflow if -ve */
	RET

/*
 * int cas(uint *p, int ov, int nv);
 * int casp(void **p, void *ov, void *nv);
 */

/*
 * CMPXCHG (CX), DX: 0000 1111 1011 000w oorr rmmm,
 * mmm = CX = 001; rrr = DX = 010
 */
#define CMPXCHG		BYTE $0x0F; BYTE $0xB1; BYTE $0x11

TEXT	cas+0(SB),0,$0
TEXT	casp+0(SB),0,$0
	MOVL	p+0(FP), CX
	MOVL	ov+4(FP), AX
	MOVL	nv+8(FP), DX
	LOCK; CMPXCHG		/* compares AX (ov) with (CX) */
	JNE	fail
	MOVL	$1,AX
	RET
fail:
	MOVL	$0,AX
	RET

/*
 * int casv(u64int *p, u64int ov, u64int nv);
 */

/*
 * CMPXCHG64 (DI): 0000 1111 1100 0111 0000 1110,
 */
#define CMPXCHG64		BYTE $0x0F; BYTE $0xC7; BYTE $0x0F

TEXT	casv+0(SB),0,$0
	MOVL	p+0(FP), DI
	MOVL	ov+0x4(FP), AX
	MOVL	ov+0x8(FP), DX
	MOVL	nv+0xc(FP), BX
	MOVL	nv+0x10(FP), CX
	LOCK; CMPXCHG64
	JNE	fail
	MOVL	$1,AX
	RET

/*
 * everything below this is experimental and not advertised.
 */

/*
 * Versions of compare-and-swap that return the old value
 * (i.e., the value of *p at the time of the operation
 * 	xcas(p, o, n) == o
 * yields the same value as
 *	cas(p, o, n)
 * ). xcas can be used in constructs like
 *	for(o = *p; (oo = xcas(p, o, o+1)) != o; o = oo)
 *		;
 * to avoid the extra dereference of *p (the example is a silly
 * way to increment *p atomically)
 *
 * int		xcas(int *p, int ov, int nv);
 * void*	xcasp(void **p, void *ov, void *nv);
 * u64int	xcasv(u64int *p, u64int ov, u64int nv);
 */

TEXT	xcas+0(SB),0,$0
TEXT	xcasp+0(SB),0,$0
	MOVL	p+0(FP), CX
	MOVL	ov+4(FP), AX	/* accumulator */
	MOVL	nv+8(FP), DX
	LOCK; CMPXCHG
	RET

/*
 * The CMPXCHG8B instruction also requires three operands: a 64-bit value in
 * EDX:EAX, a 64-bit value in ECX:EBX, and a destination operand in memory.
 * The instruction compares the 64-bit value in the EDX:EAX registers with the
 * destination operand.  If they are equal, the 64-bit value in the ECX:EBX
 * register is stored in the destination operand.  If the EDX:EAX register and
 * the destination are not equal, the destination is loaded in the EDX:EAX
 * register.  The CMPXCHG8B instruction can be combined with the LOCK prefix
 * to perform the operation atomically.
 */

TEXT	xcasv+0(SB),0,$0
	MOVL	p+4(FP), DI
	MOVL	ov+0x8(FP), AX
	MOVL	ov+0xc(FP), DX
	MOVL	nv+0x10(FP), BX
	MOVL	nv+0x14(FP), CX
	LOCK; CMPXCHG64
	MOVL	.ret+0x0(FP),CX	/* pointer to return value */
	MOVL	AX,0x0(CX)
	MOVL	DX,0x4(CX)
	RET
