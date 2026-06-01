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

#define TCP_RETX_TICKS  200   /* ~2s at 100Hz */
#define TCP_RCV_WND     8192

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

    tcp_hdr_t *th = (tcp_hdr_t *)skb_push(skb, sizeof(tcp_hdr_t));
    memset(th, 0, sizeof(tcp_hdr_t));
    th->src_port = htons(s->local_port);
    th->dst_port = htons(s->remote_port);
    th->seq      = htonl(seq);
    th->ack      = (flags & TCP_ACK) ? htonl(ack) : 0;
    th->data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    th->flags    = flags;
    th->window   = htons(TCP_RCV_WND);
    th->checksum = 0;
    th->checksum = tcp_cksum(s->local_ip ? s->local_ip : s->dev->ip,
                             s->remote_ip, th, skb->len);

    return ip_tx(s->dev, skb, s->remote_ip, IP_PROTO_TCP);
}

/* ── retransmit queue (single segment) ──────────────────── */

static void retx_arm(socket_t *s, uint8_t flags,
                     uint32_t seq, const void *data, uint16_t dlen)
{
    if (s->retx_data) { kfree(s->retx_data); s->retx_data = NULL; }
    if (dlen) {
        s->retx_data = kmalloc(dlen);
        if (s->retx_data) memcpy(s->retx_data, data, dlen);
    }
    s->retx_len      = dlen;
    s->retx_seq      = seq;
    s->retx_flags    = flags;
    s->retx_deadline = kernel_ticks + TCP_RETX_TICKS;
}

static void retx_clear(socket_t *s)
{
    if (s->retx_data) { kfree(s->retx_data); s->retx_data = NULL; }
    s->retx_len      = 0;
    s->retx_deadline = 0;
    s->retx_count    = 0;
}

static void tcp_wake_close(socket_t *s)
{
    process_t *w = s->close_waiter;
    s->close_waiter = NULL;
    if (w) task_wake(w);
}

/* ── port allocation ─────────────────────────────────────── */

static uint16_t   next_ephemeral  = 49152;
static spinlock_t ephemeral_lock  = SPINLOCK_INIT;

static uint16_t alloc_port(void)
{
    spin_lock(&ephemeral_lock);
    uint16_t p = next_ephemeral++;
    if (next_ephemeral == 0) next_ephemeral = 49152;
    spin_unlock(&ephemeral_lock);
    return p;
}

/* ── socket lookup ───────────────────────────────────────── */

socket_t *tcp_sock_find(uint32_t local_ip, uint16_t local_port,
                        uint32_t remote_ip, uint16_t remote_port)
{
    
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
        if (s->type != SOCK_STREAM) continue;
        if (s->local_port  != local_port)  continue;
        if (s->remote_port != remote_port) continue;
        if (s->remote_ip   != remote_ip)   continue;
        (void)local_ip;
        return s;
    }
    return NULL;
}

