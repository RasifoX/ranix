/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/pci.h>
#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/hwdiag.h>
#include <asm/mmu.h>
#include <kernel/proc.h>
#include <kernel/board.h>
#include <kernel/fs/gpt.h>
#include <kernel/fs/mbr.h>
#include <kernel/time.h>
#include <kernel/input.h>
#include <drivers/input/ps2.h>
#include <kernel/tick.h>
#include <kernel/init.h>

/*
 * xHCI host controller + USB enumeration + bulk-only mass-storage. Polling, no
 * interrupts: each ring is drained by spinning on the event ring. Works with QEMU's
 * qemu-xhci and is written to the spec for real controllers: BIOS handoff, scratchpad
 * pages, one slot per device on any root port. Every mass-storage LUN 0 becomes a
 * block_device ("usb0", "usb1") and GPT, ext2 or NTFS sit on top unchanged.
 *
 * Transport is Bulk-Only with a stall and reset path. A UAS-capable enclosure offers
 * BOT on alternate setting 0, which is what gets used; a device with no BOT interface
 * is reported and left alone. A read reports success only when the whole transfer
 * arrived: the bounce is reused, so a short read taken as good would return old data.
 *
 * Keyboards and mice are HID interrupt endpoints. Their transfers stay queued all the
 * time, so every wait on the event ring may meet their events: evt_next routes them
 * and the callers never see them. Between disk commands the "usbd" thread drains the
 * ring; the timer looks at the ring head once a tick, which costs one memory read, and
 * wakes it only when something is there. It also serves port changes: plugging and
 * unplugging. A hub is one more interrupt endpoint, whose report says which of its
 * ports changed; what hangs from it enumerates like anything else, with a route.
 *
 * Files, one job each: xhci.c (this one: controller bring-up, the usbd thread, the
 * tick, diagnostics), xhci_hw.h (registers), xhci_ring.c (rings, events, commands),
 * xhci_xfer.c (control and bulk transfers), usb_storage.c (mass storage), usb_hid.c
 * (keyboards and mice), usb_enum.c (enumeration, hubs), usb_port.c (plug, unplug).
 */

#if defined(__x86_64__)

#include "xhci_hw.h"

typedef struct {
	trb_t   *trb;
	uint64_t phys;
	uint32_t idx;
	uint32_t cycle;
} ring_t;

typedef struct {
	volatile uint8_t *cap;     /* MMIO base (ioremap'd) */
	volatile uint8_t *op;
	volatile uint8_t *rt;
	volatile uint32_t *db;     /* doorbell array */
	uint32_t maxslots;
	uint32_t ctxsize;          /* 32 or 64 (CSZ) */

	uint64_t *dcbaa;           /* device context base address array */
	uint64_t  dcbaa_phys;

	ring_t   cmd;

	volatile trb_t *erst_seg;  /* event ring segment (HC writes via DMA) */
	uint64_t  erst_seg_phys;
	uint64_t *erst;            /* event ring segment table (1 entry) */
	uint64_t  erst_phys;
	uint32_t  evt_idx;
	uint32_t  evt_cycle;

	mutex_t   lock;            /* one event ring: one transfer at a time */

	uint32_t  nports;          /* /proc/hwdiag */
	volatile uint32_t n_recover;   /* endpoint resets after a failed transfer */

	volatile uint64_t port_pending;  /* bit i: port i reported a change */
	uint64_t  port_tried;      /* bit i: what is on port i has had its chance */
	struct { uint16_t vid, pid; uint8_t cls; } pinfo[MAX_PORTS];
} xhci_t;

#define HID_KBD   1
#define HID_MOUSE 2
#define HID_HUB   3

/* Where a mouse report keeps its fields, in bits. */
typedef struct {
	uint8_t  id;               /* report id, 0 for none */
	uint8_t  nbtn;
	uint16_t btn;
	uint16_t x, y, w;          /* offsets */
	uint8_t  xs, ys, ws;       /* sizes, ws 0 for no wheel */
} mouse_map_t;

