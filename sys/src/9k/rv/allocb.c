/* Block allocation for dma-incoherent systems, needs dmamalloc.c */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"uncached.h"

#define BALIGNSZ(size)	ROUNDUP(size, BLOCKALIGN)

enum
{
	Debug = 0,
	Hdrspc		= 64,		/* leave room for high-level headers */
	Bdead		= 0x51494F42,	/* "QIOB" */
};

struct
{
	Lock;
	uvlong	bytes;
	uvlong	limit;			/* allocated space */
} ialloc;

/*
 * at least on DMA-incoherent systems, add padding to avoid touching previous
 * and following cache lines to avoid polluting them, or vice versa.
 * aggressive prefetchers like the eic7700x's make the problem worse.
 */
static Block*
_allocb(int size)
{
	Block *b;
	uchar *p;
	uintptr n, padding, unpadded;

	/* malloc allocations are always double-aligned */
	padding = FLUSHSTRAT()? CACHELINESZ: 0;
	n = BLOCKALIGN + BALIGNSZ(size+Hdrspc) + BALIGNSZ(sizeof(Block)) +
		2*padding;
	if((p = malloc(n)) == nil)
		return nil;

	/*
	 * the allocation looks like this, where b is BLOCKALIGN bytes:
	 *
	 *   | b align | n1*b pad | n2*b size+Hdrspc | n3*b Block | n1*b pad |
	 *
	 * except that the allocation may be only double-aligned, thus the
	 * BLOCKALIGN bytes at the start.  we may need to adjust the first pad's
	 * address to ensure BLOCKALIGN alignment of size+Hdrspc and Block.
	 */
	unpadded = n - padding;
	/*
	 * iff uncached mapping works, flush seems unneeded.  lacking
	 * uncached mapping, jupiter got 25% tcp checksum or crc errors,
	 * or otherwise misbehaved without the flush.
	 */
	if (TODO && padding)			/* in case jupiter needs it */
		cachedwbinvse(p, unpadded);

	/*
	 * Block at end of allocated space with empty cache line after.
	 * Round down to BLOCKALIGN alignment.
	 */
	b = (Block*)(((uintptr)p + unpadded - BALIGNSZ(sizeof(Block))) &
		~(BLOCKALIGN-1ULL));
	if (((uintptr)b & (BLOCKALIGN-1)) != 0)
		iprint("_allocb: b %#p unaligned\n", b);
	b->base = p;
	b->next = b->list = nil;
	b->free = nil;
	b->flag = 0;

	/* align base and bounds of data */
	b->lim = (uchar*)((uintptr)b & ~(BLOCKALIGN-1ULL));

	/*
	 * align start of writable data, leaving space below for added headers.
	 * unfortunately, padblock can back up bp->rp so it's not BLOCKALIGN-
	 * nor even long-aligned, which may be a problem for ethernet
	 * tranmission bufferw.
	 */
	b->rp = b->lim - BALIGNSZ(size);
	b->wp = b->rp;
	if (((uintptr)b->rp & (BLOCKALIGN-1)) != 0)
		iprint("_allocb: b->rp %#p unaligned\n", b->rp);

	if(b->rp < b->base || b->lim - b->rp < size)
		panic("_allocb");

	return b;
}

Block*
allocb(int size)
{
	Block *b;

	/*
	 * Check in a process and wait until successful.
	 * Can still error out of here, though.
	 */
	if(up == nil)
		panic("allocb without up: %#p", getcallerpc(&size));
	if((b = _allocb(size)) == nil){
		mallocsummary();
		panic("allocb: no memory for %d bytes", size);
	}

	return b;
}

void
ialloclimit(uvlong limit)
{
	if (limit > 2UL*GB-1)
		limit = 2UL*GB-1;	/* keep it fitting in a long for qio */
	ialloc.limit = limit;
	if (Debug)
		iprint("ialloclimit %,lld\n", limit);
}

static Block *
exceeded(char *msg, uvlong *cntp)
{
	static uint mp;

	if((*cntp)++ % (1<<13) == 0){
		if(mp++ > 1000){
			active.exiting = 1;
			exit(0);
		}
		iprint("iallocb: %s %llud/%llud\n",
			msg, ialloc.bytes, ialloc.limit);
	}
	return nil;
}

Block*
iallocb(int size)
{
	Block *b;
	static uvlong m1, m2;

	if(ialloc.bytes > ialloc.limit)
		return exceeded("limited", &m1);
	if((b = _allocb(size)) == nil) {
		iprint("iallocb called from %#p\n", getcallerpc(&size));
		return exceeded("no memory", &m2);
	}
	b->flag = BINTR;

	ilock(&ialloc);
	ialloc.bytes += b->lim - b->base;
	iunlock(&ialloc);

	return b;
}

void
freeb(Block *b)
{
	uchar *p;

	if(b == nil)
		return;

	/*
	 * drivers which perform non cache coherent DMA manage their own buffer
	 * pool of uncached buffers and provide their own free routine.
	 */
	if(b->free) {
		b->free(b);
		return;
	}
	if(b->flag & BINTR) {
		ilock(&ialloc);
		ialloc.bytes -= b->lim - b->base;
		iunlock(&ialloc);
	}

	p = b->base;			/* original allocation */

	/* poison the block in case someone is still holding onto it */
	b->next = (void*)Bdead;
	b->rp = b->wp = b->lim = b->base = (void*)Bdead;

	free(p);
}

void
checkb(Block *b, char *msg)
{
	void *dead = (void*)Bdead;

	if(b == dead)
		panic("checkb b %s %#p", msg, b);
	if(b->base == dead || b->lim == dead || b->next == dead
	  || b->rp == dead || b->wp == dead){
		print("checkb: base %#p lim %#p next %#p\n",
			b->base, b->lim, b->next);
		print("checkb: rp %#p wp %#p\n", b->rp, b->wp);
		panic("checkb dead: %s", msg);
	}

	if(b->base > b->lim)
		panic("checkb 0 %s %#p %#p", msg, b->base, b->lim);
	if(b->rp < b->base)
		panic("checkb 1 %s %#p %#p", msg, b->base, b->rp);
	if(b->wp < b->base)
		panic("checkb 2 %s %#p %#p", msg, b->base, b->wp);
	if(b->rp > b->lim)
		panic("checkb 3 %s %#p %#p", msg, b->rp, b->lim);
	if(b->wp > b->lim)
		panic("checkb 4 %s %#p %#p", msg, b->wp, b->lim);
}

void
iallocsummary(void)
{
	print("ialloc %llud/%llud\n", ialloc.bytes, ialloc.limit);
}
