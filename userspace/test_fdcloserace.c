/*
 * fd close-race UAF regression test.
 *
 * fd_get() returns an unpinned vfs_file_t*; holding it across a blocking
 * operation while a sibling thread close()s the same fd freed the file under
 * the syscall (flock yield loop, file-backed mmap loop, SCM_RIGHTS in-flight
 * dup). With fget/fput pinning the file for the duration this must never crash
 * or corrupt. One worker thread repeatedly drives the slow paths on a shared
 * fd while the main thread closes (and re-dups) that fd underneath it, under
 * -smp 4. The test only has to survive without crashing the kernel.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>

#define ROUNDS 400

static int g_base;
static int g_sock;
static volatile int g_stop;

static void scm_send(int sfd, int passfd)
{
	char data[1] = { 'x' };
	char cbuf[CMSG_SPACE(sizeof(int))];
	memset(cbuf, 0, sizeof(cbuf));
	struct iovec iov = { data, 1 };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &passfd, sizeof(int));
	sendmsg(sfd, &msg, 0);
}

/* worker: hammer the slow fd paths on a fd that main keeps closing/re-duping */
static void *worker(void *arg)
{
	(void)arg;
	while (!g_stop) {
		int fd = dup(g_base);
		if (fd < 0) continue;
		flock(fd, LOCK_EX | LOCK_NB);
		flock(fd, LOCK_UN);
		void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
		if (m != MAP_FAILED) munmap(m, 4096);
		if (g_sock >= 0) scm_send(g_sock, fd);
		close(fd);
	}
	return NULL;
}

static void drain(int sfd)
{
	char rb[1];
	char rc[CMSG_SPACE(sizeof(int))];
	struct iovec riov = { rb, 1 };
	struct msghdr rmsg;
	memset(&rmsg, 0, sizeof(rmsg));
	rmsg.msg_iov = &riov;
	rmsg.msg_iovlen = 1;
	rmsg.msg_control = rc;
	rmsg.msg_controllen = sizeof(rc);
	while (recvmsg(sfd, &rmsg, MSG_DONTWAIT) > 0) {
		struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmsg);
		if (rcm && rcm->cmsg_type == SCM_RIGHTS) {
			int got;
			memcpy(&got, CMSG_DATA(rcm), sizeof(int));
			close(got);
		}
		rmsg.msg_controllen = sizeof(rc);
	}
}

int main(void)
{
	g_base = open("/tmp/fdrace", O_CREAT | O_RDWR, 0644);
	if (g_base < 0) { puts("FAIL open"); return 1; }
	char zero[4096];
	memset(zero, 0, sizeof(zero));
	write(g_base, zero, sizeof(zero));

	int sv[2];
	g_sock = (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) ? sv[0] : -1;

	pthread_t th;
	pthread_create(&th, NULL, worker, NULL);

	/* main keeps the shared fd churning: dup then close so the worker's
	 * flock/mmap/SCM-send keep landing on a fd that vanishes mid-op. */
	for (int r = 0; r < ROUNDS; r++) {
		int fd = dup(g_base);
		if (fd >= 0) close(fd);
		if (g_sock >= 0 && (r & 31) == 0) drain(sv[1]);
	}

	g_stop = 1;
	pthread_join(th, NULL);

	if (g_sock >= 0) { drain(sv[1]); close(sv[0]); close(sv[1]); }
	close(g_base);

	puts("OK");
	return 0;
}
