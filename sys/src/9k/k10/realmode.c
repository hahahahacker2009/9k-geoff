/*
 * #P/realmode*, to let vga muck with low memory & issue real-mode bios calls
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "tos.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

#include "amd64.h"
#include "/386/include/ureg.h"

#define LORMBUF (RMBUF-KZERO)

enum { CF = 1, };		/* carry flag: indicates error in bios call */

extern void realmode0(void);	/* in l64real.s */

static Ureg rmu;
static Lock rmlock;

/*
 * Back the processor into real mode to run a BIOS call,
 * then return.  This must be used carefully, since it
 * completely disables hardware interrupts (i.e., the lapic)
 * while running.  It is *not* using VM86 mode.
 * Maybe that's really the right answer, but real mode
 * is fine for now.  We don't expect to use this very much --
 * just for VGA and APM.
 */
void
realmode(Ureg *ureg)
{
	int s;
	Page *pml4;
	PTE oldpte0;
	PTE *pml4pte, *l3pte0;
	static PTE *l2tbl;

	if(getconf("*norealmode"))
		return;

	lock(&rmlock);
	*(Ureg*)RMUADDR = *ureg;	/* copy in bios call arguments */

	/*
	 * copy low l*.s files so that it can be run from 16-bit mode.
	 * the RMCODE page is writable because it's between KZERO and KTZERO.
	 */
	memmove((void*)RMCODE, (void*)KTZERO, BY2PG);

	/* allocate private l2 table */
	if (l2tbl == nil)
		l2tbl = mallocalign(PTSZ, PTSZ, 0, 0);

	s = splhi();
	lapicintroff();

	/* change this cpu's top-level low mapping */
	pml4 = m->pml4;
	pml4pte = (PTE *)pml4->va;
	l3pte0 = &pml4pte[PTLX(0, Npglvls-1)];
	oldpte0 = *l3pte0;

	/* new map covers first 1 GB */
	l2tbl[PTLX(0, Npglvls-2)] = 0 | PtePS|PteRW|PteP;
	/* set low identity map at top level */
	*l3pte0 = PADDR(l2tbl) | PteRW | PteP;
	coherence();
	cr3put(pml4->pa);

	realmode0();			/* issue bios call in real mode */
	splhi();			/* who knows what the bios did */

	*l3pte0 = oldpte0;
	coherence();
	cr3put(pml4->pa);

	/*
	 * Called from memory.c before initialization of mmu.
	 * Don't turn interrupts on before the kernel is ready!
	 */
	if (m->tss)
		lapicintron();
	splx(s);

	*ureg = *(Ureg*)RMUADDR;	/* copy results back */
	unlock(&rmlock);
}

static long
rtrapread(Chan*, void *a, long n, vlong off)
{
	if(off < 0)
		error(Ebadarg);
	if(n+off > sizeof rmu)
		n = sizeof rmu - off;
	if(n <= 0)
		return 0;
	memmove(a, (char*)&rmu+off, n);
	return n;
}

static long
rtrapwrite(Chan*, void *a, long n, vlong off)
{
	if(off || n != sizeof rmu)
		error("didn't write a Ureg at offset 0");
	memmove(&rmu, a, sizeof rmu);
	/*
	 * Sanity check
	 */
	if(rmu.trap == 0x10){	/* VBE */
		rmu.es = (LORMBUF>>4)&0xF000;
		rmu.di = LORMBUF&0xFFFF;
	} else
		error("invalid trap arguments");
	realmode(&rmu);
	return n;
}

static long
rmemrw(int isr, void *a, long n, vlong off)
{
	if(off < 0 || n < 0)
		error("bad offset/count");
	if(isr){
		if(off >= MB)
			return 0;
		if(off+n >= MB)
			n = MB - off;
		memmove(a, KADDR(off), n);
	} else {
		/* realmode buf page ok, allow vga framebuf's access */
		if(off >= MB || off+n > MB ||
		    (off < LORMBUF || off+n > LORMBUF+BY2PG) &&
		    (off < VGAMEM() || off+n > 0xB0000+0x10000))
			error("bad offset/count in write");
		memmove(KADDR(off), a, n);
	}
	return n;
}

static long
rmemread(Chan*, void *a, long n, vlong off)
{
	return rmemrw(1, a, n, off);
}

static long
rmemwrite(Chan*, void *a, long n, vlong off)
{
	return rmemrw(0, a, n, off);
}

/* only called at start-up */
static void
realmodediag(void)
{
	Ureg *ureg;
	static Ureg uregs;

	ureg = &uregs;
	memset(ureg, 0, sizeof *ureg);
	ureg->trap = 0x15;
	/* intel removed a20 gate in 2008 */
	ureg->ax = 0x2401;	/* harmless: enable A20 line */
	iprint("to real mode...");
	realmode(ureg);
	if (ureg->flags & CF)
		iprint("a20-on bios call (nop) failed\n");
	else
		iprint("and back.\n");
}

void
realmodelink(void)
{
	addarchfile("realmode", 0660, rtrapread, rtrapwrite);
	addarchfile("realmodemem", 0660, rmemread, rmemwrite);
	realmodediag();
}
