/*
 * initialise a risc-v RV64G system enough to call main with the low identity
 * map and upper addresses mapped to lower, in supervisor mode.
 * this is traditionally done entirely in assembler, typically called l.s.
 *
 * do not call the real print nor iprint directly with '%' in the format here;
 * libc/fmt learns function addresses for verbs, which need to be high
 * addresses.  prf is promised to be safe.
 *
 * all cpus may be executing this code simultaneously.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "riscv64.h"

#define dbprf if (!soc.lowdebug) {} else prf
/* prf is now called from (i)print if early != 0 */
#define prf	iprint
#define print	iprint

enum {
	Diagnose = 0,		/* self-checking for a new machine */
	Measurestkuse = 0,

	Ptedebug = 0,		/* flag: print initial PTEs */
	Nptes = 12,		/* number of Ptes to dump */

	Pageexcs = 1<<Instpage | 1<<Loadpage | 1<<Storepage,
};

/* init with anything to force into data seg. to avoid bss zeroing */
int	asids = 0;
int	bootingcpu;
/* flag: booted in machine mode; assume super; init to avoid bss */
int	bootmachmode = 0;
Mach	dummymach = { 0 };
ulong	dummysc;
int	early = 1;		/* use prf & trap.c debugging */
ushort	hartids[MACHMAX] = { 0 };
Rvarch*	initarchp;
int	initstall;
/* uvlong assures alignment */
uvlong	initstks[MACHMAX][INITSTKSIZE / sizeof(vlong)] = { 0 };
Sys*	lowsys = 0;		/* visible to earlypagealloc in mmu.c */
uintptr	mainpc = 0;
uintptr satptval, satpepc;
void	(*tlbinvall)(void) = 0;

/*
 * machine mode setup and switch to supervisor mode.
 */

/* write csr then read back its value to see what stuck */
uintptr
csrwrrd(ushort csrno, uintptr new)
{
	csrswap(csrno, new);
	return csrrd(csrno);
}

/*
 * delegate M-mode exceptions and interrupts to S-mode, when possible,
 * and allow access to cycle counters.  also configure PMP if present.
 */
static void
delegate(void)
{
	dbprf("deleg...");
	csrswap(MCOUNTEREN, ~0ull); /* expose timers & cycle counters to S */
	csrswap(SCOUNTEREN, ~0ull);
//	mideleg = csrwrrd(MIDELEG, Superie);	/* old: punt S intrs to S */
	mideleg = csrwrrd(MIDELEG, ~0ull); /* try to punt S+M intrs to S */

	/* catch early stray M faults */
	dbprf("mtraps abort...");
	putmtvec(recmtrapalign);		/* low due to low PC */

	/* don't punt any except.s so we can detect PMP presence */
	csrswap(MEDELEG, 0);
	if (pmpinitp)
		(*(Pvfnv)ensurelow(pmpinitp))();

	/* punt except.s to S except env call from S (for reboot) */
	medeleg = csrwrrd(MEDELEG, ~(1LL<<Envcallsup));
	if ((medeleg & Pageexcs) != Pageexcs)
		prf("page fault exceptions not delegated by M mode!\n");
}

#define EXT(let) (1LL << ((let) - 'A'))

static void
loadids(Sys *lowsys)
{
	lowsys->extensions = csrrd(MISA);
	lowsys->archid =   csrrd(MARCHID);
	lowsys->vendorid = csrrd(MVENDORID);
	dbprf("misa %#p marchid %#p mvendorid %#lux...",
		lowsys->extensions, lowsys->archid, lowsys->vendorid);

	if (lowsys->vendorid == Vthead)
		dbprf("xuantie cpu...");
	if (lowsys->extensions & EXT('H')) {
		dbprf("hypervisor enabled...");
		csrswap(MISA, lowsys->extensions & ~EXT('H'));
		dbprf("now disabled...");
	}
}

static void
loadsbiids(Sys *lowsys)
{
	if (!bootmachmode && !nosbi) {
		/* dies on rvb-ice: no sbi, i guess */
		dbprf("cpu0: sbiget*...");
		lowsys->archid = sbigetmarchid();
		lowsys->vendorid = sbigetmvendorid();
	}
}

static void
assertrv(char *claim)
{
	USED(claim);
	dbprf("** If cpu is actually risc-v, %s.\n", claim);
}

