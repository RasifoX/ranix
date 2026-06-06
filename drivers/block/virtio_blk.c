#include <kernel/block.h>
#include <kernel/hal.h>
#include <kernel/pci.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * virtio-blk legacy (spec 0.9.5) over PCI.
 * PCI vendor=0x1AF4 device=0x1001.
 * Uses BAR0 I/O port space, split-ring virtqueue, polling mode.
 */

/* ── virtio legacy I/O register offsets ─────────────────────── */
#define VIRTIO_DEV_FEATURES   0x00
#define VIRTIO_DRV_FEATURES   0x04
#define VIRTIO_QUEUE_ADDR     0x08   /* queue physical page number */
#define VIRTIO_QUEUE_SIZE     0x0C   /* 16-bit R */
#define VIRTIO_QUEUE_SEL      0x0E   /* 16-bit W */
#define VIRTIO_QUEUE_NOTIFY   0x10   /* 16-bit W */
#define VIRTIO_STATUS         0x12   /* 8-bit RW */
#define VIRTIO_ISR            0x13   /* 8-bit R */
#define VIRTIO_CFG_CAPACITY   0x14   /* 64-bit, device-specific */

#define VIRTIO_STATUS_ACK     0x01
#define VIRTIO_STATUS_DRIVER  0x02
#define VIRTIO_STATUS_OK      0x04
#define VIRTIO_STATUS_FAILED  0x80

/* ── virtio split ring ───────────────────────────────────────── */
#define VRING_Q     256   /* queue depth — must be power of two */

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2   /* device writes this descriptor */

typedef struct {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VRING_Q];
} __attribute__((packed)) vring_avail_t;

