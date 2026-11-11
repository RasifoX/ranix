/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* USB network adapter in the test VM: QEMU's usb-net on the xHCI bus, its far
 * side a user-mode network 172.22.11.0/24 whose gateway is 172.22.11.2 (where
 * a roboRIO would be) and whose 172.22.11.100:7 echoes TCP. Checks that
 * usbnet0 exists with the adapter's MAC, that the kernel's DHCP leased it an
 * address there, that 256 KB go round the echo intact, and that the lease did
 * not take over /etc/resolv.conf. Prints OK, or the first step that failed. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *step)
{
	printf("usbnet-fail %s errno=%d\n", step, errno);
	return 0;
}

static uint32_t addr_of(int s, const char *ifn)
{
	struct ifreq r;
	memset(&r, 0, sizeof r);
	strncpy(r.ifr_name, ifn, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFADDR, &r) != 0) return 0;
	return ntohl(((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr);
}

static int echo(void)
{
	int c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(7) };
	inet_pton(AF_INET, "172.22.11.100", &a.sin_addr);
	if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) return fail("connect");
	static char out[262144], in[262144];
	for (size_t i = 0; i < sizeof out; i++) out[i] = (char)(i * 7 + i / 1000);
	size_t sent = 0, got = 0;
	time_t end = time(0) + 30;
	while (got < sizeof in && time(0) < end) {
		struct pollfd p = { c, POLLIN | (sent < sizeof out ? POLLOUT : 0), 0 };
		if (poll(&p, 1, 500) <= 0) continue;
		if ((p.revents & POLLOUT) && sent < sizeof out) {
			ssize_t n = write(c, out + sent, sizeof out - sent > 8192 ? 8192 : sizeof out - sent);
			if (n > 0) sent += (size_t)n;
		}
		if (p.revents & POLLIN) {
			ssize_t n = read(c, in + got, sizeof in - got);
			if (n <= 0) break;
			got += (size_t)n;
		}
	}
	close(c);
	if (got != sizeof in || memcmp(in, out, sizeof in)) return printf("usbnet-fail echo %zu/%zu\n", got, sent), 0;
	return 1;
}

int main(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq r;
	memset(&r, 0, sizeof r);
	strcpy(r.ifr_name, "usbnet0");
	if (ioctl(s, SIOCGIFHWADDR, &r) != 0) return fail("no-usbnet0");
	static const unsigned char mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x77 };
	if (memcmp(r.ifr_hwaddr.sa_data, mac, 6)) return fail("mac");
	uint32_t ip = 0;
	for (int i = 0; i < 50 && (ip >> 8) != 0xAC160B; i++) {   /* 172.22.11.x */
		ip = addr_of(s, "usbnet0");
		if ((ip >> 8) != 0xAC160B) usleep(500000);
	}
	if ((ip >> 8) != 0xAC160B) return printf("usbnet-fail lease %08x\n", ip), 0;
	if (!echo()) return 0;
	FILE *f = fopen("/etc/resolv.conf", "r");
	char line[256];
	while (f && fgets(line, sizeof line, f))
		if (strstr(line, "172.22.11.")) return printf("usbnet-fail resolv.conf %s", line), 0;
	if (f) fclose(f);
	printf("OK\n");
	return 0;
}
