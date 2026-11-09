/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Cross-CPU TLB shootdown test.
 *
 * Reader threads pinned to distinct CPUs tight-loop reading a shared page,
 * pulling a valid translation into each CPU's TLB. The main thread (pinned to
 * CPU 0) then drops the page with MADV_DONTNEED. x86 invlpg is CPU-local, so
 * without a cross-CPU shootdown a sibling keeps its stale entry and keeps reading
 * the old value from a page that may already belong to someone else. With the
 * shootdown its next read demand-faults a zero page.
 *
 * PASS = every reader sees the value change. A fault cannot be the signal here:
 * it would end the whole process, not the one thread.
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
static volatile int warmed[NREADERS];
static volatile int saw_change[NREADERS];

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
	warmed[id] = 1;
	while (page[0] == VALUE_A) acc++;
	saw_change[id] = 1;
	return (void *)(long)acc;
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

	for (int i = 0; i < NREADERS; i++)
		while (!warmed[i]) usleep(1000);
	if (madvise((void *)page, 4096, MADV_DONTNEED) != 0) { puts("FAIL madvise"); return 1; }

	usleep(400000);
	for (int i = 0; i < NREADERS; i++) {
		if (!saw_change[i]) {
			printf("FAIL reader %d still reading (stale cross-CPU TLB)\n", i);
			return 1;
		}
	}
	for (int i = 0; i < NREADERS; i++) pthread_join(t[i], NULL);
	puts("OK");
	return 0;
}
