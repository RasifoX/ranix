#include <kernel/net/ip.h>
#include <kernel/net/eth.h>
#include <kernel/net/arp.h>
#include <kernel/net/icmp.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/net/netfilter.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/board.h>

extern volatile uint64_t kernel_ticks;

#define IP_MTU       1500
#define IP_MF        0x2000   /* more fragments */
#define IP_OFFMASK   0x1fff   /* fragment offset, in 8-byte units */

static uint16_t ip_id;

/* ── reassembly ──────────────────────────────────────────────
 * A small bounded table of in-progress datagrams keyed by (src,dst,id,proto).
 * Each holds a fixed 64KB scratch and a bytemap of which 8-byte blocks arrived;
 * a datagram completes when every block up to the final (MF=0) fragment is
 * present. Incomplete entries are reaped after IP_REASM_TIMEOUT ticks. */
#define IP_REASM_SLOTS    8
#define IP_REASM_MAX      65535u
#define IP_REASM_TIMEOUT  (30u * TIMER_HZ)   /* 30s, like Linux ipfrag_time */

typedef struct {
    int      used;
    uint32_t src, dst;
    uint16_t id;
    uint8_t  proto;
    uint64_t deadline;
    uint32_t got;        /* distinct 8-byte blocks received */
    int      have_last;  /* the MF=0 fragment arrived */
    uint32_t total;      /* total length once have_last (offset+len of last) */
    uint8_t *buf;        /* IP_REASM_MAX scratch, lazily allocated */
    uint8_t *bmap;       /* received-block bitmap, 1 byte per 8-byte block */
} ip_reasm_t;

static ip_reasm_t reasm[IP_REASM_SLOTS];
static spinlock_t reasm_lock = SPINLOCK_INIT;

static void reasm_release(ip_reasm_t *r)
{
    r->used = 0;
    r->got = 0; r->have_last = 0; r->total = 0;
    if (r->bmap) memset(r->bmap, 0, IP_REASM_MAX / 8 + 1);
}

static ip_reasm_t *reasm_find(uint32_t src, uint32_t dst, uint16_t id, uint8_t proto, int alloc)
{
    uint64_t now = kernel_ticks;
    ip_reasm_t *free_slot = NULL;
    for (int i = 0; i < IP_REASM_SLOTS; i++) {
        ip_reasm_t *r = &reasm[i];
        if (r->used && now > r->deadline) reasm_release(r);   /* reap stale */
        if (r->used && r->src == src && r->dst == dst && r->id == id && r->proto == proto)
            return r;
        if (!r->used && !free_slot) free_slot = r;
    }
    if (!alloc || !free_slot) return NULL;
    ip_reasm_t *r = free_slot;
    if (!r->buf)  r->buf  = kmalloc(IP_REASM_MAX);
    if (!r->bmap) r->bmap = kmalloc(IP_REASM_MAX / 8 + 1);
    if (!r->buf || !r->bmap) return NULL;
    memset(r->bmap, 0, IP_REASM_MAX / 8 + 1);
    r->used = 1; r->src = src; r->dst = dst; r->id = id; r->proto = proto;
    r->got = 0; r->have_last = 0; r->total = 0;
    r->deadline = now + IP_REASM_TIMEOUT;
    return r;
}

/* Deliver a fully reassembled datagram to the transport layer. payload points
 * at the transport header, len is the reassembled payload length. */
static void ip_deliver(netdev_t *dev, ip_hdr_t *iph, const uint8_t *payload, uint32_t len)
{
    if (nf_in_v4(ntohl(iph->src), ntohl(iph->dst), iph->proto, payload, len) == NF_DROP)
        return;
    skb_t *skb = skb_alloc_big(len);
    if (!skb) return;
    memcpy(skb->data, payload, len);
    skb->len = (uint16_t)(len > 0xffff ? 0xffff : len);
    switch (iph->proto) {
    case IP_PROTO_ICMP: icmp_rx(dev, skb, iph); break;
    case IP_PROTO_UDP:  udp_rx(dev, skb, iph);  break;
    case IP_PROTO_TCP:  tcp_rx(dev, skb, iph);  break;
    default:            skb_free(skb); break;
    }
}

/* Add one fragment; on completion deliver and release the slot. */
static void ip_reasm_add(netdev_t *dev, ip_hdr_t *iph, const uint8_t *frag,
                         uint32_t off, uint32_t flen, int more)
{
    if (off + flen > IP_REASM_MAX) return;   /* malformed/oversized */

    uint64_t f = spin_lock_irq(&reasm_lock);
    ip_reasm_t *r = reasm_find(ntohl(iph->src), ntohl(iph->dst), ntohs(iph->id), iph->proto, 1);
    if (!r) { spin_unlock_irq(&reasm_lock, f); return; }

    memcpy(r->buf + off, frag, flen);
    for (uint32_t b = off / 8; b < (off + flen + 7) / 8; b++)
        if (!r->bmap[b]) { r->bmap[b] = 1; r->got++; }
    if (!more) { r->have_last = 1; r->total = off + flen; }

    if (!r->have_last || r->got != (r->total + 7) / 8) {
        spin_unlock_irq(&reasm_lock, f); return;
    }

    uint32_t total = r->total;
    ip_hdr_t hdr = *iph;
    uint8_t *buf = r->buf;
    r->buf = NULL;            /* hand the scratch to the delivery path */
    reasm_release(r);
    spin_unlock_irq(&reasm_lock, f);

    hdr.frag_off = 0;
    ip_deliver(dev, &hdr, buf, total);
    kfree(buf);
}

