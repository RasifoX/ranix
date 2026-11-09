/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Threads share one address space, so they share one break. With a break kept per
 * task, a thread that starts before the heap grows "grows" it again from its old
 * value and maps zero pages over memory the program is using. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PG 4096UL
static pthread_barrier_t b;
static uintptr_t seen, grown;

static uintptr_t kbrk(uintptr_t a) { return (uintptr_t)syscall(SYS_brk, a); }

static void *th(void *a)
{
	(void)a;
	pthread_barrier_wait(&b);          /* main has grown the heap by now */
	pthread_barrier_wait(&b);
	seen = kbrk(0);
	grown = kbrk(seen + PG);
	pthread_barrier_wait(&b);
	return NULL;
}

int main(void)
{
	pthread_t t;
	pthread_barrier_init(&b, NULL, 2);
	uintptr_t base = kbrk(0);
	if (pthread_create(&t, NULL, th, NULL)) { puts("FAIL create"); return 1; }
	pthread_barrier_wait(&b);
	uintptr_t top = kbrk(base + 2 * PG);
	if (top != base + 2 * PG) { puts("FAIL grow"); return 1; }
	memset((void *)base, 0x5a, 2 * PG);
	pthread_barrier_wait(&b);
	pthread_barrier_wait(&b);
	pthread_join(t, NULL);
	if (seen != top) { printf("FAIL thread saw %lx want %lx\n", (long)seen, (long)top); return 1; }
	if (grown != top + PG || kbrk(0) != grown) { puts("FAIL shared growth"); return 1; }
	for (unsigned long i = 0; i < 2 * PG; i++)
		if (((unsigned char *)base)[i] != 0x5a) { printf("FAIL heap zeroed at %lu\n", i); return 1; }
	*(volatile char *)(grown - 1) = 1;
	puts("OK");
	return 0;
}
