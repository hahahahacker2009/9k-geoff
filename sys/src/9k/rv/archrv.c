/*
 * risc-v rv64gc dependencies: clint, clock, ipis, harts, l2/3 caches, idling,
 *	delays, clz
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "riscv64.h"
#include "cpucap.h"
#include "uncached.h"

#define EXT(let) (1LL << ((let) - 'A'))
#define EXTIMA	(EXT('I') | EXT('M') | EXT('A'))
#define EXTFD	(EXT('F') | EXT('D'))
#define EXTSU	(EXT('S') | EXT('U'))

#define RDTIMES(tsc, rdtm, mtime) coherence(); \
	mtime = rdcltime();		/* clint ticks */ \
	rdtm = rdtime();		/* constant clint(?) cycles */ \
	tsc = rdtsc()			/* executing cycles */

enum {
	Livedangerously = 0,	/* flag: don't reset when sbi lies */
	Reservedways = 1,  /* we need 1 l[23] cache way unenabled as cache */
	Hartcols = 3,
	Paranoid = 0,
};

struct Cpu {
	char	*name;
	ulong	vendorid;
	uvlong	archid;
};
/* so far, only SBI lies, CSR(MHARTID) doesn't */
typedef struct {
	char	havestate;
	schar	state;		/* sbi's notion of state; dubious */
	char	running;	/* set at start up; trustworthy */
} Hart;

/*
 * usual sifive l2 or l3 cache control.  just set and forget on working systems.
 * flushing is only needed on broken systems with incoherent DMA.
 * from the sifive u74 core complex manual.
 */
typedef struct L2cache L2cache;
struct L2cache {
	union {
		uchar _0_[0x200];
		struct {
			/* le bytes: banks, ways, lg sets, lg cb bytes (ro) */
			ulong	config;
			ulong	_1_;
			ulong	wayenable; /* low byte: ways enabled - 1 */
		};
	};
	/* private l2 controllers have ops in high bits */
	uvlong	flush64;	/* write phys addr of cache line to flush here */
	uchar	flushcnt;	/* only present if this is a private l2 cache */
};

typedef struct Pagingmode Pagingmode;
struct Pagingmode {
	uchar	bits;
	uchar	levels;
	uvlong	satpmode;
};

enum Sbihartsts {			/* results of sbihartstatus */
	Hstarted, Hstopped, Hstartpend, Hstoppend,
	Hsuspended, Hsuspendpend, Hresumepend,
};

char *hartstates[] = {
	"started", "stopped", "start_pending", "stop_pending",
	"suspended", "suspend_pending", "resume_pending",
};

extern int nrdy;			/* from proc.c */

int	portclz(Clzuint n);

uvlong pagingmode = PAGINGMODE;
int (*archclz)(Clzuint) = portclz;

static Hart harts[HARTMAX];

Cpu cpus[] = {
	{ "sifive e3/s5", Vsifive,	1, },			/* 3/5-series */
	{ "sifive u7",	Vsifive,	0x80000007u, },		/* 7-series */
	{ "sifive u7",	Vsifive,	0x8000000000000007ull, }, /* 7-series */
	{ "sifive p550", Vsifive,	0x8000000000000008ull, }, /* u84 */
	{ "sifive",	Vsifive,	0, },
	{ "spacemit x60", Vspacemit,	0x8000000058000001ull, },
	{ "spacemit",	Vspacemit,	0, },
	{ "xuantie",	Vthead,		0, },			/* bugs'r'us */
	{ "c-sky (now xuantie)", 0x410,	0, },
	{ "tinyemu",	0xbe11a5d,	0x564, },
	{ "zero vendor & arch (emulation?)", 0,	0, },
	0
};

Cpu *
ourcpu(void)
{
	Cpu *cpu;

	if (sys->cpu)
		return sys->cpu;
	for (cpu = cpus; cpu->name; cpu++)
		if (cpu->vendorid == sys->vendorid &&
		    cpu->archid == sys->archid)
			return sys->cpu = cpu;
	return nil;
}

Mallocs
cachealloc(uintptr size)		/* unused */
{
	char *p;

	p = malloc(size + 2*CACHELINESZ);
	if (p == nil)
		return (Mallocs){ nil, nil };
	return (Mallocs){ p, p + CACHELINESZ };
}

static uint
umod(uint a, uint b)			/* unused */
{
	if (ISPOW2(b))
		return a & (b - 1);
	return a % b;
}

/* prevent interrupts in M or S mode */
void
nointrs(Intrstate *is)
{
	is->machmode = m->machmode;
	if (is->machmode) {
		is->osts = getmsts();
		putmsts(is->osts & ~Mie);
	} else
		is->pl = splhi();
}

