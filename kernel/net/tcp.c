#include <kernel/net/tcp.h>
#include <kernel/net/ip.h>
#include <kernel/net/net.h>
#include <kernel/net/socket.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/proc.h>

extern volatile uint64_t kernel_ticks;
extern uint32_t TIMER_HZ;
void task_wake(process_t *p);

#define TCP_RTO_INIT    100   /* 1s before the first RTT sample (RFC 6298 §2.1) */
#define TCP_RTO_MIN     20    /* 200ms floor */
#define TCP_RTO_MAX     6000  /* 60s ceiling */
#define TCP_MSS        (uint16_t)(SKB_DATA_MAX - sizeof(tcp_hdr_t) - sizeof(ip_hdr_t) - 14)
#define TCP_RCV_WND     (256u * 1024u)  /* receive buffer / max window (needs scaling) */
#define TCP_WSCALE      2               /* we advertise window >> 2 (×4 → up to 256KB) */

/* ── checksum ────────────────────────────────────────────── */

static uint16_t tcp_cksum(uint32_t src, uint32_t dst,
                          const void *tcpdata, uint16_t tcplen)
{
    tcp_pseudo_t ph;
    ph.src      = htonl(src);
    ph.dst      = htonl(dst);
    ph.zero     = 0;
    ph.proto    = IP_PROTO_TCP;
    ph.tcp_len  = htons(tcplen);

    /* sum pseudo header then TCP segment */
    uint32_t acc = 0;
    const uint16_t *p = (const uint16_t *)&ph;
    for (int i = 0; i < (int)sizeof(ph) / 2; i++) acc += p[i];
    p = (const uint16_t *)tcpdata;
    int n = tcplen;
    while (n > 1) { acc += *p++; n -= 2; }
    if (n) acc += *(const uint8_t *)p;
    while (acc >> 16) acc = (acc & 0xffff) + (acc >> 16);
    return ~(uint16_t)acc;
}

/* ── raw segment transmit ────────────────────────────────── */

static int tcp_tx_seg(socket_t *s, uint8_t flags,
                      uint32_t seq, uint32_t ack,
                      const void *data, uint16_t dlen)
{
    skb_t *skb = skb_alloc();
    if (!skb) return -12;

    /* copy payload */
    if (dlen) { memcpy(skb->data, data, dlen); skb->len = dlen; }
    else        skb->len = 0;

    /* SYN segments carry MSS + window-scale options (RFC 1323) */
    int optlen = (flags & TCP_SYN) ? 8 : 0;
    uint8_t *base = (uint8_t *)skb_push(skb, sizeof(tcp_hdr_t) + optlen);
    tcp_hdr_t *th = (tcp_hdr_t *)base;
    memset(th, 0, sizeof(tcp_hdr_t));
    th->src_port = htons(s->local_port);
    th->dst_port = htons(s->remote_port);
    th->seq      = htonl(seq);
    th->ack      = (flags & TCP_ACK) ? htonl(ack) : 0;
    th->data_off = (uint8_t)(((sizeof(tcp_hdr_t) + optlen) / 4) << 4);
    th->flags    = flags;

    /* advertised window: raw on the SYN, scaled afterwards */
    uint32_t rwnd = (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
    if (!(flags & TCP_SYN)) rwnd >>= s->rcv_wscale;
    th->window   = htons((uint16_t)(rwnd > 65535 ? 65535 : rwnd));

    if (optlen) {
        uint8_t *o = base + sizeof(tcp_hdr_t);
        o[0] = 2; o[1] = 4; o[2] = (uint8_t)(TCP_MSS >> 8); o[3] = (uint8_t)TCP_MSS; /* MSS */
        o[4] = 3; o[5] = 3; o[6] = TCP_WSCALE;                                       /* WScale */
        o[7] = 1;                                                                    /* NOP pad */
    }

    th->checksum = 0;
    th->checksum = tcp_cksum(s->local_ip ? s->local_ip : s->dev->ip,
                             s->remote_ip, th, skb->len);

    return ip_tx(s->dev, skb, s->remote_ip, IP_PROTO_TCP);
}

/* ── send window: queue of unacked segments ──────────────── */

#define TCP_CWND_INIT  (4u * TCP_MSS)   /* initial congestion window */
#define TCP_CWND_MAX   65535u
#define TCP_RETX_GIVEUP 8

typedef struct tcp_seg {
    struct tcp_seg *next;
    uint32_t  seq;
    uint16_t  len;        /* payload bytes */
    uint8_t   flags;
    uint8_t   resent;     /* retransmitted at least once — Karn: skip its RTT */
    uint64_t  sent_tick;
    uint8_t   data[];
} tcp_seg_t;

/* sequence numbers a segment consumes (SYN and FIN each count as one) */
static uint32_t seg_span(const tcp_seg_t *g)
{
    return (uint32_t)g->len + ((g->flags & (TCP_SYN | TCP_FIN)) ? 1u : 0u);
}

/* serial comparison: is a <= b in 32-bit sequence space (RFC 1982) */
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(b - a) >= 0; }

