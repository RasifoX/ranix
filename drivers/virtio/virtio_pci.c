/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Modern virtio-PCI transport (virtio 1.2, 4.1.4): what the header says, in
 * about 200 lines. The PCI function carries vendor capabilities (id 0x09); each
 * names a BAR, an offset and a length for one register block: common config,
 * doorbells, interrupt status, device config. We map the blocks we use, walk
 * the status handshake (ACKNOWLEDGE, DRIVER, FEATURES_OK, DRIVER_OK) and lay
 * each split virtqueue (descriptors, available ring, used ring) in one page.
 * No interrupts (INTx disabled, no MSI-X vectors, the rings ask for none):
 * drivers poll the used ring (virtio-gpu does it from the 1 ms tick). x86_64 only for now: that is where PCI config access exists.
 */
#include <drivers/virtio/virtio_pci.h>
#include <kernel/pci.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/hal.h>
#include <asm/mmu.h>

/* vendor capability: cfg_type */
#define CAP_COMMON  1
#define CAP_NOTIFY  2
#define CAP_DEVICE  4

/* common configuration, byte offsets (4.1.4.3) */
#define C_DFSELECT   0x00
#define C_DF         0x04
#define C_GFSELECT   0x08
#define C_GF         0x0C
#define C_MSIX       0x10
#define C_NUMQ       0x12
#define C_STATUS     0x14
#define C_QSELECT    0x16
#define C_QSIZE      0x18
#define C_QMSIX      0x1A
#define C_QENABLE    0x1C
#define C_QNOTIFYOFF 0x1E
#define C_QDESC      0x20
#define C_QDRIVER    0x28
#define C_QDEVICE    0x30

#define S_ACK        1
#define S_DRIVER     2
#define S_DRIVER_OK  4
#define S_FEATURES_OK 8
#define S_FAILED     0x80
#define F_VERSION_1  (1ULL << 32)
#define NO_VECTOR    0xFFFF
#define PCI_CMD_INTX_OFF 0x400

#define mb() __asm__ volatile("mfence" ::: "memory")

static uint8_t  r8(virtio_pci_t *v, int o)  { return *(volatile uint8_t *)(v->common + o); }
static uint16_t r16(virtio_pci_t *v, int o) { return *(volatile uint16_t *)(v->common + o); }
static uint32_t r32(virtio_pci_t *v, int o) { return *(volatile uint32_t *)(v->common + o); }
static void w8(virtio_pci_t *v, int o, uint8_t x)   { *(volatile uint8_t *)(v->common + o) = x; }
static void w16(virtio_pci_t *v, int o, uint16_t x) { *(volatile uint16_t *)(v->common + o) = x; }
static void w32(virtio_pci_t *v, int o, uint32_t x) { *(volatile uint32_t *)(v->common + o) = x; }
static void w64(virtio_pci_t *v, int o, uint64_t x) { w32(v, o, (uint32_t)x); w32(v, o + 4, (uint32_t)(x >> 32)); }

#ifdef __x86_64__
/* physical address of BAR n (memory BARs only; 64-bit BARs take two slots) */
static uint64_t bar_addr(virtio_pci_t *v, int n)
{
	uint8_t reg = (uint8_t)(PCI_BAR0 + 4 * n);
	uint32_t lo = pci_read32(v->bus, v->dev, v->fn, reg);
	if (lo & 1) return 0;                               /* I/O space */
	uint64_t a = lo & ~0xFULL;
	if (((lo >> 1) & 3) == 2 && n < 5) a |= (uint64_t)pci_read32(v->bus, v->dev, v->fn, (uint8_t)(reg + 4)) << 32;
	return a;
}

