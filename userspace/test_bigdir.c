/* Large-directory readdir: create many entries and confirm readdir enumerates
 * every one exactly once (catches getdents buffer-boundary / pagination bugs). */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define N 200

int main(void)
{
	system("rm -rf /tmp/bigdir 2>/dev/null");
	if (mkdir("/tmp/bigdir", 0755) != 0) { puts("FAIL mkdir"); return 0; }

	for (int i = 0; i < N; i++) {
		char p[64];
		snprintf(p, sizeof(p), "/tmp/bigdir/f%03d", i);
		int fd = open(p, O_CREAT | O_WRONLY, 0644);
		if (fd < 0) { printf("FAIL create %d\n", i); return 0; }
		close(fd);
	}

	DIR *d = opendir("/tmp/bigdir");
	if (!d) { puts("FAIL opendir"); return 0; }
	int seen[N];
	memset(seen, 0, sizeof(seen));
	int count = 0, dots = 0, dup = 0, bad = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { dots++; continue; }
		int idx = -1;
		if (e->d_name[0] == 'f') idx = atoi(e->d_name + 1);
		if (idx < 0 || idx >= N) { bad++; continue; }
		if (seen[idx]) dup++;
		seen[idx] = 1;
		count++;
	}
	closedir(d);

	int missing = 0;
	for (int i = 0; i < N; i++) if (!seen[i]) missing++;

	system("rm -rf /tmp/bigdir 2>/dev/null");

	if (count == N && dots == 2 && dup == 0 && bad == 0 && missing == 0)
		puts("OK");
	else
		printf("FAIL count=%d dots=%d dup=%d bad=%d missing=%d\n", count, dots, dup, bad, missing);
	return 0;
}
