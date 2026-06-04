#include <kernel/net/netfilter.h>
#include <kernel/net/ipv6.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>

/* Stateless first-match-wins filter. One rule array per direction; a rule is a
 * set of optional match fields plus a verdict. A packet that matches no rule is
 * accepted (default-accept policy). v4 and v6 share the table — addr_len tells
 * them apart and an addr-less rule matches either family. */

#define NF_MAX_RULES 32

#define NF_M_PROTO 0x01
#define NF_M_SRC   0x02
#define NF_M_DST   0x04
#define NF_M_SPORT 0x08
#define NF_M_DPORT 0x10

typedef struct {
    uint8_t  match;        /* NF_M_* bitmap of which fields are constrained */
    uint8_t  verdict;      /* NF_ACCEPT / NF_DROP */
    uint8_t  proto;
    uint8_t  addr_len;     /* 4 or 16 when src/dst constrained, else 0 */
    uint8_t  src[16], dst[16];
    uint16_t sport, dport;
} nf_rule_t;

static nf_rule_t  nf_in_rules[NF_MAX_RULES];
static nf_rule_t  nf_out_rules[NF_MAX_RULES];
static int        nf_in_n, nf_out_n;
static spinlock_t nf_lock = SPINLOCK_INIT;

int nf_rules_active;   /* nf_in_n + nf_out_n, read locklessly by the hooks */

static int rule_hits(const nf_rule_t *r, int addr_len, const void *src,
                     const void *dst, uint8_t proto, uint16_t sport, uint16_t dport)
{
    if ((r->match & NF_M_PROTO) && r->proto != proto) return 0;
    if (r->match & (NF_M_SRC | NF_M_DST)) {
        if (r->addr_len != addr_len) return 0;
        if ((r->match & NF_M_SRC) && memcmp(r->src, src, addr_len)) return 0;
        if ((r->match & NF_M_DST) && memcmp(r->dst, dst, addr_len)) return 0;
    }
    if ((r->match & NF_M_SPORT) && r->sport != sport) return 0;
    if ((r->match & NF_M_DPORT) && r->dport != dport) return 0;
    return 1;
}

int nf_filter(int dir, int addr_len, const void *src, const void *dst,
              uint8_t proto, uint16_t sport, uint16_t dport)
{
    int verdict = NF_ACCEPT;
    uint64_t f = spin_lock_irq(&nf_lock);
    const nf_rule_t *rules = dir == NF_OUT ? nf_out_rules : nf_in_rules;
    int n = dir == NF_OUT ? nf_out_n : nf_in_n;
    for (int i = 0; i < n; i++)
        if (rule_hits(&rules[i], addr_len, src, dst, proto, sport, dport)) {
            verdict = rules[i].verdict;
            break;
        }
    spin_unlock_irq(&nf_lock, f);
    return verdict;
}

/* ── parser ──────────────────────────────────────────────────
 * Grammar: VERDICT PROTO [src ADDR] [dst ADDR] [sport N] [dport N]
 *   VERDICT = accept | drop
 *   PROTO   = tcp | udp | icmp | tcp6 | udp6 | icmpv6 | any
 * Plus the bare command "flush". Whitespace-separated, one rule per line. */

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
        char v[48];
        p = tok(p, end, v, sizeof(v));
        if (!v[0]) return -1;
        if (!strcmp(t, "src"))        { if (parse_addr(v, r, r->src)) return -1; r->match |= NF_M_SRC; }
        else if (!strcmp(t, "dst"))   { if (parse_addr(v, r, r->dst)) return -1; r->match |= NF_M_DST; }
        else if (!strcmp(t, "sport")) { if (parse_u16(v, &r->sport)) return -1; r->match |= NF_M_SPORT; }
        else if (!strcmp(t, "dport")) { if (parse_u16(v, &r->dport)) return -1; r->match |= NF_M_DPORT; }
        else return -1;
    }
    return 0;
}

int nf_ctl_set(const char *line, size_t len)
{
    const char *end = line + len;
    char first[16];
    tok(line, end, first, sizeof(first));
    if (!strcmp(first, "flush")) {
        uint64_t f = spin_lock_irq(&nf_lock);
        nf_in_n = nf_out_n = 0;
        nf_rules_active = 0;
        spin_unlock_irq(&nf_lock, f);
        return 0;
    }

    nf_rule_t r; int dir;
    if (parse_rule(line, end, &r, &dir) != 0) return -22;   /* EINVAL */

    uint64_t f = spin_lock_irq(&nf_lock);
    int *n = dir == NF_OUT ? &nf_out_n : &nf_in_n;
    nf_rule_t *tbl = dir == NF_OUT ? nf_out_rules : nf_in_rules;
    if (*n >= NF_MAX_RULES) { spin_unlock_irq(&nf_lock, f); return -28; } /* ENOSPC */
    tbl[*n] = r;
    (*n)++;
    nf_rules_active = nf_in_n + nf_out_n;
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
        d_str(b, "\n");
    }
}

int nf_ctl_dump(char *buf, size_t cap)
{
    dbuf_t b = { buf, cap };
    uint64_t f = spin_lock_irq(&nf_lock);
    dump_chain(&b, "in", nf_in_rules, nf_in_n);
    dump_chain(&b, "out", nf_out_rules, nf_out_n);
    spin_unlock_irq(&nf_lock, f);
    if (b.rem) *b.p = '\0';
    return (int)(cap - b.rem);
}
