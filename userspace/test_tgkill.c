/* Thread signals: a handler installed in one thread is shared by all (POSIX
 * process-wide dispositions), pthread_kill delivers to the right thread, a
 * sig-0 probe reports liveness, and tgkill validates the tgid — a tid not in
 * the named group reports ESRCH (guards against stale, reused tids). */
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

static volatile int got;
static void handler(int s) { (void)s; got = 1; }

static volatile pid_t child_tid;
static volatile int ready;
static void *worker(void *a) { (void)a; child_tid = syscall(SYS_gettid); ready = 1; while (!got) pause(); return 0; }

int main(void)
{
	signal(SIGUSR1, handler);            /* installed by main, must apply to worker too */
	pthread_t th;
	pthread_create(&th, 0, worker, 0);
	while (!ready) usleep(1000);

	if (pthread_kill(th, 0) != 0) { printf("tgkill-fail probe0\n"); return 0; }
	if (pthread_kill(th, SIGUSR1) != 0) { printf("tgkill-fail send\n"); return 0; }
	pthread_join(th, 0);                  /* worker exits its pause loop once got is set */
	if (!got) { printf("tgkill-fail nodeliver\n"); return 0; }

	pid_t pid = getpid();
	if (syscall(SYS_tgkill, -1, pid, 0) != -1 || errno != EINVAL) { printf("tgkill-fail einval\n"); return 0; }
	if (syscall(SYS_tgkill, pid, 0x7ffff0, 0) != -1 || errno != ESRCH) { printf("tgkill-fail esrch\n"); return 0; }

	printf("OK\n");
	return 0;
}
