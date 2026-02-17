/*
 * driver for Synopsys Gb Ethernet Mac (GMAC, aka DWC EQOS) v4.0 and later.
 * v4 introduced incompatible changes with v3 and earlier.
 *
 * VisionFive 2's is v5.20 (dwmac-5.20) with bus width 8.  The Eswin version has
 * bus width 4, implements multiple queues, and needs a lot of cache flushing
 * on the Premier P550, which has incoherent DMA.
 *
 * Descriptors for full or empty buffers (as appropriate) are added to a ring at
 * its tail and are removed at its head, whether by hardware or software.
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
#include "uncached.h"
#include <ip.h>

typedef struct Ctlr Ctlr;
typedef uvlong Macaddr;
typedef struct Rd Rd;

/* highest Addr* pair index */
#define	Maxmac(ctlr) ((ctlr)->variant == Vareswin? 8: 15)

enum {
	Debug	= 0,
	Debugdetail = 0,
	Desperate= 1,		/* flag: anything to make packets flow */

	Crclen = 4,
	/*
	 * the STi7105 manual gives the maximum frame size as 1518 bytes
	 * for non-VLAN frames, and requires the descriptor's frame size
	 * to be a multiple of 4 or 8 or 16 to match or exceed the bus width.
	 */
	Rbsz	= ROUNDUP(ETHERMAXTU+Crclen, 16),

	/* tunable parameters */
	/*
	 * systems can need more receive buffers
	 * due to bursts of (non-9P) traffic.
	 */
	Ntd	= 1024,		/* <= 1024 desc.s in either ring */
	Nrd	= 1024,
	Nrb	= 2*Nrd,	/* private receive buffers, > Nrd */
};

enum {				/* interesting dev registers, by ulong* index */
	Cfg	= 0,
	Extconf	= 4/4,		/* extended config */
	Pktfilt	= 8/4,		/* rcv filters */
	Hashbot	= 0x10/4,
	Hashtop	= 0x14/4,	/* hash is upper 6 bits of crc register(?) */
	Version	= 0x20/4,	/* usual location */
	Txq0flow= 0x70/4,
	Rxflow	= 0x90/4,
	Rxq0ctl0= 0xa0/4,	/* aka MAC_Rx0_Ctrl0 */
	Rxq0ctl1= 0xa4/4,
	Rxq0ctl2= 0xa8/4,
	Intsts	= 0xb0/4,
	Intenab	= 0xb4/4,
	Rxtxsts	= 0xb8/4,
	Phyif	= 0xf8/4,
	Versioneic = 0x110/4,	/* eic7700x location */
	Hwfeat0	= 0x11c/4,
	Addrhi0	= 0x300/4,
	Addrlo0	= 0x304/4,

	/* eswin has `MTL' registers starting at 0xc00 and 0xd00 */
	Txq0op	= 0xd00/4,	/* tx queue 0 operation mode */
	Rxq0op	= 0xd30/4,	/* Rx queue 0 operation mode */
	Rxq0missedpkts = 0xd34/4,
	Rxq0debug = 0xd38/4,

	/* mac mgmt counters */
	Mmcctl	= 0x0700/4,
	Mmcrxirq= 0x0704/4,
	Mmctxirq= 0x0708/4,
	Mmcrxirqmask = 0x070c/4,
	Mmctxirqmask = 0x0710/4,

	/* l3, l4 filtering */
	L3l4f0ctl = 0x900/4,	/* 0x30 stride between filters */

	/* dma registers */
	Dmamode	= 0x1000/4,	/* aka Bus Mode */
	Dmasysbusmode = 0x1004/4,
	Dmasts	= 0x1008/4,	/* aka DMA_IS */
	Opmode	= 0x1018/4,
	Dmaaxibusmode = 0x1028/4,	/* not documented by eswin */
	Dmactl	= 0x1100/4,
	Dmatxctl= 0x1104/4,
	Dmarxctl= 0x1108/4,

	Xmitbasehi= 0x1110/4,	/* tx desc. list addr (high) */
	Xmitbase= 0x1114/4,	/* tx desc. list addr (low) */
	Rcvbasehi= 0x1118/4,	/* rx desc. list addr (high) */
	Rcvbase	= 0x111c/4,	/* rx desc. list addr (low) */
	Xmittail= 0x1120/4,	/* low phys addr of desc. after last to send */
	Rcvtail	= 0x1128/4,	/* low phys addr of desc. after last to rcv */
	Xmitlen = 0x112c/4,	/*  tx desc. count in ring - 1 */
	Rcvlen	= 0x1130/4,	/* (rx desc. count in ring - 1) | misc. */
	Dmainten= 0x1134/4,	/* aka DMA_CH0_IE */
	Curtd	= 0x1144/4,	/* curr. Td address */
	Currd	= 0x114c/4,	/* curr. Rd address */
	Curtbufhi= 0x1150/4,
	Curtbuf	= 0x1154/4,
	Currbufhi= 0x1158/4,
	Currbuf	= 0x115c/4,
	Dma0sts	= 0x1160/4,	/* aka DMA_CH0_STA; read in intr svc */
};

enum {				/* Cfg bits */
	Ipc	= 1<<27,	/* checksum offload */
	Cst	= 1<<21,	/* strip crc of incoming packets of ether type */
	Acs	= 1<<20,	/* strip crc of incoming packets ≤ 1500 bytes */
	Jd	= 1<<17,	/* jabber disable */
	Je	= 1<<16,	/* jumbo enable (up to 9018 bytes) */
	Dm	= 1<<13,	/* full duplex */
	Te	= 1<<1,		/* xmit on */
	Re	= 1<<0,		/* rcvr on */
};

enum {				/* Pktfilt bits */
	Ra	= 1ul<<31,	/* disable address filters */
	Ipfe	= 1<<20,	/* l3 & l4 filter on */
	Hpf	= 1<<10,	/* hash or perfect filter on */
	Saf	= 1<<9,		/* src addr filter on */
	Saif	= 1<<8,		/* " " inverse filter on */
	Dbf	= 1<<5,		/* disable bcasts */
	Pm	= 1<<4,		/* accept all multicast */
	Daif	= 1<<3,		/* dest addr inverse filter on */
	Hmc	= 1<<2,		/* filter multicast by hash */
	Huc	= 1<<1,		/* " unicast " " */
	Pr	= 1<<0,		/* promiscuous */
};

enum {				/* Rxq0ctl0 bits */
	Rxq0en	= 2,		/* enable queue 0 for generic traffic */
};

enum {				/* Rxq0ctl1 bits */
	Mcbcqen	= 1<<20,	/* enable queue 0 for mcast/bcast */
};

enum {				/* Intenab, Intsts bits */
	Rxstsie	= 1<<14,	/* rcv errors in Rxtxsts */
	Txstsie	= 1<<13,	/* xmit errors in Rxtxsts */
};

