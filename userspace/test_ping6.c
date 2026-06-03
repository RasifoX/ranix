/* ICMPv6 echo over IPv6: send an echo request on a raw AF_INET6 socket and
 * verify a matching reply. With no argument it pings ::1 (loopback — exercises
 * the IPv6 header, ICMPv6, pseudo-header checksum). With an address argument it
 * pings that peer, exercising neighbour discovery (NS/NA) over a real link. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129

int main(int argc, char **argv)
{
	int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (s < 0) { puts("FAIL socket"); return 1; }

	struct sockaddr_in6 d;
	memset(&d, 0, sizeof(d));
	d.sin6_family = AF_INET6;
	if (argc > 1) {
		if (inet_pton(AF_INET6, argv[1], &d.sin6_addr) != 1) { puts("FAIL addr"); return 1; }
	} else {
		d.sin6_addr = in6addr_loopback;   /* ::1 */
	}

	unsigned char req[16];
	memset(req, 0, sizeof(req));
	req[0] = ICMP6_ECHO_REQUEST;
	req[4] = 0x12; req[5] = 0x34;       /* id  */
	req[6] = 0x00; req[7] = 0x01;       /* seq */
	memcpy(req + 8, "ranix6!", 7);

	if (sendto(s, req, sizeof(req), 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
		puts("FAIL send"); return 1;
	}

	/* A raw ICMPv6 socket receives every ICMPv6 message (NS/NA/echo-request
	 * too), so read until our matching echo reply arrives, skipping the rest. */
	for (int tries = 0; tries < 64; tries++) {
		unsigned char rep[128];
		int n = (int)recvfrom(s, rep, sizeof(rep), 0, NULL, NULL);
		if (n < 16) continue;
		if (rep[0] == ICMP6_ECHO_REPLY &&
		    rep[4] == 0x12 && rep[5] == 0x34 &&
		    rep[6] == 0x00 && rep[7] == 0x01 &&
		    memcmp(rep + 8, "ranix6!", 7) == 0) {
			puts("OK");
			return 0;
		}
	}
	puts("FAIL");
	return 1;
}
