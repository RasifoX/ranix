/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* What strace and gdb are built on: syscall stops told apart from traps, events for
 * fork and exec, every signal stopping first and the tracer deciding its fate, the
 * register sets, and attaching to a task that is not a child. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define WANT(c) do { if (!(c)) { printf("FAIL line %d errno=%d st=%x\n", __LINE__, errno, st); return 1; } } while (0)

static volatile int caught;
static void h(int s) { caught = s; }
static int st;

static int t_syscalls(void)
{
	pid_t c = fork();
	if (c == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		syscall(SYS_getppid);
		_exit(0);
	}
	WANT(waitpid(c, &st, 0) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
	WANT(ptrace(PTRACE_SETOPTIONS, c, 0, PTRACE_O_TRACESYSGOOD) == 0);
	int stops = 0, seen = 0;
	for (;;) {
		WANT(ptrace(PTRACE_SYSCALL, c, 0, 0) == 0);
		WANT(waitpid(-1, &st, __WALL) == c);
		if (WIFEXITED(st)) break;
		WANT(WIFSTOPPED(st) && WSTOPSIG(st) == (SIGTRAP | 0x80));
		struct user_regs_struct r;
		struct iovec iov = { &r, sizeof r };
		WANT(ptrace(PTRACE_GETREGSET, c, NT_PRSTATUS, &iov) == 0 && iov.iov_len == sizeof r);
		if (r.orig_rax == SYS_getppid) {
			seen++;
			long pk = ptrace(PTRACE_PEEKUSER, c, offsetof(struct user, regs.orig_rax), 0);
			WANT(pk == SYS_getppid);
		}
		stops++;
	}
	WANT(seen == 2 && stops >= 2);
	return 0;
}

static int t_signals(void)
{
	pid_t c = fork();
	if (c == 0) {
		signal(SIGUSR1, h);
		signal(SIGUSR2, h);
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		raise(SIGUSR1);          /* dropped by the tracer */
		if (caught) _exit(10);
		raise(SIGUSR2);          /* let through */
		if (caught != SIGUSR2) _exit(11);
		*(volatile int *)0x10 = 1;   /* gdb sees this before the process dies */
		_exit(12);
	}
	WANT(waitpid(c, &st, 0) == c && WSTOPSIG(st) == SIGSTOP);
	WANT(ptrace(PTRACE_CONT, c, 0, 0) == 0);
	WANT(waitpid(c, &st, 0) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR1);
	siginfo_t si;
	WANT(ptrace(PTRACE_GETSIGINFO, c, 0, &si) == 0 && si.si_signo == SIGUSR1);
	WANT(ptrace(PTRACE_CONT, c, 0, 0) == 0);
	WANT(waitpid(c, &st, 0) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR2);
	WANT(ptrace(PTRACE_CONT, c, 0, SIGUSR2) == 0);
	WANT(waitpid(c, &st, 0) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSEGV);
	WANT(ptrace(PTRACE_GETSIGINFO, c, 0, &si) == 0 && si.si_addr == (void *)0x10);
	WANT(ptrace(PTRACE_CONT, c, 0, SIGSEGV) == 0);
	WANT(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
	return 0;
}

static int t_events(void)
{
	pid_t c = fork();
	if (c == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		pid_t g = fork();
		if (g == 0) _exit(0);
		waitpid(g, NULL, 0);
		execl("/bin/true", "true", (char *)NULL);
		_exit(9);
	}
	WANT(waitpid(c, &st, 0) == c && WSTOPSIG(st) == SIGSTOP);
	WANT(ptrace(PTRACE_SETOPTIONS, c, 0, PTRACE_O_TRACEFORK | PTRACE_O_TRACEEXEC) == 0);
	WANT(ptrace(PTRACE_CONT, c, 0, 0) == 0);
	int forked = 0, execd = 0, gdone = 0;
	unsigned long g = 0;
	for (;;) {
		pid_t w = waitpid(-1, &st, __WALL);
		WANT(w > 0);
		if (w == c && (WIFEXITED(st) || WIFSIGNALED(st))) break;
		if (WIFEXITED(st)) { gdone = 1; continue; }
		WANT(WIFSTOPPED(st));
		int ev = st >> 16;
		if (ev == PTRACE_EVENT_FORK) {
			forked++;
			WANT(ptrace(PTRACE_GETEVENTMSG, c, 0, &g) == 0 && g > 0);
		} else if (ev == PTRACE_EVENT_EXEC) {
			execd++;
		}
		int sig = (WSTOPSIG(st) == SIGCHLD) ? SIGCHLD : 0;
		WANT(ptrace(PTRACE_CONT, w, 0, sig) == 0);
	}
	WANT(forked == 1 && execd == 1 && gdone == 1);
	return 0;
}

/* The target is the tracer's sibling: wait must work without being its parent */
static int t_attach(void)
{
	int ready[2], res[2];
	pipe(ready); pipe(res);
	pid_t target = fork();
	if (target == 0) {
		write(ready[1], "x", 1);
		for (;;) pause();
	}
	char b;
	read(ready[0], &b, 1);
	pid_t tracer = fork();
	if (tracer == 0) {
		int ok = ptrace(PTRACE_ATTACH, target, 0, 0) == 0 &&
		         waitpid(target, &st, __WALL) == target && WIFSTOPPED(st) &&
		         ptrace(PTRACE_DETACH, target, 0, 0) == 0;
		_exit(ok ? 0 : 1);
	}
	WANT(waitpid(tracer, &st, 0) == tracer && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	kill(target, SIGKILL);
	WANT(waitpid(target, &st, 0) == target && WIFSIGNALED(st));
	return 0;
}

int main(void)
{
	alarm(25);
	if (t_syscalls() || t_signals() || t_events() || t_attach()) return 1;
	puts("OK");
	return 0;
}
