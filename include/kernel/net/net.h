#ifndef _KERNEL_NET_NET_H
#define _KERNEL_NET_NET_H

#include <uapi/types.h>
#include <kernel/sync.h>

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

/* ── socket buffer ───────────────────────────────────────── */

#define SKB_HEADROOM  64
#define SKB_DATA_MAX  1536

typedef struct skb {
    uint8_t     buf[SKB_HEADROOM + SKB_DATA_MAX];
    uint8_t    *data;   /* current start of packet data */
    uint16_t    len;    /* bytes from data */
    struct skb *next;
} skb_t;

skb_t  *skb_alloc(void);
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

    int       (*tx)(struct netdev *, skb_t *);
    void      (*poll)(struct netdev *);   /* hardware poll, set by driver */
    void       *priv;

    /* RX queue — filled by poll, drained by net_poll */
    spinlock_t  rx_lock;
    skb_t      *rx_head;
    skb_t      *rx_tail;

    struct net_namespace *net_ns;     /* owning network namespace */
} netdev_t;

/* ── network namespace ───────────────────────────────────────
 * Each holds its own set of devices (a fresh ns has only its private
 * loopback); sockets and device lookups are scoped to a namespace so two
 * namespaces are isolated network stacks. */
typedef struct net_namespace {
    int           refcount;
    unsigned long inum;
    netdev_t      lo;                 /* this namespace's private loopback */
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
netdev_t *net_dev_by_ip(uint32_t ip);
netdev_t *net_default_dev(void);     /* real NIC if present, else loopback */

/* Called from scheduler tick / idle loop — drains RX queues */
void net_poll(void);

/* Push received skb onto dev's RX queue */
void netdev_rx_enqueue(netdev_t *dev, skb_t *skb);
skb_t *netdev_rx_dequeue(netdev_t *dev);

/* Internet checksum over len bytes */
uint16_t inet_cksum(const void *data, size_t len);
/* Incrementally update checksum for 4-byte field swap */
uint16_t inet_cksum_buf(uint16_t seed, const void *data, size_t len);

#endif
