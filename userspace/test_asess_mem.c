/* A session's memory budget: a cgroup of its own, with a default. Every way of
 * holding memory counts against it, every way of letting go gives it back, and a
 * session at its limit leaves the host running. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "asess_test.h"

#define MIB       (1024L * 1024)
#define PAGE      4096L
#define SLACK     (2 * MIB)     /* page tables, stacks of short-lived helpers */
#define LIMIT     (32 * MIB)

static long sfd_create(unsigned long long mem_max, const int *keep, int nkeep)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = UID;
	a.gid = GID;
	a.root = (unsigned long long)(unsigned long)"/";
	a.keep_fds = (unsigned long long)(unsigned long)keep;
	a.nkeep = (unsigned)nkeep;
	a.mem_max = mem_max;
	return asess(ASESS_CREATE, -1, &a, sizeof(a));
}

static unsigned long long used(void)
{
	struct asess_info in;
	if (asess(ASESS_INFO, -1, &in, sizeof(in)) != 0) return ~0ULL;
	return in.mem_cur;
}

static long meminfo_kb(const char *key)
{
	char b[256];
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *p = strstr(b, key);
	return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static long free_kb(void)
{
	return meminfo_kb("MemFree:");
}

static void touch(char *p, long len)
{
	for (long i = 0; i < len; i += PAGE) p[i] = 1;
}

static char *anon(long len)
{
	return mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static char *brk_at(char *end)
{
	return (char *)syscall(SYS_brk, end);
}

/* run fn in a child of this session; returns its wait status */
static int in_child(void (*fn)(void))
{
	pid_t p = fork();
	if (p == 0) { fn(); _exit(0); }
	int st = 0;
	waitpid(p, &st, 0);
	return st;
}

/* ── giving back ──────────────────────────────────────────── */

static void give_back(void)
{
	unsigned long long base = used();

	for (int i = 0; i < 40; i++) {
		char *m = anon(16 * MIB);
		CHECK(m != MAP_FAILED, "mmap");
		touch(m, 16 * MIB);
		CHECK(used() >= base + 16 * MIB, "touched pages are charged");
		CHECK(munmap(m, 16 * MIB) == 0, "munmap");
		CHECK(used() <= base + SLACK, "munmap uncharges");
	}

	/* a hole, then the rest */
	char *m = anon(8 * MIB);
	touch(m, 8 * MIB);
	munmap(m + 2 * MIB, 4 * MIB);
	CHECK(used() <= base + 4 * MIB + SLACK, "hole uncharges");
	munmap(m, 2 * MIB);
	munmap(m + 6 * MIB, 2 * MIB);
	CHECK(used() <= base + SLACK, "both ends uncharge");

	/* the same range mapped again and again */
	m = anon(4 * MIB);
	touch(m, 4 * MIB);
	for (int i = 0; i < 50; i++) {
		void *r = mmap(m, 4 * MIB, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_POPULATE, -1, 0);
		CHECK(r == m, "MAP_FIXED over itself");
	}
	CHECK(used() <= base + 4 * MIB + SLACK, "MAP_FIXED replaces its charge");
	munmap(m, 4 * MIB);

	m = anon(4 * MIB);
	touch(m, 4 * MIB);
	char *grown = mremap(m, 4 * MIB, 8 * MIB, MREMAP_MAYMOVE);
	CHECK(grown != MAP_FAILED && grown[PAGE] == 1, "mremap");
	CHECK(used() <= base + 4 * MIB + SLACK, "mremap frees the old range");
	munmap(grown, 8 * MIB);

	m = anon(4 * MIB);
	touch(m, 4 * MIB);
	madvise(m, 4 * MIB, MADV_DONTNEED);
	CHECK(used() <= base + SLACK, "MADV_DONTNEED uncharges");
	munmap(m, 4 * MIB);

	/* musl's sbrk only reports */
	char *top = brk_at(0);
	CHECK(brk_at(top + 4 * MIB) == top + 4 * MIB, "brk");
	CHECK(used() >= base + 4 * MIB, "brk is charged");
	CHECK(brk_at(top) == top && used() <= base + SLACK, "brk shrink uncharges");

	m = mmap(NULL, 8 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(m != MAP_FAILED, "shared anon");
	CHECK(used() >= base + 8 * MIB, "shared anon is charged");
	munmap(m, 8 * MIB);
	CHECK(used() <= base + SLACK, "shared anon uncharges");

	/* an unmap of file pages must not pay off anonymous memory */
	char *held = anon(8 * MIB);
	touch(held, 8 * MIB);
	int mfd = (int)syscall(SYS_memfd_create, "t", 0);
	static char pg[PAGE];
	for (int i = 0; i < 256; i++) CHECK(write(mfd, pg, PAGE) == PAGE, "memfd write");
	for (int i = 0; i < 30; i++) {
		char *f = mmap(NULL, MIB, PROT_READ, MAP_SHARED, mfd, 0);
		CHECK(f != MAP_FAILED, "memfd map");
		munmap(f, MIB);
	}
	CHECK(used() >= base + 8 * MIB, "unmapping a file keeps the anonymous charge");
	close(mfd);
	munmap(held, 8 * MIB);
	CHECK(used() <= base + SLACK, "after the file and the memory go");

	/* a mapping keeps a closed memfd's pages, and they stay charged until it goes */
	mfd = (int)syscall(SYS_memfd_create, "t", 0);
	for (int i = 0; i < 2048; i++) CHECK(write(mfd, pg, PAGE) == PAGE, "memfd write");
	char *f = mmap(NULL, 8 * MIB, PROT_READ, MAP_SHARED, mfd, 0);
	CHECK(f != MAP_FAILED, "memfd map");
	close(mfd);
	CHECK(used() >= base + 8 * MIB, "mapped past close, still charged");
	munmap(f, 8 * MIB);
	CHECK(used() <= base + SLACK, "unmap frees and uncharges");

	/* a page shared by fork is charged once, and stays charged past its first owner */
	char *cow = anon(8 * MIB);
	touch(cow, 8 * MIB);
	unsigned long long one = used();
	pid_t c = fork();
	if (c == 0) {
		CHECK(used() <= one + SLACK, "fork charges only its tables");
		munmap(cow, 8 * MIB);
		CHECK(used() >= base + 8 * MIB, "the parent still holds the pages");
		_exit(fails ? 1 : 0);
	}
	int st;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child's view");
	munmap(cow, 8 * MIB);
	CHECK(used() <= base + SLACK, "the last unmap uncharges");
}

/* ── limits ───────────────────────────────────────────────── */

static void eat_anon(void)
{
	long len = 4 * LIMIT;
	char *m = anon(len);
	touch(m, len);
	_exit(9);   /* not reached: the fault past the limit is refused */
}

/* page tables count: scattered pages cost twice their size */
static void eat_sparse(void)
{
	long len = 256L * LIMIT;
	char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) _exit(8);
	for (long i = 0; i < len; i += 2 * MIB) {
		m[i] = 1;
		if ((i / (2 * MIB)) * 2 * PAGE > LIMIT + LIMIT / 4) _exit(9);
	}
	_exit(9);
}

static void limits(void)
{
	unsigned long long base = used();

	int st = in_child(eat_anon);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "fault past the limit is refused");
	CHECK(used() <= base + SLACK, "a dead child gives back");

	st = in_child(eat_sparse);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "page tables hit the limit");
	CHECK(used() <= base + SLACK, "a dead child gives back its tables");

	char *top = brk_at(0);
	long got = 0;
	while (brk_at(top + got + MIB) == top + got + MIB) got += MIB;
	CHECK(got < LIMIT && got > LIMIT / 2, "brk stops at the limit");
	CHECK(brk_at(top) == top, "brk back");
	CHECK(used() <= base + SLACK, "brk shrink gives back");

	static char pg[PAGE];
	int mfd = (int)syscall(SYS_memfd_create, "t", 0);
	long n = 0;
	for (;;) {
		ssize_t w = write(mfd, pg, PAGE);
		if (w != PAGE) { CHECK(errno == ENOSPC, "memfd past the limit is ENOSPC"); break; }
		n += w;
	}
	CHECK(n > 0 && n <= LIMIT, "memfd stops at the limit");
	close(mfd);
	CHECK(used() <= base + SLACK, "memfd close gives back");

	int fd = open("/tmp/f", O_CREAT | O_WRONLY, 0644);
	n = 0;
	for (;;) {
		ssize_t w = write(fd, pg, PAGE);
		if (w != PAGE) { CHECK(errno == ENOSPC, "tmpfs past the limit is ENOSPC"); break; }
		n += w;
	}
	CHECK(n > 0 && n <= LIMIT, "tmpfs stops at the budget, under its size");
	close(fd);
	unlink("/tmp/f");
	CHECK(used() <= base + SLACK, "unlink gives back");

	int ids[8], nseg = 0;
	while (nseg < 8) {
		ids[nseg] = shmget(IPC_PRIVATE, 8 * MIB, IPC_CREAT | 0600);
		if (ids[nseg] < 0) { CHECK(errno == ENOMEM, "shmget past the limit"); break; }
		nseg++;
	}
	CHECK(nseg >= 2 && nseg < 8, "shm stops at the limit");
	for (int i = 0; i < nseg; i++) CHECK(shmctl(ids[i], IPC_RMID, NULL) == 0, "shm rmid");
	CHECK(used() <= base + SLACK, "IPC_RMID gives back");

	/* survive the attach of a segment by an exiting process */
	int id = shmget(IPC_PRIVATE, MIB, IPC_CREAT | 0600);
	char *s = shmat(id, NULL, 0);
	CHECK(s != (void *)-1, "shmat");
	s[5] = 7;
	pid_t c = fork();
	if (c == 0) { s[6] = 8; _exit(0); }
	waitpid(c, NULL, 0);
	CHECK(s[5] == 7 && s[6] == 8, "shm stays shared and alive across a child's exit");
	errno = 0;
	void *bad = shmat(id, (void *)0xffff800000000000UL, 0);
	if (bad != (void *)-1 || errno != EINVAL) {
		printf("FAIL shmat at a kernel address: %p errno %d\n", bad, errno);
		fails++;
	}
	int nul = open("/dev/zero", O_RDONLY);
	DENIED(read(nul, (void *)0xffff800000100000UL, 8), EFAULT, "read into the direct map");
	close(nul);
	CHECK(shmdt(s) == 0, "shmdt");
	shmctl(id, IPC_RMID, NULL);

	st = in_child(eat_anon);
	CHECK(WIFSIGNALED(st), "the limit still holds afterwards");
}

