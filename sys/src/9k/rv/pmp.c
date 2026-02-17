/*
 * extension of low.c to configure PMP.  have to be in M mode to do it
 * and temu doesn't implement it.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "riscv64.h"

#define dbprf if (!soc.lowdebug) {} else prf

enum {
	/*
	 * tinyemu doesn't implement pmp.  rvb-ice implements pmp and starts in
	 * M mode, but its other bugs make it unusable.
	 */
	Configpmp = 1,
	Pmpdebug = 1,
	Blindpmpsetting = 0,

	Pmpwid = 8,		/* bits per slot in cfg reg */
	Pmpslots = 8,		/* slots per cfg reg */
	Pmpmask = VMASK(Pmpwid),
};

static	int npmpcfg;

/* c910 needs us to write PMPCFG regs before we can read actual contents. */
static uvlong
rdpmpcfg(int n)
{
	m->probebad = 0;
	csrset(PMPCFG + n, 0);
	return csrrd(PMPCFG + n);
}

static uvlong
rdpmpslot(uint slot)
{
	uvlong cfg;

	cfg = rdpmpcfg(2*(slot/Pmpslots));
	return (cfg >> ((slot%Pmpslots)*Pmpwid)) & Pmpmask;
}

/* work out number of pmp config csrs */
static int
getnpmpcfgs(void)
{
	rdpmpcfg(0);			/* see if it faults */
	if (m->probebad) {
		m->probebad = 0;
		return 0;
	}
	if (npmpcfg == 0) {
		rdpmpcfg(2);		/* see if it faults */
		if (m->probebad) {
			m->probebad = 0;
			return 2;
		} else
			return 8;
	}
	return npmpcfg;
}

static void
dumppmp(char *desc)
{
	int slot;
	uint reg;
	uvlong cfg, addr;

	USED(desc);
	if (!Pmpdebug)
		return;
	for (reg = 0; reg < npmpcfg; reg++) {
		cfg = rdpmpcfg(2*reg);
		if (cfg == 0)
			continue;
		dbprf("\n%s: pmpcfg[%d] %#p\n", desc, 2*reg, cfg);
		for (slot = 0; slot < Pmpslots; slot++) {
			addr = csrrd(PMPADDR+Pmpslots*reg+slot);
			if (addr)
				dbprf("pmpaddr%d %#p\n", slot, addr);
		}
	}
}

/*
 * we assume that faults are not delegated, so sent to M mode, and
 * being caught by recmtrapalign.  bad csrs are mostly ignored.
 *
 * pmp is optional, and somewhat redundant on systems with MMUs.
 * it's likely to be configured correctly already since u-boot or other
 * boot loader has normally run.
 *
 * make sure that the whole of ram is rwx, where possible.  pmp csrs are only
 * accessible in machine mode, and pmp grants access only to accesses in S and
 * U modes, but pmp can restrict accesses in M mode.
 *
 * there may be 0, 16 or 64 entries, and only even-numbered PMPCFG registers
 * are used in RV64.
 */
static void
pmpinit(void)
{
	uint reg, slot, slotsh;

	/* work out number of pmp config csrs */
	dbprf("pmpinit...");
	npmpcfg = getnpmpcfgs();
	if (npmpcfg == 0) {
		dbprf("no PMP...");
		return;
	}

	dumppmp("initial");
	wbinvd();
	if (Blindpmpsetting) {			/* cheap hack */
		csrswap(PMPADDR+0, ~0ULL);
		csrswap(PMPCFG+0, Pmpnapot|Pmpr|Pmpw|Pmpx);
	} else {
		/* find first free slot, if any */
		for (slot = 0; slot < npmpcfg*Pmpslots; slot++)
			if (rdpmpslot(slot) == 0)
				break;
		if (slot >= npmpcfg*Pmpslots) {
			dbprf("no free pmpcfg slot in %d regs\n", npmpcfg);
			return;
		}

		/*
		 * if next slot is free too, leave this one zero as base addr.
		 * special case: Pmptor in slot 0 uses zero base.
		 */
		if (slot > 0 && slot < npmpcfg*Pmpslots-1 &&
		    rdpmpslot(slot+1) == 0)
			slot++;
		dbprf("free pmpcfg slot %d; allowing all\n", slot);

		/* 56 bits max. phys., low 2 implied */
		csrswap(PMPADDR + slot, VMASK(56-2));
		reg = 2 * (slot / Pmpslots);
		slotsh = (slot % Pmpslots) * Pmpwid;
		/*
		 * if we add Pmplock to cfg, this setting will apply even to M
		 * mode, but can't be changed until reset or power cycle.
		 */
		csrset(PMPCFG + reg, (Pmptor|Pmpr|Pmpw|Pmpx)<<slotsh);
	}
	wbinvd();		/* sfence.vma is required since priv 1.12 */
	dumppmp("configured");
}

void (*pmpinitp)(void) = pmpinit;
