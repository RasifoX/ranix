#ifndef _KERNEL_NET_IP_H
#define _KERNEL_NET_IP_H

#include <kernel/net/net.h>

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ip_hdr_t;

void ip_rx(netdev_t *dev, skb_t *skb);
/* Prepend IP header and send. skb->data/len = payload on entry. */
int  ip_tx(netdev_t *dev, skb_t *skb, uint32_t dst, uint8_t proto);

#endif
