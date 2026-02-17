/*
 * eic7700x-specific stuff (u84 for e.g., premier p550): soc init, reset
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "riscv64.h"

/* eswin clock enable and reset bits */
enum {
	Clkrst	= 0x51828000,
	Eth0clk	= Clkrst + 0x158,
	Eth1clk	= Clkrst + 0x15c,

	Ethclkena= 1<<0,
	Rmiiclkena= 1u<<31,

	Pciecfgclk = Clkrst + 0x174,
	Snocclk	= Clkrst + 0x400,
	Dmarst	= Clkrst + 0x41c,

	Eth0rstdeass= 1<<26,
	Eth1rstdeass= 1<<25,

	Pcieclk	= Clkrst + 0x420,	/* pcie clock deassert bits */

	L2prefetcher= 0x02030000,		/* u74 l2 prefetch control */

	Syscon = 0x51600080,		/* FU800 syscon address from fdt */
	// or 0x51810000
	// hsp-sp-csr fo ether 0x100 0x108 0x118 (phy ctl or clk sel, lpi,
	// delay) or 0x1030 0x100 0x108

	Rootport2ndary = 1,		/* root port's secondary bus */
// TODO	Rootport2ndary = 0,		/* root port's secondary bus */
};

#define Elbictl0	0
#define Elbists0	0x100

/* CTRL0 bitfields */
#define Elbictl0devtypemask	0xF		/* [3:0] */
#define Elbictl0appltssmena	(1 << 5)
#define Elbictl0appholdphyrst	(1 << 6)
#define Elbictl0pmselauxclk	(1 << 16)

/* STATUS0 bitfields */
#define Elbists0pmselauxclk (1 << 16)

/* Pcie "device/port type" values (Pcie spec: Pciexptype*) */
#define Pciexptyperootport      4

/* --- Designware DBI regs --- */
/* Standard PCI config header offsets (type 1 header for Root Port) */
#define Pcivendorid	0
#define Pcideviceid	2
#define Pcicmd		4
#define Pcirevision	8
#define Pciprimarybus	0x18
#define Pcibaseaddr0	0x10
#define Pcibaseaddr1	0x14

#define Pcicmdio	1
#define Pcicmdmemory	2
#define Pcicmdmaster	4
#define Pcicmdserr	0x100

/* Root Port class code: 0x0604xx (PCI-to-PCI bridge) */
#define Pcibridgepci	0x0604

/* DBI "read-only write enable" (Designware) */
#define Pciemiscctl1off	0x8BC
#define Pciedbirowren	(1 << 0)

/* PCI Express Capability / Link Status */
#define Pcicaplist		0x34
#define Pcicapidexp		0x10
#define Pciexplnksta		0x12
#define Pciexplnkstadllla	(1 << 13)

/* --- iATU regs --- */
/*
 * For DWC v4.80a+ the iATU registers are "unrolled".  Each region has a 0x200
 * block, and inbound/outbound are distinguished by bit 8 of the region offset.
 */
#define Pcieaturegdirib	(1U << 31) /* use in helper only */
#define Pcieaturegdirob	0

typedef struct {
	ulong	regctrl1;
	ulong	regctrl2;
	ulong	lowerbase;
	ulong	upperbase;
	ulong	limit;
	ulong	lowertarg;
	ulong	uppertarg;
	ulong	pad0;
	ulong	upperlimit;
} Aturegion;

#define Pcieatuenable	(1U << 31)

#define Pcieatutypemem	0
#define Pcieatutypeio	2
#define Pcieatutypecfg0	4
#define Pcieatutypecfg1	5

/* Default offset from DBI to iATU registers on many DWC cores. */
#define Dfltdbiatuoff  (3 << 20)		/* 0x0030_0000 */

static uintptr dbi_base, elbi_base, cfg_base;
static uintptr atu_base;	/* usually dbi_base + Dfltdbiatuoff */
/* Which iATU outbound region to use for config transactions */
static unsigned cfg_atu_region;

static void
dbi_rowrena(uintptr dbi_base, int enable)
{
	ulong *addr = (ulong *)(dbi_base + Pciemiscctl1off);

	if (enable)
		*addr |= Pciedbirowren;
	else
		*addr &= ~Pciedbirowren;
	coherence();
}

static uintptr
atu_region_base(unsigned dir, unsigned region)
{
	return atu_base +
		((uintptr)region<<9 | (dir == Pcieaturegdirib? 1<<8: 0));
}

