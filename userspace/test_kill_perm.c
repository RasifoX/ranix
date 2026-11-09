/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* kill and its relatives need a uid match or CAP_KILL from the initial user
 * namespace, kill(-1) stops at the caller's pid namespace, and setpgid reaches
 * only the caller and its children in the same session. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define UID 64000   /* owns nothing else on the machine: this uid gets kill(-1) */

/* returns once the child runs as uid */
static pid_t sleeper(uid_t uid)
{
	int p[2]; char b;
	if (pipe(p)) return -1;
	pid_t c = fork();
	if (c == 0) {
		if (uid && setuid(uid)) _exit(1);
		prctl(PR_SET_PDEATHSIG, SIGKILL);   /* must not outlive a failed run */
		write(p[1], "x", 1);
		for (;;) pause();
	}
	read(p[0], &b, 1);
	close(p[0]); close(p[1]);
	return c;
}

static int eperm(long r) { return r == -1 && errno == EPERM; }

static int attacker(pid_t v, pid_t peer, int userns)
{
	siginfo_t si;
	memset(&si, 0, sizeof(si));
	int pidfd = syscall(SYS_pidfd_open, v, 0);
	if (pidfd < 0) return 2;
	if (setuid(UID)) return 3;
	if (userns && unshare(CLONE_NEWUSER)) return 4;
	if (!eperm(kill(v, SIGTERM))) return 5;
	if (!eperm(kill(v, 0))) return 6;
	if (!eperm(kill(-v, SIGTERM))) return 7;
	if (!eperm(syscall(SYS_tkill, v, SIGTERM))) return 8;
	if (!eperm(syscall(SYS_tgkill, v, v, SIGTERM))) return 9;
	if (!eperm(syscall(SYS_rt_sigqueueinfo, v, SIGTERM, &si))) return 10;
	if (!eperm(syscall(SYS_rt_tgsigqueueinfo, v, v, SIGTERM, &si))) return 11;
	if (!eperm(syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, 0, 0))) return 12;
	if (kill(v, SIGCONT)) return 13;                /* same session */
	if (setpgid(v, 0) != -1 || errno != ESRCH) return 14;   /* not our child */
	kill(-1, SIGTERM);
	if (kill(peer, 0)) return 15;                   /* same uid: allowed */
	return 0;
}

static int run_attacker(pid_t v, int userns)
{
	pid_t peer = sleeper(UID);
	pid_t a = fork();
	if (a == 0) _exit(attacker(v, peer, userns));
	int st = -1;
	waitpid(a, &st, 0);
	kill(peer, SIGKILL);
	waitpid(peer, 0, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static int alive(pid_t v) { return waitpid(v, 0, WNOHANG) == 0; }

static int setpgid_rules(void)
{
	int p[2]; char b;
	if (pipe(p)) return 30;
	pid_t other = fork();
	if (other == 0) {
		setsid();
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		write(p[1], "x", 1);
		for (;;) pause();
	}
	read(p[0], &b, 1);
	pid_t same = sleeper(0);
	int r = 0;
	if (!eperm(setpgid(other, other))) r = 31;          /* other session */
	else if (!eperm(setpgid(same, other))) r = 32;      /* group in another session */
	else if (!eperm(setpgid(same, 0x7000))) r = 33;     /* no such group */
	else if (setpgid(same, same) || getpgid(same) != same) r = 34;
	kill(other, SIGKILL); kill(same, SIGKILL);
	waitpid(other, 0, 0); waitpid(same, 0, 0);
	return r;
}

static int broadcast_in_ns(void)
{
	pid_t in = sleeper(0);
	if (kill(-1, SIGKILL)) return 40;
	int st = -1;
	waitpid(in, &st, 0);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) return 41;
	if (kill(-1, SIGKILL) != -1 || errno != ESRCH) return 42;   /* nothing left */
	return 0;
}

int main(void)
{
	pid_t v = sleeper(0);
	if (setpgid(v, v)) { puts("FAIL setpgid"); return 1; }
	int r = run_attacker(v, 0);
	if (!r) r = run_attacker(v, 1);
	if (r) { printf("FAIL attacker %d\n", r); return 1; }
	if (!alive(v)) { puts("FAIL victim-died"); return 1; }

	if ((r = setpgid_rules())) { printf("FAIL setpgid %d\n", r); return 1; }

	long c = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (c == 0) _exit(broadcast_in_ns());
	int st = -1;
	waitpid((pid_t)c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL broadcast %x\n", st); return 1; }
	if (!alive(v)) { puts("FAIL ns-broadcast-escaped"); return 1; }

	kill(v, SIGKILL);
	waitpid(v, 0, 0);
	puts("OK");
	return 0;
}
