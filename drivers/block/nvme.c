#include <kernel/pci.h>
#include <kernel/block.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * NVMe controller (PCI class 01:08:02), polling. Resets + enables the
 * controller, builds the admin SQ/CQ, IDENTIFY controller + namespace 1, then
 * one I/O SQ/CQ pair. NVM READ/WRITE complete by spinning on the CQ phase bit
 * (no MSI-X), the same synchronous style as ahci/xhci. Namespace 1 is exposed
 * as a block_device ("nvme0n1"). QEMU's -device nvme exercises this path.
 */

#if defined(__x86_64__)

/* controller registers (BAR0) */
#define REG_CAP     0x00    /* 64-bit: MQES[15:0], DSTRD[35:32] */
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28    /* 64-bit */
#define REG_ACQ     0x30    /* 64-bit */

#define CC_EN       (1u << 0)
#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)

/* admin opcodes */
#define ADM_IDENTIFY    0x06
#define ADM_CREATE_SQ   0x01
#define ADM_CREATE_CQ   0x05

/* I/O opcodes */
#define NVM_WRITE   0x01
#define NVM_READ    0x02

#define QDEPTH      64      /* entries per queue (admin + I/O) */

typedef struct {
	uint32_t cdw0;          /* opcode | fuse | cid<<16 */
	uint32_t nsid;
	uint64_t rsvd;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) sqe_t;

typedef struct {
	uint32_t dw0;
	uint32_t dw1;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t cid;
	uint16_t status;        /* phase in bit 0 */
} __attribute__((packed)) cqe_t;

typedef struct {
	sqe_t   *sq;
	cqe_t   *cq;
	uint64_t sq_phys, cq_phys;
	volatile uint32_t *sq_db;
	volatile uint32_t *cq_db;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t phase;
	uint16_t cid;
} nvme_q_t;

typedef struct {
	volatile uint8_t *bar;
	uint32_t  dstrd;        /* doorbell stride in bytes */
	nvme_q_t  admin;
	nvme_q_t  io;
	uint64_t  bounce_phys;  /* 4 KB DMA bounce (one PRP entry) */
	uint8_t  *bounce;
	uint32_t  nsze_lo;      /* namespace size in logical blocks */
	uint32_t  lba_shift;    /* log2(lba size) */
} nvme_t;

static nvme_t g_nvme;

static uint32_t rd32(nvme_t *n, uint32_t off)
{
	return *(volatile uint32_t *)(n->bar + off);
}
static void wr32(nvme_t *n, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(n->bar + off) = v;
}
static uint64_t rd64(nvme_t *n, uint32_t off)
{
	return (uint64_t)rd32(n, off) | ((uint64_t)rd32(n, off + 4) << 32);
}
static void wr64(nvme_t *n, uint32_t off, uint64_t v)
{
	wr32(n, off, (uint32_t)v);
	wr32(n, off + 4, (uint32_t)(v >> 32));
}

static void *dma_page(uint64_t *phys)
{
	void *p = buddy_alloc(0);
	if (!p) return 0;
	memset((void *)PHYS_TO_VIRT((uint64_t)p), 0, 4096);
	*phys = (uint64_t)p;
	return (void *)PHYS_TO_VIRT((uint64_t)p);
}

static int q_alloc(nvme_t *n, nvme_q_t *q, int qid)
{
	uint64_t sp, cp;
	q->sq = dma_page(&sp);
	q->cq = dma_page(&cp);
	if (!q->sq || !q->cq) return -1;
	q->sq_phys = sp;
	q->cq_phys = cp;
	q->sq_tail = 0;
	q->cq_head = 0;
	q->phase = 1;
	q->cid = 0;
	q->sq_db = (volatile uint32_t *)(n->bar + 0x1000 + (2 * qid) * n->dstrd);
	q->cq_db = (volatile uint32_t *)(n->bar + 0x1000 + (2 * qid + 1) * n->dstrd);
	return 0;
}

