/* SECCOMP_RET_KILL_THREAD and SECCOMP_RET_KILL_PROCESS, as on Linux:
 * - KILL_THREAD in one thread of two: that thread ends at the filtered call,
 *   the other runs on, and the process exits normally (status 7 here);
 * - KILL_PROCESS from one thread of two: the whole group ends, killed by
 *   SIGSYS, and the other thread never gets to exit(7);
 * - KILL_THREAD in a single-threaded process: killed by SIGSYS;
 * - KILL_PROCESS with a SIGSYS handler installed: no handler runs, killed by
 *   SIGSYS.
 * The filter catches getppid only. Each case runs in a child; the parent
 * reads its wait status. Prints OK, or FAIL with the status that differs. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define KILL_PROCESS 0x80000000u
#define KILL_THREAD  0x00000000u
#define ALLOW        0x7fff0000u

static volatile int before, after, handled;
static unsigned action;

static int filter_getppid(unsigned act)
{
	struct sock_filter f[] = {
		{ 0x20, 0, 0, 0 },                         /* A = nr */
		{ 0x15, 0, 1, (unsigned)SYS_getppid },     /* nr == getppid ? */
		{ 0x06, 0, 0, act },
		{ 0x06, 0, 0, ALLOW },
	};
	struct sock_fprog p = { 4, f };
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
	return (int)syscall(SYS_seccomp, 1 /* SET_MODE_FILTER */, 0, &p);
}

static void *filtered_thread(void *a)
{
	(void)a;
	if (filter_getppid(action)) _exit(20);
	before = 1;
	syscall(SYS_getppid);
	after = 1;           /* must never run */
	for (;;) pause();
	return NULL;
}

static void nap(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* main thread: start the filtered thread, wait for it to hit the call, then
 * see whether it went past it, and exit 7 if this thread is still alive */
static int two_threads(unsigned act)
{
	action = act;
	pthread_t t;
	pthread_create(&t, NULL, filtered_thread, NULL);
	for (int i = 0; i < 200 && !before; i++) nap(10);
	nap(300);
	if (after) return 21;
	syscall(SYS_getppid);  /* the filter is that thread's, not this one's */
	return 7;
}

static void on_sys(int s) { (void)s; handled = 1; }

static int single(unsigned act, int with_handler)
{
	if (with_handler) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_sys;
		sigaction(SIGSYS, &sa, NULL);
	}
	if (filter_getppid(act)) return 20;
	syscall(SYS_getppid);
	return handled ? 22 : 23;   /* must not get here */
}

static int fails;

static void run(const char *what, int (*fn)(unsigned, int), unsigned act, int arg, int want_sig, int want_exit)
{
	pid_t c = fork();
	if (c == 0) {
		prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);   /* no core file for the SIGSYS */
		_exit(fn(act, arg));
	}
	int st = 0;
	waitpid(c, &st, 0);
	int ok = want_sig ? (WIFSIGNALED(st) && WTERMSIG(st) == want_sig)
	                  : (WIFEXITED(st) && WEXITSTATUS(st) == want_exit);
	if (ok) return;
	if (WIFSIGNALED(st)) printf("FAIL %s: killed by signal %d\n", what, WTERMSIG(st));
	else printf("FAIL %s: exit %d\n", what, WEXITSTATUS(st));
	fails++;
}

static int two(unsigned act, int unused) { (void)unused; return two_threads(act); }

int main(void)
{
	run("KILL_THREAD, one thread of two", two, KILL_THREAD, 0, 0, 7);
	run("KILL_PROCESS, one thread of two", two, KILL_PROCESS, 0, SIGSYS, 0);
	run("KILL_THREAD, single thread", single, KILL_THREAD, 0, SIGSYS, 0);
	run("KILL_PROCESS, SIGSYS handler installed", single, KILL_PROCESS, 1, SIGSYS, 0);
	if (fails) return 1;
	printf("OK\n");
	return 0;
}
