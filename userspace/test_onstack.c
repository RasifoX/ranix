/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SA_ONSTACK runs the handler on the alternate stack: a stack overflow handler, and
 * QEMU's coroutine setup, depend on it. Without the flag the handler stays put. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static char *alt;
#define ALT 65536
static volatile int on, oldflags, eperm;

static void h(int s)
{
	(void)s;
	char here;
	on = &here >= alt && &here < alt + ALT;
	stack_t st, nw = { .ss_sp = alt, .ss_size = ALT };
	sigaltstack(NULL, &st);
	oldflags = st.ss_flags;
	eperm = sigaltstack(&nw, NULL);
}

static void *thr(void *a)
{
	stack_t st;
	sigaltstack(NULL, &st);
	return (void *)(intptr_t)st.ss_flags;
}

int main(void)
{
	alt = mmap(NULL, ALT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	stack_t ss = { .ss_sp = alt, .ss_size = ALT };
	if (sigaltstack(&ss, NULL)) { printf("FAIL set\n"); return 1; }
	struct sigaction a;
	memset(&a, 0, sizeof(a));
	a.sa_handler = h;
	a.sa_flags = SA_ONSTACK;
	sigaction(SIGUSR1, &a, NULL);
	raise(SIGUSR1);
	if (!on) { printf("FAIL not on the alternate stack\n"); return 1; }
	if (!(oldflags & SS_ONSTACK)) { printf("FAIL SS_ONSTACK not reported %d\n", oldflags); return 1; }
	if (!eperm) { printf("FAIL changed the stack while on it\n"); return 1; }
	a.sa_flags = 0;
	sigaction(SIGUSR1, &a, NULL);
	raise(SIGUSR1);
	if (on) { printf("FAIL no SA_ONSTACK yet on the alternate stack\n"); return 1; }
	pthread_t t; void *r;
	pthread_create(&t, NULL, thr, NULL);
	pthread_join(t, &r);
	if (!((intptr_t)r & SS_DISABLE)) { printf("FAIL thread inherited the stack\n"); return 1; }
	printf("OK\n");
	return 0;
}
