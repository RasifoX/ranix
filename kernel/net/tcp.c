#include <kernel/net/tcp.h>
#include <kernel/net/ip.h>
#include <kernel/net/ipv6.h>
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

/* unacked send-queue segment; defined early so tcp_tx_seg can read the OOO list */
typedef struct tcp_seg {
    struct tcp_seg *next;
    uint32_t  seq;
    uint16_t  len;        /* payload bytes */
    uint8_t   flags;
    uint8_t   resent;     /* retransmitted at least once — Karn: skip its RTT */
    uint8_t   sacked;     /* peer SACKed this segment — don't retransmit it */
    uint64_t  sent_tick;
    uint8_t   data[];
} tcp_seg_t;
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

    /* Build TCP options. SYN: MSS, SACK-permitted, timestamp, window-scale.
     * Other segments: timestamp (if negotiated) + SACK blocks for buffered OOO
     * data (RFC 1323/2018/7323). */
    uint8_t opts[40];
    int optlen = 0;
    uint32_t tsval = htonl((uint32_t)kernel_ticks);
    uint32_t tsecr = htonl(s->ts_recent);
    if (flags & TCP_SYN) {
        opts[optlen++] = 2; opts[optlen++] = 4;
        opts[optlen++] = (uint8_t)(TCP_MSS >> 8); opts[optlen++] = (uint8_t)TCP_MSS;
        opts[optlen++] = 4; opts[optlen++] = 2;   /* SACK-permitted */
        opts[optlen++] = 8; opts[optlen++] = 10;  /* timestamp */
        memcpy(opts + optlen, &tsval, 4); optlen += 4;
        memcpy(opts + optlen, &tsecr, 4); optlen += 4;
        opts[optlen++] = 3; opts[optlen++] = 3; opts[optlen++] = TCP_WSCALE;
    } else {
        if (s->ts_ok) {
            opts[optlen++] = 1; opts[optlen++] = 1;   /* NOP, NOP — align TS */
            opts[optlen++] = 8; opts[optlen++] = 10;
            memcpy(opts + optlen, &tsval, 4); optlen += 4;
            memcpy(opts + optlen, &tsecr, 4); optlen += 4;
        }
        if ((flags & TCP_ACK) && s->sack_ok && s->ooo_head) {
            opts[optlen++] = 1; opts[optlen++] = 1;   /* NOP, NOP — align SACK */
            int li = optlen; opts[optlen++] = 5; opts[optlen++] = 0;
            int nb = 0;
            for (tcp_seg_t *g = s->ooo_head; g && nb < 3 && optlen + 8 <= (int)sizeof(opts); ) {
                uint32_t l = g->seq, r = g->seq + g->len;
                while (g->next && g->next->seq == r) { g = g->next; r = g->seq + g->len; }
                uint32_t nl = htonl(l), nr = htonl(r);
                memcpy(opts + optlen, &nl, 4); optlen += 4;
                memcpy(opts + optlen, &nr, 4); optlen += 4;
                nb++; g = g->next;
            }
            opts[li + 1] = (uint8_t)(2 + 8 * nb);
        }
    }
    while (optlen & 3) opts[optlen++] = 1;   /* NOP-pad to a 4-byte boundary */

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

    if (optlen) memcpy(base + sizeof(tcp_hdr_t), opts, optlen);

    th->checksum = 0;
    if (s->domain == AF_INET6) {
        in6_addr_t src = ipv6_src_for(s->dev, &s->remote_ip6);
        th->checksum = ipv6_pseudo_cksum(&src, &s->remote_ip6, IP6_NH_TCP, th, skb->len);
        return ipv6_tx(s->dev, skb, &s->remote_ip6, IP6_NH_TCP);
    }
    th->checksum = tcp_cksum(s->local_ip ? s->local_ip : s->dev->ip,
                             s->remote_ip, th, skb->len);

    return ip_tx(s->dev, skb, s->remote_ip, IP_PROTO_TCP);
}

/* ── send window: queue of unacked segments ──────────────── */

#define TCP_CWND_INIT  (10u * TCP_MSS)  /* IW10 (RFC 6928) */
#define TCP_CWND_MAX   (1u << 24)       /* 16MB ceiling; window scaling caps the real rate */
#define TCP_RETX_GIVEUP 8

