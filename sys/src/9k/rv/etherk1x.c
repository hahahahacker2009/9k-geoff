/*
 * driver for Spacemit K1x-emac Gb Ethernet MAC found in milk-v jupiter.
 * DMA is incoherent with caches.
 * written without benefit of adequate English documentation.
 * can only use bottom 4GB.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"
#include "ethermii.h"
#include <ip.h>

typedef struct Ctlr Ctlr;
typedef uvlong Macaddrregs;
typedef struct Rd Rd;
#define Td Rd

enum {
	Doreset	= 0,

	Rbsz	= ETHERMAXTU+4,	/* with CRC, ≤4k */
	Descalign= CACHELINESZ,

	/* tunable parameters */
	Ntd	= 128,		/* power of 2 */
	Nrd	= 128,		/* power of 2 */
	Nrb	= 2*Nrd,	/* private receive buffers, could be ≫ Nrd */
};

/* Macaddr and Statdata get padded to 16 bytes.  Declare as arrays in Regs. */
typedef ulong Macaddrarr[3];	/* 0 is hi, 1 med, 2 lo */
typedef struct Macaddr Macaddr;
struct Macaddr {
	ulong	hi;		/* first 2 bytes sent serially */
	ulong	med;
	ulong	lo;		/* last 2 bytes sent serially */
};

typedef ulong Statdataarr[3];
typedef struct Statdata Statdata;
struct Statdata {
	ulong	ctl;
	ulong	hi;
	ulong	lo;
};

typedef struct Regs Regs;
struct Regs {
	/* dma */
	ulong	dmacfg;
	ulong	dmactl;

	ulong	dmairqsts;
	ulong	dmaintren;

	ulong	dmatxpollctr;
	ulong	dmatxpolldmnd;
	ulong	dmarxpolldmnd;
	ulong	dmatxbase;
	ulong	dmarxbase;

	ulong	dmamissed;
	ulong	dmastopflsh;
	ulong	dmarxirqmitig;

	ulong	dmacurrtxdesc;
	ulong	dmacurrtxbuf;
	ulong	dmacurrrxdesc;
	ulong	dmacurrrxbuf;
	uchar	_0[0x100 - 0x40];

	/* mac */
	ulong	macctl;			/* "global" control */
	ulong	mactxctl;
	ulong	macrxctl;
	ulong	macmaxframe;
	ulong	mactxjabsz;
	ulong	macrxjabsz;
	ulong	macaddrctl;
	ulong	macmdioclkdiv;
	Macaddrarr macaddr[4];
	ulong	macmcasthash[4];	/* low 16 bits of each used */

	ulong	macfcctl;
	ulong	macfcpausefrmgen;
	Macaddrarr macfcsrc;
	Macaddrarr macfcdest;
	ulong	macfcpausetm;
	uchar	_1[0x1a0 - 0x184];

	ulong	macmdioctl;
	ulong	macmdiodata;

	Statdataarr macrxstat;
	Statdataarr mactxstat;
	ulong	mactxfifonear;
	ulong	mactxpktstart;
	ulong	macrxpktstart;
	uchar	_2[0x1e0 - 0x1cc];

	ulong	macirqsts;		/* uninteresting conditions */
	ulong	macintren;		/* uninteresting conditions */
};

enum {					/* dmacfg: dma config */
	Swrst		= 1<<0,
	Burstlen	= MASK(7)<<1,
	Descskipshft	= 8,
	Descskip	= MASK(5)<<Descskipshft,
/* for Receive and Transmit DMA operate in Big-Endian mode for descriptors */
	Descbyteorder	= 1<<13,
	Descbiglittleend= 1<<14,
	Txrxarb		= 1<<15,
	Waitdone	= 1<<16,
	Strictburst	= 1<<17,
	Dma64bit	= 1<<18,	/* 64-bit transfers, not addresses */
};

enum {					/* dmactl: dma control */
	Txstart = 1<<0,
	Rxstart = 1<<1,
};

enum {		/* dmaintren, dmairqsts: dma interrupt enables, status */
	Txdoneirq	= 1<<0,
	Txdescunavailirq= 1<<1,		/* uninteresting: no more to send */
	Txstoppedirq	= 1<<2,
	Rxdoneirq	= 1<<4,
	Rxdescunavailirq= 1<<5,
	Rxstoppedirq	= 1<<6,
	Rxrcvmissedirq	= 1<<7,
	Macirq		= 1<<8,		/* uninteresting */
};
enum {		/* dmairqsts: dma irq status; write 1 bit to clear irq */
	Txdmastate	= MASK(3)<<16,
	Rxdmastate	= MASK(4)<<20,
};

enum {					/* dmarxirqmitig: rx irq status */
	Rxirqfrms	= MASK(8),
	Rxirqtmouts	= MASK(20)<<8,
	Rxirqfrmmode	= 1<<30,
	Rxirqmiten	= 1u<<31,
};

enum {					/* macctl: mac control */
	Macspeed	= MASK(2),
	Macspeed1g	= 2,
	Macfulldpx	= 1<<2,
	Macrstrxstats	= 1<<3,
	Macrsttxstats	= 1<<4,
	Macucastwake	= 1<<8,
	Macmagpktwake	= 1<<9,
};

enum {					/* mactxctl: mac transmit control */
	Mactxen		= 1<<0,
	Macinvfcs	= 1<<1,
	Macdisfcsins	= 1<<2,
	Mactxretry	= 1<<3,
	Macifg		= MASK(3)<<4,
	Macpreamb	= MASK(3)<<7,
};

enum {					/* macrxctl: mac receive control */
	Macrxen		= 1<<0,
	Macdisfcsck	= 1<<1,
	Macstripfcs	= 1<<2,
	Macstorefwd	= 1<<3,
	Macsts1st	= 1<<4,
	Macaccbad	= 1<<5,
	Maccntvlan	= 1<<6,
};

