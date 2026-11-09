/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* PTRACE_SETSIGINFO changes the siginfo a stopped tracee receives, as gdb
 * expects (it saves and restores siginfo around inferior calls):
 * - in a signal-delivery-stop for SIGUSR1, GETSIGINFO shows SI_USER; after
 *   SETSIGINFO with si_code SI_QUEUE, si_pid 4242 and si_value 0x1234,
 *   continuing with SIGUSR1 delivers exactly that;
 * - continuing with a different signal (SIGUSR2) delivers it as SI_USER from
 *   the tracer (Linux ptrace_signal);
 * - SETSIGINFO from a bad pointer is EFAULT, on a task not traced by the caller
 *   ESRCH;
 * - resuming with a number that is no signal (1000) is EIO, as on Linux: it
 *   would index the tracee's signal actions out of bounds. Asked in a
 *   syscall-stop, where a kernel without the check ignores the number.
 * Prints OK, or FAIL with the first case that differs. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int got_code[2], got_pid[2], got_val[2];

static void on_sig(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	int i = sig == SIGUSR2;
	got_code[i] = si->si_code;
	got_pid[i] = si->si_pid;
	got_val[i] = si->si_value.sival_int;
}

static int child(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_sig;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	if (ptrace(PTRACE_TRACEME, 0, 0, 0)) return 1;
	kill(getpid(), SIGUSR1);           /* stop 1: the tracer rewrites the siginfo */
	if (got_code[0] != SI_QUEUE) return 2;
	if (got_pid[0] != 4242) return 3;
	if (got_val[0] != 0x1234) return 4;
	kill(getpid(), SIGUSR1);           /* stop 2: the tracer delivers SIGUSR2 instead */
	if (got_code[1] != SI_USER) return 5;
	if (got_pid[1] != getppid()) return 6;
	return 0;
}

int main(void)
{
	pid_t c = fork();
	if (c == 0) _exit(child());
	int st, fails = 0;
	siginfo_t si;

	if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGUSR1) {
		printf("FAIL: first stop %x\n", st); return 1;
	}
	memset(&si, 0, sizeof(si));
	if (ptrace(PTRACE_GETSIGINFO, c, 0, &si)) { puts("FAIL: GETSIGINFO"); fails++; }
	if (si.si_signo != SIGUSR1 || si.si_code != SI_USER) {
		printf("FAIL: GETSIGINFO signo %d code %d\n", si.si_signo, si.si_code); fails++;
	}
	errno = 0;
	if (ptrace(PTRACE_SETSIGINFO, c, 0, (void *)8) != -1 || errno != EFAULT) {
		printf("FAIL: SETSIGINFO from a bad pointer: errno %d\n", errno); fails++;
	}
	si.si_code = SI_QUEUE;
	si.si_pid = 4242;
	si.si_value.sival_int = 0x1234;
	if (ptrace(PTRACE_SETSIGINFO, c, 0, &si)) { printf("FAIL: SETSIGINFO errno %d\n", errno); fails++; }
	siginfo_t back;
	memset(&back, 0, sizeof(back));
	ptrace(PTRACE_GETSIGINFO, c, 0, &back);
	if (back.si_code != SI_QUEUE || back.si_pid != 4242 || back.si_value.sival_int != 0x1234) {
		printf("FAIL: GETSIGINFO after SET: code %d pid %d\n", back.si_code, back.si_pid); fails++;
	}
	ptrace(PTRACE_CONT, c, 0, SIGUSR1);

	if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGUSR1) {
		printf("FAIL: second stop %x (child exit %d)\n", st, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}
	ptrace(PTRACE_CONT, c, 0, SIGUSR2);
	if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL: child saw the wrong siginfo: status %x (2-4 SETSIGINFO ignored, 5-6 replaced signal)\n", st);
		fails++;
	}

	/* a task this process does not trace */
	pid_t o = fork();
	if (o == 0) { pause(); _exit(0); }
	errno = 0;
	if (ptrace(PTRACE_SETSIGINFO, o, 0, &si) != -1 || errno != ESRCH) {
		printf("FAIL: SETSIGINFO on an untraced task: errno %d\n", errno); fails++;
	}
	kill(o, SIGKILL);
	waitpid(o, &st, 0);

	/* a signal number past NSIG */
	c = fork();
	if (c == 0) {
		if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(1);
		kill(getpid(), SIGUSR1);
		getpid();
		_exit(0);
	}
	if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) { printf("FAIL: badsig stop %x\n", st); return 1; }
	ptrace(PTRACE_SYSCALL, c, 0, 0);          /* drop SIGUSR1, stop at the next syscall */
	if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) { printf("FAIL: badsig syscall-stop %x\n", st); return 1; }
	errno = 0;
	if (ptrace(PTRACE_CONT, c, 0, 1000) != -1 || errno != EIO) {
		printf("FAIL: PTRACE_CONT with signal 1000: errno %d\n", errno); fails++;
	}
	ptrace(PTRACE_CONT, c, 0, 0);
	waitpid(c, &st, 0);
	if (!fails) puts("OK");
	return fails != 0;
}
