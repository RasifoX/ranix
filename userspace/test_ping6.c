/* ICMPv6 echo over IPv6 loopback (::1): send an echo request on a raw AF_INET6
 * socket and verify the kernel loops back a matching echo reply. Exercises the
 * IPv6 header, ICMPv6, the pseudo-header checksum, and ::1 routing. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129

int main(void)
{
	int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (s < 0) { puts("FAIL socket"); return 1; }

	struct sockaddr_in6 d;
	memset(&d, 0, sizeof(d));
	d.sin6_family = AF_INET6;
	d.sin6_addr   = in6addr_loopback;   /* ::1 */

	unsigned char req[16];
	memset(req, 0, sizeof(req));
	req[0] = ICMP6_ECHO_REQUEST;
	req[4] = 0x12; req[5] = 0x34;       /* id  */
	req[6] = 0x00; req[7] = 0x01;       /* seq */
	memcpy(req + 8, "ranix6!", 7);

	if (sendto(s, req, sizeof(req), 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
		puts("FAIL send"); return 1;
	}

	unsigned char rep[64];
	int n = (int)recvfrom(s, rep, sizeof(rep), 0, NULL, NULL);
	if (n < 16) { puts("FAIL recv"); return 1; }

	int ok = rep[0] == ICMP6_ECHO_REPLY &&
	         rep[4] == 0x12 && rep[5] == 0x34 &&
	         rep[6] == 0x00 && rep[7] == 0x01 &&
	         memcmp(rep + 8, "ranix6!", 7) == 0;
	puts(ok ? "OK" : "FAIL");
	return 0;
}