/* CUBIC (RFC 8312), integer-only. Window in MSS-segments, time in milliseconds.
 *   W(t) = C·(t_s − K)³ + W_max,  C = 0.4 seg/s³,  beta = 0.7
 * Scaled to ms with t_s = t_ms/1000, C·(t_ms/1000)³ = CUBIC_C_NUM·t_ms³/CUBIC_C_DEN
 * (2/5e9 = 0.4/1e9). K_ms = cbrt(W_max·(1−beta)/C)·1000 = cbrt(W_max·3/4·1e9). */
#define CUBIC_C_NUM  2ULL
#define CUBIC_C_DEN  5000000000ULL
#define CUBIC_BETA   7u     /* W_max multiplier on loss, /10 */
#define CUBIC_BETA_SCALE 10u
#define TICK_MS      10u    /* one timer tick, ms (the 100Hz the RTO consts assume) */

/* Serialises every TCP state-machine mutation — RX processing (tcp_rx, tcp_tick)
 * and the syscalls (connect/send/close) — so a close() and an incoming FIN can't
 * clobber tcp_state when RX runs on another CPU. Strict order: tcp_lock →
 * recv_lock → sched_lock. NEVER held across a sleep; blocking waits use recv_lock
 * and re-check state (an aligned int read) after each wakeup. */
static spinlock_t tcp_lock = SPINLOCK_INIT;

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

/* ── congestion control: CUBIC (default) + NewReno ───────── */

/* Integer cube root of a 64-bit value (Newton's method). */
static uint32_t icbrt(uint64_t x)
{
    if (x == 0) return 0;
    uint64_t r = 1;
    while (r * r * r <= x) r <<= 1;   /* bracket from above */
    r >>= 1;
    for (int i = 0; i < 8; i++) {
        uint64_t r2 = r * r;
        if (r2 == 0) break;
        r = (2 * r + x / r2) / 3;     /* x^(1/3) fixed point of t→(2t+x/t²)/3 */
    }
    while (r * r * r > x) r--;
    return (uint32_t)r;
}

/* New congestion epoch: recompute the cubic origin and the time-to-origin K (ms)
 * from the window at the moment of loss (RFC 8312 §3.1). cubic_wmax already holds
 * the post-fast-convergence W_max set by tcp_cc_on_loss. */
static void tcp_cubic_epoch(socket_t *s)
{
    s->cubic_epoch = kernel_ticks ? kernel_ticks : 1;
    s->cubic_tcp   = s->cwnd;
    s->cubic_cnt   = 0;
    uint32_t wmax_seg = s->cubic_wmax / TCP_MSS;
    uint32_t cwnd_seg = s->cwnd / TCP_MSS;
    if (wmax_seg <= cwnd_seg) {            /* no plateau above us → grow from here */
        s->cubic_K      = 0;
        s->cubic_origin = s->cwnd;
    } else {
        /* K = cbrt(W_max·(1−beta)/C)·1000  ⇒  cbrt(W_max·3/4·1e9) ms */
        s->cubic_K      = icbrt((uint64_t)wmax_seg * 3 * 1000000000ULL / 4);
        s->cubic_origin = s->cubic_wmax;
    }
}

/* CUBIC window for the current epoch, blended with a Reno-friendly estimate so a
 * single CUBIC flow never underperforms Reno on short/low-BDP paths. */
