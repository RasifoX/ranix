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
 */

#if defined(__x86_64__)

/* capability registers */
#define CAP_CAPLENGTH   0x00   /* byte 0; HCIVERSION at byte 2 */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* operational registers (relative to op base) */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTS        0x400   /* PORTSC[i] at 0x400 + i*0x10 */

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)  /* controller not ready */

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_OCA      (1u << 3)   /* over-current active */
#define PORTSC_OCC      (1u << 20)  /* over-current change (write 1 to clear) */
#define PORTSC_CSC      (1u << 17)  /* connect status change */
#define PORTSC_CHANGES  (0x7Fu << 17)   /* every write-1-to-clear change bit */
/* What a write to PORTSC may carry back. PED and the change bits are write-1-to-clear,
 * so echoing a read would disable the port. */
#define PORTSC_KEEP     (PORTSC_PP | (3u << 14) | (7u << 25))

/* runtime / interrupter 0 (relative to rt base) */
#define RT_IR0          0x20
#define IR_IMAN         0x00
#define IR_IMOD         0x04
#define IR_ERSTSZ       0x08
#define IR_ERSTBA       0x10
#define IR_ERDP         0x18

/* TRB types */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_RESET_EP        14
#define TRB_EVALUATE_CTX    13
#define TRB_STOP_EP         15
#define TRB_SET_TR_DEQ      16
#define TRB_EVT_TRANSFER    32
#define TRB_EVT_CMD_COMP    33
#define TRB_EVT_PORT_CHANGE 34

#define TRB_C     (1u << 0)        /* cycle bit */
#define TRB_ENT   (1u << 1)
#define TRB_ISP   (1u << 2)
#define TRB_CH    (1u << 4)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3F)
#define TRB_GET_CC(s)    (((s) >> 24) & 0xFF)   /* completion code in status */
#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13

/* Timeouts in milliseconds; a bridge can take seconds on its first command. */
#define CMD_MS    2000u
#define XFER_MS   15000u
#define CTRL_MS   3000u

typedef struct {
	uint32_t lo, hi, status, control;
} trb_t;

#define RING_TRBS  256
#define EVT_TRBS   256
#define MAX_DISKS  16           /* names usb0..: a pulled disk's name is reused once nothing holds it */
#define MAX_DEVS   24
#define MAX_HID    3            /* interfaces per device */
#define MAX_PORTS  64
#define DATA_ORDER 5
#define DATA_BYTES (4096u << DATA_ORDER)

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
extern volatile uint64_t kernel_ticks;
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

static int ring_init(ring_t *r)
{
	void *p = dma_page(&r->phys);
	if (!p) return -1;
	r->trb = p;
	r->idx = 0;
	r->cycle = 1;
	/* link TRB at the end loops back to the start (toggle cycle) */
	trb_t *link = &r->trb[RING_TRBS - 1];
	link->lo = (uint32_t)r->phys;
	link->hi = (uint32_t)(r->phys >> 32);
	link->status = 0;
	link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | r->cycle;
	return 0;
}

/* push a TRB onto a transfer/command ring, advancing past the link TRB. A chained
 * TD crossing the link needs the link chained as well. Returns the TRB's address. */
static uint64_t ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t ctrl)
{
	trb_t *t = &r->trb[r->idx];
	uint64_t at = r->phys + (uint64_t)r->idx * sizeof(trb_t);
	t->lo = (uint32_t)param;
	t->hi = (uint32_t)(param >> 32);
	t->status = status;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	t->control = (ctrl & ~TRB_C) | r->cycle;
	r->idx++;
	if (r->idx == RING_TRBS - 1) {
		/* hit the link TRB: flip its cycle, wrap */
		trb_t *link = &r->trb[RING_TRBS - 1];
		link->control = (link->control & ~(TRB_C | TRB_CH)) | r->cycle | (ctrl & TRB_CH);
		r->idx = 0;
		r->cycle ^= 1;
	}
	return at;
}

typedef struct {
	uint64_t ptr;
	uint32_t status, control;
} event_t;

/* Events nobody is waiting for: HID reports and port changes, wherever they turn up. */
static int event_route(xhci_t *x, const event_t *ev)
{
	uint32_t type = TRB_GET_TYPE(ev->control);
	if (type == TRB_EVT_PORT_CHANGE) {
		uint32_t port = (uint32_t)(ev->ptr >> 24) & 0xFF;
		if (port >= 1 && port <= MAX_PORTS)
			__atomic_fetch_or(&x->port_pending, 1ULL << (port - 1), __ATOMIC_SEQ_CST);
		return 1;
	}
	if (type != TRB_EVT_TRANSFER) return 0;
	uint32_t slot = ev->control >> 24, dci = (ev->control >> 16) & 0x1F;
	for (int i = 0; i < MAX_DEVS; i++) {
		usb_dev_t *d = &g_devs[i];
		if (!d->used || d->slot != slot) continue;
		for (int k = 0; k < d->nhid; k++)
			if (d->hid[k].dci == dci) {
				hid_event(d, &d->hid[k], ev->status);
				return 1;
			}
	}
	return 0;
}

/* The next event of any type: 1, or 0 when ms passes with none. */
static int evt_next(xhci_t *x, event_t *ev, uint32_t ms)
{
	uint64_t t0 = tsc();
	for (;;) {
		volatile trb_t *e = &x->erst_seg[x->evt_idx];   /* DMA-written */
		uint32_t ctrl = e->control;
		if ((ctrl & TRB_C) == x->evt_cycle) {
			__atomic_thread_fence(__ATOMIC_ACQUIRE);   /* order body reads after C */
			ev->ptr = e->lo | ((uint64_t)e->hi << 32);
			ev->status = e->status;
			ev->control = ctrl;
			x->evt_idx++;
			if (x->evt_idx == EVT_TRBS) { x->evt_idx = 0; x->evt_cycle ^= 1; }
			uint64_t erdp = x->erst_seg_phys + x->evt_idx * sizeof(trb_t);
			wr64(x->rt, RT_IR0 + IR_ERDP, erdp | (1u << 3));   /* EHB clear */
			if (event_route(x, ev)) continue;
			return 1;
		}
		if (tsc() - t0 > ms * TSC_PER_MS) return 0;
		cpu_relax();
	}
}

static void ring_doorbell(xhci_t *x, uint32_t slot, uint32_t target)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	x->db[slot] = target;
}

/* run the command ring, ring the host doorbell, wait for the command
 * completion event; returns CC and the slot id from the event. */
static int cmd_exec(xhci_t *x, uint64_t param, uint32_t ctrl, uint8_t *out_slot)
{
	uint64_t at = ring_push(&x->cmd, param, 0, ctrl);
	ring_doorbell(x, 0, 0);
	event_t ev;
	while (evt_next(x, &ev, CMD_MS)) {
		if (TRB_GET_TYPE(ev.control) != TRB_EVT_CMD_COMP || ev.ptr != at) continue;
		if (out_slot) *out_slot = (uint8_t)(ev.control >> 24);
		return (int)TRB_GET_CC(ev.status);
	}
	return -1;
}

/* ---- input context helpers ---- */

