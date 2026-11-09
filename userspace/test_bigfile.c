/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* tmpfs and ramfs files past the largest single allocation: whole, sparse,
 * truncated, mapped, executed, and charged to the right session. Two writers
 * growing one file must neither lose data nor miscount it. */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include "asess_test.h"

#define MIB   (1024L * 1024)
#define PAGE  4096L
#define BIG   (64 * MIB)
#define SLACK (2 * MIB)
#define MNT   "/tmp/bigmnt"

static char pg[PAGE];

static void fill(long off)
{
	long *w = (long *)pg;
	for (size_t i = 0; i < PAGE / sizeof(long); i++) w[i] = off + (long)i * 8;
}

static long tmpfs_used(const char *dir)
{
	struct statfs s;
	if (statfs(dir, &s) != 0) return -1;
	return (long)(s.f_blocks - s.f_bfree) * s.f_bsize;
}

static void write_big(int fd)
{
	for (long off = 0; off < BIG; off += PAGE) {
		fill(off);
		if (write(fd, pg, PAGE) != PAGE) { CHECK(0, "write past 32 MiB"); return; }
	}
}

static void verify_big(int fd, long len)
{
	static char b[PAGE];
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "rewind");
	for (long off = 0; off < len; off += PAGE) {
		fill(off);
		if (read(fd, b, PAGE) != PAGE || memcmp(b, pg, PAGE)) {
			printf("FAIL contents at %ld\n", off);
			fails++;
			return;
		}
	}
}

static int all_zero(const char *p, long n)
{
	for (long i = 0; i < n; i++) if (p[i]) return 0;
	return 1;
}

static void whole_file(const char *dir, int counted)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/whole", dir);
	long before = counted ? tmpfs_used(dir) : 0;
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	CHECK(fd >= 0, "create");
	write_big(fd);
	struct stat st;
	CHECK(fstat(fd, &st) == 0 && st.st_size == BIG, "64 MiB size");
	verify_big(fd, BIG);
	if (counted) {
		long u = tmpfs_used(dir) - before;
		CHECK(u >= BIG && u <= BIG + SLACK, "size accounting matches the data");
	}

	CHECK(ftruncate(fd, MIB + 100) == 0, "truncate down");
	if (counted) {
		long u = tmpfs_used(dir) - before;
		CHECK(u <= MIB + SLACK, "truncate gives pages back");
	}
	CHECK(ftruncate(fd, 4 * MIB) == 0, "truncate up");
	static char b[PAGE];
	CHECK(pread(fd, b, PAGE, MIB) == PAGE, "read across the old end");
	CHECK(all_zero(b + 100, PAGE - 100), "no old bytes past the truncate, same page");
	fill(MIB);
	CHECK(!memcmp(b, pg, 100), "bytes before the truncate kept");
	CHECK(pread(fd, b, PAGE, 3 * MIB) == PAGE && all_zero(b, PAGE), "no old bytes further out");
	close(fd);
	CHECK(unlink(path) == 0, "unlink");
	if (counted) CHECK(tmpfs_used(dir) == before, "unlink gives everything back");
}

