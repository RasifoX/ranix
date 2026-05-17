#include <kernel/net/icmp.h>
#include <kernel/net/ip.h>
#include <kernel/net/net.h>
#include <kernel/net/socket.h>
#include <kernel/lib/string.h>

void icmp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph)
{
    if (skb->len < (uint16_t)sizeof(icmp_hdr_t)) { skb_free(skb); return; }
    icmp_hdr_t *h = (icmp_hdr_t *)skb->data;

    if (h->type == ICMP_ECHO_REPLY) {
        sock_icmp_deliver(ntohl(iph->src), skb->data, skb->len);
        skb_free(skb);
        return;
    }

    if (h->type != ICMP_ECHO_REQUEST) { skb_free(skb); return; }

    h->type     = ICMP_ECHO_REPLY;
    h->checksum = 0;
    h->checksum = inet_cksum(skb->data, skb->len);

    ip_tx(dev, skb, ntohl(iph->src), IP_PROTO_ICMP);
}
