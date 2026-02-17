/*
 * on DMA-incoherent systems, cache-line aligned malloc with padding at the end
 * to avoid the next cache line to prevent polluting it, or vice versa.
 * aggressive prefetchers like the eic7700x's make the problem worse.
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"riscv64.h"
#include	"uncached.h"

/* is p a kernel uncached address? */
#define ISCACHEDVIEW(p) (sys->ucstrat == Uncview && \
	(p) >= (void *)KZERO && (p) < KADDR(soc.uncached))

static char *strats[] = {
[Uncnone]	"none; dma is coherent, as god intended",
[Uncflush]	"flush cache lines constantly",
// [Uncptenc]	"use PteNc on a region",
[Uncview]	"use uncached view of ram and flush cache lines",
};

void	*uncachedzero;		/* conversion for ISKERNNIL() */

static uintptr	touncachedoff;	/* cached -> uncached difference */

/* choose a strategy for dealing with incoherent dma, if present */
void
chooseincoher(void)
{
	if (soc.uncached) {
		/* space between phys start of ram and uncached view */
		touncachedoff = soc.uncached - ROUNDDN(membanks[0].addr, 16*MB);
		uncachedzero = KADDR(soc.uncached);
		if (soc.newmach) {
			iprint("\tuncachedzero %#p\n", uncachedzero);
			iprint("\ttouncachedoff %#p\n", touncachedoff);
		}
	}

	if (!dmaincoherent)
		sys->ucstrat = Uncnone;
	else if (soc.uncached)
		sys->ucstrat = Uncview;
//	else if (BROKEN && soc.svpbmt)
//		sys->ucstrat = Uncptenc;	/* nice idea; isn't adequate */
	else
		sys->ucstrat = Uncflush;
	if (dmaincoherent)
		print("dma is incoherent (i.e., broken); ask the hardware "
			"designers to do better.\n  coping with strategy: %s\n",
			strats[sys->ucstrat]);
}

void *
dmamallocalign(uintptr size, uintptr align)
{
	int dmapad;
	char *p;

	dmapad = FLUSHSTRAT()? CACHELINESZ: 0;
	p = mallocalign(size + dmapad, align, 0, 0);
	if (p && dmapad)
		cachedwbinvse(p, size);
	return p;
}

/*
 * iff uncached mapping works, flush seems unneeded.  lacking
 * uncached mapping, jupiter got tcp errors or otherwise
 * misbehaved without the flush and padding.
 */
void *
dmamalloc(uintptr size)
{
	return dmamallocalign(size, CACHELINESZ);
}

/*
 * this uses eic7700's uncached view of ram.
 * aggressive prefetchers like the eic7700x's make the problem worse.
 */

/* convert uncached to cached kernel address; no-op for cached addrs */
void *
cachedview(void *uca)
{
	if (sys->ucstrat != Uncview)
		return uca;
	return !ISCACHEDVIEW(uca)? (char *)uca - touncachedoff: uca;
}

/* convert kernel cached to uncached address; no-op for uncached addrs */
void *
uncachedview(void *ca)
{
	return ISCACHEDVIEW(ca)? (char *)ca + touncachedoff: ca;
}