enum {				/* Phyif bits */
	Lud	= 1<<1,		/* link up */
	Tc	= 1<<0,		/* transmit config to phy */
};

enum {				/* Txq0op bits */
	Tqs2k	= 7<<16,	/* queue len of 2048 in 256-byte blocks) - 1 */
	Txq0en	= 2<<2,
	Tsf	= 1<<1,		/* tx store-and-forward whole packets */
};

enum {				/* Rxq0op bits */
	Rqsshft	= 20,		/* rqs is log2(rcv-q-len/256)-1 */
	Ehfc	= 1<<7,		/* hw flow ctl on */
	Rsf	= 1<<5,		/* Rx store-and-forward whole packets */
	Fep	= 1<<4,		/* forward error packets */
};

enum {				/* Addrhi* bits */
	Ae	= 1u<<31,	/* address enabled */
};

enum {				/* Dmamode bits */
	Dche	= 1<<19,	/* descriptor prefetch on */
	Swr	= 1<<0,		/* software reset */
};

enum {				/* Dmasysbusmode bits */
	Enlpi	= 1u<<31,	/* enable low-power mode */
	Dmaeame	= 1<<11,	/* enhanced address mode enable: 64-bits */
	Aale	= 1<<10,	/* enable automatic low-power mode */
};

enum {				/* Dmarxctl, Dmatxctl bits */
	Ipbl	= 1<<15,	/* ignore pbl requirement */
	Ctlrstart= 1<<0,
};

enum {				/* Dmasts bits; not very interesting */
	Dmaisdma	= 1<<0,
	Dmaismtl	= 1<<16,
	Dmaismac	= 1<<17,
};

enum {				/* Dmainten & Dma0sts bits */
	Nie	= 1<<15,	/* normal intr summary enable */
	Aie	= 1<<14,	/* abnormal intr summary enable */
	Cdee	= 1<<13,	/* descriptor error */
	Fbee	= 1<<12,	/* fatal bus error */
	Rse	= 1<<8,		/* rcv stopped enable */
	Rbue	= 1<<7, 	/* rcv buf unavailable */
	Rie	= 1<<6,		/* rcv intr */
	Tbue	= 1<<2,		/* xmit buf unavailable */
	Txse	= 1<<1,		/* xmit stopped enable */
	Tie	= 1<<0,		/* xmit intr */
};

/* this is a register layout, which can't tolerate bogus padding */
#define DESCPAD	(32/sizeof(long) - 4)  /* longs to next cacheline (in theory) */
struct Rd {			/* Receive Descriptor */
	ulong	addr;
	ulong	addrhi;
	ulong	ctlcnts;	/* tx lengths */
	ulong	status;

	ulong	pad[DESCPAD];	/* avoid sharing cache lines with other descs */
};

enum {				/* Td->ctlcnts bits */
	Txioc	= 1ul<<31,	/* interrupt on tx completion */
};

enum {				/* Td-> and Rd->status bits */
	Own	= 1ul<<31,	/* owned by hw */
	Rxinte	= 1<<30,	/* intr on rcvd; set by host */
	Txfs	= 1<<29,	/* first segment */
	Txls	= 1<<28,	/* last segment */
	Rxbuf2v	= 1<<25,	/* set by host */
	Rxbuf1v	= 1<<24,	/* set by host */
	Crcerr	= 1<<24,	/* set by hw; read by host */
	Giant	= 1<<23,	/* set by hw; read by host */
	Rxwdog	= 1<<22,	/* set by hw; read by host */
	Rxovflo	= 1<<21,	/* set by hw; read by host */
	Rxerr	= 1<<20,	/* set by hw; read by host */
	Errsum	= 1<<15,	/* includes harmless trivia */
	Txjabber= 1<<14,	/* set by hw; read by host */
	Txpktflshed	= 1<<13,	/* set by hw; read by host */
	Txcarrierloss	= 1<<11,	/* set by hw; read by host */
	Txnocarrier	= 1<<10,	/* set by hw; read by host */
	Txlatecollis	= 1<<9,	/* set by hw; read by host */
	Txexcesscollis	= 1<<8,	/* set by hw; read by host */
	Txexcessdefer	= 1<<3,	/* set by hw; read by host */
	Txunderflow	= 1<<2,	/* set by hw; read by host */
};

#define Td Rd			/* Transmit Descriptor */

/* Td->addr is a byte address; there are no other bits in it */

typedef struct {
	uint	reg;
	char	*name;
} Stat;

static Stat stattab[] = {
	0,	nil,
};

struct Ctlr {
	Ethident;	/* see etherif.h */

	Lock	reglock;
	uint	im;	/* interrupt mask (enables) */
	uint	rim;	/* ie as rproc wakeup */
	uint	tim;	/* ie as tproc wakeup */
	uchar	trreq;	/* flag: there is work for xmit to do */

	Rendez	rrendez;
	Rd*	rdba;	/* receive descriptor base address */
	uint	rdh;	/* " " head index, next for sw to read */
	uint	rdt;	/* " " tail ", next after last valid desc. for hw to */
			/* fill.  eswin manual says it's "last valid desc." */
	Block**	rb;	/* " buffers */
	uint	rintr;
	uint	rsleep;
	Watermark wmrd;
	Watermark wmrb;
	int	rdfree;	/* " descriptors awaiting packets */

	Rendez	trendez;
	Td*	tdba;	/* transmit descriptor base address */
	uint	tdh;	/* " " head index, next to xmit */
	uint	tdt;	/* " " tail ", next after last valid desc. for sw to */
			/* fill.  eswin manual says it's "last valid desc." */
	Block**	tb;	/* " buffers */
	uint	tintr;
	uint	tsleep;
	Watermark wmtd;
	QLock	tlock;
	ulong	txtick;	/* tick at start of last tx start */

	uchar	flag;		/* Factive or not */
	uchar	procsrunning;	/* flag: kprocs started for this Ctlr? */
	uchar	attached;
	uchar	variant;	/* weirdo dwmac variant */
	QLock	alock;		/* attach lock */
	uchar	needreset;	/* flag: ether is stuck, reset it */

	uchar	ra[Eaddrlen];	/* receive address */
	Macaddr	macregs;	/* saved from hardware if set */

	QLock	slock;
	ulong	stats[nelem(stattab)];

	uint	ixcs;		/* valid hw checksum (crc) count */
	uint	ipcs;		/* good hw ip checksums */
	uint	tcpcs;		/* good hw tcp/udp checksums */
};

enum {				/* flag bits */
	Factive	= 1<<0,
};

enum {				/* variant values */
	Vargeneric,
	Vareswin,
};

static	Ctlr	*ctlrtab[8];
static	int	nctlr;

extern uchar ether0mac[];

/* these are shared by all the controllers of this type */
static	Lock	rblock;
static	Block	*rbpool;
/* # of rcv Blocks awaiting processing; can be briefly < 0 */
static	int	nrbfull;
static	int	nrbavail;

