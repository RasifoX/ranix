/*
 * Rings, the event ring and controller commands: ring_push, evt_next (which hands
 * HID events to their handler on the way), the doorbells, cmd_exec, and the
 * device and input contexts.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

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
