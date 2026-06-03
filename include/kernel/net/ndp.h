#ifndef _KERNEL_NET_NDP_H
#define _KERNEL_NET_NDP_H

#include <kernel/net/net.h>
#include <kernel/net/ipv6.h>

/* Resolve an on-link IPv6 address to a MAC (RFC 4861 neighbour discovery).
 * Returns 0 with mac_out filled on a cache hit; on a miss sends a Neighbour
 * Solicitation and returns -1 (the caller drops the packet and retries — like
 * arp_lookup). */
int  ndp_lookup(netdev_t *dev, const in6_addr_t *ip6, uint8_t *mac_out);

/* Send a Router Solicitation (ff02::2) to prompt the router's advertisement. */
void ndp_send_rs(netdev_t *dev);

/* ICMPv6 NDP message handlers, dispatched from icmpv6_rx by type. msg points at
 * the ICMPv6 header, len is the ICMPv6 message length. eth_src is the sender's
 * link-layer address (Ethernet source) for the solicitor's neighbour entry. */
void ndp_rx_ns(netdev_t *dev, ipv6_hdr_t *ip6, const void *msg, uint16_t len);
void ndp_rx_na(netdev_t *dev, const void *msg, uint16_t len);
void ndp_rx_ra(netdev_t *dev, ipv6_hdr_t *ip6, const void *msg, uint16_t len);

#endif
