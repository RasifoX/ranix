#include <kernel/net/ipv6.h>
#include <kernel/net/icmpv6.h>
#include <kernel/net/ndp.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/board.h>

extern volatile uint64_t kernel_ticks;

#define IP6_MTU  1500

const in6_addr_t in6addr_loopback = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
const in6_addr_t in6addr_any      = {{0}};

void ipv6_linklocal_from_mac(const uint8_t mac[6], in6_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->s6[0]  = 0xfe; out->s6[1] = 0x80;
    out->s6[8]  = mac[0] ^ 0x02;   /* flip the EUI-64 universal/local bit */
    out->s6[9]  = mac[1];
    out->s6[10] = mac[2];
    out->s6[11] = 0xff;
    out->s6[12] = 0xfe;
    out->s6[13] = mac[3];
    out->s6[14] = mac[4];
    out->s6[15] = mac[5];
}

uint16_t ipv6_pseudo_cksum(const in6_addr_t *src, const in6_addr_t *dst,
                           uint8_t next_hdr, const void *seg, uint16_t seg_len)
{
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)src;
    for (int i = 0; i < 8; i++) sum += p[i];
    p = (const uint16_t *)dst;
    for (int i = 0; i < 8; i++) sum += p[i];
    sum += htons(seg_len);            /* upper-layer length, high 16 bits zero */
    sum += htons((uint16_t)next_hdr); /* 24 zero bits + next-header byte */

    p = (const uint16_t *)seg;
    uint16_t n = seg_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

in6_addr_t ipv6_src_for(netdev_t *dev, const in6_addr_t *dst)
{
    if (in6_eq(dst, &in6addr_loopback))      return in6addr_loopback;
    if (in6_is_ll(dst) || in6_is_mcast(dst)) return dev->ip6_ll;
    if (!in6_is_unspec(&dev->ip6))           return dev->ip6;   /* SLAAC global */
    return dev->ip6_ll;
}

/* dst shares our on-link /64 prefix (we have a configured global address)? */
static int ipv6_on_link(netdev_t *dev, const in6_addr_t *dst)
{
    if (in6_is_unspec(&dev->ip6)) return 0;
    return memcmp(dst->s6, dev->ip6.s6, 8) == 0;
}

/* Can we reach dst at all? Loopback, link-local, multicast and on-link globals
 * are always reachable; an off-link global needs a default router. Used by
 * connect()/sendto() to fail fast (EHOSTUNREACH) instead of blocking on a SYN
 * that can never be resolved — this is what lets AF_UNSPEC fall back to IPv4. */
int ipv6_reachable(netdev_t *dev, const in6_addr_t *dst)
{
    if (!dev) return 0;
    if (in6_eq(dst, &in6addr_loopback) || in6_is_ll(dst) || in6_is_mcast(dst))
        return 1;
    if (ipv6_on_link(dev, dst)) return 1;
    return dev->ip6_has_router;
}

static int ipv6_for_us(netdev_t *dev, const in6_addr_t *dst)
{
    return in6_eq(dst, &in6addr_loopback) ||
           in6_eq(dst, &dev->ip6_ll)      ||
           in6_eq(dst, &dev->ip6)         ||
           in6_is_mcast(dst);
}

/* hand an upper-layer payload (skb->data = transport header) to its protocol */
static void ipv6_deliver(netdev_t *dev, ipv6_hdr_t *ip6, uint8_t nh, skb_t *skb)
{
    switch (nh) {
    case IP6_NH_ICMPV6: icmpv6_rx(dev, skb, ip6); break;
    case IP6_NH_UDP:    udp6_rx(dev, skb, ip6);   break;
    case IP6_NH_TCP:    tcp6_rx(dev, skb, ip6);   break;
    default:            skb_free(skb); break;
    }
}

/* ── reassembly ──────────────────────────────────────────── */
#define IP6_REASM_SLOTS   8
#define IP6_REASM_MAX     65535u
#define IP6_REASM_TIMEOUT (30u * TIMER_HZ)

typedef struct {
    int        used;
    in6_addr_t src, dst;
    uint32_t   id;
    uint8_t    nh;       /* upper-layer protocol from the first fragment */
    uint64_t   deadline;
    uint32_t   got;
    int        have_last;
    uint32_t   total;
    uint8_t   *buf;
    uint8_t   *bmap;
} ip6_reasm_t;

static ip6_reasm_t reasm6[IP6_REASM_SLOTS];
static spinlock_t  reasm6_lock = SPINLOCK_INIT;

static void reasm6_release(ip6_reasm_t *r)
{
    r->used = 0; r->got = 0; r->have_last = 0; r->total = 0;
    if (r->bmap) memset(r->bmap, 0, IP6_REASM_MAX / 8 + 1);
}