static void tcp_cubic_update(socket_t *s, uint32_t acked)
{
    if (s->cubic_epoch == 0) tcp_cubic_epoch(s);

    uint64_t t_ms = (kernel_ticks - s->cubic_epoch) * TICK_MS;
    int64_t  d    = (int64_t)t_ms - (int64_t)s->cubic_K;   /* ms from the origin */
    if (d >  (1 << 20)) d =  (1 << 20);   /* keep d³ in int64; window saturates anyway */
    if (d < -(1 << 20)) d = -(1 << 20);
    int64_t  cube = CUBIC_C_NUM * d * d * d / (int64_t)CUBIC_C_DEN;   /* segments */
    int64_t  target_seg = (int64_t)(s->cubic_origin / TCP_MSS) + cube;
    if (target_seg < 1) target_seg = 1;
    uint32_t target = (uint32_t)target_seg * TCP_MSS;

    /* per-ACK gate: it takes cnt acked segments to add one MSS */
    uint32_t cwnd_seg = s->cwnd / TCP_MSS;
    if (cwnd_seg == 0) cwnd_seg = 1;
    uint32_t cnt = (target > s->cwnd) ? s->cwnd / (target - s->cwnd) : 100u * cwnd_seg;
    if (cnt == 0) cnt = 1;

    /* Reno-friendly region: track Reno's window (bytes) and, if it would be
     * ahead, clamp cnt so CUBIC grows at least as fast (RFC 8312 §4.2). Reno adds
     * MSS²/cwnd per ACK; CUBIC-TCP scales that by 3(1−beta)/(1+beta). */
    uint32_t aimd = (TCP_MSS * TCP_MSS) / s->cwnd;
    s->cubic_tcp += aimd * 3u * (CUBIC_BETA_SCALE - CUBIC_BETA) / (CUBIC_BETA_SCALE + CUBIC_BETA);
    if (s->cubic_tcp > s->cwnd) {
        uint32_t tcp_cnt = s->cwnd / (s->cubic_tcp - s->cwnd + 1);
        if (tcp_cnt < cnt) cnt = tcp_cnt ? tcp_cnt : 1;
    }

    s->cubic_cnt += acked / TCP_MSS ? acked / TCP_MSS : 1;
    if (s->cubic_cnt >= cnt) { s->cwnd += TCP_MSS; s->cubic_cnt = 0; }
}

/* One ACK acknowledged bytes_acked new bytes: advance the window. */
static void tcp_cc_on_ack(socket_t *s, uint32_t bytes_acked)
{
    if (!bytes_acked) return;
    if (s->cwnd < s->ssthresh) {                       /* slow start (both algorithms) */
        s->cwnd += bytes_acked > TCP_MSS ? TCP_MSS : bytes_acked;
        if (s->cwnd >= s->ssthresh && s->cc_cubic) tcp_cubic_epoch(s);
    } else if (s->cc_cubic) {
        tcp_cubic_update(s, bytes_acked);
    } else {
        s->cwnd += (TCP_MSS * TCP_MSS) / (s->cwnd ? s->cwnd : TCP_MSS);   /* Reno AIMD */
    }
    if (s->cwnd > TCP_CWND_MAX) s->cwnd = TCP_CWND_MAX;
}

/* Loss event (3 dup-ACKs or RTO): set the new ssthresh and reduce the window.
 * CUBIC multiplicative decrease with fast convergence; Reno halves. */
static void tcp_cc_on_loss(socket_t *s)
{
    if (s->cc_cubic) {
        if (s->cwnd < s->cubic_wmax)               /* fast convergence (RFC 8312 §4.2) */
            s->cubic_wmax = s->cwnd * (CUBIC_BETA_SCALE + CUBIC_BETA) / (2u * CUBIC_BETA_SCALE);
        else
            s->cubic_wmax = s->cwnd;
        s->ssthresh = s->cwnd * CUBIC_BETA / CUBIC_BETA_SCALE;
        s->cubic_epoch = 0;                        /* start a fresh epoch on recovery */
    } else {
        s->ssthresh = s->cwnd / 2;
    }
    if (s->ssthresh < 2u * TCP_MSS) s->ssthresh = 2u * TCP_MSS;
}

/* Remove fully-acked segments, take an RTT sample, and grow the window. */
static void seg_ack(socket_t *s, uint32_t ack, uint32_t bytes_acked)
{
    int sampled = 0;
    while (s->unacked_head) {
        tcp_seg_t *g = s->unacked_head;
        if (!seq_le(g->seq + seg_span(g), ack)) break;   /* not fully acked */
        if (!g->resent && !sampled && !s->ts_ok) { tcp_rtt_update(s, g->sent_tick); sampled = 1; }
        s->unacked_head = g->next;
        if (!s->unacked_head) s->unacked_tail = NULL;
        kfree(g);
    }
    s->retx_deadline = s->unacked_head ? (kernel_ticks + s->rto) : 0;
    s->retx_count = 0;
    tcp_cc_on_ack(s, bytes_acked);
}

/* Initialise the congestion state when a connection is set up. */
static void tcp_cc_init(socket_t *s)
{
    s->cwnd        = TCP_CWND_INIT;
    s->ssthresh    = TCP_CWND_MAX;
    s->dup_acks    = 0;
    s->in_recovery = 0;
    s->cc_cubic    = 1;          /* CUBIC by default */
    s->cubic_wmax  = 0;
    s->cubic_epoch = 0;
    s->cubic_cnt   = 0;
    if (!s->snd_wnd) s->snd_wnd = TCP_RCV_WND;
}