static void
macharchinit(void)
{
	if (initarchp == nil)
		return;
	if (initarchp->machexten) {
		dbprf("\nenable M mode extensions...");
		(*(Pvfnv)ensurelow(initarchp->machexten))();
	}
	/* start other harts while we can */
	if (m->machno == 0 && initarchp->machstharts) {
		dbprf("start harts via M mode extensions...");
		(*(Pvfnv)ensurelow(initarchp->machstharts))();
	}
}

/*
 * do machine-mode set up, then leave machine mode.
 * set up for supervisor mode, needed for paging in kernel.
 * mostly punting faults to supervisor mode, preparing for clock interrupts,
 * and disabling paging.  mret will switch us to supervisor mode.
 *
 * needs m set, for CSR(MSCRATCH).
 */
static void
mach_to_super(Sys *lowsys)
{
	dbprf(" * machine mode.\n");
	delegate();			/* includes pmp init. */

	/* see setstkmach0 for # of priv modes (lowsys->nprivmodes) */
	/* M mode: little endian, prev mode = super, allow super interrupts */
	putmsts(Defmsts & ~(Mie|Tvm|Tw|Tsr|Ube|Mbe64|Sbe64));

	loadids(lowsys);
	macharchinit();			/* so far, only c910 uses this */

	/* set up machine traps on this cpu.  Mach is soon to be populated. */
	csrswap(MSCRATCH, (uintptr)m);	/* mmu will be off; low m & pc */
	/*
	 * maybe sbi is present, despite starting us in M mode?
	 * mtvec may have been from sbi, u-boot or rebootcode.
	 */
	dbprf("original mtvec %#p...", origmtvec);
	if (soc.c910 && origmtvec)
		putmtvec(origmtvec);	/* in case sbi is present */
	else
		putmtvec(mtrap);
	putstvec(rectrapalign);

	/* switch to super mode & enable mach intrs. */
	splhi();
	putmie(getmie() | Machie);	/* needed for clock intrs */
	putmsts(getmsts() | Mie);  /* Mpp is still Mppsuper, from start.s */
	dbprf("to super: mstatus %#p sstatus %#p mret\n", getmsts(), getsts());
	assertrv("switching to supervisor mode");
	mret();
}

/*
 * the rest is supervisor mode setup.
 */

#define PADDRINCRFORPTE(pa)		(((((pa)) >> PGSHFT) << PTESHFT))

/*
 * populate n page table entries at ptep with attributes from ptebits
 * and physical addresses starting from the one in ptebits.
 * lvl is the level of the page table.
 */
void
setptes(PTE *ptep, PTE ptebits, int n, int lvl)
{
	PTE pteincr, attrs, addr;

	addr  = PTEADDRBITS(ptebits);
	attrs = PTEATTRBITS(ptebits);
	pteincr = PADDRINCRFORPTE(PGLSZ(lvl));
	for (; n > 0; n--) {
		if(addr & ~(PTEADDRMASK|VMASK(PTESHFT)))
			/* addr too big for pte field, don't map */
			*ptep++ = 0;
		else
			*ptep++ = addr | attrs;
		addr += pteincr;
	}
}

uintptr
lvlkzero(int lvl)
{
	int maskbits;

	maskbits = PGLSHFT(lvl+1);
	return -(1ull << (MIN(64, maskbits) - 1));
}

/*
 * create top-level leaf page table entries at lvl in page table at ptp
 * to map to phys for phys & KZERO|phys.
 * KZERO is different for each paging mode.
 */
void
dualmap(PTE *ptp, uintptr phys, uint nptes, int lvl)
{
	uintptr kzero;
	PTE ptebits;

	/* force to physical space, round down to nearest superpage */
	kzero = lvlkzero(lvl);
	phys = ROUNDDN(phys & ~kzero, PGLSZ(lvl));
	ptebits = PADDRFORPTE(phys) | PteRWX | Pteleafvalid;
	setptes(&ptp[0], ptebits, nptes, lvl);
	setptes(&ptp[Ptpgptes/2], ptebits | (lvl == Toplvl? PteG: 0),
		nptes, lvl);
	coherence();
}

static void
dumpptes(PTE *ptp, int lvl)
{
	int i, st;

	dbprf("initial page table excerpts:\n");
	for (i = 0; i < Nptes; i++)
		if (ptp[i])
			dbprf("ptp[%d] %#p\n", i, ptp[i]);
	st = PTLX(KZERO, lvl);
	if (st == 0)			/* for Sv64 */
		st = Ptpgptes/2;
	for (i = st; i < st + Nptes; i++)
		if (ptp[i])
			dbprf("ptp[%d] %#p\n", i, ptp[i]);
}

