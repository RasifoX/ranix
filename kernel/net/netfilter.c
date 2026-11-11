/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/net/netfilter.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/tcp.h>
#include <kernel/net/socket.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <kernel/errno.h>
#include <kernel/board.h>
#include <kernel/time.h>
#include <kernel/cmdline.h>
#include <kernel/kprintf.h>

/* First-match-wins filter. One rule array per direction; a rule is a set of
 * optional match fields plus a verdict. A packet that matches no rule gets the
 * chain's policy, ACCEPT at boot. v4 and v6 share the table: addr_len tells
 * them apart and an addr-less rule matches either family.
 *
 * "established" is a socket lookup, not a connection table: a TCP segment
 * belongs to a connection when a non-listening socket has its 4-tuple (the
 * same lookup tcp_input does next). UDP has no connections, so the replies to
 * what this machine sent are remembered in a small table of flows (remote
 * address and port, local port) for NF_UDP_IDLE after the last packet out.
 * ICMP errors and echo replies count as established (related), echo requests
 * and everything else do not. */

#define NF_MAX_RULES 32

#define NF_M_PROTO 0x001
#define NF_M_SRC   0x002
#define NF_M_DST   0x004
#define NF_M_SPORT 0x008
#define NF_M_DPORT 0x010
#define NF_M_LO    0x020   /* came back from this machine */
#define NF_M_EST   0x040   /* belongs to a connection or flow this machine has */
#define NF_M_TYPE  0x080   /* ICMP type in [type_lo, type_hi] */

typedef struct {
    uint16_t match;        /* NF_M_* bitmap of which fields are constrained */
    uint8_t  verdict;      /* NF_ACCEPT / NF_DROP */
    uint8_t  proto;
    uint8_t  addr_len;     /* 4 or 16 when src/dst constrained, else 0 */
    uint8_t  type_lo, type_hi;
    uint8_t  src[16], dst[16];
    uint16_t sport, dport;
} nf_rule_t;

static nf_rule_t  nf_in_rules[NF_MAX_RULES];
static nf_rule_t  nf_out_rules[NF_MAX_RULES];
static int        nf_in_n, nf_out_n;
static int        nf_policy[2];          /* NF_ACCEPT / NF_DROP per chain */
static int        nf_in_est;             /* IN rules that need the lookup */
static uint64_t   nf_dropped[2];         /* by the policy */
static spinlock_t nf_lock = SPINLOCK_INIT;

int nf_active;   /* rules or a dropping policy, read locklessly by the hooks */

static void nf_recount(void)
{
    nf_active = nf_in_n + nf_out_n + (nf_policy[NF_IN] == NF_DROP) + (nf_policy[NF_OUT] == NF_DROP);
    nf_in_est = 0;
    for (int i = 0; i < nf_in_n; i++)
        if (nf_in_rules[i].match & NF_M_EST) nf_in_est++;
}

/* ranix.firewall=off: no filtering this boot, every command but flush refused.
 * Read once, on the first command (the command line is there long before). */
static int nf_off = -1;

static int nf_is_off(void)
{
    if (nf_off < 0) {
        char b[8];
        nf_off = cmdline_get("ranix.firewall", b, sizeof(b)) && !strcmp(b, "off");
        if (nf_off)
            kprintf(KERN_WARN "netfilter: ranix.firewall=off: no filtering this boot\n");
    }
    return nf_off;
}

/* ── UDP flows this machine started ────────────────────────── */

#define NF_FLOWS     256
#define NF_FLOW_PROBE 8
#define NF_UDP_IDLE  (180ull * TIMER_HZ)

typedef struct {
    uint64_t until;        /* kernel_ticks; 0 = free */
    uint8_t  alen;
    uint8_t  raddr[16];
    uint16_t rport, lport;
} nf_flow_t;

static nf_flow_t  nf_flows[NF_FLOWS];
static spinlock_t nf_flow_lock = SPINLOCK_INIT;

static uint32_t flow_hash(int alen, const uint8_t *a, uint16_t rport, uint16_t lport)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < alen; i++) h = (h ^ a[i]) * 16777619u;
    h = (h ^ rport) * 16777619u;
    h = (h ^ lport) * 16777619u;
    return h;
}

/* the remote end of p in network byte order */
static void pkt_addr(const nf_pkt_t *p, int remote_is_src, uint8_t out[16])
{
    if (p->addr_len == 4) {
        uint32_t v = remote_is_src ? p->src4 : p->dst4;
        out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v;
    } else {
        memcpy(out, remote_is_src ? p->src6 : p->dst6, 16);
    }
}