/* Program one outbound iATU region (unrolled) */
static void
atu_prog_outbound(unsigned region, ulong type, uvlong cfg_base,
	uvlong bdf, uvlong size)
{
	Aturegion *r = (Aturegion *)atu_region_base(Pcieaturegdirob, region);

	/* Disable region before reprogramming (optional but common) */
	r->regctrl2 = 0;
	coherence();

	cfg_base = PADDR((void *)cfg_base);
	r->lowerbase = cfg_base;
	r->upperbase = cfg_base >> 32;
	cfg_base += size - 1;
	r->limit =	cfg_base;
	r->upperlimit = cfg_base >> 32;
	bdf = BUSBNO(bdf)<<24 | BUSDNO(bdf)<<19 | BUSFNO(bdf)<<16;
	r->lowertarg = bdf;
	r->uppertarg = bdf >> 32;
	r->regctrl1 = type;
	coherence();
	r->regctrl2 = Pcieatuenable;
	coherence();
}

/*
 * config space for tbdf should be at (return address - where).
 *
 * Map a single BDF's config space into the config aperture & return pointer
 *
 * The DWC host driver programs iATU target with bus/dev/fn in the upper
 * bits, and then uses the CPU-side "config" aperture for MMIO accesses.
 * We reprogram the region as needed.
 */
static void *
eicpcicfgaddr(int tbdf, int where)
{
	ulong type, busdev;
	static ulong curbdf = -1;

	where &= ~3;
	if (tbdf == curbdf)
		return (void *)(cfg_base + where);
	curbdf = tbdf;

	/* f was shifted 8 by MKBUS in tbdf, needs to be by 16.  or not. */
//	busdev = tbdf << 8;
	busdev = tbdf;
	/*
	 * root complex is bus 0, and root port's secondary bus is usually
	 * assigned to bus 1 during enumeration.
	 */
	type = BUSBNO(tbdf) == Rootport2ndary? Pcieatutypecfg0: Pcieatutypecfg1;
	atu_prog_outbound(cfg_atu_region, type, cfg_base, BUSBDF(busdev),
		0x800000L);
	return (void *)(cfg_base + where);
}

void *(*pcicfgaddr)(int, int) = eicpcicfgaddr;

/*
 * Minimal bring-up:
 *  - ungate clocks / deassert Soc resets (platform hook)
 *  - configure controller as Root Port
 *  - toggle PERST#
 *  - release PHY reset hold, wait ~20ms for PHY ready
 *  - patch vendor/device Ids into RC config header (optional)
 *  - enable LTSSM and wait for link up
 *  - setup RC config header (bus numbers, cmd bits)
 *
 * Returns 0 on success, <0 on failure.
 */
int
eic770pciinit(void)
{
	ulong s, v;
	unsigned t;

iprint("eic770pciinit: ");
	dbi_base  = (uintptr)soc.pcivend;	/* kernel addrs */
	cfg_base  = (uintptr)soc.pci;
	elbi_base = (uintptr)soc.pcictl;
	atu_base  = dbi_base + Dfltdbiatuoff;
	cfg_atu_region = 1;

	/* Configure Root Port type in ELBI CTRL0 */
	v = *(ulong *)(elbi_base + Elbictl0) & ~Elbictl0devtypemask;
	v |= (Pciexptyperootport & Elbictl0devtypemask);
	*(ulong *)(elbi_base + Elbictl0) = v;
iprint("elbictl0\t%#p: %#lux to config root port\n", elbi_base + Elbictl0, v);
	coherence();

	/*
	 * Hold PHY reset bit: clear it to let PHY come up.  Then poll STATUS0
	 * bit 16 to confirm Pmselauxclk deasserted (PHY ready).
	 */
	*(ulong *)(elbi_base + Elbictl0) &= ~Elbictl0appholdphyrst;
iprint("elbictl0\t%#p: %#lux to clear phy reset\n", elbi_base + Elbictl0, *(ulong *)(elbi_base + Elbictl0));
	coherence();

	for (t = 0; t < 20; t++) {
		coherence();
		s = *(ulong *)(elbi_base + Elbists0);
		if ((s & Elbists0pmselauxclk) == 0)
			break;
		delay(1);
	}
	if (t == 20) {
		iprint("eic770pciinit: aux clk still asserted\n");
		return -2;
	}

	dbi_rowrena(dbi_base, 1);
	/* Optional: write Vendor/Device Ids into the RC config header (needs RO write enable). */
	/* device:vendor packed */
	*(ulong *)(dbi_base + Pcivendorid) = 0x2030<<16 | 0x1fe1;

	/* Setup minimal RC config header fields (similar to dw_pcie_setup_rc()) */
	/* RC Bars (type 1 header): BAR0=32-bit mem, BAR1=0 */
	*(ulong *)(dbi_base + Pcibaseaddr0) = 4;
	*(ulong *)(dbi_base + Pcibaseaddr1) = 0;
	coherence();

	/* Bus numbers: primary=0, secondary=1, subordinate=0xff */
	v = *(ulong *)(dbi_base + Pciprimarybus) & 0xff000000U;
	*(ulong *)(dbi_base + Pciprimarybus) = v | 0x00ff0100U;
iprint("Pciprimarybus\t%#p: %#lux\n", dbi_base + Pciprimarybus, *(ulong *)(dbi_base + Pciprimarybus));
	coherence();

	/* Command: enable IO/MEM/BUSMASTER/SERR */
	v = *(ulong *)(dbi_base + Pcicmd) & 0xffff0000U;
	v |= Pcicmdio | Pcicmdmemory | Pcicmdmaster | Pcicmdserr;
	*(ulong *)(dbi_base + Pcicmd) = v;
iprint("Pcicmd\t%#p: %#lux to enable accesses\n", dbi_base + Pcicmd, v);
	coherence();

	/* Class code (upper 24 bits at 0x08): 0x0604xx */
	v = *(ulong *)(dbi_base + Pcirevision) & 0x000000ff;
	*(ulong *)(dbi_base + Pcirevision) = v | Pcibridgepci << 8;
iprint("Pcirevision\t%#p: %#lux to set class code\n", dbi_base + Pcirevision, *(ulong *)(dbi_base + Pcirevision));
	coherence();

//	dbi_rowrena(dbi_base, 0);

	/* Enable LTSSM */
	v = *(ulong *)(elbi_base + Elbictl0);
	*(ulong *)(elbi_base + Elbictl0) = v | Elbictl0appltssmena;
iprint("Elbictl0\t%#p: %#lux to set ltssm\n", elbi_base + Elbictl0, *(ulong *)(elbi_base + Elbictl0));
	coherence();

	/* Wait for Data Link Layer Link Active */
// TODO	if (pcie_wait_link_up(p, 200) != 0)
//		return -3;
	delay(200);
iprint("done\n");
	return 0;
}

