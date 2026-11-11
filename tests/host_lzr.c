/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the LZR compressor zram uses:
 *   cc -I. -Iinclude tests/host_lzr.c kernel/lib/lzr.c && ./a.out
 * Round trips of the page shapes swap sees, and damaged streams that must be
 * refused without reading or writing out of bounds. */
#include <stdio.h>
#include <string.h>
#include <kernel/lib/lzr.h>

static int fails, passes;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL: %s\n", name); } } while (0)

static uint16_t table[LZR_TABLE_SIZE];
static uint8_t  in[8192], comp[16384], back[8192];

static long roundtrip(const char *name, size_t n)
{
	long c = lzr_compress(in, n, comp, sizeof(comp), table);
	CHECK(name, c >= 0);
	if (c < 0) return c;
	memset(back, 0xAA, sizeof(back));
	long d = lzr_decompress(comp, (size_t)c, back, n);
	char what[96];
	snprintf(what, sizeof(what), "%s: back %ld of %zu, same bytes", name, d, n);
	CHECK(what, d == (long)n && memcmp(in, back, n) == 0);
	return c;
}

static unsigned rnd = 12345;
static uint8_t rbyte(void) { rnd = rnd * 1103515245u + 12345u; return (uint8_t)(rnd >> 16); }

int main(void)
{
	memset(in, 0, 4096);
	long z = roundtrip("zero page", 4096);
	CHECK("zero page under 128 bytes", z > 0 && z < 128);

	memset(in, 0, 4096);
	for (int i = 0; i < 4096; i += 512) { in[i] = (uint8_t)i; in[i + 1] = 0x5A; }
	long s = roundtrip("sparse words", 4096);
	CHECK("sparse words under 512 bytes", s > 0 && s < 512);

	for (int i = 0; i < 4096; i++) in[i] = (uint8_t)("abcdefgh"[i % 8]);
	roundtrip("period 8", 4096);

	for (int i = 0; i < 4096; i++) in[i] = rbyte();
	long r = roundtrip("random page", 4096);
	CHECK("random page grows by under 3%", r > 0 && r < 4096 + 4096 / 32);
	CHECK("random page refused at 3/4 cap", lzr_compress(in, 4096, comp, 3072, table) == -1);

	for (int i = 0; i < 4096; i++) in[i] = (i & 64) ? rbyte() : 0;
	roundtrip("half random", 4096);

	for (size_t n = 0; n < 40; n++) { for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(i % 3); roundtrip("short input", n); }

	for (int i = 0; i < 8000; i++) in[i] = (uint8_t)(i / 300);
	roundtrip("8000 bytes of steps", 8000);

	/* damaged streams: refused, never past the buffers */
	uint8_t bad1[] = { 0x05, 'a', 'b' };                  /* literal run longer than input */
	CHECK("short literal refused", lzr_decompress(bad1, sizeof(bad1), back, 4096) == -1);
	uint8_t bad2[] = { 0x00, 'a', 0x80, 0x05, 0x00 };     /* copy from before the start */
	CHECK("far copy refused", lzr_decompress(bad2, sizeof(bad2), back, 4096) == -1);
	uint8_t bad3[] = { 0x00, 'a', 0x80, 0x00, 0x00 };     /* distance 0 */
	CHECK("distance 0 refused", lzr_decompress(bad3, sizeof(bad3), back, 4096) == -1);
	uint8_t bad4[] = { 0x00, 'a', 0xFF, 0x01, 0x00 };     /* 131 bytes into a 64-byte buffer */
	CHECK("overflow refused", lzr_decompress(bad4, sizeof(bad4), back, 64) == -1);
	uint8_t bad5[] = { 0x00, 'a', 0x80 };                 /* copy without its distance */
	CHECK("cut copy refused", lzr_decompress(bad5, sizeof(bad5), back, 4096) == -1);
	for (int t = 0; t < 2000; t++) {                      /* noise never crashes */
		uint8_t junk[64];
		for (int i = 0; i < 64; i++) junk[i] = rbyte();
		long d = lzr_decompress(junk, sizeof(junk), back, 4096);
		if (d > 4096) { CHECK("noise within cap", 0); break; }
	}
	CHECK("over LZR_MAX_IN refused", lzr_compress(in, LZR_MAX_IN + 1, comp, sizeof(comp), table) == -1);

	printf("lzr: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
