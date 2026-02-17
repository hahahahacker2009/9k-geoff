/*
 * Svpbmt PteNc support
 *
 * aggressive prefetching and caching prevents this from actually working as
 * intended.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "riscv64.h"

// #include "../port/dbgprint.h"

/* clear caches of the region to be mapped PteNc */
void
ncflush(void)
{
	if (sys->ncbase)
		cachedwbinvse((void *)sys->ncbase, sys->ncend - sys->ncbase);
}

/*
 * do this early to minimise cache pollution via the nc region's low
 * mapping.
 */
void
ncinit(void)
{
	ncflush();
	m->ptroot->daddr = Ptpgptes/2;
	mmuflushtlb(m->ptroot);		/* zero user mappings */
	ncflush();
}

/*
 * map va to PADDR(va) at leaves, but another PT in non-leaves.
 * for next-level pointers, supply next page table in pa but use
 * the actual va being mapped.
 */
void
fillpt(PTE *ptep, int lvl, uintptr va, uintptr pa, PTE ptebits, int ptes)
{
	int idx, cnt = 0;

	va &= ~m->pgszmask[lvl];	/* round down to nearest (super)page */
	if (ptebits & PteLeaf)
		pa = va;
	/* else target (pa) is another page table */

	pa = (uintptr)ensurelow(pa);
//	iprint("fillpt: ptebits %#p: va %#p -> pa %#p\n", ptebits, va, pa);
	ptebits |= PADDRFORPTE(pa) | PteP;
	idx = PTLX(va, lvl);
	if (ptes > 1)
		idx = 0;
	if (idx < 0 || idx >= Ptpgptes || idx+ptes-1 >= Ptpgptes)
		panic("fillpt: va %#p lvl %d idx %d ptes %d",
			va, lvl,idx, ptes);
	if (TODO && cnt-- > 0)
		iprint("fillpt: L%d pte %#p idx %d: va %#p -> pa %#p (%d) %s\n",
			lvl, ptep, idx, va, pa, ptes,
			ptebits&PteLeaf? "": "(pt)");
	USED(cnt);
	setptes(&ptep[idx], ptebits, ptes, lvl);
}

enum {
	Pattern = 0x12345678,
};

static uintptr testva;

static int
gotnc(void *aptep)
{
	int r;
	uintptr junk, nsatp;
	PTE *ptep;
	Mpl pl;

	pl = splhi();
	/* enable normal map to include stvec */
	if (normalmap() < 0)
		panic("putsatp faulted on %#p", normalsatp);

	ptep = aptep;

	/* clear any cached lines in nc region */
	cachedwbinvse((void *)testva, CACHELINESZ);
	cachedwbinvse((void *)PADDR((void *)testva), CACHELINESZ);
	nsatp = pagingmode | ((uintptr)ensurelow(ptep) / PGSZ);
	if (soc.lowdebug)
		iprint("gotnc: switch to test page tbl: writing satp %#p\n",
			nsatp);
	/*
	 * putsatp may fault.  wedges on vf2; perhaps SBI intercepts the fault?
	 * works on temu, jupiter.
	 */
	if (putsatp(nsatp) < 0)
		panic("failed to set satp %#p\n", getsatp());
	if (soc.lowdebug)
		iprint("deref %#p...", testva);
	junk = Pattern;
	USED(junk);
	junk = *(ulong *)testva;	/* should fault if no svpbmt */
	coherence();
	r = junk != Pattern;
	normalmap();
	splx(pl);
	return r;
}

/* called with paging enabled, from main */
void
probesvpbmt(Sys *sys)
{
	Mpl pl;

	pl = splhi();
	normalmap();
	testva = KTZERO + 8*MB;
	poppt2lvl1(sys->ncpt, testva);

	/* map 2MB leaf around testva with Nc attribute */
	testva &= ~m->pgszmask[1];
	fillpt(sys->ncpt[1], 1, testva, (uintptr)ensurelow(testva),
		PteRWX|Pteleafvalid | PteNc, 1);
	coherence();

	soc.svpbmt = haveinstr(gotnc, (uintptr)sys->ncpt[Toplvl]);
	normalmap();			/* back to normal from gotnc */
	/* remove PteNc */
	fillpt(sys->ncpt[1], 1, testva, (uintptr)ensurelow(testva),
		PteRWX|Pteleafvalid, 1);
	if (1 || soc.newmach) {		// TODO
		iprint("mmu: ");
		if (!soc.svpbmt)
			iprint("don't ");
		iprint("have svpbmt\n");
	}
	coherence();
	if (soc.svpbmt)
		setupncpt(sys);
	splx(pl);
}

uintptr
ncspace(void)	/* return space for PteNc pages, if any.  exclude Sys */
{
	uintptr pmsize, space;

	if (!soc.svpbmt)
		return 0;
	if (uncspace)
		return uncspace;
	space = kernmem;
	if (space == 0) {		/* kernmem is 0 here, estimate it */
		pmsize = membanks[0].size;
		if (pmsize < Minmb*MB)	/* sanity: enforce minimum ram */
			pmsize = Minmb*MB;
		space = 128*MB;		/* arbitrary */
		if (space >= pmsize/Kernfract)
			space = pmsize/Kernfract;
		if (space < Minmb*MB/3)
			space = Minmb*MB/3;
		space = MIN(space, Kernmax);
	}
	space = ROUNDUP(space, PGLSZ(1));		/* to fit L1 PTEs */
	iprint("memory mapped uncached (PteNc) %N\n", space);
	return space;
}

