/* IPC namespace: a POSIX message-queue name created in one IPC namespace is
 * invisible in another. The child unshares CLONE_NEWIPC, so the queue the
 * parent created is absent there and O_CREAT|O_EXCL succeeds. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <mqueue.h>
#include <sys/wait.h>

int main(void)
{
	mq_unlink("/ipcns");
	mqd_t q = mq_open("/ipcns", O_CREAT | O_RDWR, 0644, NULL);
	if (q < 0) { printf("ipcns-fail create\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		if (unshare(CLONE_NEWIPC) != 0) _exit(2);
		/* fresh IPC namespace: the name is free, so O_EXCL must succeed */
		mqd_t q2 = mq_open("/ipcns", O_CREAT | O_EXCL | O_RDWR, 0644, NULL);
		_exit(q2 >= 0 ? 0 : 3);
	}

	int st;
	waitpid(pid, &st, 0);
	mq_unlink("/ipcns");

	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "ipcns-ok" : "ipcns-fail");
	return 0;
}