static void sparse(const char *dir, int counted)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/sparse", dir);
	long before = counted ? tmpfs_used(dir) : 0;
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	CHECK(fd >= 0, "create");
	CHECK(pwrite(fd, "z", 1, 60 * MIB) == 1, "write far out");
	CHECK(pwrite(fd, "a", 1, 0) == 1, "write at the start");
	struct stat st;
	CHECK(fstat(fd, &st) == 0 && st.st_size == 60 * MIB + 1, "sparse size");
	if (counted) CHECK(tmpfs_used(dir) - before <= 8 * PAGE, "a hole costs nothing");

	static char b[PAGE];
	CHECK(pread(fd, b, PAGE, 30 * MIB) == PAGE && all_zero(b, PAGE), "hole reads zero");
	CHECK(pread(fd, b, 2, 60 * MIB - 1) == 2 && b[0] == 0 && b[1] == 'z', "read into the data");

	/* far past any real file: only the index for that one page */
	CHECK(pwrite(fd, "y", 1, 1L << 40) == 1, "write at 1 TiB");
	if (counted) CHECK(tmpfs_used(dir) - before <= 16 * PAGE, "1 TiB offset costs a few pages");
	CHECK(ftruncate(fd, 1L << 42) == 0, "4 TiB ftruncate");
	if (counted) CHECK(tmpfs_used(dir) - before <= 16 * PAGE, "sparse ftruncate costs nothing");
	CHECK(pread(fd, b, 1, 1L << 40) == 1 && b[0] == 'y', "read back from 1 TiB");
	DENIED(pwrite(fd, "n", 1, 1L << 50), EFBIG, "past the size limit");

	CHECK(ftruncate(fd, 0) == 0, "truncate to nothing");
	if (counted) CHECK(tmpfs_used(dir) == before, "truncate frees the index too");
	close(fd);
	unlink(path);
}

static void mapped(const char *dir)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/map", dir);
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	CHECK(fd >= 0, "create");
	write_big(fd);

	char *m = mmap(NULL, BIG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(m != MAP_FAILED, "map 64 MiB shared");
	if (m == MAP_FAILED) { close(fd); return; }
	fill(40 * MIB);
	CHECK(!memcmp(m + 40 * MIB, pg, PAGE), "read through the map");
	memcpy(m + 50 * MIB, "stored", 6);
	char b[8] = {0};
	CHECK(pread(fd, b, 6, 50 * MIB) == 6 && !memcmp(b, "stored", 6), "store reaches read()");
	CHECK(pwrite(fd, "pwrite", 6, 51 * MIB) == 6 && !memcmp(m + 51 * MIB, "pwrite", 6),
	      "write() reaches the map");

	/* this used to be ENOMEM: the buffer could not move while mapped */
	CHECK(pwrite(fd, "grow", 4, BIG + MIB) == 4, "grow under a live mapping");
	CHECK(!memcmp(m + 50 * MIB, "stored", 6), "map survives the grow");
	memcpy(m + 2 * PAGE, "after", 5);
	CHECK(pread(fd, b, 5, 2 * PAGE) == 5 && !memcmp(b, "after", 5), "still the same pages");

	/* shrink and regrow: the page is gone for read() and comes back zero and shared */
	CHECK(ftruncate(fd, PAGE) == 0 && ftruncate(fd, BIG) == 0, "shrink and regrow mapped");
	CHECK(all_zero(m + 50 * MIB, 6), "mapping sees the truncate");
	memcpy(m + 50 * MIB, "again", 5);
	CHECK(pread(fd, b, 5, 50 * MIB) == 5 && !memcmp(b, "again", 5), "mapping and file still shared");
	munmap(m, BIG);

	/* a hole mapped shared, stored into */
	int hf = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	CHECK(ftruncate(hf, 40 * MIB) == 0, "sparse file");
	m = mmap(NULL, 40 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, hf, 0);
	CHECK(m != MAP_FAILED, "map a hole");
	if (m != MAP_FAILED) {
		CHECK(m[20 * MIB] == 0, "hole maps as zero");
		m[20 * MIB] = 'h';
		CHECK(pread(hf, b, 1, 20 * MIB) == 1 && b[0] == 'h', "store into a hole lands in the file");
		munmap(m, 40 * MIB);
	}

	/* private: a copy, whatever happens to the file */
	CHECK(pwrite(hf, "orig", 4, 33 * MIB) == 4, "marker");
	m = mmap(NULL, 40 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE, hf, 0);
	CHECK(m != MAP_FAILED, "map private");
	if (m != MAP_FAILED) {
		CHECK(!memcmp(m + 33 * MIB, "orig", 4), "private map sees the data");
		memcpy(m + 33 * MIB, "priv", 4);
		CHECK(pread(hf, b, 4, 33 * MIB) == 4 && !memcmp(b, "orig", 4), "private store stays private");
		munmap(m, 40 * MIB);
	}
	close(hf);
	close(fd);
	unlink(path);
}