typedef struct {
	uint32_t id;
	uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

typedef struct {
	uint16_t flags;
	uint16_t idx;
	vring_used_elem_t ring[VRING_Q];
} __attribute__((packed)) vring_used_t;

/* ── blk request header ──────────────────────────────────────── */
#define VIRTIO_BLK_T_IN   0   /* read */
#define VIRTIO_BLK_T_OUT  1   /* write */

typedef struct {
	uint32_t type;
	uint32_t ioprio;
	uint64_t sector;
} __attribute__((packed)) virtio_blk_req_t;

/* ── driver state ────────────────────────────────────────────── */

typedef struct {
	uint16_t iobase;

	/* vring pages: desc on page 0, avail on page 1, used on page 2 */
	vring_desc_t  *desc;
	vring_avail_t *avail;
	vring_used_t  *used;

	/* request scratch page */
	uint8_t *req_page;

	uint16_t last_used_idx;
} vblk_priv_t;

static uint8_t vblk_vring[3 * 4096] __attribute__((aligned(4096)));
static uint8_t vblk_req[4096]        __attribute__((aligned(4096)));

static vblk_priv_t g_priv;
static block_device_t g_dev;

/* ── port I/O helpers ────────────────────────────────────────── */
#if defined(__x86_64__)
#include <asm/io.h>
static void     vio_outb(uint16_t base, uint8_t  reg, uint8_t  v) { outb((uint16_t)(base+reg), v); }
static void     vio_outw(uint16_t base, uint8_t  reg, uint16_t v) { outw((uint16_t)(base+reg), v); }
static void     vio_outl(uint16_t base, uint8_t  reg, uint32_t v) { outl((uint16_t)(base+reg), v); }
static uint8_t  vio_inb(uint16_t base,  uint8_t  reg) { return inb((uint16_t)(base+reg)); }
static uint16_t vio_inw(uint16_t base,  uint8_t  reg) { return inw((uint16_t)(base+reg)); }
#else
static void     vio_outb(uint16_t b, uint8_t r, uint8_t  v)  { (void)b;(void)r;(void)v; }
static void     vio_outw(uint16_t b, uint8_t r, uint16_t v)  { (void)b;(void)r;(void)v; }
static void     vio_outl(uint16_t b, uint8_t r, uint32_t v)  { (void)b;(void)r;(void)v; }
static uint8_t  vio_inb(uint16_t b,  uint8_t r)  { (void)b;(void)r; return 0; }
static uint16_t vio_inw(uint16_t b,  uint8_t r)  { (void)b;(void)r; return 0; }
#endif

/* ── read implementation ─────────────────────────────────────── */

static int vblk_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	vblk_priv_t *p = dev->priv;
	uint8_t *dst = buf;

	for (uint64_t i = 0; i < count; i++) {
		/* layout req_page: [virtio_blk_req_t (16B)] [sector data (512B)] [status (1B)] */
		virtio_blk_req_t *hdr  = (virtio_blk_req_t *)p->req_page;
		uint8_t          *data = p->req_page + 16;
		uint8_t          *stat = p->req_page + 16 + 512;

		hdr->type    = VIRTIO_BLK_T_IN;
		hdr->ioprio  = 0;
		hdr->sector  = lba + i;
		*stat        = 0xFF;

		uint64_t phys_req  = VIRT_TO_PHYS((uint64_t)p->req_page);

		uint16_t d0 = 0, d1 = 1, d2 = 2;

		/* descriptor 0: header (device reads) */
		p->desc[d0].addr  = phys_req;
		p->desc[d0].len   = 16;
		p->desc[d0].flags = VRING_DESC_F_NEXT;
		p->desc[d0].next  = d1;

		/* descriptor 1: data buffer (device writes) */
		p->desc[d1].addr  = phys_req + 16;
		p->desc[d1].len   = 512;
		p->desc[d1].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
		p->desc[d1].next  = d2;

		/* descriptor 2: status byte (device writes) */
		p->desc[d2].addr  = phys_req + 16 + 512;
		p->desc[d2].len   = 1;
		p->desc[d2].flags = VRING_DESC_F_WRITE;
		p->desc[d2].next  = 0;

		/* put descriptor chain in avail ring */
		uint16_t avail_idx = p->avail->idx;
		p->avail->ring[avail_idx % VRING_Q] = d0;
		__asm__ volatile("" ::: "memory");   /* compiler barrier */
		p->avail->idx = (uint16_t)(avail_idx + 1);
		__asm__ volatile("" ::: "memory");

		vio_outw(p->iobase, VIRTIO_QUEUE_NOTIFY, 0);

		/* poll used ring */
		uint32_t spin = 0;
		while (p->used->idx == p->last_used_idx) {
			cpu_relax();
			if (++spin > 10000000) {
				kprintf("virtio-blk: timeout lba=%lu\n", lba + i);
				return -1;
			}
		}
		p->last_used_idx = p->used->idx;

		if (*stat != 0) {
			kprintf("virtio-blk: read error lba=%lu status=%u\n", lba + i, *stat);
			return -1;
		}

		memcpy(dst + i * 512, data, 512);
	}
	return 0;
}

static int vblk_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	vblk_priv_t    *p   = dev->priv;
	const uint8_t  *src = buf;

	for (uint64_t i = 0; i < count; i++) {
		virtio_blk_req_t *hdr  = (virtio_blk_req_t *)p->req_page;
		uint8_t          *data = p->req_page + 16;
		uint8_t          *stat = p->req_page + 16 + 512;

		hdr->type   = VIRTIO_BLK_T_OUT;
		hdr->ioprio = 0;
		hdr->sector = lba + i;
		*stat       = 0xFF;
		memcpy(data, src + i * 512, 512);

		uint64_t phys_req = VIRT_TO_PHYS((uint64_t)p->req_page);
		uint16_t d0 = 0, d1 = 1, d2 = 2;

		p->desc[d0].addr  = phys_req;
		p->desc[d0].len   = 16;
		p->desc[d0].flags = VRING_DESC_F_NEXT;
		p->desc[d0].next  = d1;

		p->desc[d1].addr  = phys_req + 16;
		p->desc[d1].len   = 512;
		p->desc[d1].flags = VRING_DESC_F_NEXT;   /* device reads, no WRITE flag */
		p->desc[d1].next  = d2;

		p->desc[d2].addr  = phys_req + 16 + 512;
		p->desc[d2].len   = 1;
		p->desc[d2].flags = VRING_DESC_F_WRITE;
		p->desc[d2].next  = 0;

		uint16_t avail_idx = p->avail->idx;
		p->avail->ring[avail_idx % VRING_Q] = d0;
		__asm__ volatile("" ::: "memory");
		p->avail->idx = (uint16_t)(avail_idx + 1);
		__asm__ volatile("" ::: "memory");

		vio_outw(p->iobase, VIRTIO_QUEUE_NOTIFY, 0);

		uint32_t spin = 0;
		while (p->used->idx == p->last_used_idx) {
			cpu_relax();
			if (++spin > 10000000) {
				kprintf("virtio-blk: write timeout lba=%lu\n", lba + i);
				return -1;
			}
		}
		p->last_used_idx = p->used->idx;

		if (*stat != 0) {
			kprintf("virtio-blk: write error lba=%lu status=%u\n", lba + i, *stat);
			return -1;
		}
	}
	return 0;
}

