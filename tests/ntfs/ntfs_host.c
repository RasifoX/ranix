/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Host harness for kernel/fs/ntfs_core.c.
 *   ntfs_host walk IMAGE [SECTOR]        every file: sha256, size, path
 *   ntfs_host fuzz IMAGE SEED N [SECTOR] N mutated images under ASan and UBSan
 *   ntfs_host hot IMAGE [SECTOR]         sectors a clean walk reads, one per line
 *   ntfs_host lznt1                      fixed decoder vectors
 * The image is opened read-only and mapped private: mutations stay in memory.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "kernel/fs/ntfs_core.h"

/* ── sha256 (FIPS 180-4) ── */
typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; uint32_t fill; } sha_t;
static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x,c) (((x)>>(c))|((x)<<(32-(c))))
static void sha_block(sha_t *s, const uint8_t *p)
{
	uint32_t w[64], a[8];
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i]<<24|p[4*i+1]<<16|p[4*i+2]<<8|p[4*i+3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);
		uint32_t s1 = ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);
		w[i] = w[i-16]+s0+w[i-7]+s1;
	}
	memcpy(a, s->h, sizeof(a));
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = a[7]+(ROR(a[4],6)^ROR(a[4],11)^ROR(a[4],25))+((a[4]&a[5])^(~a[4]&a[6]))+K[i]+w[i];
		uint32_t t2 = (ROR(a[0],2)^ROR(a[0],13)^ROR(a[0],22))+((a[0]&a[1])^(a[0]&a[2])^(a[1]&a[2]));
		a[7]=a[6];a[6]=a[5];a[5]=a[4];a[4]=a[3]+t1;a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=t1+t2;
	}
	for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(sha_t *s)
{
	static const uint32_t I[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
	memcpy(s->h, I, sizeof(I)); s->n = 0; s->fill = 0;
}
static void sha_add(sha_t *s, const void *d, size_t n)
{
	const uint8_t *p = d;
	s->n += n;
	while (n) {
		if (!s->fill && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
		size_t t = 64 - s->fill; if (t > n) t = n;
		memcpy(s->b + s->fill, p, t); s->fill += t; p += t; n -= t;
		if (s->fill == 64) { sha_block(s, s->b); s->fill = 0; }
	}
}
static void sha_end(sha_t *s, char *hex)
{
	uint64_t bits = s->n * 8; uint8_t z = 0x80, l[8];
	sha_add(s, &z, 1); z = 0;
	while (s->fill != 56) sha_add(s, &z, 1);
	for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8*i));
	sha_add(s, l, 8);
	for (int i = 0; i < 8; i++) sprintf(hex + 8*i, "%08x", s->h[i]);
}

/* ── device ── */
static uint8_t *g_img;
static uint64_t g_sectors, g_base;
static uint8_t *g_touch;         /* per-sector: read during the clean walk */
static uint64_t g_fail_every, g_allocs;
static int      g_in_data;       /* file data is not what the fuzzer is after */

void *ntfs_host_alloc(size_t n)
{
	g_allocs++;
	if (g_fail_every && g_allocs % g_fail_every == 0) return NULL;
	return malloc(n);
}

static int img_read(void *dev, uint64_t lba, void *buf, uint32_t n)
{
	(void)dev;
	if (lba >= g_sectors || n > g_sectors - lba) return -1;
	memcpy(buf, g_img + ((g_base + lba) << 9), (size_t)n << 9);
	if (g_touch && !g_in_data && n <= 128) memset(g_touch + lba, 1, n);
	return 0;
}

/* ── walk ── */
typedef struct {
	int print;
	uint64_t cap;        /* bytes read per file */
	uint64_t files, dirs, errs, budget;
	uint8_t *buf;
	ntfs_info_t *info;
} walk_t;

