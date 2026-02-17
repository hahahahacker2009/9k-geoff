/*
 * fake pci 
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"

void
pcireset(void)
{
}

/*
 * config space for tbdf should be at (return address - rno).
 */
void *
defpcicfgaddr(int tbdf, int rno)
{
	/* f is shifted 8 in tbdf, needs to be by 16 */
	return soc.pci + (BUSBDF(tbdf)<<8) + rno;
}

void *(*pcicfgaddr)(int, int) = defpcicfgaddr;