/* submit one command and poll the CQ for its completion. returns the status
 * field (bits 15:1 are the status code; 0 = success). */
static int q_submit(nvme_q_t *q, sqe_t *cmd)
{
	cmd->cdw0 |= (uint32_t)q->cid << 16;
	q->sq[q->sq_tail] = *cmd;
	q->sq_tail = (q->sq_tail + 1) % QDEPTH;
	*q->sq_db = q->sq_tail;

	cqe_t *c = &q->cq[q->cq_head];
	volatile uint16_t *st = &c->status;
	for (uint64_t g = 0; (*st & 1) != q->phase; g++)
		if (g > 400000000UL) { kprintf("nvme: cq timeout\n"); return -1; }
	uint16_t status = *st >> 1;
	q->cq_head = (q->cq_head + 1) % QDEPTH;
	if (q->cq_head == 0) q->phase ^= 1;
	*q->cq_db = q->cq_head;
	q->cid++;
	return status & 0x7FF;
}

static int nvme_rw(nvme_t *n, int write, uint64_t lba, uint32_t nlb)
{
	sqe_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = write ? NVM_WRITE : NVM_READ;
	cmd.nsid = 1;
	cmd.prp1 = n->bounce_phys;
	cmd.cdw10 = (uint32_t)lba;
	cmd.cdw11 = (uint32_t)(lba >> 32);
	cmd.cdw12 = nlb - 1;            /* 0-based block count */
	return q_submit(&n->io, &cmd);
}

static uint32_t g_blk_per_xfer;    /* 512-byte sectors per 4 KB bounce */

static int nvme_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	nvme_t *n = dev->priv;
	uint32_t lbsz = 1u << n->lba_shift;
	uint8_t *out = buf;
	while (count) {
		uint32_t secs = count > g_blk_per_xfer ? g_blk_per_xfer : (uint32_t)count;
		uint64_t byteoff = lba * 512;
		uint32_t bytes = secs * 512;
		uint32_t nlb = bytes / lbsz;
		if (nvme_rw(n, 0, byteoff / lbsz, nlb) != 0) return -1;
		memcpy(out, n->bounce, bytes);
		out += bytes; lba += secs; count -= secs;
	}
	return 0;
}

static int nvme_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	nvme_t *n = dev->priv;
	uint32_t lbsz = 1u << n->lba_shift;
	const uint8_t *in = buf;
	while (count) {
		uint32_t secs = count > g_blk_per_xfer ? g_blk_per_xfer : (uint32_t)count;
		uint64_t byteoff = lba * 512;
		uint32_t bytes = secs * 512;
		uint32_t nlb = bytes / lbsz;
		memcpy(n->bounce, in, bytes);
		if (nvme_rw(n, 1, byteoff / lbsz, nlb) != 0) return -1;
		in += bytes; lba += secs; count -= secs;
	}
	return 0;
}

static block_device_t g_dev;

