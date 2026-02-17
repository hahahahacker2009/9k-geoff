/*
 * PC-specific code for
 * USB eXtensible Host Controller Interface (XHCI) driver
 * Super-speed USB 3.x.
 *
 * currently just a stub to shut down any XHCI controllers found
 * so that they can't generate interrupts.
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"
#include	"../port/error.h"
#include	"../port/usb.h"
#include	"../port/portusbxhci.h"
#include	"usbxhci.h"

static Ctlr* ctlrs[Nhcis];
static int maxxhci = Nhcis;

/*
 * bios handoff: take control from the bios's legacy support.
 *
 * Isn't this cap list search in a helper function?
 */
static void
getxhci(Ctlr* ctlr)
{
	int i, ptr, cap, sem;

	ptr = (ctlr->capio->capparms >> Ceecpshift) & Ceecpmask;
	for(; ptr != 0; ptr = pcicfgr8(ctlr->pcidev, ptr+1)){
		/*
		 * Check for validity.
		 * Can't be in standard header and must be double-word aligned.
		 */
		if(pcicfgptrbad(ptr))
			break;
		cap = pcicfgr8(ctlr->pcidev, ptr);
		if(cap != Clegacy)
			continue;

		sem = pcicfgr8(ctlr->pcidev, ptr+CLbiossem);
		if(sem == 0)
			continue;

		/* bios has control, take it back */
		pcicfgw8(ctlr->pcidev, ptr+CLossem, 1);
		for(i = 0; i < 100; i++){
			if(pcicfgr8(ctlr->pcidev, ptr+CLbiossem) == 0)
				break;
			delay(10);
		}
		if(i >= 100)
			dprint("xhci %#p: bios timed out\n", ctlr->capio);
		pcicfgw32(ctlr->pcidev, ptr+CLcontrol, 0);	/* no SMIs */
		ctlr->opio->config = 0;
		coherence();
		return;
	}
}

static void
xhcireset(Ctlr *ctlr)
{
	Eopio *opio;
	int i;

	ilock(ctlr);
	dprint("xhci %#p reset\n", ctlr->capio);
	opio = ctlr->opio;

	/*
	 * Turn off legacy mode. Some controllers won't
	 * interrupt us as expected otherwise.
	 */
	xhcirun(ctlr, 0);
	pcicfgw16(ctlr->pcidev, 0xc0, 0x2000);

	/*
	 * reclaim from bios
	 */
	getxhci(ctlr);

	/*
	 * clear high 32 bits of address signals if it's 64 bits capable.
	 * This is probably not needed but it does not hurt and others do it.
	 * This must be done to all xhci controllers in a system before further
	 * initialisation of them is done, in theory.  We haven't done that yet.
	 */
	if((ctlr->capio->capparms & C64) != 0){
		dprint("xhci: 64 bits\n");
		opio->seg = 0;
		coherence();
	}

	if(xhcidebugcapio != ctlr->capio){
		opio->cmd |= Chcreset;	/* controller reset */
		coherence();
		for(i = 0; i < 100; i++){
			if((opio->cmd & Chcreset) == 0)
				break;
			delay(1);
		}
		if(i >= 100)
			print("xhci: %#p controller reset timed out after %d ms\n",
				ctlr->capio, i);
	}

	/* requesting more interrupts per µframe may miss interrupts */
	opio->cmd &= ~Citcmask;
	opio->cmd |= 1 << Citcshift;		/* max of 1 intr. per 125 µs */
	coherence();
	switch(opio->cmd & Cflsmask){
	case Cfls1024:
		ctlr->nframes = 1024;
		break;
	case Cfls512:
		ctlr->nframes = 512;
		break;
	case Cfls256:
		ctlr->nframes = 256;
		break;
	default:
		panic("xhci: unknown fls %ld", opio->cmd & Cflsmask);
	}
	dprint("xhci: %d frames\n", ctlr->nframes);
	iunlock(ctlr);
}

static void
setdebug(Hci*, int d)
{
	xhcidebug = d;
}

static void
shutdown(Hci *hp)
{
	int i;
	Ctlr *ctlr;
	Eopio *opio;

	ctlr = hp->aux;
	ilock(ctlr);
	opio = ctlr->opio;
	opio->cmd &= ~Chcreset;	  /* paranoia: ensure a transition below */
	coherence();
	delay(1);
	opio->cmd |= Chcreset;		/* controller reset */
	coherence();
	for(i = 0; i < 100; i++){
		if((opio->cmd & Chcreset) == 0)
			break;
		delay(1);
	}
	if(i >= 100) {
		print("xhci: %#p controller reset timed out after %d ms\n",
			ctlr->capio, i);
		opio->cmd &= ~Chcreset;	/* force it out of reset */
		coherence();
	}
	delay(100);
	xhcirun(ctlr, 0);
//	opio->frbase = 0;	// was in ehci
	iunlock(ctlr);
}