/* Fold a round-trip sample (sent at sent_tick) into srtt/rttvar/rto (RFC 6298). */
static void tcp_rtt_update(socket_t *s, uint64_t sent_tick)
{
    uint32_t r = (uint32_t)(kernel_ticks - sent_tick);
    if (s->srtt == 0) {
        s->srtt = r; s->rttvar = r / 2;
    } else {
        uint32_t d = (s->srtt > r) ? (s->srtt - r) : (r - s->srtt);
        s->rttvar = (3 * s->rttvar + d) / 4;
        s->srtt   = (7 * s->srtt + r) / 8;
    }
    uint32_t rto = s->srtt + 4 * s->rttvar;
    if (rto < TCP_RTO_MIN) rto = TCP_RTO_MIN;
    if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
    s->rto = rto;
}

/* Record a just-transmitted segment so it can be retransmitted until acked. */
static void seg_enqueue(socket_t *s, uint8_t flags, uint32_t seq,
                        const void *data, uint16_t len)
{
    tcp_seg_t *g = kmalloc(sizeof(*g) + len);
    if (!g) return;   /* untracked: simply won't be retransmitted */
    g->next = NULL; g->seq = seq; g->len = len; g->flags = flags;
    g->resent = 0; g->sent_tick = kernel_ticks;
    if (len) memcpy(g->data, data, len);
    if (s->rto == 0) s->rto = TCP_RTO_INIT;
    if (s->unacked_tail) s->unacked_tail->next = g;
    else { s->unacked_head = g; s->retx_deadline = kernel_ticks + s->rto; }
    s->unacked_tail = g;
}

/* Drop every queued segment (connection reset / teardown). */
static void seg_clear(socket_t *s)
{
    tcp_seg_t *g = s->unacked_head;
    while (g) { tcp_seg_t *n = g->next; kfree(g); g = n; }
    s->unacked_head = s->unacked_tail = NULL;
    s->retx_deadline = 0;
    s->retx_count    = 0;
    s->dup_acks      = 0;
}

/* Remove fully-acked segments, take an RTT sample, and grow the window. */
static void seg_ack(socket_t *s, uint32_t ack, uint32_t bytes_acked)
{
    int sampled = 0;
    while (s->unacked_head) {
        tcp_seg_t *g = s->unacked_head;
        if (!seq_le(g->seq + seg_span(g), ack)) break;   /* not fully acked */
        if (!g->resent && !sampled) { tcp_rtt_update(s, g->sent_tick); sampled = 1; }
        s->unacked_head = g->next;
        if (!s->unacked_head) s->unacked_tail = NULL;
        kfree(g);
    }
    s->retx_deadline = s->unacked_head ? (kernel_ticks + s->rto) : 0;
    s->retx_count = 0;
    if (bytes_acked) {
        if (s->cwnd < s->ssthresh) s->cwnd += bytes_acked;                 /* slow start */
        else s->cwnd += (TCP_MSS * TCP_MSS) / (s->cwnd ? s->cwnd : TCP_MSS); /* avoidance */
        if (s->cwnd > TCP_CWND_MAX) s->cwnd = TCP_CWND_MAX;
    }
}

