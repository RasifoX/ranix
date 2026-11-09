/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Firewall rules are global, so reading or changing them takes CAP_NET_ADMIN in
 * the initial user namespace. Reading interface data stays open to everyone. */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define SOL_RANIX_FILTER 0x52414e58
#define NF_RULE 1

static const char rule[] = "drop tcp dport 39999";

static int denied(int s)
{
	char b[512]; socklen_t l = sizeof(b);
	if (setsockopt(s, SOL_RANIX_FILTER, NF_RULE, "flush", 5) != -1 || errno != EPERM) return 2;
	if (setsockopt(s, SOL_RANIX_FILTER, NF_RULE, rule, sizeof(rule) - 1) != -1 || errno != EPERM) return 3;
	if (getsockopt(s, SOL_RANIX_FILTER, NF_RULE, b, &l) != -1 || errno != EPERM) return 4;
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	strcpy(r.ifr_name, "lo");
	if (ioctl(s, SIOCGIFFLAGS, &r)) return 5;
	return 0;
}

static int run(int s, uid_t uid, int userns)
{
	pid_t c = fork();
	if (c == 0) {
		if (uid && setuid(uid)) _exit(10);
		if (userns && unshare(CLONE_NEWUSER)) _exit(11);
		_exit(denied(s));
	}
	int st = -1;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

int main(void)
{
	char b[512]; socklen_t l = sizeof(b) - 1;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) { puts("FAIL socket"); return 1; }
	if (setsockopt(s, SOL_RANIX_FILTER, NF_RULE, rule, sizeof(rule) - 1)) { puts("FAIL root-add"); return 1; }
	int a = run(s, 1000, 0), u = run(s, 1000, 1), r = run(s, 0, 1);
	memset(b, 0, sizeof(b));
	int d = getsockopt(s, SOL_RANIX_FILTER, NF_RULE, b, &l);
	setsockopt(s, SOL_RANIX_FILTER, NF_RULE, "flush", 5);
	if (a || u || r) { printf("FAIL user=%d userns=%d rootns=%d\n", a, u, r); return 1; }
	if (d || !strstr(b, "39999")) { puts("FAIL root-dump"); return 1; }
	puts("OK");
	return 0;
}
