/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Swap as a normal OS has it (include/kernel/mm/swap.h). Each mode brings swap up on
 * /dev/zram0 (or a file), sends a patterned range out with MADV_PAGEOUT, and
 * checks the pages come back intact and the counters move:
 *   zram     pages out and back, VmSwap, SwapFree, /proc/swaps, pswpin/out, mm_stat
 *   cow      fork + copy-on-write under swap, one read for a shared slot
 *   mlock    a locked range never goes out; mlock brings an out range back
 *   prot     mprotect, PROT_NONE, munmap and MADV_DONTNEED on pages that are out
 *   off      swapoff while pages are out, also a forked child's
 *   perm     swapon/swapoff/disksize/swappiness are root's; swappiness 0..200
 *   file     a swap file on the ext4 at /mnt; a file with holes is refused
 *   hog      (small-RAM VM) twice RAM of pattern, all back intact (hogz: compressible)
 *   oom      (small-RAM VM) RAM + swap exhausted: the hog dies, the system stays
 *   race     mprotect and THP collapse racing reclaim on other CPUs (SMP)
 *   racehook the same race made certain by a test-kernel hook (ranix.test_hooks)
 *   signal   a signal frame onto a stack page that is out on swap
 *   futex    FUTEX_WAIT on a word reclaim keeps sending out
 *   busy     an active swap file or device refuses write/truncate/fallocate (ETXTBSY)
 *   lockall  mlockall keeps heap and stack in RAM
 *   protnone PROT_NONE pages out keep their contents; swapoff says EBUSY
 * Prints FAIL lines, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#define SWAP_FLAG_PREFER 0x8000
#define PG 4096UL
#define ZRAM "/dev/zram0"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf(" (errno=%d)\n", errno); fails++; } } while (0)

/* ── /proc and /sys readers ─────────────────────────────────────────── */
static long key_kb(const char *path, const char *key)
{
	char line[256]; long v = -1;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, key, strlen(key))) sscanf(line + strlen(key), " %ld", &v);
	if (f) fclose(f);
	return v;
}
static long vm_swap_kb(void)   { return key_kb("/proc/self/status", "VmSwap:"); }
static long swap_free_kb(void) { return key_kb("/proc/meminfo", "SwapFree:"); }
static long swap_total_kb(void){ return key_kb("/proc/meminfo", "SwapTotal:"); }
static long vmstat(const char *k) { return key_kb("/proc/vmstat", k); }

static int put_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

static int resident(void *p, size_t len)
{
	unsigned char v[4096];
	size_t n = len / PG, in = 0;
	for (size_t off = 0; off < n; off += sizeof v) {
		size_t k = n - off < sizeof v ? n - off : sizeof v;
		if (mincore((char *)p + off * PG, k * PG, v) != 0) return -1;
		for (size_t i = 0; i < k; i++) in += v[i] & 1;
	}
	return (int)in;
}

/* ── swap areas ─────────────────────────────────────────────────────── */
static int sys_swapon(const char *p, int fl) { return (int)syscall(SYS_swapon, p, fl); }
static int sys_swapoff(const char *p)        { return (int)syscall(SYS_swapoff, p); }

/* What mkswap writes: version 1, the last page, "SWAPSPACE2" at the page end. */
static int mkswap_fd(int fd, uint64_t bytes)
{
	static uint8_t h[PG];
	memset(h, 0, sizeof h);
	uint32_t v = 1, last = (uint32_t)(bytes / PG - 1);
	memcpy(h + 1024, &v, 4);
	memcpy(h + 1028, &last, 4);
	memcpy(h + PG - 10, "SWAPSPACE2", 10);
	return pwrite(fd, h, PG, 0) == (ssize_t)PG ? 0 : -1;
}

static int zram_up(long mb)
{
	char b[32];
	sys_swapoff(ZRAM);
	put_file("/sys/block/zram0/disksize", "0");
	snprintf(b, sizeof b, "%ldM", mb);
	if (put_file("/sys/block/zram0/disksize", b) != 0) { CHECK(0, "disksize %s", b); return -1; }
	int fd = open(ZRAM, O_RDWR);
	if (fd < 0 || mkswap_fd(fd, (uint64_t)mb << 20) != 0) { CHECK(0, "mkswap " ZRAM); return -1; }
	close(fd);
	if (sys_swapon(ZRAM, SWAP_FLAG_PREFER | 10) != 0) { CHECK(0, "swapon " ZRAM); return -1; }
	return 0;
}

