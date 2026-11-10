/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── IPv4 routing table ──────────────────────────────────────────────────────
 * Which device a packet leaves through, and which neighbour it is handed to.
 *
 * One small table holds the routes of every namespace; a route names its
 * device, and the device names its namespace. Three kinds of entry:
 *   connected  dev's own subnet, from its address and mask (proto kernel)
 *   DHCP       default via the gateway the kernel's DHCP client learned (proto boot)
 *   static     whatever root added with `ip route add` (proto static)
 * The first two are "automatic": they follow the device (route4_sync_dev).
 * Root's routes stay until root deletes them or the device goes down; setting
 * the device up again brings back only the automatic ones, as on Linux.
 *
 * Lookup walks the whole table: the longest matching prefix wins, then the
 * lower metric, then the older entry. That is O(n) per packet, and n is a
 * handful (two per interface plus what root adds), so a trie would only hide
 * the rule. Routes through a device that is down, or has lost its carrier,
 * are passed over: pull the cable and the Wi-Fi route carries the traffic.
 *
 * 127/8 and the host's own addresses are not in the table: they always loop
 * back (ip_xmit), as Linux's local table does.
 * ───────────────────────────────────────────────────────────────────────── */

#include <kernel/net/route.h>
#include <kernel/sync.h>
#include <kernel/errno.h>

static route4_t   table[ROUTE4_MAX];
static int        used[ROUTE4_MAX];        /* slot holds a route */
static uint32_t   age[ROUTE4_MAX];         /* insertion order, for ties */
static uint32_t   next_age = 1;
static spinlock_t table_lock = SPINLOCK_INIT;

/* Remove dev's routes: all of them, or (only_auto) just the connected and
 * DHCP ones the kernel made; table_lock held. */
static void drop_where(netdev_t *dev, int only_auto)
{
    for (int i = 0; i < ROUTE4_MAX; i++) {
        if (!used[i] || table[i].dev != dev) continue;
        if (only_auto && !table[i].automatic) continue;
        used[i] = 0;
    }
}

/* Slot of the route with this dst/mask/metric, or -1; table_lock held. An
 * automatic route is only ever the same as its own device's: two devices on
 * one subnet each keep their connected route. */
static int find_same(const route4_t *r)
{
    for (int i = 0; i < ROUTE4_MAX; i++)
        if (used[i] && table[i].dst == r->dst && table[i].mask == r->mask &&
            table[i].metric == r->metric && table[i].dev->net_ns == r->dev->net_ns &&
            (!r->automatic || table[i].dev == r->dev))
            return i;
    return -1;
}

/* table_lock held */
static int put(const route4_t *r, int mode)
{
    int i = find_same(r);
    if (i >= 0 && mode == RT_ADD) return -EEXIST;
    if (i < 0 && mode == RT_CHANGE) return -ENOENT;
    if (i < 0)
        for (i = 0; i < ROUTE4_MAX && used[i]; i++) ;
    if (i == ROUTE4_MAX) return -ENOBUFS;
    table[i] = *r;
    table[i].dst = r->dst & r->mask;
    used[i] = 1;
    age[i] = next_age++;
    return 0;
}

int route4_add(const route4_t *r, int mode)
{
    if (!r->dev) return -ENODEV;
    if (r->dev->admin_down) return -ENETDOWN;
    uint64_t f = spin_lock_irq(&table_lock);
    int rc = put(r, mode);
    spin_unlock_irq(&table_lock, f);
    return rc;
}

int route4_del(net_namespace_t *ns, const route4_t *r)
{
    int rc = -ESRCH;
    uint64_t f = spin_lock_irq(&table_lock);
    for (int i = 0; i < ROUTE4_MAX; i++) {
        route4_t *t = &table[i];
        if (!used[i] || t->dev->net_ns != ns) continue;
        if (t->dst != (r->dst & r->mask) || t->mask != r->mask) continue;
        if (r->dev    && t->dev    != r->dev)    continue;
        if (r->gw     && t->gw     != r->gw)     continue;
        if (r->metric && t->metric != r->metric) continue;
        if (r->proto  && t->proto  != r->proto)  continue;
        used[i] = 0;
        rc = 0;
        break;
    }
    spin_unlock_irq(&table_lock, f);
    return rc;
}