static uint32_t *ctx_slot(usb_dev_t *d)          /* input slot context */
{
	return (uint32_t *)(d->in_ctx + d->x->ctxsize);       /* after input-control ctx */
}
static uint32_t *ctx_ep(usb_dev_t *d, int dci)   /* input endpoint context, DCI 1.. */
{
	/* layout: control(0), slot(1), EP-DCI-1(2), EP-DCI-2(3)... */
	return (uint32_t *)(d->in_ctx + d->x->ctxsize * (dci + 1));
}
static uint32_t *ctx_ctrl(usb_dev_t *d)          /* input control context */
{
	return (uint32_t *)d->in_ctx;
}

/* ---- control transfer (default control pipe, EP0) ---- */

/* one SETUP/DATA/STATUS control transfer; data goes through d->dma. dir:
 * 1 = device-to-host (IN). returns 0 on success. */
static int ctrl_xfer(usb_dev_t *d, uint8_t rtype, uint8_t req, uint16_t val,
                     uint16_t idx, uint16_t len, int dir)
{
	xhci_t *x = d->x;
	uint64_t setup = (uint64_t)rtype | ((uint64_t)req << 8) |
	                 ((uint64_t)val << 16) | ((uint64_t)idx << 32) |
	                 ((uint64_t)len << 48);
	uint32_t trt = len ? (dir ? 3 : 2) : 0;     /* transfer type in status field */
	ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
	if (len)
		ring_push(&d->ep0, d->dma_phys, len,
		          TRB_TYPE(TRB_DATA) | (dir ? TRB_ISP : 0) | ((uint32_t)dir << 16));
	/* status stage: opposite direction, IOC */
	uint64_t last = ring_push(&d->ep0, 0, 0,
	                          TRB_TYPE(TRB_STATUS) | TRB_IOC | ((dir ? 0u : 1u) << 16));
	ring_doorbell(x, d->slot, 1);              /* EP0 = DCI 1 */

	event_t ev;
	while (evt_next(x, &ev, CTRL_MS)) {
		if (TRB_GET_TYPE(ev.control) != TRB_EVT_TRANSFER) continue;
		if ((ev.control >> 24) != d->slot || ((ev.control >> 16) & 0x1F) != 1) continue;
		uint32_t cc = TRB_GET_CC(ev.status);
		if (ev.ptr != last && cc == CC_SHORT) continue;  /* a short data stage, then status */
		if (cc == CC_SUCCESS || cc == CC_SHORT) return 0;
		/* a stalled request: the pipe clears on the next SETUP after a reset */
		cmd_exec(x, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)d->slot << 24) | (1u << 16), 0);
		cmd_exec(x, (d->ep0.phys + (uint64_t)d->ep0.idx * sizeof(trb_t)) | d->ep0.cycle,
		         TRB_TYPE(TRB_SET_TR_DEQ) | ((uint32_t)d->slot << 24) | (1u << 16), 0);
		return -1;
	}
	return -1;
}

/* ---- bulk transfer (mass storage) ---- */

#define XFER_OK      0
#define XFER_STALL  (-1)
#define XFER_ERROR  (-2)   /* timeout or a transaction error: the endpoint needs a stop */

/* Queue one TD and wait for it. A TRB may not cross a 64 KB boundary, so a long
 * buffer becomes a chain. *got is how many bytes moved. */
static int bulk_xfer(usb_dev_t *d, int in, uint64_t phys, uint32_t len, uint32_t *got)
{
	xhci_t *x = d->x;
	ring_t *r = in ? &d->ep_in : &d->ep_out;
	uint32_t mps = in ? d->mps_in : d->mps_out;
	int dci = in ? d->dci_in : d->dci_out;
	uint64_t td[4];
	int ntd = 0;
	uint32_t left = len;
	uint64_t p = phys;
	do {
		uint32_t n = 0x10000u - (uint32_t)(p & 0xFFFF);
		if (n > left) n = left;
		left -= n;
		uint32_t tds = (left + mps - 1) / mps;       /* packets after this TRB */
		if (tds > 31) tds = 31;
		uint32_t ctrl = TRB_TYPE(TRB_NORMAL) | (in ? TRB_ISP : 0) | (left ? TRB_CH : TRB_IOC);
		uint64_t at = ring_push(r, p, n | tds << 17, ctrl);
		if (ntd < 4) td[ntd++] = at;
		p += n;
	} while (left);
	ring_doorbell(x, d->slot, (uint32_t)dci);

	uint32_t moved = 0;
	int short_seen = 0;
	event_t ev;
	/* A short packet is reported at the TRB it hit; the controller may then report the
	 * end of the TD too, or not. Wait briefly so a late event does not meet the next TD. */
	while (evt_next(x, &ev, short_seen ? 5 : XFER_MS)) {
		if (TRB_GET_TYPE(ev.control) != TRB_EVT_TRANSFER) continue;
		if ((ev.control >> 24) != d->slot || ((ev.control >> 16) & 0x1F) != (uint32_t)dci) continue;
		int mine = 0;
		for (int i = 0; i < ntd; i++) if (ev.ptr == td[i]) mine = 1;
		if (!mine) continue;
		uint32_t cc = TRB_GET_CC(ev.status), rem = ev.status & 0xFFFFFF;
		if (cc == CC_STALL) return XFER_STALL;
		if (cc != CC_SUCCESS && cc != CC_SHORT) return XFER_ERROR;
		if (ev.ptr != td[ntd - 1]) {                 /* short inside the chain */
			short_seen = 1;
			/* what the TRBs before it carried, less what this one lacked */
			uint32_t sum = 0;
			uint64_t q = phys;
			uint32_t l = len;
			for (int i = 0; i < ntd; i++) {
				uint32_t n = 0x10000u - (uint32_t)(q & 0xFFFF);
				if (n > l) n = l;
				if (ev.ptr == td[i]) { sum += n > rem ? n - rem : 0; break; }
				sum += n; q += n; l -= n;
			}
			moved = sum;
			continue;
		}
		if (!short_seen) moved = len > rem ? len - rem : 0;
		*got = moved;
		return XFER_OK;
	}
	if (short_seen) {
		*got = moved;
		return XFER_OK;
	}
	return XFER_ERROR;
}

/* After a stall the endpoint is Halted, after a timeout it still holds our TD: either
 * way move its dequeue pointer past what we queued, and clear the device side. */
static void ep_recover(usb_dev_t *d, int in, int stalled)
{
	xhci_t *x = d->x;
	x->n_recover++;
	ring_t *r = in ? &d->ep_in : &d->ep_out;
	uint32_t dci = (uint32_t)(in ? d->dci_in : d->dci_out);
	uint32_t ids = (uint32_t)d->slot << 24 | dci << 16;
	cmd_exec(x, 0, TRB_TYPE(stalled ? TRB_RESET_EP : TRB_STOP_EP) | ids, 0);
	cmd_exec(x, (r->phys + (uint64_t)r->idx * sizeof(trb_t)) | r->cycle,
	         TRB_TYPE(TRB_SET_TR_DEQ) | ids, 0);
	ctrl_xfer(d, 0x02, 1, 0, in ? d->addr_in : d->addr_out, 0, 0);
}

/* ---- BOT (bulk-only transport) mass storage ---- */

typedef struct {
	uint32_t sig;       /* 0x43425355 'USBC' */
	uint32_t tag;
	uint32_t len;       /* data transfer length */
	uint8_t  flags;     /* bit7: 1=IN */
	uint8_t  lun;
	uint8_t  cblen;     /* command block length */
	uint8_t  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
	uint32_t sig;       /* 0x53425355 'USBS' */
	uint32_t tag;
	uint32_t residue;
	uint8_t  status;
} __attribute__((packed)) csw_t;