static void zram_down(void)
{
	CHECK(sys_swapoff(ZRAM) == 0, "swapoff " ZRAM);
	CHECK(put_file("/sys/block/zram0/disksize", "0") == 0, "disksize 0");
}

/* ── the pattern: even pages compress (a few words), odd pages do not ── */
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }
static int all_compress;   /* hogz: odd pages compress too (zram cannot hold random data) */
static uint64_t word(size_t i, size_t j, uint64_t seed)
{
	if ((i & 1) && !all_compress) return mix(seed + i * 512 + j);
	return j % 64 == 0 ? seed ^ (i * 512 + j) : 0;
}

static void fill(uint8_t *p, size_t pages, uint64_t seed)
{
	for (size_t i = 0; i < pages; i++) {
		uint64_t *w = (uint64_t *)(p + i * PG);
		for (size_t j = 0; j < PG / 8; j++)
			w[j] = word(i, j, seed);
	}
}

/* pages [from, to) of a range filled from its start with seed: how many differ */
static size_t bad_range(const uint8_t *p, size_t from, size_t to, uint64_t seed)
{
	size_t bad = 0;
	for (size_t i = from; i < to; i++) {
		const uint64_t *w = (const uint64_t *)(p + i * PG);
		for (size_t j = 0; j < PG / 8; j++) {
			if (w[j] != word(i, j, seed)) { bad++; break; }
		}
	}
	return bad;
}
#define bad_pages(p, pages, seed) bad_range(p, 0, pages, seed)

static uint8_t *anon(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

/* ── modes ──────────────────────────────────────────────────────────── */
static void t_zram(void)
{
	const size_t pages = 8192;               /* 32 MB */
	if (zram_up(64)) return;
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 7);
	long free0 = swap_free_kb(), out0 = vmstat("pswpout"), in0 = vmstat("pswpin");
	CHECK(swap_total_kb() == 64 * 1024 - 4, "SwapTotal %ld kB, want %d", swap_total_kb(), 64 * 1024 - 4);
	CHECK(madvise(p, pages * PG, MADV_PAGEOUT) == 0, "MADV_PAGEOUT");
	long vs = vm_swap_kb();
	CHECK(vs >= (long)(pages * 4) - 64, "VmSwap %ld kB after pageout, want ~%zu", vs, pages * 4);
	CHECK(free0 - swap_free_kb() >= (long)(pages * 4) - 64, "SwapFree fell by %ld kB", free0 - swap_free_kb());
	CHECK(resident(p, pages * PG) < 64, "%d pages still resident", resident(p, pages * PG));
	CHECK(vmstat("pswpout") - out0 >= (long)pages - 16, "pswpout +%ld", vmstat("pswpout") - out0);
	char line[256]; int seen = 0;
	FILE *f = fopen("/proc/swaps", "r");
	while (f && fgets(line, sizeof line, f)) if (!strncmp(line, ZRAM " ", strlen(ZRAM) + 1) && strstr(line, "partition")) seen = 1;
	if (f) fclose(f);
	CHECK(seen, "/proc/swaps has no " ZRAM " partition line");
	uint64_t orig = 0, compr = 0;
	f = fopen("/sys/block/zram0/mm_stat", "r");
	if (f) { if (fscanf(f, "%lu %lu", &orig, &compr) != 2) orig = 0; fclose(f); }
	CHECK(orig >= pages * PG - 64 * PG && compr < orig * 3 / 4, "mm_stat orig %lu compr %lu", orig, compr);
	CHECK(bad_pages(p, pages, 7) == 0, "%zu pages came back wrong", bad_pages(p, pages, 7));
	CHECK(vmstat("pswpin") - in0 >= (long)pages - 16, "pswpin +%ld", vmstat("pswpin") - in0);
	CHECK(vm_swap_kb() == 0, "VmSwap %ld kB after reading all back", vm_swap_kb());
	CHECK(swap_free_kb() == free0, "SwapFree %ld, was %ld", swap_free_kb(), free0);
	munmap(p, pages * PG);
	zram_down();
	CHECK(swap_total_kb() == 0, "SwapTotal %ld after swapoff", swap_total_kb());
}

