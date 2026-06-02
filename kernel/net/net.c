#include <kernel/net/net.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>
#include <kernel/proc.h>
#include <kernel/sync.h>
#include <kernel/nsproxy.h>

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

static int lo_tx(netdev_t *dev, skb_t *skb);   /* defined below */

/* ── network namespaces ──────────────────────────────────── */

net_namespace_t init_net_ns = { .refcount = 1, .inum = 0xF0000007UL };
static spinlock_t netns_lock = SPINLOCK_INIT;

net_namespace_t *current_net_ns(void)
{
    process_t *p = task_current();
    return (p && p->net_ns) ? p->net_ns : &init_net_ns;
}

net_namespace_t *net_ns_get(net_namespace_t *ns)
{
    if (!ns) ns = &init_net_ns;
    if (ns != &init_net_ns) {
        uint64_t f = spin_lock_irq(&netns_lock);
        ns->refcount++;
        spin_unlock_irq(&netns_lock, f);
    }
    return ns;
}

static void netdev_unregister(netdev_t *dev)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i] == dev) {
            for (int j = i; j < ndevs - 1; j++) devs[j] = devs[j + 1];
            ndevs--;
            return;
        }
}

void net_ns_put(net_namespace_t *ns)
{
    if (!ns || ns == &init_net_ns) return;
    uint64_t f = spin_lock_irq(&netns_lock);
    int last = (--ns->refcount == 0);
    spin_unlock_irq(&netns_lock, f);
    if (last) {
        netdev_unregister(&ns->lo);   /* its private loopback */
        kfree(ns);
    }
}

net_namespace_t *net_ns_create(void)
{
    net_namespace_t *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->refcount = 1;
    ns->inum     = ns_inum_alloc();
    memcpy(ns->lo.name, "lo", 3);
    ns->lo.ip      = 0x7f000001;       /* 127.0.0.1, isolated to this ns */
    ns->lo.mask    = 0xff000000;
    ns->lo.tx      = lo_tx;
    ns->lo.rx_lock = (spinlock_t)SPINLOCK_INIT;
    ns->lo.net_ns  = ns;
    net_register(&ns->lo);
    return ns;
}

void net_register(netdev_t *dev)
{
    if (ndevs >= devs_cap) {
        int nc = devs_cap ? devs_cap * 2 : NETDEV_INIT_CAP;
        netdev_t **nb = kmalloc((size_t)nc * sizeof(netdev_t *));
        if (!nb) return;
        if (devs) { memcpy(nb, devs, (size_t)ndevs * sizeof(netdev_t *)); kfree(devs); }
        devs = nb; devs_cap = nc;
    }
    if (!dev->net_ns) dev->net_ns = current_net_ns();
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
    net_namespace_t *ns = current_net_ns();
    int k = 0;
    for (int i = 0; i < ndevs; i++)
        if (devs[i]->net_ns == ns && k++ == idx) return devs[i];
    return NULL;
}

netdev_t *net_dev_by_ip(uint32_t ip)
{
    net_namespace_t *ns = current_net_ns();
    for (int i = 0; i < ndevs; i++)
        if (devs[i]->net_ns == ns && devs[i]->ip == ip) return devs[i];
    return NULL;
}

/* ── RX queue ────────────────────────────────────────────── */

void net_rx_wake(void);

