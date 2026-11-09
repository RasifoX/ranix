/*
 * Ports: bring up what is plugged into a root or hub port, notice it leave, and
 * serve port-change events.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

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