/* parent and child share pages; both send them out; each reads them back */
static void t_cow(void)
{
	const size_t pages = 2048;               /* 8 MB */
	if (zram_up(64)) return;
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 11);
	int go[2], done[2];
	if (pipe(go) || pipe(done)) { CHECK(0, "pipe"); return; }
	pid_t c = fork();
	if (c == 0) {
		char x;
		madvise(p, pages * PG, MADV_PAGEOUT);
		write(done[1], "x", 1);
		read(go[0], &x, 1);                  /* parent has read the pages back */
		long in0 = vmstat("pswpin");
		int ok = bad_pages(p, pages, 11) == 0;
		ok &= vmstat("pswpin") - in0 < (long)pages / 8;   /* from the swap cache, not swap */
		fill(p, pages, 12);                  /* the child's own copy now */
		madvise(p, pages * PG, MADV_PAGEOUT);
		ok &= bad_pages(p, pages, 12) == 0;
		_exit(ok ? 0 : 1);
	}
	char x;
	read(done[0], &x, 1);
	CHECK(madvise(p, pages * PG, MADV_PAGEOUT) == 0, "parent pageout");
	CHECK(vm_swap_kb() >= (long)(pages * 4) - 64, "parent VmSwap %ld", vm_swap_kb());
	CHECK(resident(p, pages * PG) < 64, "shared pages still resident after both paged out");
	long in0 = vmstat("pswpin");
	CHECK(bad_pages(p, pages, 11) == 0, "parent sees wrong pages");
	CHECK(vmstat("pswpin") - in0 >= (long)pages - 16, "parent read only %ld pages", vmstat("pswpin") - in0);
	write(go[1], "x", 1);
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child: wrong pages or a second read (status %d)", st);
	CHECK(bad_pages(p, pages, 11) == 0, "child's writes reached the parent");
	/* fork while the pages are out: the child's page tables copy the swap entries */
	long free0 = swap_free_kb();
	madvise(p, pages * PG, MADV_PAGEOUT);
	c = fork();
	if (c == 0) {
		int ok = bad_range(p, 0, pages / 2, 11) == 0;
		fill(p, pages / 2, 13);
		_exit(ok ? 0 : 1);
	}
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child forked with pages out read them wrong");
	CHECK(bad_pages(p, pages, 11) == 0, "parent's out pages wrong after the child");
	CHECK(swap_free_kb() == free0, "SwapFree %ld after all came back, was %ld", swap_free_kb(), free0);
	munmap(p, pages * PG);
	zram_down();
}

static void t_mlock(void)
{
	const size_t pages = 1024;
	if (zram_up(32)) return;
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 21);
	CHECK(mlock(p, pages * PG) == 0, "mlock");
	errno = 0;
	CHECK(madvise(p, pages * PG, MADV_PAGEOUT) == -1 && errno == EINVAL, "MADV_PAGEOUT on a locked range not EINVAL");
	CHECK(vm_swap_kb() == 0 && resident(p, pages * PG) == (int)pages, "locked pages went out");
	munlock(p, pages * PG);
	CHECK(madvise(p, pages * PG, MADV_PAGEOUT) == 0 && vm_swap_kb() > 0, "unlocked range did not go out");
	CHECK(mlock(p, pages * PG) == 0, "mlock of an out range");
	CHECK(vm_swap_kb() == 0 && resident(p, pages * PG) == (int)pages, "mlock left pages out (%d resident)", resident(p, pages * PG));
	CHECK(bad_pages(p, pages, 21) == 0, "locked pages came back wrong");
	munlock(p, pages * PG);
	munmap(p, pages * PG);
	zram_down();
}

