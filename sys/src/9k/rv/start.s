/*
 * minimal risc-v assembly-language initialisation to enable
 * calling low() in C.  this is the first kernel code executed.
 */
#include "mem.h"
#include "riscv64l.h"
#include "start.h"

#define Z(n)	MOV R0, R(n)

#define ALIGNPAT 0x01020304

	/* data segment, not bss, variables, due to initialisation */
	GLOBL	datalign(SB), $4
	DATA	datalign(SB)/4, $ALIGNPAT

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

	MOV	$PAUart0, R(UART0)	/* now safe to print on PAUart0 */

	MOV	$4, R9
	SLL	$31, R9			/* must be valid rv32 shift */
	BEQ	R9, rv32 /* shifted off left end? hart in 32-bit mode, park */

	/*
	 * a misaligned data segment can behave quite strangely,
	 * so detect and report one if found.
	 */
	MOVWU	datalign(SB), R12
	MOV	$ALIGNPAT, R9
	BNE	R9, R12, unaligned

	/* assume super mode by default */
	MOV	$'S', R12
	MOV	R0, R(MACHMODE)

	/*
	 * try to catch a trap if we access M mode CSRs in S mode.
	 * this relies on M mode delegating or forwarding this
	 * type of exception (illegal instruction?) to S mode.
	 * if SBI throws a fit, we're out of luck.
	 */
	MOV	$dummymach(SB), R9
	MOV	R9, CSR(SSCRATCH)	/* m for early strap */
	MOV	$supertrap(SB), R9
	MOV	R9, CSR(STVEC)
	MOV	R9, CSR(MTVEC)
	FENCE

	/*
	 * we didn't fault on MTVEC, so we're in M mode.  set it up minimally.
	 */
	MOV	$'M', R12
	MOV	$1, R(MACHMODE)
	MOV	CSR(MHARTID), R(HARTID)
	/* device tree pointer somewhere? */

	MOV	$Defmsts, R(TMP)
	MOV	R(TMP), CSR(MSTATUS)
	CSRRC	CSR(MSTATUS), $(Sie|Mie), R0
	MOV	R0, CSR(MIE)
	MOV	R0, CSR(MIP)
	MOV	R0, CSR(MEDELEG)
	MOV	R0, CSR(MIDELEG)

	MOV	$recktrap(SB), R9	/* catch early stray M faults */
	CSRRW	CSR(MTVEC), R9, R9
	MOV	R9, origmtvec(SB)	/* stash initial mtvec for later */

	MOV	$dummymach(SB), R9
	MOV	R9, CSR(MSCRATCH)	/* m for early mtrap */

TEXT supertrap(SB), 1, $-4
	/* if we faulted, we're in S mode */
	/* interrupts are now definitely off, in M or S mode */
	MOVW	R(MACHMODE), bootmachmode(SB)
	CONSPUT(R12)
	CONSPUT($' ')

	MOV	$recktrap(SB), R9	/* catch early stray S faults */
	MOV	R9, CSR(STVEC)

	MOV	$dummysc(SB), R12
	SCW(0, 12, 0)	/* discharge any lingering reservation we hold */

	MOV	$HARTMAX, R(TMP)
	BGEU	R(TMP), R(HARTID), nostack	/* more harts than expected? */

	/*
	 * zero most registers to avoid possible non-determinacy.
	 * R2 is stack pointer, R3 is static base,
	 * R29 is UART0, R30 is MACHMODE, R31 is HARTID.
	 */
	Z(1); Z(4); Z(5); Z(6); Z(7); Z(8); Z(9); Z(10); Z(11); Z(12); Z(13)
	Z(14); Z(15); Z(16); Z(17); Z(18); Z(19); Z(20); Z(21); Z(22); Z(23)
	Z(24); Z(25); Z(26); Z(27); Z(28)

	/*
	 * in case i(un)lock are called before m is set to its real Mach*,
	 * perhaps called via panic very early on this hart.
	 */
	MOV	$dummymach(SB), R(MACH)
	MOV	R0, R(USER)

	/* save PC as approx. PADDR(KTZERO) for mainpc */
	JAL	R12, 1(PC)
	MOV	R12, mainpc(SB)

	/*
	 * assign machnos sequentially from zero.
	 * after Amoadd: old hartcnt in MACHNO, updated hartcnt in memory.
	 */
	CONSPUT($'C')
	MOV	$hartcnt(SB), R9
	MOV	$1, R10
	AMOW(Amoadd, AQ|RL, 10, 9, MACHNO)

	MOV	$MACHMAX, R(TMP)
	BGEU	R(TMP), R(MACHNO), nostack	/* more cpus than expected? */

	/*
	 * set up a temporary stack for C for this cpu, based on machno.
	 * initstks is in the data segment, so won't be zeroed when zeroing bss.
	 */
	CONSPUT($'T')
	MOV	$'0', R15
	ADD	R(MACHNO), R15
	CONSPUT(R15)

	/* put sp within stack with SBIALIGN */
	MOV	$initstks+(INITSTKSIZE-SBIALIGN)(SB), R(TMP)
	MOV	$INITSTKSIZE, R11
	MUL	R(MACHNO), R11
	ADD	R11, R(TMP), R2		/* just past my init stack */

	BNE	R(MACHNO), notzero

	/* we are cpu0, so zero bss */
	CONSPUT($'Z')
	MOV	$edata(SB), R(TMP)
	MOV	$end(SB), R(TMP2)
