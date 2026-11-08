/* SHA-256 / HMAC-SHA256 for the rescue channel's userspace side (test_rescue,
 * ranix-rescue). Single header, no libc crypto on the target. FIPS 180-4. */
#ifndef RX_HMAC_H
#define RX_HMAC_H
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; uint32_t f; } rx_sha_t;

static const uint32_t rx_K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define RX_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void rx_blk(rx_sha_t *c, const uint8_t *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, cc;
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = RX_ROR(w[i-15], 7) ^ RX_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = RX_ROR(w[i-2], 17) ^ RX_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (RX_ROR(e, 6) ^ RX_ROR(e, 11) ^ RX_ROR(e, 25)) + ((e & f) ^ (~e & g)) + rx_K[i] + w[i];
		uint32_t t2 = (RX_ROR(a, 2) ^ RX_ROR(a, 13) ^ RX_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void rx_init(rx_sha_t *c)
{
	static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
	memcpy(c->h, iv, sizeof iv); c->n = 0; c->f = 0;
}
static void rx_upd(rx_sha_t *c, const void *d, size_t n)
{
	const uint8_t *p = d; c->n += n;
	while (n--) { c->b[c->f++] = *p++; if (c->f == 64) { rx_blk(c, c->b); c->f = 0; } }
}
static void rx_fin(rx_sha_t *c, uint8_t out[32])
{
	uint64_t bits = c->n * 8; uint8_t z = 0x80;
	rx_upd(c, &z, 1); z = 0;
	while (c->f != 56) rx_upd(c, &z, 1);
	for (int i = 7; i >= 0; i--) { uint8_t x = (uint8_t)(bits >> (8 * i)); rx_upd(c, &x, 1); }
	for (int i = 0; i < 8; i++) { out[4*i] = c->h[i] >> 24; out[4*i+1] = c->h[i] >> 16; out[4*i+2] = c->h[i] >> 8; out[4*i+3] = c->h[i]; }
}
static void rx_hmac(const uint8_t key[32], const void *a, size_t an, const void *b, size_t bn, uint8_t out[32])
{
	uint8_t pad[64], ih[32]; rx_sha_t s;
	memset(pad, 0x36, 64); for (int i = 0; i < 32; i++) pad[i] ^= key[i];
	rx_init(&s); rx_upd(&s, pad, 64); rx_upd(&s, a, an); if (bn) rx_upd(&s, b, bn); rx_fin(&s, ih);
	memset(pad, 0x5c, 64); for (int i = 0; i < 32; i++) pad[i] ^= key[i];
	rx_init(&s); rx_upd(&s, pad, 64); rx_upd(&s, ih, 32); rx_fin(&s, out);
}
static void rx_hex(char *d, const uint8_t *s, size_t n)
{
	static const char H[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { d[2*i] = H[s[i] >> 4]; d[2*i+1] = H[s[i] & 15]; }
	d[2*n] = 0;
}
#endif