/* Retransmit the oldest unacked segment the peer has NOT SACKed. */
static void tcp_retransmit_one(socket_t *s)
{
    tcp_seg_t *g = s->unacked_head;
    while (g && g->sacked) g = g->next;
    if (!g) return;
    tcp_tx_seg(s, g->flags, g->seq, s->rcv_nxt, g->len ? g->data : NULL, g->len);
    g->resent = 1;
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

/* Close on a LISTEN socket: detach the accept queue (the RX thread enqueues
 * completed connections under recv_lock), then RST and free each one so they
 * don't linger in gsock[] being driven by tcp_tick forever. */
void tcp_listener_abort(socket_t *ls)
{
    uint64_t rf = spin_lock_irq(&ls->recv_lock);
    socket_t *q = ls->accept_head;
    ls->accept_head = ls->accept_tail = NULL;
    spin_unlock_irq(&ls->recv_lock, rf);

    while (q) {
        socket_t *next = q->accept_head;   /* accept_head is reused as the list link */
        uint64_t tl = spin_lock_irq(&tcp_lock);
        if (q->tcp_state != TCP_CLOSED) {
            tcp_tx_seg(q, TCP_RST | TCP_ACK, q->snd_nxt, q->rcv_nxt, NULL, 0);
            q->tcp_state = TCP_CLOSED;
        }
        spin_unlock_irq(&tcp_lock, tl);
        sock_free(q);                       /* unregister + free, outside tcp_lock */
        q = next;
    }
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
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->local_port  != local_port)  continue;
        if (s->remote_port != remote_port) continue;
        if (s->remote_ip   != remote_ip)   continue;
        (void)local_ip;
        sock_hold(s);
        spin_unlock_irq(&gsock_lock, f);
        return s;
    }
    spin_unlock_irq(&gsock_lock, f);
    return NULL;
}

socket_t *tcp_sock_find6(struct net_namespace *ns, uint16_t local_port,
                         const in6_addr_t *remote_ip6, uint16_t remote_port)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->domain != AF_INET6) continue;
        if (s->local_port  != local_port)  continue;
        if (s->remote_port != remote_port) continue;
        if (!in6_eq(&s->remote_ip6, remote_ip6)) continue;
        sock_hold(s);
        spin_unlock_irq(&gsock_lock, f);
        return s;
    }
    spin_unlock_irq(&gsock_lock, f);
    return NULL;
}

socket_t *tcp_listen_find(struct net_namespace *ns, uint16_t local_port, int domain)
{
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->net_ns != ns) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->domain != domain) continue;
        if (s->tcp_state != TCP_LISTEN) continue;
        if (s->local_port != local_port) continue;
        sock_hold(s);
        spin_unlock_irq(&gsock_lock, f);
        return s;
    }
    spin_unlock_irq(&gsock_lock, f);
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

static void send_rst_v4(netdev_t *dev, uint32_t peer_ip, uint32_t our_ip,
                        tcp_hdr_t *th, uint16_t seglen)
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
    r->checksum = tcp_cksum(our_ip, peer_ip, r, skb->len);
    ip_tx(dev, skb, peer_ip, IP_PROTO_TCP);
}

/* IPv6 RST for an unexpected segment. src6/dst6 are the received packet's
 * addresses; the RST is sent back to src6 with us (dst6) as the source. */
static void send_rst6(netdev_t *dev, const in6_addr_t *src6, const in6_addr_t *dst6,
                      tcp_hdr_t *th, uint16_t seglen)
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
    r->checksum = ipv6_pseudo_cksum(dst6, src6, IP6_NH_TCP, r, skb->len);
    ipv6_tx(dev, skb, src6, IP6_NH_TCP);
}

/* RX addressing, family-tagged so the state machine is shared by v4 and v6. */
typedef struct {
    uint8_t    family;
    uint32_t   src4, dst4;   /* AF_INET, host byte order */
    in6_addr_t src6, dst6;   /* AF_INET6 */
} tcp_rxkey_t;

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

/* Did this SYN carry the SACK-permitted option (RFC 2018)? */
static int tcp_opt_sack_perm(const tcp_hdr_t *th, uint8_t hlen)
{
    const uint8_t *o   = (const uint8_t *)th + sizeof(tcp_hdr_t);
    const uint8_t *end = (const uint8_t *)th + hlen;
    while (o < end) {
        if (o[0] == 0) break;
        if (o[0] == 1) { o++; continue; }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if (o[0] == 4 && o[1] == 2) return 1;
        o += o[1];
    }
    return 0;
}