enum {					/* macaddrctl: mac address control */
	Macaddr1en = 1<<0,
	Macaddr2en = 1<<1,
	Macaddr3en = 1<<2,
	Macaddr4en = 1<<3,
	Macinvaddr1en = 1<<4,
	Macinvaddr2en = 1<<5,
	Macinvaddr3en = 1<<6,
	Macinvaddr4en = 1<<7,
	Macprom = 1<<8,
};

enum {					/* macmdioctl: mac mdio control */
	Macphyaddr	= MASK(5),
	Macregaddrshft	= 5,
	Macregaddr	= MASK(5)<<Macregaddrshft,
	Macmdiord	= 1<<10,	/* read, not write */
	Macstmdioxfr	= 1<<15,	/* start transfer or in progress */
};

enum {					/* mactxstat: mac tx stats control */
	Mactxctrno = MASK(5),
	Macsttxctrrd = 1<<15,
};

enum {					/* macirqsts: mac irq status */
	Macunderrunirq = 1<<0,
	Macjabberirq = 1<<1,
};

enum {					/* macintren: mac interrupt enable */
	Macunderrunie = 1<<0,
	Macjabberie = 1<<1,
};

/*
 * receive & transmit descriptors
 */
#define DESCPAD	(CACHELINESZ/sizeof(long) - 4)	/* longs to next cacheline */
struct Rd {
	ulong	status;
	ulong	sizes;
	ulong	buf1;
	ulong	buf2;

	ulong	pad[DESCPAD];	/* avoid sharing cache lines with other descs */
};

enum {					/* Rd status */
	Rdpktlen = MASK(14),
	Rdcrcerr = 1<<20,
	Rdlast	= 1<<29,
	Rdfirst = 1<<30,
	Rdown	= 1u<<31,		/* owned by hw */
};
enum {					/* Rd sizes */
	Rdbufsz1 = MASK(12),
	Rdbufsz2 = MASK(12)<<12,
	Rd2ndchain = 1<<25,
	Rdendring = 1<<26,
};

enum {					/* Td status */
	Tdpktsts = MASK(30),
	Tdown	= 1u<<31,		/* owned by hw */
};
enum {					/* Td sizes */
	Tdbufsz1 = MASK(12),
	Tdbufsz2 = MASK(12)<<12,
	Td2ndchain= 1<<25,
	Tdendring = 1<<26,

	Tdnopad	= 1<<27,
	Tdnocrc = 1<<28,
	Tdfirst	= 1<<29,
	Tdlast	= 1<<30,
	Tddoneintr= 1u<<31,
};

typedef struct {
	uint	reg;
	char	*name;
} Stat;

static Stat stattab[] = {
	0,	nil,
};

struct Ctlr {
	Ethident;		/* see etherif.h, includes regs* */

	Lock	reglock;
	uint	im;		/* interrupt mask (enables) */
	uint	lim;
	uint	rim;
	uint	tim;

	Rendez	rrendez;
	Rd*	rdba;		/* receive descriptor base address */
	uint	rdh;		/* " " head */
	uint	rdt;		/* " " tail */
	Block**	rb;		/* " buffers */
	uint	rintr;
	uint	rsleep;
	Watermark wmrd;
	Watermark wmrb;
	int	rdfree;		/* " descriptors awaiting packets */

	Rendez	trendez;
	Td*	tdba;		/* transmit descriptor base address */
	uint	tdh;		/* " " head */
	uint	tdt;		/* " " tail */
	Block**	tb;		/* " buffers */
	uint	tintr;
	uint	tsleep;
	Watermark wmtd;
	QLock	tlock;
	ulong	txtick;		/* tick at start of last tx start */

	Rendez	lrendez;

	uchar	flag;
	uchar	procsrunning;	/* flag: kprocs started for this Ctlr? */
	uchar	attached;
	QLock	alock;		/* attach lock */

	uchar	ra[Eaddrlen];	/* receive address, copied to Ether->ea */
	Macaddrregs macregs;

	QLock	slock;
	ulong	stats[nelem(stattab)];

	uint	ixcs;		/* valid hw checksum (crc) count */
	uint	ipcs;		/* good hw ip checksums */
	uint	tcpcs;		/* good hw tcp/udp checksums */
};

enum {				/* flag bits */
	Factive	= 1<<0,
};

static	Ctlr	*ctlrtab[8];
static	int	nctlr;

/* these are shared by all the controllers of this type */
static	Lock	rblock;
static	Block	*rbpool;
/* # of rcv Blocks awaiting processing; can be briefly < 0 */
static	int	nrbfull;
static	int	nrbavail;

static void	ethshutdown(Ctlr *ctlr);
static void	multicast(void *a, uchar *addr, int on);
static void	replenish(Ctlr *ctlr);
static void	setmacs(Ctlr *);

static void
flushregs(void)			/* cope with weirdo hardware */
{
	coherence();
	delay(20);		/* guesswork */
}

static void
dump(ulong *p, int cnt)
{
	for (; cnt-- > 0; p++)
		if (*p)
			iprint("%#p: %#lux\n", p, *p);
}

static void
compare(ulong *old, ulong *new, int cnt)
{
	int off;
	ulong owd, nwd;

	for (off = 0; cnt-- > 0; off += 4, old++, new++) {
		owd = *old;
		nwd = *new;
		if (owd ^ nwd)
			iprint("%#x: %#lux %#lux: xor %#lux\n",
				off, owd, nwd, owd ^ nwd);
	}
}

static void
readstats(Ctlr *ctlr)
{
	uint i, reg;

	qlock(&ctlr->slock);
	for(i = 0; i < nelem(ctlr->stats); i++) {
		reg = stattab[i].reg;
		ctlr->stats[i] += ctlr->regs[reg / BY2WD];
	}
	qunlock(&ctlr->slock);
}

