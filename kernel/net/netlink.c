/* ── AF_NETLINK / NETLINK_ROUTE ──────────────────────────────────────────────
 * A read-only rtnetlink view of one namespace's netdevs: enough for `ip a`,
 * `ip r`, `ip link` and getifaddrs(). A request is answered in full during the
 * send, so "dump state" is just datagrams on the socket's receive queue. There
 * are no notifications: multicast groups can be joined but nothing is sent.
 * Replies come from the socket's namespace, not the caller's: an fd that crossed
 * setns() keeps describing the namespace it was opened in.
 * ───────────────────────────────────────────────────────────────────────── */

#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/net/ipv6.h>
#include <kernel/capabilities.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/proc.h>
#include <kernel/sync.h>

/* wire format */
typedef struct { uint32_t len; uint16_t type, flags; uint32_t seq, pid; } nl_hdr_t;
typedef struct { uint16_t len, type; } nl_attr_t;
typedef struct { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; } nl_link_t;
typedef struct { uint8_t family, prefixlen, flags, scope; uint32_t index; } nl_addr_t;
typedef struct {
    uint8_t  family, dst_len, src_len, tos, table, protocol, scope, type;
    uint32_t flags;
} nl_route_t;

#define NLF_REQUEST 0x001
#define NLF_MULTI   0x002
#define NLF_ACK     0x004
#define NLF_CAPPED  0x100       /* our error messages echo only the header */
#define NLF_DUMP    0x300

#define NLT_ERROR     2
#define NLT_DONE      3
#define NLT_RT_FIRST  16        /* below this is netlink control, never rtnetlink */

#define RTM_NEWLINK   16
#define RTM_GETLINK   18
#define RTM_NEWADDR   20
#define RTM_GETADDR   22
#define RTM_NEWROUTE  24
#define RTM_GETROUTE  26
#define RTM_KIND_GET  2         /* every rtnetlink type is NEW, DEL, GET, SET in turn */

#define IFLA_ADDRESS    1
#define IFLA_BROADCAST  2
#define IFLA_IFNAME     3
#define IFLA_MTU        4
#define IFLA_QDISC      6
#define IFLA_OPERSTATE 16
#define IFLA_LINKMODE  17

#define IFA_ADDRESS   1
#define IFA_LOCAL     2
#define IFA_LABEL     3
#define IFA_BROADCAST 4

#define RTA_DST      1
#define RTA_OIF      4
#define RTA_GATEWAY  5
#define RTA_PREFSRC  7
#define RTA_TABLE   15

#define IF_UP        0x00001
#define IF_BROADCAST 0x00002
#define IF_LOOPBACK  0x00008
#define IF_RUNNING   0x00040
#define IF_MULTICAST 0x01000
#define IF_LOWER_UP  0x10000
#define OPER_DOWN    2
#define OPER_UP      6
#define ARPHRD_ETHER_    1
#define ARPHRD_LOOPBACK_ 772

#define AF_UNSPEC_  0
#define IFA_F_PERMANENT_ 0x80
#define SCOPE_UNIVERSE   0
#define SCOPE_LINK     253
#define SCOPE_HOST     254
#define TABLE_MAIN     254
#define TABLE_LOCAL    255
#define PROTO_KERNEL     2
#define PROTO_BOOT       3
#define RTN_UNICAST_     1
#define RTN_LOCAL_       2
#define RTM_F_CLONED_ 0x200

#define NL_MTU      1500        /* what SIOCGIFMTU reports for every device */
#define NL_MSG_MAX  128         /* longest single message built below */
#define NL_RCV_MAX  (256 * 1024)/* reply backlog: past this a request gets ENOBUFS */

typedef struct {
    socket_t *s;
    skb_t    *skb;      /* datagram being filled */
    uint32_t  msg;      /* offset of the message being built */
    uint32_t  seq;
    int       err;
} nl_out_t;

/* ── port ids ────────────────────────────────────────────────────────────── */