void netdev_rx_enqueue(netdev_t *dev, skb_t *skb)
{
    skb->next = NULL;
    uint64_t f = spin_lock_irq(&dev->rx_lock);
    if (dev->rx_tail) dev->rx_tail->next = skb;
    else              dev->rx_head = skb;
    dev->rx_tail = skb;
    spin_unlock_irq(&dev->rx_lock, f);
    net_rx_wake();   /* event-driven: process now, don't wait for the timer tick */
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

/* ── event-driven RX softirq thread ──────────────────────────
 * RX processing runs in a dedicated kernel thread woken the moment a packet is
 * enqueued (loopback tx, NIC poll), instead of waiting for the idle loop's
 * timer-paced net_poll. This is what makes loopback/LAN throughput track the
 * window rather than the 100Hz tick. net_poll runs in this thread's own context
 * — never nested inside a socket syscall — so it can't deadlock against the
 * locks a sender/receiver holds. The idle loop still calls net_poll as a
 * backstop. */
static process_t *net_rx_task;
static volatile int net_rx_pending;
static spinlock_t  net_rx_lock = SPINLOCK_INIT;

void net_rx_wake(void)
{
    uint64_t f = spin_lock_irq(&net_rx_lock);
    net_rx_pending = 1;
    process_t *t = net_rx_task;
    spin_unlock_irq(&net_rx_lock, f);
    if (t) task_wake(t);
}

static void net_rx_thread(void)
{
    net_rx_task = task_current();
    for (;;) {
        net_poll();
        uint64_t f = spin_lock_irq(&net_rx_lock);
        if (net_rx_pending) {            /* work arrived during the poll — go again */
            net_rx_pending = 0;
            spin_unlock_irq(&net_rx_lock, f);
            continue;
        }
        task_sleep_unlock(&net_rx_lock, f);   /* sleep until net_rx_wake() */
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
static int lo_tx(netdev_t *dev, skb_t *skb)
{
    netdev_rx_enqueue(dev, skb);
    return 0;
}

/* Default device for a new socket: a real NIC if one exists, else loopback.
 * (127/8 traffic loops back regardless of which device carries it — ip_tx.) */
netdev_t *net_default_dev(void)
{
    net_namespace_t *ns = current_net_ns();
    for (int i = 0; i < ndevs; i++)
        if (devs[i]->net_ns == ns && (devs[i]->ip >> 24) != 127) return devs[i];
    return &ns->lo;   /* a namespace always has its own loopback */
}

/* Egress device for a destination. 127/8 must go out the loopback device so the
 * source address is 127.0.0.1 too — sending it from a NIC's address produces an
 * asymmetric tuple the in-stack loopback then mishandles. Our own NIC address is
 * already looped by ip_tx (dst == dev->ip), so only 127/8 needs redirecting. */
netdev_t *net_route_dev(uint32_t dst_ip)
{
    if ((dst_ip >> 24) == 127) return &current_net_ns()->lo;
    return net_default_dev();
}

/* ── init ────────────────────────────────────────────────── */

void net_init(void)
{
    netdev_t *lo = &init_net_ns.lo;
    memcpy(lo->name, "lo", 3);
    lo->ip      = 0x7f000001;        /* 127.0.0.1 */
    lo->mask    = 0xff000000;        /* 127/8 */
    lo->tx      = lo_tx;
    lo->rx_lock = (spinlock_t)SPINLOCK_INIT;
    lo->net_ns  = &init_net_ns;
    net_register(lo);

    proc_create("net_rx", net_rx_thread);   /* event-driven RX softirq */
}

/* Point /etc/resolv.conf at the DHCP-supplied nameserver, replacing the static
 * default so name resolution follows the network we actually joined. */
static void write_resolv_conf(uint32_t dns)
{
    extern int vfs_unlink(const char *path);
    extern int vfs_kernel_install(const char *path, const void *data, size_t len);

    char buf[40];
    int n = 0;
    for (const char *p = "nameserver "; *p; p++) buf[n++] = *p;
    for (int i = 0; i < 4; i++) {
        uint8_t v = (uint8_t)(dns >> (24 - i * 8));
        if (v >= 100) buf[n++] = '0' + v / 100;
        if (v >= 10)  buf[n++] = '0' + (v / 10) % 10;
        buf[n++] = '0' + v % 10;
        buf[n++] = (i < 3) ? '.' : '\n';
    }
    vfs_unlink("/etc/resolv.conf");
    vfs_kernel_install("/etc/resolv.conf", buf, n);
}

/* Configure every real NIC over DHCP, falling back to its driver static IP. Run
 * after the drivers register (NICs present) and the VFS is up. */
void net_dhcp_all(void)
{
    extern int dhcp_configure(netdev_t *dev, uint32_t *out_dns);
    for (int i = 0; i < ndevs; i++) {
        netdev_t *d = devs[i];
        if (!d->poll || (d->ip >> 24) == 127) continue;   /* skip loopback */
        uint32_t dns = 0;
        if (dhcp_configure(d, &dns) == 0) {
            if (dns) write_resolv_conf(dns);
        } else {
            kprintf("dhcp: %s no response, keeping %u.%u.%u.%u\n", d->name,
                    (d->ip >> 24) & 0xff, (d->ip >> 16) & 0xff,
                    (d->ip >> 8) & 0xff, d->ip & 0xff);
        }
    }
}
