#ifndef _KERNEL_NET_IPV6_H
#define _KERNEL_NET_IPV6_H

#include <kernel/net/net.h>   /* in6_addr_t lives here so netdev_t can embed it */

/* next-header values reuse the IPv4 protocol numbers, plus ICMPv6 */
#define IP6_NH_TCP      6
#define IP6_NH_UDP      17
#define IP6_NH_ICMPV6   58

typedef struct {
    uint8_t    ver_tc_fl[4]; /* version(4) | traffic-class(8) | flow-label(20) */
    uint16_t   payload_len;  /* big-endian: bytes after this 40-byte header */
    uint8_t    next_hdr;     /* upper-layer protocol (no extension headers yet) */
    uint8_t    hop_limit;
    in6_addr_t src;
    in6_addr_t dst;
} __attribute__((packed)) ipv6_hdr_t;

extern const in6_addr_t in6addr_loopback;   /* ::1 */
extern const in6_addr_t in6addr_any;        /* :: */

static inline int in6_eq(const in6_addr_t *a, const in6_addr_t *b)
{ for (int i = 0; i < 16; i++) if (a->s6[i] != b->s6[i]) return 0; return 1; }

static inline int in6_is_mcast(const in6_addr_t *a) { return a->s6[0] == 0xff; }
static inline int in6_is_unspec(const in6_addr_t *a)   /* :: */
{ for (int i = 0; i < 16; i++) if (a->s6[i]) return 0; return 1; }
static inline int in6_is_ll(const in6_addr_t *a)     /* fe80::/10 */
{ return a->s6[0] == 0xfe && (a->s6[1] & 0xc0) == 0x80; }

void ipv6_rx(netdev_t *dev, skb_t *skb);
/* Prepend the IPv6 header and send. skb->data/len = payload on entry. */
int  ipv6_tx(netdev_t *dev, skb_t *skb, const in6_addr_t *dst, uint8_t next_hdr);

/* one's-complement sum over the IPv6 pseudo-header + upper-layer segment */
uint16_t ipv6_pseudo_cksum(const in6_addr_t *src, const in6_addr_t *dst,
                           uint8_t next_hdr, const void *seg, uint16_t seg_len);

/* derive the EUI-64 link-local address (fe80::...) from a MAC */
void ipv6_linklocal_from_mac(const uint8_t mac[6], in6_addr_t *out);

/* pick the source address this device would use to reach dst */
in6_addr_t ipv6_src_for(netdev_t *dev, const in6_addr_t *dst);

#endif