/* One interrupt IN endpoint of an interface. */
typedef struct {
	uint8_t  kind, proto;
	uint8_t  iface, addr, dci;
	uint8_t  interval;         /* bInterval */
	uint8_t  needs_reset, resets;
	uint16_t mps, len;
	ring_t   ring;
	uint8_t *buf;
	uint64_t buf_phys;
	mouse_map_t map;
	uint8_t  prev[8];          /* keyboard: last report */
	uint8_t  buttons;
} hid_ep_t;

/* A device being enumerated, and afterwards a disk, a keyboard, a mouse, a hub. */
typedef struct usb_dev {
	xhci_t   *x;
	uint8_t   slot;
	uint64_t  in_ctx_phys;     /* input context */
	uint8_t  *in_ctx;
	uint64_t  dev_ctx_phys;    /* device context */
	uint8_t  *dev_ctx;
	ring_t    ep0;             /* control endpoint ring */
	ring_t    ep_in;           /* bulk IN */
	ring_t    ep_out;          /* bulk OUT */
	int       dci_in, dci_out;
	uint8_t   addr_in, addr_out;
	uint16_t  mps_in, mps_out;
	uint8_t   iface;

	uint8_t  *dma;             /* descriptors, CBW and CSW (4 KB) */
	uint64_t  dma_phys;
	uint8_t  *data;            /* SCSI data stage, DATA_BYTES contiguous */
	uint64_t  data_phys;

	uint32_t  tag;
	uint32_t  blk;             /* device block size */
	struct usb_disk *disk;     /* the block device it backs, NULL if not a disk */
	char      serial[48];

	uint8_t   used, port;
	uint8_t   is_disk, gone;
	uint8_t   nhid;
	hid_ep_t  hid[MAX_HID];

	uint8_t   speed;
	struct usb_dev *parent;    /* the hub above, NULL on a root port */
	uint8_t   hub_port;        /* which of its ports */
	uint8_t   depth;           /* tiers below the root */
	uint32_t  route;
	uint8_t   tt_slot, tt_port;
	uint8_t   is_hub, hub_ports;
	uint32_t  hub_tried;
	volatile uint32_t hub_change;
} usb_dev_t;

/* The block layer has no unregister: mounts, open nodes and /dev keep pointers to a
 * device. So a name, once registered, stays; the hardware behind it comes and goes.
 * Pulled, a disk drops its device record at once and reads fail; plugged, a new
 * disk takes the first name nothing holds. All of it under the controller lock,
 * which every transfer holds too. */
typedef struct usb_disk {
	block_device_t bd;
	usb_dev_t *dev;            /* NULL while nothing is plugged in behind it */
	uint8_t   registered;
	volatile uint8_t probe;    /* partitions to read, outside the lock */
} usb_disk_t;

static xhci_t     g_xhci;
static usb_dev_t  g_devs[MAX_DEVS];
static usb_disk_t g_disk[MAX_DISKS];
static process_t *volatile usbd_task;
static volatile int usbd_idle;

/* Key repeat: PS/2 keyboards do it themselves, USB ones leave it to the host. */
static volatile uint16_t rep_code;
static volatile uint64_t rep_at;
#define REP_DELAY_MS 400u
#define REP_EVERY_MS 33u

static void hid_event(usb_dev_t *d, hid_ep_t *h, uint32_t status);

static inline uint64_t tsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

/* The TSC runs at no less than a GHz on anything with xHCI: a ms here is at least a
 * ms and a few on a fast chip, which suits timeouts and settling delays. */
#define TSC_PER_MS 3000000ULL

static void delay_ms(uint32_t ms)
{
	uint64_t t = tsc();
	while (tsc() - t < ms * TSC_PER_MS) cpu_relax();
}