zerobss:
	MOV	R0, (R(TMP))
	ADD	$XLEN, R(TMP)
	BLTU	R(TMP2), R(TMP), zerobss
	FENCE
 	MOVW	R0, initstall(SB)	/* all-clear for secondary cpus */
	JMP	allcpus

	/*
	 * we are a secondary, so wait here until cpu0 finishes zeroing bss
	 * and other initialisation.
	 */
notzero:
	PAUSE
	FENCE
	MOVW	initstall(SB), R(TMP)
	BNE	R(TMP), notzero
	/*
	 * all is clear for secondaries; add a slight delay to give cpu0 a head
	 * start and stagger cpu starts, in case of near-simultaneous start up.
	 */
	MOV	$(100*MHZ), R(TMP)
	MUL	R(MACHNO), R(TMP)
delay:
	SUB	$1, R(TMP)
	BNE	R(TMP), delay

allcpus:
	/* BUG: shouldn't need this but can't pass arg to low() */
	/* store hart id in hartids[machno] for Mach->hartid */
	MOV	$hartids(SB), R12
	MOV	$2, R13			/* sizeof(short) */
	MUL	R(MACHNO), R13
	ADD	R13, R12
	MOVH	R(HARTID), (R12)	/* hartids[machno] = HARTID */
	FENCE

	CONSPUT($'\r')
	CONSPUT($'\n')
	SUB	$16, R2			/* room to push low's args */
	MOVW	R(MACHNO), bootingcpu(SB)
	MOV	R(MACHNO), R(ARG)
	JAL	LINK, low(SB)		/* low(machno, hartid); no return */

	/*
	 * failures of various sorts
	 */

	PRINT($'?'); PRINT($'r'); PRINT($'e'); PRINT($'t')
	JMP	crlfwfi

nostack:
	PRINT($'?'); PRINT($'n'); PRINT($'s'); PRINT($'t')
	MOV	$'0', R15
	ADD	R(MACHNO), R15
	PRINT(R15)
	JMP	crlfwfi

rv32:
	PRINT($'?'); PRINT($'3'); PRINT($'2'); PRINT($'b')
	PRINT($'i'); PRINT($'t'); PRINT($' ')
	MOV	$'0', R15
	ADD	R(HARTID), R15
	PRINT(R15)
	JMP	crlfwfi

unaligned:
	PRINT($'D'); PRINT($'a'); PRINT($'t'); PRINT($'a')
	PRINT($' '); PRINT($'s'); PRINT($'e'); PRINT($'g')
	PRINT($' '); PRINT($'m'); PRINT($'i'); PRINT($'s')
	PRINT($'a'); PRINT($'l'); PRINT($'i'); PRINT($'g')
	PRINT($'n'); PRINT($'e'); PRINT($'d'); PRINT($'!')
crlfwfi:
	PRINT($'\r'); PRINT($'\n')

TEXT wfi(SB), 1, $-4
	WFI			/* may pause the core cycle counter */
	JMP	-1(PC)
