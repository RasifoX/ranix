#include <kernel/net/net.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>

/* ── SKB allocator (heap-based, no fixed pool) ───────────── */

skb_t *skb_alloc(void)
{
    skb_t *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->data = s->buf + SKB_HEADROOM;
    s->len  = 0;
    s->next = NULL;
    return s;
}

void skb_free(skb_t *skb)
{
    kfree(skb);
}

/* ── netdev registry (dynamic) ───────────────────────────── */

#define NETDEV_INIT_CAP 4
static netdev_t **devs;
static int        ndevs;
static int        devs_cap;

void net_register(netdev_t *dev)
{
    if (ndevs >= devs_cap) {
        int nc = devs_cap ? devs_cap * 2 : NETDEV_INIT_CAP;
        netdev_t **nb = kmalloc((size_t)nc * sizeof(netdev_t *));
        if (!nb) return;
        if (devs) { memcpy(nb, devs, (size_t)ndevs * sizeof(netdev_t *)); kfree(devs); }
        devs = nb; devs_cap = nc;
    }
    devs[ndevs++] = dev;
    kprintf("net: registered %s  ip=%u.%u.%u.%u  mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
        dev->name,
        (dev->ip >> 24) & 0xff, (dev->ip >> 16) & 0xff,
        (dev->ip >>  8) & 0xff,  dev->ip & 0xff,
        dev->mac[0], dev->mac[1], dev->mac[2],
        dev->mac[3], dev->mac[4], dev->mac[5]);
}

netdev_t *net_dev(int idx)
{
    return (idx >= 0 && idx < ndevs) ? devs[idx] : NULL;
}

netdev_t *net_dev_by_ip(uint32_t ip)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i]->ip == ip) return devs[i];
    return NULL;
}

/* ── RX queue ────────────────────────────────────────────── */

void netdev_rx_enqueue(netdev_t *dev, skb_t *skb)
{
    skb->next = NULL;
    uint64_t f = spin_lock_irq(&dev->rx_lock);
    if (dev->rx_tail) dev->rx_tail->next = skb;
    else              dev->rx_head = skb;
    dev->rx_tail = skb;
    spin_unlock_irq(&dev->rx_lock, f);
}

skb_t *netdev_rx_dequeue(netdev_t *dev)
{
    uint64_t f = spin_lock_irq(&dev->rx_lock);
    skb_t *s = dev->rx_head;
    if (s) {
        dev->rx_head = s->next;
        if (!dev->rx_head) dev->rx_tail = NULL;
        s->next = NULL;
    }
    spin_unlock_irq(&dev->rx_lock, f);
    return s;
}

/* ── poll ────────────────────────────────────────────────── */

void net_poll(void)
{
    for (int i = 0; i < ndevs; i++) {
        netdev_t *dev = devs[i];
        if (dev->poll) dev->poll(dev);

        skb_t *skb;
        while ((skb = netdev_rx_dequeue(dev)) != NULL)
            eth_rx(dev, skb);
    }
}

/* ── checksum ────────────────────────────────────────────── */

uint16_t inet_cksum(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t inet_cksum_buf(uint16_t seed, const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = (uint16_t)~seed;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── loopback device ─────────────────────────────────────── */

/* The loopback netdev "lo" (127.0.0.1) is always present, with no hardware
 * behind it: ip_tx already redirects frames addressed to 127/8 (or our own IP)
 * back into the receive path, so lo's tx just enqueues. Having lo guarantees
 * there is always at least one device — a socket never ends up with a NULL
 * dev — and lets 127.0.0.1 work even with no NIC. */
static netdev_t lo_dev;

static int lo_tx(netdev_t *dev, skb_t *skb)
{
    netdev_rx_enqueue(dev, skb);
    return 0;
}

/* Default device for a new socket: a real NIC if one exists, else loopback.
 * (127/8 traffic loops back regardless of which device carries it — ip_tx.) */
netdev_t *net_default_dev(void)
{
    for (int i = 0; i < ndevs; i++)
        if ((devs[i]->ip >> 24) != 127) return devs[i];
    return ndevs ? devs[0] : NULL;
}

/* ── init ────────────────────────────────────────────────── */

void net_init(void)
{
    memcpy(lo_dev.name, "lo", 3);
    lo_dev.ip      = 0x7f000001;        /* 127.0.0.1 */
    lo_dev.mask    = 0xff000000;        /* 127/8 */
    lo_dev.tx      = lo_tx;
    lo_dev.rx_lock = (spinlock_t)SPINLOCK_INIT;
    net_register(&lo_dev);
}