static void walk_file(ntfs_vol_t *v, walk_t *w, const ntfs_info_t *in, const char *path)
{
	sha_t s; char hex[65];
	uint64_t off = 0, size = in->size;
	sha_init(&s);
	while (off < size && off < w->cap) {
		uint32_t n = size - off > (1u << 20) ? (1u << 20) : (uint32_t)(size - off);
		g_in_data = !(w->files % 4);   /* a quarter of the files keep their data hot */
		g_in_data = 1 - g_in_data;
		long r = ntfs_file_read(v, in->mft, off, w->buf, n);
		g_in_data = 0;
		if (r <= 0) {
			w->errs++;
			if (w->print) printf("ERROR %ld %s\n", r, path);
			return;
		}
		sha_add(&s, w->buf, (size_t)r);
		off += (uint64_t)r;
	}
	sha_end(&s, hex);
	w->files++;
	if (w->print) printf("%s  %llu  %s\n", hex, (unsigned long long)size, path);
}

static void walk_dir(ntfs_vol_t *v, walk_t *w, uint64_t mft, const char *path, int depth)
{
	ntfs_dir_t *d;
	if (depth > 1024 || !w->budget) { w->errs++; return; }
	int rc = ntfs_dir_load(v, mft, &d);
	if (rc) {
		w->errs++;
		if (w->print) printf("ERROR %d dir %s\n", rc, path);
		return;
	}
	if (ntfs_dir_damaged(d)) {
		w->errs++;
		if (w->print) printf("DAMAGED %s\n", path);
	}
	/* the table goes away on the next load: copy it out first */
	uint32_t n = ntfs_dir_count(d);
	uint64_t *refs = malloc((n + 1) * sizeof(*refs));
	char **names = malloc((n + 1) * sizeof(*names));
	if (!refs || !names) abort();
	for (uint32_t i = 0; i < n; i++) {
		ntfs_dirent_t e;
		if (ntfs_dir_entry(d, i, &e)) abort();
		refs[i] = e.mref;
		names[i] = strdup(e.name);
		ntfs_dirent_t f;
		if (ntfs_dir_find(d, e.name, &f) || f.mref != e.mref) abort();
	}
	for (uint32_t i = 0; i < n; i++) {
		if (w->budget) w->budget--;
		size_t plen = strlen(path) + strlen(names[i]) + 2;
		char *p = malloc(plen);
		snprintf(p, plen, "%s/%s", path, names[i]);
		ntfs_info_t *in = w->info;
		rc = w->budget ? ntfs_get_info(v, refs[i], in) : -1;
		if (rc) {
			w->errs++;
			if (w->print) printf("ERROR %d stat %s\n", rc, p);
		} else if (in->is_link) {
			if (w->print) printf("LINK %s -> %s\n", p, in->target);
		} else if (in->is_dir) {
			w->dirs++;
			walk_dir(v, w, in->mft, p, depth + 1);
		} else if (in->unreadable) {
			long r = ntfs_file_read(v, in->mft, 0, w->buf, 16);
			if (r != NTFS_EOPNOTSUPP && !g_touch) abort();
			if (w->print) printf("UNREADABLE %d tag=%08x %s\n", in->unreadable, in->reparse_tag, p);
		} else {
			walk_file(v, w, in, p);
		}
		free(p);
		free(names[i]);
	}
	free(refs);
	free(names);
}

static uint64_t g_budget = 3000;

static int walk(int print, uint64_t cap, walk_t *out)
{
	static ntfs_info_t info;
	static uint8_t *buf;
	if (!buf) buf = malloc(1u << 20);
	walk_t w = { .print = print, .cap = cap, .budget = print ? 200000 : g_budget, .buf = buf, .info = &info };
	ntfs_vol_t *v;
	int rc = ntfs_open(img_read, NULL, g_sectors, &v);
	if (rc) {
		if (print) printf("OPEN FAILED %d\n", rc);
		return rc;
	}
	ntfs_volinfo_t vi;
	ntfs_volinfo(v, &vi);
	if (print)
		fprintf(stderr, "volume %u.%u dirty=%u hibernated=%u cluster=%u record=%u label=%s\n",
		        vi.major, vi.minor, vi.dirty, vi.hibernated, vi.cluster_size, vi.record_size, vi.label);
	walk_dir(v, &w, NTFS_MFT_ROOT, "", 0);
	ntfs_close(v);
	if (out) *out = w;
	return 0;
}