static void	macremember(Ctlr *ctlr);
static void	replenish(Ctlr *ctlr);
static void	setmacs(Ctlr *ctlr);
static void	rxinit(Ctlr *ctlr);
static void	txinit(Ctlr *ctlr);

/* wait up to a second for bit in ctlr->regs[reg] to become zero */
static int
awaitregbitzero(Ctlr *ctlr, int reg, ulong bit)
{
	return awaitbitpat(&ctlr->regs[reg], bit, 0);
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
	p = seprint(p, e, "%Æ\n", ctlr);
	p = seprint(p, e, "cfg %#ux\n", ctlr->regs[Cfg]);
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
flushregs(Ctlr *ctlr)		/* cope with weirdo hardware */
{
	if (ctlr->variant == Vareswin) {
		coherence();
		microdelay(6);	/* empirical kludge.  5 works, 4 fails */
	}
}

static void
ienablelcked(Ctlr *ctlr, int ie)
{
	ctlr->im |= ie;
	ctlr->regs[Dmainten] |= ctlr->im | Nie | Cdee|Fbee|Rse|Txse;
	// ctlr->regs[Dmasts] = ~0;
	// ctlr->regs[Dma0sts] |= ctlr->im;
	flushregs(ctlr);
}

static void
ienable(Ctlr *ctlr, int ie)
{
	ilock(&ctlr->reglock);
	ienablelcked(ctlr, ie);
	iunlock(&ctlr->reglock);
}

/* return a receive Block from our pool, in uncached space */
static Block*
rballoc(void)
{
	Block *bp;

	ilock(&rblock);
	bp = rbpool;
	if (ISKERNNIL(bp))
		rbpool = bp = nil;
	if(bp != nil){
		rbpool = bp->next;
		bp->next = nil;
		adec(&nrbavail);
		if (nrbavail < 0)
			print("etherdwmac4 rballoc: nrbavail negative\n");
	}
	iunlock(&rblock);
	if (ISKERNNIL(bp))
		return nil;

	/* invalidate buffer before dma */
	if (ISKERNNIL(bp->rp))
		panic("rballoc: bp->rp is nil (%#p)", bp->rp);
//	cachedwbinvse(bp, sizeof *bp);
	/*
	 * wb seems unneeded, yet it is needed, perhaps to reach other cpus'
	 * caches?
	 */
	cachedwbinvse(bp->rp, Rbsz);
	return bp;
}

/* called from freeb for receive Blocks, returns them to our pool */
void
rbfree(Block *bp)
{
	if (ISKERNNIL(bp))
		return;
	bp->wp = bp->rp = (uchar *)ROUNDDN((uintptr)bp->lim - Rbsz, BLOCKALIGN);
	assert(bp->rp >= bp->base);
	if (ISKERNNIL(bp->rp))
		panic("rbfree: nil bp->rp");
	if (((uintptr)bp->rp & (BLOCKALIGN-1)) != 0)
		iprint("rbfree: bp->rp %#p unaligned\n", bp->rp);
	bp->flag &= ~(Bipck | Budpck | Btcpck | Bpktck);

	ilock(&rblock);
	bp->next = rbpool;
	rbpool = bp;
	adec(&nrbfull);
	ainc(&nrbavail);
	iunlock(&rblock);
}

void
dump(uchar *p, int len)
{
	while(len-- > 0)
		iprint(" %2.2ux", *p++);
	iprint("\n");
}

/*
 * The transmit ring is a queue of descriptors, of fixed maximum size.
 * Conceptually, as a queue not in a ring, the tail pointer is at a higher
 * address than the head pointer when the queue is not empty.  Descriptors in
 * the queue (from head to tail) have their Own bits set.
 *
 * New descriptors are added at the tail pointer, which is then advanced.
 * The controller transmits the packet described by the descriptor at its head
 * pointer with Own set, then clears its Own bit, advances its head pointer, and
 * ideally generates an interrupt.  When recycling such a descriptor, a new
 * Block pointer should be installed in it before setting Own again.
 */

/*
 * reclaim sent buffers & descriptors.  to avoid cache line collisions on
 * incoherent systems, try to keep head and tail at least 2 descriptors apart.
 */
static void
cleanup(Ctlr *ctlr)
{
	uint tdh, sts;
	Block *bp;
	Td *td;

	coherence();
	tdh = ctlr->tdh;
	while ((ctlr->tdt + Ntd - tdh) % Ntd > 4) { /* q longer than 4? */
		td = &ctlr->tdba[tdh];
		if (sys->ucstrat == Uncflush)
			cachedinvse(td, sizeof *td);
		/* else tdba is uncached, thus td is uncached */
		sts = td->status;
		if (sts & Own)		/* being transmitted? */
			break;
		if (sts & Errsum) {
			iprint("%Æ: cleanup: errsum in sts %#ux\n", ctlr, sts);
			((Ether *)ctlr->edev)->oerrs++;
		}
		bp = ctlr->tb[tdh];
		ctlr->tb[tdh] = nil;
		freeb(bp);
		/*
		 * setting Own here would allow this (non-existent) buffer
		 * to be transmitted.
		 */
		tdh = NEXT(tdh, Ntd);
	}
	ctlr->tdh = tdh;
	coherence();
}

static void
restart(Ctlr *ctlr, uint tdt)
{
	if ((ctlr->regs[Dmatxctl] & Ctlrstart) == 0)
		iprint("%Æ: xmitter stopped\n", ctlr);
	if (Debug)
		iprint("%Æ: out ring nearly full; xmit stopped? tdt %d tdh %d\n",
			ctlr, tdt, ctlr->tdh);
	/* give up, swreset in rproc & try to resume */
	ctlr->needreset = 1;
	wakeup(&ctlr->rrendez);
//	while (ctlr->needreset)			// TODO
		tsleep(&up->sleep, return0, 0, 250);	/* let rproc run */
}

static void
kick(Ctlr *ctlr, uint tdt, int stop)
{
	ctlr->tdt = tdt;
	ctlr->txtick = sys->ticks;

	ilock(&ctlr->reglock);
	if (stop) {
		ctlr->regs[Dmatxctl] &= ~Ctlrstart;
		ctlr->regs[Cfg] &= ~Te;
		flushregs(ctlr);
	}
	ctlr->regs[Dmatxctl] |= Ctlrstart; /* ensure xmit is listening */
	flushregs(ctlr);
	ctlr->regs[Cfg] |= Te;
	flushregs(ctlr);
	/* tdba is uncached */
	ctlr->regs[Xmittail] = PADDR(cachedview(&ctlr->tdba[tdt])); /* next desc. */
	flushregs(ctlr);
	ienablelcked(ctlr, Tie);
	iunlock(&ctlr->reglock);

	cleanup(ctlr);	/* free previously-transmitted buffers */
}

/*
 * set up this xmit descriptor for this Block's buffer.  use normal (cached)
 * dram address.  ether access to dram is uncached.
 * td is uncached.
 */
static void
setupxmitdesc(Td *td, Block *bp)
{
	uint len;
	uvlong addr;

	len = BLEN(bp);
	assert(len < 1600);		/* in case of bad uncached arithmetic */
	cachedwbse(bp->rp, len);	/* force packet to ram */
	if (Debug)
		iprint("out %d bytes...", len);

	addr = PCIWADDR(bp->rp);
	td->addr = addr;
	td->addrhi = addr >> 32;
	td->ctlcnts = Txioc | len;
	cachedwbse(td, sizeof *td);	/* push out desc. without Own */

	td->status = Txls | Txfs | Own | len; /* Own allows xmit */
	cachedwbinvse(td, sizeof *td);	/* push out desc. with Own */
}

/*
 * move as many Blocks as possible from edev's output queue
 * into its transmit ring.
 */
void
xmit(Ether *edev)
{
	uint nqd, tdt, qlen;
	Block *bp;
	Ctlr *ctlr;
	Mpl pl;
	Td *td;

	ctlr = edev->ctlr;
	if (ctlr->needreset)		/* rings changing under foot? */
		return;
	if(!canqlock(&ctlr->tlock)){
		ienable(ctlr, Tie);
		return;
	}
	if (!ctlr->attached) {
		iprint("%Æ: xmit called but ctlr not yet attached\n", ctlr);
		qunlock(&ctlr->tlock);
		return;
	}
	coherence();
	pl = splhi();
	tdt = ctlr->tdt;
	qlen = (tdt + Ntd - ctlr->tdh) % Ntd;
	for(nqd = 0; NEXT(tdt, Ntd) != ctlr->tdh; nqd++){ /* ring not full? */
		/* try to avoid cache line collisions: keep head & tail apart */
		if (qlen >= Ntd - 2)
			break;
		td = &ctlr->tdba[tdt];
		if (sys->ucstrat == Uncflush)
			cachedinvse(td, sizeof *td);
		/* else tdba is uncached, thus td is uncached */
		if (td->status & Own)		/* being transmitted? */
			break;
		bp = qget(edev->oq);
		if (ISKERNNIL(bp))
			break;

		/* got available desc. & Block */
		if (ISKERNNIL(bp->rp))
			panic("xmit: nil bp->rp");
		setupxmitdesc(td, bp);

		if (ctlr->tb[tdt])
			panic("%Æ: xmit ring full, tb[%d] not nil", ctlr, tdt);
		ctlr->tb[tdt] = bp;
		coherence();

		tdt = NEXT(tdt, Ntd);
		qlen = (tdt + Ntd - ctlr->tdh) % Ntd;

		if ((ctlr->regs[Dmatxctl] & Ctlrstart) == 0)
			iprint("%Æ: xmitter stopped\n", ctlr);
		if (qlen == Ntd/2 && ctlr->variant == Vareswin) {
			if (Debug)
				iprint("%Æ: xmitter not making progress. "
					"tdt %d tdh %d\n",
					ctlr, tdt, ctlr->tdh);
			kick(ctlr, tdt, 1);
		} else if(ctlr->variant == Vareswin)
			kick(ctlr, tdt, 0);
	}
	notemark(&ctlr->wmtd, qlen);
	/*
	 * a full ring can be legitimate, but eic7700's xmitter sometimes
	 * got stuck.  watch that it's still fixed.
	 */
	if (qlen > Ntd - 8 && ctlr->variant == Vareswin)
		restart(ctlr, tdt);
	else if(nqd || ctlr->variant == Vareswin)
		kick(ctlr, tdt, 0);
	splx(pl);
	qunlock(&ctlr->tlock);
}

static int
tim(void *vc)
{
	Ctlr *ctlr;

	ctlr = vc;
	return ctlr->tim | ctlr->trreq;
}

/*
 * do all the real work in this kproc to keep it on one hart,
 * avoiding cache-line access races on eic7700 at least.
 */
static void
tproc(void *v)
{
	Ctlr *ctlr;
	Ether *edev;

	edev = v;
	ctlr = edev->ctlr;
	spllo();
	for (;;) {
		ctlr->tsleep++;
		sleep(&ctlr->trendez, tim, ctlr);
		ctlr->tim = 0;
		/*
		 * perhaps some buffers have been transmitted and their
		 * descriptors can be reused to copy Blocks out of edev->oq.
		 */
		xmit(edev);
		ctlr->trreq = 0;
	}
}

/*
 * alert the kproc of work to do.
 */
void
transmit(Ether *edev)
{
	Ctlr *ctlr = edev->ctlr;

	ctlr->tim = ctlr->trreq = 1;
	coherence();
	wakeup(&ctlr->trendez);
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
		if (bp) {
			blks[count] = nil;
			bp->free = nil;
			freeb(bp);
		}
	}
}

