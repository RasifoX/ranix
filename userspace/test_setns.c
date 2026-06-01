/* setns(2): pin the current UTS namespace with an open fd, unshare into a
 * fresh one, then rejoin the pinned namespace and see its hostname again. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/utsname.h>

int main(void)
{
	sethostname("orig-host", 9);

	int fd = open("/proc/self/ns/uts", O_RDONLY);
	if (fd < 0) { printf("setns-fail open\n"); return 0; }

	if (unshare(CLONE_NEWUTS) != 0) { printf("setns-fail unshare\n"); return 0; }
	sethostname("new-host", 8);

	struct utsname u;
	uname(&u);
	if (strcmp(u.nodename, "new-host") != 0) { printf("setns-fail iso\n"); return 0; }

	if (setns(fd, CLONE_NEWUTS) != 0) { printf("setns-fail setns\n"); return 0; }
	uname(&u);

	printf("%s\n", strcmp(u.nodename, "orig-host") == 0 ? "setns-ok" : "setns-fail-rejoin");
	return 0;
}