/* ── fuzz ── */
static uint64_t rng_s;
static uint64_t rnd(void)
{
	rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
	return rng_s;
}

typedef struct { uint64_t off; uint8_t old; } undo_t;

static const uint8_t SPECIAL[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x40, 0x10 };

/* Bytes of the sectors the clean walk read, which is nearly all metadata. Field
 * sized multi-byte stompers reach the length and offset words. */
static int mutate(uint64_t *hot, uint64_t nhot, undo_t *u, int cap)
{
	int nu = 0;
	int ops = 1 + (int)(rnd() % 8);
	for (int k = 0; k < ops; k++) {
		uint64_t sec = (rnd() & 3) ? hot[rnd() % nhot] : hot[rnd() % (nhot < 256 ? nhot : 256)];
		uint32_t in  = (uint32_t)(rnd() % 512);
		int width = 1 << (rnd() % 4);
		int kind = (int)(rnd() % 4);
		for (int i = 0; i < width && nu < cap; i++) {
			uint64_t off = ((g_base + sec) << 9) + in + (uint64_t)i;
			if (off >= (g_base + g_sectors) << 9) break;
			u[nu].off = off; u[nu].old = g_img[off]; nu++;
			switch (kind) {
			case 0: g_img[off] = SPECIAL[rnd() % sizeof(SPECIAL)]; break;
			case 1: g_img[off] ^= (uint8_t)(1u << (rnd() % 8)); break;
			case 2: g_img[off] = (uint8_t)rnd(); break;
			default: g_img[off] = i == width - 1 ? 0x7F : 0xFF; break;
			}
		}
	}
	return nu;
}

static int fuzz(uint64_t seed, uint64_t iters)
{
	rng_s = seed * 0x9E3779B97F4A7C15ULL + 88172645463325252ULL;
	g_touch = calloc(g_sectors, 1);
	if (!g_touch) return 2;
	walk_t w0;
	if (walk(0, 1u << 24, &w0)) { fprintf(stderr, "clean image does not open\n"); return 2; }
	uint64_t nhot = 0;
	for (uint64_t i = 0; i < g_sectors; i++) nhot += g_touch[i];
	uint64_t *hot = malloc(nhot * sizeof(*hot));
	nhot = 0;
	for (uint64_t i = 0; i < g_sectors; i++) if (g_touch[i]) hot[nhot++] = i;
	free(g_touch);
	g_touch = (uint8_t *)calloc(g_sectors, 1);
	printf("clean: %llu files %llu dirs, %llu hot sectors\n", (unsigned long long)w0.files,
	       (unsigned long long)w0.dirs, (unsigned long long)nhot);

	static undo_t undo[256];
	uint64_t opened = 0, errs = 0, files = 0;
	time_t t0 = time(NULL);
	for (uint64_t it = 0; it < iters; it++) {
		int nu = mutate(hot, nhot, undo, 256);
		g_fail_every = (rnd() & 7) == 0 ? 3 + rnd() % 400 : 0;
		walk_t w = {0};
		int full = !(rnd() & 15);
		g_budget = full ? 3000 : 300;
		if (walk(0, full ? 1u << 18 : 1u << 13, &w) == 0) { opened++; errs += w.errs; files += w.files; }
		g_fail_every = 0;
		while (nu) { nu--; g_img[undo[nu].off] = undo[nu].old; }
		if (it && !(it % 200)) {
			printf("%llu iters, %llu opened, %llu errors, %llu files, %lds\n",
			       (unsigned long long)it, (unsigned long long)opened,
			       (unsigned long long)errs, (unsigned long long)files, (long)(time(NULL) - t0));
			fflush(stdout);
		}
	}
	printf("DONE %llu iters, %llu opened, %llu error results, %llu files read, no fault\n",
	       (unsigned long long)iters, (unsigned long long)opened,
	       (unsigned long long)errs, (unsigned long long)files);
	return 0;
}