static void flow_note(const nf_pkt_t *p)       /* p goes out */
{
    uint8_t a[16];
    pkt_addr(p, 0, a);
    uint32_t h = flow_hash(p->addr_len, a, p->dport, p->sport);
    uint64_t now = kernel_ticks;
    uint64_t f = spin_lock_irq(&nf_flow_lock);
    nf_flow_t *pick = NULL;
    for (int i = 0; i < NF_FLOW_PROBE; i++) {
        nf_flow_t *e = &nf_flows[(h + (uint32_t)i) % NF_FLOWS];
        if (e->until > now && e->alen == p->addr_len && e->rport == p->dport &&
            e->lport == p->sport && !memcmp(e->raddr, a, (size_t)p->addr_len)) { pick = e; break; }
        if (!pick || e->until < pick->until) pick = e;   /* free, stale, or the oldest */
    }
    pick->until = now + NF_UDP_IDLE;
    pick->alen = (uint8_t)p->addr_len;
    memcpy(pick->raddr, a, (size_t)p->addr_len);
    pick->rport = p->dport;
    pick->lport = p->sport;
    spin_unlock_irq(&nf_flow_lock, f);
}

static int flow_has(const nf_pkt_t *p)          /* p comes in */
{
    uint8_t a[16];
    pkt_addr(p, 1, a);
    uint32_t h = flow_hash(p->addr_len, a, p->sport, p->dport);
    uint64_t now = kernel_ticks;
    int hit = 0;
    uint64_t f = spin_lock_irq(&nf_flow_lock);
    for (int i = 0; i < NF_FLOW_PROBE && !hit; i++) {
        nf_flow_t *e = &nf_flows[(h + (uint32_t)i) % NF_FLOWS];
        hit = e->until > now && e->alen == p->addr_len && e->rport == p->sport &&
              e->lport == p->dport && !memcmp(e->raddr, a, (size_t)p->addr_len);
    }
    spin_unlock_irq(&nf_flow_lock, f);
    return hit;
}

/* Does an incoming packet belong to something this machine already has? */
static int nf_established(const nf_pkt_t *p)
{
    static const uint8_t zero[16];
    if (p->addr_len == 4 ? !p->src4 : !memcmp(p->src6, zero, 16))
        return 0;                                   /* nobody answers from 0.0.0.0 or :: */
    switch (p->proto) {
    case 6: {
        if (!p->dev || !p->sport) return 0;
        socket_t *s = p->addr_len == 4
            ? tcp_sock_find(p->dev->net_ns, p->dst4, p->dport, p->src4, p->sport)
            : tcp_sock_find6(p->dev->net_ns, p->dport, (const in6_addr_t *)p->src6, p->sport);
        if (!s) return 0;
        sock_put(s);
        return 1;
    }
    case 17:
        return p->sport && flow_has(p);
    case 1:     /* echo reply, unreachable, time exceeded, parameter problem */
        return p->icmp_type == 0 || p->icmp_type == 3 || p->icmp_type == 11 || p->icmp_type == 12;
    case 58:    /* the four errors, echo reply */
        return (p->icmp_type >= 1 && p->icmp_type <= 4) || p->icmp_type == 129;
    }
    return 0;
}

static int rule_hits(const nf_rule_t *r, const nf_pkt_t *p, int est)
{
    if ((r->match & NF_M_PROTO) && r->proto != p->proto) return 0;
    if (r->match & (NF_M_SRC | NF_M_DST)) {
        if (r->addr_len != p->addr_len) return 0;
        uint8_t a[16];
        if (r->match & NF_M_SRC) { pkt_addr(p, 1, a); if (memcmp(r->src, a, (size_t)p->addr_len)) return 0; }
        if (r->match & NF_M_DST) { pkt_addr(p, 0, a); if (memcmp(r->dst, a, (size_t)p->addr_len)) return 0; }
    }
    if ((r->match & NF_M_SPORT) && r->sport != p->sport) return 0;
    if ((r->match & NF_M_DPORT) && r->dport != p->dport) return 0;
    if ((r->match & NF_M_TYPE) && (p->icmp_type < r->type_lo || p->icmp_type > r->type_hi)) return 0;
    if ((r->match & NF_M_LO) && !p->looped) return 0;
    if ((r->match & NF_M_EST) && !est) return 0;
    return 1;
}

