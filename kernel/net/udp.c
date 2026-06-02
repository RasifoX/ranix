#include <kernel/net/udp.h>
#include <kernel/net/ip.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/net.h>
#include <kernel/net/socket.h>
#include <kernel/lib/string.h>

void udp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph)
{
    if (skb->len < (uint16_t)sizeof(udp_hdr_t)) { skb_free(skb); return; }
    udp_hdr_t *uh = (udp_hdr_t *)skb->data;
    uint16_t dst_port = ntohs(uh->dst_port);
    uint16_t src_port = ntohs(uh->src_port);
    uint16_t payload_len = ntohs(uh->len) - sizeof(udp_hdr_t);

    skb_pull(skb, sizeof(udp_hdr_t));
    if (skb->len < payload_len) { skb_free(skb); return; }
    skb->len = payload_len;

    sock_udp_deliver(dev->net_ns, ntohl(iph->src), src_port, dst_port, skb->data, payload_len);
    skb_free(skb);
}

int udp_tx(netdev_t *dev, uint32_t dst_ip, uint16_t src_port,
           uint16_t dst_port, const void *data, uint16_t len)
{
    skb_t *skb = skb_alloc();
    if (!skb) return -1;

    /* copy payload */
    if (len > SKB_DATA_MAX - sizeof(udp_hdr_t) - sizeof(ip_hdr_t) - 14) {
        skb_free(skb); return -1;
    }
    memcpy(skb->data, data, len);
    skb->len = len;

    udp_hdr_t *uh = (udp_hdr_t *)skb_push(skb, sizeof(udp_hdr_t));
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len      = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    uh->checksum = 0;  /* optional for IPv4 UDP */

    return ip_tx(dev, skb, dst_ip, IP_PROTO_UDP);
}

void udp6_rx(netdev_t *dev, skb_t *skb, ipv6_hdr_t *ip6)
{
    if (skb->len < (uint16_t)sizeof(udp_hdr_t)) { skb_free(skb); return; }
    udp_hdr_t *uh = (udp_hdr_t *)skb->data;
    uint16_t dst_port = ntohs(uh->dst_port);
    uint16_t src_port = ntohs(uh->src_port);
    uint16_t payload_len = ntohs(uh->len) - sizeof(udp_hdr_t);

    skb_pull(skb, sizeof(udp_hdr_t));
    if (skb->len < payload_len) { skb_free(skb); return; }
    skb->len = payload_len;

    sock_udp6_deliver(dev->net_ns, &ip6->src, src_port, dst_port, skb->data, payload_len);
    skb_free(skb);
}

int udp6_tx(netdev_t *dev, const in6_addr_t *dst, uint16_t src_port,
            uint16_t dst_port, const void *data, uint16_t len)
{
    skb_t *skb = skb_alloc();
    if (!skb) return -1;

    if (len > SKB_DATA_MAX - sizeof(udp_hdr_t) - sizeof(ipv6_hdr_t) - 14) {
        skb_free(skb); return -1;
    }
    memcpy(skb->data, data, len);
    skb->len = len;

    udp_hdr_t *uh = (udp_hdr_t *)skb_push(skb, sizeof(udp_hdr_t));
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len      = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    uh->checksum = 0;
    /* the UDP checksum is mandatory over IPv6 */
    in6_addr_t src = ipv6_src_for(dev, dst);
    uint16_t c = ipv6_pseudo_cksum(&src, dst, IP6_NH_UDP, uh, skb->len);
    uh->checksum = c ? c : 0xffff;   /* 0 means "no checksum"; send all-ones instead */

    return ipv6_tx(dev, skb, dst, IP6_NH_UDP);
}
