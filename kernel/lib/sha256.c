/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SHA-256, written from FIPS 180-4 sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2. */
#include <kernel/lib/sha256.h>
#include <kernel/lib/crc32.h>
#include <kernel/lib/string.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void compress(uint32_t h[8], const uint8_t *m)
{
	uint32_t w[64];
	for (int t = 0; t < 16; t++, m += 4)
		w[t] = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
	for (int t = 16; t < 64; t++) {
		uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
		uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}

	uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
	uint32_t e = h[4], f = h[5], g = h[6], i = h[7];
	for (int t = 0; t < 64; t++) {
		uint32_t t1 = i + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
		              ((e & f) ^ (~e & g)) + K[t] + w[t];
		uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
		              ((a & b) ^ (a & c) ^ (b & c));
		i = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += i;
}

void sha256_init(sha256_t *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(c->h, iv, sizeof(iv));
	c->total = 0;
	c->fill = 0;
}

void sha256_update(sha256_t *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	c->total += len;
	while (len) {
		if (c->fill == 0 && len >= SHA256_BLOCK_SIZE) {
			compress(c->h, p);
			p += SHA256_BLOCK_SIZE;
			len -= SHA256_BLOCK_SIZE;
			continue;
		}
		size_t n = SHA256_BLOCK_SIZE - c->fill;
		if (n > len) n = len;
		memcpy(c->block + c->fill, p, n);
		c->fill += (uint32_t)n;
		p += n;
		len -= n;
		if (c->fill == SHA256_BLOCK_SIZE) {
			compress(c->h, c->block);
			c->fill = 0;
		}
	}
}

void sha256_final(sha256_t *c, uint8_t out[SHA256_DIGEST_SIZE])
{
	uint64_t bits = c->total * 8;
	c->block[c->fill++] = 0x80;
	if (c->fill > SHA256_BLOCK_SIZE - 8) {
		memset(c->block + c->fill, 0, SHA256_BLOCK_SIZE - c->fill);
		compress(c->h, c->block);
		c->fill = 0;
	}
	memset(c->block + c->fill, 0, SHA256_BLOCK_SIZE - 8 - c->fill);
	for (int i = 0; i < 8; i++)
		c->block[SHA256_BLOCK_SIZE - 1 - i] = (uint8_t)(bits >> (8 * i));
	compress(c->h, c->block);
	for (int i = 0; i < 8; i++) {
		out[4 * i]     = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
	sha256_t c;
	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
}

int hash_selftest(void)
{
	static const struct { const char *msg; uint8_t want[SHA256_DIGEST_SIZE]; } v[] = {
		{ "abc", {
			0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
			0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad } },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", {
			0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
			0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 } },
		{ "", {
			0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
			0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 } },
	};
	uint8_t got[SHA256_DIGEST_SIZE];
	for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		sha256(v[i].msg, strlen(v[i].msg), got);
		if (memcmp(got, v[i].want, sizeof(got)) != 0) return -1;
	}
	/* a message fed in pieces hashes like one fed whole */
	sha256_t c;
	sha256_init(&c);
	for (const char *s = v[1].msg; *s; s++) sha256_update(&c, s, 1);
	sha256_final(&c, got);
	if (memcmp(got, v[1].want, sizeof(got)) != 0) return -1;

	return crc32("123456789", 9) == 0xCBF43926u ? 0 : -1;
}
