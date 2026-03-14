/*
 * eic7700_pcie_init.c - Bare-metal bring-up for ESWIN EIC7700X PCIe (HiFive
 * Premier P550)
 *
 * This SoC integrates a Synopsys DesignWare PCIe Gen3 x4 Root Complex, plus an
 * "ELBI" (Eswin Link Bus Interface) register block used for resets, LTSSM
 * enable, etc.
 *
 * References (for register offsets/bitfields and default address layout): -
 * Linux patch series adding the EIC7700 PCIe host controller driver (v8, Dec
 * 2025).
 *
 * This file is intentionally self-contained (no libc required) except for: - a
 * microsecond delay hook (pcie_delay_us) - optional platform hooks to control
 * PERST# and reset/clock gates
 *
 * Adjust base addresses and any platform hooks to match your firmware
 * environment.
 */

#include <stdint.h>
#include <stddef.h>

/* --- Platform configuration --- */
/*
 * Device-tree examples for EIC7700 show:
 *   dbi    @ 0x5400_0000 size 0x0400_0000
 *   config @ 0x4000_0000 size 0x0080_0000
 *   elbi   @ 0x5000_0000 size 0x0010_0000
 *
 * If your firmware uses different mappings, change these.
 */
#ifndef EIC7700_PCIE_DBI_BASE
#define EIC7700_PCIE_DBI_BASE   0x54000000UL
#endif
#ifndef EIC7700_PCIE_CFG_BASE
#define EIC7700_PCIE_CFG_BASE   0x40000000UL
#endif
#ifndef EIC7700_PCIE_ELBI_BASE
#define EIC7700_PCIE_ELBI_BASE  0x50000000UL
#endif

#ifndef EIC7700_PCIE_CFG_SIZE
#define EIC7700_PCIE_CFG_SIZE   0x00800000UL
#endif

/* Default offset from DBI to iATU registers on many DWC cores. */
#define DEFAULT_DBI_ATU_OFFSET  (0x3UL << 20) /* 0x0030_0000 */

static void
mmio_w32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static uint32_t
mmio_r32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

/* Provide this in your platform (busy-loop, timer, etc). */
void
pcie_delay_us(unsigned us)
{
	/* crude fallback: tune for your clock if you don't override */
	volatile unsigned i;

	for (i = 0; i < us * 100; i++)
		__asm__ __volatile__("" ::: "memory");
}

/* Optional: assert/deassert PERST# to downstream device(s). */
void
pcie_set_perst(int asserted)
{
	(void)asserted;
	/* Implement with a GPIO/reset line if you have one. */
}

/* Optional: SoC-level reset/clock enables for the PCIe controller. */
int
pcie_soc_enable_clocks_and_resets(void)
{
	/* Implement if your firmware must explicitly ungate clocks or deassert resets. */
	return 0;
}

/* --- ELBI registers --- */
/* Offsets */
#define PCIEELBI_CTRL0_OFFSET       0x000
#define PCIEELBI_STATUS0_OFFSET     0x100

/* CTRL0 bitfields */
#define PCIEELBI_CTRL0_DEV_TYPE_MASK    0x0000000FUL /* [3:0] */
#define PCIEELBI_CTRL0_APP_LTSSM_ENABLE (1U << 5)
#define PCIEELBI_CTRL0_APP_HOLD_PHY_RST (1U << 6)
#define PCIEELBI_CTRL0_PM_SEL_AUX_CLK   (1U << 16)

/* STATUS0 bitfields */
#define PCIEELBI_STATUS0_PM_SEL_AUX_CLK (1U << 16)

/* PCIe "device/port type" values (PCIe spec: PCI_EXP_TYPE_*) */
#define PCI_EXP_TYPE_ROOT_PORT      0x4

/* --- DesignWare DBI regs --- */
/* Standard PCI config header offsets (type 1 header for Root Port) */
#define PCI_VENDOR_ID               0x00
#define PCI_DEVICE_ID               0x02
#define PCI_COMMAND                 0x04
#define PCI_CLASS_REVISION          0x08
#define PCI_PRIMARY_BUS             0x18
#define PCI_BASE_ADDRESS_0          0x10
#define PCI_BASE_ADDRESS_1          0x14