static int nl_pid_taken(socket_t *self, uint32_t pid)   /* gsock_lock held */
{
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *o = gsock[i];
        if (o && o != self && o->domain == AF_NETLINK &&
            o->net_ns == self->net_ns && o->nl_pid == pid)
            return 1;
    }
    return 0;
}

int netlink_bind(socket_t *s, uint32_t pid, uint32_t groups)
{
    static uint32_t rover = (uint32_t)-4096;   /* for a process's second socket */
    uint32_t want = pid ? pid : (uint32_t)pid_to_user(task_current()->tgid);
    int r = 0;

    uint64_t f = spin_lock_irq(&gsock_lock);
    if (s->nl_pid) {
        if (pid && pid != s->nl_pid) r = -22;
    } else if (pid) {
        if (nl_pid_taken(s, pid)) r = -98;   /* EADDRINUSE */
        else                      s->nl_pid = pid;
    } else {
        while (!want || nl_pid_taken(s, want)) want = rover--;
        s->nl_pid = want;
    }
    if (r == 0) s->nl_groups = groups;
    spin_unlock_irq(&gsock_lock, f);
    return r;
}

/* ── building replies ────────────────────────────────────────────────────── */

static void nl_flush(nl_out_t *o)
{
    skb_t *skb = o->skb;
    socket_t *s = o->s;
    if (!skb) return;
    o->skb = NULL;
    if (sock_charge_skb(s, skb) != 0) { skb_free(skb); o->err = -105; return; } /* ENOBUFS */

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
    poll_notify();
}

/* appended 4-byte padded, as every netlink object is */
static void nl_put(nl_out_t *o, const void *data, uint32_t n)
{
    uint32_t padded = (n + 3) & ~3u;
    if (!o->skb) return;
    if (o->skb->len + padded > SKB_DATA_MAX) { o->err = -105; return; }
    memcpy(o->skb->data + o->skb->len, data, n);
    memset(o->skb->data + o->skb->len + n, 0, padded - n);
    o->skb->len = (uint16_t)(o->skb->len + padded);
}

static void nl_begin(nl_out_t *o, uint16_t type, uint16_t flags)
{
    if (o->skb && o->skb->len + NL_MSG_MAX > SKB_DATA_MAX) nl_flush(o);
    if (!o->skb) {
        o->skb = skb_alloc();
        if (!o->skb) { o->err = -12; return; }
    }
    o->msg = o->skb->len;
    nl_hdr_t h = { 0, type, flags, o->seq, o->s->nl_pid };
    nl_put(o, &h, sizeof(h));
}

static void nl_end(nl_out_t *o)
{
    if (o->skb) ((nl_hdr_t *)(o->skb->data + o->msg))->len = o->skb->len - o->msg;
}

static void nl_attr(nl_out_t *o, uint16_t type, const void *data, uint16_t n)
{
    nl_attr_t a = { (uint16_t)(sizeof(a) + n), type };
    nl_put(o, &a, sizeof(a));
    nl_put(o, data, n);
}

static void nl_attr_u32(nl_out_t *o, uint16_t type, uint32_t v) { nl_attr(o, type, &v, 4); }

/* netdev addresses are host order; the wire wants network order */
static void nl_attr_ip(nl_out_t *o, uint16_t type, uint32_t ip)
{
    uint32_t be = htonl(ip);
    nl_attr(o, type, &be, 4);
}

static void nl_ack(nl_out_t *o, const nl_hdr_t *req, int err)
{
    nl_begin(o, NLT_ERROR, NLF_CAPPED);
    nl_put(o, &err, sizeof(err));
    nl_put(o, req, sizeof(*req));
    nl_end(o);
}

static void nl_done(nl_out_t *o)
{
    int zero = 0;
    nl_begin(o, NLT_DONE, NLF_MULTI);
    nl_put(o, &zero, sizeof(zero));
    nl_end(o);
}

/* ── netdev → messages ───────────────────────────────────────────────────── */

static int dev_is_lo(const netdev_t *d) { return d == &d->net_ns->lo; }
static int dev_up(const netdev_t *d)    { return d->link_check ? d->link_up : 1; }

