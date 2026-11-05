/* What a crash may and may not cost a disk filesystem. "run DIR" keeps four
 * processes writing until killed or reset, recording in fsynced logs what the
 * kernel has promised. "check DIR", possibly on the next boot or under another
 * kernel, proves each promise held:
 *   - a file fsynced and logged is whole, byte for byte
 *   - a rename onto a live name after fsync leaves the old file or the new one
 *   - a block overwritten and fsynced holds that generation or a later one
 * The fourth process only churns metadata without fsync, for fsck to judge.
 * "smoke DIR" runs it for a few seconds, kills it and checks. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLK        4096
#define OVER_BLKS  256
#define ATOM_SIZE  (32u * 1024)

static uint8_t buf[256 * 1024];
static uint32_t max_file = 2u << 20;
static uint32_t keep = 64;        /* durable files alive at once */
static int      big_chunks = 64;  /* of buf, per streamed file */

static uint64_t rnd(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

/* Bytes are a function of (seed, offset), so a chunk can be checked alone. */
static void fill(uint8_t *p, size_t n, uint64_t seed, uint64_t off)
{
	uint64_t s = seed * 0x9E3779B97F4A7C15ull + (off / 8) * 0xD1342543DE82EF95ull + 1;
	for (size_t i = 0; i < n; ) {
		uint64_t v = rnd(&s);
		for (int j = (int)((off + i) & 7); j < 8 && i < n; j++, i++) p[i] = (uint8_t)(v >> (j * 8));
	}
}

static void die(const char *what)
{
	printf("FAIL %s errno=%d\n", what, errno);
	fflush(stdout);
	_exit(1);
}

static void put_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t r = write(fd, b, n);
		if (r <= 0) die("write");
		b += r; n -= (size_t)r;
	}
}

static void sync_fd(int fd) { if (fsync(fd) != 0) die("fsync"); }

static void sync_dir(void)
{
	int d = open(".", O_RDONLY);
	if (d < 0) die("open .");
	sync_fd(d);
	close(d);
}

static void logline(int fd, const char *s)
{
	put_all(fd, s, strlen(s));
	sync_fd(fd);
}

static void work_durable(void)
{
	int log = open("durable.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (log < 0) die("open durable.log");
	uint64_t s = (uint64_t)time(NULL) * 2 + 1;
	uint64_t n = s % 1000000000ull * 1000;
	char name[64], line[64];
	for (uint32_t i = 0;; i++, n++) {
		uint32_t size = (uint32_t)(rnd(&s) % max_file) + 1;
		snprintf(name, sizeof(name), "f%llu", (unsigned long long)n);
		int fd = open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd < 0) die("create");
		for (uint32_t off = 0; off < size; ) {
			uint32_t c = size - off < sizeof(buf) ? size - off : (uint32_t)sizeof(buf);
			fill(buf, c, n, off);
			put_all(fd, buf, c);
			off += c;
		}
		sync_fd(fd);
		close(fd);
		sync_dir();
		snprintf(line, sizeof(line), "+ %llu %u\n", (unsigned long long)n, size);
		logline(log, line);
		if (i >= keep) {
			snprintf(name, sizeof(name), "f%llu", (unsigned long long)(n - keep));
			snprintf(line, sizeof(line), "- %llu\n", (unsigned long long)(n - keep));
			logline(log, line);
			if (unlink(name) != 0) die("unlink");
		}
	}
}

static void work_atomic(void)
{
	int log = open("atomic.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (log < 0) die("open atomic.log");
	char line[32];
	static uint8_t b[ATOM_SIZE];
	for (uint64_t g = (uint64_t)time(NULL) * 1000;; g++) {
		memcpy(b, &g, 8);
		fill(b + 8, sizeof(b) - 8, g, 8);
		int fd = open("cur.tmp", O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd < 0) die("create tmp");
		put_all(fd, b, sizeof(b));
		sync_fd(fd);
		close(fd);
		if (rename("cur.tmp", "cur") != 0) die("rename");
		sync_dir();
		snprintf(line, sizeof(line), "%llu\n", (unsigned long long)g);
		logline(log, line);
	}
}

static void over_block(uint8_t *b, uint32_t k, uint64_t gen)
{
	memcpy(b, &gen, 8);
	memcpy(b + 8, &k, 4);
	fill(b + 12, BLK - 12, gen ^ k, 12);
}

static void work_over(void)
{
	int fd = open("over.dat", O_CREAT | O_RDWR, 0644);
	int log = open("over.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (fd < 0 || log < 0) die("open over");
	static uint8_t b[BLK];
	struct stat st;
	if (fstat(fd, &st) != 0) die("fstat");
	if (st.st_size < OVER_BLKS * BLK) {
		for (uint32_t k = 0; k < OVER_BLKS; k++) {
			over_block(b, k, 0);
			if (pwrite(fd, b, BLK, (off_t)k * BLK) != BLK) die("pwrite");
		}
		sync_fd(fd);
		sync_dir();
		logline(log, "ready\n");
	}
	uint64_t s = (uint64_t)time(NULL) * 2 + 3;
	char line[64];
	for (uint64_t g = (uint64_t)time(NULL) * 1000;; g++) {
		uint32_t k = (uint32_t)(rnd(&s) % OVER_BLKS);
		over_block(b, k, g);
		if (pwrite(fd, b, BLK, (off_t)k * BLK) != BLK) die("pwrite");
		sync_fd(fd);
		snprintf(line, sizeof(line), "%u %llu\n", k, (unsigned long long)g);
		logline(log, line);
	}
}

static void small(const char *p, uint64_t *s)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) return;
	size_t n = rnd(s) % 12000;
	fill(buf, n, *s, 0);
	if (write(fd, buf, n) < 0) { /* full disk is part of the game */ }
	close(fd);
}

