/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef _KERNEL_NET_NET_H
#define _KERNEL_NET_NET_H

#include <uapi/types.h>
#include <kernel/sync.h>
#include <kernel/config.h>

/* ── byte-order helpers ──────────────────────────────────── */

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
#define htons(x)  bswap16((uint16_t)(x))
#define ntohs(x)  bswap16((uint16_t)(x))
#define htonl(x)  bswap32((uint32_t)(x))
#define ntohl(x)  bswap32((uint32_t)(x))

/* 128-bit IPv6 address in network byte order (see ipv6.h for helpers) */
typedef struct { uint8_t s6[16]; } in6_addr_t;

/* ── socket buffer ───────────────────────────────────────── */

/* Room to prepend the full header stack without underflowing buf[]: Ethernet
 * (14) + IP (20) + TCP with the largest option set we emit (20 + 40 = 60) = 94.
 * 128 leaves margin and keeps the buffer nicely aligned. */
#define SKB_HEADROOM  CONFIG_SKB_HEADROOM
#define SKB_DATA_MAX  CONFIG_SKB_DATA_MAX

typedef struct skb {
    uint8_t     buf[SKB_HEADROOM + SKB_DATA_MAX];
    uint8_t    *data;   /* current start of packet data */
    uint16_t    len;    /* bytes from data */
    /* large reassembled datagrams (> SKB_DATA_MAX) live in this heap buffer
     * instead of buf[]; data points into it. NULL for ordinary skbs. */
    uint8_t    *ext;
    uint32_t    ext_len;
    uint32_t    src_ip;     /* datagram source — for recvfrom on a UDP socket */
    in6_addr_t  src_ip6;    /* IPv6 datagram source */
    uint16_t    src_port;
    struct skb *next;
    /* SCM_RIGHTS: in-flight passed files (AF_UNIX fd-passing) */
#define SKB_SCM_MAX CONFIG_SKB_SCM_MAX
    void       *scm_files[SKB_SCM_MAX];
    uint8_t     scm_nfiles;
    void       *cg;         /* cgroup paying while it waits in a receive queue */
    /* AF_UNIX sender, for SCM_CREDENTIALS when the receiver set SO_PASSCRED */
    int32_t     cred_pid;
    uint32_t    cred_uid, cred_gid;
} skb_t;

#define SKB_TRUESIZE(skb) ((long)sizeof(skb_t) + (long)(skb)->ext_len)

skb_t  *skb_alloc(void);
/* skb whose data buffer holds up to cap bytes — backed by buf[] when cap fits,
 * else by a heap allocation (skb->ext). For reassembled datagrams > SKB_DATA_MAX. */
skb_t  *skb_alloc_big(uint32_t cap);
void    skb_free(skb_t *skb);

/* push/pull helpers */
static inline void *skb_push(skb_t *skb, uint16_t n)
{ skb->data -= n; skb->len += n; return skb->data; }
static inline void *skb_pull(skb_t *skb, uint16_t n)
{ skb->data += n; skb->len -= n; return skb->data; }

/* ── network device ──────────────────────────────────────── */

struct netdev;

typedef struct netdev {
    char        name[8];
    uint8_t     mac[6];
    uint32_t    ip;     /* host byte order, e.g. 10<<24|0<<16|2<<8|15 */
    uint32_t    gw;
    uint32_t    mask;
    in6_addr_t  ip6_ll;     /* link-local address (fe80::, EUI-64 from MAC) */
    in6_addr_t  ip6;        /* global/ULA address (SLAAC or static), :: if unset */
    in6_addr_t  ip6_router; /* default router (from a Router Advertisement) */
    int         ip6_has_router;

    int       (*tx)(struct netdev *, skb_t *);
    void      (*poll)(struct netdev *);   /* hardware poll, set by driver */
    int       (*link_check)(struct netdev *); /* 1=carrier up, 0=down; NULL=always up */
    int         link_up;                  /* current carrier state (link watcher) */
    uint64_t    renew_tick;               /* when to renew the DHCP lease, 0 = none */
    void       *priv;

    /* /proc/net/dev. Atomic: TX runs from syscalls, RX from any polling CPU. */
    volatile uint64_t rx_packets, rx_bytes, rx_dropped;
    volatile uint64_t tx_packets, tx_bytes, tx_dropped;

    /* RX queue — filled by poll, drained by net_poll */
    spinlock_t  rx_lock;
    volatile int rx_claimed;   /* DHCP owns the RX queue; net_poll must not drain it */
    skb_t      *rx_head;
    skb_t      *rx_tail;

    struct net_namespace *net_ns;     /* owning network namespace */
} netdev_t;

/* ── network namespace ───────────────────────────────────────
 * Each holds its own set of devices (a fresh ns has only its private
 * loopback); sockets and device lookups are scoped to a namespace so two
 * namespaces are isolated network stacks. */
/* One table per namespace, so names cannot cross and neither can a full table. */
#define UNIX_REG_MAX 64
typedef struct { char path[108]; int pathlen; struct socket *sock; } unix_bind_t;

typedef struct net_namespace {
    int           refcount;
    unsigned long inum;
    netdev_t      lo;                 /* this namespace's private loopback */
    unix_bind_t   unix_reg[UNIX_REG_MAX];
    struct net_namespace *dying_next; /* net_ns_put to net_poll */
} net_namespace_t;

extern net_namespace_t init_net_ns;
net_namespace_t *current_net_ns(void);
net_namespace_t *net_ns_get(net_namespace_t *ns);
void             net_ns_put(net_namespace_t *ns);
net_namespace_t *net_ns_create(void);   /* new ns with its own loopback */

struct vfs_file;
net_namespace_t *nsfile_to_net(struct vfs_file *f);

void net_register(netdev_t *dev);
netdev_t *net_dev(int idx);          /* idx-th device in the current namespace */
netdev_t *net_dev_in(net_namespace_t *ns, int idx);
netdev_t *net_dev_by_ip(uint32_t ip);
netdev_t *net_default_dev(void);     /* real NIC if present, else loopback */
netdev_t *net_route_dev(uint32_t dst_ip);  /* egress device for a destination */
netdev_t *net_route_dev6(const in6_addr_t *dst);

/* Called from scheduler tick / idle loop — drains RX queues */
void net_poll(void);

/* Wake the RX softirq thread — called from a driver's RX interrupt handler */
void net_rx_wake(void);

/* Push received skb onto dev's RX queue */
void netdev_rx_enqueue(netdev_t *dev, skb_t *skb);
void netdev_rx_drop(netdev_t *dev);              /* driver discarded a frame */
void netdev_count_tx(netdev_t *dev, uint32_t len);
/* Every frame leaves through here, not dev->tx: this is what counts it. */
int  netdev_tx(netdev_t *dev, skb_t *skb);
skb_t *netdev_rx_dequeue(netdev_t *dev);

/* Internet checksum over len bytes */
uint16_t inet_cksum(const void *data, size_t len);
/* Incrementally update checksum for 4-byte field swap */
uint16_t inet_cksum_buf(uint16_t seed, const void *data, size_t len);

void net_init(void);
void net_dhcp_all(void);
extern int net_drop_n;                           /* test hook: drop every n-th loopback frame */
/* dhcp.c */
int  dhcp_configure(netdev_t *dev, uint32_t *out_dns, int can_sleep);
extern uint32_t dhcp_last_lease;

#endif
