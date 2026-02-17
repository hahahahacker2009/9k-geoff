TEXT _bsr(SB), $-4
	BSRQ	RARG, AX		/* return bit index of leftmost 1 bit */
	RET