static uint8_t mask_prefix(uint32_t mask)
{
    uint8_t n = 0;
    while (mask & 0x80000000u) { n++; mask <<= 1; }
    return n;
}

static void nl_link(nl_out_t *o, netdev_t *d, int ifindex, uint16_t flags)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int lo = dev_is_lo(d), up = dev_up(d);

    nl_link_t m = { 0 };
    m.type   = lo ? ARPHRD_LOOPBACK_ : ARPHRD_ETHER_;
    m.index  = ifindex;
    m.flags  = IF_UP | (lo ? IF_LOOPBACK : IF_BROADCAST | IF_MULTICAST);
    if (up) m.flags |= IF_RUNNING | IF_LOWER_UP;
    m.change = 0xffffffffu;

    nl_begin(o, RTM_NEWLINK, flags);
    nl_put(o, &m, sizeof(m));
    nl_attr(o, IFLA_IFNAME, d->name, (uint16_t)(strlen(d->name) + 1));
    nl_attr_u32(o, IFLA_MTU, NL_MTU);
    nl_attr(o, IFLA_QDISC, "noqueue", 8);
    uint8_t op = up ? OPER_UP : OPER_DOWN, mode = 0;
    nl_attr(o, IFLA_OPERSTATE, &op, 1);
    nl_attr(o, IFLA_LINKMODE, &mode, 1);
    nl_attr(o, IFLA_ADDRESS, d->mac, 6);
    if (lo) nl_attr(o, IFLA_BROADCAST, d->mac, 6);
    else    nl_attr(o, IFLA_BROADCAST, bcast, 6);
    nl_end(o);
}

static void nl_addr4(nl_out_t *o, netdev_t *d, int ifindex)
{
    nl_addr_t m = { AF_INET, mask_prefix(d->mask), IFA_F_PERMANENT_, SCOPE_UNIVERSE,
                    (uint32_t)ifindex };
    if (dev_is_lo(d)) m.scope = SCOPE_HOST;
    nl_begin(o, RTM_NEWADDR, NLF_MULTI);
    nl_put(o, &m, sizeof(m));
    nl_attr_ip(o, IFA_ADDRESS, d->ip);
    nl_attr_ip(o, IFA_LOCAL, d->ip);
    if (!dev_is_lo(d)) nl_attr_ip(o, IFA_BROADCAST, d->ip | ~d->mask);
    nl_attr(o, IFA_LABEL, d->name, (uint16_t)(strlen(d->name) + 1));
    nl_end(o);
}

static void nl_addr6(nl_out_t *o, const in6_addr_t *a, uint8_t prefix, uint8_t scope, int ifindex)
{
    nl_addr_t m = { AF_INET6, prefix, IFA_F_PERMANENT_, scope, (uint32_t)ifindex };
    nl_begin(o, RTM_NEWADDR, NLF_MULTI);
    nl_put(o, &m, sizeof(m));
    nl_attr(o, IFA_ADDRESS, a, 16);
    nl_end(o);
}

static void nl_dump_addrs(nl_out_t *o, netdev_t *d, int ifindex, int family)
{
    int lo = dev_is_lo(d);
    if (family != AF_INET6 && d->ip)
        nl_addr4(o, d, ifindex);
    if (family == AF_INET) return;
    if (lo) {
        nl_addr6(o, &d->ip6, 128, SCOPE_HOST, ifindex);
        return;
    }
    if (!in6_is_unspec(&d->ip6))
        nl_addr6(o, &d->ip6, 64, SCOPE_UNIVERSE, ifindex);
    nl_addr6(o, &d->ip6_ll, 64, SCOPE_LINK, ifindex);
}

