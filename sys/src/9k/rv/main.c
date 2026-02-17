/*
 * initialise a risc-v RV64G system, start all cpus, and
 * start scheduling processes on them, notably /boot/boot as process 1.
 * also contains graceful shutdown and reboot code.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "io.h"
#include "ureg.h"
#include "riscv64.h"

#include "init.h"
#include "reboot.h"
#include <a.out.h>
#include <ctype.h>

// #include "../port/dbgprint.h"

#define ncflush()		/* optimisation since nc is useless */

int	i8250getc(Uart *);
int	sifivegetc(Uart *);

#ifdef SIFIVEUART
Uart	sifiveuart[];
#define UARTGETC() sifivegetc(&sifiveuart[0])
#else
Uart	i8250uart[];
#define UARTGETC() i8250getc(&i8250uart[0])
#endif

extern char defnvram[];		/* from kernel config */

int	hartcnt = 0;		/* machno allocator; init to avoid bss */

Sys* sys = nil;			/* initializer keeps sys out of bss */
uintptr sizeofSys = sizeof(Sys);
uintptr sizeofSyspercpu = sizeof(Syspercpu);

char cant[] = "can't happen: notreached reached\n";
char cputype[] = "riscv64";
Watchdog *watchdog = nil;

/*
 * Option arguments from the command line (obsolete).
 * oargv[0] is the boot file (/boot/boot).
 * mboptinit() was called from multiboot() on amd64 to set it all up.
 */
static int oargc;
static char* oargv[20];
static char oargb[128];
static int oargblen;

static uintptr usp;		/* user stack of init proc */

static void	probemem(void);

/* dreg from parsing multiboot options on pcs */
void
mboptinit(char* s)
{
	oargblen = strecpy(oargb, oargb+sizeof(oargb), s) - oargb;
	oargc = tokenize(oargb, oargv, nelem(oargv)-1);
	oargv[oargc] = nil;
}

void
fakecpuhz(void)
{
	/*
	 * Need something for initial delays
	 * until a timebase is worked out.
	 */
	m->cpuhz = cpuhz;
	m->cpumhz = cpuhz / MHZ;
	m->perf.period = 1;
}

static vlong
sethz(void)
{
	vlong hz;

	hz = archhz();
	if(hz != 0){
		m->cpuhz = hz;
		m->cpumhz = hz / MHZ;
	}
	return hz;
}

static void
setepoch(void)
{
	if (m->machno == 0 && sys->epoch == 0) {
		sys->ticks = m->ticks = 0;
		/* clint->mtime is only zeroed upon reset on some systems */
		wrcltime(1);		/* attempt to reset */
		coherence();
		sys->epoch = rdcltime();
	}
}

static void
mptimesync(void)
{
	if (m->machno == 0) {
		sys->timesync = rdcltime();
		coherence();
	}
}

/*
 * start a cpu other than 0.  entered splhi with initial high id map page table,
 * zeroed low user map, and with PC in KZERO.  m->ptroot is already set to our
 * private root page table, inherited from cpu0, with PteNc enabled if
 * appropriate.
 */