static long
ifstat(Ether *edev, void *a, long n, ulong offset)
{
	uint i;
	char *s, *p, *e;
	Ctlr *ctlr;
	Regs *regs;

	ctlr = edev->ctlr;
	p = s = malloc(READSTR);
	if(p == nil)
		error(Enomem);
	e = p + READSTR;

	readstats(ctlr);
	for(i = 0; i < nelem(stattab); i++)
		if(stattab[i].name != nil && ctlr->stats[i] > 0)
			p = seprint(p, e, "%s\t%uld\n", stattab[i].name,
				ctlr->stats[i]);
	regs = ctlr->vregs;
	p = seprint(p, e, "%Æ\n", ctlr);
	p = seprint(p, e, "macctl %#lux\n",  regs->macctl);
	p = seprint(p, e, "mactxctl %#lux\n",  regs->mactxctl);
	p = seprint(p, e, "macrxctl %#lux\n",  regs->macrxctl);
	p = seprint(p, e, "dmactl %#lux\n",  regs->dmactl);
	p = seprint(p, e, "dmacfg %#lux\n",  regs->dmacfg);
	p = seprint(p, e, "rintr %d rsleep %d\n", ctlr->rintr, ctlr->rsleep);
	p = seprint(p, e, "tintr %d tsleep %d\n", ctlr->tintr, ctlr->tsleep);
	p = seprint(p, e, "ixcs %d\n", ctlr->ixcs);
	p = seprintmark(p, e, &ctlr->wmrb);
	p = seprintmark(p, e, &ctlr->wmrd);
	seprintmark(p, e, &ctlr->wmtd);
	n = readstr(offset, a, n, s);
	free(s);

	return n;
}

static void
ienablelcked(Ctlr *ctlr, int ie)
{
	Regs *regs = ctlr->vregs;

	ctlr->im |= ie;
	regs->dmaintren |= ie;
}

static void
ienable(Ctlr *ctlr, int ie)
{
	ilock(&ctlr->reglock);
	ienablelcked(ctlr, ie);
	iunlock(&ctlr->reglock);
}

/* return a Block from our pool */
static Block*
rballoc(void)
{
	Block *bp;

	ilock(&rblock);
	if((bp = rbpool) != nil){
		rbpool = bp->next;
		bp->next = nil;
		adec(&nrbavail);
		if (nrbavail < 0)
			print("etherk1x rballoc: nrbavail negative\n");
	}
	iunlock(&rblock);
	if (bp) {
		/* invalidate buffer before dma */
		cachedwbinvse(cachedview(bp->rp), Rbsz);
		bp->rp = uncachedview(bp->rp);
		bp = uncachedview(bp);
	}
	return bp;
}

/* called from freeb for receive Blocks, returns them to our pool */
void
rbfree(Block *bp)
{
	bp->wp = bp->rp = (uchar *)ROUNDDN((uintptr)bp->lim - Rbsz, BLOCKALIGN);
	assert(bp->rp >= bp->base);
	assert(((uintptr)bp->rp & (BLOCKALIGN-1)) == 0);
	bp->flag &= ~(Bipck | Budpck | Btcpck | Bpktck);

	ilock(&rblock);
	bp->next = rbpool;
	rbpool = bp;
	adec(&nrbfull);
	ainc(&nrbavail);
	iunlock(&rblock);
}

/* reclaim sent tx buffers & descriptors */
static void
cleanup(Ctlr *ctlr)
{
	uint i, tdh;
	Block *bp;
	Td *td;

	tdh = ctlr->tdh;
	i = 0;
	while (tdh != ctlr->tdt && i++ < Ntd-2) {
		td = &ctlr->tdba[tdh];
		cachedwbinvse(td, sizeof *td);	/* invalidate td->status */
		if (td->status & Tdown)	/* being transmitted? */
			break;
		bp = cachedview(ctlr->tb[tdh]);
		ctlr->tb[tdh] = nil;
		if (bp)
			freeb(bp);
		tdh = NEXT(tdh, Ntd);
	}
	ctlr->tdh = tdh;
	notemark(&ctlr->wmrb, nrbfull);
}

void
transmit(Ether *edev)
{
	Block *bp;
	Ctlr *ctlr;
	Regs *regs;
	Td *td;
	uint nqd, tdt, tdh, len;

	ctlr = edev->ctlr;
	if(!canqlock(&ctlr->tlock)){
		ienable(ctlr, Txdoneirq|Txstoppedirq);
		return;
	}
	if (!ctlr->attached) {
		qunlock(&ctlr->tlock);
		return;
	}
	cleanup(ctlr);				/* free transmitted buffers */
	tdh = ctlr->tdh;
	tdt = ctlr->tdt;
	assert(ctlr->tdba != nil);
	regs = ctlr->vregs;
	for(nqd = 0; NEXT(tdt, Ntd) != tdh; nqd++){	/* ring not full? */
		td = &ctlr->tdba[tdt];
		cachedwbinvse(td, sizeof *td);	/* invalidate td->status */
		if (td->status & Tdown)		/* tail still being xmitted? */
			break;
		if((bp = qget(edev->oq)) == nil)
			break;
		bp = uncachedview(bp);
		bp->rp = uncachedview(bp->rp);

		/* populate first available desc. for block bp */
		len = BLEN(bp);
		cachedwbse(bp->rp, len);	/* force packet to ram */
		if (len > ETHERMAXTU)
			iprint("%Æ: tx len %d too big\n", ctlr, len);
		if (ctlr->tb[tdt])
			panic("%Æ: xmit q full", ctlr);
		ctlr->tb[tdt] = uncachedview(bp);

		td->buf1 = (uintptr)cachedview((void *)PADDR(bp->rp));
		td->buf2 = 0;
		td->sizes &= ~Tdbufsz1;
		td->sizes |= Tdfirst | Tdlast | Tddoneintr | len;
		cachedwbse(td, sizeof *td);	/* push out desc. without Own */

		td->status |= Tdown;		/* allows xmit */
		cachedwbse(td, sizeof *td);	/* push out desc. with Own */

		tdt = NEXT(tdt, Ntd);
	}
	/* note size of queue of tds awaiting transmission */
	notemark(&ctlr->wmtd, (uint)(tdt + Ntd - tdh) % Ntd);
	if (NEXT(tdt, Ntd) == tdh)
		iprint("%Æ: out ring full. tdh %d tdt %d\n", ctlr, tdh, tdt);

	if(nqd) {
		ctlr->tdt = tdt;
		ctlr->txtick = sys->ticks;
		ilock(&ctlr->reglock);
		regs->mactxctl |= Mactxen|Mactxretry;	/* kick transmitter */
		coherence();
		regs->dmactl |= Txstart;
		coherence();
		regs->dmatxpolldmnd = 0xff;		/* start dma out */
		ienablelcked(ctlr, Txdoneirq|Txstoppedirq);
		iunlock(&ctlr->reglock);
	}
	qunlock(&ctlr->tlock);
}

