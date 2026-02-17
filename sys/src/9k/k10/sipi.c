/*
 * Start-up InterProcessor Interrupt
 * boot CPUs other than 0.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "apic.h"
#include "sipi.h"

extern void squidboy(int);

void
setsipihandler(uintmem sipipa)
{
	/* set bios's warm (unaligned) start address to sipi handler */
	leputl((uchar *)WARMRESET, sipipa);
	/* will need to undo this nvram change after receiving the IPI */
	nvramwrite(Cmosreset, Rstwarm);
	coherence();
}

#ifdef unused
void
sendinitipi(int apicno, uintmem sipipa)
{
	setsipihandler(sipipa);
	apicinitipi(apicno);		/* send INIT sipi to self */
	coherence();
}
#endif

/*
 * Allocate and minimally populate new page tables & Mach for the cpu at apicno.
 *
 * The data needed per-processor is the sum of the stack, page
 * table pages, vsvm page and the Mach page.  The layout (Syspercpu) is
 * similar to that described in dat.h for the bootstrap processor
 * (Sys), but with any unused space elided; see l32p.s, dat.h.
 *
 * NOTE: for now, share the page tables with the
 * bootstrap processor, until this code is worked out,
 * so only the Mach and stack portions are used below.
 */
static Mach *
newcpupages(int apicno, Apic *apic, Sipi *sipi)
{
	Mach *mach;
	Syspercpu *nsys;

	DBG("allocating Sys pages for cpu%d...", apic->machno);
	nsys = mallocalign(sizeof(Syspercpu), PGSZ, 0, 0);
	if(nsys == nil) {
		print("no memory for cpu%d\n", apic->machno);
		return nil;
	}

	/* establish new stack */
	sipi->pml4 = cr3get();		/* save current pml4 root */
	/* stack must immediately precede pml4.  see dat.h. */
	sipi->stack = (uintptr)nsys->pml4;

	/*
	 * Committed. If the AP startup fails, can't safely
	 * release the resources, who knows what mischief
	 * the AP is up to. Perhaps should try to put it
	 * back into the INIT state?
	 */
	mach = &nsys->mach;
	sipi->mach = mach;
	mach->machno = apic->machno;		/* NOT one-to-one... */
	sipi->pc = mach->splpc = (uintptr)squidboy;
	mach->apicno = apicno;
	mach->stack = (uintptr)nsys;
	mach->vsvm = nsys->vsvmpage;
	mach->pml4 = m->pml4;
	return mach;
}

#ifdef SIPI_REBOOT
void
popcpu0sipi(Sipireboot *rebootargs, Sipi *sipi, void *tramp, void *physentry,
	void *src, int len)
{
	rebootargs->tramp = tramp;
	rebootargs->physentry = physentry;
	rebootargs->src = src;
	rebootargs->len = len;
	sipi->args = (uintptr)rebootargs;
	sipi->pml4 = cr3get();		/* save current pml4 root */
	sipi->mach = sys->machptr[0];
	sipi->stack = PADDR(sys->machptr[0]->pml4);
	sipi->pc = PADDR((void *)SIPIHANDLER);
}
#endif

/*
 * warm start the cpu for apicno at sipipa, and wait for it.
 * The Universal Startup Algorithm described in the MP Spec. 1.4.
 */
static void
wakecpu(int apicno, Mach *mach, uintmem sipipa)
{
	int i, machno;

	DBG("waking cpu%d, apic %d\n", mach->machno, apicno);
	apicsipi(apicno, sipipa); /* send the IPI to warm-start it */
	coherence();

	/* wait for victim cpu to start and acknowledge */
	DBG("waiting for cpu%d\n", mach->machno);
	for (i = 5000; i > 0 && mach->splpc != 0; i--)
		millidelay(1);
	machno = mach->machno;
	if (mach->splpc != 0)
		print("cpu%d didn't start\n", machno);
	else {
		DBG("apicno%d: machno %d mach %#p (%#p) %dMHz\n", apicno,
			machno, mach, sys->machptr[machno], mach->cpumhz);
		print("%d ", machno);
	}
}

Sipi *sipis;
// Sipi *cpu0sipi;

static uintmem sipipa;

static void
tryapicno(int apicno)
{
	Apic *apic;
	Mach *mach;

	apic = &xapic[apicno];
	if(apic->machno == 0)
		return;
	if (apicno == m->apicno) {
		print("sipi: apic %d for cpu%d is mine!\n",
			apicno, apic->machno);
		return;
	}

	DBG("sipi: apic %#p useable %d addr %#p machno %d...",
		apic, apic->useable, apic->addr, apic->machno);
	if(apicavail(apic) &&
	    (mach = newcpupages(apicno, apic, &sipis[apicno])) != nil)
		wakecpu(apicno, mach, sipipa);
	delay(50);
}

/* start secondary cpus via ipi */
void
sipi(void)
{
	int apicno;
	Mpl pl;

	if (MACHMAX == 1)
		return;			/* no other cpus; avoid nvram wear */

	/*
	 * Copy the startup code into place,
	 * it must be aligned properly.
	 */
	sipipa = mmuphysaddr(SIPIHANDLER);
	if(sipipa & (PGSZ - 1) || sipipa > 1*MB - 2*PGSZ) {
		print("sipi: misaligned code PA %#p\n", sipipa);
		return;
	}
	memmove((uchar *)SIPIHANDLER, sipihandler, sizeof(sipihandler));

	/* Sipi array fits in 2 pages for Napic <= 256 */
	sipis = (Sipi *)(SIPIHANDLER + PGSZ);
	memset(sipis, 0, sizeof(Sipi)*Napic);
//	cpu0sipi = &sipis[sys->machptr[0]->apicno];

	setsipihandler(sipipa);
	wbinvd();		/* ensure all cpus see SIPIHANDLER */
	delay(50);

	/* start all cpus other than 0 */
	pl = splhi();		/* defer intrs if we interrupt ourself */
	for(apicno = 0; apicno < Napic; apicno++)
		tryapicno(apicno);

	/* revert to normal power-on startup */
	nvramwrite(Cmosreset, Rstpwron);
	splx(pl);
}
