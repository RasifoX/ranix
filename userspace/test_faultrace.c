/* Two threads reading the same untouched page: one maps it, the other arrives to
 * find it present, which is not a fault to die of. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE   4096L
#define PAGES  4096L
#define ROUNDS 40

static char *volatile region;
static pthread_barrier_t bar;
static volatile long sink;

static void *reader(void *arg)
{
	(void)arg;
	for (int r = 0; r < ROUNDS; r++) {
		pthread_barrier_wait(&bar);
		char *m = region;
		long s = 0;
		for (long i = 0; i < PAGES; i++) s += m[i * PAGE];
		sink += s;
		pthread_barrier_wait(&bar);
	}
	return NULL;
}

int main(void)
{
	pthread_t t[3];
	pthread_barrier_init(&bar, NULL, 4);
	for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, reader, NULL);
	for (int r = 0; r < ROUNDS; r++) {
		region = mmap(NULL, PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		pthread_barrier_wait(&bar);
		char *m = region;
		long s = 0;
		for (long i = 0; i < PAGES; i++) s += m[i * PAGE];
		sink += s;
		pthread_barrier_wait(&bar);
		munmap(region, PAGES * PAGE);
	}
	for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
	puts("OK");
	return 0;
}