#define BOT_OK      0
#define BOT_CHECK   1       /* the device reports a failure: ask for sense */
#define BOT_FAIL   (-1)     /* the transport lost its place, recovery has been run */

static void bot_reset(usb_dev_t *d)
{
	ctrl_xfer(d, 0x21, 0xFF, 0, d->iface, 0, 0);
	ctrl_xfer(d, 0x02, 1, 0, d->addr_in, 0, 0);
	ctrl_xfer(d, 0x02, 1, 0, d->addr_out, 0, 0);
}

static void bot_abort(usb_dev_t *d, int in, int rc)
{
	ep_recover(d, in, rc == XFER_STALL);
	bot_reset(d);
}

/* run one SCSI command via BOT: write CBW, optional data stage in d->data, read CSW.
 * want_all: anything short of dlen is a failure. */
static int bot_scsi(usb_dev_t *d, const uint8_t *cdb, int cdblen, int data_in,
                    uint32_t dlen, int want_all)
{
	cbw_t *cbw = (cbw_t *)d->dma;
	uint32_t tag = ++d->tag;
	memset(cbw, 0, sizeof(*cbw));
	cbw->sig = 0x43425355;
	cbw->tag = tag;
	cbw->len = dlen;
	cbw->flags = data_in ? 0x80 : 0;
	cbw->lun = 0;
	cbw->cblen = (uint8_t)cdblen;
	memcpy(cbw->cb, cdb, (size_t)cdblen);
	uint32_t got = 0;
	int rc = bulk_xfer(d, 0, d->dma_phys, 31, &got);
	if (rc != XFER_OK) { bot_abort(d, 0, rc); return BOT_FAIL; }

	uint32_t moved = 0;
	if (dlen) {
		rc = bulk_xfer(d, data_in, d->data_phys, dlen, &moved);
		if (rc == XFER_STALL) ep_recover(d, data_in, 1);   /* the CSW still follows */
		else if (rc != XFER_OK) { bot_abort(d, data_in, rc); return BOT_FAIL; }
	}

	csw_t *csw = (csw_t *)(d->dma + 64);
	memset(csw, 0, sizeof(*csw));
	rc = bulk_xfer(d, 1, d->dma_phys + 64, 13, &got);
	if (rc == XFER_STALL) {
		ep_recover(d, 1, 1);
		rc = bulk_xfer(d, 1, d->dma_phys + 64, 13, &got);
	}
	if (rc != XFER_OK) { bot_abort(d, 1, rc); return BOT_FAIL; }
	if (got != 13 || csw->sig != 0x53425355 || csw->tag != tag || csw->status > 1) {
		bot_reset(d);
		return BOT_FAIL;
	}
	if (csw->status) return BOT_CHECK;
	if (want_all && (moved != dlen || csw->residue)) return BOT_FAIL;
	return BOT_OK;
}

/* A command with the retries a real bridge needs: a unit attention after reset, or
 * not ready while the drive behind it wakes. */
static int usb_scsi(usb_dev_t *d, const uint8_t *cdb, int cdblen, int data_in,
                    uint32_t dlen, int want_all)
{
	for (int t = 0; t < 8; t++) {
		int rc = bot_scsi(d, cdb, cdblen, data_in, dlen, want_all);
		if (rc == BOT_OK) return 0;
		if (rc == BOT_FAIL) {
			if (t >= 2) return -1;
			continue;
		}
		uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
		if (bot_scsi(d, rs, 6, 1, 18, 0) != BOT_OK) return -1;
		uint8_t key = d->data[2] & 0xF;
		if (key != 2 && key != 6) return -1;   /* only NOT READY and UNIT ATTENTION pass */
		delay_ms(key == 2 ? 200 : 10);
	}
	return -1;
}

/* One transfer of nb device blocks, in or out of d->data. */
static int usb_rw(usb_dev_t *d, int write, uint64_t blk, uint32_t nb)
{
	uint8_t cdb[16];
	int len;
	memset(cdb, 0, sizeof(cdb));
	if ((blk + nb) >> 32) {
		cdb[0] = write ? 0x8A : 0x88;          /* READ/WRITE(16), past 2 TB of 512s */
		for (int i = 0; i < 8; i++) cdb[2 + i] = (uint8_t)(blk >> (56 - 8 * i));
		cdb[12] = (uint8_t)(nb >> 8);
		cdb[13] = (uint8_t)nb;
		len = 16;
	} else {
		cdb[0] = write ? 0x2A : 0x28;
		cdb[2] = (uint8_t)(blk >> 24);
		cdb[3] = (uint8_t)(blk >> 16);
		cdb[4] = (uint8_t)(blk >> 8);
		cdb[5] = (uint8_t)blk;
		cdb[7] = (uint8_t)(nb >> 8);
		cdb[8] = (uint8_t)nb;
		len = 10;
	}
	return usb_scsi(d, cdb, len, !write, nb * d->blk, 1);
}

/* The block layer counts 512-byte sectors, a bridge may show 4 KB blocks. */
static int usb_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	usb_disk_t *k = dev->priv;
	uint8_t *out = buf;
	int rc = 0;
	mutex_lock(&g_xhci.lock);
	usb_dev_t *d = k->dev;
	if (!d) { mutex_unlock(&g_xhci.lock); return -1; }
	uint32_t spb = d->blk / 512;
	while (count) {
		uint32_t in = (uint32_t)(lba % spb);
		uint64_t want = in + count;
		uint32_t max = DATA_BYTES / 512;
		uint32_t secs = want > max ? max : (uint32_t)want;
		uint32_t nb = (secs + spb - 1) / spb;
		if (nb * d->blk > DATA_BYTES) nb = DATA_BYTES / d->blk;
		if (usb_rw(d, 0, lba / spb, nb) != 0) { rc = -1; break; }
		uint64_t have = (uint64_t)nb * spb - in;
		if (have > count) have = count;
		memcpy(out, d->data + (uint64_t)in * 512, (size_t)have * 512);
		out += have * 512; lba += have; count -= have;
	}
	mutex_unlock(&g_xhci.lock);
	return rc;
}

static int usb_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	usb_disk_t *k = dev->priv;
	const uint8_t *in = buf;
	int rc = 0;
	mutex_lock(&g_xhci.lock);
	usb_dev_t *d = k->dev;
	if (!d) { mutex_unlock(&g_xhci.lock); return -1; }
	uint32_t spb = d->blk / 512;
	if ((lba | count) & (spb - 1)) { mutex_unlock(&g_xhci.lock); return -1; }   /* no partial device block */
	while (count) {
		uint32_t secs = count > DATA_BYTES / 512 ? DATA_BYTES / 512 : (uint32_t)count;
		secs -= secs % spb;
		memcpy(d->data, in, (size_t)secs * 512);
		if (usb_rw(d, 1, lba / spb, secs / spb) != 0) { rc = -1; break; }
		in += (uint64_t)secs * 512; lba += secs; count -= secs;
	}
	mutex_unlock(&g_xhci.lock);
	return rc;
}

/* ---- enumeration ---- */

/* set up an endpoint context for a bulk endpoint. ep_addr is the USB endpoint
 * address (bit7=dir). returns the DCI. */