static int
tim(void *vc)
{
	return ((Ctlr *)vc)->tim;
}

static void
tproc(void *v)
{
	ulong txtick;
	Ctlr *ctlr;
	Ether *edev;

	edev = v;
	ctlr = edev->ctlr;
	for (;;) {
		ctlr->tsleep++;
		/* the time-out is only for diagnosis */
		tsleep(&ctlr->trendez, tim, ctlr, 1000);
		ctlr->tim = 0;
		txtick = ctlr->txtick;
		if (txtick && sys->ticks - txtick > 2*HZ) {
			print("%Æ: not transmitting\n", ctlr);
			ctlr->txtick = 0;
		}
		/*
		 * perhaps some buffers have been transmitted and their
		 * descriptors can be reused to copy Blocks out of edev->oq.
		 */
		transmit(edev);
	}
}

/* free any buffer Blocks in an array */
static void
freeblks(Block **blks, int count)
{
	Block *bp;

	if (blks == nil)
		return;
	while(count-- > 0){
		bp = blks[count];
		blks[count] = nil;
		if (bp) {
			bp->free = nil;
			freeb(bp);
		}
	}
}

static void
initrxring(Ctlr *ctlr, Rd *rdbase, int ndescs)
{
	int i;

	ctlr->rdba = rdbase;
	for (i = 0; i < ndescs; i++)
		// rd->status = 0; /* no Rdown prevents rcv filling for now */
		rdbase[i].sizes = Rbsz;
	rdbase[ndescs - 1].sizes |= Rdendring;
	cachedwbse(rdbase, ndescs * sizeof(Rd));
	((Regs *)ctlr->regs)->dmarxbase = PADDR(rdbase);
	ctlr->rdfree = ctlr->rdh = ctlr->rdt = 0;
	flushregs();
}

static void
inittxring(Ctlr *ctlr, Td *tdbase, int ndescs)
{
	ctlr->tdba = tdbase;
	// td->status = 0;		/* available to fill to send */
	tdbase[ndescs - 1].sizes |= Tdendring;
	cachedwbse(tdbase, ndescs * sizeof(Td));
	((Regs *)ctlr->regs)->dmatxbase = PADDR(tdbase);
	ctlr->tdh = ctlr->tdt = 0;
	flushregs();
}

/* dir: 0 = read, 1 = write */
static int
mdio(Regs *regs, uchar phy, uchar reg, int wr, int data)
{
	awaitbitpat(&regs->macmdioctl, Macstmdioxfr, 0);
	regs->macmdiodata = (wr? data: 0);
	flushregs();
	regs->macmdioctl = phy | reg << Macregaddrshft | Macstmdioxfr |
		(!wr? Macmdiord: 0);
	flushregs();
	awaitbitpat(&regs->macmdioctl, Macstmdioxfr, 0);
	if (!wr)
		data = regs->macmdiodata;
	return data;
}

static void
dumpbmsr(Regs *regs, int phyaddr)
{
	int i;

	mdio(regs, phyaddr, Bmsr, Read, 0);
	i = mdio(regs, phyaddr, Bmsr, Read, 0);
	iprint("Bmsr %#ux link status %d auto neg complete %d\n",
		i, (i&BmsrLs != 0), (i&BmsrAnc != 0));
}

enum {
	Linkupbits = BmsrLs | BmsrAnc,
};

static int
islinkup(Regs *regs, int phyaddr)
{
	/* Bmsr is latched-low; read twice to clear */
	mdio(regs, phyaddr, Bmsr, Read, 0);
	return (mdio(regs, phyaddr, Bmsr, Read, 0) & Linkupbits) == Linkupbits;
}

static void
linkup(Ether *edev, Regs *regs)
{
	int phyaddr;

	phyaddr = edev->phyaddr;
	if (islinkup(regs, phyaddr)) {
		edev->link = 1;
		return;
	}

	mdio(regs, phyaddr, Mscr, Write, Mscr1000TFD|Mscr1000THD);
	mdio(regs, phyaddr, Anar, Write, AnaTXFD|AnaTXHD);
	mdio(regs, phyaddr, Bmcr, Write, BmcrDm|BmcrRan|BmcrAne);
	/* Wait briefly for link & autoneg completion; let it finish later */
	delay(20);
	edev->link = 1;
}

/*
 * Get the receiver into a usable state.  Some of this is boilerplate
 * that could be (or is) done automatically as part of reset,
 * but we also disable broken features (e.g., Intel's IP checksums).
 *
 * We're called before txinit, so arrange link auto negotiation here.
 */
