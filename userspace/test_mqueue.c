/* POSIX message-queue blocking round-trip: the parent blocks in mq_receive
 * (empty queue) until the child sends, exercising the kernel's event-driven
 * recv wait + the sender's wake. */
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <mqueue.h>

int main(void)
{
	mq_unlink("/rxt");
	struct mq_attr a = { 0 };
	a.mq_maxmsg  = 4;
	a.mq_msgsize = 64;
	mqd_t q = mq_open("/rxt", O_CREAT | O_RDWR, 0600, &a);
	if (q < 0) { puts("FAIL open"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		usleep(50000);                 /* let the parent block first */
		mq_send(q, "hello", 5, 0);
		_exit(0);
	}

	char buf[64];
	ssize_t n = mq_receive(q, buf, sizeof(buf), NULL);
	waitpid(pid, NULL, 0);
	mq_close(q);
	mq_unlink("/rxt");

	puts(n == 5 && memcmp(buf, "hello", 5) == 0 ? "OK" : "FAIL");
	return 0;
}
