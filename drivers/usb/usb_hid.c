/*
 * USB keyboards and mice (HID boot and report protocol), fed from interrupt
 * endpoints that stay queued all the time.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

/* ---- HID: keyboards and mice ---- */


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