static int setup_bulk_ep(usb_dev_t *d, uint8_t ep_addr, uint16_t mps, uint8_t burst, ring_t *r)
{
	int num = ep_addr & 0x0F;
	int in = (ep_addr & 0x80) ? 1 : 0;
	int dci = num * 2 + in;
	if (ring_init(r) != 0) return -1;
	uint32_t *ep = ctx_ep(d, dci);
	uint32_t ep_type = in ? 6 : 2;        /* bulk IN=6, bulk OUT=2 */
	ep[0] = 0;
	ep[1] = (ep_type << 3) | (3u << 1) |  /* CErr=3 */
	        ((uint32_t)burst << 8) | ((uint32_t)mps << 16);   /* MaxBurst, MaxPacket */
	ep[2] = (uint32_t)(r->phys | r->cycle);   /* TR dequeue ptr lo + DCS */
	ep[3] = (uint32_t)(r->phys >> 32);
	ep[4] = 3072;                         /* average TRB length for bandwidth estimates */
	return dci;
}

static void string_serial(usb_dev_t *d, uint8_t index)
{
	if (!index) return;
	/* first language id, then the string in it; ASCII is what a serial holds */
	if (ctrl_xfer(d, 0x80, 6, 0x0300, 0, 4, 1) != 0) return;
	uint16_t lang = (uint16_t)(d->dma[2] | d->dma[3] << 8);
	if (ctrl_xfer(d, 0x80, 6, (uint16_t)(0x0300 | index), lang, 254, 1) != 0) return;
	uint32_t n = d->dma[0];
	char s[47];
	uint32_t o = 0;
	if (n > 254) n = 254;
	for (uint32_t i = 2; i + 1 < n && o < sizeof(s); i += 2)
		s[o++] = d->dma[i + 1] ? '_' : (char)d->dma[i];
	block_copy_serial(d->serial, sizeof(d->serial), s, o);
}

/* Bring the configured device up as a disk. 0, or -1 with the device left unused. */
static int scsi_attach(usb_dev_t *d)
{
	uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
	if (usb_scsi(d, inq, 6, 1, 36, 0) != 0) {
		kprintf(KERN_ERR "xhci: inquiry failed\n");
		return -1;
	}
	if ((d->data[0] & 0x1F) != 0) {              /* peripheral type 0: a disk */
		kprintf(KERN_INFO "xhci: lun 0 is not a disk (type %u)\n", d->data[0] & 0x1F);
		return -1;
	}

	uint8_t tur[6] = {0};
	for (int t = 0; t < 25; t++) {               /* five seconds for a drive to wake */
		if (usb_scsi(d, tur, 6, 0, 0, 0) == 0) break;
		delay_ms(200);
	}

	uint8_t rc10[10] = {0x25};
	if (usb_scsi(d, rc10, 10, 1, 8, 1) != 0) {
		kprintf(KERN_ERR "xhci: read capacity failed\n");
		return -1;
	}
	const uint8_t *b = d->data;
	uint64_t last = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	uint32_t bsz  = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
	if (last == 0xFFFFFFFFu) {                   /* too big for ten bytes */
		uint8_t rc16[16] = {0x9E, 0x10};
		rc16[13] = 32;
		if (usb_scsi(d, rc16, 16, 1, 32, 0) != 0) return -1;
		last = 0;
		for (int i = 0; i < 8; i++) last = last << 8 | b[i];
		bsz = (uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
	}
	if ((bsz != 512 && bsz != 1024 && bsz != 2048 && bsz != 4096) || !last) {
		kprintf(KERN_ERR "xhci: unusable capacity, block size %u\n", bsz);
		return -1;
	}
	d->blk = bsz;

	/* a free name: registered and held by nothing, else never registered */
	usb_disk_t *k = NULL;
	for (int i = 0; i < MAX_DISKS && !k; i++)
		if (g_disk[i].registered && !g_disk[i].dev && !g_disk[i].bd.users) k = &g_disk[i];
	for (int i = 0; i < MAX_DISKS && !k; i++)
		if (!g_disk[i].registered) k = &g_disk[i];
	if (!k) {
		kprintf(KERN_WARN "xhci: all %d disk names are held\n", MAX_DISKS);
		return -1;
	}
	int nr = (int)(k - g_disk);
	block_device_t *bd = &k->bd;
	bd->size_blocks = (last + 1) * (bsz / 512);
	bd->lba_size = bsz;
	memcpy(bd->serial, d->serial, sizeof(bd->serial));
	d->disk = k;
	k->dev = d;
	if (k->registered) {
		block_device_rebind(bd);
		if (bd->ro) kprintf(KERN_INFO "block: %s read-only until allowed\n", bd->name);
	} else {
		int n = 0;
		bd->name[n++] = 'u'; bd->name[n++] = 's'; bd->name[n++] = 'b';
		if (nr >= 10) bd->name[n++] = (char)('0' + nr / 10);
		bd->name[n++] = (char)('0' + nr % 10);
		bd->name[n] = '\0';
		bd->read  = usb_read;
		bd->write = usb_write;
		bd->priv  = k;
		block_device_register(bd);
		k->registered = 1;
	}
	k->probe = 1;
	kprintf(KERN_INFO "xhci: %s %lu sectors (bsz %u)\n", bd->name,
	        (unsigned long)bd->size_blocks, bsz);
	return 0;
}

/* Find a BOT interface in the configuration descriptor at d->dma. */
typedef struct {
	int     found, uas_only;
	uint8_t iface;
	uint8_t ep_in, ep_out;
	uint16_t mps_in, mps_out;
	uint8_t burst_in, burst_out;
} msc_desc_t;

static void cfg_parse(const uint8_t *c, uint32_t total, msc_desc_t *m)
{
	int in_bot = 0, last_in = 0, any_uas = 0;
	memset(m, 0, sizeof(*m));
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {            /* interface */
			uint8_t cls = c[i + 5], sub = c[i + 6], proto = c[i + 7];
			in_bot = cls == 8 && sub == 6 && proto == 0x50 && c[i + 3] == 0 && !m->found;
			if (in_bot) { m->found = 1; m->iface = c[i + 2]; }
			if (cls == 8 && proto == 0x62) any_uas = 1;
		} else if (btype == 5 && blen >= 7 && in_bot) {   /* endpoint */
			uint8_t addr = c[i + 2];
			uint16_t mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			if ((c[i + 3] & 3) == 2) {
				last_in = addr & 0x80;
				if (last_in) { m->ep_in = addr; m->mps_in = mps; }
				else { m->ep_out = addr; m->mps_out = mps; }
			}
		} else if (btype == 0x30 && blen >= 6 && in_bot) {  /* SS endpoint companion */
			if (last_in) m->burst_in = c[i + 2];
			else m->burst_out = c[i + 2];
		}
		i += blen;
	}
	m->uas_only = any_uas && !m->found;
	if (!m->ep_in || !m->ep_out || !m->mps_in || !m->mps_out) m->found = 0;
}

/* Disk slots are filled in order; a device that is not one gives its slot back so the
 * next port can use the same memory. */
static void dev_release(usb_dev_t *d)
{
	xhci_t *x = d->x;
	if (d->slot) {
		cmd_exec(x, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)d->slot << 24), 0);
		x->dcbaa[d->slot] = 0;
	}
	dma_free(&d->dev_ctx_phys);
	dma_free(&d->in_ctx_phys);
	dma_free(&d->ep0.phys);
	dma_free(&d->ep_in.phys);
	dma_free(&d->ep_out.phys);
	dma_free(&d->dma_phys);
	for (int i = 0; i < d->nhid; i++) dma_free(&d->hid[i].ring.phys);
	if (d->data_phys) buddy_free(d->data_phys, DATA_ORDER);
	memset(d, 0, sizeof(*d));
}