/*
 * construct minimal initial top-level page table with id & upper->lower maps.
 */
static void
mkinitpgtbl(Sys *lowsys, int lvl, uintptr sv)
{
	uint nptes;
	PTE *ptp;

	/*
	 * allocate temporary top-level page table.
	 * in top-level pt for 4 levels, each PTE covers 512GB.
	 * for 3 levels, each covers 1GB.
	 */
	ptp = lowsys->initpt;
	dbprf("creating initial page table at %#p\n", ptp);
	zero(lowsys->initpt, sizeof lowsys->initpt);

	/*
	 * leave last pte of upper and lower ranges free
	 * (for VMAP in upper->lower map at least).
	 * can't do this in Sv64.
	 */
	nptes = Ptpgptes/2 - 1;		/* for each range */
	if (VMBITS == 64)
		nptes = Ptpgptes/2;
	else if (VMBITS < 39)
		nptes /= 2;	/* upper and lower ranges fit into low addr.s */

	/*
	 * populate id map in lower range & upper->lower, from 0 up.
	 */
	dbprf("populating normal root page table for lvl %d...", lvl);
	dualmap(ptp, 0, nptes, lvl);	/* default to sv39 */	
	if (soc.c910) {
		/* don't map first 4K or 2MB page to avoid hang */
		/* for PHYSMEM, use PPN((uvlong)KTZERO & VMASK(30)) */
	}
	if (Ptedebug)
		dumpptes(ptp, lvl);
	lowsys->satp = normalsatp = sv | ((uintptr)ptp / PGSZ);
	dbprf("setting normal satp %#p...\n", normalsatp);
}

/*
 * assumes a standard clint.  m->timecmp is mainly for mtrap.s, use low addr.
 */
void
supsetmtimecmp(void)
{
	Clint *clint;

	clint = m->clint;
	if (clint == nil) {
		prf("clint: zero address\n");
		return;
	}

	/* mtimecmp is most likely to be permitted by pmp. */
	if (probeulong((ulong *)clint->mtimecmp, Read) < 0) {
		if (m->machno == 0)
			prf("clint: no response from %#p\n", clint);
	} else {
		dbprf("clint: at %#p\n", clint);
		if (bootmachmode)
			m->mtimecmp = ensurelow(&clint->mtimecmp[m->hartid]);
	}
}

static void
newmach(uint cpu)
{
	/* initialise enough of Mach to get to main */
	m->machmode = bootmachmode;
	fakecpuhz();
	usephysdevaddrs();		/* device vmaps not yet in effect */
	m->machno = cpu;		/* override preset from newcpupages */
	m->hartid = hartids[cpu];
	m->mtimecmp = nil;			/* pessimism */

	if (bootmachmode) {
		putmtvec(mtrap);
		csrswap(MSCRATCH, (uintptr)m);	/* ready for mtraps now */
	}
	putstvec(strap);
	putsscratch((uintptr)m);		/* ready for straps now */

	if(cpu == 0)
		dbprf("Booting Plan 9 on hart %d\n", m->hartid);
	clrstie();
	if (nosbi)
		wrcltimecmp(VMASK(63));		/* no clock intrs on m */
}

static void
ckwdog(void)				/* try to detect a watchdog */
{
	int i;

	if (soc.newmach && Diagnose && soc.lowdebug) {
		dbprf("watchdog hanging over our heads? ");
		for (i = 0; i < 10; i++) {
			delay(500);
			dbprf(".");
		}
		dbprf("apparently not.\n");
	}
}

static void
alignchk(void)
{
	static int align = 0x123456;

	if (align != 0x123456)
		panic("data segment misaligned");
}

extern uint harts_per_cluster;

/*
 * On cpu0, fill in bits of Mach and Sys, allocate a permanent stack in
 * sys->machstk, signal other cpus to progress, return stack top in low
 * addresses.
 */