/* a chain of forks, each parent exiting: nothing is left charged to nobody */
static void chain(int done)
{
	unsigned long long max;
	struct asess_info in;
	asess(ASESS_INFO, -1, &in, sizeof(in));
	max = in.mem_max;
	for (;;) {
		char *m = anon(4 * MIB);
		if (m == MAP_FAILED) break;
		/* a refused fault would end the chain with SIGSEGV: stop short of it */
		if (used() + 4 * MIB + SLACK > max) break;
		touch(m, 4 * MIB);
		pid_t p = fork();
		if (p < 0) { CHECK(errno == ENOMEM, "fork at the limit is ENOMEM"); break; }
		if (p > 0) _exit(0);
	}
	CHECK(used() <= max, "chain within the limit");
	CHECK(write(done, "x", 1) == 1, "chain done");
	char c;
	read(done, &c, 1);
	_exit(0);
}

static void run_chain(void)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	int keep[] = { 1, sv[1] };
	long before = free_kb();
	long sfd = sfd_create(LIMIT, keep, 2);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		/* pid 1 stays: its exit would end the session */
		if (fork() == 0) chain(sv[1]);
		for (;;) pause();
	}
	close(sv[1]);
	char c = 0;
	CHECK(read(sv[0], &c, 1) == 1, "chain reports");
	long held_kb = before - free_kb();
	printf("fork chain holds %ld KB under a %ld KB limit\n", held_kb, LIMIT / 1024);
	CHECK(held_kb <= (LIMIT + 8 * MIB) / 1024, "a fork chain stays inside the limit");
	close((int)sfd);
	close(sv[0]);
	waitpid((pid_t)pid, NULL, 0);
}