/* A binary with a PT_LOAD above the old limit. Its code exits with the last word
 * of that segment, so the status proves the segment came out of the file. */
static void big_exec(const char *dir)
{
#ifdef __x86_64__
	enum { DATA = 40 * MIB, CODE_OFF = 0x1000, DATA_OFF = 0x2000 };
	const unsigned long code_va = 0x400000, data_va = 0x1000000;
	char path[64];
	snprintf(path, sizeof(path), "%s/bigbin", dir);
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0755);
	CHECK(fd >= 0, "create binary");

	struct { Elf64_Ehdr e; Elf64_Phdr p[2]; } h;
	memset(&h, 0, sizeof(h));
	memcpy(h.e.e_ident, ELFMAG, SELFMAG);
	h.e.e_ident[EI_CLASS] = ELFCLASS64;
	h.e.e_ident[EI_DATA] = ELFDATA2LSB;
	h.e.e_ident[EI_VERSION] = EV_CURRENT;
	h.e.e_type = ET_EXEC;
	h.e.e_machine = EM_X86_64;
	h.e.e_version = EV_CURRENT;
	h.e.e_entry = code_va;
	h.e.e_phoff = sizeof(h.e);
	h.e.e_ehsize = sizeof(h.e);
	h.e.e_phentsize = sizeof(h.p[0]);
	h.e.e_phnum = 2;
	h.p[0] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_X, CODE_OFF, code_va, code_va, 32, 32, PAGE };
	h.p[1] = (Elf64_Phdr){ PT_LOAD, PF_R | PF_W, DATA_OFF, data_va, data_va, DATA, DATA, PAGE };

	unsigned long last = data_va + DATA - 8;
	unsigned char code[32] = {
		0x48, 0xa1, 0, 0, 0, 0, 0, 0, 0, 0,   /* movabs rax, [last] */
		0x89, 0xc7,                           /* mov edi, eax */
		0xb8, 0x3c, 0, 0, 0,                  /* mov eax, SYS_exit */
		0x0f, 0x05,                           /* syscall */
	};
	memcpy(code + 2, &last, 8);
	unsigned long want = 42;
	CHECK(pwrite(fd, &h, sizeof(h), 0) == sizeof(h), "ELF header");
	CHECK(pwrite(fd, code, sizeof(code), CODE_OFF) == sizeof(code), "code");
	CHECK(pwrite(fd, &want, 8, DATA_OFF + DATA - 8) == 8, "data");
	close(fd);

	struct stat st;
	CHECK(stat(path, &st) == 0 && st.st_size == DATA_OFF + DATA, "binary size");
	pid_t pid = fork();
	if (pid == 0) {
		execl(path, path, NULL);
		_exit(99);
	}
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid, "wait");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
		printf("FAIL 42 MiB binary: status %x\n", status);
		fails++;
	}
	unlink(path);
#else
	(void)dir;
#endif
}

/* The file's charge follows the same rules on the way up and down. */
static void budget(void)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = UID;
	a.gid = GID;
	a.root = (unsigned long long)(unsigned long)"/";
	a.mem_max = 48 * MIB;
	long sfd = asess(ASESS_CREATE, -1, &a, sizeof(a));
	CHECK(sfd >= 0, "session");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		struct asess_info in;