static int segv_on_write(uint8_t *p)
{
	pid_t c = fork();
	if (c == 0) { signal(SIGSEGV, SIG_DFL); p[0] = 1; _exit(0); }
	int st = 0;
	waitpid(c, &st, 0);
	return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}

static void t_prot(void)
{
	const size_t pages = 1024;
	if (zram_up(32)) return;
	long free0 = swap_free_kb();
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 31);
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(mprotect(p, pages * PG, PROT_READ) == 0, "mprotect RO");
	CHECK(bad_pages(p, 16, 31) == 0, "read-only pages came back wrong");
	CHECK(segv_on_write(p), "write to a read-only swapped-in page did not SIGSEGV");
	CHECK(segv_on_write(p + 100 * PG), "write to a read-only page still out did not SIGSEGV");
	CHECK(mprotect(p, pages * PG, PROT_NONE) == 0, "mprotect NONE");
	CHECK(mprotect(p, pages * PG, PROT_READ | PROT_WRITE) == 0, "mprotect RW");
	CHECK(bad_range(p, 16, pages, 31) == 0, "pages out across PROT_NONE came back wrong");
	p[16 * PG] = 5;
	CHECK(p[16 * PG] == 5, "write after mprotect RW");
	uint8_t *q = anon(pages * PG);
	fill(q, pages, 41);
	/* MADV_DONTNEED and munmap give the slots back */
	madvise(q, pages * PG, MADV_PAGEOUT);
	CHECK(madvise(q, pages * PG, MADV_DONTNEED) == 0, "DONTNEED");
	CHECK(q[PG] == 0 && q[3 * PG + 8] == 0, "DONTNEED of out pages does not read zero");
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(munmap(p, pages * PG) == 0 && munmap(q, pages * PG) == 0, "munmap");
	CHECK(swap_free_kb() == free0, "SwapFree %ld after munmap, was %ld", swap_free_kb(), free0);
	zram_down();
}

static void t_off(void)
{
	const size_t pages = 4096;               /* 16 MB */
	if (zram_up(64)) return;
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 51);
	int go[2], done[2];
	if (pipe(go) || pipe(done)) { CHECK(0, "pipe"); return; }
	pid_t c = fork();
	if (c == 0) {
		char x;
		fill(p, 64, 52);                     /* some of its own */
		madvise(p, pages * PG, MADV_PAGEOUT);
		write(done[1], "x", 1);
		read(go[0], &x, 1);
		int ok = vm_swap_kb() == 0 && bad_pages(p, 64, 52) == 0 && bad_range(p, 64, pages, 51) == 0;
		_exit(ok ? 0 : 1);
	}
	char x;
	read(done[0], &x, 1);
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(vm_swap_kb() > 0, "nothing out before swapoff");
	CHECK(sys_swapoff(ZRAM) == 0, "swapoff with pages out");
	CHECK(vm_swap_kb() == 0, "VmSwap %ld after swapoff", vm_swap_kb());
	CHECK(resident(p, pages * PG) == (int)pages, "%d of %zu resident after swapoff", resident(p, pages * PG), pages);
	CHECK(bad_pages(p, pages, 51) == 0, "%zu pages wrong after swapoff", bad_pages(p, pages, 51));
	write(go[1], "x", 1);
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child's pages wrong after swapoff");
	CHECK(swap_total_kb() == 0, "SwapTotal %ld after swapoff", swap_total_kb());
	munmap(p, pages * PG);
	CHECK(put_file("/sys/block/zram0/disksize", "0") == 0, "disksize 0");
}

