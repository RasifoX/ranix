#include <kernel/net/ipv6.h>
#include <kernel/net/icmpv6.h>
#include <kernel/net/ndp.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>

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

static int ipv6_for_us(netdev_t *dev, const in6_addr_t *dst)
{
    return in6_eq(dst, &in6addr_loopback) ||
           in6_eq(dst, &dev->ip6_ll)      ||
           in6_eq(dst, &dev->ip6)         ||
           in6_is_mcast(dst);
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

    switch (ip6_copy.next_hdr) {
    case IP6_NH_ICMPV6: icmpv6_rx(dev, skb, &ip6_copy); break;
    case IP6_NH_UDP:    udp6_rx(dev, skb, &ip6_copy);   break;
    case IP6_NH_TCP:    tcp6_rx(dev, skb, &ip6_copy);   break;
    default:            skb_free(skb); break;
    }
}

int ipv6_tx(netdev_t *dev, skb_t *skb, const in6_addr_t *dst, uint8_t next_hdr)
{
    uint16_t plen = skb->len;
    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)skb_push(skb, sizeof(ipv6_hdr_t));
    memset(ip6, 0, sizeof(*ip6));
    ip6->ver_tc_fl[0] = 0x60;          /* version 6, traffic class/flow 0 */
    ip6->payload_len  = htons(plen);
    ip6->next_hdr     = next_hdr;
    ip6->hop_limit    = 64;
    ip6->src = ipv6_src_for(dev, dst);
    ip6->dst = *dst;

    /* loopback: a unicast frame addressed to ourselves goes back up the RX path */
    if (in6_eq(dst, &in6addr_loopback) || in6_eq(dst, &dev->ip6_ll) ||
        (!in6_is_unspec(&dev->ip6) && in6_eq(dst, &dev->ip6))) {
        eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
        memcpy(eh->dst, dev->mac, 6);
        memcpy(eh->src, dev->mac, 6);
        eh->type = htons(ETH_P_IPV6);
        netdev_rx_enqueue(dev, skb);
        return 0;
    }

    /* resolve the next-hop link-layer address */
    uint8_t dmac[6];
    if (in6_is_mcast(dst)) {
        dmac[0] = 0x33; dmac[1] = 0x33;
        memcpy(dmac + 2, dst->s6 + 12, 4);   /* 33:33 + low 32 bits of the group */
    } else {
        /* on-link → resolve the destination; off-link → resolve the router */
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
