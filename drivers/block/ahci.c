/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/sync.h>
#include <kernel/pci.h>
#include <kernel/block.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/hwdiag.h>
#include <asm/mmu.h>
#include <kernel/init.h>

/*
 * Minimal AHCI (SATA) block driver — polling, one SATA disk. Registers as a
 * block_device_t ("sata0") so GPT + ext4 sit on top unchanged. This is the
 * bridge to persistent storage on real PCs (the SSD behind an i5's SATA port);
 * QEMU's ich9-ahci exercises the same code path.
 *
 * DMA goes through a physically-contiguous bounce buffer, so callers' buffers
 * need not be contiguous or DMA-aligned.
 */

#define GHC_AE   (1u << 31)
#define PxCMD_ST  (1u << 0)
#define PxCMD_FRE (1u << 4)
#define PxCMD_FR  (1u << 14)
#define PxCMD_CR  (1u << 15)

#define ATA_IDENTIFY   0xEC
#define ATA_READ_DMA   0x25   /* READ DMA EXT  (48-bit LBA) */
#define ATA_WRITE_DMA  0x35   /* WRITE DMA EXT */
#define ATA_FLUSH_EXT  0xEA   /* FLUSH CACHE EXT */
#define ATA_SMART      0xB0   /* ATA8-ACS SMART, sub-command in FEATURE */
#define SMART_READ_DATA 0xD0
#define SMART_STATUS    0xDA
#define SMART_LBA       0xC24F00ull   /* LBA mid 0x4F, high 0xC2: the SMART key */

/* PxSERR.DIAG bits that mean a damaged frame on the SATA link (AHCI 1.3.1, 3.3.12):
 * B 10b to 8b decode, D disparity, C CRC, H handshake. */
#define SERR_LINK_BAD  ((1u << 19) | (1u << 20) | (1u << 21) | (1u << 22))

#define AHCI_POLL_MAX 10000000UL
#define BOUNCE_ORDER  5       /* 128 KB = 256 sectors per transfer */
#define BOUNCE_BYTES  (1u << (12 + BOUNCE_ORDER))

typedef struct {
	volatile uint32_t *abar;   /* HBA MMIO (ioremap'd) */
	int       port;
	uint64_t  clb_phys;        /* command list (1 KB) */
	uint64_t  fb_phys;         /* received FIS (256 B) */
	uint64_t  ctab_phys;       /* command table */
	uint8_t  *ctab;            /* command table (virt) */
	uint64_t  bounce_phys;
	uint8_t  *bounce;

	/* /proc/hwdiag. The driver never retries: every failure is counted here
	 * and returned to the caller. */
	uint64_t  n_err;           /* task file error */
	uint64_t  n_timeout;       /* command did not complete */
	uint64_t  n_busy;          /* device stayed busy before issue */
	uint64_t  n_serr;          /* commands that found link errors in PxSERR */
	uint32_t  serr_bits;       /* every PxSERR bit seen since boot */
	int       smart_ok;        /* IDENTIFY: SMART supported and enabled */
	char      model[41];
} ahci_t;

static ahci_t g_ahci;

static volatile uint32_t *port_reg(ahci_t *a)
{
	return (volatile uint32_t *)((uint8_t *)a->abar + 0x100 + a->port * 0x80);
}

/* Issue one ATA command on slot 0 and poll for completion. cmd_is_write picks
 * the PRDT direction; lba/count in sectors; data already in/out of the bounce. */