static void nl_route4(nl_out_t *o, uint16_t flags, nl_route_t m, int ifindex,
                      uint32_t dst, uint32_t gw, uint32_t src)
{
    nl_begin(o, RTM_NEWROUTE, flags);
    nl_put(o, &m, sizeof(m));
    nl_attr_u32(o, RTA_TABLE, m.table);
    if (m.dst_len) nl_attr_ip(o, RTA_DST, dst);
    if (gw)        nl_attr_ip(o, RTA_GATEWAY, gw);
    if (src)       nl_attr_ip(o, RTA_PREFSRC, src);
    nl_attr_u32(o, RTA_OIF, (uint32_t)ifindex);
    nl_end(o);
}

static void nl_route6(nl_out_t *o, uint8_t dst_len, const void *dst, const void *gw, int ifindex)
{
    nl_route_t m = { .family = AF_INET6, .dst_len = dst_len, .table = TABLE_MAIN,
                     .protocol = gw ? PROTO_BOOT : PROTO_KERNEL,
                     .scope = SCOPE_UNIVERSE, .type = RTN_UNICAST_ };
    nl_begin(o, RTM_NEWROUTE, NLF_MULTI);
    nl_put(o, &m, sizeof(m));
    nl_attr_u32(o, RTA_TABLE, m.table);
    if (dst_len) nl_attr(o, RTA_DST, dst, 16);
    if (gw)      nl_attr(o, RTA_GATEWAY, gw, 16);
    nl_attr_u32(o, RTA_OIF, (uint32_t)ifindex);
    nl_end(o);
}

static void nl_dump_routes4(nl_out_t *o, netdev_t *d, int ifindex)
{
    if (!d->ip) return;
    nl_route_t m = { .family = AF_INET, .dst_len = mask_prefix(d->mask),
                     .table = TABLE_MAIN, .protocol = PROTO_KERNEL,
                     .scope = SCOPE_LINK, .type = RTN_UNICAST_ };
    if (dev_is_lo(d)) {
        m.table = TABLE_LOCAL; m.scope = SCOPE_HOST; m.type = RTN_LOCAL_;
    }
    nl_route4(o, NLF_MULTI, m, ifindex, d->ip & d->mask, 0, d->ip);
    if (d->gw) {
        m.dst_len = 0; m.protocol = PROTO_BOOT; m.scope = SCOPE_UNIVERSE;
        nl_route4(o, NLF_MULTI, m, ifindex, 0, d->gw, 0);
    }
}

static void nl_dump_routes6(nl_out_t *o, netdev_t *d, int ifindex)
{
    in6_addr_t net;
    if (dev_is_lo(d)) {
        nl_route6(o, 128, &d->ip6, NULL, ifindex);
        return;
    }
    if (!in6_is_unspec(&d->ip6)) {
        memset(&net, 0, sizeof(net));
        memcpy(&net, &d->ip6, 8);
        nl_route6(o, 64, &net, NULL, ifindex);
    }
    memset(&net, 0, sizeof(net));
    memcpy(&net, &d->ip6_ll, 8);
    nl_route6(o, 64, &net, NULL, ifindex);
    if (d->ip6_has_router) nl_route6(o, 0, NULL, &d->ip6_router, ifindex);
}

/* attribute `want` inside the attribute run p[0..len), or NULL */
static const void *nl_find(const uint8_t *p, size_t len, uint16_t want, uint16_t *out_len)
{
    while (len >= sizeof(nl_attr_t)) {
        nl_attr_t a;
        memcpy(&a, p, sizeof(a));
        if (a.len < sizeof(a) || a.len > len) return NULL;
        if (a.type == want) { *out_len = (uint16_t)(a.len - sizeof(a)); return p + sizeof(a); }
        uint32_t adv = (a.len + 3u) & ~3u;
        if (adv >= len) return NULL;
        p += adv; len -= adv;
    }
    return NULL;
}

/* RTM_GETLINK for one device: by ifi_index, else by IFLA_IFNAME */
static int nl_get_link(nl_out_t *o, const uint8_t *body, size_t blen)
{
    net_namespace_t *ns = o->s->net_ns;
    nl_link_t req;
    if (blen < sizeof(req)) return -22;
    memcpy(&req, body, sizeof(req));

    uint16_t nlen = 0;
    const char *name = nl_find(body + sizeof(req), blen - sizeof(req), IFLA_IFNAME, &nlen);
    netdev_t *d;
    for (int i = 0; (d = net_dev_in(ns, i)); i++) {
        if (req.index ? req.index == i + 1
                      : (name && nlen && strlen(d->name) + 1 == nlen &&
                         memcmp(d->name, name, nlen) == 0)) {
            nl_link(o, d, i + 1, 0);
            return 0;
        }
    }
    return -19; /* ENODEV */
}