void
restintrs(Intrstate *is)
{
	if (is->machmode)
		putmsts(is->osts);
	else
		splx(is->pl);
}

static void
memcacheinit(L2cache *l2c, int lvl, char *name)
{
	int enabways, maxways, newways;

	if (l2c == nil)
		return;
	enabways = (l2c->wayenable & MASK(8)) + 1;
	maxways = (l2c->config>>8) & MASK(8);
	newways = maxways - Reservedways;
	if (enabways < newways) {		/* cannot reduce enabled ways */
		if (lvl > 2)
			iprint("cpu%d: %d of %d %s cache ways enabled at entry; "
				"setting to %d\n", m->machno, enabways, maxways,
				name, newways);
		coherence();
		/* index of last way enabled for cache */
		l2c->wayenable = newways - 1;
		coherence();
	}
}

static void
nol2cache(void)
{
	print("l2init: no cache flush mechanism ");
	if (dmaincoherent)
		print("and it's needed for DMA.\n");
	else
		print("but it's not needed (yay!).\n");
}

/* configure l2 & l3 caches; called by each hart in case of l2 caches */
void
l2init(void)
{
	L2cache *l2c = (L2cache *)soc.l2cache;
	char cacheline[CACHELINESZ*3];

	if (m->machno == 0) {
		if (haveinstr(cboflush, (uvlong)bdata))
			soc.havecbom = 1;
		if (haveinstr(cbozero, (uvlong)&cacheline[CACHELINESZ]))
			soc.havecboz = 1;
	}
	if (l2c != nil) {
		if (m->machno == 0)
			memcacheinit(l2c, 2, "shared l2");
	} else {
		/* just configure this hart's private l2 cache */
		if ((uint)m->hartid < nelem(soc.pl2caches))
			l2c = (L2cache *)soc.pl2caches[m->hartid];
		if (l2c == nil) {
			if (m->machno == 0)
				nol2cache();
			return;
		}
		memcacheinit(l2c, 2, "private l2");
	}
	if (m->machno == 0)
		memcacheinit((L2cache *)soc.l3cache, 3, "shared l3");
}

enum {
	Pacleaninv	= 3LL,	/* operation for pl2 cache */
	L2opshft	= 56,
};

/*
 * assumes it's okay to apply op to every cacheline from vaddr to vaddr+len-1.
 * if that isn't true for op, be sure to treat any partial leading and trailing
 * cache line fragments separately.
 */
void
cacheop(void (*op)(void *), void *vaddr, uintptr len)	/* needs Zicbom */
{
	uintptr addr, last;
	Mpl pl;

	coherence();
	if (ensurehigh(op) != ensurehigh(cbozero) && !FLUSHSTRAT() ||
	    len <= 0 || vaddr == 0)
		return;

	vaddr = cachedview(vaddr);
	pl = splhi();
	addr = (uintptr)vaddr;
	last = ROUNDUP(addr + len, CACHELINESZ); /* next line; may overshoot */
	addr = ROUNDDN(addr, CACHELINESZ);	/* start of first cache line */
	for (; addr < last; addr += CACHELINESZ)
		(*op)((void *)addr);
	coherence();
	splx(pl);
}

/* can't perform cache ops on uncached memory */
int
isuncached(void *va)
{
	/* uncached view region?  for p550 */
	if (soc.uncached && va >= KADDR(soc.uncached))
		return 1;
	/* uncached pages (via PteNc)? */
	return m->ncmapped && sys->ncbase &&
		(uintptr)va >= sys->ncbase && (uintptr)va < sys->ncend;
}

/*
 * if dma is incoherent on this machine, flush any l2 and l3 caches from virtual
 * address `vaddr' for `len' bytes.  "flush" means "write back the cache line if
 * dirty, then invalidate it".  not needed on most (that is, risc-v conforming)
 * systems.  at minimum, always call coherence().
 */
