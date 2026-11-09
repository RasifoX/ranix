/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* mremap: without MREMAP_MAYMOVE nothing moves. musl's pthread_getattr_np probes
 * the main stack with it, and a moved stack page came back as zeroes. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fails;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL %s (errno %d)\n", w, errno); fails++; } } while (0)

#define PAGE 4096L

int main(void)
{
	char *m = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(m != MAP_FAILED, "mmap");
	memset(m, 7, 4 * PAGE);

	errno = 0;
	CHECK(mremap(m, PAGE, 2 * PAGE, 0) == MAP_FAILED && errno == ENOMEM, "grow into a mapped page");
	CHECK(m[0] == 7, "the page stayed");
	munmap(m + 2 * PAGE, 2 * PAGE);
	CHECK(mremap(m + 2 * PAGE, PAGE, 2 * PAGE, 0) == MAP_FAILED && errno == EFAULT, "unmapped range");
	CHECK(mremap(m, 2 * PAGE, 3 * PAGE, 0) == m, "grow in place");
	m[2 * PAGE] = 1;
	CHECK(m[PAGE] == 7, "contents kept");
	CHECK(mremap(m, 3 * PAGE, PAGE, 0) == m, "shrink");

	m[0] = 9;
	char *g = mremap(m, PAGE, 1024 * PAGE, MREMAP_MAYMOVE);
	CHECK(g != MAP_FAILED && g[0] == 9, "move");
	munmap(g, 1024 * PAGE);

	/* the real caller: its frames sit in the pages it walks */
	pthread_attr_t a;
	void *addr; size_t size;
	volatile char marker = 42;
	CHECK(pthread_getattr_np(pthread_self(), &a) == 0, "pthread_getattr_np");
	CHECK(pthread_attr_getstack(&a, &addr, &size) == 0 && size >= 64 * PAGE, "stack size");
	CHECK(marker == 42, "the stack survives the probe");

	if (fails) { printf("FAILED %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
