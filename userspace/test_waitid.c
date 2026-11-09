/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* waitid(2) reports a child's state change in a siginfo:
 *  - exit: CLD_EXITED, si_status = exit code, si_pid, si_uid = the child's uid;
 *  - kill: CLD_KILLED, si_status = the signal;
 *  - stop (WSTOPPED): CLD_STOPPED, si_status = the stop signal;
 *  - continue (WCONTINUED): CLD_CONTINUED, si_status = SIGCONT;
 *  - WNOWAIT leaves the child waitable (an exit and a stop alike), so a second
 *    waitid still finds it (Go's os/exec and process supervisors wait this way);
 *  - options without WEXITED/WSTOPPED/WCONTINUED is EINVAL, and wait4 refuses
 *    waitid's WNOWAIT.
 * Hang-safe: a 20 s alarm kills every child (a stopped one too) and prints
 * FAIL, so a broken WSTOPPED cannot leave a stopped task behind.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root (a child changes uid). */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define USER 4321

static int fails;
static volatile pid_t kids[8];
static volatile int nkids;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static void timeout_kill(int sig)
{
	(void)sig;
	for (int i = 0; i < nkids; i++) if (kids[i] > 0) kill(kids[i], SIGKILL);
	static const char m[] = "FAIL timed out (a waitid never returned)\n";
	if (write(1, m, sizeof(m) - 1) < 0) { }
	_exit(0);
}

static pid_t child(void)
{
	pid_t c = fork();
	if (c > 0 && nkids < 8) kids[nkids++] = c;
	return c;
}

static int wid(pid_t pid, siginfo_t *si, int opts)
{
	memset(si, 0, sizeof(*si));
	return waitid(P_PID, (id_t)pid, si, opts);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, timeout_kill);
	alarm(20);
	siginfo_t si;

	/* exit with a code, as another uid; WNOWAIT first, then the real reap */
	pid_t c = child();
	if (c == 0) { if (setuid(USER)) _exit(99); _exit(7); }
	CHECK(wid(c, &si, WEXITED | WNOWAIT) == 0, "waitid WEXITED|WNOWAIT");
	CHECK(si.si_pid == c && si.si_code == CLD_EXITED && si.si_status == 7, "WNOWAIT siginfo (pid/code/status)");
	CHECK(si.si_uid == USER, "si_uid is not the child's uid");
	CHECK(wid(c, &si, WEXITED) == 0, "second waitid after WNOWAIT: the child was already reaped");
	CHECK(si.si_pid == c && si.si_code == CLD_EXITED && si.si_status == 7, "exit siginfo (pid/code/status)");
	errno = 0;
	CHECK(wid(c, &si, WEXITED) == -1 && errno == ECHILD, "a reaped child is still waitable");

	/* killed by a signal */
	c = child();
	if (c == 0) { for (;;) pause(); }
	kill(c, SIGKILL);
	CHECK(wid(c, &si, WEXITED) == 0, "waitid on a killed child");
	CHECK(si.si_code == CLD_KILLED && si.si_status == SIGKILL, "killed siginfo (code/status)");

	/* stopped (twice with WNOWAIT), continued, then killed */
	c = child();
	if (c == 0) { raise(SIGSTOP); for (;;) pause(); }
	CHECK(wid(c, &si, WSTOPPED | WNOWAIT) == 0, "waitid WSTOPPED|WNOWAIT");
	CHECK(si.si_pid == c && si.si_code == CLD_STOPPED && si.si_status == SIGSTOP, "stopped siginfo (pid/code/status)");
	CHECK(wid(c, &si, WSTOPPED | WNOHANG) == 0 && si.si_pid == c && si.si_code == CLD_STOPPED,
	      "WNOWAIT consumed the stop report");
	errno = 0;
	CHECK(wid(c, &si, WSTOPPED | WNOHANG) == 0 && si.si_pid == 0, "a stop was reported twice");
	kill(c, SIGCONT);
	CHECK(wid(c, &si, WCONTINUED) == 0, "waitid WCONTINUED");
	CHECK(si.si_pid == c && si.si_code == CLD_CONTINUED && si.si_status == SIGCONT, "continued siginfo (pid/code/status)");
	kill(c, SIGKILL);
	CHECK(wid(c, &si, WEXITED) == 0 && si.si_code == CLD_KILLED, "kill after continue");

	/* WNOHANG with nothing to report: success and si_pid 0; bad options */
	c = child();
	if (c == 0) { for (;;) pause(); }
	si.si_pid = 12345;
	CHECK(waitid(P_PID, (id_t)c, &si, WEXITED | WNOHANG) == 0 && si.si_pid == 0, "WNOHANG with a running child");
	errno = 0;
	CHECK(wid(c, &si, WNOHANG) == -1 && errno == EINVAL, "waitid without WEXITED/WSTOPPED/WCONTINUED");
	int st;
	errno = 0;
	CHECK(waitpid(c, &st, WNOHANG | WNOWAIT) == -1 && errno == EINVAL, "wait4 accepted WNOWAIT");
	kill(c, SIGKILL);
	waitid(P_PID, (id_t)c, &si, WEXITED);

	alarm(0);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