void
cachedwbinvse(void *vaddr, uintptr len)
{
	uvlong pl2cmd, plccmd;
	uintptr addr, last;
	L2cache *l2c, *l3c, *lcp;
	Mpl pl;

	coherence();
	if (!FLUSHSTRAT())
		return;
	vaddr = cachedview(vaddr);
	if (soc.havecbom) {
		cacheop(cboflush, vaddr, len);
		return;
	}

	if (vaddr == nil || len == 0)
		return;

	/* pl2cmd should be Pacleaninv for private l2 only, else 0. */
	pl2cmd = 0;
	l2c = (L2cache *)soc.l2cache;
	if (l2c == nil) {		/* no shared l2? try private */
		/* if efficiency is an issue, could cache l2c in m->l2cache */
		l2c = (L2cache *)soc.pl2caches[m->hartid];
		if (l2c == nil)
			return;
		pl2cmd = Pacleaninv;
	}

	addr = (iskern(vaddr)? PADDR(vaddr): (uintptr)vaddr);
	last = ROUNDUP(addr + len, CACHELINESZ); /* next line; may overshoot */
	addr = ROUNDDN(addr, CACHELINESZ);	/* start of first cache line */
	if (pl2cmd) {
		/* addr is phys, upper 8 bits are l2 cache op in private l2 */
		assert((addr & (VMASK(8)<<L2opshft)) == 0);
		pl2cmd <<= L2opshft;
	}

	/*
	 * a dirty line in l2 should also be in l3, so flushing l3 and
	 * invalidating the line in all caches should suffice.  this is
	 * what l3c->flush64 does.
	 *
	 * all flush64 registers invalidate the line at their level and
	 * all below (outer to inner), after the write-back(s).
	 */
	l3c = (L2cache *)soc.l3cache;
	if (l3c) {
		lcp = l3c;
		plccmd = 0;
	} else {
		lcp = l2c;
		plccmd = pl2cmd;
	}
	pl = splhi();
	for (; addr < last; addr += CACHELINESZ) {
		if (Paranoid && l3c) {		/* private l2? */
			l2c->flush64 = addr | pl2cmd;
			coherence();
			if (pl2cmd)
				while (l2c->flushcnt > 0)
					;
		}
		lcp->flush64 = addr | plccmd;
//		coherence();
	}
	coherence();
	splx(pl);
}

void
cachedwbse(void *vaddr, uintptr len)		/* needs Zicbom */
{
	coherence();
	if (!FLUSHSTRAT())
		return;
	vaddr = cachedview(vaddr);
	if (soc.havecbom)
		cacheop(cboclean, vaddr, len);
	else
		cachedwbinvse(vaddr, len);	/* gratuitous invalidate */
}

void
cachedinvse(void *vaddr, uintptr len)	/* needs Zicbom */
{
	coherence();
	if (!FLUSHSTRAT())
		return;
	vaddr = cachedview(vaddr);
	if (soc.havecbom)
		cacheop(cboinval, vaddr, len);
	else if (TODO)				/* (Paranoid) */
		/*
		 * we have only a combined flush, so should do nothing, to avoid
		 * writeback.
		 */
		cachedwbinvse(vaddr, len);	/* gratuitous wb */
}

/* zero with Zicboz if available in hopes of greater speed */
void *
zero(void *vaddr, uintptr len)
{
	uintptr startcl, endcl, start, end;

	if (!soc.havecboz || isuncached(vaddr) || len < CACHELINESZ)
		return memset((void *)vaddr, 0, len);

	if (((uintptr)vaddr | len) % CACHELINESZ == 0) {
		cacheop(cbozero, (void *)vaddr, len);	/* optimal case */
		return vaddr;
	}

	start = (uintptr)vaddr;
	startcl = ROUNDUP(start, CACHELINESZ);	/* may leave partial */

	/* partial cache line? zero to start of next cache line */
	if (startcl > start)
		memset((void *)start, 0, startcl - start);

	end = start + len;
	endcl = ROUNDDN(end, CACHELINESZ);	/* may leave partial */
	if (endcl > startcl)
		cacheop(cbozero, (void *)startcl, endcl - startcl);

	/* partial cache line? zero from start of end's cache line */
	if (end > endcl)
		memset((void *)endcl, 0, end - endcl);
	return vaddr;
}

int
ishartonline(int hart)		/* unused */
{
	return harts[hart].running;
}

/* cpu-specific setup for this cpu */
void
cpuinit(int cpu)
{
	Hart *hartst;

	trapvecs();			/* set m and stvec */
	trapsclear();			/* turns clock off */
	sys->machptr[cpu] = m;		/* publish for other cpus */
	aadd(&sys->nmach, 1);

	hartst = &harts[m->hartid];
	hartst->state = Hstarted;	/* i must be running */
	hartst->running = 1;
	coherence();
	hartst->havestate = 1;
	coherence();
	DBG("[cpu%d hart%d]\n", cpu, m->hartid);
	m->boottsc = rdtsc();
	m->bootrdtime = rdtime();

	m->plicctxt = mach2context(m);	/* base context without priv mode */
	csrswap(SCOUNTEREN, ~0ull);
}

/*
 * CLINTLONGS() produces 32-bit little-endian accesses for unmodified tinyemu
 * (not ours) and xuantie.  Also implies reading the time csr instead of mtime.
 */

static void
setcltime(ulong *p, uvlong v)
{
	if (CLINTLONGS()) {
		p[1] = VMASK(31);	/* don't trigger premature clock intr */
		p[0] = ~0ul;
		coherence();
		p[1] = v>>32;
		p[0] = v;
	} else
		*(uvlong *)p = v;
	coherence();
}

