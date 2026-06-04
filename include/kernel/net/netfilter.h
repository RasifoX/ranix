#ifndef _KERNEL_NET_NETFILTER_H
#define _KERNEL_NET_NETFILTER_H

#include <kernel/net/net.h>

/* A stateless first-match-wins packet filter, netfilter-inspired but Ranix
 * native. Two chains (INPUT on RX after reassembly, OUTPUT on TX before the
 * packet leaves), each holding an ordered rule list; default verdict ACCEPT.
 * Rules match on {protocol, src/dst address, src/dst port} for v4 and v6. */

#define NF_ACCEPT 0
#define NF_DROP   1

#define NF_IN  0
#define NF_OUT 1

extern int nf_rules_active;   /* total rule count; hooks no-op when zero */

/* Run the filter for one packet. addr_len is 4 (v4) or 16 (v6); src/dst point
 * at the address in network byte order. proto is the IP protocol / next-header.
 * sport/dport are host byte order, 0 when not a ported protocol. */
int nf_filter(int dir, int addr_len, const void *src, const void *dst,
              uint8_t proto, uint16_t sport, uint16_t dport);

/* control plane (setsockopt SOL_RANIX_FILTER) */
int nf_ctl_set(const char *line, size_t len);    /* parse+apply one command */
int nf_ctl_dump(char *buf, size_t cap);          /* render ruleset, returns length */

/* Hooks. Near-free when no rules exist. transport points at the L4 header
 * (TCP/UDP) and tlen its length, used only to read ports; pass tlen 0 if absent.
 * Return NF_ACCEPT to continue, NF_DROP to drop. */
static inline int nf_in_v4(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *transport, uint32_t tlen);
static inline int nf_out_v4(uint32_t src, uint32_t dst, uint8_t proto,
                            const void *transport, uint32_t tlen);
static inline int nf_in_v6(const in6_addr_t *src, const in6_addr_t *dst,
                           uint8_t proto, const void *transport, uint32_t tlen);
static inline int nf_out_v6(const in6_addr_t *src, const in6_addr_t *dst,
                            uint8_t proto, const void *transport, uint32_t tlen);

/* L4 src/dst port for TCP/UDP both live in the first two u16s of the header */
static inline void nf_ports(uint8_t proto, const void *transport, uint32_t tlen,
                            uint16_t *sport, uint16_t *dport)
{
    *sport = *dport = 0;
    if ((proto == 6 || proto == 17) && transport && tlen >= 4) {
        const uint16_t *p = transport;
        *sport = ntohs(p[0]);
        *dport = ntohs(p[1]);
    }
}

static inline int nf_in_v4(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *transport, uint32_t tlen)
{
    if (!nf_rules_active) return NF_ACCEPT;
    uint16_t sp, dp; nf_ports(proto, transport, tlen, &sp, &dp);
    uint32_t s = htonl(src), d = htonl(dst);
    return nf_filter(NF_IN, 4, &s, &d, proto, sp, dp);
}

static inline int nf_out_v4(uint32_t src, uint32_t dst, uint8_t proto,
                            const void *transport, uint32_t tlen)
{
    if (!nf_rules_active) return NF_ACCEPT;
    uint16_t sp, dp; nf_ports(proto, transport, tlen, &sp, &dp);
    uint32_t s = htonl(src), d = htonl(dst);
    return nf_filter(NF_OUT, 4, &s, &d, proto, sp, dp);
}

static inline int nf_in_v6(const in6_addr_t *src, const in6_addr_t *dst,
                           uint8_t proto, const void *transport, uint32_t tlen)
{
    if (!nf_rules_active) return NF_ACCEPT;
    uint16_t sp, dp; nf_ports(proto, transport, tlen, &sp, &dp);
    return nf_filter(NF_IN, 16, src, dst, proto, sp, dp);
}

static inline int nf_out_v6(const in6_addr_t *src, const in6_addr_t *dst,
                            uint8_t proto, const void *transport, uint32_t tlen)
{
    if (!nf_rules_active) return NF_ACCEPT;
    uint16_t sp, dp; nf_ports(proto, transport, tlen, &sp, &dp);
    return nf_filter(NF_OUT, 16, src, dst, proto, sp, dp);
}

#endif
