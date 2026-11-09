/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SCM_RIGHTS on a stream socket: many descriptors in one message, and one read
 * never swallows the next message's descriptors. Firefox's IPC needs both. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int send_fds(int s, int *fds, int n, const char *msg)
{
    char ctl[CMSG_SPACE(sizeof(int) * 32)];
    memset(ctl, 0, sizeof ctl);
    struct iovec v = {(void *)msg, strlen(msg)};
    struct msghdr h = {0};
    h.msg_iov = &v;
    h.msg_iovlen = 1;
    h.msg_control = ctl;
    h.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)n);
    return (int)sendmsg(s, &h, 0);
}

static int recv_fds(int s, char *buf, int cap, int *got)
{
    char ctl[CMSG_SPACE(sizeof(int) * 32)];
    struct iovec v = {buf, (size_t)cap};
    struct msghdr h = {0};
    h.msg_iov = &v;
    h.msg_iovlen = 1;
    h.msg_control = ctl;
    h.msg_controllen = sizeof ctl;
    int r = (int)recvmsg(s, &h, 0);
    *got = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_type == SCM_RIGHTS) {
            int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < n; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + (size_t)i * sizeof(int), sizeof fd);
                close(fd);
            }
            *got += n;
        }
    return (h.msg_flags & MSG_CTRUNC) ? -1 : r;
}

int main(void)
{
    int sv[2], p[2], fds[32];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(p)) { puts("socketpair"); return 1; }
    for (int i = 0; i < 32; i++) fds[i] = p[0];
    char b[64];
    int n;
    if (send_fds(sv[0], fds, 32, "thirtytwo") != 9) { printf("send 32: %s\n", strerror(errno)); return 1; }
    if (send_fds(sv[0], fds, 13, "thirteen") != 8) { puts("send 13"); return 1; }
    if (write(sv[0], "plain", 5) != 5) { puts("plain"); return 1; }
    if (send_fds(sv[0], fds, 3, "three") != 5) { puts("send 3"); return 1; }
    int r = recv_fds(sv[1], b, sizeof b, &n);
    if (r != 9 || n != 32) { printf("first r=%d n=%d\n", r, n); return 1; }
    r = recv_fds(sv[1], b, sizeof b, &n);
    if (r != 8 || n != 13) { printf("second r=%d n=%d\n", r, n); return 1; }
    r = recv_fds(sv[1], b, sizeof b, &n);
    if (r != 5 || n != 0 || memcmp(b, "plain", 5)) { printf("third r=%d n=%d\n", r, n); return 1; }
    r = recv_fds(sv[1], b, sizeof b, &n);
    if (r != 5 || n != 3) { printf("fourth r=%d n=%d\n", r, n); return 1; }
    /* a stream takes a long gather in parts, as many pieces as the caller has */
    static char big[300];
    struct iovec v[300];
    for (int i = 0; i < 300; i++) { v[i].iov_base = big + i; v[i].iov_len = 1; }
    struct msghdr h = {0};
    h.msg_iov = v;
    h.msg_iovlen = 300;
    long w = sendmsg(sv[0], &h, 0);
    if (w < 1 || w > 300) { printf("gather %ld\n", w); return 1; }
    puts("OK");
    return 0;
}
