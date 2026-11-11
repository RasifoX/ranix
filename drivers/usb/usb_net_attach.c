/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB network adapters: choosing the configuration, bringing the adapter up
 * (ECM: the data interface's second setting, the MAC string, the packet filter;
 * RNDIS: initialize, ask the MAC, set the packet filter), and letting it go.
 * Included by drivers/usb/usb_net.c, itself compiled inside drivers/usb/xhci.c.
 */

static int un_prefer_rndis(void)
{
	char b[8];
	const char *v = cmdline_get("ranix.usbnet", b, sizeof b);
	return v && strcmp(v, "rndis") == 0;
}

/* The network configuration: ECM before RNDIS (or the other way round). d->dma
 * holds configuration 0 (total bytes) and holds it again on return. */
static int usbnet_find(usb_dev_t *d, uint8_t ncfg, uint32_t total, un_desc_t *out, uint8_t *cfgval)
{
	static uint8_t cfg0[4096];
	int want = un_prefer_rndis() ? UN_RNDIS : UN_ECM, best = UN_NONE;
	memcpy(cfg0, d->dma, total);
	for (uint8_t i = 0; i < ncfg && i < 4; i++) {
		const uint8_t *c = cfg0;
		uint32_t n = total;
		if (i) {
			if (ctrl_xfer(d, 0x80, 6, (uint16_t)(0x0200 | i), 0, 9, 1) != 0) continue;
			n = (uint32_t)(d->dma[2] | d->dma[3] << 8);
			if (n < 9 || n > 4096 || ctrl_xfer(d, 0x80, 6, (uint16_t)(0x0200 | i), 0, (uint16_t)n, 1) != 0)
				continue;
			c = d->dma;
		}
		un_desc_t u;
		int k = un_match(c, n, &u);
		if (k && (!best || k == want)) {
			best = k;
			*out = u;
			*cfgval = c[5];
		}
		if (k == want) break;
	}
	if (ncfg > 1) memcpy(d->dma, cfg0, total);
	return best;
}

/* An RNDIS control message (already in d->dma, len bytes) and its reply.
 * A query's answer goes to info (at most *n bytes; *n becomes its length). */
static int un_rndis_cmd(usb_dev_t *d, const un_desc_t *u, uint32_t len, uint32_t want, uint32_t reqid,
                        uint8_t *info, uint32_t *n)
{
	if (ctrl_xfer(d, 0x21, 0x00, 0, u->ctrl_iface, (uint16_t)len, 0) != 0) return -1;   /* SEND_ENCAPSULATED */
	for (int t = 0; t < 50; t++) {
		delay_ms(t ? 10 : 1);
		memset(d->dma, 0, 1024);
		if (ctrl_xfer(d, 0xA1, 0x01, 0, u->ctrl_iface, 1024, 1) != 0) continue;           /* GET_ENCAPSULATED */
		const uint8_t *p = 0;
		uint32_t got = 0;
		if (un_rndis_reply(d->dma, 1024, want, reqid, &p, &got) != 0) continue;
		if (info) {
			if (got > *n) got = *n;
			memcpy(info, p, got);
			*n = got;
		}
		return 0;
	}
	return -1;
}

static int un_rndis_up(usb_dev_t *d, const un_desc_t *u, uint8_t mac[6])
{
	uint32_t n = 6;
	uint8_t filt[4] = { RNDIS_FILTER, 0, 0, 0 };
	const char *step = "initialize";
	if (un_rndis_cmd(d, u, un_rndis_init(d->dma, 1, UN_RXSZ), RNDIS_INIT_MSG, 1, 0, 0) != 0) goto bad;
	step = "mac address";
	if (un_rndis_cmd(d, u, un_rndis_query(d->dma, 2, OID_802_3_PERMANENT_ADDRESS, 6), RNDIS_QUERY_MSG, 2,
	                 mac, &n) != 0 || n != 6)
		goto bad;
	step = "packet filter";
	if (un_rndis_cmd(d, u, un_rndis_set(d->dma, 3, OID_GEN_CURRENT_PACKET_FILTER, filt, 4),
	                 RNDIS_SET_MSG, 3, 0, 0) == 0)
		return 0;
bad:
	kprintf(KERN_ERR "xhci: rndis %s failed\n", step);
	return -1;
}

