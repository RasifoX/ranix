/* Mutate an NTFS volume in place on a throwaway disk, mount it, walk it, unmount, put
 * the bytes back, again and again. The kernel must survive every round.
 * usage: test_ntfsfuzz DEV MOUNTPOINT HOTLIST SEED ROUNDS
 * HOTLIST: one sector number per line, the sectors a clean walk reads. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static uint64_t rng;
static uint64_t rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return rng;
}

static int dev_fd;
static uint64_t files, dirs, reads, errs, budget;
static char buf[65536];

static void walk(const char *p, int depth)
{
	if (depth > 12) return;
	DIR *d = opendir(p);
	if (!d) { errs++; return; }
	dirs++;
	struct dirent *e;
	errno = 0;
	while (budget && (e = readdir(d))) {
		char c[1024];
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (snprintf(c, sizeof(c), "%s/%s", p, e->d_name) >= (int)sizeof(c)) continue;
		budget--;
		struct stat st;
		if (lstat(c, &st) != 0) { errs++; continue; }
		if (S_ISDIR(st.st_mode)) {
			walk(c, depth + 1);
		} else if (S_ISLNK(st.st_mode)) {
			char t[256];
			readlink(c, t, sizeof(t));
		} else {
			int fd = open(c, O_RDONLY);
			if (fd < 0) { errs++; continue; }
			ssize_t r = read(fd, buf, sizeof(buf));
			if (r < 0) errs++;
			if (r > 0) reads += (uint64_t)r;
			if (st.st_size > 200000 && lseek(fd, st.st_size - 5000, SEEK_SET) > 0)
				read(fd, buf, 5000);
			close(fd);
			files++;
		}
	}
	if (errno) errs++;
	closedir(d);
}

typedef struct { uint64_t sec; uint8_t old[512]; } undo_t;
static const uint8_t SPECIAL[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x40, 0x10 };

/* --apply PATCH UNDO: write a patch file's (u64 off, u32 len, data) records to the disk,
 * saving what was there as a patch in UNDO, so a test can put it back. */
static int apply(const char *dev, const char *patch, const char *undo)
{
	static char data[1 << 20], old[1 << 20];
	FILE *p = fopen(patch, "r"), *u = fopen(undo, "w");
	int fd = open(dev, O_RDWR);
	if (!p || fd < 0) { perror("apply"); return 2; }
	uint64_t off;
	uint32_t len;
	while (fread(&off, 8, 1, p) == 1 && fread(&len, 4, 1, p) == 1) {
		if (len > sizeof(data) || fread(data, 1, len, p) != len) return 2;
		if (pread(fd, old, len, (off_t)off) != (ssize_t)len) return 2;
		if (u) { fwrite(&off, 8, 1, u); fwrite(&len, 4, 1, u); fwrite(old, 1, len, u); }
		/* the block node keeps its 4 KB sectors whole on a 512-byte write */
		if (pwrite(fd, data, len, (off_t)off) != (ssize_t)len) { perror("pwrite"); return 2; }
	}
	if (u) fclose(u);
	fclose(p);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[2], "--apply")) return apply(argv[1], argv[3], argv[4]);
	if (argc < 6) return 2;
	const char *dev = argv[1], *mnt = argv[2];
	rng = strtoull(argv[4], NULL, 0) * 0x9E3779B97F4A7C15ULL + 88172645463325252ULL;
	uint64_t rounds = strtoull(argv[5], NULL, 0);

	FILE *h = fopen(argv[3], "r");
	if (!h) { perror("hotlist"); return 2; }
	size_t nhot = 0, cap = 1 << 16;
	uint64_t *hot = malloc(cap * sizeof(*hot));
	while (fscanf(h, "%llu", (unsigned long long *)&hot[nhot]) == 1) {
		if (++nhot == cap) hot = realloc(hot, (cap *= 2) * sizeof(*hot));
	}
	fclose(h);
	printf("hot sectors %zu\n", nhot);

	dev_fd = open(dev, O_RDWR);
	if (dev_fd < 0) { perror("dev"); return 2; }
	uint64_t mounted = 0, refused = 0;
	static undo_t undo[16];
	for (uint64_t it = 0; it < rounds; it++) {
		int nu = 0;
		int ops = 1 + (int)(rnd() % 8);
		for (int k = 0; k < ops && nu < 16; k++) {
			/* the list is sorted, so its head is the boot sector and the start of $MFT */
			uint64_t sec = (rnd() & 3) ? hot[rnd() % nhot] : hot[rnd() % (nhot < 256 ? nhot : 256)];
			uint32_t in = (uint32_t)(rnd() % 512);
			int width = 1 << (rnd() % 4), kind = (int)(rnd() % 4);
			uint8_t s[512];
			if (pread(dev_fd, s, 512, (off_t)(sec * 512)) != 512) { printf("pread failed\n"); return 1; }
			undo[nu].sec = sec;
			memcpy(undo[nu].old, s, 512);
			nu++;
			for (int i = 0; i < width && in + (uint32_t)i < 512; i++) {
				uint8_t *b = &s[in + (uint32_t)i];
				switch (kind) {
				case 0: *b = SPECIAL[rnd() % sizeof(SPECIAL)]; break;
				case 1: *b ^= (uint8_t)(1u << (rnd() % 8)); break;
				case 2: *b = (uint8_t)rnd(); break;
				default: *b = i == width - 1 ? 0x7F : 0xFF; break;
				}
			}
			if (pwrite(dev_fd, s, 512, (off_t)(sec * 512)) != 512) { printf("pwrite failed\n"); return 1; }
		}
		if (mount(dev, mnt, "ntfs", MS_RDONLY, NULL) == 0) {
			mounted++;
			budget = 120;
			walk(mnt, 0);
			if (umount(mnt) != 0) { printf("umount failed %d at %llu\n", errno, (unsigned long long)it); return 1; }
		} else {
			refused++;
		}
		while (nu) {           /* newest first: one sector may be there twice */
			nu--;
			if (pwrite(dev_fd, undo[nu].old, 512, (off_t)(undo[nu].sec * 512)) != 512) return 1;
		}
		if (!(it % 200)) {
			struct sysinfo si;
			sysinfo(&si);
			printf("round %llu mounted %llu refused %llu files %llu dirs %llu errs %llu bytes %llu freeram %lu\n",
			       (unsigned long long)it, (unsigned long long)mounted, (unsigned long long)refused,
			       (unsigned long long)files, (unsigned long long)dirs, (unsigned long long)errs,
			       (unsigned long long)reads, (unsigned long)si.freeram);
			fflush(stdout);
		}
	}
	printf("FUZZ-DONE rounds %llu mounted %llu refused %llu files %llu errs %llu\n",
	       (unsigned long long)rounds, (unsigned long long)mounted, (unsigned long long)refused,
	       (unsigned long long)files, (unsigned long long)errs);
	return 0;
}