static uintptr
setstkmach0(void)
{
	char *aftreboot;
	Sys *rlowsys;

	/* compute (low)sys once, in case the computation changes */
	lowsys = rlowsys = (Sys *)
		(membanks[0].addr + membanks[0].size - Syssize);
	sys = KADDR((uintptr)rlowsys);
	coherence();
	m = &rlowsys->mach;
	usephysdevaddrs();		/* device vmaps not yet in effect */

	/* now safe to lock */
	soc.clintlongs = 1;		/* pessimism, works on all */
	alignchk();

	/* tell other cpus to wait in secstall(). */
	rlowsys->secstall |= RBFLAGSTALL;

	/* notably, zero machstk before use, for usage measurements. */
	zero(&rlowsys->Syspercpu, sizeof(Syspercpu));

	/* zero Sys from after Reboot to before kmesg */
	aftreboot = (char *)&rlowsys->startzero;
	zero(aftreboot, (char *)rlowsys->stopzero - aftreboot);

	/* publish Mach in machptr for other cpus */
	rlowsys->machptr[0] = ensurehigh(&sys->mach);

	/* set for delay() */
	rlowsys->clintsperµs = clintsperµs = timebase / MHZ;
	coherence();
	/* now safe to print or issue delays */

	rlowsys->ucstrat = Uncnone;

	rlowsys->satp = 0;	/* don't use previous kernel's map */
	/* see loadids, which notes extensions in M mode */
	rlowsys->nprivmodes = 2; /* for plic: Machine & Super can take intrs */
	/*
	 * todo: increment rlowsys->nprivmodes for each of the hyper and
	 * user-interrupt extensions that is present.
	 */
	newmach(0);

	// prf("\n9\n");		/* won't be logged in kmesg */

	if (initarchp) {
		initarchp = ensurelow(initarchp);
		dbprf("for %s cpus...", ensurelow(initarchp->name));
	}
	loadsbiids(rlowsys);
	ckwdog();
	return (uintptr)&rlowsys->machstk[MACHSTKSZ];
}

vlong
putsatp(uintptr satp)
{
	vlong ret;
	Mpl s;

	s = splhi();
	if (!m->machmode && satp) {
		if (soc.c910 && tlbinvall) {
			prf("tlbinvall 1...");
			tlbinvall();	/* superstition */
		}
		/*
		 * to really use this, we'd need an asid allocator
		 * and a strategy for reclaiming asids when we run out
		 * (unless we have more asids than procs).
		 */
		if (FUTURE && asids > 1)
			satp |= 1LL << Satpasidshft;
	}
	ret = _putsatp(satp);
	if (soc.c910 && tlbinvall) {
		tlbinvall();		/* superstition */
		prf("tlbinvall 2...");
	}
	splx(s);
	return ret;
}

vlong
normalmap(void)
{
	return putsatp(normalsatp);
}

/*
 * on a secondary cpu, start.s will have waited for initstall to clear,
 * which is done after sys, lowsys and lowsys->secstall are set, then
 * secstall waits until lowsys->secstall is released, sets
 * m, waits for go-ahead from m->online, returns stack top.  another cpu must
 * have allocated a Mach for us previously and set sys->machptr[cpu] to it.
 *
 * the MMU may be configured, but we're still in low addresses, and
 * could be in machine mode.
 */
static uintptr
secstall(int cpu)
{
	Mach *m0;

	/*
	 * in case we get here before cpu0 sets lowsys and sys.
	 * then wait for cpu0 to allocate secondaries' data structures and clear
	 * lowsys->secstall in main.c by schedcpus or settrampargs from reboot.
	 */
	if (cpu != bootingcpu)
		prf("cpu%d has bootingcpu %d\n", cpu, bootingcpu);
	while (lowsys == nil || sys == nil || lowsys->secstall) {
		pause();
		coherence();
	}

	/*
	 * cpu & hartid were validated in start.s, so sys->machptr[cpu] is now
	 * valid.
	 */
	m = lowsys->machptr[cpu]; /* machptr[cpu] must be a high address */
	if (m == nil)
		panic("setmach: nil sys->machptr[%d] before mallocinit", cpu);
	m = ensurelow(m);
	usephysdevaddrs();		/* device vmaps not yet in effect */

	/* now safe to lock */
	newmach(cpu);
	m0 = ensurelow(lowsys->machptr[0]);
	m->cpuhz  = m0->cpuhz;
	m->cpumhz = m0->cpumhz;
	/* our m->ptroot was set by cpu0 when it allocated our Mach */
	if (cpu == 0)
		prf("low: cpu is 0 in secstall!\n");

	/* start cpus in sequence, in case of near-simultaneous start ups. */
	delay(50*cpu);

	m->waiting = 1;			/* notify schedcpus that we are up */
	coherence();
	dbprf("cpu%d waiting for online...", cpu);
	while (!m->online) {
		pause();
		coherence();
	}

	return m->stack + MACHSTKSZ;	/* permanent stack top for this hart */
}

