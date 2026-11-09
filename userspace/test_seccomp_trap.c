/* seccomp actions as on Linux, the ones Chromium's sandbox is built on:
 * - RET_TRAP: the syscall does not run; SIGSYS arrives with si_code SYS_SECCOMP,
 *   si_errno the filter's data, si_syscall, si_arch and si_call_addr just past
 *   the syscall instruction; the context holds rax = the syscall number (Linux
 *   rolls the call back); what the handler leaves in rax is the result.
 *   Blocked or ignored, SIGSYS kills (it is forced).
 * - RET_TRACE with no tracer: ENOSYS. RET_LOG: runs. RET_ERRNO(0): returns 0.
 * - The most restrictive action of all filters wins (TRAP over ERRNO).
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define LD_NR    { 0x20, 0, 0, 0 }
#define LD_ARCH  { 0x20, 0, 0, 4 }
#define IF(nr, ret) { 0x15, 0, 1, (unsigned)(nr) }, { 0x06, 0, 0, (ret) }
#define RET(r)   { 0x06, 0, 0, (r) }

#define TRAP     0x00030000u
#define ERRNO    0x00050000u
#define TRACE    0x7ff00000u
#define LOG      0x7ffc0000u
#define ALLOW    0x7fff0000u

static volatile int got, code, err, nr;
static volatile unsigned arch;
static volatile uintptr_t call, rip;
static volatile long rax;

static void on_sys(int sig, siginfo_t *si, void *uc)
{
	got = sig;
	code = si->si_code;
	err = si->si_errno;
	nr = si->si_syscall;
	arch = si->si_arch;
	call = (uintptr_t)si->si_call_addr;
	/* as Linux (syscall_rollback): the context holds the call as it was made,
	 * rax the syscall number and rip si_call_addr (Chromium checks both) */
	rax = ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RAX];
	rip = ((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
	((ucontext_t *)uc)->uc_mcontext.gregs[REG_RAX] = 4242;
}

static int install(struct sock_filter *f, unsigned short n)
{
	struct sock_fprog p = { n, f };
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
	return (int)syscall(SYS_seccomp, 1, 0, &p);
}

static const char *trap_child(void)
{
	static char path[64];
	snprintf(path, sizeof path, "/tmp/sctrap.%d", (int)getpid());
	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return "create";
	close(fd);
	struct sock_filter f[] = {
		LD_ARCH, { 0x15, 1, 0, 0xc000003eu }, RET(0),   /* not x86_64: kill */
		LD_NR,
		IF(SYS_getppid, TRAP | 0x42),
		IF(SYS_unlink, TRAP | 7),
		IF(SYS_getpgrp, TRACE),
		IF(SYS_getuid, LOG),
		IF(SYS_getpid, ERRNO | 0),
		IF(SYS_getsid, ERRNO | EACCES),
		RET(ALLOW),
	};
	/* a second, older filter: ERRNO on getppid loses to the newer TRAP */
	struct sock_filter g[] = { LD_NR, IF(SYS_getppid, ERRNO | EPERM), RET(ALLOW) };
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_sys;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSYS, &sa, NULL)) return "sigaction";
	if (install(g, sizeof g / sizeof g[0]) || install(f, sizeof f / sizeof f[0])) return "install";

	long r = syscall(SYS_getppid);
	if (got != SIGSYS) return "TRAP: no SIGSYS";
	if (r != 4242) return "TRAP: rax from the handler is not the result";
	if (code != 1) return "TRAP: si_code is not SYS_SECCOMP";
	if (err != 0x42) return "TRAP: si_errno is not the filter's data";
	if (nr != SYS_getppid) return "TRAP: si_syscall";
	if (arch != 0xc000003eu) return "TRAP: si_arch";
	if (rax != SYS_getppid) return "TRAP: rax in the context is not the syscall number";
	if (rip != call) return "TRAP: rip in the context is not si_call_addr";
	if (!call || ((unsigned char *)call)[-2] != 0x0f || ((unsigned char *)call)[-1] != 0x05)
		return "TRAP: si_call_addr is not just past a syscall instruction";
	got = 0;
	syscall(SYS_unlink, path);
	if (got != SIGSYS || err != 7) return "TRAP on unlink: no SIGSYS";
	if (access(path, F_OK)) return "TRAP: the trapped unlink ran";
	errno = 0;
	if (syscall(SYS_getpgrp) != -1 || errno != ENOSYS) return "TRACE without a tracer: not ENOSYS";
	if (syscall(SYS_getuid) != (long)getuid()) return "LOG: did not run";
	if (syscall(SYS_getpid) != 0) return "ERRNO(0): did not return 0";
	if (syscall(SYS_getsid, 0) != -1 || errno != EACCES) return "ERRNO(EACCES)";
	return NULL;
}

/* negative: SIGSYS blocked (how 1) or ignored (how 2) still kills, by SIGSYS */
static int forced(int how)
{
	pid_t c = fork();
	if (c == 0) {
		struct sock_filter f[] = { LD_NR, IF(SYS_getppid, TRAP), RET(ALLOW) };
		if (how == 1) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGSYS); sigprocmask(SIG_BLOCK, &s, NULL); }
		else signal(SIGSYS, SIG_IGN);
		if (install(f, sizeof f / sizeof f[0])) _exit(2);
		syscall(SYS_getppid);
		_exit(0);   /* must not get here */
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS;
}

int main(void)
{
	int pp[2];
	if (pipe(pp)) return 0;
	pid_t c = fork();
	if (c == 0) {
		close(pp[0]);
		const char *e = trap_child();
		if (e && write(pp[1], e, strlen(e)) < 0) { }
		_exit(e ? 1 : 0);
	}
	close(pp[1]);
	char m[128];
	int n = (int)read(pp[0], m, sizeof m - 1);
	m[n > 0 ? n : 0] = 0;
	int st = 0;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		if (WIFSIGNALED(st)) printf("FAIL trap child killed by signal %d\n", WTERMSIG(st));
		else printf("FAIL %s\n", m);
		return 0;
	}
	if (!forced(1)) { puts("FAIL blocked SIGSYS did not kill by SIGSYS"); return 0; }
	if (!forced(2)) { puts("FAIL ignored SIGSYS did not kill by SIGSYS"); return 0; }
	puts("OK");
	return 0;
}
