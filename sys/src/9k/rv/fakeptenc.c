/*
 * stubs for Svpbmt PteNc support
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "riscv64.h"

void
ncflush(void)
{
}

void
ncinit(void)
{
	m->ptroot->daddr = Ptpgptes/2;
	mmuflushtlb(m->ptroot);		/* zero user mappings */
}

void
probesvpbmt(Sys *)
{
}

uintptr
ncspace(void)	/* return space for PteNc pages, if any.  exclude Sys */
{
	return 0;
}

void
setncbase(void)
{
}