static void
rxinit(Ctlr *ctlr)
{
	int i, oldphy;
	Ether *edev;
	Regs *regs;

	/*
	 * dmactl is 0 from u-boot, dmacfg is 0x70020
	 * (Waitdone|Strictburst|Dma64bit|16<<1)
	 */
	regs = ctlr->vregs;
	edev = ctlr->edev;
	ilock(&ctlr->reglock);
	oldphy = regs->macmdioctl & Macphyaddr;		/* was 1 on jupiter */
	USED(oldphy);
	ethshutdown(ctlr);

	edev->phyaddr = 1;		/* jupiter override */
	linkup(edev, regs);		/* was done at discovery */

	regs->dmacfg = regs->dmacfg & ~(Descskip|Burstlen) |
		DESCPAD << Descskipshft |
		Waitdone | Strictburst | Dma64bit | 16<<1;
	regs->macctl = (regs->macctl & ~Macspeed) | Macfulldpx | Macspeed1g;
	setmacs(ctlr);				/* in case reset undid it */
	for (i = 0; i < sizeof regs->macmcasthash; i++)
		regs->macmcasthash[i] = MASK(16); /* accept all mcast for now */
	regs->macaddrctl = Macaddr1en | Macprom;	/* accept all for now */
	flushregs();

	initrxring(ctlr, ctlr->rdba, Nrd);
	replenish(ctlr);
	delay(20);

	regs->macrxctl |= Macrxen;
	flushregs();
	regs->dmactl |= Rxstart;
	flushregs();
	regs->macrxpktstart = 60;		/* drop shorter packets */
	ienablelcked(ctlr, Rxdoneirq | Rxdescunavailirq | Rxstoppedirq |
		Rxrcvmissedirq);
	regs->dmarxpolldmnd = 0xff;		/* start dma in */
	iunlock(&ctlr->reglock);

	multicast(edev, ctlr->ra, 1);
}

static void
replenish(Ctlr *ctlr)
{
	uint rdt, rdh;
	Block *bp;
	Block **rb;
	Rd *rd;

	rdh = ctlr->rdh;
	for(rdt = ctlr->rdt; NEXT(rdt, Nrd) != rdh; rdt = NEXT(rdt, Nrd)){
		rd = &ctlr->rdba[rdt];
		cachedwbinvse(rd, sizeof *rd);	/* invalidate status */
		if (rd->status & Rdown)		/* still held by hw? */
			break;
		rb = &ctlr->rb[rdt];
		if(*rb != nil){
			iprint("%Æ: rx overrun\n", ctlr);
			break;
		}
		*rb = bp = rballoc();
		if(bp == nil)		/* don't have a buffer for this desc? */
			break;

		/* populate first available desc. for new block bp */
		rd->buf1 = PADDR(bp->rp);
		rd->buf2 = 0;
		rd->sizes &= ~Rdbufsz1;
		rd->sizes |= Rbsz;
		cachedwbse(rd, sizeof *rd);	/* push desc. without Own */
		rd->status &= ~Rdpktlen;
		rd->status |= Rdown | Rbsz;	/* Rdown allows reception */
		cachedwbse(rd, sizeof *rd);	/* push desc. with Own */
		ctlr->rdfree++;
	}
	ctlr->rdt = rdt;
}

/* returns true if no crc errors seen */
static int
ckcksum(Ctlr *ctlr, uint sts, Block *bp)
{
	if (sts & Rdcrcerr) {
		ctlr->edev->crcs++;
		return 0;
	} else {
		ctlr->ixcs++;
		bp->flag |= Bpktck;
		return 1;
	}
}

static int
qinpkt(Ctlr *ctlr)
{
	int passed;
	uint rdh, len;
	Block *bp;
	Etherpkt *pkt;
	Rd *rd;
	Regs *regs;

	ctlr->rim = 0;
	rdh = ctlr->rdh;
	rd = &ctlr->rdba[rdh];
	cachedwbinvse(rd, sizeof *rd);	/* invalidate rd->status */
	if (rd->status & Rdown)		/* still held by hw? */
		return -1;		/* wait for pkts to arrive */

	passed = 0;
	regs = (Regs *)ctlr->regs;
	bp = ctlr->rb[rdh];
	if ((rd->status & (Rdlast|Rdfirst)) == (Rdlast|Rdfirst)){
		if (bp == nil) {
			print("%Æ: nil Block* from ctlr->rb\n", ctlr);
			return -1;
		}
		len = rd->status & Rdpktlen;
		if (len <= ETHERMAXTU+4)
			bp->wp += len;
		else {
			bp->wp += ETHERMAXTU+4;		/* truncate */
			iprint("rcv pkt %d too big\n", len);
		}
		cachedinvse(bp->rp, len);
		pkt = (Etherpkt *)bp->rp;
		ckcksum(ctlr, rd->status, bp);
		bp->wp = cachedview(bp->wp);
		bp->rp = cachedview(bp->rp);
		if (bp->flag & Bpktck) {	/* good crc? */
			/*
			 * pass pkt in bp upstream, it will be freed eventually.
			 */
			etheriq(ctlr->edev, bp, 1);
			ainc(&nrbfull);
			bp = nil;
			passed++;
		} else
			iprint("%Æ: bad crc; ether type %#ux len %d\n",
				ctlr, pkt->type[0]<<8 | pkt->type[1], len);
	}
	if (!passed) {
		ainc(&nrbfull);			/* cancel adec in rbfree */
		freeb(bp);			/* toss bad pkt */
	}
	/* note size of queue of Blocks awaiting input processing */
	notemark(&ctlr->wmrb, nrbfull);

	ctlr->rb[rdh] = nil;
	ctlr->rdh = NEXT(rdh, Nrd);
	ctlr->rdfree--;
	/*
	 * if number of rds ready for packets is too low,
	 * set up the unready ones.
	 */
	if (ctlr->rdfree <= Nrd*3/4)
		replenish(ctlr);
	regs->dmarxpolldmnd = 0xff;		/* (re)start dma in */
	return passed;
}

static int
rim(void *vc)
{
	return ((Ctlr *)vc)->rim;
}

