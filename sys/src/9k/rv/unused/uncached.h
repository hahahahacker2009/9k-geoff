/*
 * uncached memory access (riscv64 version)
 */

/* physical address of uncached dram view */
#define Cachedbase \
	(sys->ucstrat == Uncview? ROUNDDN(membanks[0].addr, 16*MB): 0)
/* is p a kernel uncached address? */
#define ISCACHEDVIEW(p) \
	(sys->ucstrat == Uncview && (p) < KADDR(soc.uncached))
/* p is nil, mapped into KZERO space, cached or uncached? */
#define ISKERNNIL(p) \
	((p) == nil || (p) == KADDR(0) || uncachedk0 && (p) == uncachedk0)

void *uncachedk0;