static void t_perm(void)
{
	if (zram_up(8)) return;
	pid_t c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(9);
		int ok = sys_swapoff(ZRAM) == -1 && errno == EPERM;
		ok &= sys_swapon(ZRAM, 0) == -1 && errno == EPERM;
		ok &= put_file("/proc/sys/vm/swappiness", "10") != 0;
		ok &= put_file("/sys/block/zram0/disksize", "1M") != 0;
		_exit(ok ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "an unprivileged user changed swap (status %d)", st);
	CHECK(sys_swapon(ZRAM, 0) == -1 && errno == EBUSY, "second swapon not EBUSY");
	CHECK(put_file("/sys/block/zram0/disksize", "16M") == -EBUSY, "disksize change while on not EBUSY");
	CHECK(key_kb("/proc/sys/vm/swappiness", "") == 60, "swappiness default %ld, want 60", key_kb("/proc/sys/vm/swappiness", ""));
	CHECK(put_file("/proc/sys/vm/swappiness", "200") == 0 && key_kb("/proc/sys/vm/swappiness", "") == 200, "swappiness 200");
	CHECK(put_file("/proc/sys/vm/swappiness", "201") == -EINVAL, "swappiness 201 not EINVAL");
	CHECK(put_file("/proc/sys/vm/swappiness", "60") == 0, "swappiness back to 60");
	zram_down();
	int fd = open("/tmp/notswap", O_RDWR | O_CREAT | O_TRUNC, 0600);
	static uint8_t z[PG * 16];
	CHECK(fd >= 0 && write(fd, z, sizeof z) == sizeof z, "write /tmp/notswap");
	close(fd);
	CHECK(sys_swapon("/tmp/notswap", 0) == -1 && errno == EINVAL, "swapon of a file without a swap header not EINVAL");
	unlink("/tmp/notswap");
}

static void t_file(void)
{
	const char *path = "/mnt/swapfile", *holes = "/mnt/swaphole";
	const size_t bytes = 8 << 20, pages = 1024;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0, "create %s", path);
	if (fd < 0) return;
	static uint8_t z[1 << 16];
	for (size_t off = 0; off < bytes; off += sizeof z) CHECK(write(fd, z, sizeof z) == sizeof z, "write swapfile");
	CHECK(mkswap_fd(fd, bytes) == 0, "mkswap file");
	fsync(fd);
	close(fd);
	CHECK(sys_swapon(path, 0) == 0, "swapon %s", path);
	/* a page whose blocks lie apart on the disk is left out, as Linux does */
	CHECK(swap_total_kb() <= 8192 - 4 && swap_total_kb() >= 8192 - 4 - 64, "SwapTotal %ld for the file", swap_total_kb());
	char line[256]; int seen = 0;
	FILE *f = fopen("/proc/swaps", "r");
	while (f && fgets(line, sizeof line, f)) if (strstr(line, path) && strstr(line, "file")) seen = 1;
	if (f) fclose(f);
	CHECK(seen, "/proc/swaps has no file line");
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 61);
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(vm_swap_kb() >= (long)(pages * 4) - 64, "VmSwap %ld on the file", vm_swap_kb());
	CHECK(bad_pages(p, pages, 61) == 0, "pages from the file came back wrong");
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(sys_swapoff(path) == 0, "swapoff file");
	CHECK(bad_pages(p, pages, 61) == 0, "pages wrong after swapoff of the file");
	munmap(p, pages * PG);
	unlink(path);
	fd = open(holes, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0 && mkswap_fd(fd, 1 << 20) == 0 && ftruncate(fd, 1 << 20) == 0, "holed file");
	close(fd);
	CHECK(sys_swapon(holes, 0) == -1 && errno == EINVAL, "swapon of a file with holes not EINVAL");
	unlink(holes);
}

/* Small-RAM VM: twice MemTotal of pattern must all come back. */
static void t_hog(void)
{
	long total = key_kb("/proc/meminfo", "MemTotal:");
	size_t pages = (size_t)total * 2 / 4;
	uint8_t *p = anon(pages * PG);
	CHECK(p != NULL, "mmap %zu pages", pages);
	if (!p) return;
	fill(p, pages, 71);
	long sw = vm_swap_kb();
	CHECK(sw > total / 2, "VmSwap %ld kB with 2x of %ld kB RAM", sw, total);
	size_t bad = bad_pages(p, pages, 71);
	CHECK(bad == 0, "%zu of %zu pages came back wrong", bad, pages);
	printf("hog: %zu MB, VmSwap peak %ld MB, pswpout %ld pswpin %ld\n", pages / 256, sw / 1024, vmstat("pswpout"), vmstat("pswpin"));
	munmap(p, pages * PG);
}

static void t_hogz(void) { all_compress = 1; t_hog(); }