static int ahci_cmd_f(ahci_t *a, uint8_t cmd, uint8_t feature, uint8_t devbits,
                      uint64_t lba, uint32_t count, int write)
{
	volatile uint32_t *p = port_reg(a);

	p[0x10 / 4] = (uint32_t)-1;   /* PxIS: clear */
	uint32_t serr = p[0x30 / 4];
	if (serr) {
		a->serr_bits |= serr;
		if (serr & SERR_LINK_BAD) a->n_serr++;
	}
	p[0x30 / 4] = (uint32_t)-1;   /* PxSERR: clear */

	/* command header[0] */
	uint32_t *hdr = (uint32_t *)PHYS_TO_VIRT(a->clb_phys);
	hdr[0] = (5) | (write ? (1u << 6) : 0) | ((count ? 1u : 0u) << 16);   /* CFL=5 dwords, W, PRDTL */
	hdr[1] = 0;                                            /* PRDByteCount */
	hdr[2] = (uint32_t)a->ctab_phys;
	hdr[3] = (uint32_t)(a->ctab_phys >> 32);

	memset(a->ctab, 0, 128 + 16);
	uint8_t *fis = a->ctab;                /* command FIS (H2D) */
	fis[0] = 0x27;                         /* FIS type: register H2D */
	fis[1] = 0x80;                         /* C=1 (command) */
	fis[2] = cmd;
	fis[3] = feature;
	fis[4] = lba & 0xFF;
	fis[5] = (lba >> 8) & 0xFF;
	fis[6] = (lba >> 16) & 0xFF;
	fis[7] = devbits;                      /* 0x40: LBA mode */
	fis[8] = (lba >> 24) & 0xFF;
	fis[9] = (lba >> 32) & 0xFF;
	fis[10] = (lba >> 40) & 0xFF;
	fis[12] = count & 0xFF;
	fis[13] = (count >> 8) & 0xFF;

	/* PRDT entry at ctab + 0x80 */
	uint32_t *prdt = (uint32_t *)(a->ctab + 0x80);
	prdt[0] = (uint32_t)a->bounce_phys;
	prdt[1] = (uint32_t)(a->bounce_phys >> 32);
	prdt[2] = 0;
	if (count) prdt[3] = (count * 512u) - 1;   /* byte count - 1 */

	/* wait until not busy, then issue slot 0. The bounds are in register reads,
	 * about a microsecond each on real hardware, so roughly ten seconds. */
	uint64_t g = 0;
	while (p[0x20 / 4] & 0x88) {                       /* PxTFD BSY|DRQ */
		if (++g > AHCI_POLL_MAX) { a->n_busy++; kprintf(KERN_ERR "ahci: busy lba=%lu\n", (unsigned long)lba); return -1; }
	}
	p[0x10 / 4] = p[0x10 / 4];              /* PxIS is write-1-to-clear: drop stale errors */
	p[0x38 / 4] = 1;                        /* PxCI: slot 0 */

	g = 0;
	while (p[0x38 / 4] & 1) {
		if (p[0x10 / 4] & (1u << 30)) { a->n_err++; return -1; }   /* PxIS TFES: task file error */
		if (++g > AHCI_POLL_MAX) { a->n_timeout++; kprintf(KERN_ERR "ahci: timeout lba=%lu\n", (unsigned long)lba); return -1; }
	}
	if (p[0x20 / 4] & 0x01) { a->n_err++; return -1; }      /* PxTFD ERR */
	return 0;
}

static int ahci_cmd(ahci_t *a, uint8_t cmd, uint64_t lba, uint32_t count, int write)
{
	return ahci_cmd_f(a, cmd, 0, 0x40, lba, count, write);
}

/* One command slot and one bounce buffer: only one transfer at a time. */
static spinlock_t ahci_lock = SPINLOCK_INIT;

static int ahci_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	ahci_t *a = dev->priv;
	uint8_t *out = buf;
	while (count) {
		uint32_t n = count > (BOUNCE_BYTES / 512) ? (BOUNCE_BYTES / 512) : (uint32_t)count;
		uint64_t f = spin_lock_irq(&ahci_lock);
		int rc = ahci_cmd(a, ATA_READ_DMA, lba, n, 0);
		if (rc == 0) memcpy(out, a->bounce, n * 512u);
		spin_unlock_irq(&ahci_lock, f);
		if (rc != 0) return -1;
		out += n * 512u; lba += n; count -= n;
	}
	return 0;
}

static int ahci_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	ahci_t *a = dev->priv;
	const uint8_t *in = buf;
	while (count) {
		uint32_t n = count > (BOUNCE_BYTES / 512) ? (BOUNCE_BYTES / 512) : (uint32_t)count;
		uint64_t f = spin_lock_irq(&ahci_lock);
		memcpy(a->bounce, in, n * 512u);
		int rc = ahci_cmd(a, ATA_WRITE_DMA, lba, n, 1);
		spin_unlock_irq(&ahci_lock, f);
		if (rc != 0) return -1;
		in += n * 512u; lba += n; count -= n;
	}
	return 0;
}

static int ahci_flush(block_device_t *dev)
{
	ahci_t *a = dev->priv;
	uint64_t f = spin_lock_irq(&ahci_lock);
	int rc = ahci_cmd(a, ATA_FLUSH_EXT, 0, 0, 0);
	spin_unlock_irq(&ahci_lock, f);
	return rc;
}

static block_device_t g_dev;

/* SMART attribute ids reported raw (the 6-byte raw field, low 32 bits). The
 * numbering is the de facto vendor convention, not part of ATA8-ACS. */