/* ── the host ─────────────────────────────────────────────── */

static void eat_all(int done)
{
	static char pg[PAGE];
	int mfd = (int)syscall(SYS_memfd_create, "t", 0);
	while (write(mfd, pg, PAGE) == PAGE) ;
	int fd = open("/tmp/f", O_CREAT | O_WRONLY, 0644);
	while (write(fd, pg, PAGE) == PAGE) ;
	/* brk fails softly at the limit, so what it took stays held */
	char *top = brk_at(0);
	long got = 0;
	while (brk_at(top + got + MIB) == top + got + MIB) got += MIB;
	CHECK(write(done, "x", 1) == 1, "report");
	char b;
	read(done, &b, 1);
	_exit(0);
}

static void read_file(const char *path, char *out, size_t sz)
{
	int fd = open(path, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, out, sz - 1) : -1;
	if (fd >= 0) close(fd);
	out[n > 0 ? n : 0] = 0;
}

static void host_survives(void)
{
	int sv[2][2];
	long sfd[2], pid[2];
	long before = free_kb();

	pid_t sentinel = fork();
	if (sentinel == 0) for (;;) pause();

	for (int i = 0; i < 2; i++) {
		CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) == 0, "socketpair");
		int keep[] = { 1, sv[i][1] };
		sfd[i] = sfd_create(0, keep, 2);
		CHECK(sfd[i] >= 0, "create with the default budget");
		pid[i] = sess_must_spawn(sfd[i]);
		if (pid[i] == 0) eat_all(sv[i][1]);
		close(sv[i][1]);
	}

	struct asess_info in;
	CHECK(asess(ASESS_INFO, (int)sfd[0], &in, sizeof(in)) == 0, "info from outside");
	CHECK(in.mem_max >= ASESS_MEM_FLOOR_MIB * MIB, "a default budget");

	/* cgroupfs shows it, and an open file goes stale with the session */
	char path[80], buf[64];
	int stale = -1;
	mkdir("/cgroup", 0755);
	mount("c", "/cgroup", "cgroup2", 0, NULL);
	snprintf(path, sizeof(path), "/cgroup/asess/%llu/memory.max", in.id);
	read_file(path, buf, sizeof(buf));
	if (buf[0]) {
		CHECK((unsigned long long)atoll(buf) == in.mem_max, "memory.max shows the budget");
		stale = open(path, O_RDONLY);
	}

	char c;
	for (int i = 0; i < 2; i++) CHECK(read(sv[i][0], &c, 1) == 1, "session reports");
	long left = free_kb();
	printf("two default sessions took %ld KB, %ld KB free\n", before - left, left);
	long ram = meminfo_kb("MemTotal:");
	long reserve = ram / 4 < 1024 * 1024 ? ram / 4 : 1024 * 1024;
	CHECK(before - left <= ram - reserve + SLACK * 4 / 1024, "sessions together leave the host its share");
	CHECK(kill(sentinel, 0) == 0, "the host's task is alive");

	/* and the host can still allocate */
	char *m = anon(8 * MIB);
	CHECK(m != MAP_FAILED, "host mmap");
	touch(m, 8 * MIB);
	munmap(m, 8 * MIB);

	for (int i = 0; i < 2; i++) {
		close((int)sfd[i]);
		close(sv[i][0]);
		waitpid((pid_t)pid[i], NULL, 0);
	}
	kill(sentinel, SIGKILL);
	waitpid(sentinel, NULL, 0);

	/* the tasks are gone a moment after pid 1: give the teardown time */
	long after = 0;
	for (int i = 0; i < 50; i++) {
		after = free_kb();
		if (before - after < 4096) break;
		usleep(100000);
	}
	printf("free %ld -> %ld KB after both sessions\n", before, after);
	CHECK(before - after < 4096, "everything comes back");
	if (stale >= 0) {
		errno = 0;
		CHECK(read(stale, buf, sizeof(buf)) == -1 && errno == ENODEV, "a stale cgroup file");
		close(stale);
		CHECK(access(path, F_OK) != 0, "the cgroup is gone");
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	static const int keep[] = { 1 };

	long sfd = sfd_create(LIMIT, keep, 1);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		give_back();
		limits();
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);
	close((int)sfd);
	run_chain();
	host_survives();
	return finish();
}