/* ---- HID: keyboards and mice ---- */

extern void input_usb_key(uint16_t code, int down);
extern void input_usb_mouse(int dx, int dy, int wheel, int buttons);

#define E0 0x100
/* HID usage 0x04.. to scancode set 1, E0 for the extended ones */
static const uint16_t hid_sc[] = {
	0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,   /* a-m */
	0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* n-z */
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                     /* 1-0 */
	0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28,
	0x29, 0x33, 0x34, 0x35, 0x3A,                                                   /* caps */
	0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,         /* F1-12 */
	E0 | 0x37, 0x46, 0, E0 | 0x52, E0 | 0x47, E0 | 0x49, E0 | 0x53, E0 | 0x4F,
	E0 | 0x51, E0 | 0x4D, E0 | 0x4B, E0 | 0x50, E0 | 0x48,                          /* arrows */
	0x45, E0 | 0x35, 0x37, 0x4A, 0x4E, E0 | 0x1C,                                   /* keypad */
	0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53, 0x56, E0 | 0x5D,
};
static const uint16_t hid_mod[8] = {
	0x1D, 0x2A, 0x38, E0 | 0x5B, E0 | 0x1D, 0x36, E0 | 0x38, E0 | 0x5C
};

static uint16_t usage_code(uint8_t u)
{
	if (u < 4 || u - 4u >= sizeof(hid_sc) / sizeof(hid_sc[0])) return 0;
	return hid_sc[u - 4];
}

static void key_change(uint16_t code, int down)
{
	if (!code) return;
	input_usb_key(code, down);
	if (down) {
		rep_at = kernel_ticks + REP_DELAY_MS * TIMER_HZ / 1000;
		rep_code = code;
	} else if (rep_code == code) {
		rep_code = 0;
	}
}

static int in_report(const uint8_t *r, uint8_t u)
{
	for (int i = 2; i < 8; i++)
		if (r[i] == u) return 1;
	return 0;
}

static void kbd_report(hid_ep_t *h, const uint8_t *r)
{
	if (r[2] == 1) return;                 /* too many keys held: nothing is known */
	for (int i = 0; i < 8; i++)
		if ((r[0] ^ h->prev[0]) & (1u << i))
			input_usb_key(hid_mod[i], (r[0] >> i) & 1);
	for (int i = 2; i < 8; i++)
		if (h->prev[i] && !in_report(r, h->prev[i]))
			key_change(usage_code(h->prev[i]), 0);
	for (int i = 2; i < 8; i++)
		if (r[i] && !in_report(h->prev, r[i]))
			key_change(usage_code(r[i]), 1);
	for (int i = 0; i < 8; i++) h->prev[i] = r[i];
}

static int32_t get_bits(const uint8_t *r, uint32_t len, uint32_t off, uint32_t size)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < size && i < 32; i++) {
		uint32_t b = off + i;
		if (b / 8 < len && (r[b / 8] >> (b % 8) & 1)) v |= 1u << i;
	}
	if (size && size < 32 && (v >> (size - 1) & 1)) v |= ~0u << size;
	return (int32_t)v;
}

static void mouse_report(hid_ep_t *h, const uint8_t *r, uint32_t len)
{
	mouse_map_t *m = &h->map;
	if (m->id && (!len || r[0] != m->id)) return;
	int b = 0;
	for (int i = 0; i < m->nbtn && i < 3; i++)
		b |= (get_bits(r, len, m->btn + (uint32_t)i, 1) & 1) << i;
	int dx = get_bits(r, len, m->x, m->xs), dy = get_bits(r, len, m->y, m->ys);
	int w = m->ws ? get_bits(r, len, m->w, m->ws) : 0;
	h->buttons = (uint8_t)b;
	input_usb_mouse(dx, -dy, w, b);
}

