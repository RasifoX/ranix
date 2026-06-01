#include <kernel/net/ip.h>
#include <kernel/net/eth.h>
#include <kernel/net/arp.h>
#include <kernel/net/icmp.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/lib/string.h>

static uint16_t ip_id;

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

    ip_hdr_t iph_copy = *iph;    /* save before pull */
    skb_pull(skb, ihl);

    switch (iph->proto) {
    case IP_PROTO_ICMP: icmp_rx(dev, skb, &iph_copy); break;
    case IP_PROTO_UDP:  udp_rx(dev, skb, &iph_copy);  break;
    case IP_PROTO_TCP:  tcp_rx(dev, skb, &iph_copy);  break;
    default:            skb_free(skb); break;
    }
}

int ip_tx(netdev_t *dev, skb_t *skb, uint32_t dst_ip, uint8_t proto)
{
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

    /* loopback: a frame addressed to ourselves (our own IP, or 127/8) is
     * delivered back up the receive path instead of going on the wire — the
     * host NIC/slirp would not reflect our own packet back to us. */
    if (dst_ip == dev->ip || (dst_ip >> 24) == 127) {
        eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
        memcpy(eh->dst, dev->mac, 6);
        memcpy(eh->src, dev->mac, 6);
        eh->type = htons(ETH_P_IP);
        netdev_rx_enqueue(dev, skb);
        return 0;
    }

    /* resolve next-hop MAC */
    uint32_t nexthop = dst_ip;
    if ((dst_ip & dev->mask) != (dev->ip & dev->mask))
        nexthop = dev->gw;  /* off-link → use gateway */

    uint8_t dst_mac[6];
    if (arp_lookup(dev, nexthop, dst_mac) != 0) {
        skb_free(skb);
        return -1;  /* ARP miss; packet dropped, request sent */
    }

    eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, dev->mac, 6);
    eh->type = htons(ETH_P_IP);
    return dev->tx(dev, skb);
}
