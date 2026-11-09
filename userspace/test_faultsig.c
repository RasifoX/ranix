/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A program's own fault reaches its handler: JITs, the Go runtime and sanitizers
 * turn SIGSEGV, SIGILL and SIGFPE into language-level errors. Without a handler
 * the process dies by that signal, whatever it blocks or ignores. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static sigjmp_buf jb;
static volatile int got_sig, got_code;
static void *volatile got_addr;

static void h(int s, siginfo_t *si, void *uc)
{
	(void)uc;
	got_sig = s; got_code = si->si_code; got_addr = si->si_addr;
	siglongjmp(jb, 1);
}

static void catch(int s)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = h;
	sa.sa_flags = SA_SIGINFO;
	sigaction(s, &sa, NULL);
}

#define WANT(c) do { if (!(c)) { printf("FAIL line %d sig=%d code=%d addr=%p\n", __LINE__, got_sig, got_code, got_addr); return 1; } } while (0)

static volatile int zero, seven = 7;

#if defined(__x86_64__)
#define UD() __asm__ volatile("ud2")
/* a handler resumes the program by moving its saved rip past the instruction */
static void skip(int s, siginfo_t *si, void *ucv)
{
	(void)s; (void)si;
	ucontext_t *uc = ucv;
	uc->uc_mcontext.gregs[REG_RIP] += 2;
	got_sig = -1;
}
#else
#define UD() __asm__ volatile(".inst 0x00000000")
#endif

static int dies(int how)
{
	pid_t p = fork();
	if (p == 0) {
		sigset_t set;
		sigemptyset(&set); sigaddset(&set, SIGSEGV); sigaddset(&set, SIGILL);
		if (how == 1) sigprocmask(SIG_BLOCK, &set, NULL);
		if (how == 2) signal(SIGSEGV, SIG_IGN);
		if (how == 3) { UD(); _exit(0); }
		*(volatile int *)0x10 = 1;
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}

int main(void)
{
	catch(SIGSEGV); catch(SIGILL); catch(SIGFPE);

	if (!sigsetjmp(jb, 1)) { *(volatile int *)0x10 = 1; WANT(0); }
	WANT(got_sig == SIGSEGV && got_code == SEGV_MAPERR && got_addr == (void *)0x10);

	char *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	got_sig = 0;
	if (!sigsetjmp(jb, 1)) { ro[8] = 1; WANT(0); }
	WANT(got_sig == SIGSEGV && got_code == SEGV_ACCERR && got_addr == ro + 8);

	got_sig = 0;
	if (!sigsetjmp(jb, 1)) { UD(); WANT(0); }
	WANT(got_sig == SIGILL);

#if defined(__x86_64__)              /* aarch64 integer division does not trap */
	got_sig = 0;
	if (!sigsetjmp(jb, 1)) { volatile int x = seven / zero; (void)x; WANT(0); }
	WANT(got_sig == SIGFPE && got_code == FPE_INTDIV);
#endif

	/* the signal mask came back with the longjmp, so it can fault again */
	got_sig = 0;
	if (!sigsetjmp(jb, 1)) { *(volatile int *)0x10 = 1; WANT(0); }
	WANT(got_sig == SIGSEGV);

#if defined(__x86_64__)              /* aarch64 passes no ucontext yet */
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = skip;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGILL, &sa, NULL);
	got_sig = 0;
	UD();
	WANT(got_sig == -1);
#endif

	sigaction(SIGSEGV, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);
	WANT(dies(0) == SIGSEGV);
	WANT(dies(1) == SIGSEGV);
	WANT(dies(2) == SIGSEGV);
	sigaction(SIGILL, &(struct sigaction){ .sa_handler = SIG_DFL }, NULL);
	WANT(dies(3) == SIGILL);
	puts("OK");
	return 0;
}
