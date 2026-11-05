/* The syscall shape libuv/Node use on a stdout pipe: FIONBIO + fcntl, epoll +
 * eventfd, write-until-EAGAIN against a slow reader, then process exit while
 * helper threads sit blocked. Each case runs in its own child under a watchdog,
 * so a hang is reported by name instead of stalling the run.
 *
 *   test_uvpipe              run every case, last line is OK or the failures
 *   test_uvpipe node [N]     act like `node t.js | tail`: print N lines through
 *                            a nonblocking stdout, leave threads parked, exit */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/wait.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
	nanosleep(&t, 0);
}

static int nonblock_like_uv(int fd)
{
	int on = 1;
	if (ioctl(fd, FIONBIO, &on) != 0) return -errno;
	int fl = fcntl(fd, F_GETFL);
	if (fl < 0) return -errno;
	if (!(fl & O_NONBLOCK)) return -EINVAL;
	return 0;
}

/* read until EOF, up to ms; returns bytes, or -1 if no EOF arrived in time */
static long drain_eof(int fd, int ms)
{
	char buf[1024];
	long total = 0;
	double end = now() + (double)ms / 1000.0;
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		int left = (int)((end - now()) * 1000.0);
		if (left <= 0) return -1;
		int r = poll(&p, 1, left);
		if (r == 0) return -1;
		if (r < 0 && errno != EINTR) return -1;
		if (r < 0) continue;
		long n = read(fd, buf, sizeof(buf));
		if (n == 0) return total;
		if (n < 0 && errno != EINTR && errno != EAGAIN) return -1;
		if (n > 0) total += n;
	}
}

/* ── helper threads parked the way Node's are ─────────────────────────── */

static pthread_mutex_t park_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  park_cv = PTHREAD_COND_INITIALIZER;
static volatile int    parked;
static int             park_epfd = -1;
static int            *park_tids;    /* shared page: lets the parent audit them */

static void parked_in(void)
{
	int i = __sync_fetch_and_add(&parked, 1);
	if (park_tids) park_tids[i] = (int)syscall(SYS_gettid);
}

static void *park_cond(void *a)
{
	(void)a;
	pthread_mutex_lock(&park_mu);
	parked_in();
	for (;;) pthread_cond_wait(&park_cv, &park_mu);
}

static void *park_epoll(void *a)
{
	(void)a;
	struct epoll_event ev[4];
	parked_in();
	for (;;) epoll_pwait(park_epfd, ev, 4, -1, 0);
}

static void *park_sleep(void *a)
{
	(void)a;
	parked_in();
	for (;;) sleep(100);
}

static void *park_spin(void *a)
{
	(void)a;
	parked_in();
	for (;;) { }
}

static void park_all(void)
{
	pthread_t t;
	park_epfd = epoll_create1(EPOLL_CLOEXEC);
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN };
	epoll_ctl(park_epfd, EPOLL_CTL_ADD, efd, &ev);
	pthread_create(&t, 0, park_cond, 0);
	pthread_create(&t, 0, park_epoll, 0);
	pthread_create(&t, 0, park_sleep, 0);
	pthread_create(&t, 0, park_spin, 0);
	while (parked < 4) msleep(1);
	msleep(30);   /* let them reach the blocking syscall */
}

/* ── cases: return 0 on pass, nonzero picks the failing step ──────────── */

static int t_fionbio(void)
{
	int p[2], off = 0;
	if (pipe(p)) return 1;
	if (nonblock_like_uv(p[1]) != 0) return 2;
	if (nonblock_like_uv(p[0]) != 0) return 3;
	char c;
	if (read(p[0], &c, 1) != -1 || errno != EAGAIN) return 4;
	if (ioctl(p[1], FIONBIO, &off) != 0) return 5;
	if (fcntl(p[1], F_GETFL) & O_NONBLOCK) return 6;
	if (ioctl(p[1], FIOCLEX) != 0 || !(fcntl(p[1], F_GETFD) & FD_CLOEXEC)) return 7;
	if (ioctl(p[1], FIONCLEX) != 0 || (fcntl(p[1], F_GETFD) & FD_CLOEXEC)) return 8;
	if (write(p[1], "abc", 3) != 3) return 9;
	int n = -1;
	if (ioctl(p[0], FIONREAD, &n) != 0 || n != 3) return 10;
	return 0;
}