static void
rproc(void *v)
{
	int passed, npass;
	Ctlr *ctlr;
	Ether *edev;
	Regs *regs;

	edev = v;
	ctlr = edev->ctlr;
	regs = ctlr->vregs;
	for (;;) {
		replenish(ctlr);
		/*
		 * Prevent an idle or unplugged interface from interrupting.
		 * Allow receiver interrupts initially and again
		 * if the interface (and transmitter) see actual use.
		 */
//		if (edev->outpackets > 10 || ctlr->rintr < 2*Nrd)
			ienable(ctlr, Rxdoneirq | Rxdescunavailirq |
				Rxstoppedirq | Rxrcvmissedirq);
		ctlr->rsleep++;
		regs->dmarxpolldmnd = 0xff;		/* (re)start dma in */
		sleep(&ctlr->rrendez, rim, ctlr);

		for(passed = 0; (npass = qinpkt(ctlr)) >= 0; passed += npass)
			;
		/* note how many rds had full buffers */
		notemark(&ctlr->wmrd, passed);
	}
}

static void
promiscuous(void *a, int on)
{
	Ctlr *ctlr;
	Regs *regs;

	ctlr = ((Ether *)a)->ctlr;
	regs = ctlr->vregs;
	ilock(&ctlr->reglock);
	if(on)
		regs->macaddrctl |= Macprom;
//	else
//		regs->macaddrctl &= ~Macprom;	/* accept all for now */
	iunlock(&ctlr->reglock);
}

/* TODO: find right algorithm for k1x; see Spacekit K1 Tech. Ref. Man. */
static ulong
mcasthash(uchar *mac)
{
	uint chr, bit, hash;

	hash = 0;
	for (chr = 0; chr < Eaddrlen; chr++)
		for (bit = chr; bit < 8*Eaddrlen; bit += 6)
			if (mac[bit>>3] & (1 << (bit & 7)))
				hash ^= 1 << chr;
	return hash;
}

#define BITMAP16WD(u)	((uint)(u) / 16)
#define BITMAP16BIT(u)	(1UL << ((uint)(u) % 16))

static void
multicast(void *a, uchar *addr, int on)
{
	ulong hash, word, bit;
	Ctlr *ctlr;
	Regs *regs;

	/*
	 * multiple ether addresses can hash to the same filter bit,
	 * so it's never safe to clear a filter bit.
	 * if we want to clear filter bits, we need to keep track of
	 * all the multicast addresses in use, clear all the filter bits,
	 * then set the ones corresponding to in-use addresses.
	 */
	ctlr = ((Ether *)a)->ctlr;
	regs = ctlr->vregs;
	hash = mcasthash(addr);
	word = (uint)BITMAP16WD(hash) % nelem(regs->macmcasthash);
	bit = BITMAP16BIT(hash);
	ilock(&ctlr->reglock);
	if(on)
		regs->macmcasthash[word] |= bit;
//	else
//		regs->macmcasthash[word] &= ~bit;
	iunlock(&ctlr->reglock);
}

static void
freez(void **pptr)
{
	free(*pptr);
	*pptr = nil;
}

static void
freemem(Ctlr *ctlr)
{
	int i;
	Block *bp;

	/* only free enough rcv bufs for one controller */
	for (i = Nrb; i > 0 && (bp = rballoc()) != nil; i--){
		bp->free = nil;
		freeb(bp);
	}
	freez(&ctlr->rdba);
	freez(&ctlr->tdba);
	freez(&ctlr->rb);
	freez(&ctlr->tb);
}

#define GETMACREGS(mp) ((uvlong)(mp)->hi<<32 | (mp)->med<<16 | (mp)->lo)

/* if hw mac addr regs set, remember for later */
static void
macremember(Ctlr *ctlr)
{
	Macaddr *mp;
	Macaddrregs addr;

	mp = (Macaddr *)((Regs *)ctlr->vregs)->macaddr[0];
	addr = GETMACREGS(mp);
	if (etherismacset(addr) && !etherismacset(ctlr->macregs))
		ctlr->macregs = addr;
}

/* if remembered mac is set and hw regs are not, set hw regs to remembered */
static void
setmacregs(Ctlr *ctlr)
{
	Macaddr *mp;
	Macaddrregs addr;

	mp = (Macaddr *)((Regs *)ctlr->vregs)->macaddr[0];
	addr = ctlr->macregs;
	if (etherismacset(addr) && !etherismacset(GETMACREGS(mp))) {
		mp->hi  = (addr>>32) & MASK(16);	/* sent first */
		mp->med = (addr>>16) & MASK(16);
		mp->lo  = addr & MASK(16);
	}
}

/*
 * u-boot or the previous kernel should have left the primary mac address
 * in the mac address registers of the primary ethernet controller.
 * if we can pick it up from there, we're done.
 *
 * call while holding ctlr->reglock
 */
static void
setmacs(Ctlr *ctlr)
{
	uchar *ra;
	Macaddr *mp;
	Macaddrregs regadd;
	Regs *regs;
	static int ctlrno;	/* of this type */

	regs = ctlr->vregs;
	ra = ctlr->ra;
	mp = (Macaddr *)regs->macaddr[0];
	/* if hw mac regs are not set, set hw from ctlr->macregs, if set */
	regadd = GETMACREGS(mp);
	if (!etherismacset(regadd)) {
		setmacregs(ctlr);
		regadd = GETMACREGS(mp);
	}
	if (ethersetmac(ra, ctlrno, regadd)) {
		ctlrno++;
		iprint("%Æ: mac left unset by u-boot; setting to %E\n",
			ctlr, ra);
		mp->hi  = ra[1]<<8 | ra[0];	/* sent first */
		mp->med = ra[3]<<8 | ra[2];
		mp->lo  = ra[5]<<8 | ra[4];
		coherence();
	}
	macremember(ctlr);
	regs->macaddrctl |= Macaddr1en;
}