void
wrcltime(uvlong v)
{
	if (soc.c910)		/* on c910, mtime is a csr, not memory-mapped */
		return;
	if (nosbi)
		setcltime((ulong *)&m->clint->mtime, v);
}

uvlong
clrdcltimecmp(Mach *mp)
{
	ulong *p;

	/* seems not to work under OpenSBI, though undocumented */
	p = (ulong *)&m->clint->mtimecmp[mp->hartid];
	if (CLINTLONGS())
		return p[0] | (uvlong)p[1] << 32;
	else
		return *(uvlong *)p;
}

/* sbi provides no way to read mtimecmp, so read cached value */
#define RDCLTIMECMP(mp) (nosbi? clrdcltimecmp(mp): mp->timecmp)

void
wrcltimecmp(uvlong v)
{
	if (m->clint == nil)
		panic("wrcltimecmp: nil m->clint");
	setcltime((ulong *)&m->clint->mtimecmp[m->hartid], v);
	m->timecmp = v;		/* remember for RDCLTIMECMP() under sbi */
}

void
setclinttmr(uvlong clticks)
{
	if (nosbi)
		wrcltimecmp(clticks);
	else
		sbisettimer(clticks);	/* how long does this take? */
	m->timecmp = clticks;	/* remember for RDCLTIMECMP() under sbi */
	coherence();		/* Stip might not be extinguished immediately */
}

/*
 * compute timing parameters.
 */
void
calibrate(void)
{
	if (sys->clintsperhz != 0)
		return;
	if (soc.newmach) {
		vlong i;
		vlong tsc, rdtm, mtime, tsc2, rdtm2, mtime2;

		RDTIMES(tsc, rdtm, mtime);
		for (i = 500*1000; i > 0; i--)
			;
		RDTIMES(tsc2, rdtm2, mtime2);

		mtime2 -= mtime;
		rdtm2 -= rdtm;
		tsc2 -= tsc;
		print("%,llud clint ticks is %,llud tsc cycs and %,llud rdtime ticks\n",
			mtime2, tsc2, rdtm2);
		print("timebase given as %,llud Hz\n", timebase);
	}

	/* compute constants for use by timerset & idle code */
	assert(timebase >= MHZ);
	sys->clintsperhz = timebase / HZ;	/* clint ticks per HZ */
	sys->clintsperµs = timebase / MHZ;
	sys->clthresh = (soc.idlewakens * sys->clintsperµs) / 1000;

	/*
	/* min. interval until intr; was /100 but made too many intrs.
	 * Does minclints need to be less than timebase/HZ?  It allows
	 * shorter and more precise sleep intervals, e.g., for clock0link
	 * polling.  To keep the interrupt load and interactive response
	 * manageable, it needs to be somewhat > 0.
	 */
	sys->minclints = sys->clintsperhz / 4;
}

/* prevent further clock interrupts */
void
clockoff(void)
{
	clrstie();
	/* ~0ull makes sense, but looks negative on some machines */
	setclinttmr(VMASK(63));
}

/*
 *  set next timer interrupt for time next, in fastticks (clint ticks).
 *  we won't go longer than 1/HZ s. without a clock interrupt.
 *  as a special case, next==0 requests a small interval (e.g., 1/HZ s.).
 */
void
timerset(uvlong next)
{
	Mpl pl;
	vlong fticks, curticks, newticks;

	pl = splhi();
	if (sys->clintsperhz == 0)
		panic("timerset: sys->clintsperhz not yet set");
	if (next == 0)
		fticks = sys->clintsperhz;
	else {
		fticks = next - fastticks(nil);
		/* enforce sane bounds: 1/(4*HZ) ≤ s. ≤ 1/HZ */
		if (fticks < (vlong)sys->minclints)
			/* don't interrupt immediately or in the past */
			fticks = sys->minclints;
		else if (fticks > (vlong)sys->clintsperhz)
			fticks = sys->clintsperhz;
	}
	/* extinguish current intr source and set new deadline */
	/* don't delay past already scheduled time */
	newticks = rdcltime() + fticks;
	curticks = RDCLTIMECMP(m);
	if (newticks < curticks)
		setclinttmr(newticks);
	clrsipbit(Stie);		/* dismiss current intr */
	clockenable();
	splx(pl);
}

long ticktock;				/* set by M clock intr */

