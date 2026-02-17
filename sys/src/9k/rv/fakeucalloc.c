/*
 * stubs for uncached memory allocator
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void*
ucalloc(uintptr)
{
	return nil;
}

/* doesn't yet implement off and span */
void*
ucallocalign(uintptr, uintptr, vlong, uintptr)
{
	return nil;
}

void
ucfree(void *)
{
}

void*
ucrealloc(void *, uintptr)
{
	return nil;
}