typedef struct {
	ulong	ctl;
	ulong	morectl;
} L2prefetch;

/*
 * ensure that all the bloody fiddly clock signals are on.
 * assumes mmu is on.
 */
static void
eic7700init(void)
{
	int n;

	for (n = 0; n < 2; n++)
		*(ulong *)KADDR(Eth0clk + 4*n) |= Ethclkena | Rmiiclkena;
	coherence();
	delay(1);
//	*(ulong *)KADDR(Dmarst) |= Eth0rstdeass | Eth1rstdeass;
	*(ulong *)KADDR(Dmarst) = ~0;	/* just set all reset deass. bits */
	coherence();

	/* so far, none of this enables reading pcie config space */
	/* setting to ~0 then 0 wedges in eic770pciinit */
	*(ulong *)KADDR(Pcieclk) = 0;
	coherence();
	delay(100);
	*(ulong *)KADDR(Pcieclk) = ~0;	/* just set all reset deass. bits */
	coherence();
	delay(100);
	/* setting to 0 wedges immediately */
	*(ulong *)KADDR(Snocclk) = ~0;	/* take pcie & others out of reset */
	coherence();
	delay(100);
	/* setting to 0 wedges in eic770pciinit */
	*(ulong *)KADDR(Pciecfgclk) = ~0;
	coherence();
	delay(100);

	eic770pciinit();

	/* check uncached access sanity */
	if (soc.uncached) {
		uvlong d1, d2;
		uvlong *p1, *p2;

		cachedwbse(_main, sizeof(uvlong));
		p1 = (uvlong *)_main;
		p2 = uncachedview(p1);
		d1 = *p1;
		d2 = *p2;
		if (d1 == d2) {
			if (soc.newmach)
				iprint("uncached view access okay.\n");
		} else
			iprint("uncached view access failed: *%#p = %#llux; "
				"*%#p = %#llux\n", p1, d1, p2, d2);
	}
}

static void
eic7700hartinit(void)
{
	L2prefetch *l2pf = (L2prefetch *)KADDR(L2prefetcher);

	/* rein in the prefetcher */
	/* sifive's cache prefetcher Csrs are M-mode only, alas */
	if (probeulong((ulong *)l2pf, Read) >= 0) {
		if (m->machno == 0)
			iprint("l2 prefetcher %#p is accessible.\n", l2pf);
//		l2pf->ctl &= ~(1<<0);		/* no scalar loads */
//		l2pf->morectl &= ~(1<<19);	/* no scalar stores */
	} else
		if (m->machno == 0)
			iprint("l2 prefetcher %#p is not accessible.\n", l2pf);
}

static void
eic7700reset(void)
{
}

void (*socinit)(void) = eic7700init;
void (*hartinit)(void) = eic7700hartinit;
void (*rvreset)(void) = eic7700reset;