static void
rxon(Ctlr *ctlr)
{
	ilock(&ctlr->reglock);
	ctlr->regs[Dmarxctl] = Ctlrstart | Rbsz<<1 | 4<<16; // 2<<16; /* pbl<<16 */
	flushregs(ctlr);
	ctlr->regs[Cfg] |= Re;
	ienablelcked(ctlr, Rie);
	iunlock(&ctlr->reglock);
}

static void
setdmaskip(Ctlr *ctlr)
{
	int buswidth, dmaskip;
	ulong otail;
	uint *regs;

	/* probe to find bus width */
	regs = ctlr->regs;
	otail = regs[Xmittail];
	regs[Xmittail] = 0xf;
	coherence();
	buswidth = (regs[Xmittail] ^ 0xf) + 1;
	regs[Xmittail] = otail;

	/*
	 * configure dma skipping between descs. in buswidth words.
	 * buswidth words between desc.s; 0 = contiguous desc.s in list.
	 * vf2  buswidth 8 dmaskip 2
	 * p550 buswidth 4 dmaskip 4 (DESCPAD=32/...)
	 */
	dmaskip = (sizeof(Rd) - 4*sizeof(long)) / buswidth;
	if (soc.newmach)
		iprint("%Æ: buswidth %d dmaskip %d\n", ctlr, buswidth, dmaskip);
	if (dmaskip > MASK(3))		/* cap it at field mask */
		panic("%Æ: dmaskip %d too big", ctlr, dmaskip);
	regs[Dmactl] = 1<<16 | dmaskip<<18;	/* pbl x 8 | dsl(dmaskip) */
}

/* call with ctlr->reglock held */
static void
swreset(Ctlr *ctlr)
{
	uint *regs;

	macremember(ctlr);

	regs = ctlr->regs;
	regs[Dmamode] = Swr;		/* clears mac addr regs? */
	flushregs(ctlr);
	awaitbitpat(&regs[Dmamode], Swr, 0);
	regs[Dmainten] = 0;
	flushregs(ctlr);
	regs[Dmasts] = ~0;		/* reset all bits */
}

