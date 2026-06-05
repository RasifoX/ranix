#include <kernel/pci.h>
#include <kernel/block.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * xHCI host controller + USB enumeration + bulk-only mass-storage. Polling,
 * single-threaded, no interrupts: each ring is drained by spinning on the
 * event ring. Targets QEMU's qemu-xhci (PCI class 0C:03:30). A mass-storage
 * LUN is exposed as a block_device ("usb0") so GPT/ext2 sit on top unchanged.
 */

#if defined(__x86_64__)

#define delay_loops(n) do { for (volatile uint32_t _i = 0; _i < (n); _i++) ; } while (0)

/* capability registers */
#define CAP_CAPLENGTH   0x00   /* byte 0; HCIVERSION at byte 2 */
#define CAP_HCSPARAMS1  0x04
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
/* write-1-to-clear status bits live in PORTSC; preserve only PP when writing */
#define PORTSC_RW1CS    ((1u<<17)|(1u<<18)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23))

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
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_EVT_TRANSFER    32
#define TRB_EVT_CMD_COMP    33
#define TRB_EVT_PORT_CHANGE 34

#define TRB_C     (1u << 0)        /* cycle bit */
#define TRB_ENT   (1u << 1)
#define TRB_ISP   (1u << 2)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3F)
#define TRB_GET_CC(s)    (((s) >> 24) & 0xFF)   /* completion code in status */
#define CC_SUCCESS 1

typedef struct {
	uint32_t lo, hi, status, control;
} trb_t;

#define RING_TRBS  256
#define EVT_TRBS   256

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

	/* per-slot working set (single device supported) */
	uint8_t   slot;
	uint64_t  in_ctx_phys;     /* input context */
	uint8_t  *in_ctx;
	uint64_t  dev_ctx_phys;    /* device context */
	uint8_t  *dev_ctx;
	ring_t    ep0;             /* control endpoint ring */
	ring_t    ep_in;           /* bulk IN */
	ring_t    ep_out;          /* bulk OUT */

	uint8_t  *dma;             /* CBW/CSW DMA bounce (4 KB) */
	uint64_t  dma_phys;
	uint8_t  *data;            /* SCSI data-stage DMA bounce (4 KB) */
	uint64_t  data_phys;
} xhci_t;

static xhci_t g_xhci;

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

static void ring_init(ring_t *r)
{
	void *p = dma_page(&r->phys);
	r->trb = p;
	r->idx = 0;
	r->cycle = 1;
	/* link TRB at the end loops back to the start (toggle cycle) */
	trb_t *link = &r->trb[RING_TRBS - 1];
	link->lo = (uint32_t)r->phys;
	link->hi = (uint32_t)(r->phys >> 32);
	link->status = 0;
	link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | r->cycle;
}

/* push a TRB onto a transfer/command ring, advancing past the link TRB */
static trb_t *ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t ctrl)
{
	trb_t *t = &r->trb[r->idx];
	t->lo = (uint32_t)param;
	t->hi = (uint32_t)(param >> 32);
	t->status = status;
	t->control = (ctrl & ~TRB_C) | r->cycle;
	r->idx++;
	if (r->idx == RING_TRBS - 1) {
		/* hit the link TRB: flip its cycle, wrap */
		trb_t *link = &r->trb[RING_TRBS - 1];
		link->control = (link->control & ~TRB_C) | r->cycle;
		r->idx = 0;
		r->cycle ^= 1;
	}
	return t;
}

/* spin on the event ring for the next event of any type; returns completion
 * code and (optionally) the event TRB pointer field. */
