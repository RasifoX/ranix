#include <kernel/net/net.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>

/* ── SKB pool ────────────────────────────────────────────── */

#define SKB_POOL_SIZE 64

static skb_t skb_pool[SKB_POOL_SIZE];
static spinlock_t skb_lock = SPINLOCK_INIT;
static skb_t *skb_free_list;

static void skb_pool_init(void)
{
    for (int i = 0; i < SKB_POOL_SIZE - 1; i++)
        skb_pool[i].next = &skb_pool[i + 1];
    skb_pool[SKB_POOL_SIZE - 1].next = NULL;
    skb_free_list = &skb_pool[0];
}

skb_t *skb_alloc(void)
{
    uint64_t f = spin_lock_irq(&skb_lock);
    skb_t *s = skb_free_list;
    if (s) {
        skb_free_list = s->next;
        s->data = s->buf + SKB_HEADROOM;
        s->len  = 0;
        s->next = NULL;
    }
    spin_unlock_irq(&skb_lock, f);
    return s;
}

void skb_free(skb_t *skb)
{
    if (!skb) return;
    uint64_t f = spin_lock_irq(&skb_lock);
    skb->next = skb_free_list;
    skb_free_list = skb;
    spin_unlock_irq(&skb_lock, f);
}

/* ── netdev registry ─────────────────────────────────────── */

#define NETDEV_MAX 4
static netdev_t *devs[NETDEV_MAX];
static int       ndevs;

void net_register(netdev_t *dev)
{
    if (ndevs >= NETDEV_MAX) return;
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

/* ── init ────────────────────────────────────────────────── */

void net_init(void)
{
    skb_pool_init();
}