/*
 * Get the receiver into a usable state.  Some of this is boilerplate
 * that could be (or is) done automatically as part of reset,
 * but we also disable unused or broken features (e.g., IP checksums).
 */
static void
rxinit(Ctlr *ctlr)
{
	int i;
	uint *regs;
	uvlong physbda;

	regs = ctlr->regs;
	ilock(&ctlr->reglock);
	swreset(ctlr);

	/* on jh7100, could see bits 18-20 of Aonsys+0xc for phy # */
	regs[Dmarxctl] &= ~Ctlrstart;
	regs[Dmatxctl] &= ~Ctlrstart;
	flushregs(ctlr);
	delay(1);

	/* turn off tx and rx */
	regs[Cfg] = Cst | Acs | Dm; /* Acs: exclude CRC from rcv buf & length */
	flushregs(ctlr);
	/* don't set Enlpi|Aale, to disable low-power mode */
	regs[Dmasysbusmode] = Dmaeame;		/* 64-bit addrs */
	flushregs(ctlr);
	regs[Phyif] = Lud | Tc;
	delay(1);
	ctlr->edev->link = 1;
	ctlr->rdfree = 0;
	if (Desperate) {	/* emergency use if mcasthash is wrong */
		regs[Hashtop] = regs[Hashbot] = ~0;	/* accept all mcast */
		regs[Pktfilt] = Ra | Pm | Hpf | Pr;	/* allow all */
	} else {
		regs[Hashtop] = regs[Hashbot] = 0;
		regs[Pktfilt] = Hmc | Hpf | Pr;		/* p550 needs Pr */
	}
	regs[Rxq0ctl0] = Rxq0en;	/* enable only queue 0 */
	regs[Rxq0ctl1] = Mcbcqen;	/* even for mcast/bcast */
	regs[Rxq0ctl2] = MASK(8);	/* all priorities to queue 0 */
	for (i = 0; i < 4; i++)
		regs[L3l4f0ctl + i*(0x30/4)] = 0;  /* disable fancy crud */
	iunlock(&ctlr->reglock);

	/* tear down any old ring */
	assert(ctlr->rb);
	for (i = 0; i < Nrd; i++)
		freeb(ctlr->rb[i]);
	memset(ctlr->rb, 0, Nrd * sizeof(Block *));
	memset(ctlr->rdba, 0, Nrd * sizeof(Rd));
	cachedwbinvse(ctlr->rdba, Nrd * sizeof(Rd));	/* push out for dma */

	/* set up the rcv ring */
	ctlr->rdh = ctlr->rdt = 0;
	setmacs(ctlr);			/* in case Swr cleared it */
	setdmaskip(ctlr);
	/* rqs is rcv_q_len/256-1 */
	regs[Rxq0op] = Fep | (4096/256-1)<<Rqsshft | Rsf | Ehfc;

	/* use normal dram address.  ether access is uncached. */
	physbda = PADDR(cachedview(ctlr->rdba));
	ilock(&ctlr->reglock);
	regs[Rcvbasehi] = (uvlong)physbda >> 32;
	regs[Rcvbase] = physbda;
	regs[Rcvtail] = physbda;
	coherence();
	regs[Rcvlen] = Nrd - 1;
	iunlock(&ctlr->reglock);

	flushregs(ctlr);
	replenish(ctlr);
	rxon(ctlr);
	flushregs(ctlr);
}

/*
 * The receive ring is a queue of descriptors, of fixed maximum size.
 * Conceptually, as a queue not in a ring, the tail pointer is at a higher
 * address than the head pointer when the queue is not empty.  Descriptors in
 * the queue (from head to just before tail) have their Own bits clear.
 * 
 * New descriptors are added at the tail pointer, which is then advanced.
 * The controller stores the next incoming packet described by the descriptor at
 * its head pointer with Own set, then clears its Own bit, advances its head
 * pointer, and ideally generates an interrupt.  When recycling such a
 * descriptor, a new Block pointer should be installed in it before setting Own
 * again.
 */

/* find used descriptors above tail and give them new receive buffers */
static void
replenish(Ctlr *ctlr)
{
	uint rdt, rdh, i;
	uvlong addr;
	Block *bp;
	Block **rb;
	Rd *rd;

	i = 0;
	coherence();
	rdh = ctlr->rdh;
	rdt = ctlr->rdt;
	for(; NEXT(rdt, Nrd) != rdh; rdt = NEXT(rdt, Nrd)){
		rd = &ctlr->rdba[rdt];
		if (sys->ucstrat == Uncflush)
			cachedinvse(rd, sizeof *rd);
		/* else rdba is uncached, thus rd is uncached */
		if (rd->status & Own)		/* being filled? */
			break;
		rb = &ctlr->rb[rdt];
		if(*rb != nil){
			iprint("%Æ: rx overrun; ctlr->rb[%d] = %#p\n",
				ctlr, rdt, *rb);
			break;
		}
		*rb = bp = rballoc();
		coherence();
		if(ISKERNNIL(bp)) {	/* don't have a buffer for this desc? */
			*rb = nil;
			coherence();
			break;
		}

		/*
		 * set up this rcv descriptor for new Block's buffer.
		 * use normal (cached) dram address.  if incoherent dma, ether
		 * access to dram is uncached.
		 */
		if (ISKERNNIL(bp->rp))
			panic("replenish: nil bp->rp %#p\n", bp->rp);
		addr = PCIWADDR(bp->rp);
		rd->addr = addr;
		rd->addrhi = addr >> 32;
		rd->ctlcnts = 0;
		cachedwbse(rd, sizeof *rd);	/* push desc. without Own */

		rd->status = Own | Rxinte | Rxbuf1v; /* hand to hw to fill */
		cachedwbinvse(rd, sizeof *rd);	/* push desc. with Own */

		if (Debugdetail)
			iprint("Rrdt %d...", rdt);
		ctlr->rdfree++;
		i++;
	}
	ctlr->rdt = rdt;
	if (i) {
		ilock(&ctlr->reglock);
		ctlr->regs[Dmarxctl] |= Ctlrstart;
		flushregs(ctlr);
		ctlr->regs[Cfg] |= Re;
		flushregs(ctlr);
		ctlr->regs[Rcvtail] =
			PADDR(cachedview(&ctlr->rdba[rdt]));	/* next desc. */
		ienablelcked(ctlr, Rie);
		iunlock(&ctlr->reglock);
	}
}

/* returns true if no crc errors seen */
static int
ckcksum(Ctlr *ctlr, uint sts, Block *bp)
{
	if (sts & Crcerr) {
		ctlr->edev->crcs++;
		return 0;
	} else {
		ctlr->ixcs++;
		bp->flag |= Bpktck;
		return 1;
	}
}

