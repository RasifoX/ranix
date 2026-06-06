/* getrandom() statistical sanity (not a crypto proof, but catches the old
 * splitmix-from-ticks regression): two reads differ, bytes are roughly uniform,
 * and reads from /dev/urandom also vary. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>

static int filled(unsigned char *b, size_t n)
{
	ssize_t got = 0;
	while ((size_t)got < n) {
		ssize_t r = getrandom(b + got, n - got, 0);
		if (r < 0) return -1;
		got += r;
	}
	return 0;
}

int main(void)
{
	unsigned char a[256], b[256];
	if (filled(a, sizeof(a)) || filled(b, sizeof(b))) { printf("fail getrandom\n"); return 0; }

	if (memcmp(a, b, sizeof(a)) == 0) { printf("fail identical\n"); return 0; }

	/* not all-zero / not constant */
	int allzero = 1, allsame = 1;
	for (size_t i = 0; i < sizeof(a); i++) {
		if (a[i]) allzero = 0;
		if (a[i] != a[0]) allsame = 0;
	}
	if (allzero || allsame) { printf("fail trivial\n"); return 0; }

	/* crude byte-value spread over 4096 samples: each of the 8 high-nibble-ish
	 * buckets should get a non-trivial share (splitmix-from-ticks clustered). */
	unsigned counts[8]; memset(counts, 0, sizeof(counts));
	unsigned char big[4096];
	if (filled(big, sizeof(big))) { printf("fail getrandom2\n"); return 0; }
	for (size_t i = 0; i < sizeof(big); i++) counts[big[i] >> 5]++;
	for (int i = 0; i < 8; i++)
		if (counts[i] < 256) { printf("fail bias bucket=%d n=%u\n", i, counts[i]); return 0; }

	/* GRND_NONBLOCK must not block and must succeed */
	unsigned char c[16];
	if (getrandom(c, sizeof(c), GRND_NONBLOCK) != (ssize_t)sizeof(c)) { printf("fail nonblock\n"); return 0; }

	/* /dev/urandom path varies too */
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		unsigned char u1[64], u2[64];
		if (read(fd, u1, sizeof(u1)) == (ssize_t)sizeof(u1) &&
		    read(fd, u2, sizeof(u2)) == (ssize_t)sizeof(u2) &&
		    memcmp(u1, u2, sizeof(u1)) == 0) { printf("fail urandom-same\n"); close(fd); return 0; }
		close(fd);
	}

	printf("OK\n");
	return 0;
}
