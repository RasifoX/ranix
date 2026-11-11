/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef _KERNEL_NET_NETFILTER_H
#define _KERNEL_NET_NETFILTER_H

#include <kernel/net/net.h>

/* A first-match-wins packet filter, netfilter-inspired but Ranix native. Two
 * chains (INPUT on RX after reassembly, OUTPUT on TX before the packet leaves),
 * each an ordered rule list and a policy for what no rule matched. The kernel
 * boots with no rules and both policies ACCEPT: it is the mechanism. The wall
 * (policy drop) and its doors (accept rules) are Fjord's, the distribution's
 * policy, loaded at boot; `ranix.firewall=off` on the command line refuses
 * them for that one boot (the escape when a rule locks the owner out).
 * Rules match on {protocol, src/dst address, src/dst port, ICMP type, came
 * from this machine ("lo"), belongs to a connection this machine has
 * ("established")} for v4 and v6. */

#define NF_ACCEPT 0
#define NF_DROP   1

#define NF_IN  0
#define NF_OUT 1

extern int nf_active;   /* a rule exists or a policy drops; hooks no-op when zero */

/* One packet as the filter sees it. v4 addresses are host byte order uint32_t,
 * v6 point at the 16 bytes. sport/dport host order, 0 when not TCP/UDP;
 * icmp_type -1 when not ICMP/ICMPv6. looped: it came back from this machine
 * (loopback or our own address). dev: the device it came in on or leaves by. */
typedef struct {
    int         dir;
    int         addr_len;          /* 4 or 16 */
    uint32_t    src4, dst4;
    const void *src6, *dst6;
    uint8_t     proto;
    uint16_t    sport, dport;
    int         icmp_type;
    int         looped;
    netdev_t   *dev;
} nf_pkt_t;

/* Run the filter for one packet. */
int nf_filter(const nf_pkt_t *p);

/* control plane (setsockopt SOL_RANIX_FILTER) */
int nf_ctl_set(const char *line, size_t len);    /* parse+apply one command */
int nf_ctl_dump(char *buf, size_t cap);          /* render ruleset, returns length */

/* L4 ports for TCP/UDP (first two u16s of the header), type for ICMP/ICMPv6 */
static inline void nf_l4(nf_pkt_t *p, const void *transport, uint32_t tlen)
{
    p->sport = p->dport = 0;
    p->icmp_type = -1;
    if ((p->proto == 6 || p->proto == 17) && transport && tlen >= 4) {
        const uint8_t *b = transport;
        p->sport = (uint16_t)((b[0] << 8) | b[1]);
        p->dport = (uint16_t)((b[2] << 8) | b[3]);
    } else if ((p->proto == 1 || p->proto == 58) && transport && tlen >= 1) {
        p->icmp_type = ((const uint8_t *)transport)[0];
    }
}

/* Hooks. Near-free when no rule and no dropping policy exist. transport points
 * at the L4 header and tlen its length; pass tlen 0 if absent. */
static inline int nf_hook_v4(int dir, netdev_t *dev, int looped, uint32_t src, uint32_t dst,
                             uint8_t proto, const void *transport, uint32_t tlen)
{
    if (!nf_active) return NF_ACCEPT;
    nf_pkt_t p = { .dir = dir, .addr_len = 4, .src4 = src, .dst4 = dst, .proto = proto,
                   .looped = looped, .dev = dev };
    nf_l4(&p, transport, tlen);
    return nf_filter(&p);
}

static inline int nf_hook_v6(int dir, netdev_t *dev, int looped, const in6_addr_t *src,
                             const in6_addr_t *dst, uint8_t proto, const void *transport,
                             uint32_t tlen)
{
    if (!nf_active) return NF_ACCEPT;
    nf_pkt_t p = { .dir = dir, .addr_len = 16, .src6 = src, .dst6 = dst, .proto = proto,
                   .looped = looped, .dev = dev };
    nf_l4(&p, transport, tlen);
    return nf_filter(&p);
}

#define nf_in_v4(dev, looped, src, dst, proto, t, tl)  nf_hook_v4(NF_IN, dev, looped, src, dst, proto, t, tl)
#define nf_out_v4(dev, src, dst, proto, t, tl)         nf_hook_v4(NF_OUT, dev, 0, src, dst, proto, t, tl)
#define nf_in_v6(dev, looped, src, dst, proto, t, tl)  nf_hook_v6(NF_IN, dev, looped, src, dst, proto, t, tl)
#define nf_out_v6(dev, src, dst, proto, t, tl)         nf_hook_v6(NF_OUT, dev, 0, src, dst, proto, t, tl)

#endif
