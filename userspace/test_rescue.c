/* Rescue channel (kernel/rescue.c, include/uapi/rescue.h) over loopback, with a
 * throw-away test key so the real one is never touched: HELLO gives a nonce; a
 * MACed command is answered with a MACed reply; a replay, a bad MAC and a nonce
 * reused after a bad MAC are refused; ps lists us; kill ends a child; stuck
 * answers. reboot/poweroff are not exercised here (they reset the machine). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rx_hmac.h"

#define PORT 7395
static int sock;
static struct sockaddr_in dst;
static uint8_t key[32];

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

/* The channel answers at most 40 packets a second (kernel/rescue.c): pace the
 * requests so back-to-back runs of this test stay under it. */
static void pace(void) { usleep(40000); }

static int xfer(const char *msg, char *rep, size_t cap)
{
	pace();
	sendto(sock, msg, strlen(msg), 0, (void *)&dst, sizeof dst);
	int n = (int)recv(sock, rep, cap - 1, 0);
	if (n < 0) return -1;
	rep[n] = 0;
	return n;
}

static int hello(char nonce[33])
{
	char r[256];
	if (xfer("RX1 HELLO", r, sizeof r) < 0) return -1;
	if (strncmp(r, "RX1 NONCE ", 10) != 0) { printf("hello: %s", r); return -1; }
	memcpy(nonce, r + 10, 32); nonce[32] = 0;
	return 0;
}

static void build(char *out, size_t cap, const char *nonce, const char *cmd, int corrupt)
{
	char pre[64]; snprintf(pre, sizeof pre, "RX1 CMD %s ", nonce);
	uint8_t mac[32]; char mh[65];
	rx_hmac(key, pre, strlen(pre), cmd, strlen(cmd), mac);
	if (corrupt) mac[0] ^= 1;
	rx_hex(mh, mac, 32);
	snprintf(out, cap, "RX1 CMD %s %s %s", nonce, mh, cmd);
}

/* Sends a built request, collects every reply packet, checks each MAC. 0 ok,
 * else -1 with the error line in body. */
static int collect(const char *req, char *body, size_t cap)
{
	pace();
	sendto(sock, req, strlen(req), 0, (void *)&dst, sizeof dst);
	size_t used = 0; body[0] = 0;
	for (int seq = 0; seq < 64; seq++) {
		char p[1600];
		int n = (int)recv(sock, p, sizeof p - 1, 0);
		if (n < 0) { snprintf(body, cap, "timeout"); return -1; }
		p[n] = 0;
		if (!strncmp(p, "RX1 ERR ", 8)) { snprintf(body, cap, "%s", p + 8); return -1; }
		char *nl = memchr(p, '\n', (size_t)n);
		if (strncmp(p, "RX1 R ", 6) || !nl) { snprintf(body, cap, "garbage"); return -1; }
		char *sp = nl;                            /* head ends before " <mac>" */
		while (sp > p && *sp != ' ') sp--;
		if (nl - sp - 1 != 64) { snprintf(body, cap, "no mac"); return -1; }
		uint8_t mac[32]; char mh[65];
		size_t hl = (size_t)(sp - p);
		char head[128]; memcpy(head, p, hl); head[hl] = '\n';
		rx_hmac(key, head, hl + 1, nl + 1, (size_t)(n - (nl + 1 - p)), mac);
		rx_hex(mh, mac, 32);
		if (memcmp(mh, sp + 1, 64)) { snprintf(body, cap, "reply mac wrong"); return -1; }
		size_t bl = (size_t)(n - (nl + 1 - p));
		if (used + bl < cap) { memcpy(body + used, nl + 1, bl); used += bl; body[used] = 0; }
		int more = sp[-1] == '1';
		if (!more) return 0;
	}
	return 0;
}

static int cmd(const char *c, char *body, size_t cap)
{
	char nonce[33], req[512];
	if (hello(nonce)) { snprintf(body, cap, "no nonce"); return -1; }
	build(req, sizeof req, nonce, c, 0);
	return collect(req, body, cap);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int fd = open("/dev/rescue", O_RDWR);
	if (fd < 0) FAIL("open /dev/rescue: %s", strerror(errno));
	if (getrandom(key, sizeof key, 0) != sizeof key) FAIL("getrandom");
	char line[160], kh[65];
	rx_hex(kh, key, 32);
	snprintf(line, sizeof line, "testkey %s\n", kh);
	if (write(fd, line, strlen(line)) < 0) FAIL("load test key: %s", strerror(errno));
	if (write(fd, "key zz\n", 7) >= 0 || errno != EINVAL) FAIL("bad key accepted");
	close(fd);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct timeval tv = { 2, 0 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	bind(sock, (void *)&me, sizeof me);
	dst = me; dst.sin_port = htons(PORT);

	static char body[65536];
	/* ping */
	char nonce[33], req[512];
	if (hello(nonce)) FAIL("no HELLO answer (is the channel on udp %d?)", PORT);
	build(req, sizeof req, nonce, "ping", 0);
	if (collect(req, body, sizeof body)) FAIL("ping: %s", body);
	if (strncmp(body, "ok uptime_s=", 12)) FAIL("ping body: %s", body);
	printf("ping: %s", body);
	/* replay of the very same request */
	if (collect(req, body, sizeof body) == 0 || !strstr(body, "stale")) FAIL("replay not refused: %s", body);
	printf("replay refused: %s", body);
	/* bad MAC, then the right MAC on the spent nonce */
	if (hello(nonce)) FAIL("hello 2");
	build(req, sizeof req, nonce, "ping", 1);
	if (collect(req, body, sizeof body) == 0 || !strstr(body, "bad mac")) FAIL("bad mac not refused: %s", body);
	build(req, sizeof req, nonce, "ping", 0);
	if (collect(req, body, sizeof body) == 0 || !strstr(body, "stale")) FAIL("nonce survived a bad mac: %s", body);
	printf("bad mac refused, nonce spent\n");
	/* garbage */
	char r[256];
	if (xfer("RX1 CMD zz", r, sizeof r) < 0 || strncmp(r, "RX1 ERR", 7)) FAIL("garbage: %s", r);
	/* ps names us */
	if (cmd("ps", body, sizeof body)) FAIL("ps: %s", body);
	char pid[32]; snprintf(pid, sizeof pid, "\n%d ", (int)getpid());
	if (!strstr(body, pid)) FAIL("ps does not list pid %d", (int)getpid());
	int lines = 0; for (char *p = body; *p; p++) lines += *p == '\n';
	printf("ps: %d lines\n", lines);
	/* kill */
	pid_t c = fork();
	if (c == 0) { for (;;) pause(); }
	usleep(50000);
	char k[64]; snprintf(k, sizeof k, "kill %d", (int)c);
	if (cmd(k, body, sizeof body) || !strstr(body, "signalled")) FAIL("kill: %s", body);
	int st = 0;
	alarm(5);
	waitpid(c, &st, 0);
	alarm(0);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) FAIL("child not killed by rescue");
	printf("kill: %s", body);
	if (cmd("stuck", body, sizeof body) || strncmp(body, "kwatch ", 7)) FAIL("stuck: %s", body);
	if (cmd("log 300", body, sizeof body) || !strstr(body, "rescue:")) FAIL("log: no rescue lines");
	if (cmd("claims", body, sizeof body)) FAIL("claims: %s", body);
	printf("OK\n");
	return 0;
}