void
squidboy(uint machno)
{
	lock(&active);
	cpuactive(machno);
	unlock(&active);

	/*
	 * starting here, we are using kernel addresses, and they should
	 * be upper addresses, but won't be until mmuinitap, which
	 * switches to a private copy of cpu0's page table.
	 */
	archmmu();			/* work out page sizes available */
	/* can't invalidate nc region aliases here */
	/* zeros user mapping. returns executing in high addresses with vmap */
	mmuinitap();

	DBG("Hello Squidboy %d\n", machno);
	if(sethz() == 0)
		panic("squidboy: cpu%d: 0Hz", machno);
	cpuidprint();
	fpuinit();
	/* can't invalidate nc region aliases here */
	ncinit();

	/*
	 * Handshake with main to proceed with initialisation.
	 */
	DBG("mach %d waiting for epoch\n", machno);
	/* sys->timesync is set on cpu0 in mptimesync from multiprocinit (1) */
	while(sys->timesync == 0)
		pause();
	mptimesync();

	/*
	 * Cannot allow interrupts while waiting for online.
	 * A clock interrupt here might call the scheduler,
	 * and that would be a mistake.
	 */
	ncflush();
	/* m->online will be set on cpu0 in onlinewaiting from schedcpus (2) */
	DBG("mach%d: waiting for thunderbirdsarego\n", machno);
	/* thunderbirdsarego set on cpu0 in main before schedinit (3) */
	while (!active.thunderbirdsarego)
		pause();
	ncflush();
	if (hartinit)
		hartinit();
	DBG("mach%d: online color %d is go %#p %#p\n",
		machno, m->color, m, m->ptroot->va);

	timersinit();	/* uses m->cpuhz; set up HZ timers on this cpu */
	clocksanity();			/* enables clock & allows clock intrs */
	DBG("[cpu%d scheding after %lld cycles]\n", machno,
		rdtsc() - m->boottsc);

	/*
	 * stagger startup to let cpu0 get to schedinit first and
	 * to de-synchronise the processors.
	 * on the vf2 (1.2GHz), the best multiplier so far is 2.
	 */
	microdelay((2*TK2MS(1)*1000/sys->nmach) * m->machno + 4000);

	setsie(Superie);
	setsts();

	schedinit();			/* no return */
	panic("cpu%d: schedinit returned", machno);
}

/*
 * for cpu 0 only.  sets m->machno and sys->machptr[], and
 * needs 'up' to be set to nil.  Mach already zeroed by low.c.
 */
void
machsysinit(void)
{
	m->machno = 0;
	sys->machptr[0] = &sys->mach;
	m->stack = PTR2UINT(sys->machstk);
	sys->nonline = 0;
	cpuactive(0);
	m->online = 1;
	sys->copymode = 0;			/* COW */
}

static int
onlinewaiting(int machno)
{
	int tries;
	Mach *mp;

	mp = sys->machptr[machno];
	if(mp == nil) {
		// DBG("cpu%d non-existent...", machno);
		return 0;
	}
	if (mp->machno != machno)
		panic("schedpus: cpu%d: Mach not initialised", machno);
	for (tries = 500; tries-- > 0 && !mp->waiting; ) {
		DBG("cpu%d not yet waiting...", machno);
		delay(100);
		coherence();
	}
	if (tries <= 0)
		panic("schedpus: cpu%d not starting; mp->waiting %d",
			machno, mp->waiting);
	if(mp->online) {
		DBG("cpu%d already online...", machno);
		return 0;
	}
	/* we are waiting in squidboy() */
	if (mp->hartid < soc.hobbled) {		/* mgmt hart? */
		DBG("cpu%d is mgmt hart...", machno);
		return 0;
	}

	/* cpu is waiting but not yet on-line, so bring it up */
	mp->color = corecolor(machno);
	mp->online = 1;
	mp->waiting = 0;
	coherence();

	DBG("%d on with mach %#p...", machno, mp);
	return 1;
}

/*
 * Release the hounds.  After boot, secondary cpus will be
 * looping waiting for cpu0 to signal them.
 */
static void
schedcpus(int ncpus)
{
	int machno, running, tries;

	running = 1;			/* cpu0 */
	USED(running, ncpus);
	if (MACHMAX <= 1 || running == ncpus)
		return;

	/* if rebooting, let other cpus jump to entry from reboottramp */
	sys->secstall = 0;  /* let them start then poll m->online */
	coherence();
	delay(50);
	sys->secstall = 0;
	coherence();
	/*
	 * even on a fairly slow machine, a cpu can start in under .6 s.
	 * the range on 600MHz icicle with debugging prints:
	 * 236244054-323670622 cycles = .39-.54 s.
	 * be tolerant of delays due to debug printing or staggered startups.
	 */
	for (tries = 0; running < ncpus && tries < 150; tries++) {
		delay(100);	/* let them indicate presence; be patient */
		for(machno = 1; machno < ncpus; machno++) /* skip me (cpu0) */
			running += onlinewaiting(machno);
	}
	iprint("%d cpus running, %d missing\n", running, ncpus - running);
}