static void hid_queue(usb_dev_t *d, hid_ep_t *h)
{
	ring_push(&h->ring, h->buf_phys, h->len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
	ring_doorbell(d->x, d->slot, h->dci);
}

static void hid_event(usb_dev_t *d, hid_ep_t *h, uint32_t status)
{
	uint32_t cc = TRB_GET_CC(status);
	if (cc != CC_SUCCESS && cc != CC_SHORT) {
		h->needs_reset = 1;            /* commands wait: this may be inside one */
		return;
	}
	h->resets = 0;
	uint32_t got = h->len - (status & 0xFFFFFF);
	if (got > h->len) got = 0;
	if (h->kind == HID_HUB) {
		uint32_t m = 0;
		for (uint32_t i = 0; i < got && i < 4; i++) m |= (uint32_t)h->buf[i] << (i * 8);
		__atomic_fetch_or(&d->hub_change, m, __ATOMIC_SEQ_CST);
	} else if (h->kind == HID_KBD) {
		if (got >= 8) kbd_report(h, h->buf);
	} else {
		mouse_report(h, h->buf, got);
	}
	hid_queue(d, h);
}

/* A mouse's report descriptor: walk the items to where X, Y, the wheel and the buttons
 * sit. Only input fields under a Mouse application collection count. 1 when found. */
static int mouse_parse(const uint8_t *p, uint32_t n, mouse_map_t *m)
{
	uint32_t page = 0, rsize = 0, rcount = 0, id = 0, off[16] = {0};
	uint32_t usage[8], nu = 0, umin = 0, umax = 0;
	int depth = 0, mouse_at = -1, have_x = 0;
	memset(m, 0, sizeof(*m));
	for (uint32_t i = 0; i < n; ) {
		uint8_t it = p[i++];
		if (it == 0xFE) {              /* long item */
			if (i + 1 >= n) break;
			i += 2u + p[i];
			continue;
		}
		uint32_t sz = (it & 3) == 3 ? 4 : (it & 3), v = 0;
		if (i + sz > n) break;
		for (uint32_t k = 0; k < sz; k++) v |= (uint32_t)p[i + k] << (k * 8);
		i += sz;
		uint8_t type = (it >> 2) & 3, tag = it >> 4;
		if (type == 1) {               /* global */
			if (tag == 0) page = v;
			else if (tag == 7) rsize = v;
			else if (tag == 8) id = v;
			else if (tag == 9) rcount = v;
		} else if (type == 2) {        /* local */
			if (tag == 0 && nu < 8) usage[nu++] = v;
			else if (tag == 1) umin = v;
			else if (tag == 2) umax = v;
		} else if (type == 0) {        /* main */
			if (tag == 10) {           /* collection */
				if (v == 1 && page == 1 && nu && usage[0] == 2 && mouse_at < 0)
					mouse_at = depth;
				depth++;
			} else if (tag == 12) {    /* end collection */
				depth--;
				if (depth == mouse_at) {
					if (have_x) return 1;
					mouse_at = -1;
				}
			} else if (tag == 8) {     /* input */
				uint32_t *o = &off[id & 15];
				uint32_t base = *o + (id ? 8 : 0);
				int cnst = v & 1, rel = (v >> 2) & 1;
				if (mouse_at >= 0 && !cnst) {
					if (page == 9 && !m->nbtn) {
						m->btn = (uint16_t)base;
						m->nbtn = (uint8_t)(rcount > 8 ? 8 : rcount);
						m->id = (uint8_t)id;
					} else if (page == 1 && rel) {
						for (uint32_t k = 0; k < rcount; k++) {
							uint32_t u = k < nu ? usage[k] :
							             (umin && umin + k <= umax ? umin + k : 0);
							uint32_t at = base + k * rsize;
							if (u == 0x30) { m->x = (uint16_t)at; m->xs = (uint8_t)rsize; have_x = 1; m->id = (uint8_t)id; }
							else if (u == 0x31) { m->y = (uint16_t)at; m->ys = (uint8_t)rsize; }
							else if (u == 0x38) { m->w = (uint16_t)at; m->ws = (uint8_t)rsize; }
						}
					}
				}
				*o += rsize * rcount;
			}
			nu = 0; umin = umax = 0;
		}
	}
	return have_x;
}

/* The HID interfaces of a configuration. Non-boot interfaces wait for a look at their
 * report descriptor: most of them are a keyboard's media keys, which are left alone. */
static void hid_scan(usb_dev_t *d, const uint8_t *c, uint32_t total)
{
	hid_ep_t *h = 0;
	for (uint32_t i = 0; i + 2 <= total && d->nhid < MAX_HID; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {
			h = 0;
			if (c[i + 5] == 3 && c[i + 3] == 0) {
				h = &d->hid[d->nhid];
				memset(h, 0, sizeof(*h));
				h->iface = c[i + 2];
				h->proto = c[i + 7];
				h->kind = h->proto == 1 ? HID_KBD : HID_MOUSE;
			}
		} else if (btype == 5 && blen >= 7 && h && (c[i + 2] & 0x80) && (c[i + 3] & 3) == 3) {
			h->addr = c[i + 2];
			h->mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			h->interval = c[i + 6];
			if (h->mps) d->nhid++;
			h = 0;
		}
		i += blen;
	}
}

static int setup_int_ep(usb_dev_t *d, hid_ep_t *h, uint32_t speed)
{
	int dci = (h->addr & 0x0F) * 2 + 1;
	if (ring_init(&h->ring) != 0) return -1;
	uint32_t iv = h->interval, e = 0;
	if (speed >= 3) {
		e = iv ? iv - 1 : 0;           /* already an exponent of 125 us */
		if (e > 15) e = 15;
	} else {
		iv = iv ? iv * 8 : 8;          /* ms, in 125 us */
		while ((2u << e) <= iv) e++;
		if (e < 3) e = 3;
		if (e > 10) e = 10;
	}
	uint32_t *ep = ctx_ep(d, dci);
	ep[0] = e << 16;
	ep[1] = (7u << 3) | (3u << 1) | ((uint32_t)h->mps << 16);   /* interrupt IN, CErr 3 */
	ep[2] = (uint32_t)(h->ring.phys | h->ring.cycle);
	ep[3] = (uint32_t)(h->ring.phys >> 32);
	ep[4] = (uint32_t)h->mps | ((uint32_t)h->mps << 16);        /* average, max ESIT */
	h->dci = (uint8_t)dci;
	return dci;
}

/* After SET_CONFIGURATION: name each interface, open its endpoint, start listening.
 * Returns how many are in use. */
static int hid_attach(usb_dev_t *d, uint32_t speed)
{
	xhci_t *x = d->x;
	int keep = 0;
	for (int i = 0; i < d->nhid; i++) {
		hid_ep_t src = d->hid[i], *h = &d->hid[keep];
		if (src.kind == HID_KBD) {
			ctrl_xfer(d, 0x21, 0x0B, 0, src.iface, 0, 0);   /* boot protocol */
			ctrl_xfer(d, 0x21, 0x0A, 0, src.iface, 0, 0);   /* report on change only */
		} else {
			/* the descriptor's length is in the HID descriptor, but 1024 is plenty
			 * and a short answer is fine */
			memset(d->dma, 0, 1024);
			int ok = ctrl_xfer(d, 0x81, 6, 0x2200, src.iface, 1024, 1) == 0 &&
			         mouse_parse(d->dma, 1024, &src.map);
			if (!ok) {
				/* boot protocol has no wheel, and a media interface has no X: only
				 * trust it on an interface that says it is a boot mouse */
				if (src.proto != 2 || src.mps < 3) continue;
				if (ctrl_xfer(d, 0x21, 0x0B, 0, src.iface, 0, 0) != 0) continue;
				src.map = (mouse_map_t){.nbtn = 3, .btn = 0, .x = 8, .xs = 8,
				                        .y = 16, .ys = 8};
			}
			ctrl_xfer(d, 0x21, 0x0A, 0, src.iface, 0, 0);
		}
		src.buf = d->dma + 2048 + (uint32_t)keep * 512;
		src.buf_phys = d->dma_phys + 2048 + (uint32_t)keep * 512;
		src.len = src.mps > 512 ? 512 : src.mps;
		*h = src;
		keep++;
		kprintf(KERN_INFO "xhci: port %d %s\n", d->port + 1,
		        h->kind == HID_KBD ? "keyboard" : "mouse");
	}
	d->nhid = (uint8_t)keep;
	if (!keep) return 0;

	uint32_t *ic = ctx_ctrl(d), *sc = ctx_slot(d);
	int maxdci = 1;
	memset(ic, 0, x->ctxsize);
	ic[1] = 1u;
	for (int i = 0; i < keep; i++) {
		int dci = setup_int_ep(d, &d->hid[i], speed);
		if (dci < 0) return -1;
		ic[1] |= 1u << dci;
		if (dci > maxdci) maxdci = dci;
	}
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)maxdci << 27);
	if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf(KERN_ERR "xhci: configure hid failed\n");
		return -1;
	}
	for (int i = 0; i < keep; i++) hid_queue(d, &d->hid[i]);
	return keep;
}

static usb_dev_t *dev_alloc(void)
{
	for (int i = 0; i < MAX_DEVS; i++)
		if (!g_devs[i].used) return &g_devs[i];
	return 0;
}

/* The status endpoint of a hub that has just been configured. */
static int hub_attach(usb_dev_t *d, const uint8_t *c, uint32_t total)
{
	xhci_t *x = d->x;
	hid_ep_t *h = &d->hid[0];
	memset(h, 0, sizeof(*h));
	h->kind = HID_HUB;
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 5 && blen >= 7 && (c[i + 2] & 0x80) && (c[i + 3] & 3) == 3) {
			h->addr = c[i + 2];
			h->mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			h->interval = c[i + 6];
			break;
		}
		i += blen;
	}
	if (!h->mps) {
		kprintf(KERN_ERR "xhci: hub has no status endpoint, cfg %u\n", total);
		return -1;
	}
	uint8_t ss = d->speed >= 4;
	if (ctrl_xfer(d, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, 12, 1) != 0) {
		kprintf(KERN_ERR "xhci: hub descriptor failed\n");
		return -1;
	}
	d->hub_ports = d->dma[2];
	uint32_t good = (uint32_t)d->dma[5] * 2;
	if (d->hub_ports > 31) d->hub_ports = 31;

	uint32_t *ic = ctx_ctrl(d), *sc = ctx_slot(d);
	memset(ic, 0, x->ctxsize);
	int dci = setup_int_ep(d, h, d->speed);
	if (dci < 0) return -1;
	ic[1] = 1u | 1u << dci;
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | (uint32_t)dci << 27 | 1u << 26;   /* a hub */
	sc[1] = (sc[1] & 0x00FFFFFFu) | (uint32_t)d->hub_ports << 24;
	int cc = cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24), 0);
	if (cc != CC_SUCCESS) {
		kprintf(KERN_ERR "xhci: configure hub failed, cc %d\n", cc);
		return -1;
	}
	if (ss) ctrl_xfer(d, 0x20, 12, d->depth, 0, 0, 0);       /* SET_HUB_DEPTH */
	for (uint32_t p = 1; p <= d->hub_ports; p++)
		ctrl_xfer(d, 0x23, 3, 8, (uint16_t)p, 0, 0);         /* PORT_POWER */
	delay_ms(good < 100 ? 100 : good);
	h->buf = d->dma + 2048;
	h->buf_phys = d->dma_phys + 2048;
	h->len = h->mps > 512 ? 512 : h->mps;
	d->nhid = 1;
	d->is_hub = 1;
	hid_queue(d, h);
	kprintf(KERN_INFO "xhci: port %d hub, %u ports\n", d->port + 1, d->hub_ports);
	return 0;
}

