#ifndef _KERNEL_NET_ICMP_H
#define _KERNEL_NET_ICMP_H

#include <kernel/net/net.h>
#include <kernel/net/ip.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

void icmp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph);

#endif