/*
 * start secondary cpus.  assumes no kprocs will be started (e.g., due to
 * devtabreset) until schedinit runs init0 (see userinit).
 *
 * kernel memory mappings (including in VMAP) must be fixed when multiprocinit
 * is called, since they will be copied for each cpu before it starts.  drivers
 * typically call vmap in their reset (`pnp') functions, called from
 * devtabreset.
 *
 * we ensure that the secondary cpus do not schedule until userinit (and ideally
 * schedinit) have run on cpu0.
 *
 * allocate memory for and awaken any other cpus, if needed.  copy the page
 * table root to a private copy for each cpu.  hartcnt global is incremented in
 * start.s by each hart.
 */
static void
multiprocinit(void)
{
	int ncpus;

	/*
	 * during a reboot, secondary cpus will stall until we're ready.
	 * at cold boot, we may have to start them.
	 */
	if (soc.havesbihsm && sys->rebooting != RBFLAGSET)
		hsmstartall();
	sys->rebooting = 0;

	/*
	 * when we get here, hartcnt should be correct and secondary
	 * cpus should be running, if only spinning while waiting.
	 */
	ncpus = hartcnt;
	cpusalloc(ncpus);
	mptimesync();		/* signal secondaries via sys->timesync (1) */
	schedcpus(ncpus);	/* signal secondaries via m->online (2) */
	/* the third synchronization step is in main, after userinit */
}

static void	null(void *, uintptr) {}
static void	nullv1(void *) {}
static void	nulli1(uintptr) {}

static void *
vnull(void *va)
{
	return va;
}

Incoher incohersw[] = {
[Uncnone]  { null, null, vnull, vnull, malloc, free, },
[Uncflush] { cachedinvse, cachedwbse, vnull, vnull, malloc, free, },
// [Uncptenc] { null, null, vnull, vnull, ucalloc, ucfree, },
[Uncview]  { null, null, cachedview, uncachedview, malloc, free, },
};

/*
 * early initialization after zeroing BSS.  most of this set up is
 * for system-wide resources, thus only done once.  cpuinit runs first.
 *
 * Printinit should cause the first malloc call to happen (printinit->
 * qopen->malloc).  If the system dies here, it's probably due to malloc
 * not being initialised correctly, or the data segment is misaligned
 * (it's amazing how far you can get with things like that completely
 * broken).
 */
static void
cpu0init(void)
{
	early = 0;
	if (soc.lowdebug && sys->rebooting == RBFLAGSET)
		iprint("system is rebooting.\n");
	plicoff();

	fmtinstall('N', unitsconv);
	machsysinit();
	probemem();		/* useful when debugging on new hardware */
	physmeminit();
	/* we know physical memory size and end (sys->pmend) here */
	setkernmem();

	probesvpbmt(sys);		/* pointless feature */
	/* from here, region below sys will be uncached */

	putsscratch((uintptr)m);
	putstvec(strap);		/* high virtual */

	ptrootinit();
	usevirtdevaddrs();
	chooseincoher();
	ncinit();			/* zero user mappings */

	m->color = corecolor(0);

	mboptinit("/boot/ipconfig ether");	/* dreg from pc multiboot */

	/* device discovery hasn't happened yet, so be careful. */
	i8250console("0");
	fmtinit();			/* install P, L, R, N, Q verbs */
	/*
	 * if we linked with prf.$O, logging will only start once
	 * mallocinit is called from meminit.
	 */
	kmesginit();
	/* cpuactive(0) is now called by machsysinit */
	active.exiting = 0;
	print("\nPlan 9 for %s\n\n", RVARCH);

	/* if epoch is set after timersinit, timers will be confused */
	setepoch();		/* get notions of time right here */
	sethz();		/* also configures any l2 cache */
	calibrate();

	/*
	 * Mmuinit before meminit because it makes mappings and flushes
	 * the TLB.  It will get page table pages from Sys as needed.
	 */
	mmuinit();	/* uses kernmem; sets sys->vm*; vmaps soc devices */
	meminit();		/* map KZERO to ram, call mallocinit */
	archinit();		/* populate #P with cputype */
	trapinit();
	if (socinit)
		socinit();	/* turn on bloody clock signals */
	if (hartinit)
		hartinit();
	printinit();	/* actually, establish cooked console input queue */
}

