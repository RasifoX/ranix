/* cgroup namespace: after unshare(CLONE_NEWCGROUP) the caller's current
 * cgroup becomes the root of its /proc/self/cgroup view. Moving into /test2
 * shows "0::/test2"; unshering then re-roots the view to "0::/". */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>

int main(void)
{
	mkdir("/cgroup/test2", 0755);
	int fd = open("/cgroup/test2/cgroup.procs", O_WRONLY);
	if (fd < 0) { printf("cgroupns-fail procs\n"); return 0; }
	char b[16]; int n = sprintf(b, "%d\n", (int)getpid());
	write(fd, b, n); close(fd);

	char before[64] = {0}, after[64] = {0};
	fd = open("/proc/self/cgroup", O_RDONLY); read(fd, before, sizeof(before) - 1); close(fd);
	if (unshare(CLONE_NEWCGROUP) != 0) { printf("cgroupns-fail unshare\n"); return 0; }
	fd = open("/proc/self/cgroup", O_RDONLY); read(fd, after, sizeof(after) - 1); close(fd);

	if (strstr(before, "test2") && strcmp(after, "0::/\n") == 0)
		printf("cgroupns-ok\n");
	else
		printf("cgroupns-fail before=[%s] after=[%s]\n", before, after);
	return 0;
}