static uint32_t rd32(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}
static void wr32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}
static void wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
	*(volatile uint32_t *)(base + off) = (uint32_t)v;
	*(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static void *dma_page(uint64_t *phys)
{
	void *p = buddy_alloc(0);
	if (!p) return 0;
	memset((void *)PHYS_TO_VIRT((uint64_t)p), 0, 4096);
	*phys = (uint64_t)p;
	return (void *)PHYS_TO_VIRT((uint64_t)p);
}

static void dma_free(uint64_t *phys)
{
	if (*phys) buddy_free(*phys, 0);
	*phys = 0;
}

#include "xhci_ring.c"
#include "xhci_xfer.c"
#include "usb_storage.c"
#include "usb_hid.c"
#include "usb_enum.c"
#include "usb_port.c"

/* Firmware may be driving the controller for legacy keyboards. The handover is a
 * flag pair in an extended capability; a laptop BIOS will not let go without it. */
static void bios_handoff(xhci_t *x, uint32_t hcc1)
{
	uint32_t off = (hcc1 >> 16) << 2;
	for (int guard = 0; off && guard < 64; guard++) {
		uint32_t cap = rd32(x->cap, off);
		if ((cap & 0xFF) == 1) {
			wr32(x->cap, off, cap | (1u << 24));
			uint64_t t = tsc();
			while (rd32(x->cap, off) & (1u << 16)) {
				if (tsc() - t > 1000 * TSC_PER_MS) {
					kprintf(KERN_WARN "xhci: bios kept ownership, taking it\n");
					wr32(x->cap, off, (rd32(x->cap, off) & ~(1u << 16)) | (1u << 24));
					break;
				}
				cpu_relax();
			}
			/* no more SMIs from this controller */
			wr32(x->cap, off + 4, (rd32(x->cap, off + 4) & 0x000E1FEEu) | 0xE0000000u);
			return;
		}
		uint32_t next = (cap >> 8) & 0xFF;
		off = next ? off + (next << 2) : 0;
	}
}

/* A real controller keeps state in host pages it asks for; without them it faults on
 * the first command. QEMU asks for none. */
static int scratchpad_setup(xhci_t *x)
{
	uint32_t h2 = rd32(x->cap, CAP_HCSPARAMS2);
	uint32_t n = ((h2 >> 21) & 0x1F) << 5 | ((h2 >> 27) & 0x1F);
	if (!n) return 0;
	uint64_t arr_phys;
	uint64_t *arr = dma_page(&arr_phys);
	if (!arr || n > 512) return -1;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t ph;
		if (!dma_page(&ph)) return -1;
		arr[i] = ph;
	}
	x->dcbaa[0] = arr_phys;
	kprintf(KERN_INFO "xhci: %u scratchpad pages\n", n);
	return 0;
}

static int wait_clear(xhci_t *x, uint32_t off, uint32_t mask, uint32_t ms)
{
	uint64_t t = tsc();
	while (rd32(x->op, off) & mask) {
		if (tsc() - t > ms * TSC_PER_MS) return -1;
		cpu_relax();
	}
	return 0;
}


static int ring_has_event(xhci_t *x)
{
	return (x->erst_seg[x->evt_idx].control & TRB_C) == x->evt_cycle;
}

static void usbd_thread(void)
{
	xhci_t *x = &g_xhci;
	usbd_task = task_current();
	for (;;) {
		usbd_idle = 1;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		task_sleep_until(kernel_ticks + TIMER_HZ);
		usbd_idle = 0;

		mutex_lock(&x->lock);
		event_t ev;
		while (evt_next(x, &ev, 0))
			;                              /* evt_next routed what mattered */
		uint64_t ports = __atomic_exchange_n(&x->port_pending, 0, __ATOMIC_SEQ_CST);
		for (uint32_t i = 0; i < x->nports && i < MAX_PORTS; i++)
			if (ports >> i & 1) port_service(x, i);
		for (int i = 0; i < MAX_DEVS; i++) {
			usb_dev_t *d = &g_devs[i];
			for (int k = 0; d->used && !d->gone && k < d->nhid; k++)
				if (d->hid[k].needs_reset) hid_recover(d, &d->hid[k]);
			if (d->used && d->is_hub && d->hub_change) {
				uint32_t ch = __atomic_exchange_n(&d->hub_change, 0, __ATOMIC_SEQ_CST);
				for (uint32_t p = 1; p <= d->hub_ports; p++)
					if (ch >> p & 1) hub_port(d, p);
			}
		}
		mutex_unlock(&x->lock);

		/* Partitions of a disk just plugged in: reads take the lock again. */
		for (int i = 0; i < MAX_DISKS; i++) {
			usb_disk_t *k = &g_disk[i];
			if (!k->probe || !__atomic_exchange_n(&k->probe, 0, __ATOMIC_SEQ_CST)) continue;
			if (gpt_probe(&k->bd) != 0) mbr_probe(&k->bd);
		}

		uint16_t c = rep_code;
		if (c && (int64_t)(kernel_ticks - rep_at) >= 0) {
			rep_at = kernel_ticks + REP_EVERY_MS * TIMER_HZ / 1000;
			input_usb_key(c, 1);
		}
	}
}