static void
chooseidler(void)
{
	print("idling with WFI,");
	if (soc.idlewakens == 0)
		print(" not");
	print(" sending IPIs (over %ld ns. to next tick)\n", soc.idlewakens);
}

static void
prearlypages(void)
{
	if (sys->freepage)
		iprint("%llud/%d early pages allocated (usually by vmap)\n",
			(Ptepage *)ensurehigh(sys->freepage) -
			(Ptepage *)sys->pts,
			EARLYPAGES);
}

/*
 * copy reboot trampoline function to its expected location.
 * a reboot could be requested at any time, and all cores will
 * jump to the trampoline.  some of them may still be executing there,
 * so only overwrite if necessary.
 */
static void
copyreboottramp(void)
{
	if (memcmp(sys->reboottramp, rebootcode, 256) != 0)
		memmove(sys->reboottramp, rebootcode, sizeof(rebootcode));
	cacheflush();
}

enum {
	Memstride = 256*MB,
};

/*
 * this is not entirely safe; if it crashes, don't call it.
 * nevertheless, it can be a help when documentation is unclear.
 *
 * probing in super mode could work if sbi or hw doesn't intercept access traps,
 * or forwards them to super.  in super mode, low id map must be in place.
 * probing in super mode on jupiter wedges.  even on vf2, it yields incorrect
 * results.  writing memory and reading back might fix that.
 */
static void
probemem(void)
{
	uintptr addr, start, end;

	if (!bootmachmode)
		return;
	print("readable-memory map to %N: ", ADDRSPCSZ);
	for (start = end = addr = membanks[0].addr; addr < ADDRSPCSZ;
	    addr += Memstride) {
		if (probeulong((ulong *)addr, Read) < 0)
			break;
		end = ROUNDDN(addr + sizeof(long), Memstride);
		/* probe last word of range on next iteration */
		addr = end - sizeof(long);
	}
	print("%#p-%#p size %N\n", start, end, end - start);
}

static int
ismissing(uvlong *addr, char *name)
{
	int ret;

	ret = probeuvlong(addr, Read) < 0;
	if (ret)
		print(" no");
	print(" %s", name);
	return ret;
}

static int
ismissinglong(ulong *addr, char *name)
{
	int ret;

	ret = probeulong(addr, Read) < 0;
	if (ret)
		print(" no");
	print(" %s", name);
	return ret;
}

enum {
	Probedebug = 0,
};

int
probeuvlong(uvlong *addr, int wr)
{
	uvlong old;
	Mpl pl;

	pl = splhi();
	if (Probedebug) {
		iprint("probing %#p...", addr);
		delay(100);
	}
	m->probing = 1;		/* set probebad on any exception */
	m->probebad = 0;
	old = 0x0102030405060708ull;
	USED(old);
	coherence();

	old = *addr;
	if (wr)
		*addr = old;	/* rewrite word, in hopes of doing no harm */
	coherence();		/* should fault by now if addr is bad */

	m->probing = 0;
	if (Probedebug) {
		iprint(m->probebad? "missing\n": "present\n");
		delay(100);
	}
	splx(pl);
	if (m->probebad) {
		m->probebad = 0;
		return -1;
	} else
		return 0;
}

