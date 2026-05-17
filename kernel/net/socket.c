#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>
#include <kernel/proc.h>

/* ── global socket table (for UDP delivery by dst_port) ─── */

#define GSOCK_MAX 64

socket_t *gsock[GSOCK_MAX];   /* exported — tcp.c reads this directly */
static spinlock_t gsock_lock = SPINLOCK_INIT;

void sock_register(socket_t *s)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < GSOCK_MAX; i++) {
        if (!gsock[i]) { gsock[i] = s; break; }
    }
    spin_unlock_irq(&gsock_lock, f);
}

void sock_unregister(socket_t *s)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < GSOCK_MAX; i++) {
        if (gsock[i] == s) { gsock[i] = NULL; break; }
    }
    spin_unlock_irq(&gsock_lock, f);
}

socket_t *sock_alloc(int domain, int type, int proto)
{
    socket_t *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->domain = domain;
    s->type   = type;
    s->proto  = proto;
    s->tcp_state = TCP_CLOSED;
    s->recv_lock = (spinlock_t)SPINLOCK_INIT;
    return s;
}

void sock_free(socket_t *s)
{
    if (!s) return;
    sock_unregister(s);
    skb_t *skb = s->recv_head;
    while (skb) { skb_t *n = skb->next; skb_free(skb); skb = n; }
    if (s->retx_data) kfree(s->retx_data);
    kfree(s);
}

/* ── ICMP delivery to RAW sockets ───────────────────────── */

void sock_icmp_deliver(uint32_t src_ip, const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < GSOCK_MAX; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->type != SOCK_RAW) continue;
        if (s->proto != 1 /* IPPROTO_ICMP */) continue;
        found = s;
        break;
    }
    spin_unlock_irq(&gsock_lock, f);
    if (!found) return;

    /* prepend a fake IP header — src in network byte order, TTL=64 */
    uint8_t ipbuf[20 + SKB_DATA_MAX];
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    memset(ipbuf, 0, 20);
    ipbuf[0] = 0x45;   /* ver=4, ihl=5 */
    ipbuf[8] = 64;     /* TTL */
    uint32_t src_net = htonl(src_ip);
    memcpy(ipbuf + 12, &src_net, 4);
    memcpy(ipbuf + 20, data, len);
    uint16_t total = (uint16_t)(20 + len);
    if (total > SKB_DATA_MAX) total = SKB_DATA_MAX;

    skb_t *skb = skb_alloc();
    if (!skb) return;
    memcpy(skb->data, ipbuf, total);
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
}

/* ── UDP delivery ────────────────────────────────────────── */

void sock_udp_deliver(uint32_t src_ip, uint16_t src_port,
                      uint16_t dst_port, const void *data, uint16_t len)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    socket_t *found = NULL;
    for (int i = 0; i < GSOCK_MAX; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->type != SOCK_DGRAM) continue;
        if (s->local_port != dst_port) continue;
        found = s;
        break;
    }
    spin_unlock_irq(&gsock_lock, f);

    if (!found) return;

    skb_t *skb = skb_alloc();
    if (!skb) return;
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    memcpy(skb->data, data, len);
    skb->len = len;

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
}