static const struct { uint8_t id; const char *key; } smart_keys[] = {
	{ 5,   "realloc" },        { 9,   "power_on_h" },   { 187, "reported_unc" },
	{ 188, "cmd_timeout" },    { 194, "temp_c" },       { 196, "realloc_events" },
	{ 197, "pending" },        { 198, "offline_unc" },  { 199, "udma_crc" },
};

static void ahci_diag(hw_out_t *o)
{
	ahci_t *a = &g_ahci;
	volatile uint32_t *p = port_reg(a);
	hw_begin(o, "disk", g_dev.name);
	hw_ks(o, "driver", "ahci");
	if (a->model[0]) hw_ks(o, "model", a->model);
	hw_kv(o, "sata_gen", (p[0x28 / 4] >> 4) & 0xF);    /* PxSSTS.SPD */
	hw_kv(o, "errors", a->n_err);
	hw_kv(o, "timeouts", a->n_timeout);
	hw_kv(o, "busy", a->n_busy);
	hw_kv(o, "retries", 0);
	hw_kv(o, "link_err_cmds", a->n_serr);
	hw_kv(o, "serr_bits", a->serr_bits);
	hw_kv(o, "smart", a->smart_ok);
	if (a->smart_ok) {
		static uint8_t d[512];      /* under ahci_lock */
		uint64_t f = spin_lock_irq(&ahci_lock);
		uint64_t keep_err = a->n_err;   /* a refused SMART command is not a media error */
		int st = ahci_cmd_f(a, ATA_SMART, SMART_STATUS, 0, SMART_LBA, 0, 0);
		/* D2H register FIS at FB + 0x40: LBA mid/high become 0xF4/0x2C when a
		 * threshold is exceeded, stay 0x4F/0xC2 otherwise. */
		uint8_t *rf = (uint8_t *)PHYS_TO_VIRT(a->fb_phys) + 0x40;
		uint8_t mid = rf[5], hi = rf[6];
		int rd = ahci_cmd_f(a, ATA_SMART, SMART_READ_DATA, 0, SMART_LBA, 1, 0);
		if (rd == 0) memcpy(d, a->bounce, sizeof d);
		a->n_err = keep_err;
		spin_unlock_irq(&ahci_lock, f);
		hw_ks(o, "smart_health", st != 0 ? "unknown" :
		      (mid == 0x4F && hi == 0xC2) ? "ok" : (mid == 0xF4 && hi == 0x2C) ? "fail" : "unknown");
		hw_kv(o, "smart_read", rd == 0);
		if (rd == 0) {
			for (int i = 0; i < 30; i++) {
				const uint8_t *e = d + 2 + i * 12;
				if (!e[0]) continue;
				for (unsigned k = 0; k < sizeof smart_keys / sizeof smart_keys[0]; k++) {
					if (smart_keys[k].id != e[0]) continue;
					uint64_t raw = (uint64_t)e[5] | ((uint64_t)e[6] << 8) |
					               ((uint64_t)e[7] << 16) | ((uint64_t)e[8] << 24);
					if (e[0] == 194) raw &= 0xFF;   /* the rest are min/max on many drives */
					hw_kv(o, smart_keys[k].key, raw);
				}
			}
		}
	}
	hw_end(o);
}

