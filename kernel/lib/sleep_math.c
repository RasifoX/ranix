/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The pure half of suspend: no kernel calls, so tests/host_sleep.c checks it on
 * a host (and on the real Casper DSDT). See include/kernel/sleep.h. */
#ifdef HOST_TEST
#include <string.h>
#else
#include <kernel/lib/string.h>
#endif
#include <kernel/sleep.h>

#define NS 1000000000LL

/* ---- \_Sx_ without an interpreter ----
 * Name (_S3_, Package () { a, b, ... }) is NameOp, the name, PackageOp, a
 * PkgLength, NumElements, then the values as Zero, One or BytePrefix n. Firmware
 * often wraps it: If (SS3) { Name (_S3_ ...) }, SS3 itself a Name (SS3, One) the
 * setup program may have patched. That one If is followed; anything else is
 * reported as conditional, never guessed. */

static int pkg_len(const uint8_t *d, uint32_t len, uint32_t p, uint32_t *value, uint32_t *nbytes)
{
	if (p >= len) return -1;
	uint8_t lead = d[p];
	uint32_t nb = (uint32_t)(lead >> 6) + 1;
	if (p + nb > len) return -1;
	uint32_t v = nb == 1 ? (lead & 0x3Fu) : (lead & 0x0Fu);
	for (uint32_t k = 1; k < nb; k++) v |= (uint32_t)d[p + k] << (4 + 8 * (k - 1));
	*value = v;
	*nbytes = nb;
	return 0;
}

static int aml_const(const uint8_t *d, uint32_t len, uint32_t *p, uint32_t *out)
{
	if (*p >= len) return -1;
	uint8_t op = d[*p];
	if (op == 0x00 || op == 0x01) { *out = op; *p += 1; return 0; }          /* Zero, One */
	if (op == 0xFF) { *out = 0xFFFFFFFFu; *p += 1; return 0; }               /* Ones */
	if (op == 0x0A && *p + 1 < len) { *out = d[*p + 1]; *p += 2; return 0; } /* BytePrefix */
	return -1;
}