/*
 * pmp may restrict which, if any, parts of the clint register space we can use,
 * depending on how paternalistic the vendor is.
 */
void
probeclint(void)
{
	int longs;

	if (m->machno != 0 || soc.clintlongsset)
		return;
	print("clint mmio access:");
	longs = 0;			/* optimistic before probes */
	if (ismissinglong(m->clint->msip, "msip;"))
		soc.ipiclint = 0;
	else if (probeulong(m->clint->msip, Read) < 0)
		longs = 1;

	if (!ismissing(&m->clint->mtime, "mtime;") &&
	    probeuvlong(&m->clint->mtime, Read) < 0)
		longs = 1;
	if (!ismissing(m->clint->mtimecmp, "mtimecmp") &&
	    probeuvlong(m->clint->mtimecmp, Read) < 0)
		longs = 1;
	soc.clintlongs = longs;
	coherence();
	soc.clintlongsset = 1;
	if (soc.clintlongs)
		print(" (using 32-bit accesses)");
	print(".\n");
}

/* are unaligned accesses visible to us? */
static void
misalignedtrap(void)
{
	vlong dummy, rd;

	dummy = 0x8877665544332211ULL;
	rd = ~dummy; USED(rd);
	rd = probeulong((ulong *)((char *)&dummy + 1), Read);
	if (rd >= 0)
		print("no ");
	print("super trap on unaligned access for %#p, read %#llux\n",
		(char *)&dummy + 1, rd);
}

static void
prcpucfg(void)
{
	sanity();
	cpuidprint();
	if (soc.newmach) {
		if (bootmachmode)	/* see what stuck after delegation */
			print("mideleg %#p medeleg %#p menvcfg %#p ",
				mideleg, medeleg, csrrd(MENVCFG));
		print("senvcfg %#p\n", csrrd(SENVCFG));	/* 0 if csr missing */
	}
	print("mmu: using %d-bit virtual addresses and %d-level page tables, "
		"can exploit %N bytes\n", VMBITS, Npglvls, ADDRSPCSZ);
	chooseidler();
}

/*
 * entered in supervisor mode with paging on, both low identity map
 * and KZERO->0 map in effect, and PC in KZERO space.
 * using initial page table until pageidmap then mmuinit (from cpu0init)
 * runs on cpu0, or mmuinitap runs on other cpus, which copies page table
 * root from cpu0.
 *
 * fakecpuhz has already been called in low.c.
 */
void
main(int cpu)
{
	uintptr caller;

	cpuinit(cpu);
	if (cpu != 0)
		squidboy(cpu);
	/*
	 * this calls kmesginit but if we linked with prf.$O, logging will
	 * only start once mallocinit is called from cpu0init.
	 */
	cpu0init();
	/* we know physical memory size and end (sys->pmend) here */
	/* now logging console output to kmesg */
	pcireset();		/* turn off bus masters & intrs */

	caller = getcallerpc(&cpu);
	if (isuseraddr(caller))
		print("called from low memory: %#p\n", caller);
	prcpucfg();

	probeclint();
	supsetmtimecmp();
	timersinit();	/* uses m->cpuhz; set up HZ timers on this cpu */
	clocksanity();	/* enables clock & allows clock intrs */
	enablezerotrapcnts();
	misalignedtrap();

	fpuinit();
	psinit();
	initimage();
	links();
	devtabreset();		/* discover all devices */

	prearlypages();
	copyreboottramp();

	multiprocinit();	/* start secondary cpus */
	/* page table with vmaps has been copied into each private pt. */

	/* all cores must be online for page coloring */
	pageinit();	/* make non-kernel memory available for users */
	userinit();	/* hand-craft init process; needs demand paging */

	ncflush();
	active.thunderbirdsarego = 1;	/* signal secondaries to schedule (3) */
	coherence();

	schedinit();	/* start cpu0 scheduling; will run user init process */
	panic("cpu0 schedinit returned");
}

