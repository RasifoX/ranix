/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* rt_sigqueueinfo / rt_tgsigqueueinfo let the sender fill in a siginfo. A
 * non-negative si_code (SI_USER, SI_KERNEL, CLD_*, SEGV_*) or SI_TKILL claims
 * that the kernel or kill() produced the signal, so another process must not
 * be able to send one: the receiver would trust a forged "child exited" or
 * "segfault at X". Linux returns EPERM for these unless the target is the
 * caller itself. A negative code like SI_QUEUE is fine; it is what sigqueue()
 * sends, with the sender's own pid and uid filled in as libc does.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as any user. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef SI_TKILL
#define SI_TKILL (-6)
#endif

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static long queue(pid_t pid, int sig, int code, int val)
{
	siginfo_t si;
	memset(&si, 0, sizeof(si));
	si.si_signo = sig;
	si.si_code = code;
	si.si_value.sival_int = val;
	si.si_pid = getpid();                            /* as libc sigqueue() fills them */
	si.si_uid = getuid();
	return syscall(SYS_rt_sigqueueinfo, pid, sig, &si);
}

static long tgqueue(pid_t pid, int sig, int code)
{
	siginfo_t si;
	memset(&si, 0, sizeof(si));
	si.si_signo = sig;
	si.si_code = code;
	return syscall(SYS_rt_tgsigqueueinfo, pid, pid, sig, &si);
}

int main(void)
{
	int sig = SIGRTMIN + 1;
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, sig);
	sigprocmask(SIG_BLOCK, &set, NULL);              /* the child inherits the block */

	int go[2], res[2];
	if (pipe(go) || pipe(res)) { puts("FAIL pipe"); return 0; }
	pid_t c = fork();
	if (c == 0) {
		/* report what arrives: code, pid, uid, value of the first queued signal */
		char b;
		close(go[1]);
		siginfo_t si;
		struct timespec ts = { 3, 0 };
		int got = sigtimedwait(&set, &si, &ts);
		int out[4] = { got == sig ? si.si_code : 999, (int)si.si_pid, (int)si.si_uid, si.si_value.sival_int };
		if (write(res[1], out, sizeof(out)) != (ssize_t)sizeof(out)) _exit(1);
		read(go[0], &b, 1);                         /* stay alive until told */
		_exit(0);
	}
	close(res[1]);

	errno = 0;
	CHECK(queue(c, sig, SI_USER, 1) == -1 && errno == EPERM, "forged SI_USER to another process allowed");
	errno = 0;
	CHECK(queue(c, sig, SI_KERNEL, 2) == -1 && errno == EPERM, "forged SI_KERNEL to another process allowed");
	errno = 0;
	CHECK(queue(c, sig, CLD_EXITED, 3) == -1 && errno == EPERM, "forged CLD_EXITED to another process allowed");
	errno = 0;
	CHECK(queue(c, sig, SI_TKILL, 4) == -1 && errno == EPERM, "forged SI_TKILL to another process allowed");
	errno = 0;
	CHECK(tgqueue(c, sig, SI_KERNEL) == -1 && errno == EPERM, "rt_tgsigqueueinfo: forged SI_KERNEL allowed");

	/* the legitimate case: SI_QUEUE arrives with its pid, uid and value */
	CHECK(queue(c, sig, SI_QUEUE, 77) == 0, "SI_QUEUE to own child refused");
	int in[4] = { 0 };
	CHECK(read(res[0], in, sizeof(in)) == (ssize_t)sizeof(in), "child report");
	CHECK(in[0] == SI_QUEUE, "the first signal the child got was not the SI_QUEUE one (a forged one got through)");
	if (in[0] == SI_QUEUE) {
		CHECK(in[1] == getpid(), "si_pid is not the sender");
		CHECK(in[2] == (int)getuid(), "si_uid is not the sender");
		CHECK(in[3] == 77, "si_value lost");
	}
	close(go[1]);
	waitpid(c, NULL, 0);

	/* to itself a process may use any code */
	CHECK(queue(getpid(), sig, SI_USER, 5) == 0, "SI_USER to self refused");
	siginfo_t si;
	struct timespec ts = { 1, 0 };
	CHECK(sigtimedwait(&set, &si, &ts) == sig, "self-queued signal not delivered");

	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