static void work_churn(void)
{
	uint64_t s = (uint64_t)time(NULL) * 2 + 5;
	char a[64], b[64];
	if (mkdir("churn", 0755) != 0 && errno != EEXIST) die("mkdir churn");
	/* unlinked and open: a crash leaves it for the next mount to release */
	int held = open("churn/held", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (held >= 0 && unlink("churn/held") != 0) die("unlink held");
	for (;;) {
		if (held >= 0 && write(held, "held", 4) < 0) { /* full disk */ }
		uint32_t d = (uint32_t)(rnd(&s) % 8), f = (uint32_t)(rnd(&s) % 300);
		snprintf(a, sizeof(a), "churn/d%u", d);
		mkdir(a, 0755);
		snprintf(a, sizeof(a), "churn/d%u/x%u", d, f);
		snprintf(b, sizeof(b), "churn/d%u/y%u", (d + 1) % 8, f);
		switch (rnd(&s) % 8) {
		case 0: case 1: small(a, &s); break;
		case 2: rename(a, b); break;
		case 3: if (link(a, b) != 0) unlink(b); break;
		case 4: unlink(a); break;
		case 5: if (symlink("../some/target/name", a) != 0) unlink(a); break;
		case 6: if (truncate(a, (off_t)(rnd(&s) % 20000))) { /* a may not exist */ } break;
		case 7: {
			/* streams enough to keep the write-back busy across commits */
			snprintf(a, sizeof(a), "churn/big%u", d);
			int fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) break;
			fill(buf, sizeof(buf), s, 0);
			for (int i = 0; i < big_chunks; i++)
				if (write(fd, buf, sizeof(buf)) < 0) break;
			close(fd);
			break;
		}
		}
	}
}

static pid_t pids[4];

static void run(void)
{
	void (*w[4])(void) = { work_durable, work_atomic, work_over, work_churn };
	for (int i = 0; i < 4; i++) {
		pids[i] = fork();
		if (pids[i] < 0) die("fork");
		if (pids[i] == 0) { w[i](); _exit(0); }
	}
}

static void *slurp(const char *p, size_t *n)
{
	*n = 0;
	int fd = open(p, O_RDONLY);
	if (fd < 0) return NULL;
	struct stat st;
	if (fstat(fd, &st) != 0) { close(fd); return NULL; }
	char *m = malloc((size_t)st.st_size + 1);
	if (!m) { close(fd); return NULL; }
	size_t got = 0;
	while (got < (size_t)st.st_size) {
		ssize_t r = read(fd, m + got, (size_t)st.st_size - got);
		if (r <= 0) break;
		got += (size_t)r;
	}
	close(fd);
	/* a line the crash tore was never promised */
	while (got && m[got - 1] != '\n') got--;
	m[got] = 0;
	*n = got;
	return m;
}

static int bad;