/* ── LZNT1 vectors ── */
static int lznt1(void)
{
	/* uncompressed chunk, then compressed literals, then a match, then the end */
	static const uint8_t c1[] = {
		0x08, 0xB0, 0x00, 'a','b','c','d','e','f','g','h',  0x00, 0x00 };
	uint8_t out[8192];
	long n = ntfs_lznt1(c1, sizeof(c1), out, sizeof(out));
	if (n != 8 || memcmp(out, "abcdefgh", 8)) { printf("FAIL literal %ld\n", n); return 1; }
	/* "abcabcabcabc": three literals, then disp 3 len 9 at position 3 */
	static const uint8_t c2[] = { 0x05, 0xB0, 0x08, 'a','b','c', 0x06, 0x20, 0x00, 0x00 };
	n = ntfs_lznt1(c2, sizeof(c2), out, sizeof(out));
	if (n != 12 || memcmp(out, "abcabcabcabc", 12)) { printf("FAIL match %ld\n", n); return 1; }
	/* a match before the start of the chunk is refused */
	static const uint8_t c3[] = { 0x02, 0xB0, 0x01, 0xFF, 0xFF, 0x00, 0x00 };
	if (ntfs_lznt1(c3, sizeof(c3), out, sizeof(out)) >= 0) { printf("FAIL oob\n"); return 1; }
	/* a chunk header claiming more than there is */
	static const uint8_t c4[] = { 0xFF, 0xBF, 0x00 };
	if (ntfs_lznt1(c4, sizeof(c4), out, sizeof(out)) >= 0) { printf("FAIL trunc\n"); return 1; }
	/* an output cap must hold */
	static const uint8_t c5[] = { 0x05, 0xB0, 0x08, 'a','b','c', 0xFF, 0x0F };
	n = ntfs_lznt1(c5, sizeof(c5), out, 5);
	if (n > 5) { printf("FAIL cap\n"); return 1; }
	printf("lznt1 vectors ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "lznt1")) return lznt1();
	if (argc < 3) return 2;
	int fd = open(argv[2], O_RDONLY);
	if (fd < 0) { perror("open"); return 2; }
	struct stat st;
	fstat(fd, &st);
	/* MAP_PRIVATE on an O_RDONLY descriptor: nothing can reach the file */
	g_img = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
	if (g_img == MAP_FAILED) { perror("mmap"); return 2; }
	int fuzzing = !strcmp(argv[1], "fuzz");
	g_base = (uint64_t)(argc > (fuzzing ? 5 : 3) ? strtoull(argv[fuzzing ? 5 : 3], NULL, 0) : 0);
	g_sectors = ((uint64_t)st.st_size >> 9) - g_base;
	if (!strcmp(argv[1], "walk")) {
		walk_t w;
		int rc = walk(1, ~0ULL, &w);
		if (rc) return 1;
		fprintf(stderr, "%llu files, %llu dirs, %llu errors\n", (unsigned long long)w.files,
		        (unsigned long long)w.dirs, (unsigned long long)w.errs);
		return w.errs ? 1 : 0;
	}
	if (!strcmp(argv[1], "hot")) {
		g_touch = calloc(g_sectors, 1);
		walk_t w;
		if (walk(0, 1u << 20, &w)) return 1;
		for (uint64_t i = 0; i < g_sectors; i++) if (g_touch[i]) printf("%llu\n", (unsigned long long)i);
		return 0;
	}
	if (fuzzing && argc >= 5) return fuzz(strtoull(argv[3], NULL, 0), strtoull(argv[4], NULL, 0));
	return 2;
}