uintptr satpread;

static int
gotsatp(uintptr nsatp)
{
	int r;
	Mpl pl;

	pl = splhi();
	if (normalmap() < 0)		/* enable normal map to include stvec */
		panic("putsatp faulted on %#p", sys->satp);

	dbprf("gotsatp: switch to test page tbl: writing satp %#p\n", nsatp);
	/* this putsatp may fault.  works on temu, vf2, jupiter. */
	r = putsatp(nsatp) >= 0;
	satpread = getsatp();
	normalmap();			/* in case nsatp failed */
	splx(pl);
	return r;
}

#define MAXSV Sv64	/* Sv64 under temu doesn't work yet */

/*
 * probe satp modes and determine max. AS ids.  informational only, restore the
 * normal map upon exit.  only safe to use under the dual id map (0->0,
 * KZERO->0), because a failure to write satp may revert to Bare mode (no
 * paging).  since KZERO depends on paging mode, this is only safe to execute
 * from low memory.  writing an unimplemented paging mode is supposed to have no
 * effect, either on paging or the contents of satp (priv 1.12), but tinyemu at
 * least seems to revert the mmu to Bare mode.
 */
void
probesatp(Sys *sys)
{
	int lvl;
	uintptr mode, readmode, satp;
	void *pt;
	Mpl pl;

	dbprf("\nprobing satp modes...");
	bestmode = Sv39;
	asids = 0;

	pl = splhi();
	dbprf("usual map to set csrs...");
	putstvec(ensurehigh(strap));
	putsscratch((uintptr)ensurehigh(m));

	pt = sys->ncpt[0];
	satp = (uintptr)ensurelow(pt) / PGSZ;
	for (lvl = 2, mode = Sv39; mode <= MAXSV; lvl++, mode += 1LL<<Svshft) {
		dbprf("usual map before probe...");
		normalmap();		/* revert before changing test pt */

		dbprf("populating lvl %d root page table...", lvl);
		zero(pt, PTSZ);
		dualmap(pt, 0, Ptpgptes/2 - (VMBITS == 64? 0: 1), lvl);
		// mmudump((uintptr)pt, lvl);

		/* try test pt */
		dbprf("probing with %#p\n...", mode | satp | Satpasidmask);
		if (haveinstr(gotsatp, mode | satp | Satpasidmask) < 0)
			break;
		normalmap();		/* in case gotsatp faulted */
		readmode = satpread;
		dbprf("probesatp: read back %#p\n", readmode);
		if ((readmode & Svmask) == mode)
			bestmode = readmode;
	}
	asids = ((bestmode & Satpasidmask) >> Satpasidshft) + 1;
	/* results will be printed in main */

	normalmap();

	/*
	 * force test page to Sv39 before setting root page table for normal use
	 */
	zero(pt, PTSZ);
	dualmap(pt, 0, Ptpgptes/2 - (VMBITS == 64? 0: 1), 2);
	putsatp(Sv39 | satp);

	/* set up mmu with requesated pagingmode, verify operation */
	mkinitpgtbl(lowsys, Toplvl, pagingmode);

	dbprf("\nprobe initial page table (satp %#p)...", sys->satp);
	normalmap();			/* done testing */
	splx(pl);
	if (getsatp() != sys->satp) {
		dbprf("can't set satp to %#p!\n", sys->satp);
		return;
	}
	dbprf("installed...");
}

/*
 * start with the shared initial page table in lowsys->satp,
 * which should be lowsys->initpt.
 * enabling paging also enables speculative execution on xuantie.
 *
 * a netbsd porter says setting satp always causes a fault (S or M?);
 * it seems to make only the xuantie cpus lose their minds.
 *
 * this must not cause a page fault, except when protected by haveinstr,
 * since up is still nil (i.e., there is no user context).
 */
static void
pagingon(Sys *lowsys)
{
	uintptr satp;

	if (initarchp && initarchp->supermppaging) {
		dbprf("set stvec for exten bugs %#p...", rectrapalign);
		/* rectrapalign is low here when PC is low */
		putstvec(rectrapalign);
		dbprf("cpu%d paging extension...", m->machno);
		(*(Pvfns)ensurelow(initarchp->supermppaging))(lowsys);
	}

	putstvec(strap);
	archmmu();
	if (m->machno == 0) {
		mkinitpgtbl(lowsys, 2, Sv39);	/* universal */
		/* i'd prefer to defer this until main, but can't; see main */
		probesatp(lowsys);	/* find best mode & max. AS id */
	}

	dbprf("switch to initial page tbl: writing satp %#p\n", lowsys->satp);
	if (putsatp(lowsys->satp) < 0)
		panic("putsatp faulted on %#p", lowsys->satp);

	dbprf("set stvec for kernel %#p...", ensurehigh(strap));
	putstvec(ensurehigh(strap));		/* va; needed? */
	dbprf("\n");

	satp = getsatp();		/* asid field will be (# asids) - 1 */
	dbprf("read satp back as %#p\n", satp);
	if ((satp & ~Satpasidmask) != lowsys->satp)
		panic("couldn't enable paging with satp %#p", lowsys->satp);
	m->pagingon = 1;
}

