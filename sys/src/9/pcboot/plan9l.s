#include "mem.h"

/*
 * Treat syscalls as stray traps, since there shouldn't be any in a boot kernel.
 */
TEXT _syscalltype(SB), $0
	BYTE	$VectorSYSCALL

TEXT _syscallintr(SB), $0			/* called from ../pc/l.s */
	PUSHL	AX
	MOVL	$_syscalltype(SB), AX		/* address of trap type */
	/* intrcommon expects address of trap type in AX & saved AX on stack */
	JMP	intrcommon(SB)
