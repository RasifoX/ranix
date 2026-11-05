/* End-to-end TCP stress: one side streams a generated pattern, the other side
 * checks it, both print SHA-256 and the MB/s seen.
 *   test_tcpsha loop MIB              both ends over 127.0.0.1
 *   test_tcpsha send MIB HOST PORT    connect, send
 *   test_tcpsha recv MIB HOST PORT    connect, receive and check
 *   test_tcpsha lsend|lrecv MIB ADDR PORT    the same, waiting for one connection
 * A peer on another machine produces or checks the same stream. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLK 65536

typedef struct { uint32_t h[8]; uint64_t len; uint8_t b[64]; unsigned n; } sha_t;

static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha_t *c, const uint8_t *p)
{
	uint32_t w[64], s[8];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
		       (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t a = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t b = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + a + w[i - 7] + b;
	}
	memcpy(s, c->h, sizeof(s));
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) +
		              ((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] + w[i];
		uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) +
		              ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
		for (int j = 7; j > 0; j--) s[j] = s[j - 1];
		s[4] += t1;
		s[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++) c->h[i] += s[i];
}

static void sha_init(sha_t *c)
{
	static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
	                                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	memcpy(c->h, iv, sizeof(iv));
	c->len = 0;
	c->n = 0;
}

static void sha_add(sha_t *c, const uint8_t *p, size_t n)
{
	c->len += n;
	while (n) {
		if (!c->n && n >= 64) { sha_block(c, p); p += 64; n -= 64; continue; }
		size_t k = 64 - c->n < n ? 64 - c->n : n;
		memcpy(c->b + c->n, p, k);
		c->n += (unsigned)k;
		p += k;
		n -= k;
		if (c->n == 64) { sha_block(c, c->b); c->n = 0; }
	}
}

static void sha_end(sha_t *c, char *hex)
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	sha_add(c, &pad, 1);
	while (c->n != 56) sha_add(c, &z, 1);
	for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i * 8));
	sha_add(c, l, 8);
	for (int i = 0; i < 8; i++) sprintf(hex + i * 8, "%08x", c->h[i]);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* the stream: 64 KiB blocks of a counter-seeded xorshift, the same on any machine */
static void block(uint8_t *b, uint64_t n)
{
	uint64_t x = n * 0x9e3779b97f4a7c15ULL + 1;
	for (int i = 0; i < BLK; i += 8) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		for (int j = 0; j < 8; j++) b[i + j] = (uint8_t)(x >> (j * 8));
	}
}

static int tx(int fd, long mib, const char *who)
{
	static uint8_t b[BLK];
	sha_t c;
	char hex[65];
	sha_init(&c);
	long blocks = mib * 16;
	double t = now();
	for (long n = 0; n < blocks; n++) {
		block(b, (uint64_t)n);
		sha_add(&c, b, BLK);
		for (size_t off = 0; off < BLK; ) {
			ssize_t w = write(fd, b + off, BLK - off);
			if (w <= 0) { printf("%s FAIL write at %ld MiB\n", who, n / 16); return 1; }
			off += (size_t)w;
		}
	}
	shutdown(fd, SHUT_WR);
	double d = now() - t;
	sha_end(&c, hex);
	printf("%s sha256 %s %ld MiB %.1f s %.1f MB/s\n", who, hex, mib, d, (double)mib / d);
	return 0;
}

static int rx(int fd, long mib, const char *who)
{
	static uint8_t b[BLK], want[BLK];
	sha_t c;
	char hex[65];
	sha_init(&c);
	uint64_t total = (uint64_t)mib * 1048576, have = 0;
	double t = now();
	block(want, 0);
	size_t fill = 0;
	for (;;) {
		ssize_t r = read(fd, b + fill, BLK - fill);
		if (r < 0) { printf("%s FAIL read at %llu\n", who, (unsigned long long)have); return 1; }
		if (!r) break;
		if (memcmp(b + fill, want + fill, (size_t)r)) {
			printf("%s FAIL data at %llu\n", who, (unsigned long long)have);
			return 1;
		}
		fill += (size_t)r;
		have += (uint64_t)r;
		if (fill == BLK) {
			sha_add(&c, b, BLK);
			block(want, have / BLK);
			fill = 0;
		}
	}
	double d = now() - t;
	sha_end(&c, hex);
	printf("%s sha256 %s %.0f MiB %.1f s %.1f MB/s%s\n", who, hex, (double)have / 1048576, d,
	       (double)have / 1048576 / d, have == total ? "" : " FAIL short");
	return have != total;
}

int main(int ac, char **av)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (ac < 3) return 2;
	long mib = atol(av[2]);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ac > 4 ? (uint16_t)atoi(av[4]) : 4310) };
	a.sin_addr.s_addr = ac > 3 ? inet_addr(av[3]) : htonl(INADDR_LOOPBACK);
	if (!strcmp(av[1], "loop")) {
		int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) { puts("FAIL bind"); return 1; }
		pid_t pid = fork();
		if (pid == 0) {
			int c = socket(AF_INET, SOCK_STREAM, 0);
			if (connect(c, (struct sockaddr *)&a, sizeof(a))) _exit(1);
			_exit(tx(c, mib, "tx"));
		}
		int s = accept(ls, NULL, NULL);
		int rc = rx(s, mib, "rx");
		int st;
		waitpid(pid, &st, 0);
		puts(rc || !WIFEXITED(st) || WEXITSTATUS(st) ? "FAIL" : "OK");
		return rc;
	}
	if (av[1][0] == 'l') {
		int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) { puts("FAIL bind"); return 1; }
		alarm(300);
		int s = accept(ls, NULL, NULL);
		alarm(0);
		return !strcmp(av[1], "lsend") ? tx(s, mib, "tx") : rx(s, mib, "rx");
	}
	int c = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(c, (struct sockaddr *)&a, sizeof(a))) { puts("FAIL connect"); return 1; }
	return !strcmp(av[1], "send") ? tx(c, mib, "tx") : rx(c, mib, "rx");
}
