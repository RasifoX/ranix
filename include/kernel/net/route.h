/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef _KERNEL_NET_ROUTE_H
#define _KERNEL_NET_ROUTE_H

#include <kernel/net/net.h>

/* Who put a route in the table: the rtnetlink "proto" numbers, so `ip route`
 * prints the same words as on Linux. */
#define RT_PROTO_KERNEL  2   /* connected route of an interface address */
#define RT_PROTO_BOOT    3   /* default route learned by the kernel's DHCP client */
#define RT_PROTO_STATIC  4   /* added by root (`ip route add`) */

#define ROUTE4_MAX 64        /* the whole table, every namespace together */

typedef struct route4 {
    uint32_t  dst;       /* host order, already masked */
    uint32_t  mask;      /* contiguous; 0 = default route */
    uint32_t  gw;        /* next hop, 0 = destination is on the link */
    uint32_t  src;       /* preferred source address, 0 = the device address */
    uint32_t  metric;    /* lower wins between routes of the same prefix */
    uint8_t   proto;     /* RT_PROTO_*, as given (iproute2 adds with "boot") */
    uint8_t   automatic; /* made by route4_sync_dev, which may remake or drop it */
    netdev_t *dev;
} route4_t;

/* Add a route. An entry with the same dst/mask/metric is the "same route":
 *   RT_ADD      it must not exist (-EEXIST)       `ip route add`
 *   RT_REPLACE  overwrite it, or add              `ip route replace`
 *   RT_CHANGE   overwrite it; it must exist (-ENOENT)  `ip route change`
 * -ENETDOWN if the device is down. */
#define RT_ADD     0
#define RT_REPLACE 1
#define RT_CHANGE  2
int  route4_add(const route4_t *r, int mode);
/* Delete the first route of namespace ns matching r->dst/r->mask and every
 * other field of r that is non-zero (gw, dev, metric, proto). -ESRCH if none. */
int  route4_del(net_namespace_t *ns, const route4_t *r);
/* Copy out the i-th route of namespace ns (0, 1, ...). 0 = copied, -1 = no more. */
int  route4_get(net_namespace_t *ns, int i, route4_t *out);

/* The device's address or gateway changed (driver, DHCP) or it came up:
 * rebuild its connected route and its DHCP default; leave root's routes alone. */
void route4_sync_dev(netdev_t *dev);
/* Remove every route through dev (link set down). */
void route4_flush_dev(netdev_t *dev);

/* The egress for dst in namespace ns: the device, and in *nexthop the address to
 * resolve on it (a gateway, or dst itself). oif != NULL restricts the choice to
 * that device (SO_BINDTODEVICE). NULL = no route. */
netdev_t *route4_lookup(net_namespace_t *ns, uint32_t dst, netdev_t *oif, uint32_t *nexthop);

/* Default-route metric for a device's DHCP gateway: 100 + its interface index,
 * so the device registered first (eth0 before wlan0) wins a tie, as before the
 * table existed, and `ip route add default ... metric 50` still overrides it. */
#define RT_DHCP_METRIC_BASE 100

#endif
