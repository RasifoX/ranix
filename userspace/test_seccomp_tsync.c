/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* seccomp(SECCOMP_SET_MODE_FILTER) flags as on Linux:
 * - TSYNC puts the filter on every thread of the group (and no_new_privs);
 *   without it only the caller is filtered;
 * - the probe seccomp(FILTER, TSYNC, NULL) is EFAULT (Chromium's "supported");
 * - a thread running a filter the caller's chain does not contain blocks
 *   TSYNC: the call returns that thread's TID (ESRCH with TSYNC_ESRCH) and
 *   nothing is installed, on the caller either;
 * - SPEC_ALLOW is accepted; an unknown flag is EINVAL;
 * - negative: another process (a fork) is never touched by TSYNC.
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define TSYNC       1u
#define SPEC_ALLOW  4u
#define TSYNC_ESRCH 16u

/* getpriority (any args) fails with errno e; everything else runs */
static long install(unsigned flags, unsigned e)
{
	static struct sock_filter f[8][4];
	static int n;
	struct sock_filter *p = f[n++ & 7];
	p[0] = (struct sock_filter){ 0x20, 0, 0, 0 };
	p[1] = (struct sock_filter){ 0x15, 0, 1, SYS_getpriority };
	p[2] = (struct sock_filter){ 0x06, 0, 0, 0x00050000u | e };
	p[3] = (struct sock_filter){ 0x06, 0, 0, 0x7fff0000u };
	struct sock_fprog fp = { 4, p };
	return syscall(SYS_seccomp, 1, flags, &fp);
}

static int prio_errno(void)
{
	errno = 0;
	return syscall(SYS_getpriority, 0, 0) == -1 ? errno : 0;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stage, seen_errno, seen_nnp;
static volatile pid_t tid;
static int own_filter;   /* the thread installs one of its own first */

static void wait_stage(int s)
{
	pthread_mutex_lock(&mu);
	while (stage < s) pthread_cond_wait(&cv, &mu);
	pthread_mutex_unlock(&mu);
}

static void set_stage(int s)
{
	pthread_mutex_lock(&mu);
	stage = s;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void *worker(void *a)
{
	(void)a;
	tid = syscall(SYS_gettid);
	if (own_filter) install(0, 33);
	set_stage(1);
	wait_stage(2);
	seen_errno = prio_errno();
	seen_nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
	set_stage(3);
	return NULL;
}

/* one scenario in a child process: a worker thread, then the caller installs */
static const char *scenario(unsigned flags, int with_own)
{
	pthread_t th;
	own_filter = with_own;
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return "no_new_privs";
	if (pthread_create(&th, NULL, worker, NULL)) return "pthread_create";
	wait_stage(1);
	long r = install(flags, 11);
	int rerr = errno;
	int mine = prio_errno();
	set_stage(2);
	wait_stage(3);
	pthread_join(th, NULL);
	if (!with_own && !(flags & TSYNC)) {
		if (r != 0 || mine != 11) return "plain install failed on the caller";
		if (seen_errno != 0) return "without TSYNC the sibling thread was filtered";
	} else if (!with_own) {
		if (r != 0) return "TSYNC install failed";
		if (mine != 11) return "TSYNC: caller not filtered";
		if (seen_errno != 11) return "TSYNC: sibling thread not filtered";
		if (seen_nnp != 1) return "TSYNC: sibling has no no_new_privs";
	} else if (flags & TSYNC_ESRCH) {
		if (r != -1 || rerr != ESRCH) return "TSYNC_ESRCH: blocked sync is not ESRCH";
		if (mine != 0) return "blocked TSYNC still installed on the caller";
		if (seen_errno != 33) return "blocked TSYNC changed the sibling's filter";
	} else {
		if (r != tid) return "blocked TSYNC did not return the sibling's TID";
		if (mine != 0) return "blocked TSYNC still installed on the caller";
		if (seen_errno != 33) return "blocked TSYNC changed the sibling's filter";
	}
	return NULL;
}

static const char *in_child(unsigned flags, int with_own)
{
	static char buf[96];
	int p[2];
	if (pipe(p)) return "pipe";
	pid_t c = fork();
	if (c == 0) {
		const char *e = scenario(flags, with_own);
		int n = snprintf(buf, sizeof buf, "%s", e ? e : "");
		(void)!write(p[1], buf, n);
		_exit(e ? 1 : 0);
	}
	close(p[1]);
	int n = read(p[0], buf, sizeof buf - 1);
	close(p[0]);
	buf[n > 0 ? n : 0] = 0;
	int st;
	waitpid(c, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return NULL;
	if (!buf[0]) snprintf(buf, sizeof buf, "child status %#x", st);
	return buf;
}

/* probes, bad flags, and a forked process left alone by TSYNC */
static const char *probes(void)
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return "no_new_privs";
	errno = 0;
	if (syscall(SYS_seccomp, 1, TSYNC, NULL) != -1 || errno != EFAULT)
		return "probe seccomp(FILTER, TSYNC, NULL) is not EFAULT";
	if (install(0x40, 12) != -1 || errno != EINVAL) return "unknown flag 0x40 is not EINVAL";
	if (syscall(SYS_seccomp, 0, TSYNC, NULL) != -1 || errno != EINVAL)
		return "STRICT with a flag is not EINVAL";
	if (prio_errno() != 0) return "a refused install filtered anyway";
	int go[2], back[2];
	if (pipe(go) || pipe(back)) return "pipe";
	pid_t c = fork();
	if (c == 0) {
		char b;
		(void)!read(go[0], &b, 1);
		b = (char)prio_errno();
		(void)!write(back[1], &b, 1);
		_exit(0);
	}
	if (install(TSYNC | SPEC_ALLOW, 13) != 0) return "TSYNC|SPEC_ALLOW refused";
	if (prio_errno() != 13) return "TSYNC|SPEC_ALLOW: caller not filtered";
	char b = 1;
	(void)!write(go[1], &b, 1);
	(void)!read(back[0], &b, 1);
	waitpid(c, NULL, 0);
	if (b != 0) return "TSYNC reached another process";
	return NULL;
}

int main(void)
{
	const char *e = in_child(0, 0);
	if (!e) e = in_child(TSYNC, 0);
	if (!e) e = in_child(TSYNC, 1);
	if (!e) e = in_child(TSYNC | TSYNC_ESRCH, 1);
	if (!e) {
		int p[2];
		static char buf[96];
		if (pipe(p)) return 1;
		pid_t c = fork();
		if (c == 0) {
			const char *r = probes();
			int n = snprintf(buf, sizeof buf, "%s", r ? r : "");
			(void)!write(p[1], buf, n);
			_exit(r ? 1 : 0);
		}
		close(p[1]);
		int n = read(p[0], buf, sizeof buf - 1);
		buf[n > 0 ? n : 0] = 0;
		int st;
		waitpid(c, &st, 0);
		if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) e = buf[0] ? buf : "probes child died";
	}
	if (e) { printf("FAIL: %s\n", e); return 1; }
	printf("OK\n");
	return 0;
}
