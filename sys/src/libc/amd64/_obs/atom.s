TEXT ainc(SB), 1, $-4				/* int ainc(int*); */
	MOVL	$1, CX
	JMP	aincdec
TEXT adec(SB), 1, $-4				/* int adec(int*); */
	MOVL	$-1, CX
aincdec:
	MOVL	(RARG), AX		/* expected value */
	MOVL	AX, BX
	ADDL	CX, BX			/* new value */
	LOCK; CMPXCHGL BX, (RARG)
	JNZ	aincdec
	MOVL	BX, AX
	RET
