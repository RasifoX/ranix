#ifndef _KERNEL_NET_ARP_H
#define _KERNEL_NET_ARP_H

#include <kernel/net/net.h>

void arp_rx(netdev_t *dev, skb_t *skb);
/* Lookup MAC for ip; returns 0 on hit (mac_out filled), -1 on miss (sends request) */
int  arp_lookup(netdev_t *dev, uint32_t ip, uint8_t *mac_out);

#endif