/*
 * process 1 runs this (see userinit), so it's now okay for devtabinit to
 * spawn kprocs.
 */
void
init0(void)
{
	char buf[2*KNAMELEN];

	up->nerrlab = 0;

	spllo();

	/*
	 * These are o.k. because rootinit is null.
	 * Then early kproc's will have a root and dot.
	 */
	up->slash = namec("#/", Atodir, 0, 0);
	pathclose(up->slash->path);
	up->slash->path = newpath("/");
	up->dot = cclone(up->slash);

	devtabinit();

	if(!waserror()){
		snprint(buf, sizeof(buf), "%s %s", cputype, conffile);
		ksetenv("terminal", buf, 0);
		ksetenv("cputype", cputype, 0);
		if(cpuserver)
			ksetenv("service", "cpu", 0);
		else
			ksetenv("service", "terminal", 0);
		ksetenv("nvram", defnvram, 0);
		ksetenv("nobootprompt", "tcp", 0);
//		confsetenv();  /* no config to convert; see ../k10/bootconf.c */
		poperror();
	}
	kproc("alarm", alarmkproc, 0);
	if (soc.poll)
		kproc("poll", pollkproc, 0);
	/*
	 * start user phase executing initcode[] from init.h, compiled
	 * from init9.c (main) and ../port/initcode.c (startboot),
	 * which in turn execs /boot/boot.
	 *
	 * usp is a result of bootargs.
	 */
	setsie(Superie);
	setsts();
	clockenable();
	touser(usp);
	notreached();
}

uintptr
bootargs(uintptr base)
{
	int i;
	uintptr ssize;
	char **av, *p;

	/*
	 * Push the boot args onto the stack.
	 * Make sure the validaddr check in syscall won't fail
	 * because there are fewer than the maximum number of
	 * args by subtracting sizeof(up->arg).
	 */
	i = oargblen+1;
	p = (void *)STACKALIGN(base + PGSZ - sizeof(up->arg) - i);
	memmove(p, oargb, i);

	/*
	 * Now push argc and the argv pointers.
	 * This isn't strictly correct as the code jumped to by
	 * touser in init9.[cs] calls startboot (port/initcode.c) which
	 * expects arguments
	 * 	startboot(char* argv0, char* argv[])
	 * not the usual (int argc, char* argv[]), but argv0 is
	 * unused so it doesn't matter (at the moment...).
	 * Added +1 to ensure nil isn't stepped on, another for vlong padding.
	 */
	av = (char**)(p - (oargc+2+1+1)*sizeof(char*));
	ssize = base + PGSZ - PTR2UINT(av);
	*av++ = (char*)oargc;
	for(i = 0; i < oargc; i++)
		*av++ = (oargv[i] - oargb) + (p - base) + (USTKTOP - PGSZ);
	*av = nil;
	return STACKALIGN(USTKTOP - ssize);
}