static void
stop(Eopio *opio)
{
	opio->cmd &= ~Crun;
	coherence();
	delay(20);			/* 16 ms. max says xhci 1.1 spec */
}

static void
nointrs(Pcidev *p, ulong io)
{
	Ecapio *capio;
	Eopio *opio;

	capio = vmap(io, p->mem[0].size);
	if (capio == nil)
		panic("can't vmap usbxhci regs at %#p", io);
	opio = (Eopio*)((uintptr)capio + (capio->cap & 0xff));
	/* must stop before reset */
	stop(opio);
	opio->cmd |= Chcreset;		/* controller reset */
	coherence();
	delay(200);
	opio->cmd &= ~Inte;
	coherence();
	stop(opio);
	vunmap(capio, p->mem[0].size);
	pciclrbme(p);
}

static void
scanpci(void)
{
	static int already = 0;
	int i;
	ulong io;
	Ctlr *ctlr;
	Pcidev *p;
	Ecapio *capio;

	if(already)
		return;
	already = 1;
	p = nil;
	while ((p = pcimatch(p, 0, 0)) != nil) {
		/*
		 * Find XHCI controllers (Programming Interface = 0x30).
		 */
		if(p->ccrb != Pcibcserial || p->ccru != Pciscusb)
			continue;
		switch(p->ccrp){
		case 0x30:
			io = p->mem[0].bar & ~0x0f;
			break;
		default:
			continue;
		}
		if(io == 0){
			print("usbxhci: %ux/%ux: failed to map registers\n",
				p->vid, p->did);
			continue;
		}
		if(1) {
			print("usbxhci: shutting off usb 3 ctlr %#ux/%#ux\n",
				p->vid, p->did);
			nointrs(p, io);
			continue;
		}
		print("usbxhci: %#x %#x: port %#lux size %#x irq %d\n",
			p->vid, p->did, io, p->mem[0].size, p->intl);

		ctlr = malloc(sizeof(Ctlr));
		if (ctlr == nil)
			panic("usbxhci: out of memory");
		ctlr->pcidev = p;
		ctlr->physio = io;
		capio = ctlr->capio = vmap(io, p->mem[0].size);
		if (capio == nil)
			panic("can't vmap usbxhci regs at %#p", io);
		ctlr->opio = (Eopio*)((uintptr)capio + (capio->cap & 0xff));
		pcisetbme(p);
		pcisetpms(p, 0);
		for(i = 0; i < Nhcis; i++)
			if(ctlrs[i] == nil){
				ctlrs[i] = ctlr;
				break;
			}
		if(i >= Nhcis)
			print("xhci: bug: more than %d controllers\n", Nhcis);
		if (i >= maxxhci) {
			print("usbxhci: ignoring controllers after first %d, "
				"at %#p\n", maxxhci, io);
			ctlrs[i] = nil;
			free(ctlr);
		}
	}
}

static int
reset(Hci *hp)
{
	int i;
	char *s;
	Ctlr *ctlr;
	Ecapio *capio;
	Pcidev *p;
	static Lock resetlck;

	s = getconf("*maxxhci");
	if (s != nil && s[0] >= '0' && s[0] <= '9')
		maxxhci = atoi(s);
	if(maxxhci == 0 || getconf("*nousbxhci"))
		return -1;

	ilock(&resetlck);
	scanpci();

	/*
	 * Any adapter matches if no hp->port is supplied,
	 * otherwise the ports must match.
	 */
	ctlr = nil;
	for(i = 0; i < Nhcis && ctlrs[i] != nil; i++){
		ctlr = ctlrs[i];
		if(ctlr->active == 0)
		if(hp->port == 0 || hp->port == (uintptr)ctlr->capio){
			ctlr->active = 1;
			break;
		}
	}
	iunlock(&resetlck);
	if(i >= Nhcis || ctlrs[i] == nil)
		return -1;

	p = ctlr->pcidev;
	hp->aux = ctlr;
	hp->port = (uintptr)ctlr->capio;
	hp->irq = p->intl;
	hp->tbdf = p->tbdf;

	capio = ctlr->capio;
	hp->nports = capio->parms & Cnports;

	ddprint("xhci: %s, ncc %lud npcc %lud\n",
		capio->parms & 0x10000 ? "leds" : "no leds",
		(capio->parms >> 12) & 0xf, (capio->parms >> 8) & 0xf);
	ddprint("xhci: routing %s, %sport power ctl, %d ports\n",
		capio->parms & 0x40 ? "explicit" : "automatic",
		capio->parms & 0x10 ? "" : "no ", hp->nports);

	xhcireset(ctlr);
	xhcimeminit(ctlr);

	/*
	 * Linkage to the generic HCI driver.
	 */
	hcilinkage(hp);
	hp->shutdown = shutdown;
	hp->debug = setdebug;
	hp->type = "xhci";
	return 0;
}

void
usbxhcilink(void)
{
	addhcitype("xhci", reset);
}