static int evt_wait(xhci_t *x, uint64_t *out_ptr, uint8_t *out_type)
{
	for (uint64_t g = 0; g < 2000000UL; g++) {
		volatile trb_t *e = &x->erst_seg[x->evt_idx];   /* DMA-written */
		uint32_t ctrl = e->control;
		if ((ctrl & TRB_C) != x->evt_cycle) { delay_loops(64); continue; }
		__atomic_thread_fence(__ATOMIC_ACQUIRE);   /* order body reads after C */

		uint8_t type = TRB_GET_TYPE(ctrl);
		uint8_t cc = TRB_GET_CC(e->status);
		uint64_t ptr = e->lo | ((uint64_t)e->hi << 32);

		x->evt_idx++;
		if (x->evt_idx == EVT_TRBS) { x->evt_idx = 0; x->evt_cycle ^= 1; }
		uint64_t erdp = x->erst_seg_phys + x->evt_idx * sizeof(trb_t);
		wr64(x->rt, RT_IR0 + IR_ERDP, erdp | (1u << 3));   /* EHB clear */

		if (out_ptr) *out_ptr = ptr;
		if (out_type) *out_type = type;
		return cc;
	}
	return -1;
}

static void ring_doorbell(xhci_t *x, uint32_t slot, uint32_t target)
{
	x->db[slot] = target;
}

/* run the command ring, ring the host doorbell, wait for the command
 * completion event; returns CC and the slot id from the event. */
static int cmd_exec(xhci_t *x, uint64_t param, uint32_t ctrl, uint8_t *out_slot)
{
	ring_push(&x->cmd, param, 0, ctrl);
	ring_doorbell(x, 0, 0);
	uint64_t ptr; uint8_t type;
	for (;;) {
		int cc = evt_wait(x, &ptr, &type);
		if (cc < 0) return -1;
		if (type == TRB_EVT_CMD_COMP) {
			if (out_slot) {
				/* slot id is bits 31:24 of event's control dword */
				volatile trb_t *e = &x->erst_seg[x->evt_idx ? x->evt_idx - 1 : EVT_TRBS - 1];
				*out_slot = e->control >> 24;
			}
			return cc;
		}
		/* swallow port-change / stray events */
	}
}

/* ---- input context helpers ---- */

static uint32_t *ctx_slot(xhci_t *x)          /* input slot context */
{
	return (uint32_t *)(x->in_ctx + x->ctxsize);          /* after input-control ctx */
}
static uint32_t *ctx_ep(xhci_t *x, int dci)   /* input endpoint context, DCI 1.. */
{
	/* layout: control(0), slot(1), EP-DCI-1(2), EP-DCI-2(3)... */
	return (uint32_t *)(x->in_ctx + x->ctxsize * (dci + 1));
}
static uint32_t *ctx_ctrl(xhci_t *x)          /* input control context */
{
	return (uint32_t *)x->in_ctx;
}

/* ---- control transfer (default control pipe, EP0) ---- */

/* one SETUP/DATA/STATUS control transfer; data goes through the bounce. dir:
 * 1 = device-to-host (IN). returns 0 on success. */