/* Small-RAM VM: a child takes everything; the OOM killer ends it, not us. */
static void t_oom(void)
{
	pid_t c = fork();
	if (c == 0) {
		for (;;) { uint8_t *p = anon(64 << 20); if (p) fill(p, 16384, 81); }
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "hog ended with status %d, want SIGKILL", st);
	for (int i = 0; i < 40 && swap_free_kb() <= swap_total_kb() - 1024; i++) usleep(50000);   /* exit frees it */
	CHECK(swap_free_kb() > swap_total_kb() - 1024, "swap not given back after the kill: %ld of %ld free", swap_free_kb(), swap_total_kb());
}


/* ── review findings: each failed on the first version ── */

/* 1: mprotect and THP collapse race reclaim on other CPUs. Threads flip the
 * protection, collapse to 2 MB and page the range out while the main thread
 * checks every page; a page freed while mapped shows as wrong contents. */
static volatile int race_stop;
static uint8_t *race_p;
static size_t   race_pages;
static void *race_pageout(void *a) { (void)a; while (!race_stop) madvise(race_p, race_pages * PG, MADV_PAGEOUT); return NULL; }
static void *race_mprotect(void *a)
{
	(void)a;
	while (!race_stop) {
		mprotect(race_p, race_pages * PG, PROT_READ);
		mprotect(race_p, race_pages * PG, PROT_READ | PROT_WRITE);
		madvise(race_p, race_pages * PG, 14 /* MADV_HUGEPAGE: collapse */);
	}
	return NULL;
}

static void t_race(void)
{
	race_pages = 2048;                        /* 8 MB, 2 MB aligned below */
	if (zram_up(64)) return;
	uint8_t *raw = anon(race_pages * PG + (2 << 20));
	race_p = (uint8_t *)(((uintptr_t)raw + (2 << 20) - 1) & ~(uintptr_t)((2 << 20) - 1));
	fill(race_p, race_pages, 91);
	pthread_t a, b;
	race_stop = 0;
	pthread_create(&a, NULL, race_pageout, NULL);
	pthread_create(&b, NULL, race_mprotect, NULL);
	size_t bad = 0;
	for (int round = 0; round < 40 && !bad; round++) bad = bad_pages(race_p, race_pages, 91);
	race_stop = 1;
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	CHECK(bad == 0, "%zu pages wrong while mprotect/THP raced reclaim", bad);
	CHECK(bad_pages(race_p, race_pages, 91) == 0, "pages wrong after the race");
	munmap(raw, race_pages * PG + (2 << 20));
	zram_down();
}

/* 2a: a signal frame goes onto a page that is out on swap: the alternate
 * signal stack, paged out before each raise. */
static volatile int sig_seen;
static void on_usr1(int s) { (void)s; sig_seen++; }

