/*
 * k10 machine-language assist for real mode bios calls.
 */
#include "mem.h"
#include "amd64l.h"
#include "/sys/src/boot/pc/x16.h"

/* notice BYTE $0x15 different than l16sipi.s rLGDT */
#define rLGDT(gdtptr)	BYTE $0x0f;	/* LGDT */ \
			BYTE $0x01; BYTE $0x15; \
			LONG $gdtptr
#define ADDRSIZE BYTE $0x67

#define SAVEMSR(msr, saved) \
	MOVQ	$msr, CX; \
	RDMSR; \
	MOVL	AX, saved(SB); \
	MOVL	DX, saved+4(SB)
#define RESTMSR(msr, saved) \
	MOVQ	$msr, CX; \
	MOVL	saved(SB), AX; \
	MOVL	saved+4(SB), DX; \
	WRMSR

#define RELOC	(RMCODE-KTZERO)

	GLOBL	m0efer(SB), $8
	GLOBL	m0gsbase(SB), $8
	GLOBL	m0kgsbase(SB), $8
	GLOBL	m0gdt(SB), $10
	GLOBL	m0idt(SB), $10

TEXT _gdtptr32p<>(SB), 1, $-4
	WORD	$(4*8-1)			/* includes long mode */
	QUAD	$_gdt32p-KZERO(SB)		/* not LONG! see l64lme.s */

/* protected mode descriptors */
TEXT _gdt16<>(SB), 1, $-4
	QUAD	$0				/* NULL descriptor */
	QUAD	$(SdG|SdP|SdS|SdCODE|SdR|0xffff) /* CS compat */
	QUAD	$(SdG|SdP|SdS|SdW|0xffff)	/* DS compat */

TEXT _gdtptr16p<>(SB), 1, $-4
	WORD	$(3*8-1)
	QUAD	$_gdt16<>-KZERO(SB)

MODE $64

TEXT realmodeidtptr(SB), 1, $-4
	WORD	$(4*256-1)
	LONG	$0

/*
 * Assumed to be in long mode at time of call.
 * Switch to real mode, execute an interrupt, and
 * then switch back to long mode.
 *
 * Assumes:
 *	- no device interrupts are allowed
 *	- 0-16MB is identity mapped in page tables
 *	- realmode() has copied us down from KTZERO to 0x8000 (rmseg)
 *	- can use code segment 0x0800 in real mode to get at l.s code
 *	- l*.s code, notably l64real.s, is less than 1 page
 *	- we're called from C, so most general regs used have been saved
 *	- a bios call may trash arbitrary registers, but cannot access
 *	  R8 or above in real mode.  up and m are R14 and R15, so safe.
 */
TEXT realmode0(SB), $0
	CLI
	LEAQ	physcode-KZERO(SB), AX
	JMP	*AX
TEXT physcode(SB), $0
	/* switch to low stack */
	MOVQ	SP, AX
	MOVQ	$RMSTACK, SP
	PUSHQ	AX			/* save old sp on real-mode stack */

	/* in Virtualbox 7.2.4 r170995 storing GS base seems to be enough */
//	SAVEMSR(Efer, m0efer)
	SAVEMSR(GSbase, m0gsbase)
//	SAVEMSR(KernelGSbase, m0kgsbase)
	MOVQ	GDTR, m0gdt(SB)
	MOVQ	IDTR, m0idt(SB)

	/* change gdt to physical pointer */
	MOVQ	$_gdtptr32p<>-KZERO(SB), AX
	MOVL	(AX), GDTR

	/* jump to 32-bit code segment */
	PUSHQ	$SSEL(SiCS, SsTIGDT|SsRPL0)
	PUSHQ	$again32bit-KZERO(SB)
	RETFQ

MODE $32

/*
 * We're in long mode's 32-bit compatibility submode.
 */