#define PCI_COMMAND_IO              0x1
#define PCI_COMMAND_MEMORY          0x2
#define PCI_COMMAND_MASTER          0x4
#define PCI_COMMAND_SERR            0x100

/* Root Port class code: 0x0604xx (PCI-to-PCI bridge) */
#define PCI_CLASS_BRIDGE_PCI        0x0604

/* DBI "read-only write enable" (DesignWare) */
#define PCIE_MISC_CONTROL_1_OFF     0x8BC
#define PCIE_DBI_RO_WR_EN           (1U << 0)

/* PCI Express Capability / Link Status */
#define PCI_CAPABILITY_LIST         0x34
#define PCI_CAP_ID_EXP              0x10
#define PCI_EXP_LNKSTA              0x12
#define PCI_EXP_LNKSTA_DLLLA        (1U << 13)

/* --- iATU (unrolled) regs --- */
/*
 * For DWC v4.80a+ the iATU registers are "unrolled". Each region has a 0x200
 * block, and inbound/outbound are distinguished by bit 8 of the region offset.
 */
#define PCIE_ATU_REGION_DIR_IB      (1UL << 31) /* use in helper only */
#define PCIE_ATU_REGION_DIR_OB      0

#define PCIE_ATU_REGION_CTRL1       0x000
#define PCIE_ATU_REGION_CTRL2       0x004
#define PCIE_ATU_LOWER_BASE         0x008
#define PCIE_ATU_UPPER_BASE         0x00C
#define PCIE_ATU_LIMIT              0x010
#define PCIE_ATU_LOWER_TARGET       0x014
#define PCIE_ATU_UPPER_TARGET       0x018

#define PCIE_ATU_ENABLE             (1U << 31)

#define PCIE_ATU_TYPE_MEM           0x0
#define PCIE_ATU_TYPE_IO            0x2
#define PCIE_ATU_TYPE_CFG0          0x4
#define PCIE_ATU_TYPE_CFG1          0x5

static uintptr_t
atu_region_base(uintptr_t atu_base, uint32_t dir, unsigned index)
{
	/* From Linux pcie-designware.h: ((index << 9) | (dir==IB ? BIT(8):0)) */
	uintptr_t off = ((uintptr_t)index << 9) |
		(dir == PCIE_ATU_REGION_DIR_IB? 1UL<<8: 0);

	return atu_base + off;
}

/* Encodes {bus,device,function} into DesignWare's "busdev" field. */
static uint32_t
atu_busdev(unsigned bus, unsigned dev, unsigned fn)
{
	return ((bus & 0xff) << 24) | ((dev & 0x1f) << 19) | ((fn & 0x7) << 16);
}

/* --- Core context --- */
typedef struct {
	uintptr_t dbi_base;
	uintptr_t cfg_base;
	uintptr_t elbi_base;
	uintptr_t atu_base;   /* usually dbi_base + DEFAULT_DBI_ATU_OFFSET */

	/* Which iATU outbound region to use for config transactions */
	unsigned cfg_atu_region;
} eic7700_pcie_t;

/* --- Helper routines --- */
static void
dbi_ro_write_enable(const eic7700_pcie_t *p, int enable)
{
	uint32_t v = mmio_r32(p->dbi_base + PCIE_MISC_CONTROL_1_OFF);
	if (enable)
		v |= PCIE_DBI_RO_WR_EN;
	else
		v &= ~PCIE_DBI_RO_WR_EN;
	mmio_w32(p->dbi_base + PCIE_MISC_CONTROL_1_OFF, v);
}

static int
pcie_find_pcie_cap(const eic7700_pcie_t *p, uint16_t *cap_off)
{
	uint8_t ptr = (uint8_t)mmio_r32(p->dbi_base + PCI_CAPABILITY_LIST);

	/* capabilities are a linked list in config space */
	while (ptr >= 0x40 && (ptr & 0x3) == 0) {
		uint32_t hdr = mmio_r32(p->dbi_base + ptr);
		uint8_t cap_id = (uint8_t)(hdr & 0xff);
		uint8_t next   = (uint8_t)((hdr >> 8) & 0xff);

		if (cap_id == PCI_CAP_ID_EXP) {
			*cap_off = ptr;
			return 0;
		}
		ptr = next;
	}
	return -1;
}