/* map the register block a vendor capability at config offset `cap` describes */
static volatile uint8_t *cap_map(virtio_pci_t *v, uint8_t cap)
{
	uint8_t bar = (uint8_t)(pci_read32(v->bus, v->dev, v->fn, (uint8_t)(cap + 4)) & 0xFF);
	uint32_t off = pci_read32(v->bus, v->dev, v->fn, (uint8_t)(cap + 8));
	uint32_t len = pci_read32(v->bus, v->dev, v->fn, (uint8_t)(cap + 12));
	if (bar > 5 || !len) return NULL;
	uint64_t base = bar_addr(v, bar);
	if (!base) return NULL;
	return ioremap(base + off, len, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
}

int virtio_pci_find(uint16_t type, virtio_pci_t *v)
{
	memset(v, 0, sizeof(*v));
	if (pci_find(0x1AF4, (uint16_t)(0x1040 + type), &v->bus, &v->dev, &v->fn)) return -1;
	if (!(pci_read16(v->bus, v->dev, v->fn, 0x06) & 0x10)) return -1;   /* no capability list */
	uint16_t cmd = pci_read16(v->bus, v->dev, v->fn, PCI_COMMAND);
	/* polled, so the INTx line stays off: nobody would read the ISR to lower it,
	 * and a line shared with another device would keep firing */
	pci_write16(v->bus, v->dev, v->fn, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF);
	uint8_t cap = (uint8_t)(pci_read16(v->bus, v->dev, v->fn, 0x34) & 0xFC);
	for (int guard = 0; cap && guard < 48; guard++) {
		uint32_t h = pci_read32(v->bus, v->dev, v->fn, cap);
		if ((h & 0xFF) == PCI_CAP_VNDR) {
			uint8_t t = (uint8_t)(h >> 24);
			if (t == CAP_COMMON && !v->common) v->common = cap_map(v, cap);
			else if (t == CAP_DEVICE && !v->device) v->device = cap_map(v, cap);
			else if (t == CAP_NOTIFY && !v->notify) {
				v->notify = cap_map(v, cap);
				v->notify_mul = pci_read32(v->bus, v->dev, v->fn, (uint8_t)(cap + 16));
			}
		}
		cap = (uint8_t)((h >> 8) & 0xFC);
	}
	return v->common && v->device && v->notify ? 0 : -1;
}
#else
int virtio_pci_find(uint16_t type, virtio_pci_t *v) { (void)type; memset(v, 0, sizeof(*v)); return -1; }
#endif

int virtio_pci_start(virtio_pci_t *v, uint64_t want)
{
	w8(v, C_STATUS, 0);                                 /* reset: reads 0 when done */
	for (int i = 0; i < 1000000 && r8(v, C_STATUS); i++) cpu_relax();
	if (r8(v, C_STATUS)) return -1;
	w8(v, C_STATUS, S_ACK);
	w8(v, C_STATUS, S_ACK | S_DRIVER);
	w32(v, C_DFSELECT, 0);
	uint64_t dev = r32(v, C_DF);
	w32(v, C_DFSELECT, 1);
	dev |= (uint64_t)r32(v, C_DF) << 32;
	if (!(dev & F_VERSION_1)) { virtio_pci_fail(v); return -1; }
	v->features = dev & (want | F_VERSION_1);
	w32(v, C_GFSELECT, 0);
	w32(v, C_GF, (uint32_t)v->features);
	w32(v, C_GFSELECT, 1);
	w32(v, C_GF, (uint32_t)(v->features >> 32));
	w8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
	if (!(r8(v, C_STATUS) & S_FEATURES_OK)) { virtio_pci_fail(v); return -1; }
	w16(v, C_MSIX, NO_VECTOR);
	return 0;
}

int virtio_pci_queue(virtio_pci_t *v, uint16_t index, uint16_t size, virtq_t *q)
{
	if (index >= r16(v, C_NUMQ)) return -1;
	w16(v, C_QSELECT, index);
	uint16_t max = r16(v, C_QSIZE);
	if (!max) return -1;
	if (size > 64) size = 64;                           /* everything fits in one page */
	while (size > max) size >>= 1;
	void *phys = buddy_alloc(0);
	if (!phys) return -1;
	uint8_t *page = (uint8_t *)PHYS_TO_VIRT(phys);
	memset(page, 0, 4096);
	memset(q, 0, sizeof(*q));
	q->size = size;
	q->index = index;
	q->desc = (virtq_desc_t *)page;                     /* 16 * size <= 1024 */
	q->avail = (volatile uint16_t *)(page + 1024);      /* 6 + 2 * size <= 134 */
	q->used = page + 2048;                              /* 6 + 8 * size <= 518 */
	q->avail[0] = 1;                                    /* VIRTQ_AVAIL_F_NO_INTERRUPT */
	w16(v, C_QSIZE, size);
	w16(v, C_QMSIX, NO_VECTOR);
	w64(v, C_QDESC, (uint64_t)phys);
	w64(v, C_QDRIVER, (uint64_t)phys + 1024);
	w64(v, C_QDEVICE, (uint64_t)phys + 2048);
	q->doorbell = (volatile uint16_t *)(v->notify + (uint64_t)r16(v, C_QNOTIFYOFF) * v->notify_mul);
	w16(v, C_QENABLE, 1);
	return 0;
}

void virtio_pci_ready(virtio_pci_t *v) { w8(v, C_STATUS, (uint8_t)(r8(v, C_STATUS) | S_DRIVER_OK)); }
void virtio_pci_fail(virtio_pci_t *v)  { w8(v, C_STATUS, (uint8_t)(r8(v, C_STATUS) | S_FAILED)); }

void virtq_kick(virtq_t *q, uint16_t head)
{
	uint16_t idx = q->avail[1];
	q->avail[2 + (idx & (q->size - 1))] = head;
	mb();                                              /* the entry before the index */
	q->avail[1] = (uint16_t)(idx + 1);
	mb();                                              /* the index before the doorbell */
	*q->doorbell = q->index;
}

int virtq_pop(virtq_t *q, uint16_t *head, uint32_t *len)
{
	uint16_t idx = *(volatile uint16_t *)(q->used + 2);
	if (idx == q->last_used) return 0;
	mb();                                              /* the index before the entry */
	volatile uint8_t *e = q->used + 4 + 8 * (q->last_used & (q->size - 1));
	*head = (uint16_t)*(volatile uint32_t *)e;
	if (len) *len = *(volatile uint32_t *)(e + 4);
	q->last_used++;
	return 1;
}
