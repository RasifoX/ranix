/* Intel TCO watchdog, written from the public Intel datasheets (ICH9, 100 Series
 * PCH) and from register dumps of real hardware. See docs/watchdog_pstore.md.
 *
 * The TCO timer counts 0.6 s ticks from TCO_TMR down to 0. The first expiry sets
 * TIMEOUT and reloads; the second, with no TCO_RLD write in between, resets the
 * platform unless NO_REBOOT is set. So one full countdown is 2 * TCO_TMR ticks.
 *
 * Two chipset families are handled. They differ only in where the registers are:
 *
 *                  TCO I/O base             NO_REBOOT
 *   ICH9 (q35)     LPC PMBASE + 0x60        RCBA + 0x3410 (GCS) bit 5
 *   Sunrise Point  SMBus cfg 0x50 TCOBASE   SBREG + 0xC6000C (SMBus GC) bit 1
 *
 * Sunrise Point's NO_REBOOT is not in the PMC. It sits in the SMBus private
 * config space, reached through the P2SB sideband window, and firmware hides the
 * P2SB device. Later PCHs moved the bit again, so unknown device IDs are refused
 * rather than guessed at: a wrong write here lands in chipset private space.
 */

#include <uapi/types.h>
#include <kernel/pci.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <asm/io.h>
#include "itco.h"

#define INTEL_VID       0x8086

/* TCO registers, offsets from the TCO I/O base */
#define TCO_RLD         0x00   /* read: ticks left; write any value: reload */
#define TCO1_STS        0x04
#define TCO2_STS        0x06
#define TCO1_CNT        0x08
#define TCO_TMR         0x12

#define TCO1_STS_TIMEOUT   (1u << 3)    /* write 1 to clear */
#define TCO2_STS_SECOND_TO (1u << 1)    /* write 1 to clear; survives the reset */
#define TCO1_CNT_NMI_NOW   (1u << 8)    /* never write back: a 1 raises an NMI */
#define TCO1_CNT_TMR_HLT   (1u << 11)
#define TCO1_CNT_LOCK      (1u << 12)   /* locks SMI_EN.TCO_EN until platform reset */
#define TCO_TMR_MASK       0x03FFu

#define SMI_EN_OFF      0x30           /* from the ACPI/PM I/O base */
#define SMI_EN_TCO_EN   (1u << 13)
#define SMI_EN_GBL_EN   (1u << 0)

/* Device 31 functions, identical on both families */
#define PCH_DEV         0x1F
#define LPC_FN          0
#define P2SB_FN         1
#define PMC_FN          2
#define SMBUS_FN        4

/* SMBus (Sunrise Point) */
#define SMB_TCOBASE     0x50
#define SMB_TCOCTL      0x54
#define SMB_TCO_BASE_EN (1u << 8)
#define SMB_TCOBASE_IOS (1u << 0)
#define SMB_TCOBASE_MASK 0xFFE0u

/* PMC (Sunrise Point) */
#define PMC_ACPIBASE    0x40
#define PMC_ACPIBASE_MASK 0xFF00u

/* P2SB (Sunrise Point) */
#define P2SB_SBREG_LO   0x10
#define P2SB_SBREG_HI   0x14
#define P2SB_CTRL_HIDE  0xE1           /* byte; bit 0 of it is the HIDE bit (P2SBC[8]) */
#define SBREG_SMB_GC    0xC6000CUL     /* port 0xC6, register 0x0C */
#define SMB_GC_NO_REBOOT (1u << 1)

/* LPC (ICH9) */
#define LPC_PMBASE      0x40
#define LPC_PMBASE_MASK 0xFF80u
#define LPC_ACPI_CTRL   0x44
#define LPC_ACPI_EN     (1u << 7)
#define LPC_RCBA        0xF0
#define LPC_RCBA_EN     (1u << 0)
#define LPC_RCBA_MASK   0xFFFFC000u
#define ICH9_TCO_OFF    0x60
#define RCBA_GCS        0x3410u
#define GCS_NO_REBOOT   (1u << 5)

static uint16_t tco_base;
static uint16_t pm_base;     /* 0 = unknown; used only to report the SMI routing */
static int      tco_fired;

static uint16_t tco_r16(uint16_t off)             { return inw((uint16_t)(tco_base + off)); }
static void     tco_w16(uint16_t off, uint16_t v) { outw((uint16_t)(tco_base + off), v); }

/* Clear a NO_REBOOT bit in a 32-bit MMIO register and read it back.
 * Returns 0 if the bit is clear, -1 if it will not clear (a board strap). */
