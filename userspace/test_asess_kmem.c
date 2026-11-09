/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* What a session holds in the kernel on its own account: pipe buffers, socket
 * buffers, SysV segments. Each counts against its budget or its limit, comes back
 * when released, and never costs data. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "asess_test.h"

#define MIB     (1024L * 1024)
#define PAGE    4096L
#define LIMIT   (16 * MIB)
#define SLACK   (2 * MIB)
#define HOSTKEY 0x7a11

static long used(void)
{
	struct asess_info in;
	return asess(ASESS_INFO, -1, &in, sizeof(in)) == 0 ? (long)in.mem_cur : -1;
}

static long session(unsigned long long mem_max)
{
	struct asess_attr a;
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.uid = UID;
	a.gid = GID;
	a.root = (unsigned long long)(unsigned long)"/";
	a.mem_max = mem_max;
	static const int keep[] = { 1 };
	a.keep_fds = (unsigned long long)(unsigned long)keep;
	a.nkeep = 1;
	return asess(ASESS_CREATE, -1, &a, sizeof(a));
}

static void run(void (*fn)(void))
{
	long sfd = session(LIMIT);
	CHECK(sfd >= 0, "session");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		fails = 0;   /* this session's own */
		fn();
		_exit(fails != 0);
	}
	sess_reap((pid_t)pid);
	close((int)sfd);
}

static void pipes(void)
{
	enum { N = 100 };
	int p[N][2];
	long base = used();
	for (int i = 0; i < N; i++) CHECK(pipe(p[i]) == 0, "pipe");
	CHECK(used() >= base + N * PAGE, "pipe buffers are charged");
	for (int i = 0; i < N; i++) { close(p[i][0]); close(p[i][1]); }
	CHECK(used() <= base + PAGE, "pipe buffers come back");

	CHECK(mkfifo("/tmp/ff", 0600) == 0, "mkfifo");
	base = used();
	int fd = open("/tmp/ff", O_RDWR | O_NONBLOCK);
	CHECK(fd >= 0, "open fifo");
	CHECK(used() >= base + PAGE, "fifo buffer is charged");
	close(fd);
	unlink("/tmp/ff");
	CHECK(used() <= base + PAGE, "fifo buffer comes back with the inode");
}

/* the two cases of a stream socket: one write larger than a packet, and filling
 * it with nobody reading */
static void unix_stream(void)
{
	int sv[2];
	static char out[256 * 1024], in[sizeof(out)];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	for (size_t i = 0; i < sizeof(out); i++) out[i] = (char)(i * 7 + (i >> 8));

	pid_t pid = fork();
	if (pid == 0) {
		close(sv[1]);
		if (write(sv[0], out, sizeof(out)) != sizeof(out)) _exit(1);
		_exit(0);
	}
	close(sv[0]);
	size_t have = 0;
	while (have < sizeof(in)) {
		ssize_t r = read(sv[1], in + have, sizeof(in) - have);
		if (r <= 0) break;
		have += (size_t)r;
	}
	int st;
	waitpid(pid, &st, 0);
	CHECK(have == sizeof(in) && !memcmp(in, out, sizeof(in)), "large write arrives whole");
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "writer finished");
	close(sv[1]);

	long base = used();
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
	long sent = 0;
	static char pkt[1024];
	for (;;) {
		ssize_t w = write(sv[0], pkt, sizeof(pkt));
		if (w <= 0) break;
		sent += w;
		if (sent > 4 * LIMIT) break;
	}
	CHECK(sent <= LIMIT, "an unread socket stops at the budget");
	CHECK(errno == EAGAIN, "and says so");
	CHECK(used() > base + LIMIT / 2, "unread data is charged");
	long got = 0;
	for (;;) {
		ssize_t r = read(sv[1], pkt, sizeof(pkt));
		if (r <= 0) break;
		got += r;
	}
	CHECK(got == sent, "everything queued reads back");
	CHECK(used() <= base + SLACK, "read gives back");
	close(sv[0]);
	close(sv[1]);

	/* a blocking writer waits for room rather than losing bytes: this moves twice
	 * the budget through one socket */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	pid = fork();
	if (pid == 0) {
		close(sv[1]);
		static char mb[MIB];
		for (long i = 0; i < 2 * LIMIT / MIB; i++)
			if (write(sv[0], mb, MIB) != MIB) _exit(1);
		_exit(0);
	}
	close(sv[0]);
	sleep(1);
	static char mb2[64 * 1024];
	long total = 0;
	ssize_t r;
	while ((r = read(sv[1], mb2, sizeof(mb2))) > 0) total += r;
	waitpid(pid, &st, 0);
	CHECK(total == 2 * LIMIT, "blocking writer loses nothing at the limit");
	close(sv[1]);
}