int nf_filter(const nf_pkt_t *p)
{
    int dir = p->dir == NF_OUT ? NF_OUT : NF_IN;
    /* the lookup takes the socket table's lock: done before ours */
    int est = dir == NF_IN && nf_in_est ? nf_established(p) : 0;
    uint64_t f = spin_lock_irq(&nf_lock);
    const nf_rule_t *rules = dir == NF_OUT ? nf_out_rules : nf_in_rules;
    int n = dir == NF_OUT ? nf_out_n : nf_in_n;
    int verdict = -1;
    for (int i = 0; i < n; i++)
        if (rule_hits(&rules[i], p, est)) {
            verdict = rules[i].verdict;
            break;
        }
    if (verdict < 0) {
        verdict = nf_policy[dir];
        if (verdict == NF_DROP) nf_dropped[dir]++;
    }
    spin_unlock_irq(&nf_lock, f);
    if (verdict == NF_ACCEPT && dir == NF_OUT && p->proto == 17)
        flow_note(p);
    return verdict;
}

/* ── parser ──────────────────────────────────────────────────
 * Grammar: [in|out] VERDICT PROTO [src ADDR] [dst ADDR] [sport N] [dport N]
 *                                  [type N[-M]] [lo] [established]
 *   VERDICT = accept | drop
 *   PROTO   = tcp | udp | icmp | tcp6 | udp6 | icmpv6 | any
 *   type    = ICMP/ICMPv6 type or range (icmp and icmpv6 rules only)
 *   lo      = the packet came back from this machine (loopback, own address)
 *   established = it belongs to a connection or flow this machine has
 * Plus "policy in|out accept|drop" and "flush" (no rules, both policies
 * accept: the boot state). Whitespace-separated, one command per call. */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char *tok(const char *p, const char *end, char *out, size_t cap)
{
    p = skip_ws(p, end);
    size_t n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n') {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

static int parse_u16(const char *s, uint16_t *out)
{
    uint32_t v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 0xffff) return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int parse_v4(const char *s, uint8_t out[4])
{
    uint32_t v = 0;
    int octet = 0, ndig = 0, parts = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (*s - '0');
            if (octet > 255 || ++ndig > 3) return -1;
        } else if (*s == '.' || *s == '\0') {
            if (!ndig) return -1;
            v = (v << 8) | (uint32_t)octet;
            octet = 0; ndig = 0;
            if (++parts == 4) { if (*s) return -1; break; }
            if (*s == '\0') return -1;
        } else return -1;
    }
    out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v;
    return 0;
}

/* minimal IPv6 literal parser: hex groups with one "::" run, no embedded v4 */
static int parse_v6(const char *s, uint8_t out[16])
{
    uint16_t groups[8];
    int n = 0, gap = -1;
    memset(out, 0, 16);
    if (s[0] == ':' && s[1] == ':') { gap = 0; s += 2; if (!*s) return 0; }
    for (;;) {
        uint32_t g = 0; int ndig = 0;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
               (*s >= 'A' && *s <= 'F')) {
            int d = *s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10;
            g = (g << 4) | (uint32_t)d;
            if (++ndig > 4) return -1;
            s++;
        }
        if (!ndig) return -1;
        if (n >= 8) return -1;
        groups[n++] = (uint16_t)g;
        if (*s == '\0') break;
        if (*s != ':') return -1;
        s++;
        if (*s == ':') { if (gap >= 0) return -1; gap = n; s++; if (!*s) break; }
    }
    if (gap < 0) {
        if (n != 8) return -1;
        for (int i = 0; i < 8; i++) { out[i*2] = groups[i] >> 8; out[i*2+1] = groups[i]; }
    } else {
        if (n >= 8) return -1;
        int tail = n - gap;
        for (int i = 0; i < gap; i++) { out[i*2] = groups[i] >> 8; out[i*2+1] = groups[i]; }
        for (int i = 0; i < tail; i++) {
            uint16_t v = groups[gap + i];
            out[(8 - tail + i)*2] = v >> 8; out[(8 - tail + i)*2+1] = v;
        }
    }
    return 0;
}

static int parse_proto(const char *t, nf_rule_t *r)
{
    if (!strcmp(t, "any")) return 0;
    r->match |= NF_M_PROTO;
    if (!strcmp(t, "tcp")  || !strcmp(t, "tcp6"))   { r->proto = 6;  return 0; }
    if (!strcmp(t, "udp")  || !strcmp(t, "udp6"))   { r->proto = 17; return 0; }
    if (!strcmp(t, "icmp"))                          { r->proto = 1;  return 0; }
    if (!strcmp(t, "icmpv6") || !strcmp(t, "icmp6")) { r->proto = 58; return 0; }
    return -1;
}

