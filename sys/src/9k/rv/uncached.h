/*
 * uncached memory access (riscv64 version)
 */

#define FLUSHSTRAT() (sys->ucstrat == Uncflush || sys->ucstrat == Uncview)

/* p is nil, maybe mapped into KZERO space, cached or uncached? */
#define ISKERNNIL(p) ((p) == nil || (uintptr)(p) == KZERO || \
	(void *)(p) == uncachedzero)

void *uncachedzero;
