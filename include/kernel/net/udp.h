#ifndef _KERNEL_NET_UDP_H
#define _KERNEL_NET_UDP_H

#include <kernel/net/net.h>
#include <kernel/net/ip.h>

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

void udp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph);
int  udp_tx(netdev_t *dev, uint32_t dst_ip, uint16_t src_port,
            uint16_t dst_port, const void *data, uint16_t len);

/* Socket layer hook — called by udp_rx to deliver to a bound socket */
void udp_deliver(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, uint16_t len);

#endif