/* From the timer, once a tick: one read of the ring head. */
static void xhci_tick(void)
{
	xhci_t *x = &g_xhci;
	if (!usbd_idle || !usbd_task) return;
	if (ring_has_event(x) || (rep_code && (int64_t)(kernel_ticks - rep_at) >= 0))
		task_wake(usbd_task);
}

/* Port state straight from PORTSC (xHCI 1.2, 5.4.8). Only read, so the
 * write-1-to-clear change bits stay as the controller set them. */
static void xhci_diag(hw_out_t *o)
{
	xhci_t *x = &g_xhci;
	uint32_t conn = 0, oca = 0, occ = 0;
	for (uint32_t i = 0; i < x->nports; i++) {
		uint32_t psc = rd32(x->op, OP_PORTS + i * 0x10);
		if (psc == 0xFFFFFFFFu) continue;
		conn += !!(psc & PORTSC_CCS);
		oca  += !!(psc & PORTSC_OCA);
		occ  += !!(psc & PORTSC_OCC);
	}
	hw_begin(o, "usb", "xhci0");
	hw_ks(o, "driver", "xhci");
	hw_kv(o, "ports", x->nports);
	hw_kv(o, "connected", conn);
	hw_kv(o, "overcurrent_now", oca);
	hw_kv(o, "overcurrent_seen", occ);
	hw_kv(o, "xfer_recoveries", x->n_recover);
	hw_end(o);
	for (uint32_t i = 0; i < x->nports; i++) {
		uint32_t psc = rd32(x->op, OP_PORTS + i * 0x10);
		if (psc == 0xFFFFFFFFu || !(psc & (PORTSC_CCS | PORTSC_OCA | PORTSC_OCC))) continue;
		char name[12] = "port";
		uint32_t n = i + 1, k = 4;
		if (n >= 100) name[k++] = (char)('0' + n / 100);
		if (n >= 10)  name[k++] = (char)('0' + (n / 10) % 10);
		name[k++] = (char)('0' + n % 10);
		name[k] = 0;
		hw_begin(o, "usb", name);
		hw_kv(o, "connected", !!(psc & PORTSC_CCS));
		hw_kv(o, "enabled", !!(psc & PORTSC_PED));
		hw_kv(o, "powered", !!(psc & PORTSC_PP));
		hw_kv(o, "speed_id", (psc >> 10) & 0xF);
		if (i < MAX_PORTS && x->pinfo[i].vid) {
			hw_kv(o, "vid", x->pinfo[i].vid);
			hw_kv(o, "pid", x->pinfo[i].pid);
			hw_kv(o, "class", x->pinfo[i].cls);
		}
		usb_dev_t *ud = dev_on_port(i);
		if (ud) {
			hw_kv(o, "disk", ud->is_disk);
			for (int k = 0; k < ud->nhid; k++)
				hw_ks(o, "hid", ud->hid[k].kind == HID_KBD ? "keyboard" : ud->hid[k].kind == HID_HUB ? "hub" : "mouse");
		}
		hw_kv(o, "overcurrent_now", !!(psc & PORTSC_OCA));
		hw_kv(o, "overcurrent_seen", !!(psc & PORTSC_OCC));
		hw_end(o);
	}
}

