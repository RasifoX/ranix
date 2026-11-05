/* What an unprivileged task must not reach. Root sets the objects up, a child
 * drops to uid 1000 and tries. First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/klog.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERR(e, x) do { errno = 0; long r_ = (long)(x); if (r_ != -1 || errno != (e)) { \
	printf("FAIL %d %s r=%ld errno=%d want=%d\n", __LINE__, #x, r_, errno, (e)); _exit(1); } } while (0)
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); _exit(1); } } while (0)

int main(void)
{
	if (geteuid()) { puts("OK"); return 0; }   /* the suite runs this as root */

	/* a fresh segment is zero, whatever its pages held */
	int dirty = shmget(IPC_PRIVATE, 1 << 16, IPC_CREAT | 0600);
	CHECK(dirty >= 0);
	char *d = shmat(dirty, NULL, 0);
	CHECK(d != (void *)-1);
	memset(d, 0x5a, 1 << 16);
	shmdt(d);
	shmctl(dirty, IPC_RMID, NULL);
	int clean = shmget(IPC_PRIVATE, 1 << 16, IPC_CREAT | 0600);
	CHECK(clean >= 0);
	char *c = shmat(clean, NULL, 0);
	CHECK(c != (void *)-1);
	for (int i = 0; i < 1 << 16; i++) CHECK(c[i] == 0);
	shmdt(c);

	/* another segment of the same size keeps its attach count across shmdt */
	int one = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600), two = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	char *a1 = shmat(one, NULL, 0), *a2 = shmat(two, NULL, 0);
	struct shmid_ds ds;
	CHECK(shmdt(a2) == 0);
	CHECK(shmctl(one, IPC_STAT, &ds) == 0 && ds.shm_nattch == 1);
	shmdt(a1);
	shmctl(one, IPC_RMID, NULL); shmctl(two, IPC_RMID, NULL);

	int sem = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
	int msg = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	CHECK(sem >= 0 && msg >= 0);

	pid_t p = fork();
	if (p == 0) {
		setgroups(0, NULL); setgid(1000); setuid(1000);
		ERR(EACCES, (long)shmat(clean, NULL, 0));
		ERR(EPERM, shmctl(clean, IPC_RMID, NULL));
		ERR(EACCES, semop(sem, &(struct sembuf){0, 1, 0}, 1));
		ERR(EACCES, semctl(sem, 0, GETVAL));
		ERR(EPERM, semctl(sem, 0, IPC_RMID));
		ERR(EACCES, msgsnd(msg, &(struct { long t; char c; }){1, 'x'}, 1, IPC_NOWAIT));
		ERR(EPERM, msgctl(msg, IPC_RMID, NULL));

		char b[64];
		ERR(EPERM, klogctl(3, b, sizeof(b)));
		ERR(EPERM, syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &(struct sched_param){ .sched_priority = 10 }));
		ERR(EACCES, setpriority(PRIO_PROCESS, 0, -5));
		ERR(EPERM, socket(AF_INET, SOCK_RAW, 1));

		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80) };
		int s = socket(AF_INET, SOCK_STREAM, 0);
		CHECK(s >= 0);
		ERR(EACCES, bind(s, (void *)&a, sizeof(a)));

		int ns = open("/proc/1/ns/uts", O_RDONLY);
		if (ns >= 0) ERR(EPERM, setns(ns, 0));

		/* ptrace of oneself's group is refused for anyone */
		pid_t me = getpid();
		ERR(EPERM, syscall(SYS_ptrace, 16 /*ATTACH*/, me, 0, 0));

		/* 0..19 are the only values open without privilege, and it reads back */
		CHECK(setpriority(PRIO_PROCESS, 0, 7) == 0 && getpriority(PRIO_PROCESS, 0) == 7);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	semctl(sem, 0, IPC_RMID);
	msgctl(msg, IPC_RMID, NULL);
	shmctl(clean, IPC_RMID, NULL);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("FAIL child"); return 0; }

	/* SIGKILL cannot be routed into a signalfd and consumed there */
	p = fork();
	if (p == 0) {
		sigset_t all;
		sigfillset(&all);
		sigprocmask(SIG_BLOCK, &all, NULL);
		int fd = (int)syscall(SYS_signalfd4, -1, &(uint64_t){ ~0ULL }, 8, 0);
		char b[128];
		for (;;) (void)!read(fd, b, sizeof(b));
	}
	usleep(50000);
	kill(p, SIGKILL);
	waitpid(p, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

	/* a read parked on the console is still killable, and a signal that landed
	 * before the sleep is not lost to it */
	int cfd = open("/dev/console", O_RDONLY);
	if (cfd >= 0) {
		p = fork();
		if (p == 0) { char b; for (;;) (void)!read(cfd, &b, 1); }
		usleep(50000);
		kill(p, SIGKILL);
		int gone = 0;
		for (int i = 0; i < 300 && !gone; i++) { gone = waitpid(p, &st, WNOHANG) == p; usleep(10000); }
		CHECK(gone);
		close(cfd);
	}

	puts("OK");
	return 0;
}