void ip_rx(netdev_t *dev, skb_t *skb)
{
    if (skb->len < (uint16_t)sizeof(ip_hdr_t)) { skb_free(skb); return; }
    ip_hdr_t *iph = (ip_hdr_t *)skb->data;

    uint8_t ihl = (iph->ver_ihl & 0x0f) * 4;
    if (ihl < 20 || skb->len < ihl) { skb_free(skb); return; }

    uint32_t dst = ntohl(iph->dst);
    if (dst != dev->ip && dst != 0xffffffff && dst != (dev->ip | ~dev->mask) &&
        (dst >> 24) != 127)   /* 127/8 = loopback, looped back by ip_tx */
    { skb_free(skb); return; }  /* not for us */

    uint16_t fo = ntohs(iph->frag_off);
    if (fo & (IP_MF | IP_OFFMASK)) {
        uint16_t tot = ntohs(iph->tot_len);
        if (tot < ihl || skb->len < tot) { skb_free(skb); return; }
        uint32_t off  = (uint32_t)(fo & IP_OFFMASK) * 8;
        uint32_t flen = (uint32_t)tot - ihl;
        ip_reasm_add(dev, iph, skb->data + ihl, off, flen, (fo & IP_MF) != 0);
        skb_free(skb);
        return;
    }

    ip_hdr_t iph_copy = *iph;    /* save before pull */
    skb_pull(skb, ihl);

    if (nf_in_v4(ntohl(iph_copy.src), ntohl(iph_copy.dst), iph_copy.proto,
                 skb->data, skb->len) == NF_DROP)
    { skb_free(skb); return; }

    switch (iph->proto) {
    case IP_PROTO_ICMP: icmp_rx(dev, skb, &iph_copy); break;
    case IP_PROTO_UDP:  udp_rx(dev, skb, &iph_copy);  break;
    case IP_PROTO_TCP:  tcp_rx(dev, skb, &iph_copy);  break;
    default:            skb_free(skb); break;
    }
}

/* Resolve next-hop MAC and ship one ready frame (IP header already built). */
static int ip_xmit(netdev_t *dev, skb_t *skb, uint32_t dst_ip)
{
    if (dst_ip == dev->ip || (dst_ip >> 24) == 127) {
        eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
        memcpy(eh->dst, dev->mac, 6);
        memcpy(eh->src, dev->mac, 6);
        eh->type = htons(ETH_P_IP);
        netdev_rx_enqueue(dev, skb);
        return 0;
    }
    uint32_t nexthop = dst_ip;
    if ((dst_ip & dev->mask) != (dev->ip & dev->mask))
        nexthop = dev->gw;
    uint8_t dst_mac[6];
    if (arp_lookup(dev, nexthop, dst_mac) != 0) { skb_free(skb); return -1; }
    eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, dev->mac, 6);
    eh->type = htons(ETH_P_IP);
    return dev->tx(dev, skb);
}

/* Split a payload larger than the MTU into IP fragments. */
static int ip_tx_frag(netdev_t *dev, const uint8_t *payload, uint32_t plen,
                      uint32_t dst_ip, uint8_t proto)
{
    uint16_t id = htons(ip_id++);
    uint32_t maxdata = (IP_MTU - sizeof(ip_hdr_t)) & ~7u;   /* 8-byte aligned */
    int rc = 0;
    for (uint32_t off = 0; off < plen; off += maxdata) {
        uint32_t chunk = plen - off < maxdata ? plen - off : maxdata;
        int more = (off + chunk < plen);
        skb_t *skb = skb_alloc();
        if (!skb) return -12;
        memcpy(skb->data, payload + off, chunk);
        skb->len = (uint16_t)chunk;
        ip_hdr_t *iph = (ip_hdr_t *)skb_push(skb, sizeof(ip_hdr_t));
        memset(iph, 0, sizeof(ip_hdr_t));
        iph->ver_ihl  = 0x45;
        iph->tot_len  = htons((uint16_t)(sizeof(ip_hdr_t) + chunk));
        iph->id       = id;
        iph->frag_off = htons((uint16_t)((off / 8) | (more ? IP_MF : 0)));
        iph->ttl      = 64;
        iph->proto    = proto;
        iph->src      = htonl(dev->ip);
        iph->dst      = htonl(dst_ip);
        iph->checksum = inet_cksum(iph, sizeof(ip_hdr_t));
        int r = ip_xmit(dev, skb, dst_ip);
        if (r < 0) rc = r;   /* report failure but keep emitting the rest */
    }
    return rc;
}

int ip_tx(netdev_t *dev, skb_t *skb, uint32_t dst_ip, uint8_t proto)
{
    if (nf_out_v4(dev->ip, dst_ip, proto, skb->data, skb->len) == NF_DROP)
    { skb_free(skb); return -1; }

    if (sizeof(ip_hdr_t) + skb->len > IP_MTU) {
        int rc = ip_tx_frag(dev, skb->data, skb->len, dst_ip, proto);
        skb_free(skb);
        return rc;
    }

    ip_hdr_t *iph = (ip_hdr_t *)skb_push(skb, sizeof(ip_hdr_t));
    memset(iph, 0, sizeof(ip_hdr_t));
    iph->ver_ihl  = 0x45;
    iph->tot_len  = htons(skb->len);
    iph->id       = htons(ip_id++);
    iph->ttl      = 64;
    iph->proto    = proto;
    iph->src      = htonl(dev->ip);
    iph->dst      = htonl(dst_ip);
    iph->checksum = inet_cksum(iph, sizeof(ip_hdr_t));

    return ip_xmit(dev, skb, dst_ip);
}
