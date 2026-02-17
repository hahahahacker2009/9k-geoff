/*
 * page coloring for numa (core clustering)
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

/* typically 4 & 4 */
uint harts_per_cluster = 8, gbs_per_cluster = 8;	/* 4gb jupiter */

/*
 * Return a number identifying a color for the memory at
 * the given address (color identifies locality) and fill *sizep
 * with the size for memory of the same color starting at addr.
 * used by page.c.
 *
 * on risc-v, assume fixed number of GBs per cluster of harts,
 * so color = gb / gbs_per_cluster.
 */
int
memcolor(uintmem addr, uintmem *sizep)
{
	uintmem below;

	/* TODO: iterate through membanks to work out *sizep */
	below = addr - sys->pmbase;
	if ((vlong)below < 0)
		below = 0;
	if (sizep) {
		*sizep = sys->pmbase + sys->pmoccupied - below;
		if ((vlong)*sizep < 0)
			*sizep = 0;
	}
	return below / gbs_per_cluster;
}

/*
 * Being machno an index in sys->machptr, return the color
 * for that mach (color identifies locality).  used by main.c.
 *
 * on risc-v, assume hobbled harts followed by clusters of harts,
 * so color = (mp->hartid - soc.hobbled) / harts_per_cluster.
 */
int
corecolor(int machno)
{
	int color;
	Mach *mp;

	color = 0;
	if((uint)machno >= MACHMAX || sys == nil)
		return 0;
	mp = sys->machptr[machno];
	if(mp == nil)
		return 0;

	if (mp->hartid - soc.hobbled >= harts_per_cluster && harts_per_cluster)
		color = (mp->hartid - soc.hobbled) / harts_per_cluster;
	return color < 0? 0: color;
}
