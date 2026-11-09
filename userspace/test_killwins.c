/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Kill always wins (arastirma/ajan-insan-ozellikleri.md, item 1).
 * 2026-11-08 a task looped in an ioctl holding a lock, SIGKILL did nothing and
 * the machine had to be power-cycled. Invariant: a SIGKILLed task is gone
 * promptly wherever it is blocked. Each case parks a child in one kernel wait,
 * kills it 150 ms later and times the reap; every case must finish under
 * LIMIT_MS. Kernel-side shapes (a data-driven loop, a waiter behind a long lock
 * hold) are driven through /dev/rescue's self-test ioctls.
 *   test_killwins          all wait cases          -> last line "OK" / "FAIL ..."
 *   test_killwins kwatch   a loop that never checks (13 s): the kstall watch must
 *                          name it, and the kill must land when it returns. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/futex.h>

#define LIMIT_MS 1000
#define RESCUE_IOC_TEST_LOOP  0x5201
#define RESCUE_IOC_TEST_HOLD  0x5202
#define RESCUE_IOC_TEST_LOCK  0x5203
#define RESCUE_IOC_TEST_SPIN  0x5204

static long now_ms(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int fails;
static char tmpd[64];
static int lockfd = -1;

static void park(int which);

/* fork, park, SIGKILL after delay_ms, reap; returns kill->reap ms or -1 */
static long run_case(int which, const char *name, int delay_ms)
{
	pid_t c = fork();
	if (c == 0) { setpgid(0, 0); park(which); _exit(99); }
	usleep((useconds_t)delay_ms * 1000);
	int st = 0;
	pid_t w = waitpid(c, &st, WNOHANG);
	if (w == c) { printf("FAIL %s: child ended before the kill (status %d)\n", name, st); fails++; return -1; }
	long t0 = now_ms();
	kill(c, SIGKILL);
	long dt = -1;
	for (;;) {
		w = waitpid(c, &st, WNOHANG);
		if (w == c) { dt = now_ms() - t0; break; }
		if (now_ms() - t0 > 5000) break;
		usleep(2000);
	}
	kill(-c, SIGKILL);                    /* grandchildren of some cases */
	if (dt < 0) {
		printf("FAIL %s: SIGKILL ignored for 5 s (UNKILLABLE)\n", name); fails++;
		return -1;
	}
	int ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL && dt <= LIMIT_MS;
	printf("%s %-14s %ld ms\n", ok ? "ok  " : "FAIL", name, dt);
	if (!ok) fails++;
	return dt;
}

enum { C_PIPE_R, C_PIPE_W, C_FIFO, C_FUTEX, C_SLEEP, C_WAITPID, C_PAUSE_H, C_ACCEPT, C_RECV,
       C_FLOCK, C_SETLKW, C_POLL, C_SELECT, C_EPOLL, C_SIGWAIT, C_MSGRCV, C_STOPPED, C_VFORK,
       C_KLOOP, C_KLOCK, C_N };
static const char *names[C_N] = { "pipe-read", "pipe-write", "fifo-open", "futex", "nanosleep",
	"waitpid", "pause+handler", "accept", "recvfrom", "flock", "fcntl-setlkw", "poll", "select",
	"epoll", "sigtimedwait", "msgrcv", "stopped", "vfork-parent", "kernel-loop", "killable-mutex" };

static void on_term(int s) { (void)s; }

static void park(int which)
{
	int p[2];
	switch (which) {
	case C_PIPE_R: { pipe(p); char b; read(p[0], &b, 1); break; }
	case C_PIPE_W: {
		pipe(p);
		fcntl(p[1], F_SETFL, O_NONBLOCK);
		static char big[65536];
		while (write(p[1], big, sizeof big) > 0) ;
		fcntl(p[1], F_SETFL, 0);
		write(p[1], big, sizeof big);
		break;
	}
	case C_FIFO: {
		char f[96]; snprintf(f, sizeof f, "%s/fifo", tmpd);
		open(f, O_RDONLY);
		break;
	}
	case C_FUTEX: { static int v; syscall(SYS_futex, &v, FUTEX_WAIT, 0, NULL, NULL, 0); break; }
	case C_SLEEP: sleep(100); break;
	case C_WAITPID: {
		pid_t g = fork();
		if (g == 0) { for (;;) pause(); }
		waitpid(g, NULL, 0);
		break;
	}
	case C_PAUSE_H: signal(SIGTERM, on_term); signal(SIGINT, on_term); for (;;) pause();
	case C_ACCEPT: {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		bind(s, (void *)&a, sizeof a); listen(s, 1); accept(s, NULL, NULL);
		break;
	}
	case C_RECV: {
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		bind(s, (void *)&a, sizeof a);
		char b[16]; recvfrom(s, b, sizeof b, 0, NULL, NULL);
		break;
	}
	case C_FLOCK: {
		char f[96]; snprintf(f, sizeof f, "%s/lock", tmpd);
		int fd = open(f, O_RDWR);
		flock(fd, LOCK_EX);
		break;
	}
	case C_SETLKW: {
		char f[96]; snprintf(f, sizeof f, "%s/lock", tmpd);
		int fd = open(f, O_RDWR);
		struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
		fcntl(fd, F_SETLKW, &l);
		break;
	}
	case C_POLL: poll(NULL, 0, -1); break;
	case C_SELECT: select(0, NULL, NULL, NULL, NULL); break;
	case C_EPOLL: { int e = epoll_create1(0); struct epoll_event ev; epoll_wait(e, &ev, 1, -1); break; }
	case C_SIGWAIT: {
		sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
		sigprocmask(SIG_BLOCK, &s, NULL);
		sigtimedwait(&s, NULL, NULL);
		break;
	}
	case C_MSGRCV: {
		int q = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
		struct { long t; char b[8]; } m;
		msgrcv(q, &m, sizeof m.b, 0, 0);
		break;
	}
	case C_STOPPED: raise(SIGSTOP); for (;;) pause();
	case C_VFORK: {
		pid_t g = vfork();
		if (g == 0) { execl("/bin/sleep", "sleep", "100", (char *)0); _exit(1); }
		waitpid(g, NULL, 0);
		break;
	}
	case C_KLOOP: {
		int fd = open("/dev/rescue", O_RDWR);
		if (fd < 0) _exit(98);
		ioctl(fd, RESCUE_IOC_TEST_LOOP, 30UL);
		break;
	}
	case C_KLOCK: {
		int fd = open("/dev/rescue", O_RDWR);
		if (fd < 0) _exit(98);
		ioctl(fd, RESCUE_IOC_TEST_LOCK, 1UL);   /* sits behind the holder below */
		break;
	}
	}
	_exit(97);
}

static long read_counter(const char *key)
{
	char b[4096] = {0};
	int fd = open("/dev/rescue", O_RDONLY);
	if (fd < 0) return -1;
	int n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	char *p = strstr(b, key);
	return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static int kwatch_case(void)
{
	long before = read_counter("kstall=");
	if (before < 0) { printf("FAIL no /dev/rescue\n"); return 1; }
	pid_t c = fork();
	if (c == 0) {
		int fd = open("/dev/rescue", O_RDWR);
		ioctl(fd, RESCUE_IOC_TEST_SPIN, 13UL);
		_exit(0);
	}
	sleep(1);
	long t0 = now_ms();
	kill(c, SIGKILL);
	int st = 0;
	waitpid(c, &st, 0);
	long dt = now_ms() - t0;
	long after = read_counter("kstall=");
	printf("spin killed after %ld ms (the loop never checks), kstall %ld -> %ld\n", dt, before, after);
	if (!(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)) { printf("FAIL spinner not killed\n"); return 1; }
	if (after != before + 1) { printf("FAIL kstall watch did not name the stall\n"); return 1; }
	printf("OK\n");
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1 && !strcmp(argv[1], "kwatch")) return kwatch_case();

	snprintf(tmpd, sizeof tmpd, "/tmp/killwins.%d", (int)getpid());
	mkdir(tmpd, 0700);
	char f[96];
	snprintf(f, sizeof f, "%s/fifo", tmpd); mkfifo(f, 0600);
	snprintf(f, sizeof f, "%s/lock", tmpd);
	lockfd = open(f, O_RDWR | O_CREAT, 0600);
	flock(lockfd, LOCK_EX);
	int lk2 = open(f, O_RDWR);
	struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
	fcntl(lk2, F_SETLK, &l);   /* POSIX lock: held by this process; flock above is per-fd */

	long unk0 = read_counter("unkillable=");
	int have_rescue = unk0 >= 0;
	long worst = 0;
	for (int i = 0; i < C_N; i++) {
		if ((i == C_KLOOP || i == C_KLOCK) && !have_rescue) { printf("skip %s: no /dev/rescue\n", names[i]); continue; }
		pid_t holder = 0;
		if (i == C_KLOCK) {   /* a task holding the kernel test mutex for 20 s */
			holder = fork();
			if (holder == 0) {
				int fd = open("/dev/rescue", O_RDWR);
				ioctl(fd, RESCUE_IOC_TEST_HOLD, 20UL);
				_exit(0);
			}
			usleep(100000);
		}
		long dt = run_case(i, names[i], i == C_STOPPED ? 300 : 150);
		if (dt > worst) worst = dt;
		if (holder) {          /* the holder sleeps holding the lock: it must die promptly too */
			long t0 = now_ms();
			kill(holder, SIGKILL);
			int st; waitpid(holder, &st, 0);
			long dh = now_ms() - t0;
			int ok = dh <= LIMIT_MS;
			printf("%s %-14s %ld ms\n", ok ? "ok  " : "FAIL", "mutex-holder", dh);
			if (!ok) fails++;
		}
	}
	long unk1 = read_counter("unkillable=");
	snprintf(f, sizeof f, "%s/fifo", tmpd); unlink(f);
	snprintf(f, sizeof f, "%s/lock", tmpd); unlink(f);
	rmdir(tmpd);
	if (have_rescue && unk1 != unk0) { printf("FAIL unkillable counter moved %ld -> %ld\n", unk0, unk1); fails++; }
	printf("worst %ld ms over %d cases\n", worst, C_N);
	if (fails) { printf("FAIL %d case(s)\n", fails); return 1; }
	printf("OK\n");
	return 0;
}
