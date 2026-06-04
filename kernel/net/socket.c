#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>
#include <kernel/proc.h>

/* ── global socket table (dynamic) ──────────────────────── */

#define GSOCK_INIT_CAP 64

socket_t **gsock;
int        gsock_cap;
int        gsock_cnt;
spinlock_t gsock_lock = SPINLOCK_INIT;

int gsock_count(void) { return gsock_cnt; }

static void gsock_ensure(void)
{
    if (gsock_cnt < gsock_cap) return;
    int nc = gsock_cap ? gsock_cap * 2 : GSOCK_INIT_CAP;
    socket_t **nb = kmalloc((size_t)nc * sizeof(socket_t *));
    if (!nb) return;
    memset(nb, 0, (size_t)nc * sizeof(socket_t *));
    if (gsock) { memcpy(nb, gsock, (size_t)gsock_cap * sizeof(socket_t *)); kfree(gsock); }
    gsock = nb; gsock_cap = nc;
}

void sock_register(socket_t *s)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++)
        if (gsock[i] == s) { spin_unlock_irq(&gsock_lock, f); return; }  /* already registered */
    /* find empty slot */
    for (int i = 0; i < gsock_cap; i++) {
        if (!gsock[i]) { gsock[i] = s; gsock_cnt++; spin_unlock_irq(&gsock_lock, f); return; }
    }
    /* no empty slot — grow, then take the first freshly-added slot */
    gsock_ensure();
    for (int i = 0; i < gsock_cap; i++) {
        if (!gsock[i]) { gsock[i] = s; gsock_cnt++; break; }
    }
    spin_unlock_irq(&gsock_lock, f);
}

void sock_unregister(socket_t *s)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++) {
        if (gsock[i] == s) { gsock[i] = NULL; gsock_cnt--; break; }
    }
    spin_unlock_irq(&gsock_lock, f);
}

socket_t *sock_alloc(int domain, int type, int proto)
{
    socket_t *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->refcount = 1;
    s->domain = domain;
    s->type   = type;
    s->proto  = proto;
    s->tcp_state = TCP_CLOSED;
    s->recv_lock = (spinlock_t)SPINLOCK_INIT;
    s->net_ns    = current_net_ns();
    s->so_rcvbuf = 65536;
    s->so_sndbuf = 65536;
    return s;
}

void sock_hold(socket_t *s)
{
    if (s) atomic_fetch_add32((volatile uint32_t *)&s->refcount, 1);
}

/* Drop a reference. The registry lookups (sock_*_deliver, tcp_sock_find*) bump
 * the count under gsock_lock before releasing it, so a concurrent close cannot
 * free the socket out from under an in-flight RX/tick path: the teardown only
 * unregisters it (removing it from future lookups) and drops the "alive"
 * reference; the last holder does the actual free. */
void sock_put(socket_t *s)
{
    if (!s) return;
    if (atomic_sub_fetch32((volatile uint32_t *)&s->refcount, 1) != 0) return;
    skb_t *skb = s->recv_head;
    while (skb) { skb_t *n = skb->next; skb_free(skb); skb = n; }
    if (s->type == SOCK_STREAM) { extern void tcp_sock_free(socket_t *); tcp_sock_free(s); }
    kfree(s);
}

void sock_free(socket_t *s)
{
    if (!s) return;
    sock_unregister(s);
    sock_put(s);   /* drop the alive reference; frees once no lookup holds it */
}

/* ── AF_UNIX send ────────────────────────────────────────── */

int unix_send(socket_t *s, const void *data, uint16_t len)
{
    socket_t *peer = s->unix_peer;
    if (!peer) return -32; /* EPIPE */
    sock_tcp_deliver(peer, data, len);
    return (int)len;
}

/* SCM_RIGHTS: deliver data plus in-flight passed files to the peer in one skb.
 * On failure the caller drops the held file references. */