static int enumerate(xhci_t *x, int port, usb_dev_t *parent, int hub_port, uint32_t speed)
{
	usb_dev_t *d = dev_alloc();
	if (!d) return 0;
	memset(d, 0, sizeof(*d));
	d->x = x;
	d->used = 1;
	d->port = (uint8_t)port;
	d->speed = (uint8_t)speed;
	d->parent = parent;
	d->hub_port = (uint8_t)hub_port;
	if (parent) {
		d->depth = parent->depth + 1;
		d->route = parent->route | (uint32_t)(hub_port > 15 ? 15 : hub_port) << (parent->depth * 4);
		if (speed < 3) {            /* full or low behind a high speed hub: its TT */
			d->tt_slot = parent->speed == 3 ? parent->slot : parent->tt_slot;
			d->tt_port = parent->speed == 3 ? (uint8_t)hub_port : parent->tt_port;
		}
	}

	/* full speed may be 8 to 64: start from 8 and ask */
	uint16_t mps0 = (speed >= 4) ? 512 : speed == 3 ? 64 : 8;

	/* enable a slot */
	uint8_t slot = 0;
	if (cmd_exec(x, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot ||
	    slot > x->maxslots) {
		kprintf(KERN_ERR "xhci: enable slot failed\n");
		d->used = 0;
		return -1;
	}
	d->slot = slot;

	/* device context + input context */
	d->dev_ctx = dma_page(&d->dev_ctx_phys);
	d->in_ctx  = dma_page(&d->in_ctx_phys);
	d->dma     = dma_page(&d->dma_phys);
	if (!d->dev_ctx || !d->in_ctx || !d->dma || ring_init(&d->ep0) != 0) goto fail;
	x->dcbaa[slot] = d->dev_ctx_phys;

	/* input control context: enable A0 (slot) + A1 (EP0) */
	uint32_t *ic = ctx_ctrl(d);
	ic[1] = (1u << 0) | (1u << 1);

	/* slot context: root hub port number, context entries=1, speed */
	uint32_t *sc = ctx_slot(d);
	sc[0] = (1u << 27) | (speed << 20) | d->route;   /* context entries=1, speed */
	sc[1] = ((uint32_t)(port + 1)) << 16;        /* root hub port number (1-based) */
	sc[2] = (uint32_t)d->tt_slot | (uint32_t)d->tt_port << 8;

	/* EP0 (default control pipe), DCI 1 */
	uint32_t *ep0 = ctx_ep(d, 1);
	ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)mps0 << 16);   /* type=control(4) */
	ep0[2] = (uint32_t)(d->ep0.phys | d->ep0.cycle);
	ep0[3] = (uint32_t)(d->ep0.phys >> 32);
	ep0[4] = 8;

	if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf(KERN_ERR "xhci: address device failed on port %d\n", port + 1);
		goto fail;
	}
	delay_ms(10);

	if (speed < 3) {
		if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 8, 1) != 0) {
			kprintf(KERN_ERR "xhci: first read failed on port %d\n", port + 1);
			goto fail;
		}
		uint16_t real = d->dma[7];
		if (real != mps0 && real >= 8 && real <= 64) {
			ic[0] = 0;
			ic[1] = 1u << 1;
			ep0[1] = (ep0[1] & 0xFFFFu) | ((uint32_t)real << 16);
			if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_EVALUATE_CTX) | ((uint32_t)slot << 24), 0)
			    != CC_SUCCESS)
				goto fail;
		}
	}

	/* GET_DESCRIPTOR(device, 18) */
	if (ctrl_xfer(d, 0x80, 6, 0x0100, 0, 18, 1) != 0) {
		kprintf(KERN_ERR "xhci: get device desc failed\n");
		goto fail;
	}
	uint8_t *dd = d->dma;
	uint16_t vid = (uint16_t)(dd[8] | dd[9] << 8);
	uint16_t pid = (uint16_t)(dd[10] | dd[11] << 8);
	uint8_t  serial_idx = dd[16], devclass = dd[4];
	kprintf(KERN_INFO "xhci: port %d device %04x:%04x class %u\n", port + 1, vid, pid, dd[4]);
	if (port < MAX_PORTS && !parent) {
		x->pinfo[port].vid = vid;
		x->pinfo[port].pid = pid;
		x->pinfo[port].cls = dd[4];
	}

	/* GET_DESCRIPTOR(config, 9) then full length */
	if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, 9, 1) != 0) goto fail;
	uint16_t total = (uint16_t)(d->dma[2] | d->dma[3] << 8);
	if (total < 9) goto fail;
	if (total > 4096) total = 4096;
	if (ctrl_xfer(d, 0x80, 6, 0x0200, 0, total, 1) != 0) goto fail;
	uint8_t cfgval = d->dma[5];

	/* only a disk is configured: anything else on the bus stays as we found it */
	if (devclass == 9) {
		if (d->depth >= 5) goto fail;       /* the spec's limit */
		/* the descriptor is in dma, which the requests below overwrite */
		static uint8_t cfg[512];
		uint32_t cn = total > sizeof cfg ? sizeof cfg : total;
		memcpy(cfg, d->dma, cn);
		if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) {
			kprintf(KERN_ERR "xhci: hub set config failed\n");
			goto fail;
		}
		if (hub_attach(d, cfg, cn) != 0) goto fail;
		return 0;
	}

	msc_desc_t m;
	cfg_parse(d->dma, total, &m);
	if (!m.found) {
		if (m.uas_only)
			kprintf(KERN_WARN "xhci: port %d disk offers UAS only, not supported\n", port + 1);
		hid_scan(d, d->dma, total);
		if (!d->nhid) goto fail;
		if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) goto fail;
		if (hid_attach(d, speed) <= 0) goto fail;
		return 0;
	}
	void *dp = buddy_alloc(DATA_ORDER);
	if (!dp) goto fail;
	d->data_phys = (uint64_t)dp;
	d->data = (uint8_t *)PHYS_TO_VIRT(d->data_phys);
	d->iface = m.iface;
	d->addr_in = m.ep_in;
	d->addr_out = m.ep_out;
	d->mps_in = m.mps_in;
	d->mps_out = m.mps_out;

	string_serial(d, serial_idx);

	/* SET_CONFIGURATION */
	if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) {
		kprintf(KERN_ERR "xhci: set config failed\n");
		goto fail;
	}

	/* configure bulk endpoints via CONFIGURE_ENDPOINT */
	memset(ic, 0, x->ctxsize);
	int dci_in  = setup_bulk_ep(d, m.ep_in, m.mps_in, m.burst_in, &d->ep_in);
	int dci_out = setup_bulk_ep(d, m.ep_out, m.mps_out, m.burst_out, &d->ep_out);
	if (dci_in < 0 || dci_out < 0) goto fail;
	int maxdci = dci_in > dci_out ? dci_in : dci_out;
	ic[1] = (1u << 0) | (1u << dci_in) | (1u << dci_out);   /* A0 + the two EPs */
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)maxdci << 27);

	if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf(KERN_ERR "xhci: configure ep failed\n");
		goto fail;
	}
	d->dci_in = dci_in;
	d->dci_out = dci_out;

	if (scsi_attach(d) != 0) goto fail;
	d->is_disk = 1;
	return 0;