static int t_setfl_keeps_mode(void)
{
	int p[2];
	if (pipe(p)) return 1;
	if (fcntl(p[1], F_SETFL, O_NONBLOCK) != 0) return 2;
	if ((fcntl(p[1], F_GETFL) & O_ACCMODE) != O_WRONLY) return 3;
	if (write(p[1], "x", 1) != 1) return 4;
	close(p[1]);
	char c;
	if (read(p[0], &c, 1) != 1) return 5;
	if (read(p[0], &c, 1) != 0) return 6;    /* EOF: writer count must have dropped */
	return 0;
}

#define BULK (256 * 1024)

/* writer goes nonblocking and waits on epoll; the reader checks every byte */
static int bulk_through_epoll(int reader_delay_ms, int chunk_read)
{
	int p[2];
	if (pipe(p)) return 1;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[1]);
		static unsigned char b[8192];
		long off = 0;
		for (;;) {
			if (reader_delay_ms) msleep(reader_delay_ms);
			long n = read(p[0], b, (size_t)chunk_read);
			if (n < 0) _exit(10);
			if (n == 0) break;
			for (long i = 0; i < n; i++)
				if (b[i] != (unsigned char)((off + i) * 7)) _exit(11);
			off += n;
		}
		_exit(off == BULK ? 0 : 12);
	}
	close(p[0]);
	if (nonblock_like_uv(p[1]) != 0) return 2;
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 1 };
	if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev)) return 3;
	ev.events = 0; ev.data.u64 = 2;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev)) return 4;

	static unsigned char out[BULK];
	for (long i = 0; i < BULK; i++) out[i] = (unsigned char)(i * 7);

	long off = 0;
	int saw_eagain = 0, saw_out = 0;
	while (off < BULK) {
		long n = write(p[1], out + off, (size_t)(BULK - off > 16384 ? 16384 : BULK - off));
		if (n > 0) { off += n; continue; }
		if (errno != EAGAIN) return 5;
		saw_eagain = 1;
		ev.events = EPOLLOUT; ev.data.u64 = 2;
		if (epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &ev)) return 6;
		struct epoll_event got[4];
		int r;
		do r = epoll_pwait(ep, got, 4, -1, 0); while (r < 0 && errno == EINTR);
		if (r <= 0) return 7;
		if (!(got[0].events & EPOLLOUT) || got[0].data.u64 != 2) return 8;
		saw_out = 1;
		ev.events = 0;
		if (epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &ev)) return 9;
	}
	close(p[1]);
	int st = 0;
	if (waitpid(pid, &st, 0) != pid) return 13;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 20 + (WIFEXITED(st) ? WEXITSTATUS(st) : 50);
	if (!saw_eagain || !saw_out) return 14;
	return 0;
}

static int t_bulk_slow_reader(void) { return bulk_through_epoll(2, 512); }

static volatile double drained_at;
static volatile int    drain_go;

/* Stays alive across rounds: a thread's exit wakes its creator, which would
 * hide a missing pipe wakeup. */
static void *drainer(void *a)
{
	int seen = 0;
	for (;;) {
		char c;
		while (drain_go == seen) msleep(1);
		seen = drain_go;
		msleep(30);            /* let the writer settle in epoll_pwait */
		drained_at = now();
		if (read(*(int *)a, &c, 1) != 1) return (void *)1;
	}
}

/* A reader freeing space must wake an EPOLLOUT sleeper itself. Left to the
 * 10-tick backstop, each pipeful costs 100 ms and a pipeline crawls. Best of
 * several, so one scheduling hiccup can't fail it. */