static void t_signal(void)
{
	if (zram_up(32)) return;
	pid_t c = fork();
	if (c == 0) {
		const size_t len = 16 * PG;
		uint8_t *alt = anon(len);
		memset(alt, 1, len);
		stack_t ss = { .ss_sp = alt, .ss_size = len, .ss_flags = 0 };
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_usr1;
		sa.sa_flags = SA_ONSTACK;
		if (sigaltstack(&ss, NULL) != 0 || sigaction(SIGUSR1, &sa, NULL) != 0) _exit(9);
		for (int i = 0; i < 50; i++) {
			madvise(alt, len, MADV_PAGEOUT);    /* the frame goes where nothing is in RAM */
			raise(SIGUSR1);
		}
		_exit(sig_seen == 50 ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "signal onto a swapped stack: status 0x%x (SIGSEGV is 0xb)", st);
	zram_down();
}

/* 2b: FUTEX_WAIT on a word reclaim keeps sending out: EAGAIN, never EFAULT. */
static uint32_t *fx_word;
static void *fx_pageout(void *a) { (void)a; while (!race_stop) madvise(fx_word, PG, MADV_PAGEOUT); return NULL; }

static void t_futex(void)
{
	if (zram_up(32)) return;
	fx_word = (uint32_t *)anon(PG);
	*fx_word = 7;
	race_stop = 0;
	pthread_t a;
	pthread_create(&a, NULL, fx_pageout, NULL);
	int efault = 0, other = 0;
	for (int i = 0; i < 20000; i++) {
		long r = syscall(SYS_futex, fx_word, 0 /* FUTEX_WAIT */, 8, NULL, NULL, 0);
		if (r == -1 && errno == EFAULT) efault++;
		else if (!(r == -1 && errno == EAGAIN)) other++;
	}
	race_stop = 1;
	pthread_join(a, NULL);
	CHECK(efault == 0 && other == 0, "FUTEX_WAIT on a word going out: %d EFAULT, %d other", efault, other);
	munmap(fx_word, PG);
	zram_down();
}

/* 3: an active swap file and device: no write, truncate or fallocate. */
static void t_busy(void)
{
	const char *path = "/mnt/swapbusy";
	const size_t bytes = 2 << 20;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	static uint8_t z[1 << 16];
	for (size_t off = 0; fd >= 0 && off < bytes; off += sizeof z) if (write(fd, z, sizeof z) != sizeof z) break;
	CHECK(fd >= 0 && mkswap_fd(fd, bytes) == 0, "make %s", path);
	fsync(fd);
	CHECK(sys_swapon(path, 0) == 0, "swapon %s", path);
	errno = 0;
	CHECK(write(fd, z, 16) == -1 && errno == ETXTBSY, "write through an fd opened before swapon not ETXTBSY");
	CHECK(ftruncate(fd, 0) == -1 && errno == ETXTBSY, "ftruncate of an active swap file not ETXTBSY");
	CHECK(fallocate(fd, 0, 0, bytes * 2) == -1 && errno == ETXTBSY, "fallocate of an active swap file not ETXTBSY");
	close(fd);
	CHECK(open(path, O_WRONLY) == -1 && errno == ETXTBSY, "open for writing of an active swap file not ETXTBSY");
	CHECK(truncate(path, 0) == -1 && errno == ETXTBSY, "truncate of an active swap file not ETXTBSY");
	CHECK(sys_swapoff(path) == 0, "swapoff %s", path);
	fd = open(path, O_WRONLY);
	CHECK(fd >= 0, "after swapoff the file opens for writing again");
	if (fd >= 0) close(fd);
	unlink(path);
	if (zram_up(8)) return;
	CHECK(open(ZRAM, O_WRONLY) == -1 && errno == ETXTBSY, "write-open of " ZRAM " while on not ETXTBSY");
	zram_down();
}

/* 4: mlockall covers the heap and the stack, which have no VMA. */
static void t_lockall(void)
{
	if (zram_up(32)) return;
	pid_t c = fork();
	if (c == 0) {
		uint8_t *h = (uint8_t *)syscall(SYS_brk, 0);      /* the heap: no VMA */
		h = (uint8_t *)(((uintptr_t)h + PG - 1) & ~(PG - 1));
		if ((uint8_t *)syscall(SYS_brk, h + 64 * PG) < h + 64 * PG) _exit(9);
		fill(h, 64, 101);
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) _exit(8);
		errno = 0;
		int r = madvise((void *)((uintptr_t)h & ~(PG - 1)), 64 * PG, MADV_PAGEOUT);
		int ok = (r == -1 && errno == EINVAL) && vm_swap_kb() == 0 && bad_pages(h, 64, 101) == 0;
		_exit(ok ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "mlockall left the heap swappable (status 0x%x)", st);
	zram_down();
}

/* 5: pages out in a PROT_NONE region keep their contents; swapoff refuses
 * rather than drop them, and succeeds once the region is readable again. */
static void t_protnone(void)
{
	const size_t pages = 256;
	if (zram_up(32)) return;
	uint8_t *p = anon(pages * PG);
	fill(p, pages, 111);
	madvise(p, pages * PG, MADV_PAGEOUT);
	CHECK(mprotect(p, pages * PG, PROT_NONE) == 0, "mprotect NONE");
	errno = 0;
	CHECK(sys_swapoff(ZRAM) == -1 && errno == EBUSY, "swapoff with PROT_NONE pages out not EBUSY");
	CHECK(mprotect(p, pages * PG, PROT_READ | PROT_WRITE) == 0, "mprotect RW");
	CHECK(bad_pages(p, pages, 111) == 0, "PROT_NONE pages lost across swapoff");
	munmap(p, pages * PG);
	zram_down();
}


/* 1, made certain: a test kernel (ranix.test_hooks) swaps the page out right
 * where reclaim on another CPU could strike, between mprotect's or the THP
 * collapse's look at the page and its rewrite (PR_RANIX_TEST_HOOK). Code that
 * remaps the page it read frees a page that stays mapped: the next allocation
 * on this CPU gets it, so its bytes show up here, and unmapping it later frees
 * it twice. The THP collapse copies a page that is gone. */
static int klog_count(const char *what)
{
	int n = klogctl(10, NULL, 0);            /* SYSLOG_ACTION_SIZE_BUFFER */
	if (n <= 0) n = 1 << 18;
	char *b = malloc((size_t)n + 1);
	int len = b ? klogctl(3, b, n) : -1;     /* SYSLOG_ACTION_READ_ALL */
	int c = 0;
	if (len > 0) { b[len] = 0; for (char *q = b; (q = strstr(q, what)); q++) c++; }
	free(b);
	return c;
}

static void t_racehook(void)
{
	if (zram_up(32)) return;
	if (prctl(0x52580020 /* PR_RANIX_TEST_HOOK */, 1, 0, 0, 0) != 0) { CHECK(0, "PR_RANIX_TEST_HOOK: boot with ranix.test_hooks"); zram_down(); return; }
	int df0 = klog_count("double free"), kb0 = klog_count("kfree bad ptr");
	const size_t n = 16;
	uint8_t *p = anon(n * PG);
	fill(p, n, 121);
	CHECK(mprotect(p, n * PG, PROT_READ) == 0, "mprotect RO");   /* each page goes out just before its remap */
	uint8_t *q = anon(64 * PG);
	memset(q, 0xEE, 64 * PG);                /* takes the pages freed just now, if any were */
	size_t bad = bad_pages(p, n, 121);
	CHECK(bad == 0, "%zu of %zu pages wrong after mprotect raced reclaim", bad, n);
	mprotect(p, n * PG, PROT_READ | PROT_WRITE);
	munmap(q, 64 * PG);
	munmap(p, n * PG);

	uint8_t *raw = anon(4 << 20);
	uint8_t *t = (uint8_t *)(((uintptr_t)raw + (2 << 20) - 1) & ~(uintptr_t)((2 << 20) - 1));
	fill(t, 512, 131);
	madvise(t, 2 << 20, 14 /* MADV_HUGEPAGE: collapse, the first page goes out just before */);
	bad = bad_pages(t, 512, 131);
	CHECK(bad == 0, "%zu pages wrong after a THP collapse raced reclaim", bad);
	munmap(raw, 4 << 20);
	prctl(0x52580020, 0, 0, 0, 0);
	CHECK(klog_count("double free") == df0 && klog_count("kfree bad ptr") == kb0,
	      "page freed twice: %d double free, %d bad kfree", klog_count("double free") - df0, klog_count("kfree bad ptr") - kb0);
	zram_down();
}

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*fn)(void); } modes[] = {
		{ "zram", t_zram }, { "cow", t_cow }, { "mlock", t_mlock }, { "prot", t_prot },
		{ "off", t_off }, { "perm", t_perm }, { "file", t_file }, { "hog", t_hog }, { "hogz", t_hogz }, { "oom", t_oom },
		{ "race", t_race }, { "signal", t_signal }, { "futex", t_futex }, { "busy", t_busy },
		{ "lockall", t_lockall }, { "protnone", t_protnone }, { "racehook", t_racehook },
	};
	const char *m = argc > 1 ? argv[1] : "zram";
	int found = 0;
	for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
		if (!strcmp(m, modes[i].name)) { modes[i].fn(); found = 1; }
	if (!found) { printf("unknown mode %s\n", m); return 2; }
	puts(fails ? "FAIL" : "OK");
	return fails != 0;
}
