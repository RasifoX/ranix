/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Scheduler-lock storm: threads on every CPU hammer the paths that take
 * sched_lock and the TLB shootdown lock at once (yield, futex wait/wake,
 * signals, short sleeps, fork/exit/wait, mprotect/munmap in a shared address
 * space), then the kernel log is checked for the spinlock diagnostics:
 * "smp: ... unlocked ... held by" (an unlock by a CPU that was not the owner)
 * and "smp: ... stuck" (a lost ticket). A real lock loss panics the kernel and
 * ends the run on the console instead.
 *
 *   test_spinstorm [SECONDS] [THREADS]      default 15 s, 8 threads
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static volatile unsigned long sigs;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned long turn;
static unsigned long ops[64];

static void on_usr1(int s) { (void)s; __atomic_add_fetch(&sigs, 1, __ATOMIC_RELAXED); }

static void *storm(void *arg)
{
	long id = (long)arg;
	unsigned long n = 0;
	struct timespec z = { 0, 1000 };
	while (!stop) {
		switch (n % 6) {
		case 0:
			sched_yield();
			break;
		case 1:   /* futex wait/wake through a condvar handoff */
			pthread_mutex_lock(&mu);
			turn++;
			pthread_cond_broadcast(&cv);
			if (!stop) {
				struct timespec t;
				clock_gettime(CLOCK_REALTIME, &t);
				t.tv_nsec += 200000;
				if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
				pthread_cond_timedwait(&cv, &mu, &t);
			}
			pthread_mutex_unlock(&mu);
			break;
		case 2:
			kill(getpid(), SIGUSR1);
			break;
		case 3:
			nanosleep(&z, NULL);
			break;
		case 4: { /* PTE downgrade in a multithreaded space: cross-CPU shootdown */
			char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p != MAP_FAILED) {
				p[0] = 1; p[4096] = 2;
				mprotect(p, 8192, PROT_READ);
				munmap(p, 8192);
			}
			break;
		}
		case 5:
			if (id == 0 && (n % 60) == 5) {
				pid_t c = fork();
				if (c == 0) _exit(0);
				if (c > 0) waitpid(c, NULL, 0);
			}
			break;
		}
		n++;
	}
	ops[id] = n;
	return NULL;
}

/* Lines of the kernel log that report a broken lock. /dev/kmsg is read without
 * blocking until it runs dry. */
static int scan_kmsg(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { printf("FAIL kmsg errno=%d\n", errno); return -1; }
	char buf[1024];
	int bad = 0;
	for (int i = 0; i < 200000; i++) {
		ssize_t r = read(fd, buf, sizeof(buf) - 1);
		if (r < 0 && errno == EPIPE) continue;   /* overwritten record: skip it */
		if (r <= 0) break;
		buf[r] = 0;
		if (strstr(buf, "smp: cpu") && (strstr(buf, " unlocked ") || strstr(buf, " stuck "))) {
			if (bad < 4) printf("kmsg: %s", buf);
			bad++;
		}
	}
	close(fd);
	return bad;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int secs = argc > 1 ? atoi(argv[1]) : 15;
	int nthr = argc > 2 ? atoi(argv[2]) : 8;
	if (secs < 1) secs = 1;
	if (nthr < 2) nthr = 2;
	if (nthr > 64) nthr = 64;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	int before = scan_kmsg();
	if (before < 0) return 1;

	pthread_t t[64];
	for (long i = 0; i < nthr; i++)
		if (pthread_create(&t[i], NULL, storm, (void *)i) != 0) {
			printf("FAIL pthread_create %ld\n", i);
			return 1;
		}
	sleep((unsigned)secs);
	stop = 1;
	pthread_mutex_lock(&mu);
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	unsigned long total = 0;
	for (int i = 0; i < nthr; i++) { pthread_join(t[i], NULL); total += ops[i]; }

	/* the watchdog prints recorded bad unlocks on its ~2.5 s sweep */
	sleep(3);
	int bad = scan_kmsg();
	printf("ops=%lu signals=%lu lock-reports=%d\n", total, sigs, bad);
	if (bad != 0 || total == 0) { printf("FAIL\n"); return 1; }
	puts("OK");
	return 0;
}
