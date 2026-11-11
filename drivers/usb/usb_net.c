/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB network adapters (CDC-ECM, RNDIS): the device becomes the network
 * interface usbnet0 (usbnet1, ...), and the kernel's DHCP client configures it
 * when it comes up, as for eth0. The roboRIO's USB port is one: plug it in and
 * it is 172.22.11.2 from here. A device that offers both in two configurations
 * (a Linux gadget does) is used as ECM; ranix.usbnet=rndis on the command line
 * prefers RNDIS.
 *
 * Nothing here may wait: the stack calls tx from a timer tick and poll from the
 * idle loop. So tx copies the frame into one of UN_TXN slots and leaves it for
 * the usbd thread, which queues it on the bulk OUT ring (a zero-length packet
 * after a frame that fills its last packet). UN_RXN bulk IN transfers stay
 * queued; evt_next hands their completions to un_rx_event, which turns each
 * frame into an skb for the stack and queues the buffer again. Transfers end in
 * ring order, so both sides keep a first-in first-out count.
 *
 * Plug and pull: the interface, once made, stays registered (the stack has no
 * unregister); one found at boot joins after the built-in NICs (net_boot_done). While nothing is behind it, its carrier is down and frames
 * sent to it are dropped. The next adapter takes it again.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

#define UN_MAX    2
#define UN_RXN    8
#define UN_RXSZ   4096          /* also RNDIS's maximum transfer to us */
#define UN_TXN    8
#define UN_TXSZ   2048
#define UN_TXOFF  (UN_RXN * UN_RXSZ)   /* in d->data, after the RX buffers */

typedef struct usbnet {
	netdev_t   nd;
	uint8_t    registered;
	usb_dev_t *dev;             /* NULL while unplugged */
	un_desc_t  desc;
	spinlock_t lock;            /* the TX counters */
	uint32_t   tx_fill, tx_queued, tx_done;   /* filled by tx / on the ring / finished */
	uint16_t   tx_len[UN_TXN];
	uint64_t   tx_trb[UN_TXN];  /* the TRB whose completion ends each slot */
	uint32_t   rx_next;         /* the slot that completes next */
	uint64_t   rx_trb[UN_RXN];
	uint32_t   rx_errors, tx_errors;
} usbnet_t;

static usbnet_t g_net[UN_MAX];
static volatile int un_kick;    /* a frame waits for usbd */
static volatile int un_pending; /* an adapter waits to be registered */

/* From usbd: adapters found before the boot's NICs were registered join after them. */
static void un_register_pending(void)
{
	if (!un_pending || !net_boot_done) return;
	un_pending = 0;
	for (int i = 0; i < UN_MAX; i++) {
		usbnet_t *n = &g_net[i];
		if (n->registered || !n->dev) continue;
		net_register(&n->nd);
		n->registered = 1;
	}
}

/* ---- the stack's side: never waits ---- */

static int un_tx(netdev_t *nd, skb_t *skb)
{
	usbnet_t *u = nd->priv;
	uint32_t len = skb->len, hdr = u->desc.kind == UN_RNDIS ? RNDIS_HDR : 0;
	uint64_t f = spin_lock_irq(&u->lock);
	usb_dev_t *d = u->dev;
	int ok = d && len && len + hdr <= UN_TXSZ && u->tx_fill - u->tx_done < UN_TXN;
	if (ok) {
		uint32_t s = u->tx_fill % UN_TXN;
		uint8_t *p = d->data + UN_TXOFF + s * UN_TXSZ;
		if (hdr) un_rndis_wrap(p, len);
		memcpy(p + hdr, skb->data, len);
		u->tx_len[s] = (uint16_t)(len + hdr);
		u->tx_fill++;
		un_kick = 1;                   /* the next tick wakes usbd */
	}
	spin_unlock_irq(&u->lock, f);
	if (ok) { __atomic_add_fetch(&nd->tx_packets, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&nd->tx_bytes, len, __ATOMIC_RELAXED); }
	else __atomic_add_fetch(&nd->tx_dropped, 1, __ATOMIC_RELAXED);
	skb_free(skb);
	return ok ? 0 : -1;
}

static void un_poll(netdev_t *nd) { (void)nd; }   /* frames arrive through usbd */
static int un_link(netdev_t *nd) { return ((usbnet_t *)nd->priv)->dev != 0; }

/* ---- the usbd side (controller lock held) ---- */