void
userinit(void)
{
	Proc *p;
	Segment *s;
	KMap *k;
	Page *pg;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->ref = 1;
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->procmode = 0640;

	kstrdup(&eve, "");
	kstrdup(&p->text, "*init*");
	kstrdup(&p->user, eve);

	/*
	 * Kernel Stack
	 *
	 * N.B. make sure there's enough space for syscall to check
	 *	for valid args and
	 *	space for gotolabel's return PC
	 */
	p->sched.pc = PTR2UINT(init0);	/* proc 1 starts here in kernel phase */
	p->sched.sp = PTR2UINT(p->kstack+KSTACK-sizeof(up->arg));
	p->sched.sp = STACKALIGN(p->sched.sp);

	/*
	 * User Stack
	 *
	 * Technically, newpage can't be called here because it
	 * should only be called when in a user context as it may
	 * try to sleep if there are no pages available, but that
	 * shouldn't be the case here.
	 */
	s = newseg(SG_STACK, USTKTOP-USTKSIZE, USTKTOP);
	p->seg[SSEG] = s;
	pg = newpage(Zeropage, s, USTKTOP-(1LL<<s->lg2pgsize), 0);
	segpage(s, pg);
	k = kmap(pg);
	usp = bootargs(VA(k)); /* will be init0's stack pointer via touser */
	kunmap(k);

	/*
	 * Text
	 */
	s = newseg(SG_TEXT, UTZERO, UTZERO+PGSZ);
	s->flushme++;
	p->seg[TSEG] = s;
	pg = newpage(Zeropage, s, UTZERO, 0);
	memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));
	segpage(s, pg);
	k = kmap(s->map[0]->pages[0]);
	memmove((void *)VA(k), initcode, sizeof initcode);
	kunmap(k);

	wbinvd();
	/* not using address-space ids currently */
	putsatp(pagingmode | (m->ptroot->pa / PGSZ));

	ready(p);
}

void
drainuart(void)
{
	int i;

	if (islo())
		for (i = 300; i > 0 && consactive(); i--)
			delay(10);
	else
		delay(10);
}

/* shutdown this cpu */
static void
shutdown(int ispanic)
{
	int ms, once;

	/* simplify life by shutting off any watchdog */
	if (watchdogon && watchdog) {
		watchdog->disable();
		watchdogon = 0;
	}

	lock(&active);
	if(ispanic)
		active.ispanic = ispanic;
	else if(m->machno == 0 && !iscpuactive(0))
		active.ispanic = 0;		/* reboot */
	once = iscpuactive(m->machno);
	/*
	 * setting exiting will make hzclock() on each processor call exit(0),
	 * which calls shutdown(0) and mpshutdown(), which idles non-bootstrap
	 * cpus and returns on bootstrap processors (to permit a reboot).
	 * clearing our bit in active.machsmap avoids calling exit(0) from
	 * hzclock() on this processor.
	 */
	cpuinactive(m->machno);
	active.exiting = 1;
	unlock(&active);

	if(once)
		iprint("cpu%d: %s...", m->machno, m->machno? "idling": "exiting");
	spllo();
	if (m->machno == 0)
		for(ms = 5*1000; ms > 0; ms -= TK2MS(2)){
			delay(TK2MS(2));
			if(sys->nonline <= 1 && consactive() == 0)
				break;
		}
	cacheflush();
	m->clockintrsok = 0;
	if(active.ispanic && m->machno == 0){
		if(cpuserver)
			delay(2000);	/* let secondaries shutdown */
		else
			for(;;)		/* leave messages on terminal */
				halt();
	} else
		delay(1000);		/* secondary or normal shutdown */
}

/* will be called from hzclock if active.exiting is true */
void
exit(int ispanic)
{
	shutdown(ispanic);	/* will print "exiting" or "idling" */
	mpshutdown();
	/* only cpu0 gets here, and only on reboot */
	archreset();
	notreached();
}

static int
okkernel(int magic)
{
	return magic == Y_MAGIC || magic == B_MAGIC;
}

int (*isokkernel)(int) = okkernel;

/*
 * if we have to reschedule, up must be set (i.e., we must be in a
 * process context).
 */
void
runoncpu(int cpu)
{
	if (m->machno == cpu)
		return;			/* done! */
	if (up == nil)
		panic("runoncpu: nil up");
	if (up->nlocks)
		print("runoncpu: holding locks, so sched won't work\n");
	procwired(up, cpu);
	sched();
	if (m->machno != cpu)
		iprint("cpu%d: can't switch proc to cpu%d\n", m->machno, cpu);
}

