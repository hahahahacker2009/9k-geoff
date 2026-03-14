/*
 * #P/realmodemem, to let vga muck with low memory
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"
#include "tos.h"
#include "ureg.h"

#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

#define LORMBUF (RMBUF-KZERO)

static long
rmemrw(int isr, void *a, long n, vlong off)
{
	if(off < 0 || n < 0)
		error("bad offset/count");
	if(isr){
		if(off >= MB)
			return 0;
		if(off+n >= MB)
			n = MB - off;
		memmove(a, KADDR(off), n);
	} else {
		/* realmode buf page ok, allow vga framebuf's access */
		if(off >= MB || off+n > MB ||
		    (off < LORMBUF  || off+n > LORMBUF+4*KB) &&
		    (off < VGAMEM() || off+n > 0xB0000+0x10000))
			error("bad offset/count in write");
		memmove(KADDR(off), a, n);
	}
	return n;
}

static long
rmemread(Chan*, void *a, long n, vlong off)
{
	return rmemrw(1, a, n, off);
}

static long
rmemwrite(Chan*, void *a, long n, vlong off)
{
	return rmemrw(0, a, n, off);
}

void
realmodelink(void)
{
	addarchfile("realmodemem", 0660, rmemread, rmemwrite);
}
