#include <kernel/pci.h>
#include <kernel/block.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

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
} ahci_t;

static ahci_t g_ahci;

static volatile uint32_t *port_reg(ahci_t *a)
{
	return (volatile uint32_t *)((uint8_t *)a->abar + 0x100 + a->port * 0x80);
}

/* Issue one ATA command on slot 0 and poll for completion. cmd_is_write picks
 * the PRDT direction; lba/count in sectors; data already in/out of the bounce. */
static int ahci_cmd(ahci_t *a, uint8_t cmd, uint64_t lba, uint32_t count, int write)
{
	volatile uint32_t *p = port_reg(a);

	p[0x10 / 4] = (uint32_t)-1;   /* PxIS: clear */
	p[0x30 / 4] = (uint32_t)-1;   /* PxSERR: clear */

	/* command header[0] */
	uint32_t *hdr = (uint32_t *)PHYS_TO_VIRT(a->clb_phys);
	hdr[0] = (5) | (write ? (1u << 6) : 0) | (1u << 16);   /* CFL=5 dwords, W, PRDTL=1 */
	hdr[1] = 0;                                            /* PRDByteCount */
	hdr[2] = (uint32_t)a->ctab_phys;
	hdr[3] = (uint32_t)(a->ctab_phys >> 32);

	memset(a->ctab, 0, 128 + 16);
	uint8_t *fis = a->ctab;                /* command FIS (H2D) */
	fis[0] = 0x27;                         /* FIS type: register H2D */
	fis[1] = 0x80;                         /* C=1 (command) */
	fis[2] = cmd;
	fis[4] = lba & 0xFF;
	fis[5] = (lba >> 8) & 0xFF;
	fis[6] = (lba >> 16) & 0xFF;
	fis[7] = 0x40;                         /* device: LBA mode */
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
	prdt[3] = (count * 512u) - 1;          /* byte count - 1 */

	/* wait until not busy, then issue slot 0 */
	uint64_t g = 0;
	while ((p[0x20 / 4] & 0x88) && ++g < 100000000UL) ;   /* PxTFD BSY|DRQ */
	p[0x38 / 4] = 1;                        /* PxCI: slot 0 */

	g = 0;
	while (p[0x38 / 4] & 1) {
		if (p[0x10 / 4] & (1u << 30)) return -1;   /* PxIS TFES: task file error */
		if (++g > 200000000UL) { kprintf("ahci: timeout lba=%lu\n", (unsigned long)lba); return -1; }
	}
	if (p[0x20 / 4] & 0x01) return -1;      /* PxTFD ERR */
	return 0;
}

static int ahci_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	ahci_t *a = dev->priv;
	uint8_t *out = buf;
	while (count) {
		uint32_t n = count > (BOUNCE_BYTES / 512) ? (BOUNCE_BYTES / 512) : (uint32_t)count;
		if (ahci_cmd(a, ATA_READ_DMA, lba, n, 0) != 0) return -1;
		memcpy(out, a->bounce, n * 512u);
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
		memcpy(a->bounce, in, n * 512u);
		if (ahci_cmd(a, ATA_WRITE_DMA, lba, n, 1) != 0) return -1;
		in += n * 512u; lba += n; count -= n;
	}
	return 0;
}

static block_device_t g_dev;

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
	if (port < 0) { kprintf("ahci: no SATA disk\n"); return; }
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
	}

	memcpy(g_dev.name, "sata0", 6);
	g_dev.size_blocks = sectors;
	g_dev.read  = ahci_read;
	g_dev.write = ahci_write;
	g_dev.priv  = &g_ahci;
	block_device_register(&g_dev);
	kprintf("ahci: sata0 port %d, %lu MB\n", port, (unsigned long)(sectors / 2048));
#endif
}