void nvme_init(void)
{
	uint8_t bus, dev, fn;
	if (pci_find_class(0x01, 0x08, 0x02, &bus, &dev, &fn) != 0)
		return;   /* no NVMe controller */

	uint16_t cmd = pci_read16(bus, dev, fn, PCI_COMMAND);
	pci_write16(bus, dev, fn, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER);

	uint64_t bar = pci_read32(bus, dev, fn, PCI_BAR0) & ~0xFUL;
	uint64_t barhi = pci_read32(bus, dev, fn, PCI_BAR1);
	bar |= barhi << 32;
	if (!bar) return;

	nvme_t *n = &g_nvme;
	n->bar = ioremap(bar, 0x2000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);

	uint64_t cap = rd64(n, REG_CAP);
	n->dstrd = 4u << ((cap >> 32) & 0xF);     /* doorbell stride */

	/* disable, then wait for !RDY */
	wr32(n, REG_CC, rd32(n, REG_CC) & ~CC_EN);
	for (uint64_t g = 0; g < 200000000UL; g++)
		if (!(rd32(n, REG_CSTS) & CSTS_RDY)) break;

	/* admin queues */
	if (q_alloc(n, &n->admin, 0) != 0) return;
	wr32(n, REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
	wr64(n, REG_ASQ, n->admin.sq_phys);
	wr64(n, REG_ACQ, n->admin.cq_phys);

	/* CC: IOSQES=6 (64 B), IOCQES=4 (16 B), MPS=0 (4 KB), enable */
	wr32(n, REG_CC, (6u << 16) | (4u << 20) | CC_EN);
	for (uint64_t g = 0; ; g++) {
		uint32_t s = rd32(n, REG_CSTS);
		if (s & CSTS_RDY) break;
		if (s & CSTS_CFS) { kprintf("nvme: fatal status\n"); return; }
		if (g > 200000000UL) { kprintf("nvme: enable timeout\n"); return; }
	}

	uint64_t idphys;
	uint8_t *id = dma_page(&idphys);
	if (!id) return;

	/* IDENTIFY controller (CNS=1) */
	sqe_t c;
	memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_IDENTIFY;
	c.prp1 = idphys;
	c.cdw10 = 1;
	if (q_submit(&n->admin, &c) != 0) { kprintf("nvme: identify ctrl failed\n"); return; }

	/* IDENTIFY namespace 1 (CNS=0) */
	memset(id, 0, 4096);
	memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_IDENTIFY;
	c.nsid = 1;
	c.prp1 = idphys;
	c.cdw10 = 0;
	if (q_submit(&n->admin, &c) != 0) { kprintf("nvme: identify ns failed\n"); return; }

	uint64_t nsze = *(uint64_t *)(id + 0);          /* NSZE: size in logical blocks */
	uint8_t  flbas = id[26] & 0xF;                  /* formatted LBA index */
	uint32_t lbaf = *(uint32_t *)(id + 128 + flbas * 4);
	n->lba_shift = (lbaf >> 16) & 0xFF;             /* LBADS: log2 of LBA bytes */
	if (n->lba_shift < 9) n->lba_shift = 9;

	/* I/O CQ (qid 1) */
	if (q_alloc(n, &n->io, 1) != 0) return;
	memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_CREATE_CQ;
	c.prp1 = n->io.cq_phys;
	c.cdw10 = ((QDEPTH - 1) << 16) | 1;             /* qsize-1 | qid */
	c.cdw11 = 1;                                    /* PC=1 (physically contiguous) */
	if (q_submit(&n->admin, &c) != 0) { kprintf("nvme: create cq failed\n"); return; }

	/* I/O SQ (qid 1, points at CQ 1) */
	memset(&c, 0, sizeof(c));
	c.cdw0 = ADM_CREATE_SQ;
	c.prp1 = n->io.sq_phys;
	c.cdw10 = ((QDEPTH - 1) << 16) | 1;
	c.cdw11 = (1u << 16) | 1;                       /* CQID=1 | PC=1 */
	if (q_submit(&n->admin, &c) != 0) { kprintf("nvme: create sq failed\n"); return; }

	void *bnc = dma_page(&n->bounce_phys);
	if (!bnc) return;
	n->bounce = bnc;
	g_blk_per_xfer = 4096 / 512;                    /* one PRP page per transfer */

	uint32_t lbsz = 1u << n->lba_shift;
	uint64_t sectors = nsze * (lbsz / 512);         /* expose in 512-byte units */

	memcpy(g_dev.name, "nvme0n1", 8);
	g_dev.size_blocks = sectors;
	g_dev.read  = nvme_read;
	g_dev.write = nvme_write;
	g_dev.priv  = n;
	block_device_register(&g_dev);
	kprintf("nvme: nvme0n1 %lu sectors (lba %u)\n",
	        (unsigned long)sectors, lbsz);
}

#else
void nvme_init(void) {}
#endif
