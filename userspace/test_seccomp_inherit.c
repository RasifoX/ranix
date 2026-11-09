/* seccomp filters follow fork, threads and exec, stack instead of replacing each
 * other, need no_new_privs or CAP_SYS_ADMIN, and are freed with their last task. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define RET_ALLOW 0x7fff0000u
#define RET_KILL  0u
#define RET_ERRNO(e) (0x00050000u | (e))
#define RET 0x06

static long install(struct sock_filter *f, unsigned short n)
{
	struct sock_fprog p = { n, f };
	return syscall(SYS_seccomp, 1, 0, &p);
}

/* act for syscall nr, allow the rest */
static long filter(int nr, unsigned act)
{
	struct sock_filter f[] = {
		{ 0x20, 0, 0, 0 },
		{ 0x15, 0, 1, (unsigned)nr },
		{ RET, 0, 0, act },
		{ RET, 0, 0, RET_ALLOW },
	};
	return install(f, 4);
}

static int denied(int nr, int e)
{
	return syscall(nr) == -1 && errno == e;
}

static void *thread_check(void *a)
{
	(void)a;
	return (void *)(long)denied(SYS_getppid, EPERM);
}

/* fork a child running fn, return its wait status */
static int run(int (*fn)(void))
{
	pid_t c = fork();
	if (c == 0) _exit(fn());
	int st = -1;
	waitpid(c, &st, 0);
	return st;
}

static int forked_check(void) { return denied(SYS_getppid, EPERM) ? 0 : 1; }

static const char *self;

static int inherit(void)
{
	if (filter(SYS_getppid, RET_ERRNO(EPERM))) return 2;
	int st = run(forked_check);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) return 3;
	pthread_t t; void *r = 0;
	if (pthread_create(&t, 0, thread_check, 0) || pthread_join(t, &r) || !r) return 4;
	/* a later filter that allows everything must not lift the first */
	if (filter(SYS_getuid, RET_ALLOW)) return 5;
	if (!denied(SYS_getppid, EPERM)) return 6;
	if (filter(SYS_getegid, RET_ERRNO(EACCES))) return 7;
	if (!denied(SYS_getppid, EPERM) || !denied(SYS_getegid, EACCES)) return 8;
	execl(self, self, "exec-check", (char *)0);
	return 9;
}

/* KILL in an older filter beats ERRNO in a newer one */
static int precedence(void)
{
	if (filter(SYS_getegid, RET_KILL) || filter(SYS_getegid, RET_ERRNO(EPERM))) return 2;
	syscall(SYS_getegid);
	return 3;
}

static int need_nnp(void)
{
	if (setuid(1000)) return 2;
	if (filter(SYS_getegid, RET_ALLOW) != -1 || errno != EACCES) return 3;
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 4;
	if (filter(SYS_getegid, RET_ALLOW)) return 5;
	return 0;
}

static struct sock_filter big[4096];

static int big_child(void)
{
	if (install(big, 4096)) return 2;
	return run(forked_check) == -1;
}

/* an unbounded chain would let a task slow every later syscall forever */
static int chain_limit(void)
{
	for (int i = 0; i < 16; i++)
		if (install(big, 4096)) return errno == ENOMEM ? 0 : 2;
	return 3;
}

static long mem_kb(void)
{
	char b[4096];
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;
	long v = -1;
	while (fgets(b, sizeof(b), f))
		if (sscanf(b, "MemFree: %ld", &v) == 1) break;
	fclose(f);
	return v;
}

/* 200 * 32 KiB of filters would stay allocated if exit did not free them */
static int no_leak(void)
{
	for (int i = 0; i < 20; i++) run(big_child);
	long before = mem_kb();
	for (int i = 0; i < 200; i++) run(big_child);
	long after = mem_kb();
	return before >= 0 && after >= 0 && before - after < 3000 ? 0 : 1;
}

static int strict_in_pidns(void)
{
	long c = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (c == 0) {
		syscall(SYS_seccomp, 0, 0, 0);
		syscall(SYS_getegid);
		syscall(SYS_exit, 0);
	}
	int st = -1;
	waitpid((pid_t)c, &st, 0);
	return WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL ? 0 : 1;
}

/* Jump and load offsets outside the program must end in KILL, not a kernel fault
 * or an endless loop. */
static int bad_jump(void)
{
	struct sock_filter f[] = { { 0x05, 0, 0, 0xffffffffu }, { RET, 0, 0, RET_ALLOW } };
	if (install(f, 2)) return 0;
	syscall(SYS_getegid);
	return 3;
}

static int bad_load(void)
{
	struct sock_filter f[] = { { 0x20, 0, 0, 0xffffffffu }, { RET, 0, 0, RET_ALLOW } };
	if (install(f, 2)) return 0;
	syscall(SYS_getegid);
	return 3;
}

static int killed_or_refused(int st)
{
	return (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS) || (WIFEXITED(st) && !WEXITSTATUS(st));
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "exec-check"))
		_exit(denied(SYS_getppid, EPERM) && denied(SYS_getegid, EACCES) ? 0 : 20);
	self = argv[0];
	for (unsigned i = 0; i < 4096; i++) big[i] = (struct sock_filter){ RET, 0, 0, RET_ALLOW };

	int st = run(inherit);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL inherit %x\n", st); return 1; }
	st = run(precedence);
	/* RET_KILL is KILL_THREAD; for a single thread Linux kills the process by SIGSYS */
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSYS) { printf("FAIL precedence %x\n", st); return 1; }
	st = run(need_nnp);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL nnp %x\n", st); return 1; }
	st = run(chain_limit);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL chain %x\n", st); return 1; }
	st = run(no_leak);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL leak %x\n", st); return 1; }
	if (strict_in_pidns()) { puts("FAIL pidns-kill"); return 1; }
	if (!killed_or_refused(run(bad_load))) { puts("FAIL bad-load"); return 1; }
	if (!killed_or_refused(run(bad_jump))) { puts("FAIL bad-jump"); return 1; }
	puts("OK");
	return 0;
}
