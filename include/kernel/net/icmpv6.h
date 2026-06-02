#ifndef _KERNEL_NET_ICMPV6_H
#define _KERNEL_NET_ICMPV6_H

#include <kernel/net/net.h>
#include <kernel/net/ipv6.h>

#define ICMP6_ECHO_REQUEST   128
#define ICMP6_ECHO_REPLY     129
#define ICMP6_ROUTER_SOL     133
#define ICMP6_ROUTER_ADV     134
#define ICMP6_NEIGH_SOL      135
#define ICMP6_NEIGH_ADV      136

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t rest;       /* echo: id(16)|seq(16); NS/NA: flags */
} __attribute__((packed)) icmp6_hdr_t;

void icmpv6_rx(netdev_t *dev, skb_t *skb, ipv6_hdr_t *ip6);

#endif
