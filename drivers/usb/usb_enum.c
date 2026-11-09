/*
 * Enumeration: a new device gets a slot, an address and a configuration, and
 * becomes a disk, a keyboard or mouse, or a hub. dev_release undoes it.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

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

/* A new device's place in the tree: root port, hub, route string, and the
 * transaction translator a full or low speed device behind a high speed hub uses. */
static void dev_setup(usb_dev_t *d, xhci_t *x, int port, usb_dev_t *parent, int hub_port, uint32_t speed)
{
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
}

/* Give the slot its contexts and an address, and learn EP0's real packet size.
 * 0 on success; on failure the caller releases the device. */
static int dev_address(usb_dev_t *d, int port, uint32_t speed)
{
	xhci_t *x = d->x;
	uint8_t slot = d->slot;
	/* full speed may be 8 to 64: start from 8 and ask */
	uint16_t mps0 = (speed >= 4) ? 512 : speed == 3 ? 64 : 8;

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
	return 0;
fail:
	return -1;
}

/* The device is a Bulk-Only disk (m): take its endpoints, configure it and
 * attach the SCSI LUN. 0 on success; on failure the caller releases it. */
static int disk_attach(usb_dev_t *d, const msc_desc_t *mp, uint8_t cfgval, uint8_t serial_idx)
{
	xhci_t *x = d->x;
	uint8_t slot = d->slot;
	const msc_desc_t m = *mp;
	uint32_t *ic = ctx_ctrl(d), *sc = ctx_slot(d);
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
	return -1;
}

static int enumerate(xhci_t *x, int port, usb_dev_t *parent, int hub_port, uint32_t speed)
{
	usb_dev_t *d = dev_alloc();
	if (!d) return 0;
	dev_setup(d, x, port, parent, hub_port, speed);

	/* enable a slot */
	uint8_t slot = 0;
	if (cmd_exec(x, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot ||
	    slot > x->maxslots) {
		kprintf(KERN_ERR "xhci: enable slot failed\n");
		d->used = 0;
		return -1;
	}
	d->slot = slot;

	if (dev_address(d, port, speed) != 0) goto fail;

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
	if (disk_attach(d, &m, cfgval, serial_idx) != 0) goto fail;
	return 0;

fail:
	dev_release(d);
	return -1;
}