/* Extract the timestamp option's TSval/TSecr (RFC 7323); 1 if present. */
static int tcp_opt_ts(const tcp_hdr_t *th, uint8_t hlen, uint32_t *tsval, uint32_t *tsecr)
{
    const uint8_t *o   = (const uint8_t *)th + sizeof(tcp_hdr_t);
    const uint8_t *end = (const uint8_t *)th + hlen;
    while (o < end) {
        if (o[0] == 0) break;
        if (o[0] == 1) { o++; continue; }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if (o[0] == 8 && o[1] == 10) {
            uint32_t v, e;
            memcpy(&v, o + 2, 4); memcpy(&e, o + 6, 4);
            *tsval = ntohl(v); *tsecr = ntohl(e);
            return 1;
        }
        o += o[1];
    }
    return 0;
}

/* Mark unacked segments the peer reported as received via SACK blocks, so the
 * retransmit path skips them and resends only the real gaps. */
static void tcp_process_sack(socket_t *s, const tcp_hdr_t *th, uint8_t hlen)
{
    const uint8_t *o   = (const uint8_t *)th + sizeof(tcp_hdr_t);
    const uint8_t *end = (const uint8_t *)th + hlen;
    while (o < end) {
        if (o[0] == 0) break;
        if (o[0] == 1) { o++; continue; }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if (o[0] == 5 && o[1] >= 10 && ((o[1] - 2) % 8) == 0) {
            int nb = (o[1] - 2) / 8;
            for (int i = 0; i < nb; i++) {
                uint32_t l, r;
                memcpy(&l, o + 2 + i * 8, 4);
                memcpy(&r, o + 2 + i * 8 + 4, 4);
                l = ntohl(l); r = ntohl(r);
                for (tcp_seg_t *g = s->unacked_head; g; g = g->next)
                    if (seq_le(l, g->seq) && seq_le(g->seq + seg_span(g), r))
                        g->sacked = 1;
            }
        }
        o += o[1];
    }
}

