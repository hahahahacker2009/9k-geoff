/*
 * (i)(un)lock, canlock - spinlocks with test-and-set, for short-lived locks
 *
 * recently added more coherence calls to push changes to other cpus sooner.
 *
 * Somehow in the inner lock-contention loops of lock and ilock, we can get
 * rescheduled on to another cpu, and the cycle counters are not synchronised,
 * so don't use them to detect lock loops.  I don't know how this is possible,
 * maybe clock interrupts triggering scheduling.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "../port/edf.h"

enum {
	Lockdebug = 0,
	LOCKCYCLES = 0,
};

/* there are races updating all of these, so they are only approximate */
uvlong	maxlockcycles;
uintptr	maxlockpc;
uvlong	maxilockcycles;
uintptr	maxilockpc;
struct
{
	ulong	locks;
	ulong	glare;
	ulong	inglare;
} lockstats;

static void
lockloop(Lock *l, uintptr pc)
{
	Proc *p;

	p = l->p;
	iprint("lock %#p loop key %#ux pc %#p held by pc %#p proc %d\n",
		l, l->key, pc, l->pc, p? p->pid: 0);
	dumpaproc(up);
	if(p != nil)
		dumpaproc(p);
}

static void
userlock(Lock *lck, char *func, uintptr pc)
{
	panic("%s: nil or user-space Lock* %#p; called from %#p", func, lck, pc);
}

/*
 * Priority inversion, yield on a uniprocessor;
 * on a multiprocessor, another processor will unlock.
 * Not a common code path.
 */
static void
edfunipriinversion(Lock *l, uintptr pc)
{
	print("lock: inversion %#p pc %#p proc %d held by pc %#p proc %d\n",
		l, pc, up? up->pid: 0, l->pc, l->p? l->p->pid: 0);
	up->edf->d = todget(nil);	/* yield to process with lock */
}

/* call once we have l->key (via TAS) */
#define TAKELOCK(l, callpc) \
	if(up) \
		up->lastlock = l; \
	l->pc = callpc; \
	l->p = up; \
	l->isilock = 0; \
	if (LOCKCYCLES) \
		cycles(&l->lockcycles); \
	coherence()		/* make changes visible to other cpus */

/*
 * if waitfor/watchforchange/waitchange use monitor/mwait or lr/wrsnto for
 * l->key, an interrupt could ilock and change the single per-cpu monitor
 * address used by lock, for example, causing a single failed attempt to acquire
 * the lock, which shouldn't be a problem.
 */
static void
waitforlock(Lock *l, ulong pc, Edf *edf)
{
	uint i;

	watchforchange(&l->key);
	i = 0;
	while (l->key){
		/* lock's busy; wait a bit for it to be released */
		if(edf && edf->flags & Admitted)
			edfunipriinversion(l, pc);
		if(i++ > 2LL*GHZ) {		/* are we stuck? */
			lockloop(l, pc);
			i = 0;
		}
		waitchange(&l->key);
		watchforchange(&l->key);
	}
}

static void
decnlocks(char *fn, uintptr pc)
{
	int x;

	if (up && (x = adec(&up->nlocks)) < 0) /* allow being scheded */
		panic("%s: ref %d < 0 after adec; callerpc=%#p", fn, x, pc);
}

static void
lockdebug(Lock *l, uintptr pc)
{
	/* we can be called from low addresses before the mmu is on. */
	if (l == nil)
		userlock(l, "lock", pc);
	if (l->noninit)
		panic("lock: lock %#p is not a lock, from %#p", l, pc);
}

/*
 * returns with Lock l held and, if up is non-nil, up->nlocks non-zero.
 * leaves PL alone, especially when waiting for the lock.
 */
int
lock(Lock *l)
{
	uintptr pc;
	Edf *edf;

	pc = getcallerpc(&l);
	if (Lockdebug)
		lockdebug(l, pc);
	lockstats.locks++;
	if(up)
		ainc(&up->nlocks);	/* prevent being scheded while locked */
	if(TAS(&l->key) == 0){
		TAKELOCK(l, pc);
		return 0;		/* got it on first try */
	}

	/* slow but less common path; there's contention */
	edf = nil;
	if(up) {
		decnlocks("lock", pc);		/* allow scheding */
		if(l->p == up)
			panic("lock: deadlock acquiring lock held by same proc,"
				" from %#p", pc);
		if (sys->nonline <= 1)
			edf = up->edf;
	}
	lockstats.glare++;
	for(;;){
		lockstats.inglare++;
		waitforlock(l, pc, edf);

		/* we believe that the lock is free; try again to grab it. */
		if(up)
			ainc(&up->nlocks);	/* prevent scheding */
		if(TAS(&l->key) == 0)
			break;

		/* lost the tas race, still contending */
		decnlocks("lock 2", pc);	/* allow scheding */
	}
	TAKELOCK(l, pc);
	return 1;			/* got it, but not on first try */
}

static void
ilockdebug(Lock *l, uintptr pc)
{
	if (l == nil)
		userlock(l, "ilock", pc);
	if (l->noninit)
		panic("ilock: lock %#p is not a lock (noninit = %#lux),"
			" from %#p", l, l->noninit, pc);
}

/*
 * waits for the lock at entry PL, returns at high PL.
 *
 * if called splhi on a uniprocessor, the loop on l->key!=0
 * below will run at splhi, and thus cannot succeed as
 * nothing can change l->key, unless another cpu is spinning
 * up concurrently, or dma changes l->key, or iunlock writing
 * 0 to l->key has been vastly delayed in a write buffer, so
 * there is no way out.
 */