/* Initialise the congestion state when a connection is set up. */
static void tcp_cc_init(socket_t *s)
{
    s->cwnd     = TCP_CWND_INIT;
    s->ssthresh = TCP_CWND_MAX;
    s->dup_acks = 0;
    if (!s->snd_wnd) s->snd_wnd = TCP_RCV_WND;
}

/* ── out-of-order receive buffer ─────────────────────────── */

#define TCP_OOO_MAX  32   /* segments buffered ahead of rcv_nxt */

static void tcp_ooo_insert(socket_t *s, uint32_t seq, const void *data, uint16_t len)
{
    tcp_seg_t **pp = &s->ooo_head;
    int count = 0;
    while (*pp && seq_le((*pp)->seq, seq)) {
        if ((*pp)->seq == seq) return;   /* already buffered */
        pp = &(*pp)->next; count++;
    }
    for (tcp_seg_t *q = *pp; q; q = q->next) count++;
    if (count >= TCP_OOO_MAX) return;    /* bound the buffer */

    tcp_seg_t *g = kmalloc(sizeof(*g) + len);
    if (!g) return;
    g->seq = seq; g->len = len; g->flags = 0; g->resent = 0; g->sent_tick = 0;
    if (len) memcpy(g->data, data, len);
    g->next = *pp; *pp = g;
}

/* Deliver buffered segments that have become contiguous with rcv_nxt. */
static void tcp_ooo_drain(socket_t *s)
{
    while (s->ooo_head) {
        tcp_seg_t *g = s->ooo_head;
        if (seq_le(g->seq + g->len, s->rcv_nxt)) {   /* stale: already delivered */
            s->ooo_head = g->next; kfree(g); continue;
        }
        if (g->seq != s->rcv_nxt) break;             /* still a gap */
        s->ooo_head = g->next;
        s->rcv_nxt += g->len;
        sock_tcp_deliver(s, g->data, g->len);
        kfree(g);
    }
}

/* Release all TCP queues (called from sock_free). */
void tcp_sock_free(socket_t *s)
{
    seg_clear(s);
    tcp_seg_t *g = s->ooo_head;
    while (g) { tcp_seg_t *n = g->next; kfree(g); g = n; }
    s->ooo_head = NULL;
}

/* The app drained n bytes from the recv buffer; the advertised window grows.
 * If it reopened from (near) zero, send a window-update ACK so a sender that
 * stalled on a full window resumes. */
void tcp_rx_consumed(socket_t *s, uint32_t n)
{
    if (s->type != SOCK_STREAM) return;
    uint32_t before = (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
    s->rcv_buffered = (n > s->rcv_buffered) ? 0 : s->rcv_buffered - n;
    uint32_t after = (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
    if (s->tcp_state == TCP_ESTABLISHED && before < TCP_MSS && after >= TCP_MSS)
        tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
}

/* Wake the connect()/accept()/close() sleeper. The caller has already updated
 * tcp_state (and any accept queue), so a waiter that re-checks its condition
 * under recv_lock after we release it sees the new state — the read+clear here
 * happening-after the state write makes a lost wakeup structurally impossible,
 * without locking the whole TCP state machine. */
static void tcp_wake_close(socket_t *s)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    process_t *w = s->close_waiter;
    s->close_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
}

static void tcp_wake_connect(socket_t *s)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    process_t *w = s->connect_waiter;
    s->connect_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
}

/* ── port allocation ─────────────────────────────────────── */

static uint16_t   next_ephemeral  = 49152;
static spinlock_t ephemeral_lock  = SPINLOCK_INIT;

uint16_t alloc_port(void)
{
    spin_lock(&ephemeral_lock);
    uint16_t p = next_ephemeral++;
    if (next_ephemeral == 0) next_ephemeral = 49152;
    spin_unlock(&ephemeral_lock);
    return p;
}

/* ── socket lookup ───────────────────────────────────────── */