void ahci_init(void)
{
#if defined(__x86_64__)
	uint8_t bus, dev, fn;
	if (pci_find_class(0x01, 0x06, 0x01, &bus, &dev, &fn) != 0) {
		return;   /* no AHCI controller */
	}

	uint16_t cmd = pci_read16(bus, dev, fn, PCI_COMMAND);
	pci_write16(bus, dev, fn, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER);

	uint64_t abar = pci_read32(bus, dev, fn, PCI_BAR5) & ~0xFUL;
	if (!abar) return;
	g_ahci.abar = (volatile uint32_t *)ioremap(abar, 0x2000,
	                                            VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	/* Firmware need not leave these on, and without bus mastering DMA never runs. */
	pci_write16(bus, dev, fn, PCI_COMMAND,
	            pci_read16(bus, dev, fn, PCI_COMMAND) | PCI_CMD_MEM | PCI_CMD_MASTER);
	g_ahci.abar[0x04 / 4] |= GHC_AE;        /* GHC.AE */

	uint32_t pi = g_ahci.abar[0x0C / 4];    /* ports implemented */
	int port = -1;
	for (int i = 0; i < 32; i++) {
		if (!(pi & (1u << i))) continue;
		volatile uint32_t *p = (volatile uint32_t *)((uint8_t *)g_ahci.abar + 0x100 + i * 0x80);
		if ((p[0x28 / 4] & 0x0F) != 3) continue;          /* PxSSTS.DET != 3 */
		if (p[0x24 / 4] != 0x00000101) continue;          /* PxSIG != SATA disk */
		port = i; break;
	}
	if (port < 0) { kprintf(KERN_INFO "ahci: no sata disk\n"); return; }
	g_ahci.port = port;

	/* One page holds the command list (1 KB) + received FIS (256 B) + command
	 * table (256 B); a separate buffer is the DMA bounce. */
	void *page = buddy_alloc(0);
	void *bnc  = buddy_alloc(BOUNCE_ORDER);
	if (!page || !bnc) return;
	memset((void *)PHYS_TO_VIRT((uint64_t)page), 0, 4096);
	g_ahci.clb_phys    = (uint64_t)page;
	g_ahci.fb_phys     = (uint64_t)page + 0x400;
	g_ahci.ctab_phys   = (uint64_t)page + 0x500;
	g_ahci.ctab        = (uint8_t *)PHYS_TO_VIRT(g_ahci.ctab_phys);
	g_ahci.bounce_phys = (uint64_t)bnc;
	g_ahci.bounce      = (uint8_t *)PHYS_TO_VIRT((uint64_t)bnc);

	volatile uint32_t *p = port_reg(&g_ahci);
	p[0x18 / 4] &= ~(PxCMD_ST | PxCMD_FRE);                /* stop */
	uint64_t g = 0;
	while ((p[0x18 / 4] & (PxCMD_CR | PxCMD_FR)) && ++g < 100000000UL) ;
	p[0x00 / 4] = (uint32_t)g_ahci.clb_phys;               /* PxCLB */
	p[0x04 / 4] = (uint32_t)(g_ahci.clb_phys >> 32);
	p[0x08 / 4] = (uint32_t)g_ahci.fb_phys;                /* PxFB */
	p[0x0C / 4] = (uint32_t)(g_ahci.fb_phys >> 32);
	p[0x30 / 4] = (uint32_t)-1;                            /* clear PxSERR */
	p[0x18 / 4] |= PxCMD_FRE;
	p[0x18 / 4] |= PxCMD_ST;                               /* start */

	/* IDENTIFY → sector count (words 100-103, 48-bit LBA) */
	uint64_t sectors = 0;
	if (ahci_cmd(&g_ahci, ATA_IDENTIFY, 0, 1, 0) == 0) {
		uint16_t *id = (uint16_t *)g_ahci.bounce;
		sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
		          ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
		if (!sectors) sectors = id[60] | ((uint32_t)id[61] << 16);   /* 28-bit fallback */
		char sn[20];                                     /* words 10-19, bytes swapped */
		for (int i = 0; i < 10; i++) {
			sn[2 * i]     = (char)(id[10 + i] >> 8);
			sn[2 * i + 1] = (char)(id[10 + i] & 0xFF);
		}
		block_copy_serial(g_dev.serial, sizeof(g_dev.serial), sn, sizeof(sn));
		/* words 27-46 model, byte-swapped; words 82/85 bit 0: SMART supported/enabled */
		int m = 0;
		for (int i = 0; i < 20; i++) {
			g_ahci.model[m++] = (char)(id[27 + i] >> 8);
			g_ahci.model[m++] = (char)(id[27 + i] & 0xFF);
		}
		while (m > 0 && (g_ahci.model[m - 1] == ' ' || !g_ahci.model[m - 1])) m--;
		g_ahci.model[m] = 0;
		for (int i = 0; i < m; i++)
			if ((unsigned char)g_ahci.model[i] < 0x21 || (unsigned char)g_ahci.model[i] > 0x7E)
				g_ahci.model[i] = '_';
		g_ahci.smart_ok = (id[82] & 1) && (id[85] & 1) && id[82] != 0xFFFF;
	}

	memcpy(g_dev.name, "sata0", 6);
	g_dev.size_blocks = sectors;
	g_dev.flush = ahci_flush;
	g_dev.read  = ahci_read;
	g_dev.write = ahci_write;
	g_dev.priv  = &g_ahci;
	block_device_register(&g_dev);
	hwdiag_register(ahci_diag);
	kprintf(KERN_INFO "ahci: sata0 port %d, %lu MB\n", port, (unsigned long)(sectors / 2048));
#endif
}