static void
ethshutdown(Ctlr *ctlr)
{
	Regs *regs;

	macremember(ctlr);

	/* shutdown manually in case we choose to not reset */
	regs = ctlr->vregs;
	regs->dmaintren = regs->macintren = 0;
	regs->mactxctl &= ~Mactxen;
	regs->macrxctl &= ~Macrxen;
	coherence();
	regs->dmactl = 0;
	flushregs();

	if (Doreset) {
		regs->dmacfg |= Swrst;
		coherence();
		/* reading dmacfg during reset seems to hang the system */
		delay(200);
		// ((Ether *)ctlr->edev)->link = 0;
	}
	regs->dmacfg = 0;
	flushregs();
	ctlr->im = 0;
}

/* don't discard all state; we may be attached again */
static int
detach(Ctlr *ctlr)
{
	ilock(&ctlr->reglock);
	ethshutdown(ctlr);
	iunlock(&ctlr->reglock);
	ctlr->attached = 0;
	return 0;
}

static void
shutdown(Ether *edev)
{
	detach(edev->ctlr);
	/* don't freemem; kprocs are using existing rings and we may reattach */
}

/* called from newctlr from discover with ctlr only partially populated */
static int
reset(Ctlr *ctlr)
{
	if (soc.newmach)
		iprint("resetting etherk1x, we hope...");
	if(detach(ctlr)){
		print("%Æ: reset timeout\n", ctlr);
		return -1;
	}

	if (ctlr->regs == nil) {
		print("%Æ: nil regs\n", ctlr);
		return -1;
	}
	/* if unknown, load mac address from non-volatile memory, if present */
	ilock(&ctlr->reglock);
	setmacs(ctlr);
	iunlock(&ctlr->reglock);
	readstats(ctlr);		/* zero stats by reading regs */
	memset(ctlr->stats, 0, sizeof ctlr->stats);
	if (soc.newmach)
		iprint("\n");
	return 0;
}

/*
 * Get the transmitter into a usable state.  Much of this is boilerplate
 * that could be (or is) done automatically as part of reset (hint, hint).
 */
static void
txinit(Ctlr *ctlr)
{
	Regs *regs;

	regs = ctlr->vregs;
	ilock(&ctlr->reglock);
	regs->dmactl &= ~Txstart;
	coherence();
	regs->mactxctl &= ~Mactxen;
	flushregs();

	/* set up tx queue 0 ring */
	inittxring(ctlr, ctlr->tdba, Ntd);

	regs->mactxctl |= Mactxen|Mactxretry;
	flushregs();
	regs->dmatxpollctr = 0;		/* magic */
	regs->mactxfifonear = 504;	/* recommended value 8; was 504 */
	regs->mactxpktstart = 192;	/* recommended value 1024; was 192 */
	coherence();
	regs->dmactl |= Txstart;
	flushregs();
	iunlock(&ctlr->reglock);
	ienable(ctlr, Txdoneirq|Txstoppedirq);
}

static void
allocall(Ctlr *ctlr)
{
	int i;
	Block *bp;
	static int first = 1;

	/* discard any buffer Blocks left over from before detach */
//	freeblks(ctlr->tb, Ntd);
//	freeblks(ctlr->rb, Nrd);

	if (ctlr->rdba == nil)
		ctlr->rdba = dmamalloc(Nrd * sizeof(Rd));
	if (ctlr->tdba == nil)
		ctlr->tdba = dmamalloc(Ntd * sizeof(Td));

	if (ctlr->rb == nil)
		ctlr->rb = malloc(Nrd * sizeof(Block *));
	if (ctlr->tb == nil)
		ctlr->tb = malloc(Ntd * sizeof(Block *));
	if (ctlr->rdba == nil || ctlr->tdba == nil ||
	    ctlr->rb == nil || ctlr->tb == nil)
		error(Enomem);

	cachedwbinvse(ctlr->rdba, Nrd * sizeof(Rd));	/* push out for dma */
	/* using only uncached addr from here */
	ctlr->rdba = uncachedview(ctlr->rdba);

	cachedwbinvse(ctlr->tdba, Ntd * sizeof(Td));	/* push out for dma */
	/* using only uncached addr from here */
	ctlr->tdba = uncachedview(ctlr->tdba);

	if (first) {
		first = 0;
		/* add enough rcv bufs for one controller to the pool */
		for(i = 0; i < Nrb; i++){
			bp = allocb(Rbsz);
			if(bp == nil)
				error(Enomem);
			assert(PADDR(bp) < 4ull*GB);
			bp->free = rbfree;
			freeb(bp);
		}
		aadd(&nrbfull, Nrb);	/* compensate for adecs in rbfree */
	}
}

static void
etherkproc(Ether *edev, void (*kp)(void *), char *sfx)
{
	char buf[KNAMELEN];

	snprint(buf, sizeof buf, "#l%d%s", edev->ctlrno, sfx);
	kproc(buf, kp, edev);
}

static void
startkprocs(Ctlr *ctlr)
{
	Ether *edev;

	if (ctlr->procsrunning)
		return;
	edev = ctlr->edev;
	etherkproc(edev, tproc, "xmit");
	etherkproc(edev, rproc, "rcv");
	ctlr->procsrunning = 1;
}

static void
wrvfy(uint *reg, uint val)		/* unused */
{
	uint rdval;

	*reg = val;
	coherence();
	rdval = *reg;
	if (rdval != val)
		print("wrote %#ux to %#p, but read back %#ux\n",
			val, reg, rdval);
}

static void
discoverlink(Ether *edev)
{
	Ctlr *ctlr;

	ctlr = edev->ctlr;
	qlock(&ctlr->alock);
	edev->phyaddr = 1;			/* jupiter override */
	linkup(edev, (Regs *)ctlr->regs);
	qunlock(&ctlr->alock);
}