static int name_char(uint8_t c, int first)
{
	return c == '_' || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

/* Name (SEG, constant) anywhere in the table: 0 or 1 in *v, or -1 when there is none */
static int const_name(const uint8_t *d, uint32_t len, const uint8_t *seg, uint32_t *v)
{
	for (uint32_t i = 36; i + 6 <= len; i++) {
		if (d[i] != 0x08 || memcmp(d + i + 1, seg, 4)) continue;
		uint32_t p = i + 5, x;
		if (aml_const(d, len, &p, &x)) continue;
		*v = x != 0;
		return 0;
	}
	return -1;
}

/* An If whose PkgLength covers the Name at ns, its predicate a lone NameSeg right
 * before it: SLEEP_TYPE_OK / NONE by that Name's constant, COND when unknown.
 * No If there: OK. */
static int enclosing_if(const uint8_t *d, uint32_t len, uint32_t ns)
{
	for (uint32_t k = ns > 10 ? ns - 10 : 0; k < ns; k++) {
		if (k < 36 || d[k] != 0xA0) continue;
		uint32_t plen, nb;
		if (pkg_len(d, len, k + 1, &plen, &nb)) continue;
		uint32_t pred = k + 1 + nb;
		if (pred + 4 != ns || k + 1 + plen <= ns) continue;
		const uint8_t *seg = d + pred;
		if (!name_char(seg[0], 1) || !name_char(seg[1], 0) || !name_char(seg[2], 0) || !name_char(seg[3], 0))
			continue;
		uint32_t v;
		if (const_name(d, len, seg, &v)) return SLEEP_TYPE_COND;
		return v ? SLEEP_TYPE_OK : SLEEP_TYPE_NONE;
	}
	return SLEEP_TYPE_OK;
}

int acpi_sleep_type(const uint8_t *d, uint32_t len, int state, uint8_t *a, uint8_t *b)
{
	if (!d || state < 0 || state > 5 || len < 36) return SLEEP_TYPE_NONE;
	uint8_t name[4] = { '_', 'S', (uint8_t)('0' + state), '_' };
	for (uint32_t i = 37; i + 4 <= len; i++) {
		if (memcmp(d + i, name, 4)) continue;
		uint32_t ns;
		if (d[i - 1] == 0x08) ns = i - 1;
		else if (d[i - 1] == '\\' && i >= 38 && d[i - 2] == 0x08) ns = i - 2;
		else continue;                                     /* a reference, not the Name */
		uint32_t p = i + 4, plen, nb, ta, tb;
		if (p >= len || d[p] != 0x12) continue;            /* PackageOp */
		if (pkg_len(d, len, p + 1, &plen, &nb)) continue;
		p += 1 + nb + 1;                                   /* PackageOp, PkgLength, NumElements */
		if (aml_const(d, len, &p, &ta) || aml_const(d, len, &p, &tb)) continue;
		int r = enclosing_if(d, len, ns);
		if (r == SLEEP_TYPE_NONE) continue;
		*a = (uint8_t)ta;
		*b = (uint8_t)tb;
		return r;
	}
	return SLEEP_TYPE_NONE;
}

uint64_t sleep_length_ns(int kind, uint64_t raw_before, uint64_t raw_after, int64_t rtc_before, int64_t rtc_after)
{
	int rtc_ok = rtc_before > 0 && rtc_after > 0;
	uint64_t rtc_ns = rtc_ok && rtc_after >= rtc_before ? (uint64_t)(rtc_after - rtc_before) * (uint64_t)NS : 0;
	if (kind == SLEEP_S2IDLE && raw_after >= raw_before) {
		uint64_t cnt = raw_after - raw_before;
		if (!rtc_ok || rtc_after < rtc_before) return cnt;
		uint64_t diff = cnt > rtc_ns ? cnt - rtc_ns : rtc_ns - cnt;
		return diff <= 2 * (uint64_t)NS ? cnt : rtc_ns;
	}
	return rtc_ns;
}

static void ts_add(int64_t *sec, int64_t *nsec, uint64_t ns)
{
	*sec += (int64_t)(ns / (uint64_t)NS);
	*nsec += (int64_t)(ns % (uint64_t)NS);
	while (*nsec >= NS) { *nsec -= NS; (*sec)++; }
	while (*nsec < 0)   { *nsec += NS; (*sec)--; }
}

void sleep_clock_resume(sleep_clock_t *c, uint64_t mono_at_entry_ns, uint64_t slept_ns)
{
	c->anchor_ns = mono_at_entry_ns;
	ts_add(&c->boot_off_sec, &c->boot_off_nsec, slept_ns);
	ts_add(&c->rt_off_sec, &c->rt_off_nsec, slept_ns);
	c->slept_total_ns += slept_ns;
	c->count++;
}

/* the word without the shell's newline or trailing blanks */
static size_t word(const char *s, size_t n)
{
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == 0)) n--;
	return n;
}

static int is(const char *s, size_t n, const char *w)
{
	size_t k = strlen(w);
	return n == k && !memcmp(s, w, k);
}

int sleep_parse_state(const char *s, size_t n, int mem_default)
{
	n = word(s, n);
	if (is(s, n, "freeze")) return SLEEP_S2IDLE;
	if (is(s, n, "mem")) return mem_default;
	return -22;
}

int sleep_parse_mem_sleep(const char *s, size_t n)
{
	n = word(s, n);
	if (is(s, n, "s2idle")) return SLEEP_S2IDLE;
	if (is(s, n, "deep")) return SLEEP_DEEP;
	return -22;
}

int sleep_format_mem_sleep(char *out, size_t n, int chosen, int deep_ok)
{
	const char *t = !deep_ok ? "[s2idle]\n" : chosen == SLEEP_DEEP ? "s2idle [deep]\n" : "[s2idle] deep\n";
	size_t k = strlen(t);
	if (k + 1 > n) return -1;
	memcpy(out, t, k + 1);
	return (int)k;
}

int wakealarm_parse(const char *s, size_t n, int64_t now, int64_t *at)
{
	n = word(s, n);
	size_t i = 0;
	int rel = n && s[0] == '+';
	if (rel) i = 1;
	if (i >= n) return -22;
	int64_t v = 0;
	for (; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') return -22;
		if (v > (1LL << 40)) return -22;                   /* 35000 years: junk */
		v = v * 10 + (s[i] - '0');
	}
	if (rel) { *at = now + v; return 0; }
	if (v == 0) { *at = 0; return 0; }
	if (v <= now) return -22;
	*at = v;
	return 0;
}