static int t_epollout_prompt(void)
{
	int p[2];
	if (pipe(p)) return 1;
	if (nonblock_like_uv(p[1]) != 0) return 2;
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLOUT };
	if (epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev)) return 3;
	pthread_t t;
	pthread_create(&t, 0, drainer, &p[0]);
	char fill[512];
	memset(fill, 'x', sizeof(fill));
	double best = 1e9;
	for (int i = 0; i < 6; i++) {
		while (write(p[1], fill, sizeof(fill)) > 0) { }
		while (write(p[1], fill, 1) > 0) { }
		if (errno != EAGAIN) return 4;
		drain_go++;
		struct epoll_event got;
		if (epoll_pwait(ep, &got, 1, 5000, 0) != 1 || !(got.events & EPOLLOUT)) return 5;
		double dt = now() - drained_at;
		if (dt < best) best = dt;
	}
	if (best > 0.040) { printf("  best wake %.0f ms\n", best * 1000); return 6; }
	return 0;
}

static void *kick_eventfd(void *a)
{
	msleep(50);
	uint64_t one = 1;
	if (write(*(int *)a, &one, 8) != 8) return (void *)1;
	return 0;
}

static int t_eventfd_wakes_epoll(void)
{
	int ep = epoll_create1(EPOLL_CLOEXEC);
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ep < 0 || efd < 0) return 1;
	if (!(fcntl(ep, F_GETFD) & FD_CLOEXEC)) return 2;
	if (!(fcntl(efd, F_GETFD) & FD_CLOEXEC)) return 3;
	sigset_t set;
	sigemptyset(&set);
	if (!(fcntl(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC), F_GETFD) & FD_CLOEXEC)) return 8;
	if (!(fcntl(signalfd(-1, &set, SFD_CLOEXEC), F_GETFD) & FD_CLOEXEC)) return 9;
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 42 };
	if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev)) return 4;
	pthread_t t;
	pthread_create(&t, 0, kick_eventfd, &efd);
	struct epoll_event got;
	if (epoll_pwait(ep, &got, 1, 5000, 0) != 1 || got.data.u64 != 42) return 5;
	pthread_join(t, 0);
	uint64_t v = 0;
	if (read(efd, &v, 8) != 8 || v != 1) return 6;
	if (read(efd, &v, 8) != -1 || errno != EAGAIN) return 7;
	return 0;
}

static void on_usr1(int s) { (void)s; }

static int t_epoll_eintr(void)
{
	signal(SIGUSR1, on_usr1);
	int ep = epoll_create1(0);
	int efd = eventfd(0, 0);
	struct epoll_event ev = { .events = EPOLLIN };
	epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	pid_t me = getpid();
	if (fork() == 0) { msleep(50); kill(me, SIGUSR1); _exit(0); }
	int r = epoll_pwait(ep, &ev, 1, -1, 0);
	if (r != -1 || errno != EINTR) return 1;
	return 0;
}

/* the writer's epoll must see the reader go away, and EPIPE follows */
static int t_reader_gone(void)
{
	signal(SIGPIPE, SIG_IGN);
	int p[2];
	if (pipe(p)) return 1;
	if (nonblock_like_uv(p[1]) != 0) return 2;
	char b[4096];
	memset(b, 'x', sizeof(b));
	while (write(p[1], b, sizeof(b)) > 0) { }
	if (errno != EAGAIN) return 3;
	int ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLOUT };
	epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
	int rd = p[0];
	pid_t pid = fork();
	if (pid == 0) { msleep(50); _exit(0); }
	close(rd);       /* parent's copy; the child's goes at _exit */
	waitpid(pid, 0, 0);
	struct epoll_event got;
	if (epoll_pwait(ep, &got, 1, 5000, 0) != 1) return 4;
	if (!(got.events & EPOLLERR)) return 5;
	if (write(p[1], b, 1) != -1 || errno != EPIPE) return 6;
	return 0;
}

/* libuv reads IPC sockets through recvmsg on a nonblocking fd: empty is EAGAIN,
 * never a sleep, and a closed peer is EOF on every read path */
static int t_nonblock_sock(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 1;
	if (nonblock_like_uv(sv[0]) != 0) return 2;
	char b[8];
	struct iovec v = { b, sizeof(b) };
	struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
	if (recvmsg(sv[0], &m, 0) != -1 || errno != EAGAIN) return 3;
	if (recv(sv[0], b, sizeof(b), 0) != -1 || errno != EAGAIN) return 4;
	close(sv[1]);
	if (recv(sv[0], b, sizeof(b), 0) != 0) return 5;
	if (recvmsg(sv[0], &m, 0) != 0) return 6;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 7;
	close(sv[1]);
	if (recv(sv[0], b, sizeof(b), 0) != 0) return 8;   /* blocking: must not sleep past EOF */
	return 0;
}

