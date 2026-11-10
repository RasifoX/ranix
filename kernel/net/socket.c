/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>
#include <kernel/proc.h>
#include <kernel/cgroup.h>
#include <kernel/net/tcp.h>
#include <kernel/errno.h>

/* ── global socket table (dynamic) ──────────────────────── */

#define GSOCK_INIT_CAP CONFIG_GSOCK_INIT_CAP

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

/* Egress for an IPv4 destination from socket s. SO_BINDTODEVICE fixes the
 * device. Otherwise a socket whose local address belongs to one device leaves
 * through that device when it has a route there: with cable and Wi-Fi on one
 * subnet, a connection made to (or bound to) eth0's address keeps using eth0
 * and eth0's address both ways. Without such a route the main table decides. */
netdev_t *sock_route4(socket_t *s, uint32_t dst)
{
    if (s->bound_dev) return net_route_dev(dst, s->bound_dev);
    netdev_t *lo = &current_net_ns()->lo;
    netdev_t *own = (s->local_ip && (s->local_ip >> 24) != 127) ? net_dev_by_ip(s->local_ip) : NULL;
    if (own) {
        netdev_t *d = net_route_dev(dst, own);
        if (d != lo) return d;
    }
    return net_route_dev(dst, NULL);
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
    s->net_ns    = net_ns_get(current_net_ns());   /* the fd may outlive every process in it */
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
    if (s->type == SOCK_STREAM) { tcp_sock_free(s); }
    net_ns_put(s->net_ns);
    cgroup_file_release(s->cg, sizeof(*s));
    kfree(s);
}

void sock_free(socket_t *s)
{
    if (!s) return;
    sock_unregister(s);
    sock_put(s);   /* drop the alive reference; frees once no lookup holds it */
}

int sock_own(socket_t *s)
{
    return cgroup_charge_file(&s->cg, 0, sizeof(*s)) ? -12 : 0;
}

/* A stream's receive window goes through whatever the budget says, or a session
 * at its limit could not so much as report that: the overshoot stops at one window
 * per socket, and past it the budget refuses. Datagrams have no such need. */
static int sock_stream_room(const socket_t *s)
{
    return s->type == SOCK_STREAM && s->rcv_buffered < s->so_rcvbuf;
}

/* From receive paths with no meaningful current task. */
int sock_charge_skb(socket_t *s, skb_t *skb)
{
    if (!s->cg) return 0;
    if (cgroup_charge_to(s->cg, SKB_TRUESIZE(skb), sock_stream_room(s)) != 0) return -1;
    skb->cg = s->cg;
    return 0;
}

void sock_queue_skb(socket_t *s, skb_t *skb)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    skb->next = NULL;
    if (s->recv_tail) s->recv_tail->next = skb;
    else              s->recv_head = skb;
    s->recv_tail = skb;
    s->rcv_buffered += skb->len;
    process_t *w = s->recv_waiter;
    s->recv_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
    poll_notify_key(s);
}

/* ── AF_UNIX send ────────────────────────────────────────── */

/* who sent it: the receiver may ask (SO_PASSCRED). Chromium's zygote learns
 * its children's pids this way and CHECKs that it did. */
static void unix_skb_cred(skb_t *skb)
{
    process_t *p = task_current();
    skb->cred_pid = p ? (int32_t)p->tgid : 0;
    skb->cred_uid = p ? p->uid : 0;
    skb->cred_gid = p ? p->gid : 0;
}

/* The sender pays, not the receiver: a session must not be able to fill a host
 * process's queue for free. -11 when its budget is spent; returns what it took. */
/* A datagram (or seqpacket) receiver holds at most UNIX_DGRAM_QLEN messages,
 * as Linux's net.unix.max_dgram_qlen: past that a sender gets EAGAIN (and a
 * blocking one waits) instead of queueing until memory runs out. */
#define UNIX_DGRAM_QLEN 512
static int unix_peer_full(socket_t *peer)
{
    if (peer->type == SOCK_STREAM) return 0;
    int n = 0;
    uint64_t f = spin_lock_irq(&peer->recv_lock);
    for (skb_t *q = peer->recv_head; q && n < UNIX_DGRAM_QLEN; q = q->next) n++;
    spin_unlock_irq(&peer->recv_lock, f);
    return n >= UNIX_DGRAM_QLEN;
}

