/*
 * this was an attempt to use eic7700's uncached view of ram.
 * aggressive prefetchers like the eic7700x's make the problem worse.
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"riscv64.h"
#include	"uncached.h"

void *uncachedk0;

void *
uncachedview(void *kv)
{
	uintptr p;

	if (uncachedk0 == 0)
		uncachedk0 = (char *)KADDR(0) - Cachedbase + soc.uncached;

	if (sys->ucstrat != Uncview || !ISCACHEDVIEW(kv))
		return kv;

	/* convert cached to uncached kernel address */
	p = (uintptr)kv;
	p = (iskern(kv)? p: (uintptr)KADDR(p));		/* make kernel */
	return (void *)(p - Cachedbase + soc.uncached);
}

void *
cachedview(void *kv)
{
	uintptr p;

	if (uncachedk0 == 0)
		uncachedk0 = (char *)KADDR(0) - Cachedbase + soc.uncached;

	if (sys->ucstrat != Uncview || ISCACHEDVIEW(kv))
		return kv;

	/* convert uncached to cached kernel address */
	p = (uintptr)kv;
	p = (iskern(kv)? p: (uintptr)KADDR(p));		/* make kernel */
	if (p < (uintptr)KADDR(soc.uncached))
		iprint("cachedview: p %#p < KADDR(soc.uncached) %#p\n",
			p, KADDR(soc.uncached));
	return (void *)(p - soc.uncached + Cachedbase);
}