static ip6_reasm_t *reasm6_find(const in6_addr_t *src, const in6_addr_t *dst,
                                uint32_t id, int alloc)
{
    uint64_t now = kernel_ticks;
    ip6_reasm_t *fs = NULL;
    for (int i = 0; i < IP6_REASM_SLOTS; i++) {
        ip6_reasm_t *r = &reasm6[i];
        if (r->used && now > r->deadline) reasm6_release(r);
        if (r->used && r->id == id && in6_eq(&r->src, src) && in6_eq(&r->dst, dst))
            return r;
        if (!r->used && !fs) fs = r;
    }
    if (!alloc || !fs) return NULL;
    if (!fs->buf)  fs->buf  = kmalloc(IP6_REASM_MAX);
    if (!fs->bmap) fs->bmap = kmalloc(IP6_REASM_MAX / 8 + 1);
    if (!fs->buf || !fs->bmap) return NULL;
    memset(fs->bmap, 0, IP6_REASM_MAX / 8 + 1);
    fs->used = 1; fs->src = *src; fs->dst = *dst; fs->id = id;
    fs->got = 0; fs->have_last = 0; fs->total = 0;
    fs->deadline = now + IP6_REASM_TIMEOUT;
    return fs;
}

static void ip6_reasm_add6(netdev_t *dev, ipv6_hdr_t *ip6, uint8_t nh, uint32_t id,
                           const uint8_t *frag, uint32_t off, uint32_t flen, int more)
{
    if (off + flen > IP6_REASM_MAX) return;

    uint64_t f = spin_lock_irq(&reasm6_lock);
    ip6_reasm_t *r = reasm6_find(&ip6->src, &ip6->dst, id, 1);
    if (!r) { spin_unlock_irq(&reasm6_lock, f); return; }

    memcpy(r->buf + off, frag, flen);
    for (uint32_t b = off / 8; b < (off + flen + 7) / 8; b++)
        if (!r->bmap[b]) { r->bmap[b] = 1; r->got++; }
    if (off == 0) r->nh = nh;
    if (!more) { r->have_last = 1; r->total = off + flen; }

    if (!r->have_last || r->got != (r->total + 7) / 8) {
        spin_unlock_irq(&reasm6_lock, f); return;
    }

    uint32_t total = r->total;
    uint8_t  dnh   = r->nh;
    ipv6_hdr_t hdr = *ip6;
    uint8_t *buf = r->buf;
    r->buf = NULL;
    reasm6_release(r);
    spin_unlock_irq(&reasm6_lock, f);

    skb_t *skb = skb_alloc_big(total);
    if (skb) {
        memcpy(skb->data, buf, total);
        skb->len = (uint16_t)(total > 0xffff ? 0xffff : total);
        ipv6_deliver(dev, &hdr, dnh, skb);
    }
    kfree(buf);
}

void ipv6_rx(netdev_t *dev, skb_t *skb)
{
    if (skb->len < (uint16_t)sizeof(ipv6_hdr_t)) { skb_free(skb); return; }
    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)skb->data;
    if ((ip6->ver_tc_fl[0] >> 4) != 6) { skb_free(skb); return; }

    uint16_t plen = ntohs(ip6->payload_len);
    if (skb->len < (uint16_t)sizeof(ipv6_hdr_t) + plen) { skb_free(skb); return; }
    if (!ipv6_for_us(dev, &ip6->dst)) { skb_free(skb); return; }

    ipv6_hdr_t ip6_copy = *ip6;
    skb_pull(skb, sizeof(ipv6_hdr_t));
    skb->len = plen;   /* trim any padding the link layer added */

    /* walk past extension headers (hop-by-hop, routing, destination options) to
     * the upper-layer protocol — real IPv6 traffic carries these, and a switch
     * on the fixed next_hdr alone would drop it. Each such header is
     * [next(1)][hdr_ext_len(1)] + payload, total (hdr_ext_len+1)*8 bytes. */
    uint8_t nh = ip6_copy.next_hdr;
    for (;;) {
        if (nh == IP6_NH_HOPOPTS || nh == IP6_NH_ROUTING || nh == IP6_NH_DSTOPTS) {
            if (skb->len < 8) { skb_free(skb); return; }
            uint8_t  next = skb->data[0];
            uint16_t elen = ((uint16_t)skb->data[1] + 1) * 8;
            if (skb->len < elen) { skb_free(skb); return; }
            skb_pull(skb, elen);   /* advances data and reduces len */
            nh = next;
            continue;
        }
        if (nh == IP6_NH_FRAGMENT) {
            if (skb->len < sizeof(ipv6_frag_hdr_t)) { skb_free(skb); return; }
            ipv6_frag_hdr_t fh = *(ipv6_frag_hdr_t *)skb->data;
            skb_pull(skb, sizeof(ipv6_frag_hdr_t));
            uint16_t fo   = ntohs(fh.frag_off);
            uint32_t off  = fo & IP6_FRAG_OFFMASK;
            int      more = fo & IP6_FRAG_MF;
            ip6_reasm_add6(dev, &ip6_copy, fh.next_hdr, ntohl(fh.id),
                           skb->data, off, skb->len, more);
            skb_free(skb);
            return;
        }
        break;
    }

    ipv6_deliver(dev, &ip6_copy, nh, skb);
}