socket_t *tcp_sock_find(struct net_namespace *ns, uint32_t local_ip, uint16_t local_port,
                        uint32_t remote_ip, uint16_t remote_port)
{
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->local_port  != local_port)  continue;
        if (s->remote_port != remote_port) continue;
        if (s->remote_ip   != remote_ip)   continue;
        (void)local_ip;
        return s;
    }
    return NULL;
}

socket_t *tcp_listen_find(struct net_namespace *ns, uint16_t local_port)
{
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->tcp_state != TCP_LISTEN) continue;
        if (s->local_port != local_port) continue;
        return s;
    }
    return NULL;
}

/* ── data delivery to socket recv queue ─────────────────── */

void sock_tcp_deliver(socket_t *s, const void *data, uint16_t len)
{
    if (!len) return;
    skb_t *skb = skb_alloc();
    if (!skb) return;
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    memcpy(skb->data, data, len);
    skb->len = len;

    extern void spin_lock_irq_fn(void *);  /* not needed — use macro */
    uint64_t f = spin_lock_irq(&s->recv_lock);
    skb->next = NULL;
    if (s->recv_tail) s->recv_tail->next = skb;
    else              s->recv_head = skb;
    s->recv_tail = skb;
    s->rcv_buffered += len;
    process_t *w = s->recv_waiter;
    s->recv_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
}

/* ── RST sender ──────────────────────────────────────────── */

static void send_rst(netdev_t *dev, ip_hdr_t *iph, tcp_hdr_t *th, uint16_t seglen)
{
    skb_t *skb = skb_alloc();
    if (!skb) return;
    skb->len = 0;

    tcp_hdr_t *r = (tcp_hdr_t *)skb_push(skb, sizeof(tcp_hdr_t));
    memset(r, 0, sizeof(tcp_hdr_t));
    r->src_port = th->dst_port;
    r->dst_port = th->src_port;
    if (th->flags & TCP_ACK) {
        r->seq   = th->ack;
        r->flags = TCP_RST;
    } else {
        r->seq   = 0;
        r->ack   = htonl(ntohl(th->seq) + seglen);
        r->flags = TCP_RST | TCP_ACK;
    }
    r->data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    r->window   = 0;
    uint32_t src = ntohl(iph->dst);
    uint32_t dst = ntohl(iph->src);
    r->checksum = tcp_cksum(src, dst, r, skb->len);
    /* fabricate a temporary socket for ip_tx */
    socket_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.dev        = dev;
    tmp.local_ip   = src;
    tmp.remote_ip  = dst;
    tmp.local_port = ntohs(th->dst_port);
    tmp.remote_port= ntohs(th->src_port);
    ip_tx(dev, skb, dst, IP_PROTO_TCP);
}

/* ── main RX path ────────────────────────────────────────── */

/* Scan a SYN segment's options for the window-scale shift (RFC 1323).
 * Returns the shift, or -1 if the peer offered no window scaling. */
static int tcp_opt_wscale(const tcp_hdr_t *th, uint8_t hlen)
{
    const uint8_t *o   = (const uint8_t *)th + sizeof(tcp_hdr_t);
    const uint8_t *end = (const uint8_t *)th + hlen;
    while (o < end) {
        if (o[0] == 0) break;                 /* end of options */
        if (o[0] == 1) { o++; continue; }      /* NOP */
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if (o[0] == 3 && o[1] == 3) return o[2];   /* window scale */
        o += o[1];
    }
    return -1;
}

