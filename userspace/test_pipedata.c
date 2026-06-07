/* Pipe data-integrity under the dpkg-deb pipeline shape: a producer child writes
 * a known pseudo-random stream through a pipe to a consumer (this process),
 * which verifies every byte. Large transfers (> pipe buffer) so writes block and
 * resume across scheduler boundaries on SMP — the path a corrupted byte would
 * take. Also chains two pipes (producer | filter | consumer) to mirror dpkg's
 * 3-stage ar→decompress→tar topology. Prints OK only if every byte matches. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/wait.h>

#define TOTAL (256 * 1024)

static uint32_t rng(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s; }

static void produce(int fd, uint32_t seed)
{
	uint8_t buf[8192];
	uint32_t s = seed;
	size_t left = TOTAL;
	while (left) {
		size_t n = left < sizeof(buf) ? left : sizeof(buf);
		for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rng(&s);
		size_t off = 0;
		while (off < n) {
			ssize_t w = write(fd, buf + off, n - off);
			if (w <= 0) _exit(1);
			off += (size_t)w;
		}
		left -= n;
	}
	close(fd);
	_exit(0);
}

static int verify(int fd, uint32_t seed)
{
	uint8_t buf[8192];
	uint32_t s = seed;
	size_t got = 0;
	for (;;) {
		ssize_t r = read(fd, buf, sizeof(buf));
		if (r < 0) return 1;
		if (r == 0) break;
		for (ssize_t i = 0; i < r; i++)
			if (buf[i] != (uint8_t)rng(&s)) return 1;
		got += (size_t)r;
	}
	return got == TOTAL ? 0 : 1;
}

/* filter: read fd_in, write fd_out unchanged — the middle stage. Copies in odd
 * chunk sizes so its read/write boundaries don't line up with the producer's. */
static void filter(int fd_in, int fd_out)
{
	uint8_t buf[3001];
	for (;;) {
		ssize_t r = read(fd_in, buf, sizeof(buf));
		if (r < 0) _exit(1);
		if (r == 0) break;
		ssize_t off = 0;
		while (off < r) {
			ssize_t w = write(fd_out, buf + off, r - off);
			if (w <= 0) _exit(1);
			off += w;
		}
	}
	close(fd_out);
	_exit(0);
}

int main(void)
{
	int rounds = 20, fails = 0;
	for (int round = 0; round < rounds; round++) {
		uint32_t seed = 0x1234567u + round * 2654435761u;
		int p1[2], p2[2];
		if (pipe(p1) < 0 || pipe(p2) < 0) { fails++; continue; }

		pid_t pp = fork();
		if (pp == 0) { close(p1[0]); close(p2[0]); close(p2[1]); produce(p1[1], seed); }

		pid_t pf = fork();
		if (pf == 0) { close(p1[1]); close(p2[0]); filter(p1[0], p2[1]); }

		close(p1[0]); close(p1[1]); close(p2[1]);
		int rc = verify(p2[0], seed);
		close(p2[0]);
		waitpid(pp, NULL, 0);
		waitpid(pf, NULL, 0);
		if (rc) fails++;
	}
	printf(fails == 0 ? "OK\n" : "FAIL %d/%d\n", fails, rounds);
	return fails ? 1 : 0;
}
