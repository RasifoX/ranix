/* Minimal DHCP client (RFC 2131): DISCOVER → OFFER → REQUEST → ACK, then sets
 * the device's ip/gw/mask and returns the offered DNS server. Runs at boot by
 * busy-polling the NIC, bypassing the socket layer (we have no IP yet, and the
 * normal RX path would drop broadcast/unaddressed replies). Falls back to the
 * driver's static IP if no server answers. */
#include <kernel/net/net.h>
#include <kernel/net/eth.h>
#include <kernel/net/ip.h>
#include <kernel/net/udp.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

typedef struct {
	uint8_t  op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint32_t ciaddr, yiaddr, siaddr, giaddr;
	uint8_t  chaddr[16];
	uint8_t  sname[64];
	uint8_t  bootfile[128];
	uint32_t magic;          /* 0x63825363 */
	uint8_t  options[312];
} __attribute__((packed)) dhcp_pkt_t;

#define DHCP_MAGIC 0x63825363u

static int build_options(uint8_t *o, int msgtype, uint32_t reqip, uint32_t serverid)
{
	int i = 0;
	o[i++] = 53; o[i++] = 1; o[i++] = (uint8_t)msgtype;          /* message type */
	if (reqip) {                                                /* requested IP */
		o[i++] = 50; o[i++] = 4;
		o[i++] = reqip >> 24; o[i++] = reqip >> 16; o[i++] = reqip >> 8; o[i++] = reqip;
	}
	if (serverid) {                                             /* server id */
		o[i++] = 54; o[i++] = 4;
		o[i++] = serverid >> 24; o[i++] = serverid >> 16; o[i++] = serverid >> 8; o[i++] = serverid;
	}
	o[i++] = 55; o[i++] = 3; o[i++] = 1; o[i++] = 3; o[i++] = 6; /* param list: mask, router, dns */
	o[i++] = 255;                                               /* end */
	return i;
}

static void dhcp_send(netdev_t *dev, uint32_t xid, int msgtype, uint32_t reqip, uint32_t serverid)
{
	skb_t *skb = skb_alloc();
	if (!skb) return;
	dhcp_pkt_t *p = (dhcp_pkt_t *)skb->data;
	memset(p, 0, sizeof(*p));
	p->op = 1; p->htype = 1; p->hlen = 6;
	p->xid = xid;                  /* kept in network order throughout */
	p->flags = htons(0x8000);      /* broadcast reply, please */
	memcpy(p->chaddr, dev->mac, 6);
	p->magic = htonl(DHCP_MAGIC);
	int olen = build_options(p->options, msgtype, reqip, serverid);
	int dhcp_len = (int)(sizeof(*p) - sizeof(p->options) + olen);
	skb->len = (uint16_t)dhcp_len;

	udp_hdr_t *uh = (udp_hdr_t *)skb_push(skb, sizeof(udp_hdr_t));
	uh->src_port = htons(68); uh->dst_port = htons(67);
	uh->len = htons((uint16_t)(sizeof(udp_hdr_t) + dhcp_len)); uh->checksum = 0;

	ip_hdr_t *iph = (ip_hdr_t *)skb_push(skb, sizeof(ip_hdr_t));
	memset(iph, 0, sizeof(*iph));
	iph->ver_ihl = 0x45; iph->tot_len = htons(skb->len); iph->ttl = 64;
	iph->proto = IP_PROTO_UDP; iph->src = 0; iph->dst = 0xFFFFFFFF;
	iph->checksum = inet_cksum(iph, sizeof(*iph));

	eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
	memcpy(eh->dst, eth_bcast, 6); memcpy(eh->src, dev->mac, 6);
	eh->type = htons(ETH_P_IP);

	dev->tx(dev, skb);
}

/* Parse a received frame; if it is a DHCP reply for our xid, fill *yiaddr / opts
 * and return the message type, else 0. */
