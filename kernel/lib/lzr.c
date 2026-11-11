/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* LZR compressor (format in lzr.h). Greedy LZ77: at each position the last
 * place the same four bytes were seen is looked up in a hash table; if they
 * still match, the match is stretched as far as it goes and written as one
 * copy token, else the byte waits in the literal run. Fast and short rather
 * than tight: zram pages are 4 KB and mostly zeros, counters and pointers. */
#include <kernel/lib/lzr.h>

#define MIN_MATCH 4
#define MAX_MATCH (0x7F + MIN_MATCH)
#define MAX_LIT   128

static uint32_t hash4(const uint8_t *p)
{
	uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	return (v * 2654435761u) >> (32 - LZR_HASH_BITS);
}

static int same4(const uint8_t *a, const uint8_t *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* n literal bytes from src as runs of up to MAX_LIT; the new out length or -1 */
static long put_literals(uint8_t *out, size_t cap, long o, const uint8_t *src, size_t n)
{
	while (n > 0) {
		size_t k = n < MAX_LIT ? n : MAX_LIT;
		if (o < 0 || (size_t)o + 1 + k > cap) return -1;
		out[o++] = (uint8_t)(k - 1);
		for (size_t j = 0; j < k; j++) out[o++] = src[j];
		src += k;
		n -= k;
	}
	return o;
}

long lzr_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint16_t *table)
{
	if (n > LZR_MAX_IN) return -1;
	for (uint32_t i = 0; i < LZR_TABLE_SIZE; i++) table[i] = 0;   /* 0: never seen */
	size_t i = 0, lit = 0;
	long o = 0;
	while (i + MIN_MATCH <= n) {
		uint32_t h = hash4(in + i);
		size_t seen = table[h];                      /* position + 1 */
		table[h] = (uint16_t)(i + 1);
		if (!seen || !same4(in + seen - 1, in + i)) {
			i++;
			continue;
		}
		const uint8_t *m = in + seen - 1;
		size_t len = MIN_MATCH;
		while (i + len < n && len < MAX_MATCH && m[len] == in[i + len]) len++;
		o = put_literals(out, cap, o, in + lit, i - lit);
		if (o < 0 || (size_t)o + 3 > cap) return -1;
		size_t d = (size_t)(in + i - m);
		out[o++] = (uint8_t)(0x80 | (len - MIN_MATCH));
		out[o++] = (uint8_t)d;
		out[o++] = (uint8_t)(d >> 8);
		i += len;
		lit = i;
	}
	return put_literals(out, cap, o, in + lit, n - lit);
}

long lzr_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	size_t i = 0, o = 0;
	while (i < n) {
		uint8_t c = in[i++];
		if (c < 0x80) {
			size_t k = (size_t)c + 1;
			if (k > n - i || k > cap - o) return -1;
			for (size_t j = 0; j < k; j++) out[o++] = in[i++];
			continue;
		}
		size_t len = (size_t)(c & 0x7F) + MIN_MATCH;
		if (n - i < 2) return -1;
		size_t d = (size_t)in[i] | (size_t)in[i + 1] << 8;
		i += 2;
		if (d == 0 || d > o || len > cap - o) return -1;
		for (size_t j = 0; j < len; j++, o++) out[o] = out[o - d];
	}
	return (long)o;
}
