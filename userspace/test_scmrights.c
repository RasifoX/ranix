/* SCM_RIGHTS: pass an open file descriptor over an AF_UNIX socketpair via
 * sendmsg/recvmsg ancillary data. The received fd is a new descriptor in this
 * process referring to the same open file — reading it yields the file's
 * contents. This is the Wayland/D-Bus/systemd fd-passing primitive. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void)
{
	/* a file to pass */
	int pf = open("/tmp/scm_payload", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (pf < 0) { printf("scm-fail payload\n"); return 0; }
	write(pf, "PASSED-FD-OK", 12);
	lseek(pf, 0, SEEK_SET);

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("scm-fail pair\n"); return 0; }

	/* send: 1 data byte + SCM_RIGHTS carrying pf */
	char d = 'x';
	struct iovec iov = { &d, 1 };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr msg; memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov; msg.msg_iovlen = 1;
	msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &pf, sizeof(int));
	if (sendmsg(sv[0], &msg, 0) < 0) { printf("scm-fail sendmsg\n"); return 0; }

	/* recv: pick up the data byte + the passed fd */
	char rd = 0;
	struct iovec riov = { &rd, 1 };
	char rcbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr rmsg; memset(&rmsg, 0, sizeof(rmsg));
	rmsg.msg_iov = &riov; rmsg.msg_iovlen = 1;
	rmsg.msg_control = rcbuf; rmsg.msg_controllen = sizeof(rcbuf);
	long r = recvmsg(sv[1], &rmsg, 0);
	if (r != 1 || rd != 'x') { printf("scm-fail recvmsg r=%ld\n", r); return 0; }

	struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmsg);
	if (!rcm || rcm->cmsg_level != SOL_SOCKET || rcm->cmsg_type != SCM_RIGHTS) {
		printf("scm-fail no-cmsg\n"); return 0;
	}
	int newfd;
	memcpy(&newfd, CMSG_DATA(rcm), sizeof(int));
	if (newfd < 0 || newfd == pf) { printf("scm-fail badfd %d\n", newfd); return 0; }

	/* the received fd reads the same file */
	char buf[16] = {0};
	if (read(newfd, buf, 12) != 12 || memcmp(buf, "PASSED-FD-OK", 12) != 0) {
		printf("scm-fail content [%s]\n", buf); return 0;
	}

	printf("OK\n");
	return 0;
}