/* pass full packets at ring head upstream */
static int
qinpkt(Ctlr *ctlr)
{
	int passed;
	uint rdh, len, sts;
	Block *bp;
	Etherpkt *pkt;
	Mpl pl;
	Rd *rd;

	coherence();
	rdh = ctlr->rdh;
	rd = &ctlr->rdba[rdh];
	if (rd == nil)
		panic("%Æ: nil Rd* from ctlr->rdba", ctlr);
	if (sys->ucstrat == Uncflush)
		cachedinvse(rd, sizeof *rd);
	/* else rdba is uncached, thus rd is uncached */
	sts = rd->status;
	if (sts & Own)
		return -1;		/* no rcvd pkts waiting at ring head */

	/* we have an input packet */
	pl = splhi();
	coherence();
	bp = ctlr->rb[rdh];
	if (bp == nil)
		panic("%Æ: nil Block* from ctlr->rb[%d]", ctlr, rdh);
	ctlr->rb[rdh] = nil;
	ckcksum(ctlr, sts, bp);
	ctlr->rdh = NEXT(rdh, Nrd);
	--ctlr->rdfree;

	passed = 0;
	len = sts & MASK(14);
	if (len > ETHERMAXTU + Crclen)
		iprint("%Æ: ignoring jumbo\n", ctlr);
	else if ((sts & (Giant|Rxwdog|Rxovflo|Rxerr)) == 0) {
		bp->wp += len;
		/*
		 * buffer was flushed from caches before dma, now invalidate
		 * (some of) its cache lines, if any, before reading it, since a
		 * prefetcher may have polluted a cache.
		 */
		cachedinvse(bp->rp, len);
		pkt = (Etherpkt *)bp->rp;
		if (Debug)
			iprint("in %d bytes, rdh %d...", len, rdh);
		if (bp->flag & Bpktck) {	/* good crc? */
			/*
			 * pass pkt in bp upstream; it will be freed eventually.
			 */
			etheriq(ctlr->edev, bp, 1);
			bp = nil;
			passed++;
			ainc(&nrbfull);
		} else
			iprint("%Æ: bad crc, sts %#ux; ether type %#ux len %d\n",
				ctlr, sts, pkt->type[0]<<8 | pkt->type[1], len);
	} else if (sts & Errsum) {
		iprint("mtl rxq0 op %#ux\n", ctlr->regs[Rxq0op]);
		iprint("mtl rxq0 missed pkts %#ux\n",
			ctlr->regs[Rxq0missedpkts]);
		iprint("mtl rxq0 debug %#ux\n", ctlr->regs[Rxq0debug]);
		iprint("%Æ: qinpkt: errsum in sts %#ux\n", ctlr, sts);
	} else
		iprint("%Æ: qinpkt: sts %#ux\n", ctlr, sts);
	if (!passed) {
		ainc(&nrbfull);			/* cancel adec in rbfree */
		freeb(bp);			/* toss bad pkt */
	}
	/* note size of queue of Blocks awaiting input processing */
	notemark(&ctlr->wmrb, nrbfull);
	splx(pl);
	return passed;
}

static void
resetrxtx(Ctlr *ctlr)
{
	iprint("%Æ: resetting...", ctlr);
	qlock(&ctlr->tlock);
	rxinit(ctlr);
	txinit(ctlr);
	ctlr->needreset = 0;
	qunlock(&ctlr->tlock);
	iprint("\n");
}

/* returns true if there's been a recent receive interrupt */
static int
rim(void *vc)
{
	Ctlr *ctlr;

	ctlr = vc;
	return ctlr->rim | ctlr->needreset;
}

static void
rproc(void *v)
{
	int passed, npass;
	Ctlr *ctlr;
	Ether *edev;

	edev = v;
	ctlr = edev->ctlr;
	spllo();
	coherence();
	for (ctlr->rdh = 0; ; ) {
		/*
		 * Prevent an idle or unplugged interface from interrupting.
		 * Allow receiver interrupts initially and again
		 * if the interface (and transmitter) see actual use.
		 */
		if (Desperate || edev->outpackets > 10 || ctlr->rintr < Nrd)
			ienable(ctlr, Rie);
		ctlr->rsleep++;
		sleep(&ctlr->rrendez, rim, ctlr);
		ctlr->rim = 0;
		if (ctlr->needreset)
			resetrxtx(ctlr);
		/*
		 * controller added pkts at ring tail, so ring head now has
		 * full packet(s).
		 */
		for(passed = 0; (npass = qinpkt(ctlr)) >= 0; passed += npass)
			;
		if (passed <= 0)
			continue;
		if (Debugdetail)
			iprint("*R");
		/* note how many rds had full buffers */
		notemark(&ctlr->wmrd, passed);
		replenish(ctlr);
	}
}

static void
promiscuous(void *a, int on)
{
	Ctlr *ctlr;
	Ether *edev;

	edev = a;
	ctlr = edev->ctlr;
	ilock(&ctlr->reglock);
	if(on)
		ctlr->regs[Pktfilt] |= Pr;
	else if (!Desperate)
		ctlr->regs[Pktfilt] &= ~Pr;
	iunlock(&ctlr->reglock);
}

static ulong
mcasthash(Ctlr *ctlr, uchar *mac)
{
	uint i, hash, rev;

	/* apparently have to complement the crc on vf2; not on eswin? */
	hash = ethercrc(mac, Eaddrlen);
	if (ctlr->variant == Vargeneric)
		hash = ~hash;
	/* reverse bits, take 6 top-most. equivalently, reverse bottom 6. */
	rev = 0;
	for (i = 0; i < 6; i++) {
		rev |= (hash & 1) << (5-i);
		hash >>= 1;
	}
	return rev;
}