static int dhcp_parse(skb_t *skb, uint32_t xid, uint32_t *yiaddr,
                      uint32_t *mask, uint32_t *gw, uint32_t *dns, uint32_t *serverid)
{
	if (skb->len < sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + 240)
		return 0;
	eth_hdr_t *eh = (eth_hdr_t *)skb->data;
	if (ntohs(eh->type) != ETH_P_IP) return 0;
	ip_hdr_t *iph = (ip_hdr_t *)(eh + 1);
	if (iph->proto != IP_PROTO_UDP) return 0;
	int ihl = (iph->ver_ihl & 0x0f) * 4;
	udp_hdr_t *uh = (udp_hdr_t *)((uint8_t *)iph + ihl);
	if (ntohs(uh->dst_port) != 68 || ntohs(uh->src_port) != 67) return 0;
	dhcp_pkt_t *p = (dhcp_pkt_t *)(uh + 1);
	if (p->op != 2 || p->xid != xid || ntohl(p->magic) != DHCP_MAGIC) return 0;

	*yiaddr = ntohl(p->yiaddr);
	int msgtype = 0;
	uint8_t *o = p->options;
	int max = (int)skb->len - (int)((uint8_t *)o - skb->data);
	for (int i = 0; i < max && o[i] != 255; ) {
		if (o[i] == 0) { i++; continue; }
		int t = o[i], l = o[i + 1];
		uint8_t *v = &o[i + 2];
		uint32_t val = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
		if (t == 53) msgtype = v[0];
		else if (t == 1)  *mask = val;
		else if (t == 3)  *gw = val;
		else if (t == 6)  *dns = val;
		else if (t == 54) *serverid = val;
		i += 2 + l;
	}
	return msgtype;
}

/* Wait until `deadline` (kernel_ticks) for a reply of want_type. Time-based,
 * not a fixed iteration count: on real hardware the OFFER/ACK round-trip takes
 * real milliseconds, and a tight count-loop finishes in microseconds — long
 * before a real server answers (it was fine only against QEMU's instant SLIRP). */
static skb_t *dhcp_poll_reply(netdev_t *dev, uint32_t xid, int want_type,
                              uint32_t *yiaddr, uint32_t *mask, uint32_t *gw,
                              uint32_t *dns, uint32_t *serverid, uint64_t deadline)
{
	extern volatile uint64_t kernel_ticks;
	while (kernel_ticks < deadline) {
		if (dev->poll) dev->poll(dev);
		skb_t *skb;
		while ((skb = netdev_rx_dequeue(dev)) != NULL) {
			int mt = dhcp_parse(skb, xid, yiaddr, mask, gw, dns, serverid);
			if (mt == want_type) return skb;
			skb_free(skb);
		}
	}
	return NULL;
}

/* Returns 0 and configures dev (ip/gw/mask) on success; *out_dns gets the DNS
 * server (host order, 0 if none). Returns -1 on timeout. */
int dhcp_configure(netdev_t *dev, uint32_t *out_dns)
{
	extern volatile uint64_t kernel_ticks;
	uint32_t xid = htonl((uint32_t)(0x52414e00u ^ (kernel_ticks * 2654435761u)));
	uint32_t yiaddr = 0, mask = 0, gw = 0, dns = 0, serverid = 0;

	/* DISCOVER → OFFER, retransmitting up to 4× (a real server may be slow or
	 * the first DISCOVER may be lost); ~1.5s per attempt. */
	skb_t *off = NULL;
	for (int attempt = 0; attempt < 4 && !off; attempt++) {
		dhcp_send(dev, xid, DHCP_DISCOVER, 0, 0);
		off = dhcp_poll_reply(dev, xid, DHCP_OFFER, &yiaddr, &mask, &gw, &dns,
		                      &serverid, kernel_ticks + 150);
	}
	if (!off) return -1;
	skb_free(off);
	if (!yiaddr) return -1;

	skb_t *ack = NULL;
	for (int attempt = 0; attempt < 4 && !ack; attempt++) {
		dhcp_send(dev, xid, DHCP_REQUEST, yiaddr, serverid);
		ack = dhcp_poll_reply(dev, xid, DHCP_ACK, &yiaddr, &mask, &gw, &dns,
		                      &serverid, kernel_ticks + 150);
	}
	if (!ack) return -1;
	skb_free(ack);

	dev->ip = yiaddr;
	if (mask) dev->mask = mask;
	if (gw)   dev->gw = gw;
	if (out_dns) *out_dns = dns;

	kprintf("dhcp: %s ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n", dev->name,
	        (yiaddr >> 24) & 0xff, (yiaddr >> 16) & 0xff, (yiaddr >> 8) & 0xff, yiaddr & 0xff,
	        (gw >> 24) & 0xff, (gw >> 16) & 0xff, (gw >> 8) & 0xff, gw & 0xff,
	        (dns >> 24) & 0xff, (dns >> 16) & 0xff, (dns >> 8) & 0xff, dns & 0xff);
	return 0;
}