static void tcp_input(netdev_t *dev, skb_t *skb, const tcp_rxkey_t *key)
{
    if (skb->len < (uint16_t)sizeof(tcp_hdr_t)) { skb_free(skb); return; }

    tcp_hdr_t *th  = (tcp_hdr_t *)skb->data;
    uint8_t  hlen  = (uint8_t)((th->data_off >> 4) * 4);
    if (hlen < sizeof(tcp_hdr_t) || hlen > skb->len) { skb_free(skb); return; }

    uint16_t src_port = ntohs(th->src_port);
    uint16_t dst_port = ntohs(th->dst_port);
    uint32_t src_ip   = key->src4;
    uint32_t seq      = ntohl(th->seq);
    uint32_t ack      = ntohl(th->ack);
    uint8_t  flags    = th->flags;
    int      syn_ws   = (flags & TCP_SYN) ? tcp_opt_wscale(th, hlen) : -1;
    int      syn_sack = (flags & TCP_SYN) ? tcp_opt_sack_perm(th, hlen) : 0;
    uint8_t  rx_hlen  = hlen;   /* keep for SACK-block parsing after skb_pull */
    uint32_t rx_tsval = 0, rx_tsecr = 0;
    int      rx_has_ts = tcp_opt_ts(th, hlen, &rx_tsval, &rx_tsecr);
    int      v6        = (key->family == AF_INET6);

    skb_pull(skb, hlen);
    uint16_t dlen = skb->len;

    /* find socket */
    socket_t *s = v6
        ? tcp_sock_find6(dev->net_ns, dst_port, &key->src6, src_port)
        : tcp_sock_find(dev->net_ns, key->dst4, dst_port, src_ip, src_port);

    if (!s) {
        /* check for listen socket */
        socket_t *ls = tcp_listen_find(dev->net_ns, dst_port, key->family);
        if (ls && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
            /* incoming SYN → create new socket, send SYN-ACK */
            socket_t *ns = sock_alloc(key->family, SOCK_STREAM, IP_PROTO_TCP);
            if (!ns) { skb_free(skb); sock_put(ls); return; }
            ns->dev         = dev;
            if (v6) { ns->local_ip6 = key->dst6; ns->remote_ip6 = key->src6; }
            else    { ns->local_ip  = key->dst4; ns->remote_ip  = src_ip;    }
            ns->local_port  = dst_port;
            ns->remote_port = src_port;
            ns->rcv_nxt     = seq + 1;
            ns->snd_nxt     = 0x12340000;
            ns->snd_una     = ns->snd_nxt;
            ns->tcp_state   = TCP_SYN_RECV;
            ns->snd_wnd     = ntohs(th->window);   /* SYN window is unscaled */
            if (syn_ws >= 0) { ns->snd_wscale = (uint8_t)syn_ws; ns->rcv_wscale = TCP_WSCALE; }
            if (syn_sack) ns->sack_ok = 1;
            if (rx_has_ts) { ns->ts_ok = 1; ns->ts_recent = rx_tsval; }
            tcp_cc_init(ns);
            sock_register(ns);

            tcp_tx_seg(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, ns->rcv_nxt, NULL, 0);
            seg_enqueue(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, NULL, 0);
            ns->snd_nxt++;

            skb_free(skb); sock_put(ls); return;
        }
        /* unexpected segment → RST */
        if (!(flags & TCP_RST)) {
            uint16_t seglen = dlen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
            if (v6) send_rst6(dev, &key->src6, &key->dst6, th, seglen);
            else    send_rst_v4(dev, key->src4, key->dst4, th, seglen);
        }
        sock_put(ls);
        skb_free(skb); return;
    }

    /* serialise the per-socket state machine against the syscalls */
    uint64_t tl = spin_lock_irq(&tcp_lock);

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
        spin_unlock_irq(&tcp_lock, tl);
        sock_put(s);
        skb_free(skb); return;
    }

    switch (s->tcp_state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack != s->snd_nxt) break;   /* stale/duplicate SYN-ACK — unlock+free below */
            s->rcv_nxt    = seq + 1;
            s->snd_una    = ack;
            s->snd_wnd    = ntohs(th->window);   /* SYN-ACK window is unscaled */
            if (syn_ws >= 0) { s->snd_wscale = (uint8_t)syn_ws; s->rcv_wscale = TCP_WSCALE; }
            if (syn_sack) s->sack_ok = 1;
            if (rx_has_ts) { s->ts_ok = 1; s->ts_recent = rx_tsval; }
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
            socket_t *ls = tcp_listen_find(s->net_ns, s->local_port, s->domain);
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
            sock_put(ls);
        }
        break;

    case TCP_ESTABLISHED:
        if (s->ts_ok && rx_has_ts && seq == s->rcv_nxt) s->ts_recent = rx_tsval;
        /* ACK advances snd_una; 3 duplicate ACKs trigger fast retransmit and the
         * NewReno fast-recovery loop (RFC 5681 §3.2, RFC 6582). */
        if (flags & TCP_ACK) {
            if (s->sack_ok && rx_hlen > sizeof(tcp_hdr_t)) tcp_process_sack(s, th, rx_hlen);
            if (s->ts_ok && rx_has_ts && rx_tsecr)        /* RTT from echoed timestamp (no Karn) */
                tcp_rtt_update(s, rx_tsecr);
            if (!seq_le(ack, s->snd_una)) {           /* ack > snd_una: new data acked */
                uint32_t acked = ack - s->snd_una;
                uint32_t old_una = s->snd_una;
                s->snd_una = ack;
                if (s->in_recovery) {
                    seg_ack(s, ack, 0);               /* free acked segments, no cwnd growth */
                    if (seq_le(s->recover, ack)) {    /* full ACK → leave fast recovery */
                        s->in_recovery = 0;
                        s->dup_acks = 0;
                        s->cwnd = s->ssthresh;        /* deflate (RFC 6582 §3.2 step 6) */
                    } else {                          /* partial ACK: resend next, deflate */
                        tcp_retransmit_one(s);
                        uint32_t dec = ack - old_una;
                        s->cwnd = (s->cwnd > dec) ? s->cwnd - dec : TCP_MSS;
                        if (dec >= TCP_MSS) s->cwnd += TCP_MSS;
                    }
                } else {
                    s->dup_acks = 0;
                    seg_ack(s, ack, acked);
                }
            } else if (ack == s->snd_una && dlen == 0 && s->unacked_head &&
                       !(flags & (TCP_SYN | TCP_FIN))) {
                if (s->in_recovery) {
                    s->cwnd += TCP_MSS;               /* inflate per extra dup-ACK */
                } else if (++s->dup_acks == 3) {
                    tcp_cc_on_loss(s);
                    s->recover     = s->snd_nxt;      /* recovery point (NewReno) */
                    s->in_recovery = 1;
                    s->cwnd        = s->ssthresh + 3u * TCP_MSS;
                    tcp_retransmit_one(s);
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

    spin_unlock_irq(&tcp_lock, tl);
    sock_put(s);
    skb_free(skb);
}

void tcp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph)
{
    tcp_rxkey_t key = { .family = AF_INET,
                        .src4 = ntohl(iph->src), .dst4 = ntohl(iph->dst) };
    tcp_input(dev, skb, &key);
}

void tcp6_rx(netdev_t *dev, skb_t *skb, ipv6_hdr_t *ip6)
{
    tcp_rxkey_t key = { .family = AF_INET6, .src6 = ip6->src, .dst6 = ip6->dst };
    tcp_input(dev, skb, &key);
}

/* ── public API ──────────────────────────────────────────── */

/* TCP_INFO prefix, matching the Linux/musl struct tcp_info byte layout up to
 * snd_cwnd (the fields a getsockopt(TCP_INFO) reader cares about for cc state).
 * Windows are reported in MSS units. */
typedef struct {
    uint8_t  state, ca_state, retransmits, probes, backoff, options, wscale, flags;
    uint32_t rto, ato, snd_mss, rcv_mss;
    uint32_t unacked, sacked, lost, retrans, fackets;
    uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
    uint32_t pmtu, rcv_ssthresh, rtt, rttvar, snd_ssthresh, snd_cwnd;
} __attribute__((packed)) tcp_info_t;

/* Fill caller's buffer with up to sizeof(tcp_info_t) bytes; returns bytes written. */
uint32_t tcp_get_info(socket_t *s, void *buf, uint32_t buflen)
{
    tcp_info_t ti;
    memset(&ti, 0, sizeof(ti));
    uint64_t tl = spin_lock_irq(&tcp_lock);
    ti.state        = (uint8_t)s->tcp_state;
    ti.ca_state     = s->in_recovery ? 3 : 0;   /* TCP_CA_Recovery / TCP_CA_Open */
    ti.backoff      = (uint8_t)s->retx_count;
    ti.options      = (uint8_t)((s->ts_ok ? 1 : 0) | (s->sack_ok ? 2 : 0) |
                                (s->rcv_wscale ? 4 : 0));
    ti.wscale       = (uint8_t)((s->snd_wscale & 0xf) | ((s->rcv_wscale & 0xf) << 4));
    ti.rto          = s->rto;
    ti.snd_mss      = TCP_MSS;
    ti.rcv_mss      = TCP_MSS;
    ti.unacked      = (s->snd_nxt - s->snd_una) / TCP_MSS;
    ti.rtt          = s->srtt;
    ti.rttvar       = s->rttvar;
    ti.snd_ssthresh = s->ssthresh / TCP_MSS;
    ti.snd_cwnd     = s->cwnd / TCP_MSS;
    spin_unlock_irq(&tcp_lock, tl);

    uint32_t n = buflen < sizeof(ti) ? buflen : sizeof(ti);
    if (n) memcpy(buf, &ti, n);
    return n;
}

int tcp_connect(socket_t *s)
{
    s->dev = (s->domain == AF_INET6) ? net_route_dev6(&s->remote_ip6)
                                     : net_route_dev(s->remote_ip);
    uint64_t tl = spin_lock_irq(&tcp_lock);
    if (!s->local_port) s->local_port = alloc_port();
    if (s->domain == AF_INET6) s->local_ip6 = ipv6_src_for(s->dev, &s->remote_ip6);
    else if (!s->local_ip)     s->local_ip  = s->dev->ip;
    s->snd_una   = 0xAB000000;
    s->snd_nxt   = s->snd_una + 1;   /* SYN consumes one seq — advance now so a
                                      * SYN-ACK racing in (fast net thread) matches */
    s->rcv_nxt   = 0;
    s->snd_wnd   = 1024;
    s->tcp_state = TCP_SYN_SENT;
    spin_unlock_irq(&tcp_lock, tl);
    sock_register(s);

    /* ARP retry loop: ip_tx returns -1 on ARP miss; poll until resolved. Must
     * run WITHOUT tcp_lock — net_poll re-enters tcp_rx which takes it. */
    int rc = -1;
    for (int i = 0; i < 64 && rc == -1; i++) {
        rc = tcp_tx_seg(s, TCP_SYN, s->snd_una, 0, NULL, 0);
        if (rc == -1) net_poll();
    }
    tl = spin_lock_irq(&tcp_lock);
    if (s->tcp_state == TCP_SYN_SENT)   /* still connecting — SYN-ACK may have raced in */
        seg_enqueue(s, TCP_SYN, s->snd_una, NULL, 0);
    spin_unlock_irq(&tcp_lock, tl);
    return 0;
}

int tcp_send(socket_t *s, const void *data, uint16_t len)
{
    uint64_t tl = spin_lock_irq(&tcp_lock);
    if (s->tcp_state != TCP_ESTABLISHED) { spin_unlock_irq(&tcp_lock, tl); return -107; }

    /* effective window = min(congestion, peer-advertised); send while it has room */
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    uint32_t wnd = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
    if (in_flight >= wnd) { spin_unlock_irq(&tcp_lock, tl); return -11; }

    uint32_t room = wnd - in_flight;
    uint16_t seglen = len > TCP_MSS ? TCP_MSS : len;
    if (seglen > room) seglen = (uint16_t)room;
    if (seglen == 0) { spin_unlock_irq(&tcp_lock, tl); return -11; }

    int rc = tcp_tx_seg(s, TCP_ACK | TCP_PSH, s->snd_nxt, s->rcv_nxt, data, seglen);
    if (rc < 0) { spin_unlock_irq(&tcp_lock, tl); return rc; }
    seg_enqueue(s, TCP_ACK | TCP_PSH, s->snd_nxt, data, seglen);
    s->snd_nxt += seglen;
    spin_unlock_irq(&tcp_lock, tl);
    return (int)seglen;
}

int tcp_close(socket_t *s)
{
    uint64_t tl = spin_lock_irq(&tcp_lock);
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
    spin_unlock_irq(&tcp_lock, tl);
    return 0;
}

int tcp_listen(socket_t *s, int backlog)
{
    (void)backlog;
    if (!s->local_port) return -22;
    uint64_t tl = spin_lock_irq(&tcp_lock);
    s->tcp_state = TCP_LISTEN;
    spin_unlock_irq(&tcp_lock, tl);
    sock_register(s);
    return 0;
}

/* ── retransmit tick (called from sched_tick) ────────────── */

void tcp_tick(void)
{
    for (int i = 0; i < gsock_cap; i++) {
        /* Read + pin the slot under gsock_lock: a concurrent close runs
         * sock_free under it, and gsock_ensure may realloc the array, so the
         * bare gsock[i] read + deref raced both. */
        uint64_t gf = spin_lock_irq(&gsock_lock);
        socket_t *s = (i < gsock_cap) ? gsock[i] : NULL;
        if (s && (s->type != SOCK_STREAM ||
                  s->retx_deadline == 0 || kernel_ticks < s->retx_deadline))
            s = NULL;
        if (s) sock_hold(s);
        spin_unlock_irq(&gsock_lock, gf);
        if (!s) continue;

        uint64_t tl = spin_lock_irq(&tcp_lock);   /* serialise vs RX + syscalls */
        tcp_seg_t *g = s->unacked_head;
        if (!g) { s->retx_deadline = 0; spin_unlock_irq(&tcp_lock, tl); sock_put(s); continue; }

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
            spin_unlock_irq(&tcp_lock, tl);
            sock_put(s);
            continue;
        }

        /* timeout: drop ssthresh per the active algorithm, collapse cwnd to slow
         * start, abandon any fast recovery, retransmit the oldest non-SACKed
         * segment, and back off the timer (RFC 6298 §5.5). */
        tcp_cc_on_loss(s);
        s->cwnd        = TCP_MSS;
        s->dup_acks    = 0;
        s->in_recovery = 0;
        s->cubic_epoch = 0;
        tcp_retransmit_one(s);
        s->rto = (s->rto ? s->rto : TCP_RTO_INIT) * 2;
        if (s->rto > TCP_RTO_MAX) s->rto = TCP_RTO_MAX;
        s->retx_deadline = kernel_ticks + s->rto;
        spin_unlock_irq(&tcp_lock, tl);
        sock_put(s);
    }
}