socket_t *tcp_listen_find(uint16_t local_port)
{
    
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *s = gsock[i];
        if (!s) continue;
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

    skb_pull(skb, hlen);
    uint16_t dlen = skb->len;

    /* find socket */
    socket_t *s = tcp_sock_find(ntohl(iph->dst), dst_port, src_ip, src_port);

    if (!s) {
        /* check for listen socket */
        socket_t *ls = tcp_listen_find(dst_port);
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
            sock_register(ns);

            tcp_tx_seg(ns, TCP_SYN | TCP_ACK,
                       ns->snd_nxt, ns->rcv_nxt, NULL, 0);
            retx_arm(ns, TCP_SYN | TCP_ACK, ns->snd_nxt, NULL, 0);
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
        retx_clear(s);
        process_t *w = s->connect_waiter; s->connect_waiter = NULL;
        if (w) task_wake(w);
        uint64_t lf = spin_lock_irq(&s->recv_lock);
        w = s->recv_waiter; s->recv_waiter = NULL;
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
            s->snd_wnd    = ntohs(th->window);
            s->tcp_state  = TCP_ESTABLISHED;
            retx_clear(s);
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
            process_t *w = s->connect_waiter; s->connect_waiter = NULL;
            if (w) task_wake(w);
        }
        break;

    case TCP_SYN_RECV:
        /* expecting ACK of SYN-ACK */
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            s->snd_una   = ack;
            s->tcp_state = TCP_ESTABLISHED;
            retx_clear(s);
            /* move to listen socket's accept queue */
            socket_t *ls = tcp_listen_find(s->local_port);
            if (ls) {
                s->accept_head = NULL; /* reuse field for list link */
                /* append to ls->accept_head list using accept_tail chain */
                if (!ls->accept_head) ls->accept_head = s;
                else                  ls->accept_tail->accept_head = s;
                ls->accept_tail = s;
                process_t *w = ls->accept_waiter; ls->accept_waiter = NULL;
                if (w) task_wake(w);
            }
        }
        break;

    case TCP_ESTABLISHED:
        /* ACK advances snd_una */
        if (flags & TCP_ACK) {
            if (ack > s->snd_una) {
                s->snd_una = ack;
                if (s->snd_una == s->snd_nxt) retx_clear(s);
            }
            s->snd_wnd = ntohs(th->window);
            /* wake a blocked send() on any ACK (window or snd_una advance) */
            process_t *w = s->connect_waiter;
            s->connect_waiter = NULL;
            if (w) task_wake(w);
        }
        /* deliver data */
        if (dlen > 0 && seq == s->rcv_nxt) {
            s->rcv_nxt += dlen;
            sock_tcp_deliver(s, skb->data, dlen);
            /* send ACK */
            tcp_tx_seg(s, TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        } else if (dlen > 0) {
            /* out-of-order: just re-ACK what we have */
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
        if ((flags & TCP_ACK) && ack > s->snd_una) {
            s->snd_una = ack;
            if (s->snd_una == s->snd_nxt) retx_clear(s);
            s->snd_wnd = ntohs(th->window);
            process_t *w = s->connect_waiter;
            s->connect_waiter = NULL;
            if (w) task_wake(w);
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            s->tcp_state = TCP_CLOSED;
            retx_clear(s);
            tcp_wake_close(s);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) {
            if (ack == s->snd_nxt) {
                retx_clear(s);
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
            retx_clear(s);
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
    retx_arm(s, TCP_SYN, s->snd_nxt, NULL, 0);
    s->snd_nxt++;
    return 0;
}

int tcp_send(socket_t *s, const void *data, uint16_t len)
{
    if (s->tcp_state != TCP_ESTABLISHED) return -107; /* ENOTCONN */

    /* clamp to MSS */
    uint16_t mss = (uint16_t)(SKB_DATA_MAX - sizeof(tcp_hdr_t) - sizeof(ip_hdr_t) - 14);
    if (len > mss) len = mss;

    /* flow control: block caller if previous segment unACKed or window exhausted */
    uint32_t in_flight = s->snd_nxt - s->snd_una;
    if (s->retx_data || in_flight >= (uint32_t)s->snd_wnd)
        return -11; /* EAGAIN — caller must block and retry */

    int rc = tcp_tx_seg(s, TCP_ACK | TCP_PSH, s->snd_nxt, s->rcv_nxt, data, len);
    if (rc < 0) return rc;
    retx_arm(s, TCP_ACK | TCP_PSH, s->snd_nxt, data, len);
    s->snd_nxt += len;
    return (int)len;
}

int tcp_close(socket_t *s)
{
    if (s->tcp_state == TCP_ESTABLISHED) {
        tcp_tx_seg(s, TCP_FIN | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        retx_arm(s, TCP_FIN | TCP_ACK, s->snd_nxt, NULL, 0);
        s->snd_nxt++;
        s->tcp_state = TCP_FIN_WAIT1;
    } else if (s->tcp_state == TCP_CLOSE_WAIT) {
        tcp_tx_seg(s, TCP_FIN | TCP_ACK, s->snd_nxt, s->rcv_nxt, NULL, 0);
        retx_arm(s, TCP_FIN | TCP_ACK, s->snd_nxt, NULL, 0);
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
        if (s->retx_deadline == 0) continue;         /* no pending retransmit */
        if (kernel_ticks < s->retx_deadline) continue;

        s->retx_count++;
        if (s->retx_count > 5) {
            /* give up — force close and wake anyone waiting */
            s->tcp_state = TCP_CLOSED;
            retx_clear(s);
            tcp_wake_close(s);
            uint64_t lf = spin_lock_irq(&s->recv_lock);
            process_t *w = s->recv_waiter; s->recv_waiter = NULL;
            spin_unlock_irq(&s->recv_lock, lf);
            if (w) task_wake(w);
            continue;
        }
        /* retransmit */
        tcp_tx_seg(s, s->retx_flags, s->retx_seq, s->rcv_nxt,
                   s->retx_data, s->retx_len);
        s->retx_deadline = kernel_ticks + TCP_RETX_TICKS;
    }
}
