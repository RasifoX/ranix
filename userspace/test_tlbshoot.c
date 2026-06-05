/*
 * Cross-CPU TLB shootdown test.
 *
 * Reader threads pinned to distinct CPUs tight-loop reading a shared page,
 * pulling a valid translation into each CPU's TLB and bumping a per-thread
 * progress counter every read. The main thread (pinned to CPU 0) then
 * mprotect(PROT_NONE)s the page. x86 invlpg is CPU-local, so without a
 * cross-CPU shootdown a sibling keeps its stale readable entry and keeps
 * reading — its counter keeps climbing. With the shootdown every sibling's TLB
 * is flushed and its next read faults; Ranix kills the faulting thread (it does
 * not deliver a catchable in-thread SIGSEGV — see test_mmap), so the counter
 * freezes.
 *
 * PASS = after the barrier every reader's counter stops advancing (all readers
 * faulted -> TLBs were flushed cross-CPU). A counter that keeps climbing past
 * the barrier proves a stale TLB (shootdown missing/broken).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#ifdef __aarch64__
#define SYS_SCHED_SETAFFINITY 122
#else
#define SYS_SCHED_SETAFFINITY 203
#endif

#define NREADERS 3
#define VALUE_A  0xC0FFEE42u

static volatile uint32_t *page;
static volatile int barrier;             /* main flips after mprotect(PROT_NONE) */
static volatile unsigned long progress[NREADERS];

static void pin(int cpu)
{
	uint64_t mask = 1ULL << cpu;
	syscall(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), &mask);
}

static void *reader(void *arg)
{
	int id = (int)(long)arg;
	pin(id + 1);                         /* readers on CPUs 1..NREADERS */

	uint32_t acc = 0;
	for (int i = 0; i < 100000; i++) acc += page[0];   /* warm this CPU's TLB */
	while (!barrier) acc += page[0];

	/* PROT_NONE now. Each read faults once the TLB is flushed, killing this
	 * thread mid-loop and freezing progress[id]. A stale TLB keeps it climbing. */
	for (;;) {
		acc += page[0];
		progress[id]++;
	}
	return NULL;
}

int main(void)
{
	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) { puts("FAIL mmap"); return 1; }
	page[0] = VALUE_A;
	pin(0);

	pthread_t t[NREADERS];
	for (int i = 0; i < NREADERS; i++)
		pthread_create(&t[i], NULL, reader, (void *)(long)i);

	usleep(200000);                      /* let readers warm their TLBs */
	if (mprotect((void *)page, 4096, PROT_NONE) != 0) { puts("FAIL mprotect"); return 1; }
	__sync_synchronize();
	barrier = 1;

	usleep(100000);                      /* let post-barrier reads fault */
	unsigned long a[NREADERS];
	for (int i = 0; i < NREADERS; i++) a[i] = progress[i];
	usleep(300000);                      /* faulted threads are dead: frozen */

	for (int i = 0; i < NREADERS; i++) {
		if (progress[i] != a[i]) {
			printf("FAIL reader %d still reading (stale cross-CPU TLB)\n", i);
			return 1;
		}
	}
	puts("OK");
	return 0;
}