static void udp(void)
{
	long base = used();
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(s >= 0, "udp socket");
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(4242) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0, "bind");
	int c = socket(AF_INET, SOCK_DGRAM, 0);
	static char d[512];
	for (long i = 0; i < 20000; i++) sendto(c, d, sizeof(d), 0, (struct sockaddr *)&a, sizeof(a));
	long held = used();
	CHECK(held > base + 2 * MIB, "queued datagrams are charged");
	CHECK(held <= LIMIT, "datagrams stop at the budget");
	close(s);
	close(c);
	/* packets still on the loopback queue land on nothing and go then */
	for (int i = 0; i < 50 && used() > base + SLACK; i++) usleep(20000);
	CHECK(used() <= base + SLACK, "close gives back");
}

/* TCP may only drop what it has not acknowledged: a receiver with no room left
 * must see every byte once it has room again */
static void tcp(void)
{
	alarm(60);   /* a stalled stream shows as a dead session, not a 170 s wait */
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(4243) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0, "bind");
	CHECK(listen(ls, 1) == 0, "listen");

	int c = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(c >= 0 && connect(c, (struct sockaddr *)&a, sizeof(a)) == 0, "connect");
	int s = accept(ls, NULL, NULL);
	CHECK(s >= 0, "accept");

	/* leave almost no budget */
	int hog[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, hog) == 0, "socketpair");
	static char pkt[1024];
	while (write(hog[0], pkt, sizeof(pkt)) > 0) {}

	pid_t pid = fork();
	if (pid == 0) {
		close(s);
		for (int i = 0; i < 70; i++) {
			memset(pkt, i, sizeof(pkt));
			char *p = pkt;
			size_t left = sizeof(pkt);
			while (left) {
				ssize_t w = write(c, p, left);
				if (w <= 0) _exit(2);
				p += w;
				left -= (size_t)w;
			}
		}
		_exit(0);   /* what is unacknowledged is the closed socket's to finish */
	}
	close(c);
	sleep(1);
	/* room again */
	while (read(hog[1], pkt, sizeof(pkt)) > 0) {}
	close(hog[0]);
	close(hog[1]);

	long have = 0;
	static char b[1024];
	while (have < 70 * 1024) {
		ssize_t r = read(s, b, sizeof(b));
		if (r <= 0) break;
		for (ssize_t i = 0; i < r; i++)
			if ((unsigned char)b[i] != (unsigned char)((have + i) / 1024)) {
				printf("FAIL tcp byte %ld\n", have + i);
				fails++;
				r = 0;
				break;
			}
		if (!r) break;
		have += r;
	}
	CHECK(have == 70 * 1024, "tcp delivers every byte after the squeeze");
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "tcp sender");
	close(s);
	close(ls);
}

static int host;

/* must be able to run twice: the segments go with the session */
static void shm_slots(void)
{
	int ids[64], n = 0;
	for (; n < 64; n++) {
		ids[n] = shmget(IPC_PRIVATE, PAGE, IPC_CREAT | 0600);
		if (ids[n] < 0) break;
	}
	CHECK(n > 0, "a session gets segments");
	CHECK(n <= 8, "and only its share");
	if (n < 64) CHECK(errno == ENOSPC, "ENOSPC at the limit");
	DENIED(shmget(HOSTKEY, PAGE, 0600), ENOENT, "a host segment by key");
	DENIED(shmat(host, NULL, 0), EINVAL, "a host segment by id");
}

int main(void)
{
	alarm(170);
	setvbuf(stdout, NULL, _IONBF, 0);
	host = shmget(HOSTKEY, PAGE, IPC_CREAT | 0600);
	CHECK(host >= 0, "host segment");

	run(pipes); puts("pipes done");
	run(unix_stream); puts("unix done");
	run(udp); puts("udp done");
	run(tcp); puts("tcp done");
	run(shm_slots);
	run(shm_slots); puts("shm done");

	int extra[8], n = 0;
	for (; n < 8; n++) {
		extra[n] = shmget(IPC_PRIVATE, PAGE, IPC_CREAT | 0600);
		if (extra[n] < 0) break;
	}
	CHECK(n == 8, "a session left the host its slots");
	while (n > 0) shmctl(extra[--n], IPC_RMID, NULL);
	shmctl(host, IPC_RMID, NULL);
	return finish();
}