static int
pcie_wait_link_up(const eic7700_pcie_t *p, unsigned timeout_ms)
{
	uint16_t cap;
	unsigned i;

	if (pcie_find_pcie_cap(p, &cap) != 0)
		return -1;

	for (i = 0; i < timeout_ms; i++) {
		uint16_t lnksta = (uint16_t)(mmio_r32(p->dbi_base +
			cap + PCI_EXP_LNKSTA) & 0xffff);

		if (lnksta & PCI_EXP_LNKSTA_DLLLA)
			return 0;
		pcie_delay_us(1000);
	}
	return -1;
}

/* Program one outbound iATU region (unrolled) */
static void
atu_prog_outbound(eic7700_pcie_t *p, unsigned region, uint32_t type,
	uint64_t cpu_base, uint64_t pci_target, uint64_t size)
{
	uintptr_t r = atu_region_base(p->atu_base, PCIE_ATU_REGION_DIR_OB,
		 region);

	/* Disable region before reprogramming (optional but common) */
	mmio_w32(r + PCIE_ATU_REGION_CTRL2, 0);

	mmio_w32(r + PCIE_ATU_LOWER_BASE,  (uint32_t)cpu_base);
	mmio_w32(r + PCIE_ATU_UPPER_BASE,  (uint32_t)(cpu_base >> 32));
	mmio_w32(r + PCIE_ATU_LIMIT,       (uint32_t)(cpu_base + size - 1));
	mmio_w32(r + PCIE_ATU_LOWER_TARGET, (uint32_t)pci_target);
	mmio_w32(r + PCIE_ATU_UPPER_TARGET, (uint32_t)(pci_target >> 32));

	mmio_w32(r + PCIE_ATU_REGION_CTRL1, type);
	mmio_w32(r + PCIE_ATU_REGION_CTRL2, PCIE_ATU_ENABLE);
}

/* Map a single BDF's config space into the config aperture and return pointer */
static uint32_t *
pcie_map_cfg(eic7700_pcie_t *p, unsigned bus, unsigned dev, unsigned fn,
	unsigned where)
{
	uint32_t type = bus == 1? PCIE_ATU_TYPE_CFG0: PCIE_ATU_TYPE_CFG1;
	uint32_t busdev = atu_busdev(bus, dev, fn);

	/*
	 * The DWC host driver programs iATU target with bus/dev/fn in the upper
	 * bits, and then uses the CPU-side "config" aperture for MMIO accesses.
	 * We reprogram the region on each access for simplicity.
	 */
	atu_prog_outbound(p, p->cfg_atu_region, type, (uint64_t)p->cfg_base,
		 (uint64_t)busdev, (uint64_t)EIC7700_PCIE_CFG_SIZE);

	return (uint32_t *)(p->cfg_base + (where & ~3U));
}

static uint32_t
pcie_cfg_read32(const eic7700_pcie_t *p, unsigned bus, unsigned dev,
	unsigned fn, unsigned where)
{
	uint32_t *addr = pcie_map_cfg(p, bus, dev, fn, where);

	return addr[0];
}

/* --- Public init API --- */
/*
 * Minimal bring-up:
 *  - ungate clocks / deassert SoC resets (platform hook)
 *  - configure controller as Root Port
 *  - toggle PERST#
 *  - release PHY reset hold, wait ~20ms for PHY ready
 *  - patch vendor/device IDs into RC config header (optional)
 *  - enable LTSSM and wait for link up
 *  - setup RC config header (bus numbers, command bits)
 *
 * Returns 0 on success, <0 on failure.
 */
