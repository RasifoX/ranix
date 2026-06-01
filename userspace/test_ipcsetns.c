/* setns(2) for IPC: pin the current IPC namespace, unshare into an empty one
 * (the queue name is free there), then rejoin and see the name is taken
 * again. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <mqueue.h>

int main(void)
{
	mq_unlink("/snq");
	mqd_t q = mq_open("/snq", O_CREAT | O_RDWR, 0644, NULL);
	if (q < 0) { printf("ipcsetns-fail create\n"); return 0; }

	int fd = open("/proc/self/ns/ipc", O_RDONLY);
	if (fd < 0) { printf("ipcsetns-fail open\n"); return 0; }

	if (unshare(CLONE_NEWIPC) != 0) { printf("ipcsetns-fail unshare\n"); return 0; }
	mqd_t q2 = mq_open("/snq", O_CREAT | O_EXCL | O_RDWR, 0644, NULL);
	if (q2 < 0) { printf("ipcsetns-fail iso\n"); return 0; }

	if (setns(fd, CLONE_NEWIPC) != 0) { printf("ipcsetns-fail setns\n"); return 0; }
	/* back in the original namespace: the name exists, so O_EXCL must fail */
	mqd_t q3 = mq_open("/snq", O_CREAT | O_EXCL | O_RDWR, 0644, NULL);
	mq_unlink("/snq");

	printf("%s\n", (q3 < 0) ? "ipcsetns-ok" : "ipcsetns-fail-rejoin");
	return 0;
}