static int un_ecm_up(usb_dev_t *d, const un_desc_t *u, uint8_t mac[6])
{
	if (u->data_alt && ctrl_xfer(d, 0x01, 0x0B, u->data_alt, u->data_iface, 0, 0) != 0) return -1;
	if (ctrl_xfer(d, 0x80, 6, 0x0300, 0, 4, 1) != 0) return -1;            /* languages */
	uint16_t lang = (uint16_t)(d->dma[2] | d->dma[3] << 8);
	if (ctrl_xfer(d, 0x80, 6, (uint16_t)(0x0300 | u->mac_index), lang, 64, 1) != 0) return -1;
	if (un_mac_from_string(d->dma, 64, mac) != 0) return -1;
	/* SET_ETHERNET_PACKET_FILTER: directed, broadcast, all multicast (NDP) */
	return ctrl_xfer(d, 0x21, 0x43, 0x000E, u->ctrl_iface, 0, 0);
}

/* Configuration, endpoints and buffers; 0, or -1 with the device left to release. */
static int un_endpoints(usb_dev_t *d, const un_desc_t *u, uint8_t cfgval)
{
	xhci_t *x = d->x;
	void *dp = buddy_alloc(DATA_ORDER);
	if (!dp) return -1;
	d->data_phys = (uint64_t)dp;
	d->data = (uint8_t *)PHYS_TO_VIRT(d->data_phys);
	if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) return -1;            /* SET_CONFIGURATION */
	uint32_t *ic = ctx_ctrl(d), *sc = ctx_slot(d);
	memset(ic, 0, x->ctxsize);
	int dci_in = setup_bulk_ep(d, u->ep_in, u->mps_in, 0, &d->ep_in);
	int dci_out = setup_bulk_ep(d, u->ep_out, u->mps_out, 0, &d->ep_out);
	if (dci_in < 0 || dci_out < 0) return -1;
	ic[1] = 1u | 1u << dci_in | 1u << dci_out;
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | (uint32_t)(dci_in > dci_out ? dci_in : dci_out) << 27;
	if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24), 0) != CC_SUCCESS)
		return -1;
	d->dci_in = dci_in;
	d->dci_out = dci_out;
	d->addr_in = u->ep_in;
	d->addr_out = u->ep_out;
	d->mps_in = u->mps_in;
	d->mps_out = u->mps_out;
	d->iface = u->data_iface;
	return 0;
}

static int usbnet_attach(usb_dev_t *d, const un_desc_t *u, uint8_t cfgval)
{
	usbnet_t *n = 0;
	for (int i = 0; i < UN_MAX && !n; i++) if (!g_net[i].dev) n = &g_net[i];
	uint8_t mac[6];
	if (!n || un_endpoints(d, u, cfgval) != 0) return -1;
	if ((u->kind == UN_ECM ? un_ecm_up(d, u, mac) : un_rndis_up(d, u, mac)) != 0) {
		kprintf(KERN_ERR "xhci: port %d network adapter did not come up\n", d->port + 1);
		return -1;
	}
	if (!n->registered) {
		memset(&n->nd, 0, sizeof n->nd);
		memcpy(n->nd.name, "usbnet0", 8);
		n->nd.name[6] = (char)('0' + (n - g_net));
		n->nd.tx = un_tx;
		n->nd.poll = un_poll;
		n->nd.link_check = un_link;
		n->nd.priv = n;
		n->nd.rx_lock = (spinlock_t)SPINLOCK_INIT;
		n->nd.net_ns = &init_net_ns;
		n->nd.keep_dns = 1;            /* a roboRIO or a phone is not the way to the internet */
		n->lock = (spinlock_t)SPINLOCK_INIT;
	}
	memcpy(n->nd.mac, mac, 6);
	if (n->registered) ipv6_linklocal_from_mac(mac, &n->nd.ip6_ll);   /* a new adapter, a new address */
	n->desc = *u;
	uint64_t f = spin_lock_irq(&n->lock);
	n->tx_fill = n->tx_queued = n->tx_done = 0;
	n->dev = d;
	spin_unlock_irq(&n->lock, f);
	n->rx_next = 0;
	d->net = n;
	for (uint32_t s = 0; s < UN_RXN; s++) un_queue_rx(n, s);
	ring_doorbell(d->x, d->slot, (uint32_t)d->dci_in);
	if (!n->registered) un_pending = 1;     /* usbd registers it once boot's NICs are in */
	kprintf(KERN_INFO "xhci: port %d %s is %s %02x:%02x:%02x:%02x:%02x:%02x\n", d->port + 1,
	        u->kind == UN_ECM ? "cdc-ecm" : "rndis", n->nd.name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return 0;
}

static void un_unplug(usb_dev_t *d)
{
	usbnet_t *u = d->net;
	uint64_t f = spin_lock_irq(&u->lock);
	u->dev = NULL;                     /* tx drops from here on; the carrier goes down */
	spin_unlock_irq(&u->lock, f);
	d->net = NULL;
	kprintf(KERN_INFO "xhci: %s pulled\n", u->nd.name);
}
