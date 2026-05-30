/*
 * COW double-free regression test.
 *
 * A page that is COW-shared across two address spaces (parent + forked
 * guardian) must keep its refcount honest even when several threads in one
 * space fault-write it at the same instant. A buggy fault handler runs the
 * copy-and-free twice for the single shared PTE, dropping refcount to zero and
 * recycling a page the guardian still maps. The guardian holds its snapshot
 * read-only and verifies it never changes underfoot for the whole race window
 * (coordinated by a pipe: parent closes the write end when it is done, the
 * guardian sees EOF and stops). Post-race fork churn reallocates any wrongly
 * freed page so the guardian observes the foreign content.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>

#define NPAGES   24
#define WPP      (4096 / (int)sizeof(uint32_t))
#define NTHREAD  4
#define ITERS    4

#define PARENT_SALT  0xA5A5u
#define RACER_SALT   0x5A5Au

static uint32_t buf[NPAGES][WPP];

static uint32_t cell(unsigned salt, int pg, int w)
{
	return (uint32_t)(((unsigned)pg << 20) ^ ((unsigned)w << 4) ^ salt);
}

static void fill(unsigned salt)
{
	for (int p = 0; p < NPAGES; p++)
		for (int w = 0; w < WPP; w++)
			buf[p][w] = cell(salt, p, w);
}

static volatile int go;

static void *racer(void *arg)
{
	(void)arg;
	while (!go) { }
	for (int p = 0; p < NPAGES; p++)
		for (int w = 0; w < WPP; w++)
			buf[p][w] = cell(RACER_SALT, p, w);
	return NULL;
}

int main(void)
{
	for (int it = 0; it < ITERS; it++) {
		fill(PARENT_SALT);

		int pfd[2];
		if (pipe(pfd) < 0) { puts("FAIL pipe"); return 1; }

		pid_t g = fork();
		if (g == 0) {
			close(pfd[1]);
			fcntl(pfd[0], F_SETFL, O_NONBLOCK);
			/* Verify continuously until the parent closes its write end. */
			for (;;) {
				char c;
				if (read(pfd[0], &c, 1) == 0)
					_exit(0);
				for (int p = 0; p < NPAGES; p++)
					for (int w = 0; w < WPP; w++)
						if (buf[p][w] != cell(PARENT_SALT, p, w)) {
							printf("CORRUPT it=%d pg=%d w=%d got=%x want=%x\n",
							       it, p, w, buf[p][w], cell(PARENT_SALT, p, w));
							fflush(stdout);
							_exit(2);
						}
			}
		}
		close(pfd[0]);

		go = 0;
		pthread_t t[NTHREAD];
		for (int i = 0; i < NTHREAD; i++)
			pthread_create(&t[i], NULL, racer, NULL);
		go = 1;
		for (int i = 0; i < NTHREAD; i++)
			pthread_join(t[i], NULL);

		/* Reallocate any wrongly freed page while the guardian still watches. */
		for (int c = 0; c < 4; c++) {
			pid_t k = fork();
			if (k == 0) { fill(RACER_SALT); _exit(0); }
			int s; waitpid(k, &s, 0);
		}

		close(pfd[1]);       /* signal guardian: race window over */

		int st;
		waitpid(g, &st, 0);
		if (!WIFEXITED(st)) {
			printf("FAIL it=%d guardian killed st=%x\n", it, st);
			return 1;
		}
		if (WEXITSTATUS(st) != 0) {
			printf("FAIL it=%d\n", it);
			return 1;
		}
	}

	puts("OK");
	return 0;
}
