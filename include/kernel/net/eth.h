#ifndef _KERNEL_NET_ETH_H
#define _KERNEL_NET_ETH_H

#include <kernel/net/net.h>

#define ETH_ALEN    6
#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ETH_P_IPV6  0x86dd

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;  /* big-endian */
} __attribute__((packed)) eth_hdr_t;

void   eth_rx(netdev_t *dev, skb_t *skb);
/* Allocate skb with Ethernet header pre-filled (dst_mac=NULL → broadcast) */
skb_t *eth_alloc(netdev_t *dev, const uint8_t *dst_mac, uint16_t ethertype);
int    eth_tx(netdev_t *dev, skb_t *skb);

extern const uint8_t eth_bcast[ETH_ALEN];

#endif