/* RTM_GETROUTE for one destination (`ip route get`): IPv4, same choice as
 * net_route_dev, but against the socket's namespace */
static int nl_get_route(nl_out_t *o, const uint8_t *body, size_t blen)
{
    net_namespace_t *ns = o->s->net_ns;
    nl_route_t req;
    if (blen < sizeof(req)) return -22;
    memcpy(&req, body, sizeof(req));
    if (req.family != AF_INET) return -95; /* EOPNOTSUPP */

    uint32_t dst = 0;
    uint16_t dlen = 0;
    const void *a = nl_find(body + sizeof(req), blen - sizeof(req), RTA_DST, &dlen);
    if (a && dlen == 4) { memcpy(&dst, a, 4); dst = ntohl(dst); }

    nl_route_t m = { .family = AF_INET, .dst_len = 32, .table = TABLE_MAIN,
                     .protocol = PROTO_BOOT, .scope = SCOPE_UNIVERSE,
                     .type = RTN_UNICAST_, .flags = RTM_F_CLONED_ };
    netdev_t *d, *hit = NULL;
    int hit_i = 0;

    for (int i = 0; (d = net_dev_in(ns, i)); i++) {
        if (d->ip && d->ip == dst) {            /* one of ours: looped back */
            m.table = TABLE_LOCAL; m.scope = SCOPE_HOST; m.type = RTN_LOCAL_;
            for (int j = 0; (d = net_dev_in(ns, j)); j++)
                if (d == &ns->lo) nl_route4(o, 0, m, j + 1, dst, 0, dst);
            return 0;
        }
    }
    for (int i = 0; (d = net_dev_in(ns, i)); i++) {
        if (d->ip && (dst & d->mask) == (d->ip & d->mask)) {
            if (dev_is_lo(d)) {
                m.table = TABLE_LOCAL; m.scope = SCOPE_HOST; m.type = RTN_LOCAL_;
            }
            nl_route4(o, 0, m, i + 1, dst, 0, d->ip);
            return 0;
        }
        if (!hit && d->gw) { hit = d; hit_i = i + 1; }
    }
    if (!hit) return -101; /* ENETUNREACH */
    nl_route4(o, 0, m, hit_i, dst, hit->gw, hit->ip);
    return 0;
}

static void nl_dump(nl_out_t *o, uint16_t type, int family)
{
    netdev_t *d;
    for (int i = 0; (d = net_dev_in(o->s->net_ns, i)) && !o->err; i++) {
        if (type == RTM_GETLINK) {
            nl_link(o, d, i + 1, NLF_MULTI);
        } else if (type == RTM_GETADDR) {
            nl_dump_addrs(o, d, i + 1, family);
        } else {
            if (family != AF_INET6) nl_dump_routes4(o, d, i + 1);
            if (family != AF_INET)  nl_dump_routes6(o, d, i + 1);
        }
    }
    nl_done(o);
}

static void nl_handle(nl_out_t *o, const nl_hdr_t *h, const uint8_t *body, size_t blen)
{
    int err = 0;

    if ((h->flags & NLF_REQUEST) && h->type >= NLT_RT_FIRST) {
        if (((h->type - NLT_RT_FIRST) & 3) != RTM_KIND_GET) {
            /* nothing here changes state yet: say so rather than pretend */
            err = capable_task(task_current(), CAP_NET_ADMIN) ? -95 : -1;
        } else if (h->type != RTM_GETLINK && h->type != RTM_GETADDR && h->type != RTM_GETROUTE) {
            err = -95; /* EOPNOTSUPP */
        } else if ((h->flags & NLF_DUMP) == NLF_DUMP) {
            /* the first byte of every request body is the address family */
            nl_dump(o, h->type, blen ? body[0] : AF_UNSPEC_);
            return;
        } else if (h->type == RTM_GETLINK) {
            err = nl_get_link(o, body, blen);
        } else if (h->type == RTM_GETROUTE) {
            err = nl_get_route(o, body, blen);
        } else {
            err = -95; /* a single-address get has no rtnetlink meaning */
        }
    }
    if (err || (h->flags & NLF_ACK)) {
        nl_flush(o);    /* the ack is its own datagram, after any reply */
        nl_ack(o, h, err);
    }
}