void tcp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph)
{
    if (skb->len < (uint16_t)sizeof(tcp_hdr_t)) { skb_free(skb); return; }

    tcp_hdr_t *th  = (tcp_hdr_t *)skb->data;
    uint8_t  hlen  = (uint8_t)((th->data_off >> 4) * 4);
    if (hlen < sizeof(tcp_hdr_t) || hlen > skb->len) { skb_free(skb); return; }

    uint16_t src_port = ntohs(th->src_port);
    uint16_t dst_port = ntohs(th->dst_port);
    uint32_t src_ip   = ntohl(iph->src);
    uint32_t seq      = ntohl(th->seq);
    uint32_t ack      = ntohl(th->ack);
    uint8_t  flags    = th->flags;
    int      syn_ws   = (flags & TCP_SYN) ? tcp_opt_wscale(th, hlen) : -1;

    skb_pull(skb, hlen);
    uint16_t dlen = skb->len;

    /* find socket */
    socket_t *s = tcp_sock_find(dev->net_ns, ntohl(iph->dst), dst_port, src_ip, src_port);

    if (!s) {
        /* check for listen socket */
        socket_t *ls = tcp_listen_find(dev->net_ns, dst_port);
        if (ls && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
            /* incoming SYN → create new socket, send SYN-ACK */
            socket_t *ns = sock_alloc(AF_INET, SOCK_STREAM, IP_PROTO_TCP);
            if (!ns) { skb_free(skb); return; }
            ns->dev         = dev;
            ns->local_ip    = ntohl(iph->dst);
            ns->local_port  = dst_port;
            ns->remote_ip   = src_ip;
            ns->remote_port = src_port;
            ns->rcv_nxt     = seq + 1;
            ns->snd_nxt     = 0x12340000;
            ns->snd_una     = ns->snd_nxt;
            ns->tcp_state   = TCP_SYN_RECV;
            ns->snd_wnd     = ntohs(th->window);   /* SYN window is unscaled */
            if (syn_ws >= 0) { ns->snd_wscale = (uint8_t)syn_ws; ns->rcv_wscale = TCP_WSCALE; }
            tcp_cc_init(ns);
            sock_register(ns);

            tcp_tx_seg(ns, TCP_SYN | TCP_ACK,
                       ns->snd_nxt, ns->rcv_nxt, NULL, 0);
            seg_enqueue(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, NULL, 0);
            ns->snd_nxt++;

            skb_free(skb); return;
        }
        /* unexpected segment → RST */
        if (!(flags & TCP_RST)) {
            uint16_t seglen = dlen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
            send_rst(dev, iph, th, seglen);
        }
        skb_free(skb); return;
    }

    /* RST handling */
    if (flags & TCP_RST) {
        s->tcp_state = TCP_CLOSED;
        seg_clear(s);
        tcp_wake_connect(s);
        uint64_t lf = spin_lock_irq(&s->recv_lock);
        process_t *w = s->recv_waiter; s->recv_waiter = NULL;
        spin_unlock_irq(&s->recv_lock, lf);
        if (w) task_wake(w);
        tcp_wake_close(s);
        skb_free(skb); return;
    }

    switch (s->tcp_state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack != s->snd_nxt) { skb_free(skb); return; }
            s->rcv_nxt    = seq + 1;
            s->snd_una    = ack;
            s->snd_wnd    = ntohs(th->window);   /* SYN-ACK window is unscaled */
            if (syn_ws >= 0) { s->snd_wscale = (uint8_t)syn_ws; s->rcv_wscale = TCP_WSCALE; }
            s->tcp_state  = TCP_ESTABLISHED;
            tcp_cc_init(s);
            seg_ack(s, ack, 0);
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            tcp_wake_connect(s);
        }
        break;

    case TCP_SYN_RECV:
        /* expecting ACK of SYN-ACK */
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            s->snd_una   = ack;
            s->snd_wnd   = (uint32_t)ntohs(th->window) << s->snd_wscale;
            s->tcp_state = TCP_ESTABLISHED;
            tcp_cc_init(s);
            seg_ack(s, ack, 0);
            /* move to listen socket's accept queue */
            socket_t *ls = tcp_listen_find(s->net_ns, s->local_port);
            if (ls) {
                /* enqueue on the listener's accept list and wake the acceptor —
                 * all under ls->recv_lock so it can't race accept()'s dequeue */
                uint64_t af = spin_lock_irq(&ls->recv_lock);
                s->accept_head = NULL; /* reuse field as list link */
                if (!ls->accept_head) ls->accept_head = s;
                else                  ls->accept_tail->accept_head = s;
                ls->accept_tail = s;
                process_t *aw = ls->accept_waiter;
                ls->accept_waiter = NULL;
                spin_unlock_irq(&ls->recv_lock, af);
                if (aw) task_wake(aw);
            }
        }
        break;

    case TCP_ESTABLISHED:
        /* ACK advances snd_una; duplicate ACKs trigger fast retransmit */
        if (flags & TCP_ACK) {
            if (!seq_le(ack, s->snd_una)) {           /* ack > snd_una: new data acked */
                uint32_t acked = ack - s->snd_una;
                s->snd_una = ack;
                s->dup_acks = 0;
                seg_ack(s, ack, acked);
            } else if (ack == s->snd_una && dlen == 0 && s->unacked_head &&
                       !(flags & (TCP_SYN | TCP_FIN))) {
                if (++s->dup_acks == 3) {             /* three dup ACKs → resend oldest */
                    tcp_seg_t *g = s->unacked_head;
                    tcp_tx_seg(s, g->flags, g->seq, s->rcv_nxt,
                               g->len ? g->data : NULL, g->len);
                    g->resent = 1;
                    uint32_t inf = s->snd_nxt - s->snd_una;
                    s->ssthresh = (inf / 2 > 2u * TCP_MSS) ? inf / 2 : 2u * TCP_MSS;
                    s->cwnd     = s->ssthresh + 3u * TCP_MSS;
                }
            }
            s->snd_wnd = (uint32_t)ntohs(th->window) << s->snd_wscale;
            tcp_wake_connect(s);
        }
        /* deliver data, reassembling out-of-order arrivals */
        if (dlen > 0) {
            if (seq == s->rcv_nxt) {
                s->rcv_nxt += dlen;
                sock_tcp_deliver(s, skb->data, dlen);
                tcp_ooo_drain(s);
            } else if (!seq_le(seq, s->rcv_nxt)) {   /* seq > rcv_nxt: future, buffer it */
                tcp_ooo_insert(s, seq, skb->data, dlen);
            }
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        }
        /* FIN from remote */
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            s->tcp_state = TCP_CLOSE_WAIT;
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            /* wake blocked recv — sock_read_vfs sees CLOSE_WAIT and returns 0 */
            uint64_t lf = spin_lock_irq(&s->recv_lock);
            process_t *rw = s->recv_waiter;
            s->recv_waiter = NULL;
            spin_unlock_irq(&s->recv_lock, lf);
            if (rw) task_wake(rw);
        }
        break;

    case TCP_CLOSE_WAIT:
        if ((flags & TCP_ACK) && !seq_le(ack, s->snd_una)) {
            uint32_t acked = ack - s->snd_una;
            s->snd_una = ack;
            seg_ack(s, ack, acked);
            s->snd_wnd = (uint32_t)ntohs(th->window) << s->snd_wscale;
            tcp_wake_connect(s);
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            s->tcp_state = TCP_CLOSED;
            seg_clear(s);
            tcp_wake_close(s);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) {
            if (ack == s->snd_nxt) {
                seg_clear(s);
                s->tcp_state = TCP_FIN_WAIT2;
            }
        }
        /* simultaneous close */
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            s->tcp_state = (s->tcp_state == TCP_FIN_WAIT2) ? TCP_TIME_WAIT : TCP_CLOSING;
            if (s->tcp_state == TCP_TIME_WAIT) tcp_wake_close(s);
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            s->tcp_state = TCP_TIME_WAIT;
            tcp_wake_close(s);
        }
        break;

    case TCP_CLOSING:
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            seg_clear(s);
            s->tcp_state = TCP_TIME_WAIT;
            tcp_wake_close(s);
        }
        break;

    case TCP_TIME_WAIT:
        /* re-ACK any FIN we see */
        if (flags & TCP_FIN) {
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        }
        break;

    default: break;
    }

    skb_free(skb);
}

