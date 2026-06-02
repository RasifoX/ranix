/* Real-internet TCP check: resolve a host, open a TCP connection out through the
 * gateway (SLIRP NAT), send an HTTP/1.0 GET, and print the status line. Proves
 * off-link routing + the full TCP stack against a real server. Ad-hoc — needs a
 * NIC and outbound connectivity; not part of the offline suite. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "example.com";

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) { printf("HTTP FAIL resolve\n"); return 1; }

	char ip[INET_ADDRSTRLEN];
	struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
	inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
	printf("HTTP resolved %s -> %s\n", host, ip);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { printf("HTTP FAIL socket\n"); return 1; }
	if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { printf("HTTP FAIL connect\n"); return 1; }
	printf("HTTP connected\n");

	char req[256];
	int n = snprintf(req, sizeof(req),
	                 "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
	if (write(fd, req, n) != n) { printf("HTTP FAIL send\n"); return 1; }

	char buf[2048];
	int total = 0, r;
	while (total < (int)sizeof(buf) - 1 && (r = (int)read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
		total += r;
	buf[total > 0 ? total : 0] = '\0';
	close(fd);

	if (total <= 0) { printf("HTTP FAIL no response\n"); return 1; }
	char *nl = strchr(buf, '\r');
	if (nl) *nl = '\0';
	printf("HTTP OK status: %s (%d bytes total)\n", buf, total);
	return 0;
}
