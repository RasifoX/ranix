/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SA_SIGINFO handlers get a real ucontext_t on x86_64.
 *
 * The kernel passed NULL as the third handler argument. JavaScriptCore (Bun,
 * so Claude Code) suspends threads for GC with a signal whose handler reads
 * uc->uc_mcontext.gregs[REG_RSP]; with NULL it faulted at cr2=0xa0
 * (rip=0x383ae2c in claude 2.1.318). The gregs must be the interrupted
 * registers, in glibc/musl REG_* order. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__x86_64__)

static int fails;
static volatile int got, spin = 1;
static volatile uintptr_t seen_rsp, seen_rip, lo, hi;
static volatile int fpregs_ok, mask_ok;

static void on_usr1(int sig, siginfo_t *si, void *v)
{
	ucontext_t *uc = v;
	(void)si;
	if (!uc) { got = -1; return; }
	seen_rsp = uc->uc_mcontext.gregs[REG_RSP];
	seen_rip = uc->uc_mcontext.gregs[REG_RIP];
	fpregs_ok = uc->uc_mcontext.fpregs != NULL;
	mask_ok = !sigismember(&uc->uc_sigmask, SIGUSR1);
	got = sig;
}

static void *spinner(void *a)
{
	char here;
	(void)a;
	hi = (uintptr_t)&here + 4096; lo = (uintptr_t)&here - 256 * 1024;
	while (spin) __asm__ volatile("" ::: "memory");
	return NULL;
}

static volatile unsigned long g_rax = 1, g_r12, g_rdi, g_rdx;

static void on_usr2(int sig, siginfo_t *si, void *v)
{
	ucontext_t *uc = v;
	(void)sig; (void)si;
	if (!uc) return;
	g_rax = uc->uc_mcontext.gregs[REG_RAX];        /* tgkill's own return value */
	g_r12 = uc->uc_mcontext.gregs[REG_R12];
	g_rdi = uc->uc_mcontext.gregs[REG_RDI];
	g_rdx = uc->uc_mcontext.gregs[REG_RDX];
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_usr1; sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);

	/* 1: raise in this thread */
	char here;
	raise(SIGUSR1);
	if (got != SIGUSR1) { printf("FAIL raise: ucontext %s\n", got < 0 ? "NULL" : "missing"); fails++; }
	else if (seen_rsp < (uintptr_t)&here - 65536 || seen_rsp > (uintptr_t)&here + 65536 || !seen_rip || !fpregs_ok || !mask_ok) {
		printf("FAIL raise: rsp=%#lx rip=%#lx fp=%d mask=%d\n", (unsigned long)seen_rsp, (unsigned long)seen_rip, fpregs_ok, mask_ok); fails++;
	}

	/* 2: pthread_kill a busy thread, as JSC's GC suspend does */
	pthread_t t; got = 0;
	pthread_create(&t, NULL, spinner, NULL);
	while (!hi) usleep(1000);
	usleep(20000);
	pthread_kill(t, SIGUSR1);
	for (int i = 0; i < 2000 && !got; i++) usleep(1000);
	spin = 0; pthread_join(t, NULL);
	if (got != SIGUSR1) { printf("FAIL thread: ucontext %s\n", got < 0 ? "NULL" : "missing"); fails++; }
	else if (seen_rsp < lo || seen_rsp > hi) { printf("FAIL thread: rsp=%#lx not in [%#lx,%#lx]\n", (unsigned long)seen_rsp, (unsigned long)lo, (unsigned long)hi); fails++; }

	/* 3: the gregs hold the interrupted registers. The signal lands on the
	 * way out of tgkill: rax is its 0, rdi/rdx its arguments, r12 a marker. */
	sa.sa_sigaction = on_usr2;
	sigaction(SIGUSR2, &sa, NULL);
	unsigned long r;
	__asm__ volatile("movabsq $0x1234567890, %%r12\n\tsyscall"
	                 : "=a"(r)
	                 : "a"(234L), "D"((long)getpid()), "S"((long)gettid()), "d"((long)SIGUSR2)
	                 : "rcx", "r11", "r12", "memory");
	if (r != 0 || g_rax != 0 || g_r12 != 0x1234567890UL || g_rdi != (unsigned long)getpid() || g_rdx != SIGUSR2) {
		printf("FAIL gregs: rax=%#lx r12=%#lx rdi=%lu rdx=%lu\n", g_rax, g_r12, g_rdi, g_rdx); fails++;
	}

	if (!fails) printf("OK\n");
	return fails ? 1 : 0;
}
#else
int main(void) { puts("OK"); return 0; }   /* x86_64 frame only */
#endif