static void un_queue_rx(usbnet_t *u, uint32_t s)
{
	usb_dev_t *d = u->dev;
	u->rx_trb[s] = ring_push(&d->ep_in, d->data_phys + (uint64_t)s * UN_RXSZ, UN_RXSZ,
	                         TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
}

/* Frames tx left: onto the bulk OUT ring. */
static void un_service(void)
{
	for (int i = 0; i < UN_MAX; i++) {
		usbnet_t *u = &g_net[i];
		usb_dev_t *d = u->dev;
		if (!d) continue;
		int rung = 0;
		for (;;) {
			uint64_t f = spin_lock_irq(&u->lock);
			int more = u->tx_queued != u->tx_fill;
			uint32_t s = u->tx_queued % UN_TXN, len = u->tx_len[s];
			spin_unlock_irq(&u->lock, f);
			if (!more) break;
			uint64_t p = d->data_phys + UN_TXOFF + (uint64_t)s * UN_TXSZ;
			int zlp = un_needs_zlp(len, u->desc.mps_out);
			uint64_t at = ring_push(&d->ep_out, p, len, TRB_TYPE(TRB_NORMAL) | (zlp ? 0 : TRB_IOC));
			if (zlp) at = ring_push(&d->ep_out, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
			u->tx_trb[s] = at;
			u->tx_queued++;
			rung = 1;
		}
		if (rung) ring_doorbell(d->x, d->slot, (uint32_t)d->dci_out);
	}
}

static void un_frame(void *ctx, const uint8_t *p, uint32_t n)
{
	netdev_t *nd = ctx;
	if (!((usbnet_t *)nd->priv)->registered) return;   /* nobody drains it yet */
	if (n < 14 || n > SKB_DATA_MAX) { netdev_rx_drop(nd); return; }
	skb_t *skb = skb_alloc();
	if (!skb) { netdev_rx_drop(nd); return; }
	memcpy(skb->data, p, n);
	skb->len = (uint16_t)n;
	netdev_rx_enqueue(nd, skb);
}

/* From evt_next: a bulk IN or OUT transfer of an adapter ended. 1 if it was one. */
static int un_event(usb_dev_t *d, uint32_t dci, const event_t *ev)
{
	usbnet_t *u = d->net;
	uint32_t cc = TRB_GET_CC(ev->status);
	int bad = cc != CC_SUCCESS && cc != CC_SHORT;
	if (dci == (uint32_t)d->dci_out) {
		uint32_t s = u->tx_done % UN_TXN;
		if (u->tx_done == u->tx_queued || ev->ptr != u->tx_trb[s]) return 1;   /* a ZLP's first half */
		if (bad) { u->tx_errors++; d->net_reset = 1; }
		uint64_t f = spin_lock_irq(&u->lock);
		u->tx_done++;
		spin_unlock_irq(&u->lock, f);
		return 1;
	}
	uint32_t s = u->rx_next % UN_RXN;
	if (ev->ptr != u->rx_trb[s]) return 1;
	u->rx_next++;
	if (bad) { u->rx_errors++; d->net_reset = 1; return 1; }
	uint32_t got = UN_RXSZ - (ev->status & 0xFFFFFF);
	const uint8_t *p = d->data + (uint64_t)s * UN_RXSZ;
	if (got > UN_RXSZ) got = 0;
	if (u->desc.kind == UN_RNDIS) un_rndis_unwrap(p, got, un_frame, &u->nd);
	else if (got) un_frame(&u->nd, p, got);
	un_queue_rx(u, s);
	ring_doorbell(d->x, d->slot, (uint32_t)d->dci_in);
	net_rx_wake();
	return 1;
}

/* An endpoint that failed: skip what it held and start the receive side again. */
static void un_recover(usb_dev_t *d)
{
	usbnet_t *u = d->net;
	d->net_reset = 0;
	if (++d->net_resets > 8) return;
	for (int in = 0; in <= 1; in++) {
		ring_t *r = in ? &d->ep_in : &d->ep_out;
		uint32_t ids = (uint32_t)d->slot << 24 | (uint32_t)(in ? d->dci_in : d->dci_out) << 16;
		cmd_exec(d->x, 0, TRB_TYPE(TRB_STOP_EP) | ids, 0);
		cmd_exec(d->x, 0, TRB_TYPE(TRB_RESET_EP) | ids, 0);
		cmd_exec(d->x, (r->phys + (uint64_t)r->idx * sizeof(trb_t)) | r->cycle, TRB_TYPE(TRB_SET_TR_DEQ) | ids, 0);
	}
	uint64_t f = spin_lock_irq(&u->lock);
	u->tx_done = u->tx_queued;            /* what was on the ring is gone */
	spin_unlock_irq(&u->lock, f);
	u->rx_next = 0;
	for (uint32_t s = 0; s < UN_RXN; s++) un_queue_rx(u, s);
	ring_doorbell(d->x, d->slot, (uint32_t)d->dci_in);
	d->x->n_recover++;
}

#include "usb_net_attach.c"