static void
multicast(void *a, uchar *addr, int on)
{
	ulong hash, word, bit;
	Ctlr *ctlr;

	if (a == nil)
		panic("multicast: nil edev arg");
	ctlr = ((Ether *)a)->ctlr;
	if (ctlr == nil)
		panic("multicast: nil edev->ctlr");

	/*
	 * multiple ether addresses can hash to the same filter bit,
	 * so it's never safe to clear a filter bit.
	 * if we want to clear filter bits, we need to keep track of
	 * all the multicast addresses in use, clear all the filter bits,
	 * then set the ones corresponding to in-use addresses.
	 */
	hash = mcasthash(ctlr, addr);
	word = Hashbot;
	if (BITMAPWD(hash) % 2 != 0)		/* mod 2 enforces bounds */
		word = Hashtop;
	bit = BITMAPBIT(hash);
	ilock(&ctlr->reglock);
	if(on)
		ctlr->regs[word] |= bit;
//	else
//		ctlr->regs[word] &= ~bit;
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

#define GETMACREGS(regs) \
	(((uvlong)(regs)[Addrhi0] & MASK(16))<<32 | (regs)[Addrlo0])

/* if hw mac addr regs set, remember for later */
static void
macremember(Ctlr *ctlr)
{
	Macaddr addr;

	addr = GETMACREGS(ctlr->regs);
	if (etherismacset(addr) && !etherismacset(ctlr->macregs))
		ctlr->macregs = addr;
}

/*
 * u-boot or the previous kernel should have left the primary mac address
 * in the mac address registers of the primary ethernet controller.
 * if we can pick it up from there, we're done.
 *
 * call while holding ctlr->reglock
 */
static void
setmacregs(Ctlr *ctlr)
{
	uint *regs;
	Macaddr addr;

	addr = ctlr->macregs;
	if (etherismacset(addr)) {
		/*
		 * set addr regs no matter what, the act of writing is
		 * significant to some controllers (e.g., eic7700x).
		 */
		regs = ctlr->regs;
		regs[Addrhi0] = addr>>32;
		coherence();
		regs[Addrlo0] = addr;
		coherence();
		regs[Addrhi0] = addr>>32 | Ae;
		coherence();
	}
}

/* don't discard all state; we may be attached again later */
static int
detach(Ctlr *ctlr)
{
	uint *regs;

	regs = ctlr->regs;
	assert(regs);
	/* ctlr->edev may be unset here */

	ilock(&ctlr->reglock);
	swreset(ctlr);
	regs[Intenab] = 0;
	regs[Intsts] = regs[Dma0sts] = ~0;
	regs[Cfg] &= ~(Te|Re);
	regs[Dmarxctl] &= ~Ctlrstart;
	regs[Dmatxctl] &= ~Ctlrstart;
	iunlock(&ctlr->reglock);
	flushregs(ctlr);
	delay(10);			/* let dma stop */
	ctlr->attached = 0;

	/* ensure mac addr persists across reboot, in case Swr reset it */
	setmacregs(ctlr);
	flushregs(ctlr);
	return 0;
}

static void
shutdown(Ether *edev)
{
	detach(edev->ctlr);
	/* don't freemem; kprocs are using existing rings and we may reattach */
}

/*
 * u-boot or the previous kernel should have left the primary mac address
 * in the mac address registers of the primary ethernet controller.
 * if we can pick it up from there, we're done.
 */
static void
setmacs(Ctlr *ctlr)
{
	int i;
	Macaddr regadd;
	uint *regs;
	uchar *ra;
	Ether *edev;
	static int ctlrno;	/* of this type */

	regs = ctlr->regs;
	edev = ctlr->edev;
	ra = ctlr->ra;
	ilock(&ctlr->reglock);
	/* if hw mac regs are not set, set hw from ctlr->macregs, if set */
	regadd = GETMACREGS(regs);
	if (!etherismacset(regadd)) {
		setmacregs(ctlr);
		regadd = GETMACREGS(regs);
	}
	if (ethersetmac(ra, ctlrno, regadd)) {
		ctlrno++;
		iprint("%Æ: mac unset; setting to %E, wiill activate\n",
			ctlr, ra);
		regs[Addrhi0] = ra[5]<<8  | ra[4];
		coherence();
		regs[Addrlo0] = ra[3]<<24 | ra[2]<<16 | ra[1]<<8 | ra[0];
		coherence();
		regs[Addrhi0] = ra[5]<<8  | ra[4] | Ae;
		coherence();
	}
	macremember(ctlr);
	for (i = Addrlo0 + 1; i <= Addrlo0 + 2*Maxmac(ctlr); i++)
		regs[i] = 0;
	iunlock(&ctlr->reglock);
	if (edev)
		multicast(edev, ra, 1);
}

/*
 * may be called from discover with ctlr only partially populated.
 */
static int
reset(Ctlr *ctlr)
{
	uint *regs;

	if (soc.newmach)
		iprint("resetting etherdwmac4...");
	if(detach(ctlr)){
		iprint(" reset timeout\n");
		return -1;
	}

	regs = ctlr->regs;
	if (regs == nil) {
		iprint(" nil regs\n");
		return -1;
	}
	if (ctlr->variant == Vareswin)
		iprint(" dwmac eswin version %#ux\n", regs[Versioneic]);
	else
		iprint(" dwmac version %#ux\n", regs[Version]);

	/* if unknown, load mac address from non-volatile memory, if present */
	setmacs(ctlr);
	readstats(ctlr);		/* zero stats by reading regs */
	memset(ctlr->stats, 0, sizeof ctlr->stats);
	return 0;
}

/*
 * Get the transmitter into a usable state.  Much of this is boilerplate
 * that could be (or is) done automatically as part of reset (hint, hint).
 */
static void
txinit(Ctlr *ctlr)
{
	int i;
	uint *regs;
	uvlong physbda;

	regs = ctlr->regs;
	ilock(&ctlr->reglock);
	regs[Cfg] &= ~Te;
	regs[Dmatxctl] &= ~Ctlrstart;
	iunlock(&ctlr->reglock);
	flushregs(ctlr);
	delay(1);

	/* tear down any old ring */
	assert(ctlr->tdba != nil);
	for (i = 0; i < Ntd; i++)
		freeb(ctlr->tb[i]);
	memset(ctlr->tb, 0, Ntd * sizeof(Block *));
	/* td->status = 0;	/* Own=0: available for us to fill to send */
	memset(ctlr->tdba, 0, Ntd * sizeof(Td));
	cachedwbinvse(ctlr->tdba, Ntd * sizeof(Td));	/* push out for dma */

	/* set up tx queue ring */
	ctlr->tdt = ctlr->tdh = 0;
	/* use normal dram address.  ether access is uncached. */
	physbda = PADDR(cachedview(ctlr->tdba));
	ilock(&ctlr->reglock);
	regs[Xmitbasehi] = (uvlong)physbda >> 32;
	regs[Xmitbase] = physbda;
	regs[Xmittail] = physbda;
	coherence();
	regs[Xmitlen] = Ntd - 1;
	coherence();
	regs[Txq0op] = Tqs2k | Txq0en | Tsf;
	flushregs(ctlr);

	regs[Dmatxctl] = Ctlrstart | Ipbl | 4<<16; // 2<<16; /* pbl<<16 */
	flushregs(ctlr);
	regs[Cfg] |= Te;
	flushregs(ctlr);
	iunlock(&ctlr->reglock);
}

static void
allocall(Ctlr *ctlr)
{
	int i;
	Block *bp;
	static int first = 1;

	/*
	 * allocate aligned to cache lines, and pad with an extra cache line.
	 * allocate in PteNc region, if available.
	 */
	if (ctlr->rdba == nil)
		ctlr->rdba = dmamallocalign(Nrd * sizeof(Rd), PGSZ);
	if (ctlr->tdba == nil)
		ctlr->tdba = dmamallocalign(Ntd * sizeof(Td), PGSZ);

	if (ctlr->rb == nil)
		ctlr->rb = malloc(Nrd * sizeof(Block *));
	if (ctlr->tb == nil)
		ctlr->tb = malloc(Ntd * sizeof(Block *));
	if (ctlr->rdba == nil || ctlr->tdba == nil ||
	    ctlr->rb == nil || ctlr->tb == nil)
		error(Enomem);

	/* using only uncached addrs from here */
	ctlr->rdba = uncachedview(ctlr->rdba);
	ctlr->tdba = uncachedview(ctlr->tdba);
	if (first) {
		first = 0;
		/* add enough rcv bufs for one controller to the pool */
		for(i = 0; i < Nrb; i++){
			bp = allocb(Rbsz);
			if(bp == nil)
				error(Enomem);
			bp->free = rbfree;
			freeb(bp);
		}
		aadd(&nrbfull, Nrb);	/* compensate for adecs in rbfree */
	} else {
		/* discard any buffer Blocks left over from before detach or reset */
		/* in case of reuse, rd->status = 0; */
		/* Own=0: prevent rcv filling by hw for now */
		memset(ctlr->rdba, 0, Nrd * sizeof(Rd));
		memset(ctlr->rb, 0, Nrd * sizeof(Block *));

		/* in case of reuse, td->status = 0; */
		/* Own=0: prevent transmission by hw for now */
		memset(ctlr->tdba, 0, Ntd * sizeof(Td));
		memset(ctlr->tb, 0, Ntd * sizeof(Block *));
	}
	cachedwbinvse(ctlr->rdba, Nrd * sizeof(Rd));	/* push out for dma */
	cachedwbinvse(ctlr->tdba, Ntd * sizeof(Td));
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
	etherkproc(edev, rproc, "recv");
	ctlr->procsrunning = 1;
}

/*
 * could be attaching again after a detach.
 * ether interrupt will have been enabled by etherprobe.
 */
static void
attach(Ether *edev)
{
	Ctlr *ctlr;
	static Lock attlock;

	ctlr = edev->ctlr;
	qlock(&ctlr->alock);
	if (ctlr->attached) {
		qunlock(&ctlr->alock);
		return;
	}
	if(waserror()){
		reset(ctlr);
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

	ilock(&attlock);
	rxinit(ctlr);
	txinit(ctlr);
	startkprocs(ctlr);
	ctlr->attached = 1;
	iunlock(&attlock);

	etherenableirqs(edev);
	qunlock(&ctlr->alock);
	poperror();
}

static void
resumetx(Ctlr *ctlr)
{
	/* clean dregs out of tx ring */
	cachedinvse(&ctlr->tdba[ctlr->tdh], sizeof(Td));
	cleanup(ctlr);
	/* give tx a kick to restart it */
	ctlr->regs[Dmatxctl] |= Ctlrstart;
	flushregs(ctlr);
	ctlr->regs[Cfg] |= Te;
	flushregs(ctlr);
	ctlr->regs[Xmittail] =
		PADDR(cachedview(&ctlr->tdba[ctlr->tdt])); /* next desc. */
	flushregs(ctlr);
}

static Intrsvcret
interrupt(Ureg*, void *ve)
{
	int dmasts, intsts, tint, rint;
	uint *regs;
	Ctlr *ctlr;

	/* mtrap and strap start with a fence */
	ctlr = ((Ether *)ve)->ctlr;
	regs = ctlr->regs;
	assert(regs);
	if (!ctlr->attached) {
		regs[Intenab] = regs[Dmainten] = 0;	/* disable all intrs */
		return Intrnotforme;
	}

	if (Debugdetail)
		iprint("*I%#ux", regs[Dma0sts]);
	ilock(&ctlr->reglock);
	dmasts = regs[Dma0sts];
	intsts = regs[Dmainten];
	USED(intsts);

	/* alternatively, have Curtd or Currd changed? */
	intsts = regs[Intsts];
	if (dmasts & (Cdee|Fbee)) {
		iprint("%Æ: fatal error. dma0sts %#ux dmatxctl %#ux\n",
			ctlr, dmasts, ctlr->regs[Dmatxctl]);
		ctlr->needreset = 1;
		wakeup(&ctlr->rrendez);
	}
	if (dmasts & Rse)
		iprint("%Æ: rcv stopped\n", ctlr);
	if (dmasts & Txse) {
		if (Debug)
			iprint("%Æ: xmit stopped. dma0sts %#ux dmatxctl %#ux\n",
				ctlr, dmasts, ctlr->regs[Dmatxctl]);
		resumetx(ctlr);
	}
	regs[Intsts] = intsts;		/* try to clear status */

	rint = dmasts & Rie;
	if (rint)
		ctlr->rintr++;
	ctlr->rim |= rint;

	tint = dmasts & Tie;
	if (tint) {
		ctlr->txtick = 0;
		ctlr->tintr++;
	}
	ctlr->tim |= tint;

	regs[Dma0sts] = dmasts;		/* ack the interrupts */
	regs[Dmasts] = regs[Dmasts];	/* clear Dmasts */
	iunlock(&ctlr->reglock);
	flushregs(ctlr);

	/* now that registers have been updated, wake sleepers */
	if(rint) {
		if (Debugdetail)
			iprint("*Wr");
		wakeup(&ctlr->rrendez);
	}
	if(tint) {
		if (Debugdetail)
			iprint("*Wt");
		wakeup(&ctlr->trendez);
	}
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
	if(probeulong(regs, Read) < 0) {
		print("#l%d: etherdwmac4: no regs at %#p\n", nctlr, regs);
		return nil;
	}
	mem = vmap(io, 64*KB);
	if(mem == nil){
		print("#l%d: etherdwmac4: can't map regs %#p\n", nctlr, regs);
		return nil;
	}

	ctlr = malloc(sizeof *ctlr);
	if(ctlr == nil) {
		vunmap(mem, 64*KB);
		error(Enomem);
	}
	ctlr->regs = (uint*)mem;
	ctlr->physreg = (uint*)io;
	ctlr->prtype = "dwmac4";
	ctlr->variant = Vargeneric;		/* default */
	if (ctlr->regs[Version] == 0 && ctlr->regs[Versioneic] == 0x1052)
		ctlr->variant = Vareswin;
	if(reset(ctlr)){
		print("%Æ: can't reset\n", ctlr);
		free(ctlr);
		vunmap(mem, 64*KB);
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

/*
 * called from etherprobe with edev->ctlrno set.
 * upon return, etherprobe will enable interrupts
 * for edev->irq if it has Attachenable not set.
 */
static int
adddwmac4(Ether *edev)
{
	int i;
	Ctlr *ctlr;
	Ioconf *iocf;

	if (edev == nil)
		panic("etherdwmac4 pnp: nil edev arg");
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
	return 0;
}

void
etherdwmac4link(void)
{
	addethercard("dwmac4", adddwmac4);
}