int
clocksanity(void)
{
	int i;
	uvlong omtime, now;

	assert(timebase >= MHZ);
	clockoff();
	m->clockintrsok = 1;

	omtime = now = rdcltime();
	clockenable();
	setclinttmr(omtime + 20*timebase/MHZ);

	for (i = 0; i < 1000 && (now = rdcltime()) == omtime; i++)
		delay(1);
	if (now <= omtime)
		panic("clint clock is not advancing (was %llud is %llud)",
			omtime, now);

	for (i = 100; (getsip() & (Mtie|Stie)) == 0 && i > 0; i--)
		delay(1);
	if ((getsip() & (Mtie|Stie)) == 0 && ticktock == 0)
		panic("clint clock not interrupting");
	timerset(0);

	if (m->machno == 0)
		if (timebase >= MHZ && timebase % MHZ == 0)
			print("clint timebase: %,lld MHz\n", timebase/MHZ);
		else
			print("clint timebase: %,lld Hz\n", timebase);
	return 1;
}

static int mwords[1];
static int *mwaitwd = mwords;		/* make safe from the start */

/*
 * wait for an interrupt, which conserves power, thus heat.
 * an interrupt will resume after WFI.  assume
 * individual interrupt bits of interest are set in SIE.
 * currently on risc-v, we can only wait for an interrupt.
 * see the privileged ISA spec for WFI; it's a bit subtle.
 * in particular, WFI may pause the core's cycle counter,
 * and it can be implemented as a NOP.
 *
 * note that halt, ainc, adec, and the spl* functions contain fences.
 */

static ulong
idlenowakehands(void)
{
	ulong ip;

	coherence();		/* make our changes visible to other harts */
	if ((getsie() & Superie) == 0)
		panic("sie csr has no S intrs enabled: %#p\n", getsie());
	while (((ip = getsip()) & Superie) == 0)
		halt();
	coherence();		/* make other harts' changes visible to us */
	/* can't call intrclknotrap: needs non-nil Ureg* */
	if (ip & Seie)
		intrnotrap();
	return ip;
}

static ulong idlecpus;		/* bitmap of waiting cpus */

static void
idlewakehands(void)
{
	ulong ip, cpubit;

	/*
	 * ask for an ipi from another cpu if work becomes possibly available.
	 * if no other devices or processors interrupt, the clock will.
	 */
	cpubit = 1ull << m->machno;
	amoorw(&idlecpus, cpubit);
	m->ipiwait = 1;
	ip = idlenowakehands();

	/*
	 * some interrupt popped us out of wfi at splhi, if we were in wfi.  if
	 * we popped out of wfi due to an ipi, clear it.  as a side-effect, if
	 * we waited in wfi, the ipi won't have been automatically counted for
	 * irqalloc since it wasn't serviced, but ipiwait will have been zeroed
	 * when sending the ipi.
	 */
//	if (amoswapw(&m->ipiwait, 0) == 0) {
	if (m->ipiwait == 0) {
		countipi();			/* were still waiting */
//		procipimsgs();
	}
	m->ipiwait = 0;
	amoandnw(&idlecpus, cpubit);
	if (ip & (Ssie|Msie))
		clearipi();
}

/*
 *  put the processor in the halt state if we've no processes to run.
 *  an interrupt will get us going again.  the clock interrupt every 1/HZ s.
 *  puts an upper bound on the wait time.
 *  called at spllo from proc.c/runproc.
 */
void
idlehands(void)
{
	Mpl pl;

	if (nrdy != 0 || active.exiting || active.rebooting)
		return;
	pl = splhi();
	if (soc.idlewakens && sys->nonline > 1)
		idlewakehands();
	else
		idlenowakehands();
	splx(pl);
}

/*
 * wake some cpus in wfi state.  an interrupt will make one cpu break
 * out of wfi, but a qunlock probably should wake any idlers too.
 *
 * idlewake is called from the locking primitives, among others, so we
 * can't use them within it.  however, note that on riscv64, ainc and adec
 * use atomic memory operations, not locks.
 *
 * spread the load around, taking into account that there may
 * be other cpus also running idlewake.  assume there's no point in waking
 * more cpus than currently-runnable processes.
 * only interrupt cpus advertising that they are waiting for an ipi.
 *
 * if we use sbi, this currently only works for first 64 cpus.  by the time
 * we have that many cpus, we should have better sbi HSM functions available.
 */