static int noreboot_clear(uint64_t phys, uint32_t bit)
{
	volatile uint32_t *reg = ioremap(phys, 4, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!reg) return -1;
	uint32_t v = mmio_read32(reg);
	if (v == 0xFFFFFFFFu) return -1;   /* nothing decoded there */
	if (v & bit) {
		mmio_write32(reg, v & ~bit);
		v = mmio_read32(reg);
	}
	return (v & bit) ? -1 : 0;
}

/* Read the sideband window base out of the hidden P2SB. A hidden device reads as
 * all ones but still takes config writes, so HIDE must be flipped with a byte
 * write: a dword read-modify-write would store ones over its neighbours. */
static uint64_t p2sb_sbreg(void)
{
	pci_write8(0, PCH_DEV, P2SB_FN, P2SB_CTRL_HIDE, 0);
	uint32_t id = pci_read32(0, PCH_DEV, P2SB_FN, PCI_VENDOR_ID);
	uint32_t lo = pci_read32(0, PCH_DEV, P2SB_FN, P2SB_SBREG_LO);
	uint32_t hi = pci_read32(0, PCH_DEV, P2SB_FN, P2SB_SBREG_HI);
	pci_write8(0, PCH_DEV, P2SB_FN, P2SB_CTRL_HIDE, 1);

	if ((id & 0xFFFF) != INTEL_VID || lo == 0xFFFFFFFFu) return 0;
	return ((uint64_t)hi << 32) | (lo & ~0xFu);
}

/* Sunrise Point family: TCOBASE lives in the SMBus function.
 * Returns 0 found and resettable, -1 no such hardware, -2 cannot reset. */
static int probe_spt(uint16_t did)
{
	if (did != 0x9D23 &&    /* Sunrise Point-LP, verified on a real i5-7200U board */
	    did != 0xA123 &&    /* Sunrise Point-H */
	    did != 0xA2A3)      /* Union Point-H */
		return -1;

	uint32_t base = pci_read32(0, PCH_DEV, SMBUS_FN, SMB_TCOBASE);
	uint32_t ctl  = pci_read32(0, PCH_DEV, SMBUS_FN, SMB_TCOCTL);
	if (!(base & SMB_TCOBASE_IOS) || !(ctl & SMB_TCO_BASE_EN) ||
	    !(base & SMB_TCOBASE_MASK)) {
		kprintf(KERN_DEBUG "wdog: smbus tco decode off (tcobase=0x%x tcoctl=0x%x)\n", base, ctl);
		return -1;
	}
	tco_base = (uint16_t)(base & SMB_TCOBASE_MASK);

	uint32_t pmc = pci_read32(0, PCH_DEV, PMC_FN, PCI_VENDOR_ID);
	if ((pmc & 0xFFFF) == INTEL_VID)
		pm_base = (uint16_t)(pci_read32(0, PCH_DEV, PMC_FN, PMC_ACPIBASE) & PMC_ACPIBASE_MASK);

	uint64_t sbreg = p2sb_sbreg();
	if (!sbreg) {
		/* Arm anyway: harmless if the bit is already clear, and the panic path
		 * has software resets. The log line says what is not known. */
		kprintf(KERN_WARN "wdog: p2sb did not unhide, no_reboot state unknown\n");
		return 0;
	}
	if (noreboot_clear(sbreg + SBREG_SMB_GC, SMB_GC_NO_REBOOT) != 0) {
		kprintf(KERN_WARN "wdog: no_reboot is stuck set (sbreg=0x%lx)\n", sbreg);
		return -2;
	}
	return 0;
}

/* ICH9: q35 in QEMU. TCO sits inside the ACPI PM block. */
static int probe_ich9(uint16_t did)
{
	if (did < 0x2910 || did > 0x291F)
		return -1;

	uint32_t pmb  = pci_read32(0, PCH_DEV, LPC_FN, LPC_PMBASE);
	uint32_t ctl  = pci_read32(0, PCH_DEV, LPC_FN, LPC_ACPI_CTRL);
	uint32_t rcba = pci_read32(0, PCH_DEV, LPC_FN, LPC_RCBA);
	if (!(ctl & LPC_ACPI_EN) || !(pmb & LPC_PMBASE_MASK)) {
		kprintf(KERN_WARN "wdog: ich9 acpi block disabled (pmbase=0x%x)\n", pmb);
		return -1;
	}
	pm_base  = (uint16_t)(pmb & LPC_PMBASE_MASK);
	tco_base = (uint16_t)(pm_base + ICH9_TCO_OFF);

	if (!(rcba & LPC_RCBA_EN)) {
		kprintf(KERN_WARN "wdog: rcba disabled, no_reboot state unknown\n");
		return 0;
	}
	if (noreboot_clear((uint64_t)(rcba & LPC_RCBA_MASK) + RCBA_GCS, GCS_NO_REBOOT) != 0) {
		kprintf(KERN_WARN "wdog: no_reboot is stuck set\n");
		return -2;
	}
	return 0;
}