/* run body() in a child with a pipe on stdout; the reader must get EOF once the
 * process exits, however many threads it left behind */
static int exit_gives_eof(void (*body)(void), int want_exit, int want_sig)
{
	int p[2];
	if (pipe(p)) return 1;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[1]);
		parked = 0;
		body();
		_exit(99);
	}
	close(p[1]);
	long got = drain_eof(p[0], 5000);
	int st = 0;
	pid_t r = 0;
	for (int i = 0; i < 300 && r == 0; i++) { r = waitpid(pid, &st, WNOHANG); if (!r) msleep(10); }
	if (r == 0) { kill(pid, SIGKILL); }
	if (got < 0) return 2;                         /* the Node hang */
	if (r != pid) return 3;
	if (want_sig) {
		if (!WIFSIGNALED(st) || WTERMSIG(st) != want_sig) { printf("  st=%x\n", st); return 4; }
	} else if (!WIFEXITED(st) || WEXITSTATUS(st) != want_exit) {
		printf("  st=%x\n", st);
		return 5;
	}
	return 0;
}

static void b_main_exits(void)
{
	park_all();
	if (nonblock_like_uv(1) != 0) _exit(98);
	if (write(1, "done\n", 5) != 5) _exit(97);
	exit(3);
}

static void *exit_from_worker(void *a) { (void)a; msleep(20); exit(7); }

static void b_worker_exits(void)
{
	park_all();
	pthread_t t;
	pthread_create(&t, 0, exit_from_worker, 0);
	for (;;) pause();
}

static void b_killed(void)
{
	park_all();
	pid_t me = getpid();
	if (fork() == 0) { msleep(20); kill(me, SIGKILL); _exit(0); }
	for (;;) pause();
}

static void b_terminated(void)
{
	park_all();
	pid_t me = getpid();
	if (fork() == 0) { msleep(20); kill(me, SIGTERM); _exit(0); }
	for (;;) pause();
}

static void *segv_worker(void *a) { (void)a; msleep(20); *(volatile int *)8 = 1; return 0; }

static void b_worker_faults(void)
{
	park_all();
	pthread_t t;
	pthread_create(&t, 0, segv_worker, 0);
	for (;;) pause();
}

static int t_exit_main(void)    { return exit_gives_eof(b_main_exits, 3, 0); }
static int t_exit_worker(void)  { return exit_gives_eof(b_worker_exits, 7, 0); }
static int t_exit_sigkill(void) { return exit_gives_eof(b_killed, 0, SIGKILL); }
static int t_exit_sigterm(void) { return exit_gives_eof(b_terminated, 0, SIGTERM); }
static int t_exit_segv(void)    { return exit_gives_eof(b_worker_faults, 0, SIGSEGV); }

static void *churn_sleep(void *a)
{
	(void)a;
	struct timespec t = { 0, 1 };
	for (;;) nanosleep(&t, 0);
}

static void *churn_cond(void *a)
{
	(void)a;
	for (;;) {
		struct timespec t;
		clock_gettime(CLOCK_REALTIME, &t);
		pthread_mutex_lock(&park_mu);
		pthread_cond_timedwait(&park_cv, &park_mu, &t);
		pthread_mutex_unlock(&park_mu);
		pthread_cond_signal(&park_cv);
	}
}

static void b_exit_mid_sleep(void)
{
	pthread_t t;
	pthread_create(&t, 0, churn_sleep, 0);
	pthread_create(&t, 0, churn_sleep, 0);
	pthread_create(&t, 0, churn_cond, 0);
	pthread_create(&t, 0, churn_cond, 0);
	msleep(3);
	exit(0);
}

/* the group exit lands while siblings are on their way into a sleep: one that
 * misses the wakeup keeps the pipe open and this reports a hang */
static int t_exit_mid_sleep(void)
{
	for (int i = 0; i < 40; i++) {
		int r = exit_gives_eof(b_exit_mid_sleep, 0, 0);
		if (r) { printf("  round %d\n", i); return r; }
	}
	return 0;
}