void xhci_init(void)
{
	uint8_t bus, dev, fn;
	if (pci_find_class(0x0C, 0x03, 0x30, &bus, &dev, &fn) != 0)
		return;   /* no xHCI controller */

	uint16_t cmd = pci_read16(bus, dev, fn, PCI_COMMAND);
	pci_write16(bus, dev, fn, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER);

	uint64_t bar = pci_read32(bus, dev, fn, PCI_BAR0) & ~0xFUL;
	uint64_t barhi = pci_read32(bus, dev, fn, PCI_BAR1);
	bar |= barhi << 32;
	if (!bar) return;

	xhci_t *x = &g_xhci;
	x->lock = (mutex_t)MUTEX_INIT;
	x->cap = ioremap(bar, 0x10000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);

	uint32_t caplen = rd32(x->cap, CAP_CAPLENGTH) & 0xFF;
	x->op = x->cap + caplen;
	x->rt = x->cap + (rd32(x->cap, CAP_RTSOFF) & ~0x1Fu);
	x->db = (volatile uint32_t *)(x->cap + (rd32(x->cap, CAP_DBOFF) & ~0x3u));

	uint32_t hcs1 = rd32(x->cap, CAP_HCSPARAMS1);
	x->maxslots = hcs1 & 0xFF;
	uint32_t hcc1 = rd32(x->cap, CAP_HCCPARAMS1);
	x->ctxsize = (hcc1 & (1u << 2)) ? 64 : 32;

	bios_handoff(x, hcc1);

	/* reset */
	uint32_t sts = rd32(x->op, OP_USBSTS);
	if (!(sts & USBSTS_HCH)) {
		wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~USBCMD_RS);
		uint64_t t = tsc();
		while (!(rd32(x->op, OP_USBSTS) & USBSTS_HCH) && tsc() - t < 100 * TSC_PER_MS)
			cpu_relax();
	}
	wr32(x->op, OP_USBCMD, USBCMD_HCRST);
	if (wait_clear(x, OP_USBCMD, USBCMD_HCRST, 1000) != 0 ||
	    wait_clear(x, OP_USBSTS, USBSTS_CNR, 1000) != 0) {
		kprintf(KERN_ERR "xhci: controller did not come out of reset\n");
		return;
	}

	/* DCBAA */
	x->dcbaa = dma_page(&x->dcbaa_phys);
	if (!x->dcbaa || scratchpad_setup(x) != 0) return;
	wr32(x->op, OP_CONFIG, x->maxslots);
	wr64(x->op, OP_DCBAAP, x->dcbaa_phys);

	/* command ring */
	if (ring_init(&x->cmd) != 0) return;
	wr64(x->op, OP_CRCR, x->cmd.phys | 1);   /* RCS=1 */

	/* event ring: one segment + 1-entry ERST */
	x->erst_seg = dma_page(&x->erst_seg_phys);
	x->erst = dma_page(&x->erst_phys);
	if (!x->erst_seg || !x->erst) return;
	x->evt_idx = 0;
	x->evt_cycle = 1;
	x->erst[0] = x->erst_seg_phys;
	x->erst[1] = EVT_TRBS;
	wr32(x->rt, RT_IR0 + IR_ERSTSZ, 1);
	wr64(x->rt, RT_IR0 + IR_ERDP, x->erst_seg_phys);
	wr64(x->rt, RT_IR0 + IR_ERSTBA, x->erst_phys);

	/* run */
	wr32(x->op, OP_USBCMD, USBCMD_RS);
	uint64_t t = tsc();
	while ((rd32(x->op, OP_USBSTS) & USBSTS_HCH) && tsc() - t < 100 * TSC_PER_MS)
		cpu_relax();
	if (rd32(x->op, OP_USBSTS) & USBSTS_HCH) {
		kprintf(KERN_ERR "xhci: controller did not start\n");
		return;
	}

	kprintf(KERN_INFO "xhci: hc up, %u slots, ctx %u\n", x->maxslots, x->ctxsize);

	uint32_t nports = (hcs1 >> 24) & 0xFF;
	x->nports = nports;
	hwdiag_register(xhci_diag);
	for (uint32_t i = 0; i < nports; i++) {
		uint32_t psc = rd32(x->op, OP_PORTS + i * 0x10);
		if (!(psc & PORTSC_PP))
			wr32(x->op, OP_PORTS + i * 0x10, (psc & PORTSC_KEEP) | PORTSC_PP);
	}
	delay_ms(200);       /* USB 3 links train on their own after the reset */

	/* every connected port: a disk may sit behind a webcam in port order */
	for (uint32_t i = 0; i < nports; i++)
		if (rd32(x->op, OP_PORTS + i * 0x10) & PORTSC_CCS)
			port_bringup(x, i);

	proc_create("usbd", usbd_thread);
	tick_register(xhci_tick);
}

#else
void xhci_init(void) {}
#endif
