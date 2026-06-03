#include <kernel/net/ndp.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/icmpv6.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>

/* ── neighbour cache ─────────────────────────────────────── */

#define NDP_CACHE_SIZE 64

typedef struct { in6_addr_t ip; uint8_t mac[6]; int valid; uint32_t age; } ndp_entry_t;
static ndp_entry_t ndp_cache[NDP_CACHE_SIZE];
static uint32_t    ndp_clock;

static void ndp_cache_update(const in6_addr_t *ip, const uint8_t *mac)
{
    ndp_clock++;
    for (int i = 0; i < NDP_CACHE_SIZE; i++)
        if (ndp_cache[i].valid && in6_eq(&ndp_cache[i].ip, ip)) {
            memcpy(ndp_cache[i].mac, mac, 6);
            ndp_cache[i].age = ndp_clock;
            return;
        }
    for (int i = 0; i < NDP_CACHE_SIZE; i++)
        if (!ndp_cache[i].valid) {
            ndp_cache[i].ip = *ip; ndp_cache[i].age = ndp_clock;
            memcpy(ndp_cache[i].mac, mac, 6);
            ndp_cache[i].valid = 1;
            return;
        }
    int oldest = 0;
    for (int i = 1; i < NDP_CACHE_SIZE; i++)
        if (ndp_cache[i].age < ndp_cache[oldest].age) oldest = i;
    ndp_cache[oldest].ip = *ip; ndp_cache[oldest].age = ndp_clock;
    memcpy(ndp_cache[oldest].mac, mac, 6);
    ndp_cache[oldest].valid = 1;
}

static int ndp_cache_get(const in6_addr_t *ip, uint8_t *mac_out)
{
    for (int i = 0; i < NDP_CACHE_SIZE; i++)
        if (ndp_cache[i].valid && in6_eq(&ndp_cache[i].ip, ip)) {
            memcpy(mac_out, ndp_cache[i].mac, 6);
            return 0;
        }
    return -1;
}

/* ── ND option helpers ───────────────────────────────────── */

#define ND_OPT_SLLA  1   /* source link-layer address */
#define ND_OPT_TLLA  2   /* target link-layer address */
#define ND_OPT_PREFIX 3

/* Find an option of the given type in [opts, opts+len); return its data (after
 * the 2-byte type/length header) or NULL. */
static const uint8_t *nd_find_opt(const uint8_t *opts, uint16_t len, uint8_t want)
{
    uint16_t off = 0;
    while (off + 2 <= len) {
        uint8_t t = opts[off];
        uint16_t olen = (uint16_t)opts[off + 1] * 8;   /* length in 8-byte units */
        if (olen == 0 || off + olen > len) break;
        if (t == want) return opts + off + 2;
        off += olen;
    }
    return NULL;
}

/* ── solicitation / advertisement TX ─────────────────────── */

static void ndp_solicited_node(const in6_addr_t *target, in6_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->s6[0]  = 0xff; out->s6[1] = 0x02;
    out->s6[11] = 0x01; out->s6[12] = 0xff;
    out->s6[13] = target->s6[13];
    out->s6[14] = target->s6[14];
    out->s6[15] = target->s6[15];
}

static void ndp_send_ns(netdev_t *dev, const in6_addr_t *target)
{
    skb_t *skb = skb_alloc();
    if (!skb) return;
    uint8_t *m = skb->data;
    memset(m, 0, 32);
    m[0] = ICMP6_NEIGH_SOL;          /* type 135 */
    memcpy(m + 8, target->s6, 16);   /* target address */
    m[24] = ND_OPT_SLLA; m[25] = 1;  /* source link-layer addr option */
    memcpy(m + 26, dev->mac, 6);
    skb->len = 32;

    in6_addr_t sn;
    ndp_solicited_node(target, &sn);
    in6_addr_t src = ipv6_src_for(dev, &sn);
    icmp6_hdr_t *h = (icmp6_hdr_t *)m;
    h->checksum = 0;
    h->checksum = ipv6_pseudo_cksum(&src, &sn, IP6_NH_ICMPV6, m, skb->len);
    ipv6_tx(dev, skb, &sn, IP6_NH_ICMPV6);
}

static void ndp_send_na(netdev_t *dev, const in6_addr_t *target, const in6_addr_t *dst)
{
    skb_t *skb = skb_alloc();
    if (!skb) return;
    uint8_t *m = skb->data;
    memset(m, 0, 32);
    m[0] = ICMP6_NEIGH_ADV;          /* type 136 */
    ((icmp6_hdr_t *)m)->rest = htonl(0x60000000);  /* Solicited + Override */
    memcpy(m + 8, target->s6, 16);
    m[24] = ND_OPT_TLLA; m[25] = 1;
    memcpy(m + 26, dev->mac, 6);
    skb->len = 32;

    in6_addr_t src = ipv6_src_for(dev, dst);
    icmp6_hdr_t *h = (icmp6_hdr_t *)m;
    h->checksum = 0;
    h->checksum = ipv6_pseudo_cksum(&src, dst, IP6_NH_ICMPV6, m, skb->len);
    ipv6_tx(dev, skb, dst, IP6_NH_ICMPV6);
}