void
idlewake(void)
{
	int left;
	uint cpu, nonline;
	uvlong hartbm, imminent, idlebits, now;
	Mach *mp;
	Mach **machptr;
	static int waking;	/* flag: some cpu is waking. lock with _tas */
	static uvlong lastwakecl;

	if (FUTURE)		/* we don't currently emulate x86 mwait */
		++*mwaitwd;	/* need not be atomic increment even on x86 */
	nonline = sys->nonline;
	if (soc.idlewakens == 0 || nonline <= 1 || (left = nrdy) == 0 ||
	    lastwakecl > (now = rdcltime()) - sys->clthresh ||
	    TAS(&waking) != 0)
		return;		/* another cpu is waking or was just waking */

	/*
	 * this cpu can run one runnable process, so wake up enough idling cpus
	 * to run the other ready ones, if available.  this is somewhat
	 * approximate as the other cpus may change state underfoot.
	 */
	if (left >= nonline)
		left = nonline - 1;
	imminent = now + sys->clthresh;

	/*
	 * if a cpu is awaiting an ipi (or other interrupt) and doesn't
	 * have an imminent clock interrupt, wake it.
	 * assume on-line cpus are 0 — nonline-1.
	 */
	hartbm = 0;
	machptr = sys->machptr;
	for (idlebits = idlecpus; idlebits && left > 0;
	    idlebits &= ~(1ull << cpu)) {
		cpu = Clzbits - 1 - clz(idlebits);
		if (cpu >= nelem(sys->machptr))
			break;
		mp = machptr[cpu];
		if (mp == nil || RDCLTIMECMP(mp) <= imminent)
			continue;
		if (nosbi)
			m->clint->msip[mp->hartid] = 1;
		else
			hartbm |= 1ull << mp->hartid;
		mp->ipiwait = 0;
		left--;
	}
	if (hartbm)				/* only set if using sbi */
		/*
		 * wake them all at once.  they'll fight among themselves for
		 * runnable procs, consuming system time; that's okay, it
		 * reduces elapsed time by keeping the cpus busy.
		 */
		sbisendipi(&hartbm);
	lastwakecl = now;
	coherence();
	waking = 0;
}

/*
 * SBI sometimes reports one or two incorrect, random hart(s) as started,
 * especially for the current hart.  Could just start them all; will that work
 * if status is wrong?  It would be good not to have renegade harts running
 * random code, yet we currently have no way to suspend them.
 */

static vlong
gethartstate(int hart)
{
	vlong state;
	Hart *hartst;

	if (m->hartid == hart)
		return Hstarted; /* never query my state: sbi gets it wrong */
	state = sbihartstatus(hart);
	if (state >= 0) {
		hartst = &harts[hart];
		if (hartst->havestate)
			return hartst->state;
		/* sbi lies */
		hartst->state = state;
		hartst->running = (state == Hstarted);
		coherence();
		hartst->havestate = 1;
	}
	return state;
}

/*
 * on p550, sbi lies, so we're trying to fake initial states.
 * if we don't get all harts running, we don't get plic interrupts.
 */
static void
hartstatesinit(void)
{
	int hart, state;
	Hart *hartst;

	assert(m->machno == 0);
	for (hart = soc.hobbled; hart < HARTMAX; hart++) {
		if (m->hartid == hart)
			continue;		/* never query my own state */
		state = sbihartstatus(hart);
		if (state < 0)
			continue;		/* not a real hart */
		hartst = &harts[hart];
		if (hartst->havestate)
			continue;
		hartst->state = (hart == m->hartid? Hstarted: Hstopped);
		hartst->running = hartst->state == Hstarted;
		coherence();
		hartst->havestate = 1;
	}
}

static void
getallhartstates(void)
{
	int hart;

	for (hart = soc.hobbled; hart < HARTMAX; hart++)
		gethartstate(hart);
}

static void
startagain(void)
{
	iprint("resetting the system.\n");
	delay(5);
	/*
	 * we are cpu0 and unsure of the states of other cores,
	 * but a system reset will fix that.
	 */
	archreset();
	notreached();
}

static int
stophart(int hart, int state)
{
	/*
	 * hart is probably started but not running our code, at least
	 * not at our start address.  sbi or u-boot bug?
	 * if only there were a way to reset a single hart...
	 */
	iprint("hart %d in state %s on sbi hsm system; not sending it an IPI.\n",
		hart, hartstates[state]);
	if (!Livedangerously)
		/* there's really nothing we can do but start over and hope. */
		startagain();

	state = gethartstate(hart);
	if (state < 0)
		iprint("can't get hart %d status\n", hart);
	else if (state != Hstopped) {
		iprint("can't stop hart %d in state %s\n",
			hart, hartstates[state]);
		return -1;
	}
	return state;
}

/*
 * start any stopped cpus at _main.  some or all may already be started,
 * but we are going to make the unwarranted assumption that systems
 * with SBI HSM calls only start one cpu initially (the only other
 * legitimate configuration is to start them all at once).
 * if sbi started multiple harts, what address would it start them at?
 * stopped cpus will not be waiting but need to be started.
 *
 * simulate the non-hsm near-simultaneous start up of all harts
 * except 0 if it's hobbled (for running and managing the others).
 */