/* One datagram from userspace: any number of messages, each answered in order.
 * A message whose length does not fit ends the batch silently, as on Linux. */
int netlink_request(socket_t *s, const void *buf, size_t len)
{
    int r = netlink_bind(s, 0, s->nl_groups);
    if (r) return r;

    nl_out_t o = { .s = s };
    const uint8_t *p = buf;
    while (len >= sizeof(nl_hdr_t)) {
        nl_hdr_t h;
        memcpy(&h, p, sizeof(h));
        if (h.len < sizeof(h) || h.len > len) break;
        if (s->rcv_buffered > NL_RCV_MAX) { o.err = -105; break; } /* ENOBUFS */
        o.seq = h.seq;
        nl_handle(&o, &h, p + sizeof(h), h.len - sizeof(h));
        nl_flush(&o);
        if (o.err) break;
        uint32_t adv = (h.len + 3u) & ~3u;
        if (adv >= len) break;
        p += adv; len -= adv;
    }
    return o.err;
}

/* ── receive ─────────────────────────────────────────────────────────────── */

#define MSG_PEEK_  0x02
#define MSG_TRUNC_ 0x20

long netlink_recv(socket_t *s, const k_iovec *iov, uint64_t niov, int flags,
                  int nonblock, int *out_flags)
{
    extern volatile uint64_t kernel_ticks;
    uint64_t deadline = s->rcv_timeo ? kernel_ticks + s->rcv_timeo : 0;
    int peek = flags & MSG_PEEK_;
    /* a peeked skb stays queued for a sibling thread to free: copy it under the lock */
    uint8_t *peeked = peek ? kmalloc(SKB_DATA_MAX) : NULL;
    if (peek && !peeked) return -12;

    skb_t *skb = NULL;
    const uint8_t *src;
    uint32_t total;
    long ret;
    for (;;) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb = s->recv_head;
        if (skb) {
            total = skb->len;
            if (peek) {
                memcpy(peeked, skb->data, total);
                skb = NULL;
            } else {
                s->recv_head = skb->next;
                if (!s->recv_head) s->recv_tail = NULL;
                s->rcv_buffered -= total;
            }
            spin_unlock_irq(&s->recv_lock, f);
            src = peek ? peeked : skb->data;
            break;
        }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); ret = -11; goto out; }
        if (deadline && kernel_ticks >= deadline) {
            spin_unlock_irq(&s->recv_lock, f); ret = -11; goto out;   /* SO_RCVTIMEO */
        }
        s->recv_waiter = task_current();
        if (deadline) { task_current()->wake_deadline = deadline; sched_note_wake_deadline(deadline); }
        task_sleep_unlock(&s->recv_lock, f);
        task_current()->wake_deadline = 0;
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; ret = -4; goto out; }
    }

    uint32_t done = 0;
    for (uint64_t i = 0; i < niov && done < total; i++) {
        uint32_t c = total - done;
        if (iov[i].len < c) c = (uint32_t)iov[i].len;
        if (c && copy_fop_to_user(iov[i].base, src + done, c) != 0) { ret = -14; goto out; }
        done += c;
    }
    *out_flags = (done < total) ? MSG_TRUNC_ : 0;
    ret = (flags & MSG_TRUNC_) ? (long)total : (long)done;
out:
    if (peek) kfree(peeked);
    else      skb_free(skb);
    return ret;
}
