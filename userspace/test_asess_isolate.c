/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Nothing outside can be named from inside: signals, ptrace, process memory,
 * /proc, sockets, IPC. The sentinel shares the session's uid, so uid rules allow
 * everything here and only the session boundary refuses. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ipc.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include "asess_test.h"

#define TCP_PORT 9471
#define UDP_PORT 9472
#define UNIX_PATH "/tmp/asess-iso.sock"
#define ABSTRACT  "\0asess-iso"
#define SHM_KEY   0x4153
#define MQ_NAME   "/asess-iso"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static void signals_and_memory(pid_t sentinel, pid_t host_pgid)
{
	long word = 0, remote = 0;
	struct iovec l = { &word, sizeof(word) }, r = { &remote, sizeof(remote) };
	char path[64];

	DENIED(kill(sentinel, 0), ESRCH, "kill(0) by host pid");
	DENIED(kill(sentinel, SIGKILL), ESRCH, "kill by host pid");
	DENIED(kill(-sentinel, SIGKILL), ESRCH, "kill of the sentinel's group");
	DENIED(kill(-host_pgid, SIGKILL), ESRCH, "kill of the host's group");
	DENIED(syscall(SYS_tkill, sentinel, SIGKILL), ESRCH, "tkill");
	DENIED(syscall(SYS_tgkill, sentinel, sentinel, SIGKILL), ESRCH, "tgkill");
	DENIED(sigqueue(sentinel, SIGUSR1, (union sigval){ 0 }), ESRCH, "sigqueue");
	DENIED(syscall(SYS_pidfd_open, sentinel, 0), ESRCH, "pidfd_open");
	DENIED(ptrace(PTRACE_ATTACH, sentinel, 0, 0), ESRCH, "ptrace attach");
	DENIED(ptrace(PTRACE_SEIZE, sentinel, 0, 0), ESRCH, "ptrace seize");
	DENIED(process_vm_readv(sentinel, &l, 1, &r, 1, 0), ESRCH, "process_vm_readv");
	DENIED(process_vm_writev(sentinel, &l, 1, &r, 1, 0), ESRCH, "process_vm_writev");
	DENIED(setpgid(0, host_pgid), EPERM, "join the host's group");
	DENIED(prlimit(sentinel, RLIMIT_NOFILE, NULL, NULL), ESRCH, "prlimit");
	cpu_set_t cpus;
	DENIED(sched_getaffinity(sentinel, sizeof(cpus), &cpus), ESRCH, "sched_getaffinity");

	snprintf(path, sizeof(path), "/proc/%d", (int)sentinel);
	DENIED(open(path, O_RDONLY), ENOENT, "/proc/<host pid>");
	snprintf(path, sizeof(path), "/proc/%d/mem", (int)sentinel);
	DENIED(open(path, O_RDONLY), ENOENT, "/proc/<host pid>/mem");

	/* the supervisor has no number here, so kill(-1) reaches only members */
	CHECK(getppid() == 0, "the parent is outside the namespace");
	pid_t member = fork();
	if (member == 0) { pause(); _exit(0); }
	CHECK(member == 2, "a member gets pid 2");
	CHECK(kill(-1, SIGKILL) == 0, "kill(-1) finds the member");
	int st = 0;
	CHECK(waitpid(member, &st, 0) == member && WIFSIGNALED(st), "kill(-1) took the member");

	/* ptrace works inside: the target check must not refuse a member */
	member = fork();
	if (member == 0) { pause(); _exit(0); }
	CHECK(ptrace(PTRACE_ATTACH, member, 0, 0) == 0, "ptrace a member");
	kill(member, SIGKILL);
	waitpid(member, NULL, 0);

	DIR *d = opendir("/proc");
	CHECK(d != NULL, "list /proc");
	/* readdir walks the global task list, so exits elsewhere can shift it: judge
	 * the names it shows, not their number */
	int foreign = 0;
	for (struct dirent *e; d && (e = readdir(d)); )
		if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && atoi(e->d_name) > member) foreign++;
	if (d) closedir(d);
	CHECK(!foreign, "/proc lists members only");
}

static void network(void)
{
	struct sockaddr_in in;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons(TCP_PORT);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(s >= 0, "tcp socket");
	if (connect(s, (struct sockaddr *)&in, sizeof(in)) == 0) {
		printf("FAIL TCP reached the host listener\n");
		fails++;
	}
	close(s);

	s = socket(AF_INET, SOCK_DGRAM, 0);
	in.sin_port = htons(UDP_PORT);
	sendto(s, "x", 1, 0, (struct sockaddr *)&in, sizeof(in));   /* the host checks it got nothing */
	close(s);

	/* QEMU's gateway: only the namespace's lack of a route stops this */
	s = socket(AF_INET, SOCK_STREAM, 0);
	inet_pton(AF_INET, "10.0.2.2", &in.sin_addr);
	in.sin_port = htons(80);
	DENIED(connect(s, (struct sockaddr *)&in, sizeof(in)), ENETUNREACH, "TCP out of the namespace");
	close(s);

	/* the session's own loopback works */
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons(TCP_PORT + 10);
	CHECK(bind(ls, (struct sockaddr *)&in, sizeof(in)) == 0 && listen(ls, 1) == 0, "listen on loopback");
	s = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(connect(s, (struct sockaddr *)&in, sizeof(in)) == 0, "connect on loopback");
	close(s);
	close(ls);

	struct sockaddr_un un;
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, UNIX_PATH);
	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(s, (struct sockaddr *)&un, sizeof(un)) == 0) {
		printf("FAIL AF_UNIX reached a host socket\n");
		fails++;
	}
	close(s);

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	memcpy(un.sun_path, ABSTRACT, sizeof(ABSTRACT) - 1);
	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(s, (struct sockaddr *)&un, offsetof(struct sockaddr_un, sun_path) + sizeof(ABSTRACT) - 1) == 0) {
		printf("FAIL abstract AF_UNIX reached a host socket\n");
		fails++;
	}
	close(s);
}

