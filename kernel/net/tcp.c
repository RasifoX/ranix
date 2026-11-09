#include <kernel/board.h>
#include <kernel/cgroup.h>
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
#include <kernel/time.h>
#include <kernel/errno.h>

void task_wake(process_t *p);

#define TCP_RTO_INIT    TIMER_HZ   /* 1s before the first RTT sample (RFC 6298 §2.1) */
#define TCP_RTO_MIN     (TIMER_HZ / 5)   /* 200ms floor */
#define TCP_RTO_MAX     (60 * TIMER_HZ)  /* 60s ceiling */
#define TCP_MSS        1460u  /* IPv4 on a 1500-byte MTU; a segment must fit one frame */
#define TCP_MSS6       1440u
#define TCP_OPT_TS     12u    /* the timestamp option on every data segment */
#define TCP_FIN_LINGER (30u * TIMER_HZ)  /* orphan in FIN_WAIT2: the peer may never close */
#define TCP_TIME_WAIT_TICKS (5u * TIMER_HZ)  /* well short of 2MSL: gsock[] is scanned per packet */

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

static uint32_t tcp_rwnd(const socket_t *s)
{
    if (s->rcv_refused) return 0;   /* the budget said no: promising room invites loss */
    return (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
}

/* ── raw segment transmit ────────────────────────────────── */

static int tcp_tx_seg(socket_t *s, uint8_t flags,
                      uint32_t seq, uint32_t ack,
                      const void *data, uint16_t dlen)
{
    skb_t *skb = skb_alloc();
    if (!skb) return -ENOMEM;

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
        uint32_t mss = s->domain == AF_INET6 ? TCP_MSS6 : TCP_MSS;
        opts[optlen++] = 2; opts[optlen++] = 4;
        opts[optlen++] = (uint8_t)(mss >> 8); opts[optlen++] = (uint8_t)mss;
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
        if ((flags & TCP_ACK) && !dlen && s->sack_ok && s->ooo_head) {
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
    uint32_t rwnd = tcp_rwnd(s);
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
#define TICK_MS      (1000u / TIMER_HZ)    /* one timer tick, ms */

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
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(b - a) > 0; }

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

/* Record a segment so it can be retransmitted until acked. Nonzero: not recorded,
 * so the caller must not count it as sent. */
static int seg_enqueue(socket_t *s, uint8_t flags, uint32_t seq,
                       const void *data, uint16_t len)
{
    tcp_seg_t *g = kmalloc(sizeof(*g) + len);
    if (!g) return -1;
    g->next = NULL; g->seq = seq; g->len = len; g->flags = flags;
    g->resent = 0; g->sacked = 0; g->sent_tick = kernel_ticks;
    if (len) memcpy(g->data, data, len);
    if (s->rto == 0) s->rto = TCP_RTO_INIT;
    if (s->unacked_tail) s->unacked_tail->next = g;
    else { s->unacked_head = g; s->retx_deadline = kernel_ticks + s->rto; }
    s->unacked_tail = g;
    return 0;
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
    s->persist_deadline = 0;
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

/* Retransmit the oldest unacked segment the peer has not SACKed and this recovery
 * has not resent yet. */
static void tcp_retransmit_one(socket_t *s)
{
    tcp_seg_t *g = s->unacked_head;
    while (g && (g->sacked || seq_lt(g->seq, s->rtx_next))) g = g->next;
    if (!g) return;
    tcp_tx_seg(s, g->flags, g->seq, s->rcv_nxt, g->len ? g->data : NULL, g->len);
    g->resent = 1;
    s->rtx_next = g->seq + seg_span(g);
}

/* As above, only for a real hole: something after it has been SACKed. */
static void tcp_retransmit_hole(socket_t *s)
{
    tcp_seg_t *g = s->unacked_head;
    while (g && (g->sacked || seq_lt(g->seq, s->rtx_next))) g = g->next;
    for (tcp_seg_t *q = g; q; q = q->next)
        if (q->sacked) { tcp_retransmit_one(s); return; }
}

/* ── out-of-order receive buffer ─────────────────────────── */

#define TCP_OOO_MAX  (TCP_RCV_WND / TCP_MSS + 8)   /* the whole window after one loss */

static void tcp_ooo_insert(socket_t *s, uint32_t seq, const void *data, uint16_t len)
{
    tcp_seg_t **pp = &s->ooo_head;
    int count = 0;
    while (*pp && seq_le((*pp)->seq, seq)) {
        if ((*pp)->seq == seq) return;   /* already buffered */
        pp = &(*pp)->next; count++;
    }
    for (tcp_seg_t *q = *pp; q; q = q->next) count++;
    if (count >= (int)TCP_OOO_MAX) return;    /* bound the buffer */

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
        if (seq_lt(s->rcv_nxt, g->seq)) break;       /* still a gap */
        /* may overlap what is already in: a peer need not resend on the old edges */
        uint16_t off = (uint16_t)(s->rcv_nxt - g->seq);
        uint16_t want = g->len - off;
        uint16_t n = sock_tcp_deliver(s, g->data + off, want);
        s->rcv_nxt += n;
        if (n < want) { s->rcv_refused = 1; break; }
        s->ooo_head = g->next;
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

static int tcp_can_rx(const socket_t *s)
{
    return s->tcp_state == TCP_ESTABLISHED || s->tcp_state == TCP_FIN_WAIT1 ||
           s->tcp_state == TCP_FIN_WAIT2;
}

/* The app drained n bytes from the recv buffer; the advertised window grows.
 * If it reopened from (near) zero, send a window-update ACK so a sender that
 * stalled on a full window resumes. */
void tcp_rx_consumed(socket_t *s, uint32_t n)
{
    if (s->type != SOCK_STREAM) return;
    uint64_t f = spin_lock_irq(&s->recv_lock);
    uint32_t before = (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
    s->rcv_buffered = (n > s->rcv_buffered) ? 0 : s->rcv_buffered - n;
    uint32_t after = (TCP_RCV_WND > s->rcv_buffered) ? TCP_RCV_WND - s->rcv_buffered : 0;
    spin_unlock_irq(&s->recv_lock, f);
    if (s->domain == AF_UNIX || !tcp_can_rx(s)) return;

    /* Reassembled data refused for want of budget can go now. Nothing else would
     * retry it: the sender does not resend what the receiver has SACKed. */
    uint64_t tl = spin_lock_irq(&tcp_lock);
    uint32_t was = s->rcv_nxt;
    int refused = s->rcv_refused;
    s->rcv_refused = 0;
    tcp_ooo_drain(s);
    if (s->rcv_nxt != was || (refused && !s->rcv_refused) ||
        (before < TCP_MSS && after >= TCP_MSS))
        tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
    spin_unlock_irq(&tcp_lock, tl);
}

/* Wake the connect()/send() sleeper. The caller has already updated tcp_state,
 * the send window and any accept queue, so a waiter that re-checks its condition
 * under recv_lock after we release it sees the new state — the read+clear here
 * happening-after the state write makes a lost wakeup structurally impossible,
 * without locking the whole TCP state machine. */
static void tcp_wake_connect(socket_t *s)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    process_t *w = s->connect_waiter;
    s->connect_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
}

static void tcp_wake_recv(socket_t *s)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    process_t *w = s->recv_waiter;
    s->recv_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
    poll_notify_key(s);
}

/* The connection is over; err is left for the next syscall to report. */
static void tcp_closed(socket_t *s, int err)
{
    if (err) s->so_error = err;
    s->tcp_state = TCP_CLOSED;
    s->linger_deadline = 0;
    seg_clear(s);
    tcp_wake_connect(s);
    tcp_wake_recv(s);
}

/* Room for a full segment, or for anything at all when nothing is in flight: no
 * ACK is coming then to open more. */
int tcp_can_send(const socket_t *s)
{
    if (s->tcp_state != TCP_ESTABLISHED && s->tcp_state != TCP_CLOSE_WAIT) return 0;
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    uint32_t wnd = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
    if (in_flight >= wnd) return 0;
    return !in_flight || wnd - in_flight >= s->snd_mss;
}

/* Sleep until tcp_send can make progress or the connection has an answer. The
 * timeout is a backstop; every path that opens the window wakes this. */
int tcp_send_wait(socket_t *s)
{
    process_t *cur = task_current();
    uint64_t f = spin_lock_irq(&s->recv_lock);
    if (tcp_can_send(s) || (s->tcp_state != TCP_ESTABLISHED && s->tcp_state != TCP_CLOSE_WAIT &&
                            s->tcp_state != TCP_SYN_SENT)) {
        spin_unlock_irq(&s->recv_lock, f);
        return 0;
    }
    s->connect_waiter = cur;
    cur->wake_deadline = kernel_ticks + TIMER_HZ;
    sched_note_wake_deadline(cur->wake_deadline);
    task_sleep_unlock(&s->recv_lock, f);
    cur->wake_deadline = 0;
    f = spin_lock_irq(&s->recv_lock);
    if (s->connect_waiter == cur) s->connect_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    return task_has_actionable_signal() ? -4 : 0;
}

/* ── port allocation ─────────────────────────────────────── */

static uint16_t   next_ephemeral  = 49152;
static spinlock_t ephemeral_lock  = SPINLOCK_INIT;

uint16_t alloc_port(void)
{
    uint64_t f = spin_lock_irq(&ephemeral_lock);
    uint16_t p = next_ephemeral++;
    if (next_ephemeral == 0) next_ephemeral = 49152;
    spin_unlock_irq(&ephemeral_lock, f);
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
        if (s->tcp_state == TCP_CLOSED)    continue;   /* gets the peer a RST */
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
        if (s->tcp_state == TCP_CLOSED) continue;
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
    /* none for IPv4: an IPv6 listener on "::" takes it too, unless IPV6_V6ONLY */
    if (domain == AF_INET) {
        static const in6_addr_t any6;
        for (int i = 0; i < gsock_cap; i++) {
            socket_t *s = gsock[i];
            if (!s || s->net_ns != ns || s->type != SOCK_STREAM) continue;
            if (s->domain != AF_INET6 || s->v6only || s->tcp_state != TCP_LISTEN) continue;
            if (s->local_port != local_port) continue;
            if (memcmp(&s->local_ip6, &any6, sizeof(any6)) != 0) continue;
            sock_hold(s);
            spin_unlock_irq(&gsock_lock, f);
            return s;
        }
    }
    spin_unlock_irq(&gsock_lock, f);
    return NULL;
}

/* ── data delivery to socket recv queue ─────────────────── */

/* Bytes queued, from the front of data: the caller acknowledges only those. A
 * segment reassembled from IP fragments can be longer than one skb. */
int sock_tcp_deliver(socket_t *s, const void *data, uint16_t len)
{
    uint16_t done = 0;
    while (done < len) {
        skb_t *skb = skb_alloc();
        if (!skb) break;
        uint16_t n = len - done > SKB_DATA_MAX ? SKB_DATA_MAX : len - done;
        memcpy(skb->data, (const uint8_t *)data + done, n);
        skb->len = n;
        if (sock_charge_skb(s, skb) != 0) { skb_free(skb); break; }
        sock_queue_skb(s, skb);
        done += n;
    }
    return done;
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

/* The peer's MSS from a SYN, or the RFC 1122 default when it sent none. */
static uint32_t tcp_opt_mss(const tcp_hdr_t *th, uint8_t hlen)
{
    const uint8_t *o   = (const uint8_t *)th + sizeof(tcp_hdr_t);
    const uint8_t *end = (const uint8_t *)th + hlen;
    while (o < end) {
        if (o[0] == 0) break;
        if (o[0] == 1) { o++; continue; }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end) break;
        if (o[0] == 2 && o[1] == 4) return ((uint32_t)o[2] << 8) | o[3];
        o += o[1];
    }
    return 536;
}

/* Payload of a full segment on this connection, once ts_ok is settled. */
static void tcp_set_mss(socket_t *s, uint32_t peer_mss)
{
    uint32_t ours = s->domain == AF_INET6 ? TCP_MSS6 : TCP_MSS;
    uint32_t mss = peer_mss < ours ? peer_mss : ours;
    if (s->ts_ok) mss -= TCP_OPT_TS;
    s->snd_mss = mss < 64 ? 64 : mss;
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

/* Everything an ACK says, in any state past the handshake: 3 duplicate ACKs trigger
 * fast retransmit and the NewReno fast-recovery loop (RFC 5681 §3.2, RFC 6582). */
static void tcp_ack_in(socket_t *s, uint32_t ack, const tcp_hdr_t *th, uint8_t hlen,
                       int has_ts, uint32_t tsecr, uint16_t dlen, uint8_t flags)
{
    uint32_t old_wnd = s->snd_wnd;
    uint32_t wnd = (uint32_t)ntohs(th->window) << s->snd_wscale;

    if (s->sack_ok && hlen > sizeof(tcp_hdr_t)) tcp_process_sack(s, th, hlen);
    if (seq_lt(s->snd_una, ack) && seq_le(ack, s->snd_nxt)) {   /* new data acked */
        uint32_t acked = ack - s->snd_una;
        s->snd_una = ack;
        /* only here: a duplicate echoes an old TSval and would undo the backoff */
        if (s->ts_ok && has_ts && tsecr) tcp_rtt_update(s, tsecr);
        if (s->in_recovery) {
            /* 2 is recovery from an RTO: slow start, not a deflating fast recovery */
            int rto = s->in_recovery == 2;
            seg_ack(s, ack, rto ? acked : 0);
            if (seq_le(s->recover, ack)) {        /* full ACK → leave recovery */
                s->in_recovery = 0;
                s->dup_acks = 0;
                if (!rto) s->cwnd = s->ssthresh;  /* deflate (RFC 6582 §3.2 step 6) */
            } else if (rto) {
                tcp_retransmit_one(s);
                tcp_retransmit_one(s);
            } else {                              /* partial ACK: resend next, deflate */
                tcp_retransmit_one(s);
                s->cwnd = (s->cwnd > acked) ? s->cwnd - acked : TCP_MSS;
                if (acked >= TCP_MSS) s->cwnd += TCP_MSS;
            }
        } else {
            s->dup_acks = 0;
            seg_ack(s, ack, acked);
        }
    } else if (ack == s->snd_una && dlen == 0 && s->unacked_head && wnd == old_wnd &&
               !(flags & (TCP_SYN | TCP_FIN))) {
        if (s->in_recovery) {
            if (s->in_recovery == 1) s->cwnd += TCP_MSS;   /* inflate per extra dup-ACK */
            tcp_retransmit_hole(s);
        } else if (++s->dup_acks == 3) {
            tcp_cc_on_loss(s);
            s->recover     = s->snd_nxt;      /* recovery point (NewReno) */
            s->in_recovery = 1;
            s->cwnd        = s->ssthresh + 3u * TCP_MSS;
            s->rtx_next    = s->unacked_head->seq;
            tcp_retransmit_one(s);
        }
    }
    s->snd_wnd = wnd;
    s->probes  = 0;
    if (!wnd) {
        if (s->unacked_head) s->retx_count = 0;   /* full, not gone: keeps retrying */
    } else {
        s->persist_deadline = 0;
        s->persist_n = 0;
        if (!old_wnd && s->unacked_head) {        /* no need to wait out a backed-off RTO */
            s->rtx_next = s->unacked_head->seq;
            tcp_retransmit_one(s);
            s->retx_deadline = kernel_ticks + s->rto;
        }
    }
    if (s->poll_out_blocked && tcp_can_send(s)) { s->poll_out_blocked = 0; poll_notify_key(s); }
    tcp_wake_connect(s);
}

static void tcp_enter_time_wait(socket_t *s)
{
    s->tcp_state = TCP_TIME_WAIT;
    s->linger_deadline = kernel_ticks + TCP_TIME_WAIT_TICKS;
    seg_clear(s);
}

/* The peer has acknowledged everything, our FIN included if one went out. */
static void tcp_fin_acked(socket_t *s)
{
    switch (s->tcp_state) {
    case TCP_FIN_WAIT1:
        seg_clear(s);
        s->tcp_state = TCP_FIN_WAIT2;
        if (s->orphan) s->linger_deadline = kernel_ticks + TCP_FIN_LINGER;
        break;
    case TCP_CLOSING:  tcp_enter_time_wait(s); break;
    case TCP_LAST_ACK: tcp_closed(s, 0);       break;
    default: break;
    }
}

/* In-sequence and future data. Everything is answered with an ACK, which also
 * tells the peer of a refusal through the window it carries. */
static void tcp_rx_data(socket_t *s, uint32_t seq, const uint8_t *data, uint16_t len)
{
    if (seq_lt(seq, s->rcv_nxt)) {      /* a retransmission may reach past rcv_nxt */
        uint32_t old = s->rcv_nxt - seq;
        data += old < len ? old : len;
        len  -= old < len ? (uint16_t)old : len;
        seq   = s->rcv_nxt;
    }
    uint32_t room = TCP_RCV_WND > s->rcv_buffered ? TCP_RCV_WND - s->rcv_buffered : 0;
    if (seq == s->rcv_nxt) {
        if (len > room) len = (uint16_t)room;
        uint16_t n = (uint16_t)sock_tcp_deliver(s, data, len);
        s->rcv_nxt += n;
        s->rcv_refused = n < len;
        if (!s->rcv_refused) tcp_ooo_drain(s);
    } else if (len && (seq - s->rcv_nxt) + len <= TCP_RCV_WND) {
        tcp_ooo_insert(s, seq, data, len);
    }
    tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
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
    uint32_t syn_mss  = (flags & TCP_SYN) ? tcp_opt_mss(th, hlen) : 0;
    uint8_t  rx_hlen  = hlen;   /* keep for SACK-block parsing after skb_pull */
    uint32_t rx_tsval = 0, rx_tsecr = 0;
    int      rx_has_ts = tcp_opt_ts(th, hlen, &rx_tsval, &rx_tsecr);
    int      v6        = (key->family == AF_INET6);

    skb_pull(skb, hlen);
    uint16_t dlen = skb->len;

    /* find socket */
find:;
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
            /* this runs on no task of the listener's: it pays for its connections */
            if (cgroup_charge_to(ls->cg, sizeof(*ns), 0) != 0) {
                sock_put(ns); skb_free(skb); sock_put(ls); return;
            }
            ns->cg = ls->cg;
            /* sock_alloc took the namespace of whichever task runs this RX path */
            net_ns_put(ns->net_ns);
            ns->net_ns      = net_ns_get(dev->net_ns);
            ns->dev         = dev;
            if (v6) { ns->local_ip6 = key->dst6; ns->remote_ip6 = key->src6; }
            else    { ns->local_ip  = key->dst4; ns->remote_ip  = src_ip;    }
            ns->local_port  = dst_port;
            ns->remote_port = src_port;
            if (!v6 && (ls->domain == AF_INET6 || ls->v4mapped)) ns->v4mapped = 1;
            ns->rcv_nxt     = seq + 1;
            ns->snd_nxt     = 0x12340000;
            ns->snd_una     = ns->snd_nxt;
            ns->tcp_state   = TCP_SYN_RECV;
            ns->snd_wnd     = ntohs(th->window);   /* SYN window is unscaled */
            if (syn_ws >= 0) { ns->snd_wscale = (uint8_t)syn_ws; ns->rcv_wscale = TCP_WSCALE; }
            if (syn_sack) ns->sack_ok = 1;
            if (rx_has_ts) { ns->ts_ok = 1; ns->ts_recent = rx_tsval; }
            tcp_set_mss(ns, syn_mss);
            tcp_cc_init(ns);
            ns->orphan = 1;      /* TCP's own until it reaches the accept queue */
            sock_register(ns);

            tcp_tx_seg(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, ns->rcv_nxt, NULL, 0);
            if (seg_enqueue(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, NULL, 0) != 0)
                ns->tcp_state = TCP_CLOSED;
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
        int syn = s->tcp_state == TCP_SYN_SENT || s->tcp_state == TCP_SYN_RECV;
        tcp_closed(s, syn ? 111 /* ECONNREFUSED */ : 104 /* ECONNRESET */);
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
            tcp_set_mss(s, syn_mss);
            s->tcp_state  = TCP_ESTABLISHED;
            tcp_cc_init(s);
            seg_ack(s, ack, 0);
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            tcp_wake_connect(s);
        }
        break;

    case TCP_SYN_RECV:
        /* A repeated SYN means our SYN-ACK never arrived: send it again. */
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcp_tx_seg(s, TCP_SYN | TCP_ACK, s->snd_nxt - 1, s->rcv_nxt, NULL, 0);
            break;
        }
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
                s->orphan = 0;
                uint64_t af = spin_lock_irq(&ls->recv_lock);
                s->accept_head = NULL; /* reuse field as list link */
                if (!ls->accept_head) ls->accept_head = s;
                else                  ls->accept_tail->accept_head = s;
                ls->accept_tail = s;
                process_t *aw = ls->accept_waiter;
                ls->accept_waiter = NULL;
                spin_unlock_irq(&ls->recv_lock, af);
                if (aw) task_wake(aw);
                poll_notify_key(ls);
            } else {                    /* the listener went away during the handshake */
                tcp_tx_seg(s, TCP_RST | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
                tcp_closed(s, 0);
                break;
            }
            sock_put(ls);
            /* The handshake ACK may carry the peer's first bytes. */
            if (dlen > 0 || (flags & TCP_FIN)) goto established;
        }
        break;

    case TCP_TIME_WAIT:
        /* a new connection on the tuple ends the wait for it */
        if ((flags & TCP_SYN) && !(flags & TCP_ACK) && s->orphan) {
            s->tcp_state = TCP_CLOSED;
            s->orphan = 0;
            spin_unlock_irq(&tcp_lock, tl);
            sock_free(s);
            sock_put(s);
            goto find;
        }
        /* fall through */
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_FIN_WAIT1:
    case TCP_FIN_WAIT2:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    established:
        if (s->ts_ok && rx_has_ts && seq == s->rcv_nxt) s->ts_recent = rx_tsval;
        if (flags & TCP_ACK) {
            tcp_ack_in(s, ack, th, rx_hlen, rx_has_ts, rx_tsecr, dlen, flags);
            if (ack == s->snd_nxt) tcp_fin_acked(s);
            if (s->tcp_state == TCP_CLOSED) break;
        }
        if (!tcp_can_rx(s)) {
            /* past the peer's FIN: anything more with a length is a retransmission */
            if (dlen || (flags & TCP_FIN) || seq_lt(seq, s->rcv_nxt))
                tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            break;
        }
        if (dlen) {
            /* nobody will read it; left unanswered the peer retransmits to no end */
            if (s->orphan && seq_lt(s->rcv_nxt, seq + dlen)) {
                tcp_tx_seg(s, TCP_RST | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
                tcp_closed(s, 0);
                break;
            }
            tcp_rx_data(s, seq, skb->data, dlen);
        } else if (seq_lt(seq, s->rcv_nxt)) {
            /* a window probe: only an answer tells the prober the window */
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        }
        /* FIN from remote: only in order, or it ends the stream before data it
         * follows; the peer sends it again */
        if ((flags & TCP_FIN) && seq + dlen == s->rcv_nxt) {
            s->rcv_nxt++;
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            if (s->tcp_state == TCP_ESTABLISHED)      s->tcp_state = TCP_CLOSE_WAIT;
            else if (s->tcp_state == TCP_FIN_WAIT1)   s->tcp_state = TCP_CLOSING;
            else                                      tcp_enter_time_wait(s);
            tcp_wake_recv(s);   /* a blocked recv sees EOF and returns 0 */
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
    s->rtx_next  = s->snd_una;
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
    if (s->tcp_state == TCP_SYN_SENT &&   /* still connecting — SYN-ACK may have raced in */
        seg_enqueue(s, TCP_SYN, s->snd_una, NULL, 0) != 0) {
        tcp_closed(s, 12);
        spin_unlock_irq(&tcp_lock, tl);
        return -ENOMEM;
    }
    spin_unlock_irq(&tcp_lock, tl);
    return 0;
}

int tcp_send(socket_t *s, const void *data, uint16_t len)
{
    uint64_t tl = spin_lock_irq(&tcp_lock);
    if (s->tcp_state != TCP_ESTABLISHED && s->tcp_state != TCP_CLOSE_WAIT) {
        int rc = -32;                   /* EPIPE: ours is shut, or it is over */
        if (s->tcp_state == TCP_SYN_SENT) rc = -11;
        else if (s->tcp_state == TCP_CLOSED) {
            if (s->so_error) { rc = -s->so_error; s->so_error = 0; }
            else if (!s->snd_nxt) rc = -107;   /* never connected */
        }
        spin_unlock_irq(&tcp_lock, tl);
        return rc;
    }

    /* effective window = min(congestion, peer-advertised); send while it has room */
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    uint32_t wnd = s->cwnd < s->snd_wnd ? s->cwnd : s->snd_wnd;
    if (in_flight >= wnd) {
        /* nothing out means no ACK and no RTO will ever reopen a closed window */
        if (!in_flight && !s->persist_deadline)
            s->persist_deadline = kernel_ticks + (s->rto ? s->rto : TCP_RTO_INIT);
        s->poll_out_blocked = 1;
        spin_unlock_irq(&tcp_lock, tl);
        return -EAGAIN;
    }

    uint32_t room = wnd - in_flight;
    uint32_t seglen = len > s->snd_mss ? s->snd_mss : len;
    if (seglen > room) seglen = room;
    if (seglen < len && seglen < s->snd_mss && in_flight) {   /* no tiny tails: an ACK is due */
        s->poll_out_blocked = 1;
        spin_unlock_irq(&tcp_lock, tl);
        return -EAGAIN;
    }

    if (seg_enqueue(s, TCP_ACK | TCP_PSH, s->snd_nxt, data, (uint16_t)seglen) != 0) {
        spin_unlock_irq(&tcp_lock, tl);
        return -ENOMEM;
    }
    /* queued now: a frame the device refused is a loss like any other */
    tcp_tx_seg(s, TCP_ACK | TCP_PSH, s->snd_nxt, s->rcv_nxt, data, (uint16_t)seglen);
    s->snd_nxt += seglen;
    spin_unlock_irq(&tcp_lock, tl);
    return (int)seglen;
}

static void tcp_send_fin(socket_t *s, int next_state)
{
    if (seg_enqueue(s, TCP_FIN | TCP_ACK, s->snd_nxt, NULL, 0) != 0) {
        tcp_tx_seg(s, TCP_RST | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        tcp_closed(s, 12);
        return;
    }
    tcp_tx_seg(s, TCP_FIN | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
    s->snd_nxt++;
    s->tcp_state = next_state;
}

int tcp_close(socket_t *s)
{
    uint64_t tl = spin_lock_irq(&tcp_lock);
    if (s->tcp_state == TCP_ESTABLISHED)       tcp_send_fin(s, TCP_FIN_WAIT1);
    else if (s->tcp_state == TCP_CLOSE_WAIT)   tcp_send_fin(s, TCP_LAST_ACK);
    else if (s->tcp_state == TCP_SYN_SENT)     tcp_closed(s, 0);
    spin_unlock_irq(&tcp_lock, tl);
    return 0;
}

/* The last file reference is gone, tcp_close has run. Returns 1 if the connection
 * is over and the caller frees the socket; otherwise tcp_tick owns it from here,
 * so unacked data and the FIN keep being retransmitted. */
int tcp_orphan(socket_t *s)
{
    uint64_t tl = spin_lock_irq(&tcp_lock);
    if (s->tcp_state == TCP_CLOSED) { spin_unlock_irq(&tcp_lock, tl); return 1; }
    s->orphan = 1;
    if (s->tcp_state == TCP_FIN_WAIT2 && !s->linger_deadline)
        s->linger_deadline = kernel_ticks + TCP_FIN_LINGER;
    spin_unlock_irq(&tcp_lock, tl);

    uint64_t f = spin_lock_irq(&s->recv_lock);
    skb_t *skb = s->recv_head;
    s->recv_head = s->recv_tail = NULL;
    s->rcv_buffered = 0;
    spin_unlock_irq(&s->recv_lock, f);
    while (skb) { skb_t *n = skb->next; skb_free(skb); skb = n; }
    return 0;
}

int tcp_listen(socket_t *s, int backlog)
{
    (void)backlog;
    if (!s->local_port) return -EINVAL;
    uint64_t tl = spin_lock_irq(&tcp_lock);
    s->tcp_state = TCP_LISTEN;
    spin_unlock_irq(&tcp_lock, tl);
    sock_register(s);
    return 0;
}

/* ── retransmit tick (called from sched_tick) ────────────── */

/* A zero window with nothing outstanding: nudge the peer with an old sequence
 * number, which it must answer with its current window. */
static void tcp_persist(socket_t *s)
{
    if (s->snd_wnd || s->unacked_head) { s->persist_deadline = 0; return; }
    if (++s->probes > TCP_RETX_GIVEUP) {
        tcp_tx_seg(s, TCP_RST | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        tcp_closed(s, 110);   /* ETIMEDOUT */
        return;
    }
    tcp_tx_seg(s, TCP_ACK, s->snd_nxt - 1, s->rcv_nxt, NULL, 0);
    uint32_t t = (s->rto ? s->rto : TCP_RTO_INIT) << s->persist_n;
    if (t >= TCP_RTO_MAX) t = TCP_RTO_MAX;
    else s->persist_n++;
    s->persist_deadline = kernel_ticks + t;
}

static int tcp_timer_due(const socket_t *s, uint64_t now)
{
    return (s->orphan && s->tcp_state == TCP_CLOSED) ||
           (s->retx_deadline && now >= s->retx_deadline) ||
           (s->persist_deadline && now >= s->persist_deadline) ||
           (s->linger_deadline && now >= s->linger_deadline);
}

void tcp_tick(void)
{
    /* walks every socket: 100 times a second is plenty */
    if (kernel_ticks % (TIMER_HZ / 100))
        return;
    for (int i = 0; i < gsock_cap; i++) {
        /* Read + pin the slot under gsock_lock: a concurrent close runs
         * sock_free under it, and gsock_ensure may realloc the array, so the
         * bare gsock[i] read + deref raced both. */
        uint64_t gf = spin_lock_irq(&gsock_lock);
        socket_t *s = (i < gsock_cap) ? gsock[i] : NULL;
        if (s && (s->type != SOCK_STREAM || !tcp_timer_due(s, kernel_ticks)))
            s = NULL;
        if (s) sock_hold(s);
        spin_unlock_irq(&gsock_lock, gf);
        if (!s) continue;

        uint64_t tl = spin_lock_irq(&tcp_lock);   /* serialise vs RX + syscalls */
        if (s->orphan && s->tcp_state == TCP_CLOSED) {
            s->orphan = 0;
            spin_unlock_irq(&tcp_lock, tl);
            sock_free(s);
            sock_put(s);
            continue;
        }
        if (s->linger_deadline && kernel_ticks >= s->linger_deadline)
            tcp_closed(s, 0);
        if (s->persist_deadline && kernel_ticks >= s->persist_deadline)
            tcp_persist(s);
        if (!s->retx_deadline || kernel_ticks < s->retx_deadline) {
            spin_unlock_irq(&tcp_lock, tl);
            sock_put(s);
            continue;
        }
        tcp_seg_t *g = s->unacked_head;
        if (!g) { s->retx_deadline = 0; spin_unlock_irq(&tcp_lock, tl); sock_put(s); continue; }

        s->retx_count++;
        if (s->retx_count > TCP_RETX_GIVEUP) {
            /* the peer may still be there, waiting: say that this is the end */
            if (s->tcp_state != TCP_SYN_SENT)
                tcp_tx_seg(s, TCP_RST | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            tcp_closed(s, 110);   /* ETIMEDOUT */
            spin_unlock_irq(&tcp_lock, tl);
            sock_put(s);
            continue;
        }

        /* timeout: drop ssthresh per the active algorithm, collapse cwnd to slow
         * start, abandon any fast recovery, retransmit the oldest non-SACKed
         * segment, and back off the timer (RFC 6298 §5.5). */
        if (s->retx_count == 1) tcp_cc_on_loss(s);
        s->cwnd        = TCP_MSS;
        s->dup_acks    = 0;
        s->in_recovery = 2;
        s->recover     = s->snd_nxt;
        s->cubic_epoch = 0;
        s->rtx_next    = g->seq;
        tcp_retransmit_one(s);
        s->rto = (s->rto ? s->rto : TCP_RTO_INIT) * 2;
        if (s->rto > TCP_RTO_MAX) s->rto = TCP_RTO_MAX;
        s->retx_deadline = kernel_ticks + s->rto;
        spin_unlock_irq(&tcp_lock, tl);
        sock_put(s);
    }
}
