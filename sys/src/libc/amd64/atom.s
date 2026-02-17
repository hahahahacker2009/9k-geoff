/*
 * amd64 atomic operations
 */

/*
 * adec is typically used to release a lock.  on 386 and amd64, writes are
 * never re-ordered, so a fence isn't needed to ensure that previous stores
 * complete before the count is decremented.
 */

/*
 * this used to operate on longs, but vlongs work slightly better
 * on apu2 (amd jaguar).
 */
TEXT ainc(SB), 1, $-4				/* int ainc(int*); */
	MOVQ	$1, CX
	JMP	aincdec
TEXT adec(SB), 1, $-4				/* int adec(int*); */
	MOVQ	$-1, CX
aincdec:
	MOVQ	CX, AX			/* copy delta */
	LOCK; XADDL AX, (RARG)		/* swaps AX & (RARG), (RARG) += AX */
	MOVLQSX	AX, AX
	ADDQ	CX, AX			/* add delta to old value */
	RET

/*
 * int cas(uint *p, int ov, int nv);
 *
 * compare-and-swap: atomically set *addr to nv only if it contains ov,
 * and returns the old value.  this version returns 1 on success, 0 on failure
 * instead.
 */

TEXT cas(SB), 1, $0
	MOVL	ov+8(FP), AX
	MOVL	nv+16(FP), BX
	LOCK; CMPXCHGL BX, (RARG)		/* compares with AX (ov) */
_cascset:
	MOVL	$1, AX				/* use CMOVLEQ etc. here? */
	JEQ	_casr1				/* success? jump, return 1 */
	DECL	AX				/* failed: return 0 */
_casr1:
	RET

/*
 * int casv(u64int *p, u64int ov, u64int nv);
 * int casp(void **p, void *ov, void *nv);
 */

TEXT casv(SB), 1, $0
TEXT casp(SB), 1, $0
	MOVQ	ov+8(FP), AX
	MOVQ	nv+16(FP), BX
	LOCK; CMPXCHGQ BX, (RARG)
	JMP	_cascset