void
hsmstartall(void)
{
	int hart, state, col;
	Hart *hartst;

	// hartstatesinit();
	getallhartstates();

	/*
	 * print incorrect sbi states, ensure other harts are stopped.
	 * skip possibly weird management hart(s).
	 */
	for (hart = soc.hobbled; hart < HARTMAX; hart++) {
		hartst = &harts[hart];
		state = (hartst->havestate? hartst->state: -1);
		if (state < 0)
			continue;
		/* cpu0's sbi hart state doesn't matter; we don't start it. */
		if (hart != m->hartid &&
		    (state == Hstarted) != hartst->running) {
			iprint("sbi status for hart %d (%s) is wrong; "
				"it's %srunning this kernel\n", hart,
				hartstates[state], hartst->running? "": "not ");
			if (!Livedangerously)
				startagain();
		}
	}

	/* start any stopped harts, and they should all be stopped except me */
	col = 0;
	for (hart = soc.hobbled; hart < HARTMAX; hart++) {
		if (hart == m->hartid) {
			iprint("hart %d is me, already running\n", hart);
			continue;
		}
		hartst = &harts[hart];
		if (hartst->running) {		/* trust hartstate over sbi */
			iprint("hart %d already running\n", hart);
			continue;
		}

		/* we think hart is not running; does sbi agree? */
		state = (hartst->havestate? hartst->state: -1);
		if (state < 0)
			continue;		/* not a real hart */
		if (state != Hstopped && state != Hsuspended)
			if (stophart(hart, state) < 0)
				continue;

		if (sbihartstart(hart, PADDR(_main), hart) < 0)
			/*
			 * opensbi source says failure to start puts a hart
			 * into stopped state, so it should be ready to go now.
			 */
			if (sbihartstart(hart, PADDR(_main), hart) < 0) {
				iprint("sbi can't start hart %d\n", hart);
				continue;
			}
		iprint("sbi started hart %d%s", hart,
			col++ % Hartcols == Hartcols-1? "\n": "\t");
		/* give time to start & bump hartcnt; stagger startups */
		delay(250);
	}
	if (col % Hartcols != 0)
		iprint("\n");
}

static ulong
prifexts(ulong isa, ulong exts, char *name)
{
	if ((isa & exts) == exts) {
		print(name);
		isa &= ~exts;
	}
	return isa;
}

vlong
archhz(void)
{
	l2init();		/* may be private l2 caches per hart */
	if(m->machno != 0)
		return sys->machptr[0]->cpuhz; /* cpus have to be ~identical */
	/* meanwhile, other cpus are spinning */
	return cpuhz;
}

int
haveinstr(void *fn, uintptr arg)
{
	int failed;

	failed = 1;
	probeinstr(fn, arg, &failed);	/* discards fn's return value */
	return !failed;
}

static void
prcpuchar(void)
{
	print("hart %d plic M context %d", m->hartid, m->plicctxt);
}

static void
prcpuhartctxt(void)
{
#ifdef unused
	static Lock idlock;

	ilock(&idlock);
	prflush();
	print("cpu%d: ", m->machno);
	prcpuchar();
	print("\n");
	prflush();
	iunlock(&idlock);
#endif
}

static int
gotvec(int)
{
	putsts(getsts() & ~Vsst | Initial<<Vsshft);
	csrrd(VL);
	putsts(getsts() & ~Vsst | Off<<Vsshft);
	return 0;
}

void
cpuidprint(void)
{
	int bit;
	ulong isa, misa;	/* misa extension bits fit in low long */
	Cpu *cpu;

	if(m->machno != 0) {
		prcpuhartctxt();
		return;			/* cpus have to be ~identical */
	}

	print("cpu%d: risc-v RV%d", m->machno, BI2BY*(int)sizeof(uintptr));
	if (haveinstr(gotvec, 0))
		sys->cpucap |= Capvec;
	if (bootmachmode) {
		if ((sys->extensions >> 62) != Mxlen64)
			print("	misa mxl %lld is not 64-bits\n",
				sys->extensions>>62);
		misa = sys->extensions;
		isa = prifexts(misa, EXTIMA|EXTFD, "G");
		isa = prifexts(isa, EXTIMA, "IMA");
		isa = prifexts(isa, EXTFD, "FD");
		isa = prifexts(isa, EXT('C'), "C");
		isa &= MASK('z'+1-'a');		/* isolate extensions */
		for (bit = 0; bit < 'z'+1-'a'; bit++)
			if (isa & (1<<bit))
				print("%c", 'A' + bit);
		if ((misa & (EXTIMA|EXTFD)) != (EXTIMA|EXTFD))
			panic("cpu is not RV64G: misa %#lux", misa);
		if ((misa & EXTSU) != EXTSU)
			panic("don't have super & user modes; hopeless.");
		if ((misa & EXT('C')) == 0)
			print(" no compression, many binaries won't run.\n");
	} else {
		print("GCSU");			/* can't easily tell */
		if (sys->cpucap & Capvec)
			print("V");
	}
	if (haveinstr(clzzbb, 0)) {
		print("_Zbb");
		archclz = clzzbb;
		sys->cpucap |= Capclz;
	}
	if (soc.havecbom)
		print("_Zicbom");
//		sys->cpucap |= Capcbom;
	if (soc.havecboz)
		print("_Zicboz");
//		sys->cpucap |= Capcboz;
	if (haveinstr(wrsnto, (uintptr)main)) {
		soc.havewrsnto = 1;
		print("_Zawrs");
	}
	if (soc.svpbmt)
		print("_Svpbmt");		/* not that we use it */
	if (soc.nodevamo)
		print(" no_dev_amo");		/* jupiter or other cheapo */
	if (soc.c910 && haveinstr(sync_is, 0))	/* xuantie instr. */
		print(" sync_is");

	sys->cpu = cpu = ourcpu();
	if (cpu)
		print(" %s", cpu->name);
	else {
		print(" unknown make");
		if (sys->vendorid != 0 || sys->archid != 0)
			print(" vendor %#lux arch %#p",
				sys->vendorid, sys->archid);
	}
	print(" ");
	prcpuchar();
	print(" at %d MHz\n", (uint)(cpuhz / MHZ));
}