TEXT again32bit(SB), 1, $-4
	MOVL	$SSEL(SiDS, SsTIGDT|SsRPL0), AX
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS
	MOVW	AX, SS
	DELAY
	MFENCE

	/* disable paging by zeroing CR0.PG (cpu will zero LMA bit). */
	MOVL	CR0, CX
	MOVL	$(Pg|Wp), AX
	NOTL	AX
	ANDL	AX, CX		/* clear Pg */
/**/	MOVL	CX, CR0		/* paging off */

	/*
	 * now behaving as 386 (32-bit).
	 *
	 * Intel says a jump is needed after disabling paging,
	 * but this one crashes, at least on AMD cpus.
	 *	JMP	clrprefq(SB)
	 */
	DELAY
	MFENCE

	/* deactivate long mode by zeroing efer.lme. */
	MOVL	$Efer, CX	/* Extended Feature Enable */
	RDMSR
	ANDL	$~Lme, AX	/* zero Long Mode Enable */
/**/	WRMSR

	/*
	 * we're now in legacy 32-bit protected mode.
	 */
	/* load GDT with 16-bit compatibility version */
	MOVL	$_gdtptr16p<>-KZERO(SB), AX
	MOVL	(AX), GDTR

	/* load IDT with real-mode version */
	MOVL	realmodeidtptr-KZERO(SB), IDTR

	/* edit INT $0x00 instruction below */
	MOVL	$(RMUADDR-KZERO+12*4), AX		/* &rmu.trap */
	MOVL	(AX), AX
	MOVB	AX, realmodeintrinst+(-KZERO+1+RELOC)(SB)

	/* paranoia: make sure modified INT & JMPFAR instr.s are seen below */
	MFENCE
	WBINVD

	/* jump to 16-bit code segment */
/*	JMPFAR	$SSEL(SiCS, SsTIGDT|SsRPL0):$again16bit(SB)	/**/
	 BYTE	$0xEA
	 LONG	$again16bit-KZERO(SB)
	 WORD	$SSEL(SiCS, SsTIGDT|SsRPL0)

MODE $16

TEXT again16bit(SB), $0
	/*
	 * Now in 16-bit compatibility mode.
	 * These are 32-bit instructions being interpreted
	 * as 16-bit instructions.  I'm being lazy and
	 * not using the macros because I know when
	 * the 16- and 32-bit instructions look the same
	 * or close enough.
	 */

	/* disable protected mode and jump to real mode cs */
	OPSIZE; MOVL CR0, AX
	OPSIZE; XORL BX, BX
	OPSIZE; INCL BX			/* set PE bit in BX */
	OPSIZE; XORL BX, AX
	OPSIZE; MOVL AX, CR0

	/* JMPFAR (RMCODE-KZERO)>>4:now16real */
	 BYTE	$0xEA
	 WORD	$now16real-KZERO(SB)
TEXT rmseg(SB), $0
	 WORD	$((RMCODE-KZERO)>>4)	/* modified by realmode() */

TEXT now16real(SB), $0
	/* load the low registers from Ureg at RMUADDR for the bios call */
	CLR(rAX)
	MTSR(rAX, rSS)
	LWI((RMUADDR-KZERO), rBP)

	/* offsets are in Ureg */
	LXW(44, xBP, rAX)
	MOVW	AX, DS
	LXW(40, xBP, rAX)
	MOVW	AX, ES

	OPSIZE; LXW(0, xBP, rDI)
	OPSIZE; LXW(4, xBP, rSI)
	OPSIZE; LXW(16, xBP, rBX)
	OPSIZE; LXW(20, xBP, rDX)
	OPSIZE; LXW(24, xBP, rCX)
	OPSIZE; LXW(28, xBP, rAX)

	CLC

/*
 * finally do the bios call, then unwind all this complexity back to long mode.
 */