/* 'Z' or gone is fine: only a thread still running would be a straggler */
static int tid_alive(int tid)
{
	char path[64], st[128];
	snprintf(path, sizeof(path), "/proc/%d/stat", tid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	long n = read(fd, st, sizeof(st) - 1);
	close(fd);
	if (n <= 0) return 0;
	st[n] = 0;
	char *e = strrchr(st, ')');
	return e && e[1] == ' ' && e[2] != 'Z' && e[2] != 'X';
}

static int t_no_stragglers(void)
{
	park_tids = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (park_tids == MAP_FAILED) return 1;
	pid_t pid = fork();
	if (pid == 0) { park_all(); exit(0); }
	int st;
	if (waitpid(pid, &st, 0) != pid) return 2;
	msleep(300);
	for (int i = 0; i < 4; i++) {
		if (!park_tids[i]) return 3;
		if (tid_alive(park_tids[i])) { printf("  tid %d alive\n", park_tids[i]); return 4; }
	}
	return 0;
}

static void *late_writer(void *a)
{
	msleep(40);
	if (write(*(int *)a, "hello", 5) != 5) return (void *)1;
	return 0;
}

/* a blocking read returns what is there; it does not wait to fill the buffer */
static int t_short_read(void)
{
	int p[2];
	if (pipe(p)) return 1;
	pthread_t t;
	pthread_create(&t, 0, late_writer, &p[1]);
	char b[4096];
	long n = read(p[0], b, sizeof(b));   /* watchdog catches the blocking bug */
	pthread_join(t, 0);
	if (n != 5) return 2;
	return 0;
}

static int t_writev_partial(void)
{
	int p[2];
	if (pipe(p)) return 1;
	if (nonblock_like_uv(p[1]) != 0) return 2;
	static char a[32768], b[32768];
	memset(a, 'a', sizeof(a));
	memset(b, 'b', sizeof(b));
	struct iovec v[2] = { { a, sizeof(a) }, { b, sizeof(b) } };
	long sent = writev(p[1], v, 2);
	if (sent <= 0) return 3;
	if (sent >= (long)sizeof(a) + (long)sizeof(b)) return 0;   /* roomy pipe: nothing to test */
	/* whatever was accepted must be a prefix: a short first iovec may not be
	 * followed by bytes from the second */
	char *in = malloc((size_t)sent);
	long got = 0;
	while (got < sent) {
		long n = read(p[0], in + got, (size_t)(sent - got));
		if (n <= 0) return 4;
		got += n;
	}
	for (long i = 0; i < sent; i++) {
		char want = i < (long)sizeof(a) ? 'a' : 'b';
		if (in[i] != want) return 5;
	}
	char x;
	if (nonblock_like_uv(p[0]) != 0) return 6;
	if (read(p[0], &x, 1) != -1 || errno != EAGAIN) return 7;  /* bytes beyond sent = lost count */
	return 0;
}

static int t_poll_by_end(void)
{
	int p[2];
	if (pipe(p)) return 1;
	if (write(p[1], "x", 1) != 1) return 2;
	struct pollfd f = { p[1], POLLIN, 0 };
	if (poll(&f, 1, 0) != 0) return 3;       /* write end is never readable */
	f = (struct pollfd){ p[0], POLLOUT, 0 };
	if (poll(&f, 1, 0) != 0) return 4;       /* read end is never writable */
	return 0;
}

/* buffers in pages no one has touched: anon mmap is demand-zero now, and the
 * fault lands inside the pipe copy */
static int t_fresh_pages(void)
{
	int p[2];
	if (pipe(p)) return 1;
	size_t sz = 16 * 4096;
	unsigned char *dst = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char *src = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (dst == MAP_FAILED || src == MAP_FAILED) return 2;
	pid_t pid = fork();
	if (pid == 0) {
		for (size_t i = 0; i < sz; i++) src[i] = (unsigned char)(i * 3);
		_exit(write(p[1], src, sz) == (long)sz ? 0 : 1);
	}
	close(p[1]);
	size_t got = 0;
	while (got < sz) {
		long n = read(p[0], dst + got, sz - got);
		if (n <= 0) return 3;
		got += (size_t)n;
	}
	for (size_t i = 0; i < sz; i++)
		if (dst[i] != (unsigned char)(i * 3)) return 4;
	waitpid(pid, 0, 0);
	return 0;
}

/* a wild user pointer is EFAULT, for the caller alone */
static int t_bad_pointers(void)
{
	int p[2];
	if (pipe(p)) return 1;
	void *hole = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (hole == MAP_FAILED) return 2;
	if (write(p[1], hole, 16) != -1 || errno != EFAULT) return 3;
	if (write(p[1], "abcd", 4) != 4) return 4;
	if (read(p[0], hole, 4) != -1 || errno != EFAULT) return 5;
	return 0;
}

/* ── harness ──────────────────────────────────────────────────────────── */

static int run(const char *name, int (*fn)(void), int secs)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) _exit(fn() & 0xff);
	double end = now() + secs;
	int st = 0;
	for (;;) {
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid) break;
		if (now() > end) {
			kill(pid, SIGKILL);
			printf("FAIL %-14s hang\n", name);
			return 1;
		}
		msleep(5);
	}
	if (WIFSIGNALED(st)) { printf("FAIL %-14s sig %d\n", name, WTERMSIG(st)); return 1; }
	if (WEXITSTATUS(st)) { printf("FAIL %-14s step %d\n", name, WEXITSTATUS(st)); return 1; }
	printf("ok   %s\n", name);
	return 0;
}

