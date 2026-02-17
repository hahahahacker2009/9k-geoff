/*
 * Trap catcher for machine mode of RV64.
 *
 * There is no paging in machine mode, so this handler, at least,
 * must be in identity-mapped memory.
 *
 * Note well that the exact stack layout of a trap is known in various other
 * places too (e.g., main.c, mmu.c, trap.c).  The intended layout is: a Ureg
 * struct at the very end of up->kstack if trapped from user mode, or
 * somewhere on m->stack if trapped from kernel, with a pointer to that Ureg
 * just before (below) it, for the argument to trap().
 *
 * All interrupts and most exceptions are delegated, if the system allows.
 * so we should at most get timer, IPI and external interrupts,
 * and environment calls from super in machine mode.
 */

#include "mem.h"
#include "riscv64l.h"

/* registers */
RSYS=29
TMP=30
UART0=31

	GLOBL	bootmachmode(SB), $4	/* flag: boot in machine mode */
	DATA	bootmachmode(SB)/4, $1	/* is machine; init to avoid bss */

/*
 * cope with machine-mode faults, from user or kernel mode.
 * the trap mechanism saved the return address in CSR(MEPC) and masked
 * interrupts.  we could be interrupting supervisor-mode fault handling.
 * entered with PC in low address range with no paging.
 */
TEXT mtrap(SB), 1, $-4
	CSRRW	CSR(MSCRATCH), R(MACH), R(MACH)	/* save old m, load new m */
	BNE	R(MACH), ok
	MOV	$setSB(SB), R3
	JAL	R0, recmtrapalign(SB)
ok:
	FENCE

	/* stash a few regs in m->regsave to free working registers */
	MOV	R2, SAVEMR2(R(MACH))	/* save SP */
	MOV	R3, SAVEMR3(R(MACH))	/* save SB */
	MOV	R4, SAVEMR4(R(MACH))	/* save j[acl] temporary */
	MOV	R6, SAVEMR6(R(MACH))	/* save R6 up */
	MOV	R9, SAVEMR9(R(MACH))

	MOV	$setSB(SB), R3	/* set SB before MOVing 64-bit constants */
	ENSURELOW(R3)			/* believed optional due to low PC */

	/* discharge any reservation we interrupted so we can use LR/SC */
	MOV	$dummysc(SB), R9
	SCW(0, 9, 0)

	MOV	CSR(MCAUSE), R6
	BGE	R6, except		/* sign bit is Rv64intr */

	/*
	 * fast path: punt M interrupts to supervisor mode.
	 * this should run to MRET without interruption.
	 */
intr:
	MOV	CSR(MIP), R2		/* M intrs pending */
	AND	$Machie, R2		/* isolate usual mach intr bits */

	/* either clock interrupt? */
	ADDW	R0, R6			/* clear at least Rv64intr bit */
	AND	$~Msdiff, R6		/* convert M intr to S intr, if any */
	MOV	$Suptmrintr, R9
	BNE	R9, R6, notclock

	/* clock interrupt: extinguish source */
	/* assumes a standard clint, notably mtimecmp registers */
	MOV	MTIMECMP(R(MACH)), R9
	BEQ	R9, notclock
	/* cater to e.g., xuantie, with 32-bit clint stores */
	MOV	$VMASK(31), R6
	MOVW	R6, 4(R9)		/* extinguish timer intr source: hi */
	FENCE
	MOV	$1, R9
	MOVW	R9, ticktock(SB)	/* for sanity checking */
notclock:
	/* clear MIP *ie bits to dismiss (but they are mostly readonly) */
	CSRRC	CSR(MIP), R2, R0
	SRL	$2, R2
	CSRRS	CSR(SIP), R2, R0  /* set corr. SIP bits to punt to super */
	MOV	$(VMASK(Nlintr-Local0intr)<<Local0intr), R6 /* local intrs */
	CSRRC	CSR(MIP), R6, R0
restmachret:
	MOV	SAVEMR9(R(MACH)), R9
	MOV	SAVEMR6(R(MACH)), R6	/* R6 up */
	MOV	SAVEMR4(R(MACH)), R4	/* R4 temporary */
	MOV	SAVEMR3(R(MACH)), R3	/* SB */
	MOV	SAVEMR2(R(MACH)), R2	/* SP */
	/* the corresponding super intr is awaiting MRET */
swapmachret:
	CSRRW	CSR(MSCRATCH), R(MACH), R(MACH)	/* restore m, load saved m */
	FENCE
	MRET

	/*
	 * undelegated M exception from kernel (super) or user mode.  should
	 * only happen rarely because exceptions are almost all delegated
	 * directly to supervisor mode, except for Envcallsup, which we
	 * explicitly did not delegate.
	 */
except:
	/* R6 has pure MCAUSE with Rv64intr off */
	MOV	CSR(MSTATUS), R2
	MOV	$Mpp, R9
	AND	R9, R2
	BEQ	R2, mfromuser		/* 0 == Mppuser */

	/*
	 * M exception from kernel mode: R2 (SP), SB, USER, MACH were all okay,
	 * but we stepped on MACH (a no-op) and SP above.
	 */
	MOV	SAVEMR2(R(MACH)), R2	/* SP */
	MOV	SAVEMR3(R(MACH)), R3	/* SB */

	MOV	$Envcallsup, R9
	BNE	R9, R6, mfromuser

/*
 * we're here as the result of an ecall from supervisor mode (M mode
 * exception), which triggers a reboot by calling the reboot trampoline code.
 */
	ENSURELOW(R3)			/* correct SB for low id map */
	MOV	origmtvec(SB), R31	/* restore mtvec in case of sbi */
	MOV	R31, CSR(MTVEC)

	MOV	sys(SB), R(RSYS)
	ENSURELOW(R(RSYS))		/* sys physical */
	MOV	$LOW0SYSPAGE(R(RSYS)), R14	/* &sys->Reboot physical */

	MOV	(R14), R15		/* tramp address */
	ENSURELOW(R15)			/* tramp physical */

	MOV	R14, R(ARG)
	MOV	$PAUart0, R(UART0)
	CONSPUT($'\n')

	JMP	(R15)			/* sys->tramp(&sys->Reboot) */

	/* kernel exception other than Envcallsup */
mfromuser:
	MOV	$PAUart0, R(UART0)
	CONSPUT($'\n')
	CONSPUT($'?')
	CONSPUT($'M')
	CONSPUT($'E')
	ADD	$'0', R6, R9
	CONSPUT(R9)
	JAL	R0, wfi(SB)		/* JMP only takes local labels */
