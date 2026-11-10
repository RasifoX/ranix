/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Root of a user namespace administers the network namespace it made (LTP
 * setsockopt05's setup): SIOCSIFMTU and SIOCSIFFLAGS(IFF_UP) on its loopback,
 * the MTU is read back and IPv4 fragments to it, an MTU of -1 is EINVAL, and
 * SO_NO_CHECK is taken. An unprivileged process outside gets EPERM. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
	close(fd);
	return r;
}

static int set_mtu(int s, int mtu)
{
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	strcpy(r.ifr_name, "lo");
	r.ifr_mtu = mtu;
	return ioctl(s, SIOCSIFMTU, &r);
}

static int inside(void)
{
	char m[32];
	int uid = getuid(), gid = getgid();
	if (unshare(CLONE_NEWUSER | CLONE_NEWNET)) { printf("unshare: errno %d\n", errno); return 1; }
	put("/proc/self/setgroups", "deny");
	snprintf(m, sizeof(m), "0 %d 1", uid); put("/proc/self/uid_map", m);
	snprintf(m, sizeof(m), "0 %d 1", gid); put("/proc/self/gid_map", m);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (set_mtu(s, 1400)) { printf("SIOCSIFMTU 1400 in my netns: errno %d\n", errno); bad++; }
	struct ifreq r;
	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	if (ioctl(s, SIOCGIFMTU, &r) || r.ifr_mtu != 1400) { printf("SIOCGIFMTU: %d, want 1400\n", r.ifr_mtu); bad++; }
	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo"); r.ifr_flags = IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &r)) { printf("SIOCSIFFLAGS IFF_UP: errno %d\n", errno); bad++; }
	errno = 0;
	if (set_mtu(s, -1) != -1 || errno != EINVAL) { printf("SIOCSIFMTU -1: errno %d, want EINVAL\n", errno); bad++; }
	int one = 1, v = 0; socklen_t vl = sizeof(v);
	if (setsockopt(s, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) ||
	    getsockopt(s, SOL_SOCKET, SO_NO_CHECK, &v, &vl) || v != 1) { printf("SO_NO_CHECK: errno %d, value %d\n", errno, v); bad++; }
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t al = sizeof(a);
	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	struct timeval tv = { 2, 0 };
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	bind(rx, (struct sockaddr *)&a, sizeof(a));
	getsockname(rx, (struct sockaddr *)&a, &al);
	static char big[4000], got[5000];
	memset(big, 0x42, sizeof(big));
	if (sendto(s, big, sizeof(big), 0, (struct sockaddr *)&a, sizeof(a)) != sizeof(big)) { printf("send 4000 over mtu 1400: errno %d\n", errno); bad++; }
	else if (recv(rx, got, sizeof(got), 0) != sizeof(big) || memcmp(got, big, sizeof(big))) { printf("4000 bytes did not arrive whole: errno %d\n", errno); bad++; }
	return bad;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int ok = 1, st;
	pid_t c = fork();
	if (c == 0) _exit(inside());
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;
	c = fork();
	if (c == 0) {
		if (setuid(65534)) _exit(2);
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		errno = 0;
		if (set_mtu(s, 1400) != -1 || errno != EPERM) { printf("unprivileged SIOCSIFMTU: errno %d, want EPERM\n", errno); _exit(1); }
		_exit(0);
	}
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;
	puts(ok ? "OK" : "FAIL");
	return !ok;
}
