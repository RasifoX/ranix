#include <kernel/net/icmpv6.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/socket.h>
#include <kernel/lib/string.h>

void icmpv6_rx(netdev_t *dev, skb_t *skb, ipv6_hdr_t *ip6)
{
    if (skb->len < (uint16_t)sizeof(icmp6_hdr_t)) { skb_free(skb); return; }
    icmp6_hdr_t *h = (icmp6_hdr_t *)skb->data;

    if (h->type == ICMP6_ECHO_REPLY) {
        sock_icmp6_deliver(dev->net_ns, &ip6->src, skb->data, skb->len);
        skb_free(skb);
        return;
    }

    if (h->type == ICMP6_ECHO_REQUEST) {
        in6_addr_t reply_src = ip6->dst, reply_dst = ip6->src;
        h->type     = ICMP6_ECHO_REPLY;
        h->checksum = 0;
        h->checksum = ipv6_pseudo_cksum(&reply_src, &reply_dst,
                                        IP6_NH_ICMPV6, skb->data, skb->len);
        ipv6_tx(dev, skb, &reply_dst, IP6_NH_ICMPV6);
        return;
    }

    /* NS/NA/RS/RA arrive here once neighbour discovery lands; drop for now */
    skb_free(skb);
}