static void ipc(void)
{
	DENIED(shmget(SHM_KEY, 0, 0), ENOENT, "host SysV segment");
	DENIED(mq_open(MQ_NAME, O_RDONLY), ENOENT, "host POSIX queue");
}

/* files sent in over a kept socket: bytes pass, paths do not */
static void scm_filter(int sock)
{
	char data, cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec v = { &data, 1 };
	struct msghdr m;

	for (int want = 0; want < 2; want++) {
		memset(&m, 0, sizeof(m));
		memset(cbuf, 0, sizeof(cbuf));
		m.msg_iov = &v; m.msg_iovlen = 1;
		m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
		CHECK(recvmsg(sock, &m, 0) == 1, "recvmsg");
		struct cmsghdr *c = CMSG_FIRSTHDR(&m);
		int got = c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS;
		if (want == 0) CHECK(!got, "a regular file does not arrive");
		else {
			CHECK(got, "a pipe arrives");
			if (got) {
				int fd;
				memcpy(&fd, CMSG_DATA(c), sizeof(fd));
				CHECK(fcntl(fd, F_GETFD) >= 0, "the pipe is open");
			}
		}
	}
}

static void send_fd(int sock, int fd)
{
	char data = 'f', cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec v = { &data, 1 };
	struct msghdr m;
	memset(&m, 0, sizeof(m));
	memset(cbuf, 0, sizeof(cbuf));
	m.msg_iov = &v; m.msg_iovlen = 1;
	m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(fd));
	CHECK(sendmsg(sock, &m, 0) == 1, "sendmsg");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	int alive[2];
	CHECK(pipe(alive) == 0, "pipe");
	pid_t sentinel = fork();
	if (sentinel == 0) {
		close(alive[0]);
		setpgid(0, 0);
		if (setgid(GID) || setuid(UID)) _exit(9);
		prctl(PR_SET_PDEATHSIG, SIGKILL);   /* must not outlive a failed run */
		pause();
		_exit(0);
	}
	close(alive[1]);
	usleep(100000);

	int tcp = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in in;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons(TCP_PORT);
	CHECK(bind(tcp, (struct sockaddr *)&in, sizeof(in)) == 0 && listen(tcp, 4) == 0, "host tcp listener");

	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	in.sin_port = htons(UDP_PORT);
	CHECK(bind(udp, (struct sockaddr *)&in, sizeof(in)) == 0, "host udp socket");
	CHECK(fcntl(udp, F_SETFL, O_NONBLOCK) == 0, "udp nonblock");

	unlink(UNIX_PATH);
	struct sockaddr_un un;
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, UNIX_PATH);
	int ux = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(ux, (struct sockaddr *)&un, sizeof(un)) == 0 && listen(ux, 4) == 0, "host unix listener");
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	memcpy(un.sun_path, ABSTRACT, sizeof(ABSTRACT) - 1);
	int ab = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(ab, (struct sockaddr *)&un, offsetof(struct sockaddr_un, sun_path) + sizeof(ABSTRACT) - 1) == 0 &&
	      listen(ab, 4) == 0, "host abstract listener");

	int shm = shmget(SHM_KEY, 4096, IPC_CREAT | 0666);
	CHECK(shm >= 0, "host segment");
	struct mq_attr ma = { .mq_maxmsg = 1, .mq_msgsize = 8 };
	mqd_t q = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &ma);
	CHECK(q != (mqd_t)-1, "host queue");

	pid_t host_pgid = getpgrp();
	int sp[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
	int keep[] = { 1, sp[1] };
	long sfd = sess_create("/", UID, keep, 2, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		signals_and_memory(sentinel, host_pgid);
		network();
		ipc();
		scm_filter(sp[1]);
		_exit(fails ? 1 : 0);
	}

	int reg = open("/bin/sh", O_RDONLY), pp[2];
	CHECK(reg >= 0 && pipe(pp) == 0, "fds to send");
	send_fd(sp[0], reg);
	send_fd(sp[0], pp[0]);
	sess_reap((pid_t)pid);

	CHECK(kill(sentinel, 0) == 0, "sentinel survived");
	char b;
	CHECK(recv(udp, &b, 1, 0) == -1 && errno == EAGAIN, "no datagram crossed");
	int nb = fcntl(tcp, F_SETFL, O_NONBLOCK);
	CHECK(nb == 0 && accept(tcp, NULL, NULL) == -1 && errno == EAGAIN, "no connection crossed");

	kill(sentinel, SIGKILL);
	waitpid(sentinel, NULL, 0);
	shmctl(shm, IPC_RMID, NULL);
	mq_close(q);
	mq_unlink(MQ_NAME);
	unlink(UNIX_PATH);
	return finish();
}