int unix_send_scm(socket_t *s, const void *data, uint16_t len, void **files, int nfiles)
{
    socket_t *peer = s->unix_peer;
    if (!peer) return -32; /* EPIPE */
    skb_t *skb = skb_alloc();
    if (!skb) return -12;  /* ENOMEM */
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    if (len) memcpy(skb->data, data, len);
    skb->len = len;
    if (nfiles > SKB_SCM_MAX) nfiles = SKB_SCM_MAX;
    for (int i = 0; i < nfiles; i++) skb->scm_files[i] = files[i];
    skb->scm_nfiles = (uint8_t)nfiles;

    uint64_t f = spin_lock_irq(&peer->recv_lock);
    skb->next = NULL;
    if (peer->recv_tail) peer->recv_tail->next = skb;
    else                 peer->recv_head = skb;
    peer->recv_tail = skb;
    process_t *w = peer->recv_waiter;
    peer->recv_waiter = NULL;
    spin_unlock_irq(&peer->recv_lock, f);
    if (w) task_wake(w);
    return (int)len;
}

/* ── ICMP delivery to RAW sockets ───────────────────────── */

void sock_icmp_deliver(struct net_namespace *ns, uint32_t src_ip, const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_RAW && s->type != SOCK_DGRAM) continue;  /* incl. ping socket */
        if (s->proto != 1 /* IPPROTO_ICMP */) continue;
        found = s;
        sock_hold(s);
        break;
    }
    spin_unlock_irq(&gsock_lock, f);
    if (!found) return;

    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    skb_t *skb = skb_alloc();
    if (!skb) { sock_put(found); return; }
    uint16_t total;
    if (found->type == SOCK_RAW) {
        /* SOCK_RAW: deliver with a fake IP header (src net order, TTL=64) — Linux
         * raw ICMP sockets include the IP header; SOCK_DGRAM ping sockets do not */
        memset(skb->data, 0, 20);
        skb->data[0] = 0x45;   /* ver=4, ihl=5 */
        skb->data[8] = 64;     /* TTL */
        uint32_t src_net = htonl(src_ip);
        memcpy(skb->data + 12, &src_net, 4);
        if ((int)len > SKB_DATA_MAX - 20) len = (uint16_t)(SKB_DATA_MAX - 20);
        memcpy(skb->data + 20, data, len);
        total = (uint16_t)(20 + len);
    } else {
        memcpy(skb->data, data, len);   /* DGRAM ping socket: ICMP message only */
        total = len;
    }
    skb->len = total;

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify();
    sock_put(found);
}

void sock_icmp6_deliver(struct net_namespace *ns, const in6_addr_t *src,
                        const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->domain != AF_INET6) continue;
        if (s->type != SOCK_RAW && s->type != SOCK_DGRAM) continue;  /* incl. ping socket */
        if (s->proto != IPPROTO_ICMPV6) continue;
        found = s;
        sock_hold(s);
        break;
    }
    spin_unlock_irq(&gsock_lock, f);
    if (!found) return;

    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    skb_t *skb = skb_alloc();
    if (!skb) { sock_put(found); return; }
    memcpy(skb->data, data, len);   /* raw ICMPv6 message, no IPv6 header */
    skb->len    = len;
    skb->src_ip6 = *src;

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify();
    sock_put(found);
}

/* ── UDP delivery ────────────────────────────────────────── */

void sock_udp6_deliver(struct net_namespace *ns, const in6_addr_t *src, uint16_t src_port,
                       uint16_t dst_port, const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->domain != AF_INET6 || s->type != SOCK_DGRAM) continue;
        if (s->local_port != dst_port) continue;
        found = s;
        sock_hold(s);
        break;
    }
    spin_unlock_irq(&gsock_lock, f);
    if (!found) return;

    skb_t *skb = skb_alloc_big(len);
    if (!skb) { sock_put(found); return; }
    memcpy(skb->data, data, len);
    skb->len      = len;
    skb->src_ip6  = *src;
    skb->src_port = src_port;

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify();
    sock_put(found);
}

void sock_udp_deliver(struct net_namespace *ns, uint32_t src_ip, uint16_t src_port,
                      uint16_t dst_port, const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_DGRAM) continue;
        if (s->local_port != dst_port) continue;
        found = s;
        sock_hold(s);
        break;
    }
    spin_unlock_irq(&gsock_lock, f);

    if (!found) return;

    skb_t *skb = skb_alloc_big(len);
    if (!skb) { sock_put(found); return; }
    memcpy(skb->data, data, len);
    skb->len = len;
    skb->src_ip   = src_ip;
    skb->src_port = src_port;

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify();
    sock_put(found);
}