/* ── public API ──────────────────────────────────────────── */

int tcp_connect(socket_t *s)
{
    s->dev = net_route_dev(s->remote_ip);
    if (!s->local_port) s->local_port = alloc_port();
    if (!s->local_ip)   s->local_ip   = s->dev->ip;
    s->snd_nxt   = 0xAB000000;
    s->snd_una   = s->snd_nxt;
    s->rcv_nxt   = 0;
    s->snd_wnd   = 1024;
    s->tcp_state = TCP_SYN_SENT;
    sock_register(s);

    /* ARP retry loop: ip_tx returns -1 on ARP miss; poll until resolved */
    int rc = -1;
    for (int i = 0; i < 64 && rc == -1; i++) {
        rc = tcp_tx_seg(s, TCP_SYN, s->snd_nxt, 0, NULL, 0);
        if (rc == -1) net_poll();
    }
    seg_enqueue(s, TCP_SYN, s->snd_nxt, NULL, 0);
    s->snd_nxt++;
    return 0;
}

int tcp_send(socket_t *s, const void *data, uint16_t len)
{
    if (s->tcp_state != TCP_ESTABLISHED) return -107; /* ENOTCONN */

    /* effective window = min(congestion, peer-advertised); send while it has room */
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    uint32_t wnd = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
    if (in_flight >= wnd) return -11; /* window full — caller blocks and retries */

    uint32_t room = wnd - in_flight;
    uint16_t seglen = len > TCP_MSS ? TCP_MSS : len;
    if (seglen > room) seglen = (uint16_t)room;
    if (seglen == 0) return -11;

    int rc = tcp_tx_seg(s, TCP_ACK | TCP_PSH, s->snd_nxt, s->rcv_nxt, data, seglen);
    if (rc < 0) return rc;
    seg_enqueue(s, TCP_ACK | TCP_PSH, s->snd_nxt, data, seglen);
    s->snd_nxt += seglen;
    return (int)seglen;
}

