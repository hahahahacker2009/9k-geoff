#include "../port/portfns.h"

typedef long (*Archfnp)(Chan*, void*, long, vlong);

void	aamloop(int);
Dirtab*	addarchfile(char*, int, Archfnp, Archfnp);
void	archfmtinstall(void);
vlong	archhz(void);
void	archinit(void);
int	archmmu(void);
void	archreset(void);
uintptr	asmalloc(uintptr, uintptr, int, uintptr);
int	asmfree(uintptr, uintptr, int);
void	asminit(void);
void	asmmapinit(uintptr, uintptr, int);
void	asmmodinit(uintptr, uintptr, char*);
PTE	asmwalkalloc(uintptr size);
void	badminst(void);
void	badsinst(void);
Mallocs	cacheallocalign(uintptr, uintptr, vlong, uintptr);
Mallocs	cachealloc(uintptr);
void	cachedinvse(void*, uintptr);
void*	cachedview(void *);
void	cachedwbinvse(void*, uintptr);
void	cachedwbse(void*, uintptr);
void	cacheflush(void);
void	calibrate(void);
void	cboclean(void *);
void	cboflush(void *);
void	cboinval(void *);
void	cbozero(void *);
void	cgaconsputs(char*, int);
void	cgainit(void);
void	chooseincoher(void);
void	clearipi(void);
/* until soc.clintlongs is set by probing, always use longs for clint */
#define CLINTLONGS() (!soc.clintlongsset || soc.clintlongs)
void	clockenable(void);
void	clockintr(Ureg* ureg, void *);
void	clockoff(void);
int	clocksanity(void);
void	clrreserv(void);
uintptr	clrsie(uintptr);
void	clrsipbit(ulong);
void	clrstie(void);
int	clzzbb(Clzuint);
void	coherence(void);
void	confsetenv(void);
#define CONSREGS()	(m && m->consuart? m->consuart: PAUart0) /* for prf */
int	corecolor(int machno);
void	countipi(void);
int	cpu2context(uint cpu);
void	cpuidinit(void);
void	cpuidprint(void);
void	cpuinit(int cpu);
#define	csrclr(csrno, bits)	docsr(Csrrc, (csrno), ARG, (bits))
#define	csrrd(csrno)		docsr(Csrrs, (csrno), 0, 0) /* rs 0: no write */
#define	csrset(csrno, bits)	docsr(Csrrs, (csrno), ARG, (bits))
#define	csrswap(csrno, new)	docsr(Csrrw, (csrno), ARG, (new))
#define	cycles(t) (*(t) = rdtsc())
int	dbgprint(char*, ...);
#define decref(r)	adec(&(r)->ref)
#define delay(ms) millidelay(ms)
uintptr	docsr(uchar op, ushort csrno, uchar rs, uintptr new);
void	drainuart(void);
void	dualmap(PTE *ptp, uintptr phys, uint nptes, int lvl);
void	dumpstk(void *stk);
PTE	*earlypagealloc(void);
void	ecall(...);
void	enablezerotrapcnts(void);
#define	ensurehigh(addr) (void *)((uintptr)(addr) | KZERO)
#define	ensurelow(addr)  (void *)((uintptr)(addr) & ~KZERO)
void	etherenableirqs(Ether *);
int	etherfmt(Fmt* fmt);
void	etherintr(Ureg *);
int	etherismacset(uvlong addr);
int	ethersetmac(uchar *ra, int ctlrno, uvlong regadd);
void	evenaddr(uintptr);
void*	evmap(uintptr pa, uintptr size);
void	fakecpuhz(void);
void	fillpt(PTE *, int, uintptr, uintptr, PTE, int);
void	fpconstset(void);
void	fpoff(void);
void	fpon(void);
void	fptrap(Ureg *, void *);
int	fpudevprocio(Proc*, void*, long, uintptr, int);
void	_fpuinit(void);
void	fpuinit(void);
void	fpunoted(void);
void	fpunotify(Ureg*);
void	fpuprocrestore(Proc*);
void	fpuprocsave(Proc*);
void	fpurestore(uchar *);
void	fpusave(uchar *);
void	fpusysprocsetup(Proc*);
void	fpusysrforkchild(Proc*, Proc*);
void	fpusysrfork(Ureg*);
#define getconf(n) nil
ulong	getfcsr(void);
uintptr	getmie(void);
uintptr	getmip(void);
uintptr	getmsts(void);
void*	getmtvec(void);
uintptr	getsatp(void);
uintptr getsb(void);
uintptr	getsie(void);
uintptr	getsip(void);
uintptr	getsp(void);
uintptr	getsts(void);
void*	getstvec(void);
void	golow(void);
void	halt(void);
int	haveinstr(void *fn, uintptr arg);
void	hsmstartall(void);
void*	i8250alloc(uintptr, int, int);
#ifdef SIFIVEUART
#define i8250console sifiveconsole
#endif
Uart*	i8250console(char*);
void	idlehands(void);
void	idthandlers(void);
#define incref(r)	ainc(&(r)->ref)
void	intrall(void);
int	intrclknotrap(Ureg *);
void	intrcpu0(void);
int	intrdisable(void*);
void	intrenableall(void);
void*	intrenable(int, void (*)(Ureg*, void*), void*, int, char*);
int	intrnotrap(void);
void	invlpg(uintptr);
Ioconf*	ioconf(char *, int);
void	iovmapsoc(void);
int	iprint(char*, ...);
#define	isaconfig(s, i, isaconfp)	nil
int	ishartonline(int hart);
#define	iskern(addr) (((uintptr)(addr) & KZERO) != 0)
int	ismemory(uintptr *va, uintptr addr);
int	isnotempty(uintptr *va);
#define	isphys(addr) (((uintptr)(addr) & KZERO) == 0)
void	jumphigh(void);
void	jumplow(void);
void	kbdenable(void);
void	kbdinit(void);
void	kexit(Ureg*);
#define	kmapinval()
void	kmesginit(void);
void	links(void);
uint	mach2context(Mach *);
void	main(int);
void	mappage(Page *ptroot, PTE *pte, uintptr va, uintptr pa);
int	memcolor(uintmem addr, uintmem *sizep);
void	meminit(void);
void	mfence(void);
void	mmudumplvl(uintptr, uintptr, int lvl);
void	mmudump(uintptr, int toplvl);
void	mmuflushtlb(Page *);
void	mmuidentitymap(void);
void	mmuinitap(void);
void	mmuinit(void);
void	mmulowidmap(void);
uintptr	mmuphysaddr(uintptr);
uintptr	mmutrans(PTE *ptroot, uintptr va);
int	mmuwalknewpte(uintptr va, uintmem mem, int lvl, PTE attrs);
int	mmuwalk(uintptr, int, PTE**, uintptr (*)(uintptr));
#define mmuwrpte(ptep, pte) (wbinvd(), *(ptep) = (pte))
void	monitor(void* address, ulong extensions, ulong hints);
void	mpshutdown(void);
void	mret(void);
void	mtrap(void);
void	mwait(ulong extensions, ulong hints);
uintptr	ncbase(Sys *sys);
void	ncflush(void);
void	ncinit(void);
Mach*	newcpupages(int machno);
void	nointrs(Intrstate *is);
void	nop(void);
vlong	normalmap(void);
int	notify(Ureg*);
uchar	nvramread(int);
void	nvramwrite(int, uchar);
void	pageidmap(void);
void	pause(void);
ulong	pcibarsize(Pcidev*, int);
void	*(*pcicfgaddr)(int tbdf, int rno);
int	pcicfgr16(Pcidev*, int);
int	pcicfgr32(Pcidev*, int);
int	pcicfgr8(Pcidev*, int);
void	pcicfgw16(Pcidev*, int, int);
void	pcicfgw32(Pcidev*, int, int);
void	pcicfgw8(Pcidev*, int, int);
void	pciclrbme(Pcidev*);
int	pciclrcfgbit(Pcidev *p, int reg, ulong bit, char *offmsg);
void	pciclrioe(Pcidev*);
void	pciclrmwi(Pcidev*);
int	pcigetmsi(Pcidev *p, Msi *msi);
int	pcigetmsixcap(Pcidev *p);
int	pcigetpciecap(Pcidev *p);
int	pcigetpms(Pcidev*);
void	pcihinv(Pcidev*);
void	pciintrs(Pcidev*);
uchar	pciipin(Pcidev*, uchar);
Pcidev*	pcimatch(Pcidev*, int, int);
Pcidev*	pcimatchtbdf(int);
void	pcimsioff(Vctl*, Pcidev*);
void	pcinointrs(Pcidev*);
void	pcireset(void);
int	pciscan(int, Pcidev**);
void	pcisetbme(Pcidev*);
int	pcisetcfgbit(Pcidev *p, int reg, ulong bit, char *onmsg);
void	pcisetioe(Pcidev*);
int	pcisetmsi(Pcidev *p, Msi *msi);
void	pcisetmwi(Pcidev*);
int	pcisetpms(Pcidev*, int);
#define	perfticks() ((ulong)rdcltime())/* cheap perf. measurement ticks. need count to 1s. */
void	physmeminit(void);
void	plicdisable(uint irq);
int	plicenable(uint irq);
void	plicinit(void);
void	plicoff(void);
void	(*pmpinitp)(void);
void	pollkproc(void *);
void	poppt2lvl1(Ptepage ptarr[], uintptr va);
void	probeclint(void);
uintptr	probeinstr(void *insts, uintptr new, int *failedp);
void	probesvpbmt(Sys *sys);
vlong	probeulong(ulong *addr, int wr);
int	probeuvlong(uvlong *addr, int wr);
void	procipimsgs(void);
Page*	ptrootinit(void);
void	putmie(uintptr);
void	putmip(uintptr);
void	putmsts(uintptr);
void*	putmtvec(void *);
vlong	_putsatp(uintptr);
vlong	putsatp(uintptr);
void	putsie(uintptr);
void	putsip(uintptr);
void	putsscratch(uintptr);
void	putsts(uintptr);
void*	putstvec(void *);
void	qipimsg(void *, void *);
/* rdtime() is safe on hardware with sbi and on our tinyemu. */
#define	rdcltime() (CLINTLONGS() || !nosbi? rdtime(): m->clint->mtime)
uvlong	rdtime(void);
uvlong	rdtsc(void);
vlong	readmprv(ulong *);
void	recmtrapalign(void);
void	rectrapalign(void);
void	restintrs(Intrstate *is);
void	runoncpu(int cpu);
vlong	sbicall(uvlong, uvlong, uvlong, Sbiret *, uvlong *);
vlong	sbiclearipi(void);
vlong	sbiecall(uvlong, uvlong, uvlong, Sbiret *, uvlong *);
vlong	sbigetimplid(void);
vlong	sbigetimplvers(void);
vlong	sbigetmarchid(void);
vlong	sbigetmvendorid(void);
vlong	sbihartstart(uvlong hartid, uvlong phys_start, uvlong private);
vlong	sbihartstatus(uvlong hartid);
vlong	sbihartsuspend(void);
vlong	sbiprobeext(uvlong);
void	sbireset(ulong type, ulong reason);
vlong	sbisendipi(uvlong map[]);
void	sbisettimer(uvlong);
void	sbishutdown(void);
int	screenprint(char*, ...);			/* debugging */
void	setclinttmr(uvlong clints);
void	setfcsr(ulong);
void	setkernmem(void);
void	setncbase(void);
void	setptes(PTE *ptep, PTE ptebits, int n, int lvl);
void	setsb(void);
uintptr	setsie(uintptr);
void	setsp(uintptr);
void	setsts(void);
void	setupncpt(Sys *sys);
void	sfence_vma(uintptr);
void*	sigsearch(char* signature);
void	strap(void);
void	supsetmtimecmp(void);
void*	swapmtvec(void *);
void*	swapstvec(void *);
void	sync_is(void);
void*	sysexecregs(uintptr, ulong, ulong);
uintptr	sysexecstack(uintptr, int);
void	sysprocsetup(Proc*);
void	sysrforkret(void);
void	touser(uintptr);
void	trapenable(int, void (*)(Ureg*, void*), void*, char*);
void	trapinit(void);
void	trapsclear(void);
void	trapvecs(void);
void	uartextintr(Ureg *reg);
void	_uartputs(char *s, int n);
void	uartsetregs(int i, uintptr regs);
void*	ucallocalign(uintptr, uintptr, vlong, uintptr);
void*	ucalloc(uintptr);
void	ucfree(void *);
void*	ucrealloc(void*, uintptr);
uintptr	ncspace(void);
void*	uncachedview(void *);
void	usephysdevaddrs(void);
int	userureg(Ureg*);
void	usevirtdevaddrs(void);
PTE	va2gbit(uintptr va);
void*	vmap(uintmem, uintptr);
void	vmbotch(ulong, char *);
void	vunmap(void*, uintptr);
int	waitfor(int *vp, int val);
void	wbinvd(void);
void	wfi(void);
void	wrcltimecmp(uvlong);
void	wrcltime(uvlong);
void	writeconf(void);
void	writelmprv(ulong *, ulong);
void	wrsnto(uintptr);
void	wrssto(uintptr);
void	wrtsc(uvlong);
void	zerotrapcnts(void);
void*	zero(void *, uintptr);

extern int islo(void);
extern void spldone(void);
extern Mpl splhi(void);
extern Mpl spllo(void);
extern void splx(Mpl);

/* riscv atomics */
ulong	amoorw(ulong *addr, ulong bits);	/* set bits */
ulong	amoandnw(ulong *addr, ulong bits);	/* clear bits */
ulong	amoswapw(ulong *addr, ulong nv);

/* libc atomics */
int	_tas(int*);
int	cas(uint*, int, int);

#define CASW		cas
#define TAS		_tas

#define	waserror()	(up->nerrlab++, setlabel(&up->errlab[up->nerrlab-1]))

#define PTR2UINT(p)	((uintptr)(p))
#define UINT2PTR(i)	((void*)(i))

void*	KADDR(uintptr);
uintptr	PADDR(void*);

/*
 * archrv.c
 */
extern void millidelay(int);

/*
 * mmu.c
 */
extern void cpusalloc(int);