static void
shutothercpus(void)
{
	intrcpu0();
	/*
	 * the other cpus could be holding locks that will never get
	 * released (e.g., in the print path) if we put them into
	 * reset now, so ask them to shutdown gracefully.
	 * once active.rebooting is set, any or all
	 * of the other cpus may be idling but not servicing interrupts.
	 */
	sys->secstall = RBFLAGSET; /* stall sec cores 'til sys->Reboot ready */
	lock(&active);
	active.rebooting = 1;		/* request other cpus shutdown */
	unlock(&active);
	shutdown(Shutreboot);

	/* any intrs to other cpus will not be delivered hereafter */
}

static void
settrampargs(void *phyentry, void *code, long size)
{
	/* set up args for trampoline */
	sys->tramp = (void (*)(Reboot *))PADDR(sys->reboottramp);
	/*
	 * jl by default doesn't produce an extended header, so entry is
	 * only 32 bits.
	 */
	sys->phyentry = (uintptr)ensurelow(phyentry);
	sys->phycode = PADDR(code);
	sys->rebootsize = size;
	coherence();
	sys->secstall = 0;  /* let cpus in mpshutdown proceed to trampoline */
	coherence();
}

/*
 * shutdown this kernel, jump to trampoline code in Sys, which copies
 * the next kernel (size @ code) into the addresses it was linked for,
 * and jumps to the new kernel's entry address.
 *
 * put other harts into wfi in trampoline in sys, jump into id map, jump
 * to trampoline code which copies new kernel into place, start all harts
 * running it.
 */
void
reboot(void *phyentry, void *code, long size)
{
	if (m->machno != 0 && up)
		runoncpu(0);

	/* other cpus may be idling; make them jump to trampoline */
	sys->rebooting = RBFLAGSET;		/* for new kernel */
	if (sys->nonline > 1)
		shutothercpus();		/* calls shutdown */

	if (prstackmax)
		prstackmax();

	/* there's no config and nowhere to store it */
	drainuart();

	/*
	 * interrupts (including uart) may be routed to any or all cpus, so
	 * shutdown devices, other cpus, and interrupts (rely upon iprint
	 * hereafter).
	 */
	devtabshutdown();
	drainuart();		/* before stopping cpus & interrupts */

	/*
	 * should be the only processor scheduling now.
	 * any intrs to other cpus will not be delivered hereafter.
	 */
	memset(active.machsmap, 0, sizeof active.machsmap);
	splhi();
	plicoff();
	pcireset();			/* disable pci bus masters & intrs */
	clockoff();
	putsie(0);
	clrsipbit(~0);

	/* we've been asked to just `halt'? */
	if (phyentry == 0 && code == 0 && size == 0) {
		spllo();
		archreset();	/* we can now use the uniprocessor reset */
		notreached();
	}

	ncflush();
	mmulowidmap();				/* disables user mapping too */
	settrampargs(phyentry, code, size);	/* clears sys->secstall */

	delay(100);			/* let secondaries settle */
	while (sys->nonline > 1) {	/* paranoia */
		iprint("%d cpus...", sys->nonline);
		delay(50);
		coherence();
	}

	/* possibly won't be seen */
	iprint("\nstarting new kernel via tramp @ %#p (%#p <- %#p size %,ld)\n",
		sys->tramp, sys->phyentry, sys->phycode,
		(ulong)sys->rebootsize);
	/*
	 * if we were entered in machine mode, we can get back to it from
	 * supervisor mode with an ECALL.  otherwise, we can't get back to it
	 * at all.  the reboot trampoline will adjust the entry point suitably.
	 */
	m->bootmachmode = bootmachmode;		/* for rebootcode.s */
	if (bootmachmode) {
		//putmtvec(origmtvec); /* restore in case of sbi; see totramp */
		/*
		 * to machine mode to trigger reboot with sys->Reboot args.
		 * in machine mode, it's unsafe to use sys.
		 */
		ecall();			/* see totramp in mtrap.s */
	} else
		/* jump into the trampoline in the id map - never to return */
		(*sys->tramp)(&sys->Reboot);
	notreached();
}
