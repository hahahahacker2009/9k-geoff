/*
 * spacemit-specific stuff (for k1): soc init, reset.
 * watchdog is maybe the synopsys dw_apb_wdt.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

enum {
	/* SpacemiT K1 system controller (syscon/CCU) bases */
	Apbcbase	= 0xd4015000,
	Apbc2base	= 0xf0610000,
	Rcpubase 	= 0xc0880000,
	Rcpu2base 	= 0xc0888000,

	/* clock enable, de-assert and assert bits */
	Apbclken	= 1 << 0,
	Apbrst		= 1 << 2,
	Rcpudeass	= 1 << 0,
	Rcpuass		= 1 << 2,		/* if present */

	Notmpmubase	= 0xd4050000,
	Wdogclks	= 0xd4050200,
	Wdogripcrr	= 0xd4050210,
	Mpmuaprr	= 0xd4051020,		/* from spacemit linux */
	Wdogclken	= 0xd4051024,

	Mpmubase	= 0xd4280000,
	Resetreg	= Mpmubase + 0x200,
	Pmubase		= 0xd4282800,		/* apmu */
	Pmuaprr		= Pmubase + 0x1020,	/* from spacemit linux */
	Rebootreg	= 0xd4282f90,
	Dragonciu	= 0xd8440000,

	/* 4 possible wdog0 values: d4014000,  d4016000, d4080000, f0616000 */
};

enum {			/* emacX_clk_rst_ctrl */
	Emac0clkrstctl = Pmubase + 0x3e4,
	Emac1clkrstctl = Pmubase + 0x3ec,

	Emacaxiclken	= 1<<0,	/* enable emac axi bus clock */
	Emacrstdeass	= 1<<1,	/* reset when 0; not reset when 1 */
	Emacphyintfrgmii= 1<<2,	/* documented only in spacemit linux code */
	Emaccllk312m	= 1<<4,	/* 312M, not 208M clock */
	Emacrgmiitxclk	= 1<<8,	/* rgmii tx clock select from SoC */
	Emacaxi1id	= 1<<13, /* axi single id */
	Emactxclk125m	= 1<<14, /* 125M, not 25M, TX clock */
};

typedef struct Spacedog {
	uchar	_0[0xb0];
	ulong	twfar;		/* access registers; see unlockdog */
	ulong	twsar;
	ulong	twmer;		/* match enable */
	ulong	twmr;		/* match value in low short */
	ulong	twsts;		/* status */
	ulong	twicr;		/* intr clear */
	ulong	twcrr;		/* write 1 to reset count */
	ulong	twvr;		/* current count */
} Spacedog;

enum {				/* twmer bits */
	Wrie	= 1<<1,		/* reset instead of interrupt */
	We	= 1,		/* watchdog count enable */
};

/*
 * APBC offsets we can touch
 * (Subset of the published list; add more as needed.)
 */
static uchar apbcclkoffs[] = {
	0x00,	/* UART1_CLK_RST */	0x04,	/* UART2_CLK_RST */
	0x08,	/* GPIO_CLK_RST */	0x0C,	/* PWM0_CLK_RST */
	0x10,	/* PWM1_CLK_RST */	0x20,	/* TWSI8_CLK_RST */
	0x24,	/* UART3_CLK_RST */	0x28,	/* RTC_CLK_RST */
	0x2C,	/* TWSI0_CLK_RST */	0x34,	/* TIMERS1_CLK_RST */
	0x3C,	/* AIB_CLK_RST */	0x44,	/* TIMERS2_CLK_RST */
	0x48,	/* ONEWIRE_CLK_RST */	0x58,	/* DRO_CLK_RST */
	0x5C,	/* IR_CLK_RST */	0x60,	/* TWSI6_CLK_RST */
	0x68,	/* TWSI7_CLK_RST */	0x6C,	/* TSEN_CLK_RST */
	0x70,	/* UART4_CLK_RST */	0x74,	/* UART5_CLK_RST */
	0x78,	/* UART6_CLK_RST */	0x7C,	/* SSP3_CLK_RST */
	0x80,	/* SSPA0_CLK_RST */	0x84,	/* SSPA1_CLK_RST */
	0x90,	/* IPC_AP2AUD_CLK_RST */ 0x94,	/* UART7_CLK_RST */
	0x98,	/* UART8_CLK_RST */	0x9C,	/* UART9_CLK_RST */
	0xA0,	/* CAN0_CLK_RST */
	/* 0xA8..0xE4: PWM4..PWM19 (stride per SoC header) */
};

/* APBC2 offsets */
static uchar apbc2clkoffs[] = {
	0x00,	/* UART1_CLK_RST */	0x04,	/* SSP2_CLK_RST */
	0x08,	/* TWSI3_CLK_RST */	0x0C,	/* RTC_CLK_RST */
	0x10,	/* TIMERS0_CLK_RST */	0x14,	/* KPC_CLK_RST */
	0x1C,	/* GPIO_CLK_RST */
};