int unix_send(socket_t *s, const void *data, uint16_t len)
{
    socket_t *peer = s->unix_peer;
    if (!peer || s->unix_wr_shut) return -EPIPE;
    if (!len) return 0;
    if (unix_peer_full(peer)) return -EAGAIN;
    /* a datagram (or seqpacket) is one message, whole: a stream is cut to skbs */
    int whole = s->type != SOCK_STREAM;
    skb_t *skb = whole ? skb_alloc_big(len) : skb_alloc();
    if (!skb) return -ENOMEM;
    if (!whole && len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    memcpy(skb->data, data, len);
    skb->len = len;
    unix_skb_cred(skb);
    void *cg = cgroup_current();
    if (cgroup_charge_to(cg, SKB_TRUESIZE(skb), sock_stream_room(peer)) != 0) {
        skb_free(skb);
        return -EAGAIN;
    }
    skb->cg = cg;
    sock_queue_skb(peer, skb);
    return (int)len;
}

/* SCM_RIGHTS: deliver data plus in-flight passed files to the peer in one skb.
 * On failure the caller drops the held file references. */
int unix_send_scm(socket_t *s, const void *data, uint16_t len, void **files, int nfiles)
{
    socket_t *peer = s->unix_peer;
    if (!peer || s->unix_wr_shut) return -EPIPE;
    if (unix_peer_full(peer)) return -EAGAIN;
    int whole = s->type != SOCK_STREAM;
    skb_t *skb = whole ? skb_alloc_big(len) : skb_alloc();
    if (!skb) return -ENOMEM;
    if (!whole && len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    if (len) memcpy(skb->data, data, len);
    skb->len = len;
    unix_skb_cred(skb);
    if (nfiles > SKB_SCM_MAX) nfiles = SKB_SCM_MAX;
    void *cg = cgroup_current();
    if (cgroup_charge_to(cg, SKB_TRUESIZE(skb), sock_stream_room(peer)) != 0) {
        skb_free(skb);
        return -EAGAIN;
    }
    skb->cg = cg;
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
    poll_notify_key(peer);   /* a poll()/epoll waiter hears it too, as for plain data */
    return (int)len;
}

/* ── ICMP delivery to RAW sockets ───────────────────────── */

/* ── AF_PACKET ───────────────────────────────────────────
 * Every frame a device takes in is offered to the packet sockets of its
 * namespace that ask for its ethertype (or ETH_P_ALL) on that interface (or
 * any): SOCK_RAW gets the whole frame, SOCK_DGRAM what follows the Ethernet
 * header. No rings (PACKET_RX_RING); a socket that is not read drops after
 * PK_QUEUE_MAX frames rather than growing without end. */
#define PK_QUEUE_MAX 256
#define PK_MATCH_MAX 16

int net_dev_ifindex(struct netdev *dev)
{
    for (int i = 0; ; i++) {
        netdev_t *d = net_dev_in(dev->net_ns, i);
        if (!d) return 0;
        if (d == dev) return i + 1;
    }
}

static void packet_queue(socket_t *s, netdev_t *dev, int ifindex, const uint8_t *frame, uint16_t len)
{
    uint16_t skip = s->type == SOCK_DGRAM ? 14 : 0;
    if (len <= skip || len - skip > SKB_DATA_MAX) return;
    uint64_t f = spin_lock_irq(&s->recv_lock);
    int n = 0;
    for (skb_t *q = s->recv_head; q && n < PK_QUEUE_MAX; q = q->next) n++;
    spin_unlock_irq(&s->recv_lock, f);
    if (n >= PK_QUEUE_MAX) return;
    skb_t *skb = skb_alloc();
    if (!skb) return;
    memcpy(skb->data, frame + skip, len - skip);
    skb->len = (uint16_t)(len - skip);
    memcpy(&skb->ll_proto, frame + 12, 2);
    memcpy(skb->ll_src, frame + 6, 6);
    skb->ll_ifindex = ifindex;
    skb->ll_loop = dev == &dev->net_ns->lo;
    const uint8_t *dst = frame;
    int bcast = dst[0] == 0xff && dst[1] == 0xff && dst[2] == 0xff && dst[3] == 0xff && dst[4] == 0xff && dst[5] == 0xff;
    skb->ll_pkttype = bcast ? 1 : (dst[0] & 1) ? 2 : memcmp(dst, dev->mac, 6) == 0 ? 0 : 3;
    if (sock_charge_skb(s, skb) != 0) { skb_free(skb); return; }
    sock_queue_skb(s, skb);
}

void sock_packet_deliver(struct netdev *dev, const uint8_t *frame, uint16_t len)
{
    if (len < 14) return;
    uint16_t type;
    memcpy(&type, frame + 12, 2);
    socket_t *m[PK_MATCH_MAX];
    int n = 0, ifindex = 0;
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap && n < PK_MATCH_MAX; i++) {
        socket_t *s = gsock[i];
        if (!s || s->domain != AF_PACKET || s->net_ns != dev->net_ns) continue;
        if (s->pk_proto != htons(0x0003) && s->pk_proto != type) continue;
        sock_hold(s);
        m[n++] = s;
    }
    spin_unlock_irq(&gsock_lock, f);
    if (n) ifindex = net_dev_ifindex(dev);
    for (int i = 0; i < n; i++) {
        if (!m[i]->pk_ifindex || m[i]->pk_ifindex == ifindex)
            packet_queue(m[i], dev, ifindex, frame, len);
        sock_put(m[i]);
    }
}

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

    if (sock_charge_skb(found, skb) != 0) { skb_free(skb); sock_put(found); return; }

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify_key(found);
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

    if (sock_charge_skb(found, skb) != 0) { skb_free(skb); sock_put(found); return; }

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify_key(found);
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

    if (sock_charge_skb(found, skb) != 0) { skb_free(skb); sock_put(found); return; }

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify_key(found);
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
        if (s->type != SOCK_DGRAM || s->domain == AF_NETLINK) continue;
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

    if (sock_charge_skb(found, skb) != 0) { skb_free(skb); sock_put(found); return; }

    uint64_t sf = spin_lock_irq(&found->recv_lock);
    skb->next = NULL;
    if (found->recv_tail) found->recv_tail->next = skb;
    else                  found->recv_head = skb;
    found->recv_tail = skb;
    process_t *waiter = found->recv_waiter;
    found->recv_waiter = NULL;
    spin_unlock_irq(&found->recv_lock, sf);

    if (waiter) task_wake(waiter);
    poll_notify_key(found);
    sock_put(found);
}
