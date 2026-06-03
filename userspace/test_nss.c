/* NSS: musl resolves users from /etc/passwd and hosts from /etc/hosts with no
 * nsswitch indirection. getpwnam/getpwuid round-trip the root entry, and
 * gethostbyname("localhost") yields the loopback address from /etc/hosts. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <netdb.h>
#include <arpa/inet.h>

int main(void)
{
	struct passwd *pw = getpwnam("root");
	if (!pw || pw->pw_uid != 0 || strcmp(pw->pw_name, "root") != 0) {
		printf("nss-fail getpwnam\n"); return 0;
	}

	struct passwd *pw2 = getpwuid(0);
	if (!pw2 || strcmp(pw2->pw_name, "root") != 0) {
		printf("nss-fail getpwuid\n"); return 0;
	}

	struct group *gr = getgrnam("root");
	if (!gr || gr->gr_gid != 0) {
		printf("nss-fail getgrnam\n"); return 0;
	}

	struct hostent *he = gethostbyname("localhost");
	if (!he || !he->h_addr_list[0]) {
		printf("nss-fail gethostbyname\n"); return 0;
	}
	struct in_addr a;
	memcpy(&a, he->h_addr_list[0], sizeof(a));
	if (a.s_addr != htonl(0x7f000001)) {  /* 127.0.0.1 */
		printf("nss-fail loopback %s\n", inet_ntoa(a)); return 0;
	}

	printf("OK\n");
	return 0;
}
