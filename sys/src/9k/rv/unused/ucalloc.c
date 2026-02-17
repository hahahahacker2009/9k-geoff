/*
 * uncached memory allocator
 * based on Plan 9's 1998 ucalloc.c, relatively simple
 *
 * sys->ncbase to sys->ncend is the uncached region mapped with PteNc under
 * Svpbmt.  not to be used with Uncview; that uses normal allocations from an
 * uncached alias view of ram.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

enum
{
	MAGIC		= 0xbada110c,
	MAX2SIZE	= 63,
	CUTOFF		= 12,
	Align		= (uintptr)sizeof(double),
};

typedef struct Bucket Bucket;
struct Bucket
{
	int	size;		/* lg(allocation size) (at data) */
	int	magic;
	Bucket	*alignedalloc;	/* original allocation in ucallocalign */
	Bucket	*next;
//	vlong	pad;
	char	data[];		/* was data[1] */
};

typedef struct Arena Arena;
struct Arena
{
	Bucket	*btab[MAX2SIZE];
};
static Arena arena;
static Lock malloclck;
static char *ucbrk;

#define datoff		((uintptr)((Bucket*)0)->data)

/* kernel addresses look negative, so return 0 on failure */
static void *
sbrk(uintptr size)		/* raise uncached arena break */
{
	char *ret;

	if (ucbrk == 0)
		ucbrk = (char *)sys->ncbase;
	if (ucbrk == 0)
		panic("ucalloc: no uncached region to allocate from\n");
	ret = ucbrk;
	if (ret >= (char *)sys->ncend)
		return 0;
	ucbrk += ROUNDUP(size, sizeof(double));
	return ret;
}

void*
ucalloc(uintptr size)
{
	uintptr next;
	int pow, n;
	Bucket *bp, *nbp;

	for(pow = 1; pow < MAX2SIZE; pow++)
		if(size <= (1ull<<pow))
			goto good;
	return nil;
good:
	ilock(&malloclck);
	/* Allocate off this list */
	bp = arena.btab[pow];
	if(bp) {
		arena.btab[pow] = bp->next;

		if(bp->magic != 0)
			panic("ucalloc: bad magic %ux", bp->magic);

		bp->magic = MAGIC;
		bp->alignedalloc = nil;
		memset(bp->data, 0, size);
		iunlock(&malloclck);
		return bp->data;
	}
	size = ROUNDUP(sizeof(Bucket) + (1ull<<pow), Align);
	if(pow < CUTOFF) {
		n = (CUTOFF-pow)+2;
		bp = sbrk(size*n);
		if(bp == 0) {
			iunlock(&malloclck);
			return nil;
		}
		next = (uintptr)bp+size;
		nbp = (Bucket*)next;
		arena.btab[pow] = nbp;
		for(n -= 2; n; n--) {
			next = (uintptr)nbp+size;
			nbp->next = (Bucket*)next;
			nbp->size = pow;
			nbp = nbp->next;
		}
		nbp->size = pow;
	} else {
		bp = sbrk(size);
		if(bp == 0) {
			iunlock(&malloclck);
			return nil;
		}
	}

	bp->size = pow;
	bp->alignedalloc = nil;
	bp->magic = MAGIC;

	memset(bp->data, 0, size);
	iunlock(&malloclck);
	return bp->data;
}

/* doesn't yet implement off and span */
void*
ucallocalign(uintptr size, uintptr align, vlong offset, uintptr span)
{
	char *p, *aligned;
	Bucket *bp, *nbp;

	if (align == 0 && offset == 0 && span == 0)
		return ucalloc(size);

	assert(align != 0);
	p = ucalloc(size + align + offset + span + sizeof(Bucket));
	if (p == nil)
		return nil;

	/* we know that there's a Bucket before p. */
	bp = (Bucket*)((uintptr)p - datoff);
	aligned = (void *)ROUNDUP((uintptr)p, align);
	if (aligned < p || aligned >= p + size + align + offset + span)
		panic("ucallocalign: p %#p p end %#p aligned %#p",
			p, p + size + align + offset + span, aligned);
	if (aligned != bp->data) {
		/* slide Bucket up to aligned data */
		bp->alignedalloc = bp;		/* original addr */
		nbp = (Bucket *)(aligned - sizeof *bp);
		memmove(nbp, bp, sizeof *bp);
		bp = nbp;
		assert(aligned == bp->data);
	}
	assert((uintptr)bp->data % align == 0);
	return bp->data;
}

void
ucfree(void *ptr)
{
	Bucket *bp, **l;

	if(ptr == nil)
		return;

	/* Find the start of the structure */
	bp = (Bucket*)((uintptr)ptr - datoff);
	if(bp->magic != MAGIC)
		panic("ucfree: bad magic %ux", bp->magic);

	if (bp->alignedalloc) {
		/* slide Bucket down from aligned data to orig. alloc'n */
		memmove(bp->alignedalloc, bp, sizeof *bp);
		bp = bp->alignedalloc;
		bp->alignedalloc = nil;
	}
	bp->magic = 0;
	ilock(&malloclck);
	l = &arena.btab[bp->size];
	bp->next = *l;
	*l = bp;
	iunlock(&malloclck);
}

void*
ucrealloc(void *ptr, uintptr n)
{
	void *new;
	uintptr osize;
	Bucket *bp;

	if(ptr == nil)
		return ucalloc(n);

	/* Find the start of the structure */
	bp = (Bucket*)((uintptr)ptr - datoff);
	if(bp->magic != MAGIC)
		panic("ucrealloc: bad magic %ux", bp->magic);

	/* enough space in this bucket */
	osize = 1ull<<bp->size;
	if(osize >= n && n > osize/2)
		return ptr;

	new = ucalloc(n);
	if(new == nil)
		return nil;

	memmove(new, ptr, osize < n ? osize : n);
	ucfree(ptr);

	return new;
}
