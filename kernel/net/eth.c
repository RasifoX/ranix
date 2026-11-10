/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/net/eth.h>
#include <kernel/net/socket.h>
#include <kernel/net/arp.h>
#include <kernel/net/ip.h>
#include <kernel/net/ipv6.h>
#include <kernel/lib/string.h>

const uint8_t eth_bcast[ETH_ALEN] = {0xff,0xff,0xff,0xff,0xff,0xff};

void eth_rx(netdev_t *dev, skb_t *skb)
{
    if (skb->len < (uint16_t)sizeof(eth_hdr_t)) { skb_free(skb); return; }
    eth_hdr_t *eh = (eth_hdr_t *)skb->data;
    uint16_t type = ntohs(eh->type);
    sock_packet_deliver(dev, skb->data, skb->len);   /* AF_PACKET sockets see it first */
    skb_pull(skb, sizeof(eth_hdr_t));

    if      (type == ETH_P_ARP)  arp_rx(dev, skb);
    else if (type == ETH_P_IP)   ip_rx(dev, skb);
    else if (type == ETH_P_IPV6) ipv6_rx(dev, skb);
    else                         skb_free(skb);
}

skb_t *eth_alloc(netdev_t *dev, const uint8_t *dst_mac, uint16_t ethertype)
{
    skb_t *skb = skb_alloc();
    if (!skb) return NULL;
    eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
    memcpy(eh->dst, dst_mac ? dst_mac : eth_bcast, ETH_ALEN);
    memcpy(eh->src, dev->mac, ETH_ALEN);
    eh->type = htons(ethertype);
    return skb;
}

int eth_tx(netdev_t *dev, skb_t *skb)
{
    return netdev_tx(dev, skb);
}