static int node_shape(int lines)
{
	park_all();
	if (nonblock_like_uv(1) != 0) { fprintf(stderr, "stdout: FIONBIO failed\n"); return 2; }
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = 0 };
	epoll_ctl(ep, EPOLL_CTL_ADD, 1, &ev);
	char b[64];
	for (int i = 0; i < lines; i++) {
		int n = snprintf(b, sizeof(b), "line %d\n", i), off = 0;
		while (off < n) {
			long w = write(1, b + off, (size_t)(n - off));
			if (w > 0) { off += (int)w; continue; }
			if (errno != EAGAIN) return 3;
			ev.events = EPOLLOUT;
			epoll_ctl(ep, EPOLL_CTL_MOD, 1, &ev);
			struct epoll_event got;
			epoll_pwait(ep, &got, 1, -1, 0);
			ev.events = 0;
			epoll_ctl(ep, EPOLL_CTL_MOD, 1, &ev);
		}
	}
	if (write(1, "node-shape-done\n", 16) < 0 && errno != EAGAIN) return 4;
	exit(0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "node") == 0)
		return node_shape(argc > 2 ? atoi(argv[2]) : 2000);

	/* a case may be named on the command line to run it alone */
	static const struct { const char *n; int (*f)(void); int s; } c[] = {
		{ "fionbio",        t_fionbio,            8 },
		{ "setfl-mode",     t_setfl_keeps_mode,   8 },
		{ "short-read",     t_short_read,         8 },
		{ "poll-by-end",    t_poll_by_end,        8 },
		{ "writev-partial", t_writev_partial,     8 },
		{ "fresh-pages",    t_fresh_pages,        8 },
		{ "eventfd-epoll",  t_eventfd_wakes_epoll,8 },
		{ "epoll-eintr",    t_epoll_eintr,        8 },
		{ "reader-gone",    t_reader_gone,        8 },
		{ "nonblock-sock",  t_nonblock_sock,      8 },
		{ "bulk-slow",      t_bulk_slow_reader,  30 },
		{ "epollout-wake",  t_epollout_prompt,    8 },
		{ "exit-main",      t_exit_main,         10 },
		{ "exit-worker",    t_exit_worker,       10 },
		{ "exit-sigkill",   t_exit_sigkill,      10 },
		{ "exit-sigterm",   t_exit_sigterm,      10 },
		{ "exit-segv",      t_exit_segv,         10 },
		{ "exit-mid-sleep", t_exit_mid_sleep,    40 },
		{ "no-stragglers",  t_no_stragglers,     10 },
		{ "bad-pointers",   t_bad_pointers,       8 },
	};
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		if (argc > 1 && strcmp(argv[1], c[i].n) != 0) continue;
		fails += run(c[i].n, c[i].f, c[i].s);
	}
	if (fails) printf("uvpipe-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