static void
addpgsz(int lg2)
{
	int npg;

	assert(m->npgsz < NPGSZ);
	npg = m->npgsz++;
	m->pgszlg2[npg] = lg2;
	m->pgszmask[npg] = VMASK(lg2);
}

int
archmmu(void)
{
	int lvl;

	if (m->npgsz == 0) {
		CTASSERT(PGSZ == 4*KB, PGSZ_4K);
		for (lvl = 0; lvl < Npglvls; lvl++)
			addpgsz(PGLSHFT(lvl));
	}
	assert(m->npgsz >= 1);
	return Npglvls;
}

static int
fmtP(Fmt* f)
{
	uintmem pa;

	pa = va_arg(f->args, uintmem);

	if(f->flags & FmtSharp)
		return fmtprint(f, "%#16.16p", pa);

	return fmtprint(f, "%llud", pa);
}

static int
fmtL(Fmt* f)
{
	return fmtprint(f, "%#16.16p", va_arg(f->args, Mpl));
}

static int
fmtR(Fmt* f)
{
	return fmtprint(f, "%#16.16p", va_arg(f->args, Mreg));
}

void
archfmtinstall(void)
{
	/*
	 * Architecture-specific formatting. Not as neat as they
	 * could be (e.g., there's no defined type for a 'register':
	 *	L - Mpl, mach priority level
	 *	P - uintmem, physical address
	 *	R - register
	 * With a little effort these routines could be written
	 * in a fairly architecturally-independent manner, relying
	 * on the compiler to optimise-away impossible conditions,
	 * and/or by exploiting the innards of the fmt library.
	 */
	fmtinstall('P', fmtP);

	fmtinstall('L', fmtL);
	fmtinstall('R', fmtR);
}

/*
 * delay for at least microsecs, placating the watchdog if necessary.
 * sys and lowsys are set in low() at the very start.
 * m is set just a little later, but we may be called first,
 * and the mmu may be off.
 */
void
microdelay(vlong microsecs)
{
	uvlong t, now, nxtdog, dogincr;

	if (microsecs <= 0)
		return;

	now = rdtime();
	t = now + microsecs * clintsperµs;	/* target in clint ticks */
	/* when islo(), cpu0 clock interrupts will restart the dog */
	if (islo() || m == nil || m->machno != 0 ||
	    !watchdogon || watchdog == nil) {
		while (rdtime() < t)
			pause();
		return;
	}

	dogincr = Wdogms * 1000LL * clintsperµs;
	nxtdog = now + dogincr;
	for (; now < t; now = rdtime()) {
		if (now > nxtdog) {
			watchdog->restart();
			nxtdog = now + dogincr;
		}
		pause();
	}
}

void
millidelay(int millisecs)
{
	if (millisecs)
		microdelay(1000LL * millisecs);
}

/* the Zbb extension provides a CLZ instruction */
int
portclz(Clzuint n)			/* count leading zeroes */
{
	/* (u)vlong makes jc generate better code than (u)int */
	uvlong cnt, hibits;
	Clzuint mask;

	if (n == 0)
		return Clzbits;
	cnt = 0;
	mask = VMASK(Clzbits/2) << (Clzbits/2);
	/* this will take at most log2(Clzbits) iterations */
	for (hibits = Clzbits/2; hibits > 0; ) {
		if ((n & mask) == 0) {
			/* highest bits are zero; count and toss them */
			cnt += hibits;
			n <<= hibits;
		}
		/* halve mask width for next iteration */
		hibits /= 2;
		mask <<= hibits;
	}
	return cnt;
}

int
clz(Clzuint n)			/* must be a function for port code */
{
	return (*archclz)(n);
}