static int check_durable(uint64_t *seen)
{
	size_t n;
	char *m = slurp("durable.log", &n);
	if (!m) return 0;
	static uint8_t want[sizeof(buf)];
	char name[64];
	size_t cap = n / 8 + 1, cnt = 0;
	uint64_t *ids = malloc(cap * 8);
	uint32_t *szs = malloc(cap * 4);
	for (char *l = strtok(m, "\n"); l; l = strtok(NULL, "\n")) {
		unsigned long long id; unsigned sz;
		if (sscanf(l, "+ %llu %u", &id, &sz) == 2) { ids[cnt] = id; szs[cnt++] = sz; }
		else if (sscanf(l, "- %llu", &id) == 1) {
			for (size_t i = cnt; i--; ) if (ids[i] == id) { ids[i] = 0; break; }
		}
	}
	for (size_t i = 0; i < cnt; i++) {
		if (!ids[i]) continue;
		snprintf(name, sizeof(name), "f%llu", (unsigned long long)ids[i]);
		int fd = open(name, O_RDONLY);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) != 0 || (uint64_t)st.st_size != szs[i]) {
			printf("FAIL durable %s size=%lld want %u errno=%d\n", name,
			       fd < 0 ? -1LL : (long long)st.st_size, szs[i], errno);
			bad++;
			if (fd >= 0) close(fd);
			continue;
		}
		for (uint32_t off = 0; off < szs[i]; ) {
			uint32_t c = szs[i] - off < sizeof(buf) ? szs[i] - off : (uint32_t)sizeof(buf);
			if (read(fd, buf, c) != (ssize_t)c) { printf("FAIL durable %s short read\n", name); bad++; break; }
			fill(want, c, ids[i], off);
			if (memcmp(buf, want, c)) { printf("FAIL durable %s differs near %u\n", name, off); bad++; break; }
			off += c;
		}
		close(fd);
		(*seen)++;
	}
	free(m); free(ids); free(szs);
	return 1;
}

static uint64_t atom_ok(const char *p)
{
	static uint8_t b[ATOM_SIZE], want[ATOM_SIZE];
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t r = read(fd, b, sizeof(b));
	close(fd);
	if (r != (ssize_t)sizeof(b)) return 0;
	uint64_t g;
	memcpy(&g, b, 8);
	fill(want + 8, sizeof(want) - 8, g, 8);
	return memcmp(b + 8, want + 8, sizeof(b) - 8) ? 0 : g;
}

static void check_atomic(void)
{
	size_t n;
	char *m = slurp("atomic.log", &n);
	unsigned long long last = 0;
	if (m) {
		for (char *l = strtok(m, "\n"); l; l = strtok(NULL, "\n")) last = strtoull(l, NULL, 10);
		free(m);
	}
	uint64_t g = atom_ok("cur");
	if (last && !g) { printf("FAIL atomic: cur torn or missing, last logged %llu\n", last); bad++; }
	else if (g < last) { printf("FAIL atomic: cur %llu older than logged %llu\n", (unsigned long long)g, last); bad++; }
}

static void check_over(void)
{
	size_t n;
	char *m = slurp("over.log", &n);
	if (!m || !strstr(m, "ready")) { free(m); return; }
	static uint64_t last[OVER_BLKS];
	for (char *l = strtok(m, "\n"); l; l = strtok(NULL, "\n")) {
		unsigned k; unsigned long long g;
		if (sscanf(l, "%u %llu", &k, &g) == 2 && k < OVER_BLKS) last[k] = g;
	}
	free(m);
	int fd = open("over.dat", O_RDONLY);
	if (fd < 0) { printf("FAIL over.dat missing\n"); bad++; return; }
	static uint8_t b[BLK], w[BLK];
	int torn = 0;
	for (uint32_t k = 0; k < OVER_BLKS; k++) {
		if (pread(fd, b, BLK, (off_t)k * BLK) != BLK) { printf("FAIL over.dat short\n"); bad++; break; }
		uint64_t g; uint32_t kk;
		memcpy(&g, b, 8); memcpy(&kk, b + 8, 4);
		over_block(w, k, g);
		if (kk != k || memcmp(b, w, BLK)) torn++;   /* the one write in flight may tear */
		else if (g < last[k]) { printf("FAIL over block %u gen %llu older than logged %llu\n", k, (unsigned long long)g, (unsigned long long)last[k]); bad++; }
	}
	close(fd);
	if (torn > 1) { printf("FAIL over: %d torn blocks, at most one write was in flight\n", torn); bad++; }
}

static int check(void)
{
	uint64_t seen = 0;
	bad = 0;
	check_durable(&seen);
	check_atomic();
	check_over();
	if (bad) { printf("FAIL %d broken promises\n", bad); return 1; }
	printf("OK durable=%llu\n", (unsigned long long)seen);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 3 || chdir(argv[2]) != 0) { printf("FAIL usage: run|check|smoke DIR\n"); return 2; }
	if (!strcmp(argv[1], "check")) return check();
	if (!strcmp(argv[1], "run")) {
		run();
		for (int i = 0; i < 4; i++) wait(NULL);
		return 0;
	}
	if (!strcmp(argv[1], "smoke")) {
		max_file = 64 * 1024; keep = 8; big_chunks = 4;   /* test disks are small */
		run();
		sleep(argc > 3 ? (unsigned)atoi(argv[3]) : 4);
		for (int i = 0; i < 4; i++) kill(pids[i], SIGKILL);
		for (int i = 0; i < 4; i++) wait(NULL);
		return check();
	}
	return 2;
}
