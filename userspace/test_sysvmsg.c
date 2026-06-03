/* SysV message queues: a child receives by message type out of FIFO order
 * (asks for type 2 before type 1), proving type-selective msgrcv and the
 * blocking send/receive wakeup path. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>

struct mbuf { long mtype; char mtext[64]; };

int main(void)
{
	int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	if (id < 0) { printf("sysvmsg-fail get\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		struct mbuf m;
		ssize_t n = msgrcv(id, &m, sizeof(m.mtext), 2, 0);  /* type 2 first */
		if (n < 0 || strcmp(m.mtext, "two") != 0) _exit(2);
		n = msgrcv(id, &m, sizeof(m.mtext), 1, 0);          /* then type 1 */
		if (n < 0 || strcmp(m.mtext, "one") != 0) _exit(3);
		_exit(0);
	}

	struct mbuf a = { 1, "one" }, b = { 2, "two" };
	if (msgsnd(id, &a, strlen(a.mtext) + 1, 0) != 0) { printf("sysvmsg-fail snd1\n"); return 0; }
	if (msgsnd(id, &b, strlen(b.mtext) + 1, 0) != 0) { printf("sysvmsg-fail snd2\n"); return 0; }

	int st;
	waitpid(pid, &st, 0);
	msgctl(id, IPC_RMID, NULL);

	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		printf("OK\n");
	else
		printf("sysvmsg-fail code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return 0;
}