int tcp_close(socket_t *s)
{
    if (s->tcp_state == TCP_ESTABLISHED) {
        tcp_tx_seg(s, TCP_FIN | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        seg_enqueue(s, TCP_FIN | TCP_ACK, s->snd_nxt, NULL, 0);
        s->snd_nxt++;
        s->tcp_state = TCP_FIN_WAIT1;
    } else if (s->tcp_state == TCP_CLOSE_WAIT) {
        tcp_tx_seg(s, TCP_FIN | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        seg_enqueue(s, TCP_FIN | TCP_ACK, s->snd_nxt, NULL, 0);
        s->snd_nxt++;
        s->tcp_state = TCP_LAST_ACK;
    }
    return 0;
}

int tcp_listen(socket_t *s, int backlog)
{
    (void)backlog;
    if (!s->local_port) return -22;
    s->tcp_state = TCP_LISTEN;
    sock_register(s);
    return 0;
}

/* ── retransmit tick (called from sched_tick) ────────────── */

void tcp_tick(void)
{
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s || s->type != SOCK_STREAM) continue;
        if (s->retx_deadline == 0 || kernel_ticks < s->retx_deadline) continue;

        tcp_seg_t *g = s->unacked_head;
        if (!g) { s->retx_deadline = 0; continue; }

        s->retx_count++;
        if (s->retx_count > TCP_RETX_GIVEUP) {
            /* give up — force close and wake anyone waiting */
            s->tcp_state = TCP_CLOSED;
            seg_clear(s);
            tcp_wake_close(s);
            uint64_t lf = spin_lock_irq(&s->recv_lock);
            process_t *w = s->recv_waiter; s->recv_waiter = NULL;
            spin_unlock_irq(&s->recv_lock, lf);
            if (w) task_wake(w);
            continue;
        }

        /* timeout: collapse to slow start, retransmit the oldest segment, and
         * back off the timer (RFC 6298 §5.5). */
        uint32_t inf = s->snd_nxt - s->snd_una;
        s->ssthresh = (inf / 2 > 2u * TCP_MSS) ? inf / 2 : 2u * TCP_MSS;
        s->cwnd     = TCP_MSS;
        s->dup_acks = 0;
        tcp_tx_seg(s, g->flags, g->seq, s->rcv_nxt, g->len ? g->data : NULL, g->len);
        g->resent = 1;
        s->rto = (s->rto ? s->rto : TCP_RTO_INIT) * 2;
        if (s->rto > TCP_RTO_MAX) s->rto = TCP_RTO_MAX;
        s->retx_deadline = kernel_ticks + s->rto;
    }
}
