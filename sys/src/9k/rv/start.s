/*
 * minimal risc-v assembly-language initialisation to enable
 * calling low() in C.  this is the first kernel code executed.
 */
#include "mem.h"
#include "riscv64l.h"
#include "start.h"

#define Z(n)	MOV R0, R(n)

/* dedicated registers during start-up */
LOCK	= 12
UART0	= 13
TMP	= 14
TMP2	= 15
MACHMODE= 30
HARTID	= 31

	/* data segment, not bss, variables, due to initialisation */
	GLOBL	printlck(SB), $4
	DATA	printlck(SB)/4, $0

/*
 * this may be entered in machine or super mode.
 * stack pointer is unknown at entry, so use $-4 to not touch it
 * until we can establish a new stack.
 * First, disable all interrupts.
 *
 * N.B.: all cpus may be executing this code simultaneously.
 * also, if we're rebooting, the secondary cpus may be stalled on sys->secstall.
 */
TEXT _main(SB), 1, $-4			/* _main avoids libc's main9.s */
	SPLHI
	MOV	R0, CSR(SIE)
	MOV	R0, CSR(SIP)

	MOV	R10, R(HARTID)	/* save likely sbi hartid in a safe place */
	/* likewise, R11 may contain a device tree pointer from sbi */

	/*
	 * stop paging, if it's on.  we must be executing in the identity map
	 * (physical == virtual) for this to work, but that's likely on risc-v
	 * systems intended to run unix.  otherwise, traps to machine mode
	 * (with no virtual memory) could fault endlessly.
	 */
	LOADSATP(R0)			/* many fences here */

	/*
	 * Prepare the static base before use.
	 * SB will be in the physical (low) address range because the PC is
	 * (setting SB is an LUI off(PC) and an ADD).
	 * This eliminates the need for "-KZERO" in machine mode or
	 * when otherwise executing in low addresses, except when using
	 * addresses of static data not based on SB (e.g., an Rvarch struct).
	 */
	MOV	$setSB(SB), R3
	MOV	$Defssts, R(TMP)	/* prev S mode is user */
	MOV	R(TMP), CSR(SSTATUS)

	MOV	$panicstk+(INITSTKSIZE)(SB), R2	/* very temporary stack */
// TEXT pstkalign(SB), 1, $-4		/* reset SP, FP for new R2 */

	/*
	 * try to catch a trap if we access M mode CSRs in S mode.
	 * this relies on M mode delegating or forwarding this
	 * type of exception (illegal instruction?) to S mode.
	 * if SBI throws a fit, we're out of luck.
	 */
	MOV	$'S', R11		/* assume super mode by default */
	MOV	R0, R(MACHMODE)
	MOV	$supertrap(SB), R(TMP)
	MOV	R(TMP), CSR(STVEC)
	MOV	$recktrap(SB), R(TMP)	/* catch early stray M faults */
	CSRRW	CSR(MTVEC), R(TMP), R(TMP)
	MOV	R(TMP), origmtvec(SB)	/* stash initial mtvec for later */
	FENCE

	/* we didn't fault on MTVEC access, so we're in M mode. */
	JAL	LINK, mnointrs(SB)

TEXT supertrap(SB), 1, $-4
	/* if we faulted, we're in S mode */
	/* interrupts are now definitely off, in M or S mode */
	MOV	$recktrap(SB), R(TMP)	/* catch early stray S faults */
	MOV	R(TMP), CSR(STVEC)

	MOV	$PAUart0, R(UART0)	/* now safe to print on PAUart0 */
 	MOV	$printlck(SB), R(LOCK)
	MOVW	R(MACHMODE), bootmachmode(SB)
	CONSPUT(R11)

	/*
	 * zero most registers to avoid possible non-determinacy.
	 * R2 is stack pointer, R3 is static base, R6 is up, R12 is LOCK,
	 * R13 is UART0, R30 is MACHMODE, R31 is HARTID.
	 */
	Z(1); Z(4); Z(5); Z(6); Z(7); Z(8); Z(9); Z(10); Z(11); Z(14); Z(15)
	Z(16); Z(17); Z(18); Z(19); Z(20); Z(21); Z(22); Z(23); Z(24); Z(25)
	Z(26); Z(27); Z(28); Z(29)

	/*
	 * set up a temporary stack for C for this cpu.
	 * initstks is in the data segment, so won't be zeroed when zeroing bss.
	 */
	MOV	$initstkp(SB), R(TMP2)
	MOV	$INITSTKSIZE, R(TMP)
	/* after Amoadd: old initstkp in R2, updated initstkp in memory. */
	AMOD(Amoadd, AQ|RL, TMP, TMP2, 2)	/* end of my init stack */
	ENSURELOW(R2)			/* initstkp initialised to high addr */
	MOV	$initstks+(MACHMAX*INITSTKSIZE)(SB), R(TMP2)
	BGTU	R(TMP2), R2, nostack

	MOV	$4, R(TMP)
	SLL	$31, R(TMP)		/* must be valid rv32 shift */
	BEQ	R(TMP), rv32 /* shifted off left end? in 32-bit mode, park */

	MOV	R(HARTID), R(ARG)
	JAL	LINK, low(SB)		/* low(hartid); no return */

	PR($'?'); PR($'r'); PR($'e'); PR($'t')
	JMP	crlfwfi

/*
 * disable M interrupts and delegations.  uncommon case.
 */
TEXT mnointrs(SB), 1, $-4
	MOV	R0, CSR(MIE)
	MOV	R0, CSR(MIP)
	MOV	$Defmsts, R(TMP)
	MOV	R(TMP), CSR(MSTATUS)
	CSRRC	CSR(MSTATUS), $(Sie|Mie), R0
	MOV	R0, CSR(MEDELEG)
	MOV	R0, CSR(MIDELEG)

	MOV	$'M', R11
	MOV	$1, R(MACHMODE)
	MOV	CSR(MHARTID), R(HARTID)
	/* device tree pointer somewhere? */

	MOV	R(MACH), CSR(MSCRATCH)	/* m for early mtrap */
	RET

/*
 * failures of various sorts
 */

nostack:
	PR($'?'); PR($'n'); PR($'o'); PR($' '); PR($'s'); PR($'t')
	PR($'a'); PR($'c'); PR($'k')
	PR($' '); PR($'h'); PR($'a'); PR($'r'); PR($'t')
	MOV	R(HARTID), R(TMP2)
	JMP	digcrlfwfi
rv32:
	PR($'?'); PR($'3'); PR($'2'); PR($'b'); PR($'i'); PR($'t')
	PR($' '); PR($'h'); PR($'a'); PR($'r'); PR($'t')
	MOV	R(HARTID), R(TMP2)
digcrlfwfi:
	ADD	$'0', R(TMP2)
	PR(R(TMP2))
crlfwfi:
	PR($'\r'); PR($'\n')

TEXT wfi(SB), 1, $-4
	WFI			/* may pause the core cycle counter */
	JMP	-1(PC)
