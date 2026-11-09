/*
 * Transfers: control transfers on EP0, bulk transfers, and recovery of a halted
 * endpoint.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

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