void ndp_send_rs(netdev_t *dev)
{
    skb_t *skb = skb_alloc();
    if (!skb) return;
    uint8_t *m = skb->data;
    memset(m, 0, 16);
    m[0] = ICMP6_ROUTER_SOL;          /* type 133 */
    m[8] = ND_OPT_SLLA; m[9] = 1;
    memcpy(m + 10, dev->mac, 6);
    skb->len = 16;

    in6_addr_t all_routers;
    memset(&all_routers, 0, sizeof(all_routers));
    all_routers.s6[0] = 0xff; all_routers.s6[1] = 0x02; all_routers.s6[15] = 0x02;
    in6_addr_t src = ipv6_src_for(dev, &all_routers);
    icmp6_hdr_t *h = (icmp6_hdr_t *)m;
    h->checksum = 0;
    h->checksum = ipv6_pseudo_cksum(&src, &all_routers, IP6_NH_ICMPV6, m, skb->len);
    ipv6_tx(dev, skb, &all_routers, IP6_NH_ICMPV6);
}

/* ── resolution ──────────────────────────────────────────── */

int ndp_lookup(netdev_t *dev, const in6_addr_t *ip6, uint8_t *mac_out)
{
    if (ndp_cache_get(ip6, mac_out) == 0) return 0;
    ndp_send_ns(dev, ip6);
    return -1;
}

/* ── message RX ──────────────────────────────────────────── */

void ndp_rx_ns(netdev_t *dev, ipv6_hdr_t *ip6, const void *msg, uint16_t len)
{
    if (len < 24) return;
    const uint8_t *m = msg;
    in6_addr_t target;
    memcpy(target.s6, m + 8, 16);

    /* record the solicitor so the NA (and later traffic) can reach it */
    const uint8_t *slla = nd_find_opt(m + 24, (uint16_t)(len - 24), ND_OPT_SLLA);
    if (slla && !in6_is_unspec(&ip6->src)) ndp_cache_update(&ip6->src, slla);

    /* answer only if the solicitation targets one of our addresses */
    if (in6_eq(&target, &dev->ip6_ll) || in6_eq(&target, &dev->ip6))
        ndp_send_na(dev, &target, &ip6->src);
}

void ndp_rx_na(netdev_t *dev, const void *msg, uint16_t len)
{
    (void)dev;
    if (len < 24) return;
    const uint8_t *m = msg;
    in6_addr_t target;
    memcpy(target.s6, m + 8, 16);
    const uint8_t *tlla = nd_find_opt(m + 24, (uint16_t)(len - 24), ND_OPT_TLLA);
    if (tlla) ndp_cache_update(&target, tlla);
}

void ndp_rx_ra(netdev_t *dev, ipv6_hdr_t *ip6, const void *msg, uint16_t len)
{
    if (len < 16) return;
    const uint8_t *m = msg;
    uint16_t router_lifetime = ((uint16_t)m[6] << 8) | m[7];

    const uint8_t *opts = m + 16;
    uint16_t       olen = (uint16_t)(len - 16);

    /* SLAAC: form a global address from the advertised /64 prefix + EUI-64 */
    const uint8_t *pfx = nd_find_opt(opts, olen, ND_OPT_PREFIX);
    if (pfx) {
        /* pfx[0]=prefix_len, pfx[14..29]=prefix (after the 2-byte opt header the
         * prefix option body is: prefix_len, flags, valid(4), preferred(4),
         * reserved(4), prefix(16)) */
        in6_addr_t ll = dev->ip6_ll;        /* already EUI-64 from the MAC */
        in6_addr_t g;
        memcpy(g.s6, pfx + 14, 8);          /* high 64 bits: the prefix */
        memcpy(g.s6 + 8, ll.s6 + 8, 8);     /* low 64 bits: our EUI-64 */
        dev->ip6 = g;
    }

    /* default router: the advertisement's source (a link-local address) */
    if (router_lifetime > 0) {
        dev->ip6_router     = ip6->src;
        dev->ip6_has_router = 1;
        const uint8_t *slla = nd_find_opt(opts, olen, ND_OPT_SLLA);
        if (slla) ndp_cache_update(&ip6->src, slla);
    }
}
