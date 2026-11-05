/* A GPT partition labelled ranix-sesslog stays inside the kernel: no node, no mount,
 * while its neighbours appear as usual. Needs a disk with that label; without one
 * the kernel logged nothing to check and the test says so. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

/* "gpt: <dev> start=N size=N label=<label>[ kernel-only]" */
static int find(const char *log, const char *label, char *dev, size_t sz)
{
	char want[64];
	snprintf(want, sizeof(want), " label=%s", label);
	for (const char *p = strstr(log, "gpt: "); p; p = strstr(p + 1, "gpt: ")) {
		const char *nl = strchr(p, '\n');
		const char *hit = strstr(p, want);
		if (!hit || (nl && hit > nl)) continue;
		const char *sp = strchr(p + 5, ' ');
		size_t n = (size_t)(sp - (p + 5));
		if (n >= sz) return 0;
		memcpy(dev, p + 5, n);
		dev[n] = 0;
		return 1;
	}
	return 0;
}

int main(void)
{
	static char b[262144];
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { puts("FAIL kmsg"); return 1; }
	size_t got = 0;
	ssize_t n;
	while (got < sizeof(b) - 1 && (n = read(fd, b + got, sizeof(b) - 1 - got)) > 0) got += (size_t)n;
	close(fd);

	char hid[40], sib[40], path[64];
	if (!find(b, "ranix-sesslog kernel-only", hid, sizeof(hid))) {
		puts("no disk labelled ranix-sesslog, nothing to check");
		puts("OK");
		return 0;
	}
	if (!find(b, "data", sib, sizeof(sib))) { puts("FAIL sibling label"); return 1; }
	printf("hidden %s, sibling %s\n", hid, sib);

	struct stat st;
	snprintf(path, sizeof(path), "/dev/%s", hid);
	if (stat(path, &st) == 0 || errno != ENOENT) { puts("FAIL node exists"); return 1; }
	if (open(path, O_RDONLY) >= 0) { puts("FAIL open"); return 1; }
	mkdir("/tmp", 0755);
	mkdir("/tmp/gpm", 0755);
	if (mount(path, "/tmp/gpm", "ext2", 0, NULL) == 0) { puts("FAIL mount"); return 1; }

	snprintf(path, sizeof(path), "/dev/%s", sib);
	if (stat(path, &st) || !S_ISBLK(st.st_mode)) { puts("FAIL sibling node"); return 1; }
	fd = open(path, O_RDONLY);
	char s[512];
	if (fd < 0 || read(fd, s, sizeof(s)) != sizeof(s) || memcmp(s, "DATAPART", 8)) { puts("FAIL sibling data"); return 1; }
	close(fd);
	puts("OK");
	return 0;
}