/*
 * RCPU "CLK_RST"-style offsets (bit0=deassert)
 * (Representative set used by the reset driver)
 */
static ushort rcpuclkoffs[] = {
	0x0028,	/* SSP0_CLK_RST */	0x0030,	/* I2C0_CLK_RST */
	0x003C,	/* UART1_CLK_RST */	0x0048,	/* CAN_CLK_RST */
	0x004C,	/* IR_CLK_RST */	0x00D8,	/* UART0_CLK_RST */
	0x2044,	/* AUDIO_HDMI_CLK_CTRL */
};

/* RCPU2 PWM lines (bit0=deassert, bit2=assert) */
static uchar rcpu2pwmoffs[] = {
	0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24,
};

static void
apbcdeass(uintptr base, ulong off)
{
	/* Enable clock, clear reset */
	*(ulong *)KADDR(base + off) |= Apbclken;
	*(ulong *)KADDR(base + off) &= ~Apbrst;
	coherence();
}

static void
rcpudeass(uintptr base, int off)
{
	*(ulong *)KADDR(base + off) |= Rcpudeass;
	/* If an assert bit exists at bit2, make sure it's clear */
	*(ulong *)KADDR(base + off) &= ~Rcpuass;
	coherence();
}

/* enable all clocks & deassert resets */
void
k1clksondeass(void)
{
	int i;

	for (i = 0; i < sizeof(apbcclkoffs) / sizeof apbcclkoffs[0]; i++)
		apbcdeass(Apbcbase, apbcclkoffs[i]);
	for (i = 0; i < sizeof(apbc2clkoffs) / sizeof apbc2clkoffs[0]; i++)
		apbcdeass(Apbc2base, apbc2clkoffs[i]);
	for (i = 0; i < sizeof(rcpuclkoffs) / sizeof rcpuclkoffs[0]; i++)
		rcpudeass(Rcpubase, rcpuclkoffs[i]);
	for (i = 0; i < sizeof(rcpu2pwmoffs) / sizeof rcpu2pwmoffs[0]; i++)
		rcpudeass(Rcpu2base, rcpu2pwmoffs[i]);
}

static void
unlockdog(Spacedog *dog)
{
	coherence();
	delay(10);
	dog->twfar = 0xbaba;
	coherence();
	delay(1);
	dog->twsar = 0xeb10;
	coherence();
	delay(1);
}

static void
emaccfg(ulong *regp)
{
	ulong cfg;

	/*
	 * this is quite delicate.  it works with exactly this config.
	 * adding Emacrgmiitxclk prevents all transmission on jupiter k1x.
	 * was 0x2007 from u-boot.
	 */
	cfg = Emacaxi1id | Emacaxiclken | Emacphyintfrgmii | Emactxclk125m;
	*regp = cfg;
	coherence();
	delay(100);
	*regp = cfg | Emacrstdeass;
	coherence();
}

static void
spacemitinit(void)
{
	ulong *emac0clkrstctl, *l2ctl;
	Spacedog *dog = (Spacedog *)soc.wdog0;

	k1clksondeass();
	if(dog) {
		unlockdog(dog);
		dog->twmer = 0;		/* clear We */

		l2ctl = (ulong *)KADDR(Wdogclks);
		*l2ctl = 0;		/* reset watchdog + clocks off */
		coherence();
	}
 if (0) {			/* apparently we can't touch this */
	l2ctl = (ulong *)KADDR(Dragonciu + 0x1b0);
	l2ctl[0] |= 5;		/* c0 flush hw enable, hw pmu flush mode */
	l2ctl[1] |= 5;		/* c1 flush hw enable, hw pmu flush mode */
	coherence();
 }
	// iprint("configuring emac clocks...");
	emac0clkrstctl = (ulong *)KADDR(Emac0clkrstctl);
	emaccfg(&emac0clkrstctl[0]);
	emaccfg(&emac0clkrstctl[2]);
}

/* MPMU offsets (clock/reset side) used by Linux CCU */
#define Mpmuposroff	0x0010	/* PLL lock status bits */
#define Mpmusuccroff	0x0014
#define Mpmuisccroff	0x0044
#define Mpmuwdtpcroff	0x0200		/* Watchdog reset pulse control */
#define Mpmuripccroff	0x0210
#define Mpmuacgroff	0x1024
#define Mpmuapbcscroff	0x1050
#define Mpmusuccr1off	0x10B0

/*
 * APBC register offsets (subset shown for reference)
 * These are *per-block* CLK_RST registers. Useful if you
 * want to pulse-reset specific APB peripherals.
 */
#define Apbcuart1clkrst	0x000
#define Apbcuart2clkrst	0x004
#define Apbcgpioclkrst	0x008
#define Apbctimers1clkrst	0x034
#define Apbctimers2clkrst	0x044
/* many more exist in the same sequence per k1-syscon.h */