void
ilock(Lock *l)
{
	int lo;
	uintptr pc;
	Mpl s;

	pc = getcallerpc(&l);
	if (Lockdebug)
		ilockdebug(l, pc);
	lockstats.locks++;

	lo = islo();
	s = splhi();
	if(TAS(&l->key) != 0){
		lockstats.glare++;
		if(!lo && sys->nmach <= 1 && sys->nonline <= 1)
			iprint("ilock: lock %#p: no way out, from %#p splhi\n",
				l, pc);
		/*
		 * Cannot also check l->pc, l->m, or l->isilock here
		 * because they might just not be set yet, or
		 * (for pc and m) the lock might have just been unlocked.
		 */
		do {
			lockstats.inglare++;
			splx(s);
			waitforlock(l, pc, nil);
			splhi();
		} while (TAS(&l->key) != 0);	/* try to grab lock again */
	}

	/* we have the lock (l->key), update other fields to reflect that */
	l->sr = s;
	l->m = m;
	/* modified TAKELOCK */
	if(up)
		up->lastilock = l;
	l->pc = pc;
	l->p = up;
	l->isilock = 1;
	if (LOCKCYCLES)
		cycles(&l->lockcycles);
	coherence();		/* make changes visible to other cpus */

	m->ilockpc = pc;
	m->ilockdepth++; /* increment after acquiring lock */
}

int
canlock(Lock *l)
{
	uintptr pc;

	pc = getcallerpc(&l);
	if (Lockdebug && l == nil)
		userlock(l, "canlock", pc);
	if(up)
		ainc(&up->nlocks);
	if(TAS(&l->key) != 0){		/* failed to acquire the lock? */
		decnlocks("canlock", pc);	/* allow scheding */
		return 0;
	}

	TAKELOCK(l, pc);
	l->m = m;
	return 1;
}

#define GIVELOCKBACK(l) \
	l->m = nil; \
	l->p = nil; \
	coherence(); \
	/* actual release; data protected by this Lock and the Lock itself */ \
	/* must be current before release. */ \
	l->key = 0; \
	coherence()

static void
unlockdebug(Lock *l, uintptr pc)
{
	if (l == nil)
		userlock(l, "unlock", pc);
	if(l->key == 0)
		iprint("unlock: not locked: pc %#p\n", pc);
	if(l->isilock)
		iprint("unlock of ilock: pc %#p, held by %#p\n", pc, l->pc);
}

static void
countcycles(Lock *l, uvlong *maxcycp, uintptr *maxpcp)
{
	uvlong cyc;

	cycles(&cyc);
	l->lockcycles = cyc - l->lockcycles;
	if(l->lockcycles > *maxcycp){
		*maxcycp = l->lockcycles;
		*maxpcp = l->pc;
	}
}

/* may trigger rescheduling */
void
unlock(Lock *l)
{
	uintptr pc;

	pc = getcallerpc(&l);
	if (LOCKCYCLES)
		countcycles(l, &maxlockcycles, &maxlockpc);
	if (Lockdebug)
		unlockdebug(l, pc);
	USED(pc);

	/*
	 * only the Lock-holding process should release it.  otherwise,
	 * the wrong up->nlocks will be decremented.  a Lock may have been
	 * acquired when up was nil and is being released after setting it,
	 * but Locks are not supposed to be held for long (in particular,
	 * across sleeps or sched calls).
	 *
	 * if l->p is nil, up->nlocks should not have been incremented when
	 * locking, thus should not be decremented here.
	 * l->p == up should always be true.
	 */
	if (l->p != up)
		iprint("unlock: l->p changed: pc %#p, acquired at pc %#p, "
			"lock p %#p != unlock up %#p\n", getcallerpc(&l),
			l->pc, l->p, up);
	GIVELOCKBACK(l);
	if (up) {
		decnlocks("unlock", getcallerpc(&l));	/* allow scheding */
		/*
		 * Call sched if the need arose while locks were held, but
		 * don't do it from interrupt routines, hence the islo() test.
		 */
		if (up->nlocks == 0 && up->delaysched && islo())
			sched();
	}
	/*
	 * contenders for this lock will be spinning or watching l->key, no need
	 * to wake them.
	 */
}

static void
iunlockdebug(Lock *l, uintptr pc)
{
	if (l == nil)
		userlock(l, "iunlock", pc);
	if(l->key == 0)
		print("iunlock: not locked: pc %#p\n", pc);
	if(!l->isilock)
		print("iunlock of lock: pc %#p, held by %#p\n", pc, l->pc);
	if(l->m != m)
		print("iunlock by cpu%d, locked by cpu%d: pc %#p, held by %#p\n",
			(m? m->machno: -1), (l && l->m? l->m->machno: -1),
			pc, l->pc);
}

void
iunlock(Lock *l)
{
	uintptr pc;
	Mpl s;

	pc = getcallerpc(&l);
	if (LOCKCYCLES)
		countcycles(l, &maxilockcycles, &maxilockpc);
	if (Lockdebug)
		iunlockdebug(l, pc);
	if(islo())
		print("iunlock while lo: pc %#p, held by %#p\n", pc, l->pc);

	m->ilockdepth--;	/* decrement before lock release */
	if(up)
		up->lastilock = nil;
	s = l->sr;
	GIVELOCKBACK(l);
	/*
	 * contenders for this lock will be spinning or watching l->key, no need
	 * to wake them.
	 */
	splx(s);
}