static int parse_addr(const char *t, nf_rule_t *r, uint8_t out[16])
{
    if (parse_v4(t, out) == 0) {
        if (r->addr_len && r->addr_len != 4) return -1;
        r->addr_len = 4;
        return 0;
    }
    if (parse_v6(t, out) == 0) {
        if (r->addr_len && r->addr_len != 16) return -1;
        r->addr_len = 16;
        return 0;
    }
    return -1;
}

static int parse_type(const char *s, nf_rule_t *r);

static int parse_rule(const char *p, const char *end, nf_rule_t *r, int *dir)
{
    char t[48];
    memset(r, 0, sizeof(*r));
    *dir = NF_IN;

    p = tok(p, end, t, sizeof(t));
    if (!strcmp(t, "out")) { *dir = NF_OUT; p = tok(p, end, t, sizeof(t)); }
    else if (!strcmp(t, "in")) { p = tok(p, end, t, sizeof(t)); }

    if (!strcmp(t, "accept")) r->verdict = NF_ACCEPT;
    else if (!strcmp(t, "drop")) r->verdict = NF_DROP;
    else return -1;

    p = tok(p, end, t, sizeof(t));
    if (parse_proto(t, r) != 0) return -1;

    for (;;) {
        p = tok(p, end, t, sizeof(t));
        if (!t[0]) break;
        if (!strcmp(t, "lo"))          { r->match |= NF_M_LO;  continue; }
        if (!strcmp(t, "established")) { r->match |= NF_M_EST; continue; }
        char v[48];
        p = tok(p, end, v, sizeof(v));
        if (!v[0]) return -1;
        if (!strcmp(t, "src"))        { if (parse_addr(v, r, r->src)) return -1; r->match |= NF_M_SRC; }
        else if (!strcmp(t, "dst"))   { if (parse_addr(v, r, r->dst)) return -1; r->match |= NF_M_DST; }
        else if (!strcmp(t, "sport")) { if (parse_u16(v, &r->sport)) return -1; r->match |= NF_M_SPORT; }
        else if (!strcmp(t, "dport")) { if (parse_u16(v, &r->dport)) return -1; r->match |= NF_M_DPORT; }
        else if (!strcmp(t, "type"))  { if (parse_type(v, r)) return -1; r->match |= NF_M_TYPE; }
        else return -1;
    }
    if ((r->match & NF_M_TYPE) && !((r->match & NF_M_PROTO) && (r->proto == 1 || r->proto == 58)))
        return -1;     /* a type is ICMP's */
    return 0;
}

/* "N" or "N-M", each 0..255 */
static int parse_type(const char *s, nf_rule_t *r)
{
    char a[8], b[8];
    const char *dash = strchr(s, '-');
    size_t la = dash ? (size_t)(dash - s) : strlen(s);
    if (!la || la >= sizeof(a)) return -1;
    memcpy(a, s, la); a[la] = 0;
    const char *bs = dash ? dash + 1 : a;
    if (strlen(bs) >= sizeof(b)) return -1;
    strcpy(b, bs);
    uint16_t lo, hi;
    if (parse_u16(a, &lo) || parse_u16(b, &hi) || lo > 255 || hi > 255 || lo > hi) return -1;
    r->type_lo = (uint8_t)lo;
    r->type_hi = (uint8_t)hi;
    return 0;
}

int nf_ctl_set(const char *line, size_t len)
{
    const char *end = line + len;
    char first[16];
    const char *p = tok(line, end, first, sizeof(first));
    if (!strcmp(first, "flush")) {
        uint64_t f = spin_lock_irq(&nf_lock);
        nf_in_n = nf_out_n = 0;
        nf_policy[NF_IN] = nf_policy[NF_OUT] = NF_ACCEPT;
        nf_recount();
        spin_unlock_irq(&nf_lock, f);
        return 0;
    }
    if (nf_is_off()) return -ECANCELED;

    if (!strcmp(first, "policy")) {
        char c[8], v[8], x[4];
        p = tok(p, end, c, sizeof(c));
        p = tok(p, end, v, sizeof(v));
        tok(p, end, x, sizeof(x));
        int dir = !strcmp(c, "in") ? NF_IN : !strcmp(c, "out") ? NF_OUT : -1;
        int pol = !strcmp(v, "accept") ? NF_ACCEPT : !strcmp(v, "drop") ? NF_DROP : -1;
        if (dir < 0 || pol < 0 || x[0]) return -EINVAL;
        uint64_t f = spin_lock_irq(&nf_lock);
        nf_policy[dir] = pol;
        nf_recount();
        spin_unlock_irq(&nf_lock, f);
        return 0;
    }

    nf_rule_t r; int dir;
    if (parse_rule(line, end, &r, &dir) != 0) return -EINVAL;

    uint64_t f = spin_lock_irq(&nf_lock);
    int *n = dir == NF_OUT ? &nf_out_n : &nf_in_n;
    nf_rule_t *tbl = dir == NF_OUT ? nf_out_rules : nf_in_rules;
    if (*n >= NF_MAX_RULES) { spin_unlock_irq(&nf_lock, f); return -ENOSPC; }
    tbl[*n] = r;
    (*n)++;
    nf_recount();
    spin_unlock_irq(&nf_lock, f);
    return 0;
}