/* ── init ────────────────────────────────────────────────────── */

void virtio_blk_init(void)
{
#if !defined(__x86_64__)
	kprintf("virtio-blk: not available on this arch\n");
	return;
#else
	uint8_t bus, devn, fn;
	if (pci_find(0x1AF4, 0x1001, &bus, &devn, &fn) != 0) {
		kprintf("virtio-blk: not found\n");
		return;
	}

	/* enable I/O + Bus Master */
	uint16_t cmd = pci_read16(bus, devn, fn, PCI_COMMAND);
	pci_write16(bus, devn, fn, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_MASTER);

	/* BAR0: I/O space — bit 0 = 1, bits 31:2 = base */
	uint32_t bar0 = pci_read32(bus, devn, fn, PCI_BAR0);
	uint16_t iobase = (uint16_t)(bar0 & 0xFFFC);

	/* legacy virtio init sequence */
	vio_outb(iobase, VIRTIO_STATUS, 0);                                      /* reset */
	vio_outb(iobase, VIRTIO_STATUS, VIRTIO_STATUS_ACK);
	vio_outb(iobase, VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

	/* accept all device features */
	uint32_t features = vio_inw(iobase, VIRTIO_DEV_FEATURES);
	vio_outl(iobase, VIRTIO_DRV_FEATURES, features);

	/* read capacity (2 × 32-bit little-endian, device-specific region at +0x14) */
	uint32_t cap_lo = inl((uint16_t)(iobase + VIRTIO_CFG_CAPACITY));
	uint32_t cap_hi = inl((uint16_t)(iobase + VIRTIO_CFG_CAPACITY + 4));
	uint64_t capacity = ((uint64_t)cap_hi << 32) | cap_lo;

	/* set up virtqueue 0 */
	vio_outw(iobase, VIRTIO_QUEUE_SEL, 0);
	uint16_t qsz = vio_inw(iobase, VIRTIO_QUEUE_SIZE);
	if (qsz == 0 || qsz > VRING_Q) qsz = VRING_Q;

	memset(vblk_vring, 0, sizeof(vblk_vring));
	memset(vblk_req,  0, sizeof(vblk_req));

	vring_desc_t  *desc  = (vring_desc_t  *)vblk_vring;
	vring_avail_t *avail = (vring_avail_t *)(vblk_vring + 4096);
	vring_used_t  *used  = (vring_used_t  *)(vblk_vring + 8192);
	uint8_t       *req_page = vblk_req;

	uint64_t vring_phys = VIRT_TO_PHYS((uint64_t)vblk_vring);
	uint64_t pfn = vring_phys / 4096;
	vio_outl(iobase, VIRTIO_QUEUE_ADDR, (uint32_t)pfn);

	vio_outb(iobase, VIRTIO_STATUS,
	         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_OK);

	/* fill driver state */
	g_priv.iobase        = iobase;
	g_priv.desc          = desc;
	g_priv.avail         = avail;
	g_priv.used          = used;
	g_priv.req_page      = req_page;
	g_priv.last_used_idx = 0;

	memset(&g_dev, 0, sizeof(g_dev));
	memcpy(g_dev.name, "vblk0", 6);
	g_dev.size_blocks = capacity;
	g_dev.read        = vblk_read;
	g_dev.write       = vblk_write;
	g_dev.priv        = &g_priv;

	if (block_device_register(&g_dev) == 0)
		kprintf("virtio-blk: %lu MB at I/O 0x%x\n",
		        capacity >> 11, iobase);
#endif
}