TEXT realmodeintrinst(SB), $0
	INT $0x00	/* modified to insert INT # */
	CLI		/* who knows what evil the bios got up to */

	LWI((RMSTACK-BY2V), rSP)
	OPSIZE; PUSHFL
	OPSIZE; PUSHL AX

	/* store the low registers into Ureg at RMUADDR from the bios call */
	CLR(rAX)
	MOVW	AX, SS
	LWI((RMUADDR-KZERO), rBP)

	OPSIZE; SXW(rDI, 0, xBP)
	OPSIZE; SXW(rSI, 4, xBP)
	OPSIZE; SXW(rBX, 16, xBP)
	OPSIZE; SXW(rDX, 20, xBP)
	OPSIZE; SXW(rCX, 24, xBP)
	OPSIZE; POPL AX
	OPSIZE; SXW(rAX, 28, xBP)

	MOVW	DS, AX
	OPSIZE; SXW(rAX, 44, xBP)
	MOVW	ES, AX
	OPSIZE; SXW(rAX, 40, xBP)

	OPSIZE; POPL AX
	OPSIZE; SXW(rAX, 64, xBP)	/* flags */

	/* re-enter protected mode and jump to 32-bit code */
	OPSIZE; MOVL	CR0, AX
	OPSIZE; ORL	$Pe, AX
	OPSIZE; MOVL	AX, CR0

	ADDRSIZE; rLGDT(_gdtptr32p<>-KZERO(SB))	/* load a basic gdt */

/*	JMPFAR	$SSEL(SiCS, SsTIGDT|SsRPL0):$again64bit-KZERO(SB)	/**/
	 OPSIZE; BYTE $0xEA
	 LONG	$again64bit-KZERO(SB)
	 WORD	$SSEL(SiCS, SsTIGDT|SsRPL0)

MODE $32

/* we're in 32-bit mode, despite the label */
TEXT again64bit(SB), $0
	MOVL	CR4, AX
	ANDL	$~Pse, AX	/* Page Size */
	ORL	$(Pge|Pae), AX	/* Page Global, Phys. Address */
	MOVL	AX, CR4

	MOVL	$Efer, CX	/* Extended Feature Enable */
	RDMSR
	ORL	$Lme, AX	/* set Long Mode Enable */
	WRMSR

	MOVL	CR0, DX
	ORL	$(Pg|Wp), DX		/* Paging Enable */
/**/	MOVL	DX, CR0

	/* we're now in (64-bit) compatibility mode of long mode */

	/* can we restore original gdt here and skip again64phys? */
/*	JMPFAR	$SSEL(3, SsTIGDT|SsRPL0):$again64phys-KZERO(SB)		/**/
	 BYTE	$0xEA
	 LONG	$again64phys-KZERO(SB)
	 WORD	$SSEL(3, SsTIGDT|SsRPL0)

MODE $64

/* we're in 64-bit long mode */
TEXT again64phys(SB), 1, $-4
DBGPUT('I', looplmeI)
	MOVQ	m0gdt(SB), GDTR

	XORQ	DX, DX		/* DX is 0 from here on */
	MOVW	DX, DS		/* not used in long mode */
	MOVW	DX, ES		/* not used in long mode */
	MOVW	DX, FS
	MOVW	DX, GS
	MOVW	DX, SS		/* not used in long mode */

	PUSHQ	$SSEL(SiCS, SsTIGDT|SsRPL0)
	PUSHQ	$again64kzero(SB)
	RETFQ

#define SAVEDREGS 16

TEXT again64kzero(SB), $0
DBGPUT('v', looplmev)
	/* switch to old stack */
	PUSHQ	AX		/* match popq below for 6l */
	MOVQ	$(RMSTACK-BY2V), SP
	POPQ	SP		/* restore old sp from real-mode stack */

	MOVQ	m0idt(SB), IDTR
//	RESTMSR(Efer, m0efer)
	RESTMSR(GSbase, m0gsbase)
//	RESTMSR(KernelGSbase, m0kgsbase)

	/* 0(SP) should hold realmode0's return PC */
	RET