/* could be attaching again after a detach */
static void
attach(Ether *edev)
{
	Ctlr *ctlr;

	ctlr = edev->ctlr;
	qlock(&ctlr->alock);
	if (ctlr->attached) {
		qunlock(&ctlr->alock);
		return;
	}
	assert(up != nil);
	if(waserror()){
		/* reset was called in newctlr at discovery */
		freemem(ctlr);
		qunlock(&ctlr->alock);
		nexterror();
	}
	if(ctlr->rdba == nil)
		allocall(ctlr);
	/* don't change nrbfull; it's shared by all controllers */
	initmark(&ctlr->wmrb, Nrb, "rcv Blocks not yet processed");
	initmark(&ctlr->wmrd, Nrd-1, "rcv descrs processed at once");
	initmark(&ctlr->wmtd, Ntd-1, "xmit descr queue len");

	rxinit(ctlr);
	txinit(ctlr);
	startkprocs(ctlr);
	ctlr->attached = 1;

	etherenableirqs(edev);
	qunlock(&ctlr->alock);
	if (soc.newmach && edev->ctlrno == 1)
		compare((ulong *)KADDR((uintptr)soc.ether[0]),
			(ulong *)KADDR((uintptr)soc.ether[1]),
			sizeof(Regs) / sizeof(ulong));
	poperror();
}

static Intrsvcret
interrupt(Ureg*, void *v)
{
	uint mskedsts, dmasts;
	Regs *regs;
	Ctlr *ctlr;

	/* mtrap and strap start with a fence */
	ctlr = ((Ether *)v)->ctlr;
	regs = ctlr->vregs;

	ilock(&ctlr->reglock);
	dmasts = regs->dmairqsts;
	regs->dmairqsts = dmasts; /* extinguish intr status to dismiss irq */

	mskedsts = dmasts & ctlr->im;
	if (mskedsts == 0) {		/* mainly for debugging */
		iunlock(&ctlr->reglock);
		if (dmasts != 0)
			iprint("%Æ: uninteresting intr, mskedsts %#ux vs im %#ux\n",
				ctlr, dmasts, ctlr->im);
		return Intrnotforme;
	}
	if(mskedsts & (Rxdoneirq|Rxdescunavailirq|Rxstoppedirq|Rxrcvmissedirq)){
		if (0 && mskedsts & Rxdescunavailirq)	/* boring */
			iprint("rx unavail...");
		if (mskedsts & Rxstoppedirq)
			iprint("rx stopped...");
		ctlr->rim = Rxdoneirq;
		ctlr->rintr++;
		wakeup(&ctlr->rrendez);
	}
	if(mskedsts & (Txdoneirq|Txstoppedirq)){
		if (mskedsts & Txstoppedirq)
			iprint("tx stopped...");
		ctlr->tim = Txdoneirq;
		ctlr->tintr++;
		ctlr->txtick = 0;
		wakeup(&ctlr->trendez);
	}
	iunlock(&ctlr->reglock);
	return Intrforme;
}

/*
 * map device p and populate a new Ctlr for it.
 * add the new Ctlr to our list.
 */
static Ctlr *
newctlr(void *regs)
{
	uintptr io;
	void *mem;
	Ctlr *ctlr;

	if (regs == nil)
		return nil;
	io = (uintptr)regs;
	mem = vmap(io, PGSZ);
	if(mem == nil){
		print("#l%d: etherk1x: can't map regs %#p\n", nctlr, regs);
		return nil;
	}
	if(probeulong(mem, Read) < 0) {
		vunmap(mem, PGSZ);
		return nil;
	}

	ctlr = malloc(sizeof *ctlr);
	if(ctlr == nil) {
		vunmap(mem, PGSZ);
		error(Enomem);
	}
	ctlr->regs = (uint*)mem;
	ctlr->physreg = (uint*)io;
	ctlr->prtype = "k1x";
	if(reset(ctlr)){
		print("%Æ: can't reset\n", ctlr);
		free(ctlr);
		vunmap(mem, PGSZ);
		return nil;
	}
	ctlrtab[nctlr++] = ctlr;
	return ctlr;
}

static void
discover(void)
{
	int i;

	for (i = 0; i < nelem(soc.ether); i++)
		newctlr(soc.ether[i]);
}

/* edev->ctlrno is set by etherprobe when called */
static int
addk1x(Ether *edev)
{
	int i;
	Ctlr *ctlr;
	Ioconf *iocf;

	if (edev == nil)
		panic("etherk1x pnp: nil edev arg");
	if(nctlr == 0)
		discover();
	ctlr = nil;
	for(i = 0; i < nctlr; i++){
		ctlr = ctlrtab[i];
		if(ctlr == nil || ctlr->flag & Factive)
			continue;
		if(edev->port == 0 || edev->port == (uintptr)ctlr->physreg)
			break;
	}
	if (i >= nctlr || ctlr == nil)
		return -1;
	ctlr->flag |= Factive;

	ctlr->edev = edev;		/* point back to Ether */
	edev->ctlr = ctlr;
	edev->port = (uintptr)ctlr->physreg;	/* for printing */
	iocf = ioconf("ether", edev->ctlrno);
	edev->irq = (iocf? iocf->irq: 0);
	edev->pcidev = nil;
	edev->tbdf = BUSUNKNOWN;
	edev->mbps = 1000;
	edev->maxmtu = ETHERMAXTU;
	memmove(edev->ea, ctlr->ra, Eaddrlen);

	edev->arg = edev;
	edev->attach = attach;
	edev->detach = shutdown;
	edev->transmit = transmit;
	edev->interrupt = interrupt;
	edev->ifstat = ifstat;
	edev->shutdown = shutdown;
	edev->ctl = nil;
	edev->multicast = multicast;
	edev->promiscuous = promiscuous;

	/* bringing link up here cuts at least 5 s. off boot time */
	discoverlink(edev);
	return 0;
}

void
etherk1xlink(void)
{
	addethercard("k1x", addk1x);
}