#define USED() (asess(ASESS_INFO, -1, &in, sizeof(in)) == 0 ? (long)in.mem_cur : -1)
		long base = USED();
		int fd = open("/tmp/b", O_CREAT | O_RDWR, 0644);
		int bad = fd < 0;
		bad |= (pwrite(fd, "x", 1, 40 * MIB) != 1) << 1;
		bad |= (USED() > base + 8 * PAGE) << 2;
		long left = 4 * MIB;
		for (long o = 0; o < left; o += PAGE) bad |= (pwrite(fd, pg, PAGE, o) != PAGE) << 3;
		bad |= (USED() < base + left) << 4;
		long n = 0;
		ssize_t w;
		while ((w = pwrite(fd, pg, PAGE, n)) == PAGE) n += PAGE;
		bad |= (errno != ENOSPC) << 5;
		bad |= (n >= 48 * MIB || n < 8 * MIB) << 6;
		bad |= (ftruncate(fd, MIB) != 0) << 7;
		bad |= (USED() > base + MIB + SLACK) << 8;
		close(fd);
		bad |= (unlink("/tmp/b") != 0) << 9;
		bad |= (USED() > base + SLACK) << 10;
		if (bad) printf("FAIL budget bits %x\n", bad);
		_exit(bad != 0);
	}
	sess_reap((pid_t)pid);
	close((int)sfd);
}

#define WRITERS 2
#define RECORD  1000
#define RECORDS 6000

static void two_writers(const char *dir)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/race", dir);
	unlink(path);
	long before = tmpfs_used(dir);
	pid_t pid[WRITERS];
	for (int i = 0; i < WRITERS; i++) {
		pid[i] = fork();
		if (pid[i] == 0) {
			static char rec[RECORD];
			int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
			memset(rec, 'a' + i, sizeof(rec));
			for (int r = 0; r < RECORDS; r++)
				if (write(fd, rec, RECORD) != RECORD) _exit(1);
			_exit(0);
		}
	}
	for (int i = 0; i < WRITERS; i++) {
		int st;
		waitpid(pid[i], &st, 0);
		CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "writer");
	}
	struct stat st;
	CHECK(stat(path, &st) == 0 && st.st_size == WRITERS * RECORD * RECORDS, "no append lost");
	int fd = open(path, O_RDONLY);
	static char rec[RECORD];
	long count[WRITERS] = {0};
	for (long r = 0; r < WRITERS * RECORDS; r++) {
		if (read(fd, rec, RECORD) != RECORD) { CHECK(0, "read record"); break; }
		int w = rec[0] - 'a';
		int ok = w >= 0 && w < WRITERS;
		for (int i = 1; ok && i < RECORD; i++) ok = rec[i] == rec[0];
		if (!ok) { printf("FAIL torn record %ld\n", r); fails++; break; }
		count[w]++;
	}
	CHECK(count[0] == RECORDS && count[1] == RECORDS, "every record survived");
	close(fd);
	long pages = (WRITERS * RECORD * RECORDS + PAGE - 1) / PAGE;
	long u = tmpfs_used(dir) - before;
	if (u < pages * PAGE || u > (pages + 8) * PAGE) {
		printf("FAIL charged %ld bytes for %ld pages\n", u, pages);
		fails++;
	}
	unlink(path);
	CHECK(tmpfs_used(dir) == before, "no charge left behind");
}

/* O_CREAT without O_EXCL opens the file whoever got there first */
static void create_race(const char *dir)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/creat", dir);
	for (int round = 0; round < 300 && !fails; round++) {
		unlink(path);
		pid_t pid = fork();
		int fd = open(path, O_CREAT | O_WRONLY, 0644);
		if (pid == 0) _exit(fd < 0);
		int st;
		CHECK(fd >= 0, "create race, parent");
		waitpid(pid, &st, 0);
		CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "create race, child");
		close(fd);
	}
	unlink(path);
}

int main(void)
{
	alarm(150);
	mkdir(MNT, 0755);
	CHECK(mount("none", MNT, "tmpfs", 0, "size=200m") == 0, "mount tmpfs");

	create_race(MNT);
	create_race("/root");
	whole_file(MNT, 1);
	sparse(MNT, 1);
	mapped(MNT);
	big_exec(MNT);
	two_writers(MNT);

	/* the root here is ramfs */
	whole_file("/root", 0);
	sparse("/root", 0);
	big_exec("/root");

	budget();

	CHECK(umount(MNT) == 0, "umount");
	return finish();
}
