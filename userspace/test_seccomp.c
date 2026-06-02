/* seccomp: STRICT mode kills a process that makes a forbidden syscall; FILTER
 * mode runs a BPF program (here: force getpid to fail with EPERM). Raw syscalls
 * are used so musl's wrappers/caching don't hide the kernel behaviour. */
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

int main(void)
{
	/* STRICT: getpid is not allowed → SIGKILL */
	pid_t a = fork();
	if (a == 0) {
		syscall(SYS_seccomp, 0, 0, 0);   /* SECCOMP_SET_MODE_STRICT */
		syscall(SYS_getpid);             /* forbidden → killed */
		_exit(0);
	}
	int sa = 0;
	waitpid(a, &sa, 0);
	int strict_ok = WIFSIGNALED(sa) && WTERMSIG(sa) == SIGKILL;

	/* FILTER: getpid → errno EPERM, everything else allowed */
	pid_t b = fork();
	if (b == 0) {
		struct sock_filter f[] = {
			{ 0x20, 0, 0, 0 },                                  /* LD nr */
			{ 0x15, 0, 1, (unsigned)SYS_getpid },               /* JEQ getpid? */
			{ 0x06, 0, 0, 0x00050000u | 1u },                   /* RET ERRNO(EPERM) */
			{ 0x06, 0, 0, 0x7fff0000u },                        /* RET ALLOW */
		};
		struct sock_fprog prog = { 4, f };
		if (syscall(SYS_seccomp, 1, 0, &prog) != 0) _exit(2);   /* SET_MODE_FILTER */
		long r = syscall(SYS_getpid);
		_exit((r == -1 && errno == EPERM) ? 0 : 1);
	}
	int sb = 0;
	waitpid(b, &sb, 0);
	int filter_ok = WIFEXITED(sb) && WEXITSTATUS(sb) == 0;

	puts(strict_ok && filter_ok ? "OK" : "FAIL");
	return 0;
}