void
setncbase(void)
{
	uintptr sysbase;

	if (sys->ncbase == 0) {
		if (uncspace == 0)
			uncspace = ncspace();
		sys->ncend = sysbase = ROUNDDN((uintptr)sys, PGLSZ(1));
		sys->ncbase = sysbase - uncspace;
	}
}

/*
 * called with paging enabled, from main.  cpu0 only should run this.
 * secondaries will pick up the nc pt.
 */
void
setupncpt(Sys *sys)
{
	uintptr ncva, addr, nsatp;
	Mpl s;
	PTE *ptep;

	s = splhi();
	DBG("setupncpt %d\n", m->machno);
	normalmap();
	setncbase();
	ncva = sys->ncbase;
	poppt2lvl1(sys->ncpt, ncva);

	/* the whole region fits under sys, so in 1GB */
	ncva &= ~m->pgszmask[1];
	iprint("mmu: mapping %#p-%#p with PteNc\n", ncva, sys->ncend);
	for (addr = ncva; addr < sys->ncend; addr += PGLSZ(1))
		/* map 2MB leaf around addr with Nc attribute */
		fillpt(sys->ncpt[1], 1, addr, (uintptr)ensurelow(addr),
			PteRWX|Pteleafvalid | PteNc, 1);

	ptep = sys->ncpt[Toplvl];
	if (soc.lowdebug && m->machno == 0) {
		// mmudump((uintptr)ptep, Toplvl);
		iprint("va %#p -> pa %#p\n", 0ULL, mmutrans(ptep, 0ULL));
		iprint("va %#p -> pa %#p\n", (uintptr)PHYSMEM,
			mmutrans(ptep, PHYSMEM));
		iprint("va %#p -> pa %#p\n", sys->ncbase,
			mmutrans(ptep, sys->ncbase));
		iprint("va %#p -> pa %#p\n", KTZERO,
			mmutrans(ptep, KTZERO));
		iprint("va %#p -> pa %#p\n", testva,
			mmutrans(ptep, testva));
	}

	/* make ncpt the production page table */
	nsatp = pagingmode | ((uintptr)ensurelow(sys->ncpt[Toplvl]) / PGSZ);
	DBG("mmu: switching to PteNc page table %#p...", nsatp);

	wbinvd();
	ncflush();
	cachedwbinvse(ensurelow(sys->ncbase), sys->ncend - sys->ncbase);
	wbinvd();

	putsatp(nsatp);
	sys->satp = normalsatp = nsatp;
	ncflush();
	splx(s);
	DBG("\n");
}

/*
 * thispt at level "lvl" will point into nextpt with an ultimate target of va.
 * populate next level PT with identity map.
 */
static void
poppt(Ptepage ptarr[], int lvl, uintptr va)
{
	PTE *nextpt;

	assert(lvl > 0);
	nextpt = ptarr[lvl-1];
	/* don't map low va, even at top level, to avoid creating an nc alias */
	fillpt(ptarr[lvl], lvl, va, (uintptr)nextpt, PtePtr, 1);

	/*
	 * populate the next level (nextpt) with leaves.
	 * it seems that lvl should be decremented here, not below,
	 * but this way works and that way doesn't.
	 */
	va &= ~m->pgszmask[lvl];
	fillpt(nextpt, lvl-1, va, (uintptr)ensurelow(va),
		PteRWX | Pteleafvalid, Ptpgptes);
}

void
poppt2lvl1(Ptepage ptarr[], uintptr va)
{
	/* dualmap has been established in initpt. */
	memmove(ptarr[Toplvl], sys->initpt, PTSZ); /* copy normal top-level pt */

	switch (pagingmode) {
	case Sv64:		/* there's no way to test this yet */
		/* 16EB at 0, KZERO. populate L4 (128PB) with 256TB leaves. */
		/* 64-57 is just 7, not the usual 9 shift. */
		poppt(ptarr, 5, va);
		/* fall through */
	case Sv57:		/* there's no way to test this yet */
		/* 128PB at 0, KZERO. populate L3 (256TB) with 512GB leaves. */
		poppt(ptarr, 4, va);
		/* fall through */
	case Sv48:
		/* 256TB at 0, KZERO. populate L2 (512GB) with 1GB leaves. */
		poppt(ptarr, 3, va);
		/* fall through */
	case Sv39:
		/* 512GB at 0. populate L1 (1GB) with 2MB super-page leaves. */
		poppt(ptarr, 2, va);
		break;
	default:
		panic("pagingmode %#p not supported", pagingmode);
	}
}

void
ncsetup(void)
{
	if (m->machno == 0) {
		ncflush();
		delay(200);
	}
	ncflush();
	if (soc.svpbmt) {
		m->ncmapped = 1;
		if (m->machno == 0)
			/* disables cache ops in nc region */
			sys->ucstrat = Uncptenc;
	}
}
