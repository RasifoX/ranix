/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/net/tcp speaks Linux: a listening socket shows state 0A and its owner's
 * uid, so netstat, ss and a door listing see what is open and whose it is.
 * Prints OK, or FAIL with what was wrong. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>

static int check(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(47123) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) { puts("FAIL listen"); return 0; }
	FILE *f = fopen("/proc/net/tcp", "r");
	if (!f) { puts("FAIL open /proc/net/tcp"); return 0; }
	char line[256], st[8] = "";
	unsigned uid = 99999;
	while (fgets(line, sizeof line, f)) {
		char local[32];
		if (sscanf(line, " %*d: %31s %*s %7s %*s %*s %*s %u", local, st, &uid) == 3 && strstr(local, ":B813"))
			break;
		st[0] = 0;
	}
	fclose(f);
	if (!st[0]) { puts("FAIL listener not listed"); return 0; }
	if (strcmp(st, "0A")) { printf("FAIL listener state %s, want 0A\n", st); return 0; }
	if (uid != (unsigned)getuid()) { printf("FAIL listener uid %u, want %u\n", uid, (unsigned)getuid()); return 0; }
	puts("OK");
	return 0;
}

int main(void)
{
	/* as nobody, so the uid column is shown to be the listener's, not a constant 0 */
	if (getuid() == 0) {
		pid_t p = fork();
		if (p == 0) { if (setgid(65534) || setuid(65534)) { puts("FAIL setuid"); _exit(0); } check(); fflush(stdout); _exit(0); }
		waitpid(p, NULL, 0);
		return 0;
	}
	return check();
}