int
eic7700_pcie_init(eic7700_pcie_t *p)
{
	uint32_t v;
	unsigned t;

	/* Default context initialization */
	if (!p->dbi_base)
		p->dbi_base  = (uintptr_t)EIC7700_PCIE_DBI_BASE;
	if (!p->cfg_base)
		p->cfg_base  = (uintptr_t)EIC7700_PCIE_CFG_BASE;
	if (!p->elbi_base)
		p->elbi_base = (uintptr_t)EIC7700_PCIE_ELBI_BASE;
	if (!p->atu_base)
		p->atu_base  = p->dbi_base + DEFAULT_DBI_ATU_OFFSET;
	if (!p->cfg_atu_region)
		p->cfg_atu_region = 1;

	if (pcie_soc_enable_clocks_and_resets() != 0)
		return -1;

	/* Configure Root Port type in ELBI CTRL0 */
	v = mmio_r32(p->elbi_base + PCIEELBI_CTRL0_OFFSET);
	v &= ~PCIEELBI_CTRL0_DEV_TYPE_MASK;
	v |= (PCI_EXP_TYPE_ROOT_PORT & PCIEELBI_CTRL0_DEV_TYPE_MASK);
	mmio_w32(p->elbi_base + PCIEELBI_CTRL0_OFFSET, v);

	/* PERST# assert >= 100ms, then deassert (Linux driver uses 100ms) */
	pcie_set_perst(1);
	pcie_delay_us(100 * 1000);
	pcie_set_perst(0);

	/*
	 * Hold PHY reset bit: clear it to let PHY come up.  Then poll STATUS0
	 * bit 16 to confirm PM_SEL_AUX_CLK deasserted (PHY ready).
	 */
	v = mmio_r32(p->elbi_base + PCIEELBI_CTRL0_OFFSET);
	v &= ~PCIEELBI_CTRL0_APP_HOLD_PHY_RST;
	mmio_w32(p->elbi_base + PCIEELBI_CTRL0_OFFSET, v);

	for (t = 0; t < 20; t++) {
		uint32_t s = mmio_r32(p->elbi_base + PCIEELBI_STATUS0_OFFSET);

		if ((s & PCIEELBI_STATUS0_PM_SEL_AUX_CLK) == 0)
			break;
		pcie_delay_us(1000);
	}
	if (t == 20)
		return -2;

	/* Optional: write Vendor/Device IDs into the RC config header (needs RO write enable). */
	dbi_ro_write_enable(p, 1);
	mmio_w32(p->dbi_base + PCI_VENDOR_ID, 0x2030U << 16 | 0x1fe1U); /* device:vendor packed */
	dbi_ro_write_enable(p, 0);

	/* Setup minimal RC config header fields (similar to dw_pcie_setup_rc()) */
	dbi_ro_write_enable(p, 1);

	/* RC BARs (type 1 header): BAR0=32-bit mem, BAR1=0 */
	mmio_w32(p->dbi_base + PCI_BASE_ADDRESS_0, 4);
	mmio_w32(p->dbi_base + PCI_BASE_ADDRESS_1, 0);

	/* Bus numbers: primary=0, secondary=1, subordinate=0xff */
	v = mmio_r32(p->dbi_base + PCI_PRIMARY_BUS);
	v &= 0xff000000U;
	v |= 0x00ff0100U;
	mmio_w32(p->dbi_base + PCI_PRIMARY_BUS, v);

	/* Command: enable IO/MEM/BUSMASTER/SERR */
	v = mmio_r32(p->dbi_base + PCI_COMMAND);
	v &= 0xffff0000U;
	v |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		 PCI_COMMAND_SERR;
	mmio_w32(p->dbi_base + PCI_COMMAND, v);

	/* Class code (upper 24 bits at 0x08): 0x0604xx */
	v = mmio_r32(p->dbi_base + PCI_CLASS_REVISION);
	v &= 0x000000ff;
	v |= PCI_CLASS_BRIDGE_PCI << 8;
	mmio_w32(p->dbi_base + PCI_CLASS_REVISION, v);

	dbi_ro_write_enable(p, 0);

	/* Enable LTSSM */
	v = mmio_r32(p->elbi_base + PCIEELBI_CTRL0_OFFSET);
	v |= PCIEELBI_CTRL0_APP_LTSSM_ENABLE;
	mmio_w32(p->elbi_base + PCIEELBI_CTRL0_OFFSET, v);

	/* Wait for Data Link Layer Link Active */
	if (pcie_wait_link_up(p, 200) != 0)
		return -3;

	return 0;
}