static int ctrl_xfer(xhci_t *x, uint8_t rtype, uint8_t req, uint16_t val,
                     uint16_t idx, uint16_t len, int dir)
{
	uint64_t setup = (uint64_t)rtype | ((uint64_t)req << 8) |
	                 ((uint64_t)val << 16) | ((uint64_t)idx << 32) |
	                 ((uint64_t)len << 48);
	uint32_t trt = len ? (dir ? 3 : 2) : 0;     /* transfer type in status field */
	ring_push(&x->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
	if (len)
		ring_push(&x->ep0, x->dma_phys, len,
		          TRB_TYPE(TRB_DATA) | (dir ? TRB_ISP : 0) | (1u << 16 /*dir IN*/ ) * dir);
	/* status stage: opposite direction, IOC */
	ring_push(&x->ep0, 0, 0,
	          TRB_TYPE(TRB_STATUS) | TRB_IOC | ((dir ? 0u : 1u) << 16));
	ring_doorbell(x, x->slot, 1);              /* EP0 = DCI 1 */

	uint8_t type;
	for (;;) {
		int cc = evt_wait(x, 0, &type);
		if (cc < 0) return -1;
		if (type == TRB_EVT_TRANSFER)
			return cc == CC_SUCCESS ? 0 : -1;
	}
}

/* ---- bulk transfer (mass storage) ---- */

static int g_dci_in, g_dci_out;

static int bulk_xfer(xhci_t *x, int in, uint64_t phys, uint32_t len)
{
	ring_t *r = in ? &x->ep_in : &x->ep_out;
	ring_push(r, phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | (in ? TRB_ISP : 0));
	ring_doorbell(x, x->slot, in ? g_dci_in : g_dci_out);
	uint8_t type;
	for (;;) {
		int cc = evt_wait(x, 0, &type);
		if (cc < 0) return -1;
		if (type == TRB_EVT_TRANSFER)
			return (cc == CC_SUCCESS || cc == 13 /* short packet */) ? 0 : -1;
	}
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

static uint32_t g_tag = 1;

/* run one SCSI command via BOT: write CBW, optional data stage, read CSW. */
static int bot_scsi(xhci_t *x, const uint8_t *cdb, int cdblen, int data_in,
                    void *data, uint32_t dlen)
{
	cbw_t *cbw = (cbw_t *)x->dma;
	memset(cbw, 0, sizeof(*cbw));
	cbw->sig = 0x43425355;
	cbw->tag = g_tag++;
	cbw->len = dlen;
	cbw->flags = data_in ? 0x80 : 0;
	cbw->lun = 0;
	cbw->cblen = cdblen;
	memcpy(cbw->cb, cdb, cdblen);
	if (bulk_xfer(x, 0, x->dma_phys, 31) != 0) return -1;
	/* QEMU's usb-storage starts the backing read asynchronously after the CBW
	 * completion event; ringing the data doorbell too early misses it. */
	delay_loops(100000);

	if (dlen) {
		if (data_in) {
			if (bulk_xfer(x, 1, x->data_phys, dlen) != 0) return -1;
			memcpy(data, x->data, dlen);
		} else {
			memcpy(x->data, data, dlen);
			if (bulk_xfer(x, 0, x->data_phys, dlen) != 0) return -1;
		}
	}

	if (bulk_xfer(x, 1, x->dma_phys, 13) != 0) return -1;
	csw_t *csw = (csw_t *)x->dma;
	if (csw->sig != 0x53425355 || csw->status != 0) return -1;
	return 0;
}

static uint64_t g_capacity;     /* in 512-byte sectors */
static uint32_t g_sector;       /* device block size */

static int usb_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	xhci_t *x = dev->priv;
	uint8_t *out = buf;
	while (count) {
		uint32_t n = count > 8 ? 8 : (uint32_t)count;   /* <=4 KB/xfer */
		uint8_t cdb[10] = {0x28, 0,
			(uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
			(uint8_t)(lba >> 8), (uint8_t)lba, 0,
			(uint8_t)(n >> 8), (uint8_t)n, 0};
		if (bot_scsi(x, cdb, 10, 1, out, n * 512u) != 0) return -1;
		out += n * 512u; lba += n; count -= n;
	}
	return 0;
}

static int usb_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	xhci_t *x = dev->priv;
	const uint8_t *in = buf;
	while (count) {
		uint32_t n = count > 8 ? 8 : (uint32_t)count;
		uint8_t cdb[10] = {0x2A, 0,
			(uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
			(uint8_t)(lba >> 8), (uint8_t)lba, 0,
			(uint8_t)(n >> 8), (uint8_t)n, 0};
		if (bot_scsi(x, cdb, 10, 0, (void *)in, n * 512u) != 0) return -1;
		in += n * 512u; lba += n; count -= n;
	}
	return 0;
}

static block_device_t g_dev;

/* ---- enumeration ---- */

/* set up an endpoint context for a bulk endpoint. ep_addr is the USB endpoint
 * address (bit7=dir). returns the DCI. */
static int setup_bulk_ep(xhci_t *x, uint8_t ep_addr, uint16_t mps, uint8_t burst, ring_t *r)
{
	int num = ep_addr & 0x0F;
	int in = (ep_addr & 0x80) ? 1 : 0;
	int dci = num * 2 + in;
	ring_init(r);
	uint32_t *ep = ctx_ep(x, dci);
	uint32_t ep_type = in ? 6 : 2;        /* bulk IN=6, bulk OUT=2 */
	ep[0] = 0;
	ep[1] = (ep_type << 3) | (3u << 1) |  /* CErr=3 */
	        ((uint32_t)burst << 8) | ((uint32_t)mps << 16);   /* MaxBurst, MaxPacket */
	ep[2] = (uint32_t)(r->phys | r->cycle);   /* TR dequeue ptr lo + DCS */
	ep[3] = (uint32_t)(r->phys >> 32);
	ep[4] = mps;                          /* avg trb length */
	return dci;
}

static int enumerate(xhci_t *x, int port)
{
	/* enable a slot */
	uint8_t slot = 0;
	if (cmd_exec(x, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot) {
		kprintf("xhci: enable slot failed\n");
		return -1;
	}
	x->slot = slot;

	/* device context + input context */
	x->dev_ctx = dma_page(&x->dev_ctx_phys);
	x->in_ctx  = dma_page(&x->in_ctx_phys);
	x->dcbaa[slot] = x->dev_ctx_phys;

	/* input control context: enable A0 (slot) + A1 (EP0) */
	uint32_t *ic = ctx_ctrl(x);
	ic[1] = (1u << 0) | (1u << 1);

	/* slot context: root hub port number, context entries=1, speed */
	uint32_t portsc = rd32(x->op, OP_PORTS + port * 0x10);
	uint32_t speed = (portsc >> 10) & 0xF;
	uint16_t mps0 = (speed >= 4) ? 512 : (speed == 3 ? 64 : 8);  /* SS=512, HS=64 */
	uint32_t *sc = ctx_slot(x);
	sc[0] = (1u << 27) | (speed << 20);          /* context entries=1, speed */
	sc[1] = ((uint32_t)(port + 1)) << 16;        /* root hub port number (1-based) */

	/* EP0 (default control pipe), DCI 1 */
	ring_init(&x->ep0);
	uint32_t *ep0 = ctx_ep(x, 1);
	ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)mps0 << 16);   /* type=control(4) */
	ep0[2] = (uint32_t)(x->ep0.phys | x->ep0.cycle);
	ep0[3] = (uint32_t)(x->ep0.phys >> 32);
	ep0[4] = 8;

	if (cmd_exec(x, x->in_ctx_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | (slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf("xhci: address device failed\n");
		return -1;
	}

	/* GET_DESCRIPTOR(device, 18) */
	if (ctrl_xfer(x, 0x80, 6, 0x0100, 0, 18, 1) != 0) {
		kprintf("xhci: get device desc failed\n");
		return -1;
	}
	uint8_t *dd = x->dma;
	uint16_t vid = dd[8] | (dd[9] << 8);
	uint16_t pid = dd[10] | (dd[11] << 8);
	kprintf("xhci: device %04x:%04x class %u\n", vid, pid, dd[4]);

	/* GET_DESCRIPTOR(config, 9) then full length */
	if (ctrl_xfer(x, 0x80, 6, 0x0200, 0, 9, 1) != 0) return -1;
	uint16_t total = x->dma[2] | (x->dma[3] << 8);
	if (total > 4096) total = 4096;
	if (ctrl_xfer(x, 0x80, 6, 0x0200, 0, total, 1) != 0) return -1;

	/* parse config: find interface (class 8 = mass storage) + bulk endpoints */
	uint8_t cfgval = x->dma[5];
	uint8_t cfgbuf[256];
	memcpy(cfgbuf, x->dma, total < sizeof(cfgbuf) ? total : sizeof(cfgbuf));
	int is_msc = 0;
	uint8_t ep_in = 0, ep_out = 0;
	uint16_t mps_in = 512, mps_out = 512;
	uint8_t burst_in = 0, burst_out = 0;
	uint8_t last_in = 0;     /* which bulk ep the next companion desc refers to */
	for (int i = 0; i + 2 < total && i + 2 < (int)sizeof(cfgbuf); ) {
		uint8_t blen = cfgbuf[i];
		uint8_t btype = cfgbuf[i + 1];
		if (blen < 2) break;
		if (btype == 4) {                 /* interface */
			is_msc = (cfgbuf[i + 5] == 8 && cfgbuf[i + 7] == 0x50); /* BOT */
		} else if (btype == 5 && is_msc) {   /* endpoint */
			uint8_t addr = cfgbuf[i + 2];
			uint8_t attr = cfgbuf[i + 3] & 3;
			uint16_t mps = cfgbuf[i + 4] | (cfgbuf[i + 5] << 8);
			if (attr == 2) {              /* bulk */
				last_in = addr & 0x80;
				if (addr & 0x80) { ep_in = addr; mps_in = mps; }
				else { ep_out = addr; mps_out = mps; }
			}
		} else if (btype == 0x30 && is_msc) {   /* SS endpoint companion */
			if (last_in) burst_in = cfgbuf[i + 2];
			else burst_out = cfgbuf[i + 2];
		}
		i += blen;
	}

	/* SET_CONFIGURATION */
	if (ctrl_xfer(x, 0x00, 9, cfgval, 0, 0, 0) != 0) {
		kprintf("xhci: set config failed\n");
		return -1;
	}
	kprintf("xhci: configured (msc=%d ep_in=%02x ep_out=%02x)\n", is_msc, ep_in, ep_out);

	if (!is_msc || !ep_in || !ep_out) return 0;   /* enumerated, not mass storage */

	/* configure bulk endpoints via CONFIGURE_ENDPOINT */
	memset(ic, 0, x->ctxsize);
	int dci_in  = setup_bulk_ep(x, ep_in, mps_in, burst_in, &x->ep_in);
	int dci_out = setup_bulk_ep(x, ep_out, mps_out, burst_out, &x->ep_out);
	int maxdci = dci_in > dci_out ? dci_in : dci_out;
	ic[1] = (1u << 0) | (1u << dci_in) | (1u << dci_out);   /* A0 + the two EPs */
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((uint32_t)maxdci << 27);

	if (cmd_exec(x, x->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | (slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf("xhci: configure ep failed\n");
		return -1;
	}
	g_dci_in = dci_in;
	g_dci_out = dci_out;

	/* SCSI: INQUIRY, then READ CAPACITY(10) */
	uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
	uint8_t inqbuf[36];
	if (bot_scsi(x, inq, 6, 1, inqbuf, 36) != 0) {
		kprintf("xhci: INQUIRY failed\n");
		return -1;
	}

	/* TEST UNIT READY / spin until ready (a few tries) */
	uint8_t tur[6] = {0};
	for (int t = 0; t < 5; t++)
		if (bot_scsi(x, tur, 6, 0, 0, 0) == 0) break;

	uint8_t rc[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	uint8_t rcbuf[8];
	if (bot_scsi(x, rc, 10, 1, rcbuf, 8) != 0) {
		kprintf("xhci: READ CAPACITY failed\n");
		return -1;
	}
	uint32_t last = (rcbuf[0] << 24) | (rcbuf[1] << 16) | (rcbuf[2] << 8) | rcbuf[3];
	uint32_t bsz  = (rcbuf[4] << 24) | (rcbuf[5] << 16) | (rcbuf[6] << 8) | rcbuf[7];
	g_sector = bsz;
	g_capacity = ((uint64_t)last + 1) * (bsz / 512);

	memcpy(g_dev.name, "usb0", 5);
	g_dev.size_blocks = g_capacity;
	g_dev.read  = usb_read;
	g_dev.write = usb_write;
	g_dev.priv  = x;
	block_device_register(&g_dev);
	kprintf("xhci: usb0 %lu sectors (bsz %u)\n",
	        (unsigned long)g_capacity, bsz);
	return 0;
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
	x->cap = ioremap(bar, 0x4000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);

	uint32_t caplen = rd32(x->cap, CAP_CAPLENGTH) & 0xFF;
	x->op = x->cap + caplen;
	x->rt = x->cap + (rd32(x->cap, CAP_RTSOFF) & ~0x1Fu);
	x->db = (volatile uint32_t *)(x->cap + (rd32(x->cap, CAP_DBOFF) & ~0x3u));

	uint32_t hcs1 = rd32(x->cap, CAP_HCSPARAMS1);
	x->maxslots = hcs1 & 0xFF;
	uint32_t hcc1 = rd32(x->cap, CAP_HCCPARAMS1);
	x->ctxsize = (hcc1 & (1u << 2)) ? 64 : 32;

	/* reset */
	uint32_t sts = rd32(x->op, OP_USBSTS);
	if (!(sts & USBSTS_HCH)) {
		wr32(x->op, OP_USBCMD, rd32(x->op, OP_USBCMD) & ~USBCMD_RS);
		for (uint64_t g = 0; g < 100000000UL; g++)
			if (rd32(x->op, OP_USBSTS) & USBSTS_HCH) break;
	}
	wr32(x->op, OP_USBCMD, USBCMD_HCRST);
	for (uint64_t g = 0; g < 100000000UL; g++)
		if (!(rd32(x->op, OP_USBCMD) & USBCMD_HCRST) &&
		    !(rd32(x->op, OP_USBSTS) & USBSTS_CNR)) break;

	/* DCBAA */
	x->dcbaa = dma_page(&x->dcbaa_phys);
	wr32(x->op, OP_CONFIG, x->maxslots);
	wr64(x->op, OP_DCBAAP, x->dcbaa_phys);

	/* command ring */
	ring_init(&x->cmd);
	wr64(x->op, OP_CRCR, x->cmd.phys | 1);   /* RCS=1 */

	/* event ring: one segment + 1-entry ERST */
	x->erst_seg = dma_page(&x->erst_seg_phys);
	x->erst = dma_page(&x->erst_phys);
	x->evt_idx = 0;
	x->evt_cycle = 1;
	x->erst[0] = x->erst_seg_phys;
	x->erst[1] = EVT_TRBS;
	wr32(x->rt, RT_IR0 + IR_ERSTSZ, 1);
	wr64(x->rt, RT_IR0 + IR_ERDP, x->erst_seg_phys);
	wr64(x->rt, RT_IR0 + IR_ERSTBA, x->erst_phys);

	/* shared DMA bounce */
	x->dma = dma_page(&x->dma_phys);
	x->data = dma_page(&x->data_phys);

	/* run */
	wr32(x->op, OP_USBCMD, USBCMD_RS);
	for (uint64_t g = 0; g < 100000000UL; g++)
		if (!(rd32(x->op, OP_USBSTS) & USBSTS_HCH)) break;

	kprintf("xhci: hc up, %u slots, ctx %u\n", x->maxslots, x->ctxsize);

	/* find a connected port, reset it, enumerate */
	uint32_t nports = (hcs1 >> 24) & 0xFF;
	for (uint32_t i = 0; i < nports; i++) {
		uint32_t psc = rd32(x->op, OP_PORTS + i * 0x10);
		if (!(psc & PORTSC_CCS)) continue;

		/* reset the port */
		uint32_t v = (psc & ~PORTSC_RW1CS) | PORTSC_PR;
		wr32(x->op, OP_PORTS + i * 0x10, v);
		for (uint64_t g = 0; g < 50000000UL; g++) {
			psc = rd32(x->op, OP_PORTS + i * 0x10);
			if (psc & PORTSC_PRC) break;
		}
		/* clear PRC */
		wr32(x->op, OP_PORTS + i * 0x10, (psc & ~PORTSC_RW1CS) | PORTSC_PRC);
		delay_loops(2000000);
		psc = rd32(x->op, OP_PORTS + i * 0x10);
		if (!(psc & PORTSC_PED)) continue;     /* not enabled */

		kprintf("xhci: port %u connected, enabled\n", i + 1);
		enumerate(x, i);
		break;
	}
}

#else
void xhci_init(void) {}
#endif