int itco_probe(void)
{
	uint32_t lpc = pci_read32(0, PCH_DEV, LPC_FN, PCI_VENDOR_ID);
	uint32_t smb = pci_read32(0, PCH_DEV, SMBUS_FN, PCI_VENDOR_ID);
	int rc = -1;

	if ((smb & 0xFFFF) == INTEL_VID)
		rc = probe_spt((uint16_t)(smb >> 16));
	if (rc == -1 && (lpc & 0xFFFF) == INTEL_VID)
		rc = probe_ich9((uint16_t)(lpc >> 16));
	if (rc == -1) {
		kprintf(KERN_INFO "wdog: no supported intel tco (lpc id 0x%x, smbus id 0x%x)\n", lpc, smb);
		tco_base = 0;
		return -1;
	}
	if (rc != 0) {
		tco_base = 0;
		return rc;
	}

	/* The port must actually decode: an unclaimed read floats to all ones. */
	uint16_t cnt = tco_r16(TCO1_CNT);
	uint16_t tmr = tco_r16(TCO_TMR);
	if (cnt == 0xFFFF || tmr == 0xFFFF) {
		kprintf(KERN_WARN "wdog: tco block at 0x%x does not decode\n", (unsigned)tco_base);
		tco_base = 0;
		return -1;
	}

	tco_fired = (tco_r16(TCO2_STS) & TCO2_STS_SECOND_TO) != 0;
	return 0;
}

int itco_fired_last_boot(void) { return tco_fired; }

/* NMI_NOW is excluded from every read-modify-write of TCO1_CNT. */
static uint16_t tco1_cnt_rmw(void)
{
	return (uint16_t)(tco_r16(TCO1_CNT) & ~TCO1_CNT_NMI_NOW);
}

static void tco_clear_status(void)
{
	tco_w16(TCO1_STS, TCO1_STS_TIMEOUT);
	tco_w16(TCO2_STS, TCO2_STS_SECOND_TO);
}

static void tco_load_ticks(unsigned ticks)
{
	if (ticks < ITCO_TICKS_MIN) ticks = ITCO_TICKS_MIN;
	if (ticks > ITCO_TICKS_MAX) ticks = ITCO_TICKS_MAX;
	tco_w16(TCO_TMR, (uint16_t)((tco_r16(TCO_TMR) & ~TCO_TMR_MASK) | ticks));
}

int itco_start(unsigned ticks)
{
	if (!tco_base) return -1;

	uint16_t cnt = tco1_cnt_rmw();
	tco_w16(TCO1_CNT, (uint16_t)(cnt | TCO1_CNT_TMR_HLT));
	tco_clear_status();
	tco_load_ticks(ticks);
	tco_w16(TCO_RLD, 1);
	tco_w16(TCO1_CNT, (uint16_t)(cnt & ~TCO1_CNT_TMR_HLT));

	if (tco_r16(TCO1_CNT) & TCO1_CNT_TMR_HLT) {
		kprintf(KERN_WARN "wdog: tco timer will not start\n");
		return -1;
	}

	/* On a board that routes the first expiry to SMM the firmware handler runs
	 * before the second stage. Said out loud because only a real timeout shows
	 * what that handler does; see the document for this hardware's values. */
	if (pm_base) {
		uint32_t smi = inl((uint16_t)(pm_base + SMI_EN_OFF));
		if ((smi & SMI_EN_TCO_EN) && (smi & SMI_EN_GBL_EN))
			kprintf(KERN_WARN "wdog: first expiry goes to smm (smi_en=0x%x, %s)\n", smi,
			        (cnt & TCO1_CNT_LOCK) ? "locked" : "unlocked");
	}
	return 0;
}

void itco_pet(void)
{
	if (!tco_base) return;
	tco_w16(TCO_RLD, 1);
	if (tco_r16(TCO1_STS) & TCO1_STS_TIMEOUT)
		tco_w16(TCO1_STS, TCO1_STS_TIMEOUT);
}

void itco_set_ticks(unsigned ticks)
{
	if (!tco_base) return;
	tco_clear_status();
	tco_load_ticks(ticks);
	tco_w16(TCO_RLD, 1);
}

uint16_t itco_counter(void)
{
	return tco_base ? (uint16_t)(tco_r16(TCO_RLD) & TCO_TMR_MASK) : 0;
}