/* resolve next-hop link-layer address for an IPv6 header already built, ship it */
static int ipv6_xmit(netdev_t *dev, skb_t *skb, const in6_addr_t *dst)
{
    if (in6_eq(dst, &in6addr_loopback) || in6_eq(dst, &dev->ip6_ll) ||
        (!in6_is_unspec(&dev->ip6) && in6_eq(dst, &dev->ip6))) {
        eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
        memcpy(eh->dst, dev->mac, 6);
        memcpy(eh->src, dev->mac, 6);
        eh->type = htons(ETH_P_IPV6);
        netdev_rx_enqueue(dev, skb);
        return 0;
    }
    uint8_t dmac[6];
    if (in6_is_mcast(dst)) {
        dmac[0] = 0x33; dmac[1] = 0x33;
        memcpy(dmac + 2, dst->s6 + 12, 4);   /* 33:33 + low 32 bits of the group */
    } else {
        const in6_addr_t *nh = dst;
        if (!in6_is_ll(dst) && !ipv6_on_link(dev, dst) && dev->ip6_has_router)
            nh = &dev->ip6_router;
        if (ndp_lookup(dev, nh, dmac) != 0) { skb_free(skb); return -1; } /* NS sent */
    }
    eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
    memcpy(eh->dst, dmac, 6);
    memcpy(eh->src, dev->mac, 6);
    eh->type = htons(ETH_P_IPV6);
    return dev->tx(dev, skb);
}

static uint32_t ip6_frag_id;

/* split a payload too large for the MTU into Fragment-header'd IPv6 packets */
static int ipv6_tx_frag(netdev_t *dev, const uint8_t *payload, uint32_t plen,
                        const in6_addr_t *dst, uint8_t next_hdr)
{
    in6_addr_t src = ipv6_src_for(dev, dst);
    uint32_t id = ++ip6_frag_id;
    uint32_t maxdata = (IP6_MTU - sizeof(ipv6_hdr_t) - sizeof(ipv6_frag_hdr_t)) & ~7u;
    int rc = 0;
    for (uint32_t off = 0; off < plen; off += maxdata) {
        uint32_t chunk = plen - off < maxdata ? plen - off : maxdata;
        int more = (off + chunk < plen);
        skb_t *skb = skb_alloc();
        if (!skb) return -12;
        memcpy(skb->data, payload + off, chunk);
        skb->len = (uint16_t)chunk;
        ipv6_frag_hdr_t *fh = (ipv6_frag_hdr_t *)skb_push(skb, sizeof(*fh));
        fh->next_hdr = next_hdr;
        fh->reserved = 0;
        fh->frag_off = htons((uint16_t)(off | (more ? IP6_FRAG_MF : 0)));
        fh->id       = htonl(id);
        uint16_t plen16 = skb->len;
        ipv6_hdr_t *ip6 = (ipv6_hdr_t *)skb_push(skb, sizeof(ipv6_hdr_t));
        memset(ip6, 0, sizeof(*ip6));
        ip6->ver_tc_fl[0] = 0x60;
        ip6->payload_len  = htons(plen16);
        ip6->next_hdr     = IP6_NH_FRAGMENT;
        ip6->hop_limit    = 64;
        ip6->src = src;
        ip6->dst = *dst;
        int r = ipv6_xmit(dev, skb, dst);
        if (r < 0) rc = r;
    }
    return rc;
}

int ipv6_tx(netdev_t *dev, skb_t *skb, const in6_addr_t *dst, uint8_t next_hdr)
{
    if (sizeof(ipv6_hdr_t) + skb->len > IP6_MTU) {
        int rc = ipv6_tx_frag(dev, skb->data, skb->len, dst, next_hdr);
        skb_free(skb);
        return rc;
    }

    uint16_t plen = skb->len;
    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)skb_push(skb, sizeof(ipv6_hdr_t));
    memset(ip6, 0, sizeof(*ip6));
    ip6->ver_tc_fl[0] = 0x60;          /* version 6, traffic class/flow 0 */
    ip6->payload_len  = htons(plen);
    ip6->next_hdr     = next_hdr;
    ip6->hop_limit    = 64;
    ip6->src = ipv6_src_for(dev, dst);
    ip6->dst = *dst;

    return ipv6_xmit(dev, skb, dst);
}
