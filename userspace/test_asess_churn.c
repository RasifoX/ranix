/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Sessions come and go while the network runs: every one creates and destroys a
 * namespace and a loopback that the receive thread is walking. Each session
 * leaves a socket open, so its last references drop on the way out. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include "asess_test.h"

#define ROUNDS 60
#define PORT 9483

static void noise(void)
{
	struct sockaddr_in in;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	in.sin_port = htons(PORT);
	int rx = socket(AF_INET, SOCK_DGRAM, 0), tx = socket(AF_INET, SOCK_DGRAM, 0);
	bind(rx, (struct sockaddr *)&in, sizeof(in));
	fcntl(rx, F_SETFL, O_NONBLOCK);
	char b[32] = "x";
	for (;;) {
		sendto(tx, b, sizeof(b), 0, (struct sockaddr *)&in, sizeof(in));
		recv(rx, b, sizeof(b), 0);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	pid_t n = fork();
	if (n == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); noise(); }

	for (int i = 0; i < ROUNDS; i++) {
		long sfd = sess_create("/", UID, NULL, 0, 0);
		if (sfd < 0) { printf("FAIL create round %d: errno %d\n", i, errno); fails++; break; }
		long pid = sess_spawn(sfd);
		if (pid == 0) {
			struct sockaddr_in in;
			memset(&in, 0, sizeof(in));
			in.sin_family = AF_INET;
			in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			in.sin_port = htons(PORT);
			int u = socket(AF_INET, SOCK_DGRAM, 0);
			sendto(u, "y", 1, 0, (struct sockaddr *)&in, sizeof(in));
			int t = socket(AF_INET, SOCK_STREAM, 0);
			connect(t, (struct sockaddr *)&in, sizeof(in));
			_exit(0);
		}
		if (pid < 0) { printf("FAIL spawn round %d: errno %d\n", i, errno); fails++; break; }
		close((int)sfd);
		waitpid((pid_t)pid, NULL, 0);
	}
	kill(n, SIGKILL);
	waitpid(n, NULL, 0);
	return finish();
}
