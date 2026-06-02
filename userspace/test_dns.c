/* End-to-end DNS over the real NIC: resolve a hostname via getaddrinfo, which
 * reads /etc/resolv.conf and queries the nameserver over UDP:53. Proves unicast
 * TX/RX through the NIC plus ARP to the gateway. Ad-hoc (needs a NIC + a
 * reachable resolver); not part of the offline suite. */
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "example.com";
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0 || !res) { printf("DNS FAIL %s rc=%d\n", host, rc); return 1; }

	char ip[INET_ADDRSTRLEN];
	struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
	inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
	printf("DNS OK %s -> %s\n", host, ip);
	freeaddrinfo(res);
	return 0;
}