/*
 * Force full-chip reset via MPMU WDTPCR
 * The WDTPCR register is a `pulse control' writing the kick pattern issues a
 * reset via the SoC's watchdog path.  The exact key/mask values are
 * SoC-defined; on K1 this register exists at MPMU_BASE+0x200.  If your platform
 * uses a key, set k1_Wdtkey accordingly.
 */
#define Wdtkey 0		/* set to required key if any (0 if none) */
#define Wdtwdtr (1 << 0)	/* reset pulse bit (common placement) */

/*
 * APBC / APBC2 peripheral soft resets
 * Many APB blocks expose a CLK_RST word in APBC/APBC2 windows where bit0=CLKEN,
 * bit1=RST, bit2=BUSY (pattern similar across Marvell-style CCUs).  If you need
 * to bounce a peripheral, you can do: write(CLKEN=1,RST=1), small delay, then
 * write(CLKEN=1,RST=0).
 */
static void
apbcpulse_reset(void *regint) 
{
	enum {
		CLK_EN	= 1 << 0,
		RST	= 1 << 1,
	};
	ulong *reg = (ulong *)regint;

	*reg |= CLK_EN | RST;	/* Enable clock and assert reset */
	coherence();
	delay(1);
	*reg &= ~RST;		/* Deassert reset, keep clock enabled */
	coherence();
	delay(1);
}

/*
 * This doesn't work and I still don't know why.
 */
static void
spacemitreset(void)
{
	int i, ocnt, cnt, junk;
	ulong *clkctl;
	Spacedog *dog = (Spacedog *)soc.wdog0;

	if(dog == nil)
		return;

	iprint("spacemitreset: setting watchdog %#p...", dog);
//	splhi();
	/* Wdogclken initial u-boot value: 0x2dffff, includes bits 3 & 19 */
	clkctl  = (ulong *)KADDR(Wdogclks);
	*clkctl = 7;		/* watchdog clocks, reset enabled */
	coherence();
	*(ulong *)KADDR(Mpmuaprr) |= 1<<4;	/* magic: wdt to reset */
	coherence();
	*(ulong *)KADDR(Wdogripcrr) = 5;	/* generate reset, clock on */
	coherence();
	*(ulong *)KADDR(Mpmuaprr) |= 1<<4;	/* magic: wdt to reset */
	coherence();

	iprint("timer...");
	unlockdog(dog);
	dog->twmer = We|Wrie;	/* start counting at 0; reset at match */

	unlockdog(dog);
	dog->twmr = 128;	/* with 256 Hz clock */

	unlockdog(dog);
	dog->twcrr = 1;		/* reset count */

	unlockdog(dog);
	dog->twmer = We|Wrie;	/* start counting at 0; reset at match */

	iprint("awaiting reset...");
	ocnt = -1;
	USED(ocnt);
	for (i = 4; i-- > 0; ) {
		unlockdog(dog);
		cnt = dog->twvr;
		coherence();

		iprint("%d ", cnt);
		if (cnt == ocnt) {
			iprint("count stuck\n");
			break;
		}
		delay(500);
		ocnt = cnt;
		USED(ocnt);
	}
//	unlockdog(dog);
//	dog->twmer = 0;		/* clear We */
//	*clkctl = 0;		/* watchdog reset + clocks off */

	iprint("using reboot reg...");
	*(ulong *)KADDR(Rebootreg) = 1;
	coherence();
	delay(1000);
	*(ulong *)KADDR(Rebootreg) = 3;
	coherence();
	delay(1000);
	*(ulong *)KADDR(Rebootreg) = 0xa5 << 24 | 1;
	coherence();
	delay(1000);

	/* Write `reset pulse' (some SoCs require key|WDTR, others WDTR). */
	iprint("pulsing wdt reset reg...");
	*(ulong *)KADDR(Mpmubase + Mpmuwdtpcroff) = Wdtkey | Wdtwdtr;
	coherence();
	delay(1000);

	iprint("pulsing uart reset reg...");
	apbcpulse_reset(KADDR(Mpmubase + Mpmuwdtpcroff));
	delay(1000);

	iprint("using reset reg...");
	*(ulong *)KADDR(Resetreg) |= 1<<2;
	coherence();
	junk = *(ulong *)KADDR(Resetreg);
	USED(junk);
	delay(2000);

#ifdef hang_the_system
//	*(ulong *)KADDR(Pmuaprr)  |= 1<<4;	/* magic: wdt to reset */

	iprint("deref bad addr...");
	*(ulong *)(0xffULL<<56) = 0;	/* bad address */
	delay(1);

	iprint("using uart clock...");
	apbcpulse_reset(KADDR(Apbcbase + Apbcuart1clkrst));
	delay(1000);
#endif

	iprint("failed utterly to reset\n");
}

void (*socinit)(void) = spacemitinit;
void (*rvreset)(void) = spacemitreset;
