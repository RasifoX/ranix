/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The siginfo of a standard signal names its sender, as on Linux:
 * - kill(self) to a handler: si_code SI_USER, si_pid getpid(), si_uid getuid();
 * - kill from a child, taken with sigwaitinfo: si_pid is the child;
 * - kill from a second thread: si_pid is the thread group id, not the tid;
 * - tgkill and tkill: si_code SI_TKILL, sender pid and uid;
 * - rt_sigqueueinfo with a standard signal: SI_QUEUE, its si_value;
 * - a standard signal sent twice while blocked keeps the first sender (the
 *   second one coalesces), and a stale record never leaks into a later
 *   signal posted by the kernel itself (SIGCHLD from a child's exit must
 *   not report this process as its sender);
 * - an RT signal sent with kill() carries the sender too.
 * Prints OK, or FAIL lines with the first case that differs. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int h_code, h_pid, h_uid, h_val, h_n;
static int fails;

static void on_sig(int sig, siginfo_t *si, void *uc)
{
	(void)sig; (void)uc;
	h_code = si->si_code;
	h_pid = si->si_pid;
	h_uid = (int)si->si_uid;
	h_val = si->si_value.sival_int;
	h_n++;
}

static void expect(const char *what, int code, int pid, int uid, int gcode, int gpid, int guid)
{
	if (gcode == code && gpid == pid && guid == uid) return;
	printf("FAIL %s: si_code %d si_pid %d si_uid %d, want %d %d %d\n",
	       what, gcode, gpid, guid, code, pid, uid);
	fails++;
}

static void *thread_kill(void *a)
{
	(void)a;
	kill(getpid(), SIGUSR2);
	return NULL;
}

/* Block sig, let a child send it, reap the child; returns the child's pid. */
static pid_t child_sends(int sig)
{
	pid_t c = fork();
	if (c == 0) { kill(getppid(), sig); _exit(0); }
	int st;
	waitpid(c, &st, 0);
	return c;
}

int main(void)
{
	pid_t me = getpid();
	int uid = (int)getuid();
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_sig;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGRTMIN + 3, &sa, NULL);

	kill(me, SIGUSR1);
	expect("kill self", SI_USER, me, uid, h_code, h_pid, h_uid);

	syscall(SYS_tgkill, me, (pid_t)syscall(SYS_gettid), SIGUSR1);
	expect("tgkill", SI_TKILL, me, uid, h_code, h_pid, h_uid);

	syscall(SYS_tkill, (pid_t)syscall(SYS_gettid), SIGUSR1);
	expect("tkill", SI_TKILL, me, uid, h_code, h_pid, h_uid);

	siginfo_t q;
	memset(&q, 0, sizeof(q));
	q.si_signo = SIGUSR1;
	q.si_code = SI_QUEUE;
	q.si_pid = me;            /* Linux trusts these; Ranix stamps the real ones */
	q.si_uid = (uid_t)uid;
	q.si_value.sival_int = 4321;
	h_val = 0;
	syscall(SYS_rt_sigqueueinfo, me, SIGUSR1, &q);
	expect("rt_sigqueueinfo", SI_QUEUE, me, uid, h_code, h_pid, h_uid);
	if (h_val != 4321) { printf("FAIL rt_sigqueueinfo: si_value %d, want 4321\n", h_val); fails++; }

	pthread_t th;
	h_pid = -1;
	pthread_create(&th, NULL, thread_kill, NULL);
	pthread_join(th, NULL);
	expect("kill from a thread", SI_USER, me, uid, h_code, h_pid, h_uid);

	kill(me, SIGRTMIN + 3);
	expect("kill rt", SI_USER, me, uid, h_code, h_pid, h_uid);

	/* blocked: the first of two senders is the one reported */
	sigset_t set, old;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGCHLD);
	sigprocmask(SIG_BLOCK, &set, &old);
	pid_t c1 = child_sends(SIGUSR1);
	child_sends(SIGUSR1);
	siginfo_t si;
	memset(&si, 0, sizeof(si));
	sigdelset(&set, SIGCHLD);
	int got = sigwaitinfo(&set, &si);
	if (got != SIGUSR1) { printf("FAIL sigwaitinfo returned %d\n", got); fails++; }
	expect("first of two senders", SI_USER, c1, uid, si.si_code, si.si_pid, (int)si.si_uid);

	/* SIGCHLD is pending from the children's exits (blocked above), posted by
	 * the kernel: a stale kill record must not show up on it. */
	kill(me, SIGCHLD);        /* coalesces into the pending SIGCHLD: no new record */
	memset(&si, 0, sizeof(si));
	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	got = sigwaitinfo(&set, &si);
	if (got != SIGCHLD || si.si_pid == me) {
		printf("FAIL SIGCHLD from exit reported as sig %d si_code %d si_pid %d\n",
		       got, si.si_code, si.si_pid);
		fails++;
	}
	sigprocmask(SIG_SETMASK, &old, NULL);

	if (fails) return 1;
	printf("OK\n");
	return 0;
}