fail:
	dev_release(d);
	return -1;
}

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

/* Reset what is on a port and let it enumerate. */
static void port_bringup(xhci_t *x, uint32_t i)
{
	uint32_t reg = OP_PORTS + i * 0x10;
	uint32_t psc = rd32(x->op, reg);
	wr32(x->op, reg, (psc & PORTSC_KEEP) | PORTSC_CSC);
	if (i < MAX_PORTS) x->port_tried |= 1ULL << i;

	wr32(x->op, reg, (psc & PORTSC_KEEP) | PORTSC_PR);
	uint64_t pt = tsc();
	for (;;) {
		psc = rd32(x->op, reg);
		if ((psc & PORTSC_PRC) || tsc() - pt > 500 * TSC_PER_MS) break;
		cpu_relax();
	}
	wr32(x->op, reg, (psc & PORTSC_KEEP) | PORTSC_PRC);
	delay_ms(20);
	psc = rd32(x->op, reg);
	if (!(psc & PORTSC_PED)) return;       /* not enabled */

	if (enumerate(x, (int)i, 0, 0, (psc >> 10) & 0xF) != 0)
		kprintf(KERN_DEBUG "xhci: port %u left unused\n", i + 1);
}

static usb_dev_t *dev_on_port(uint32_t i)
{
	for (int k = 0; k < MAX_DEVS; k++)
		if (g_devs[k].used && !g_devs[k].gone && !g_devs[k].parent && g_devs[k].port == i)
			return &g_devs[k];
	return 0;
}

static void dev_unplug(usb_dev_t *d)
{
	kprintf(KERN_INFO "xhci: port %d unplugged\n", d->port + 1);
	for (int k = 0; d->is_hub && k < MAX_DEVS; k++)
		if (g_devs[k].used && !g_devs[k].gone && g_devs[k].parent == d)
			dev_unplug(&g_devs[k]);
	for (int i = 0; i < d->nhid; i++) {
		hid_ep_t *h = &d->hid[i];
		if (h->kind == HID_KBD) {      /* nothing may stay held down */
			static const uint8_t none[8];
			kbd_report(h, none);
		} else if (h->buttons) {
			input_usb_mouse(0, 0, 0, 0);
		}
	}
	if (d->disk) {                     /* the name stays, reading nothing */
		usb_disk_t *k = d->disk;
		k->dev = NULL;
		k->probe = 0;
		block_device_rebind(&k->bd);   /* partitions empty, protection back */
		k->bd.size_blocks = 0;
		kprintf(KERN_INFO "xhci: %s pulled%s\n", k->bd.name,
		        k->bd.users ? ", still mounted or open" : "");
	}
	dev_release(d);
}

/* One port of a hub, after its status endpoint named it. */
static void hub_port(usb_dev_t *d, uint32_t p)
{
	uint8_t bit = 0;
	if (ctrl_xfer(d, 0xA3, 0, 0, (uint16_t)p, 4, 1) != 0) return;
	uint32_t st = d->dma[0] | (uint32_t)d->dma[1] << 8;
	uint32_t ch = d->dma[2] | (uint32_t)d->dma[3] << 8;
	for (bit = 0; bit < 5; bit++)
		if (ch >> bit & 1) ctrl_xfer(d, 0x23, 1, (uint16_t)(16 + bit), (uint16_t)p, 0, 0);

	usb_dev_t *c = 0;
	for (int k = 0; k < MAX_DEVS; k++)
		if (g_devs[k].used && !g_devs[k].gone && g_devs[k].parent == d && g_devs[k].hub_port == p)
			c = &g_devs[k];
	if (c && (!(st & 1) || (ch & 1))) {
		dev_unplug(c);
		c = 0;
		d->hub_tried &= ~(1u << p);
	}
	if (!(st & 1)) {
		d->hub_tried &= ~(1u << p);
		return;
	}
	if (c || (d->hub_tried >> p & 1)) return;
	d->hub_tried |= 1u << p;
	delay_ms(100);

	ctrl_xfer(d, 0x23, 3, 4, (uint16_t)p, 0, 0);     /* PORT_RESET */
	for (int t = 0; ; t++) {
		delay_ms(10);
		if (ctrl_xfer(d, 0xA3, 0, 0, (uint16_t)p, 4, 1) != 0 || t > 50) return;
		if (d->dma[2] & 0x10) break;
	}
	st = d->dma[0] | (uint32_t)d->dma[1] << 8;
	ctrl_xfer(d, 0x23, 1, 20, (uint16_t)p, 0, 0);    /* C_PORT_RESET */
	if (!(st & 2)) return;
	uint32_t speed = d->speed >= 4 ? 4 : st & 0x200 ? 2 : st & 0x400 ? 3 : 1;
	delay_ms(20);
	if (enumerate(d->x, d->port, d, (int)p, speed) != 0)
		kprintf(KERN_DEBUG "xhci: hub port %u left unused\n", p);
}

static void port_service(xhci_t *x, uint32_t i)
{
	uint32_t reg = OP_PORTS + i * 0x10;
	uint32_t psc = rd32(x->op, reg);
	uint32_t changed = psc & PORTSC_CHANGES;
	if (changed) wr32(x->op, reg, (psc & PORTSC_KEEP) | changed);
	usb_dev_t *d = dev_on_port(i);
	if (d && (!(psc & PORTSC_CCS) || (changed & PORTSC_CSC))) {
		dev_unplug(d);
		d = 0;
		x->port_tried &= ~(1ULL << i);
	}
	if (!(psc & PORTSC_CCS)) {
		x->port_tried &= ~(1ULL << i);
		x->pinfo[i].vid = x->pinfo[i].pid = 0;
		return;
	}
	if (d || (x->port_tried >> i & 1)) return;
	delay_ms(100);                         /* contacts settle */
	if (rd32(x->op, reg) & PORTSC_CCS) port_bringup(x, i);
}

/* An endpoint that failed and whose device is still here: clear the halt and go on. */
static void hid_recover(usb_dev_t *d, hid_ep_t *h)
{
	xhci_t *x = d->x;
	h->needs_reset = 0;
	if (++h->resets > 8) return;           /* it means it */
	uint32_t sl = (uint32_t)d->slot << 24, ep = (uint32_t)h->dci << 16;
	cmd_exec(x, 0, TRB_TYPE(TRB_RESET_EP) | sl | ep, 0);
	cmd_exec(x, (h->ring.phys + (uint64_t)h->ring.idx * sizeof(trb_t)) | h->ring.cycle,
	         TRB_TYPE(TRB_SET_TR_DEQ) | sl | ep, 0);
	x->n_recover++;
	hid_queue(d, h);
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
	extern void tick_register(void (*fn)(void));
	tick_register(xhci_tick);
}

#else
void xhci_init(void) {}
#endif