/* ── dump ──────────────────────────────────────────────────── */

typedef struct { char *p; size_t rem; } dbuf_t;

static void d_str(dbuf_t *b, const char *s)
{
    while (*s && b->rem > 1) { *b->p++ = *s++; b->rem--; }
}

static void d_u(dbuf_t *b, uint32_t v)
{
    char tmp[12]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + v % 10; v /= 10; }
    while (n && b->rem > 1) { *b->p++ = tmp[--n]; b->rem--; }
}

static void d_u64(dbuf_t *b, uint64_t v)
{
    char tmp[21]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && b->rem > 1) { *b->p++ = tmp[--n]; b->rem--; }
}

static void d_addr(dbuf_t *b, int len, const uint8_t *a)
{
    if (len == 4) {
        for (int i = 0; i < 4; i++) { if (i) d_str(b, "."); d_u(b, a[i]); }
        return;
    }
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i += 2) {
        if (i) d_str(b, ":");
        uint16_t g = ((uint16_t)a[i] << 8) | a[i+1];
        char t[5]; int n = 0;
        t[n++] = hx[(g >> 12) & 0xf]; t[n++] = hx[(g >> 8) & 0xf];
        t[n++] = hx[(g >> 4) & 0xf];  t[n++] = hx[g & 0xf];
        for (int j = 0; j < n; j++) if (b->rem > 1) { *b->p++ = t[j]; b->rem--; }
    }
}

static const char *proto_name(uint8_t p)
{
    switch (p) {
    case 1:  return "icmp";
    case 6:  return "tcp";
    case 17: return "udp";
    case 58: return "icmpv6";
    default: return "ip";
    }
}

static void dump_chain(dbuf_t *b, const char *dir, const nf_rule_t *tbl, int n)
{
    for (int i = 0; i < n; i++) {
        const nf_rule_t *r = &tbl[i];
        d_str(b, dir); d_str(b, " ");
        d_str(b, r->verdict == NF_DROP ? "drop " : "accept ");
        d_str(b, (r->match & NF_M_PROTO) ? proto_name(r->proto) : "any");
        if (r->match & NF_M_SRC)   { d_str(b, " src ");   d_addr(b, r->addr_len, r->src); }
        if (r->match & NF_M_DST)   { d_str(b, " dst ");   d_addr(b, r->addr_len, r->dst); }
        if (r->match & NF_M_SPORT) { d_str(b, " sport "); d_u(b, r->sport); }
        if (r->match & NF_M_DPORT) { d_str(b, " dport "); d_u(b, r->dport); }
        if (r->match & NF_M_TYPE) {
            d_str(b, " type "); d_u(b, r->type_lo);
            if (r->type_hi != r->type_lo) { d_str(b, "-"); d_u(b, r->type_hi); }
        }
        if (r->match & NF_M_LO)    d_str(b, " lo");
        if (r->match & NF_M_EST)   d_str(b, " established");
        d_str(b, "\n");
    }
}

int nf_ctl_dump(char *buf, size_t cap)
{
    dbuf_t b = { buf, cap };
    if (nf_is_off()) d_str(&b, "off ranix.firewall=off\n");
    uint64_t f = spin_lock_irq(&nf_lock);
    d_str(&b, nf_policy[NF_IN] == NF_DROP ? "policy in drop\n" : "policy in accept\n");
    d_str(&b, nf_policy[NF_OUT] == NF_DROP ? "policy out drop\n" : "policy out accept\n");
    dump_chain(&b, "in", nf_in_rules, nf_in_n);
    dump_chain(&b, "out", nf_out_rules, nf_out_n);
    uint64_t din = nf_dropped[NF_IN], dout = nf_dropped[NF_OUT];
    spin_unlock_irq(&nf_lock, f);
    d_str(&b, "dropped in "); d_u64(&b, din);
    d_str(&b, " out "); d_u64(&b, dout); d_str(&b, "\n");
    if (b.rem) *b.p = '\0';
    return (int)(cap - b.rem);
}