static void
prstackuse(void)
{
	uvlong *stkbot, *stkbase;

	if (Measurestkuse) {
		stkbot = stkbase = initstks[m->machno];
		prf("\ncpu%d: initstk %#p: %#p %#p %#p %#p use %lld bytes\n\n",
			m->machno, stkbase, stkbot[0], stkbot[1], stkbot[2],
			stkbot[3], stackuse((char *)stkbase,
			  (char *)stkbase + INITSTKSIZE));
	}
}

void
setsts(void)
{
	/* S mode: little endian, prev mode = user, allow intrs in U mode */
	putsts((getsts() & ~(Ube|Spp|Uxl|Mpp|Sbe64)) | Defssts|Spie|Mppuser);
}

/*
 * We are called with all interrupts disabled.
 *
 * A temporary stack (in initstks) must be in use when we are called.
 * BSS has already been zeroed.  Don't use any high addresses until we are
 * definitely in supervisor mode with paging on.
 *
 * Main goals are: establish m, sys, a permanent stack, [sm]scratch, [sm]tvec,
 * [sm]status, page table, ensure cpu is in super mode, enable paging,
 * call main with machno.
 * Other cpus may be executing the same code at or near the same time.
 *
 * sys is a shared global ptr, always a high address until shutdown, but our
 * use here will be of its (low) physical address, lowsys.  It's 1MB at the
 * top of the first memory bank, usually the first GB.
 */
void
low(uint cpu)
{
	uintptr stktop;
	Page *ptroot;

	up = nil;
	setsts();
	if (cpu >= MACHMAX)
		panic("low: cpu %d out of range", cpu);
	/* Set bits of Mach and Sys, m, and works out new stack top. */
	if (cpu == 0)
		stktop = setstkmach0();		/* also sets sys and lowsys */
	else
		stktop = secstall(cpu);

	/*
	 * on secondaries, m->online is now 1, so iovmapsoc must have run on
	 * cpu0 by now, thus its page table's kernel mappings are final.
	 * On all cpus, get out of machine mode if necessary, and start paging.
	 */
	dbprf("[cpu%d hart%d]\n", cpu, hartids[cpu]); /* show signs of life */
	if (bootmachmode)
		mach_to_super(lowsys);	/* enables any M extensions too */

	dbprf("\n * supervisor mode.  sbi %s\n", nosbi? "not used": "assumed");
	m->machmode = 0;
	putstvec(strap);		/* make probes work w/ low addr */
	putsscratch((uintptr)m);	/* ready for straps now */
	pagingon(lowsys);	/* install low id map & upper->low map */

	/*
	 * switch to new, permanent stack in Syspercpu->machstk for this cpu at
	 * upper addresses.  All extant automatic variables and saved return
	 * addresses are on the old (current) stack (or in registers), thus
	 * unpredictable after the switch.
	 *
	 * jump into high (kernel) addresses, thus vacating the lower range for
	 * user processes.  adjust pointers and registers as needed.
	 */
	putstvec(rectrapalign);
	prstackuse();

	dbprf("new stack %#p...", (uintptr)ensurelow(stktop - SBIALIGN));
	setsp((uintptr)ensurelow(stktop - SBIALIGN));

	dbprf("jump to kernel space...");
	jumphigh();			/* adjust registers (m, sp, sb) */

	/*
	 * now executing in upper space with high static base
	 * and invalid automatic variables.
	 */
	if (m->ptroot) {
		ptroot = m->ptroot = ensurehigh(m->ptroot);
		/* newcpupages has set ptroot->va low on secondary cpus */
		ptroot->va = (uintptr)ensurehigh(ptroot->va);
	}
	m->stack = (uintptr)ensurehigh(m->stack);

	dbprf("main...\n");
	main(m->machno);
	notreached();
}