int route4_get(net_namespace_t *ns, int n, route4_t *out)
{
    int rc = -1;
    uint64_t f = spin_lock_irq(&table_lock);
    for (int i = 0; i < ROUTE4_MAX; i++) {
        if (!used[i] || table[i].dev->net_ns != ns) continue;
        if (n-- == 0) { *out = table[i]; rc = 0; break; }
    }
    spin_unlock_irq(&table_lock, f);
    return rc;
}

void route4_sync_dev(netdev_t *dev)
{
    if (dev == &dev->net_ns->lo) return;   /* loopback is not routed: see above */
    uint64_t f = spin_lock_irq(&table_lock);
    drop_where(dev, 1);
    if (!dev->admin_down && dev->ip) {
        /* the connected route carries the device's metric as well: with two
         * devices on one subnet (cable and Wi-Fi at home) both routes stay,
         * and the one registered first is preferred, as for the default */
        uint32_t metric = RT_DHCP_METRIC_BASE + (uint32_t)net_ifindex(dev);
        route4_t c = { .dst = dev->ip & dev->mask, .mask = dev->mask, .src = dev->ip,
                       .metric = metric, .proto = RT_PROTO_KERNEL, .automatic = 1, .dev = dev };
        put(&c, RT_REPLACE);
        /* a gateway outside the device's own subnet could never be reached */
        if (dev->gw && (dev->gw & dev->mask) == (dev->ip & dev->mask)) {
            route4_t d = { .gw = dev->gw, .proto = RT_PROTO_BOOT, .automatic = 1, .dev = dev,
                           .metric = metric };
            put(&d, RT_REPLACE);
        }
    }
    spin_unlock_irq(&table_lock, f);
}

void route4_flush_dev(netdev_t *dev)
{
    uint64_t f = spin_lock_irq(&table_lock);
    drop_where(dev, 0);
    spin_unlock_irq(&table_lock, f);
}

netdev_t *route4_lookup(net_namespace_t *ns, uint32_t dst, netdev_t *oif, uint32_t *nexthop)
{
    *nexthop = dst;
    /* loopback and our own addresses (see the comment at the top) */
    if ((dst >> 24) == 127) return &ns->lo;
    netdev_t *d;
    for (int i = 0; (d = net_dev_in(ns, i)); i++)
        if (d->ip && d->ip == dst) return d;

    int best = -1;
    uint64_t f = spin_lock_irq(&table_lock);
    for (int i = 0; i < ROUTE4_MAX; i++) {
        route4_t *r = &table[i];
        if (!used[i] || r->dev->net_ns != ns || !netdev_usable(r->dev)) continue;
        if (oif && r->dev != oif) continue;
        if ((dst & r->mask) != r->dst) continue;
        if (best >= 0) {
            route4_t *b = &table[best];
            /* host-order contiguous masks: a longer prefix is a larger number */
            if (r->mask < b->mask) continue;
            if (r->mask == b->mask && r->metric > b->metric) continue;
            if (r->mask == b->mask && r->metric == b->metric && age[i] > age[best]) continue;
        }
        best = i;
    }
    netdev_t *out = NULL;
    if (best >= 0) {
        out = table[best].dev;
        if (table[best].gw) *nexthop = table[best].gw;
    }
    spin_unlock_irq(&table_lock, f);

    /* the all-ones broadcast on a socket bound to a device goes out on that
     * link even without a route, as on Linux (DHCP clients rely on it) */
    if (!out && oif && dst == 0xffffffffu && netdev_usable(oif)) out = oif;
    return out;
}
