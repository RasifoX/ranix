/* SIOCGIF* socket ioctls — the ifreq/ifconf path used by ifconfig/ip. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int fail = 0;
static void check(const char *n, int ok)
{
	if (!ok) { printf("  FAIL: %s\n", n); fail = 1; }
}

int main(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	check("socket", fd >= 0);
	if (fd < 0) { printf("FAIL\n"); return 1; }

	/* SIOCGIFCONF: enumerate interfaces; expect loopback present */
	struct ifreq reqs[8];
	struct ifconf ifc;
	memset(&ifc, 0, sizeof(ifc));
	ifc.ifc_len = sizeof(reqs);
	ifc.ifc_req = reqs;
	check("gifconf", ioctl(fd, SIOCGIFCONF, &ifc) == 0);
	int n = ifc.ifc_len / (int)sizeof(struct ifreq);
	check("gifconf-count", n >= 1);

	int saw_lo = 0;
	for (int i = 0; i < n; i++)
		if (strcmp(reqs[i].ifr_name, "lo") == 0) saw_lo = 1;
	check("gifconf-lo", saw_lo);

	/* query the loopback explicitly */
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	strcpy(r.ifr_name, "lo");
	check("gifaddr", ioctl(fd, SIOCGIFADDR, &r) == 0);
	struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;
	check("gifaddr-family", sin->sin_family == AF_INET);
	check("gifaddr-127", ntohl(sin->sin_addr.s_addr) == 0x7f000001);

	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	check("gifnetmask", ioctl(fd, SIOCGIFNETMASK, &r) == 0);

	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	check("gifflags", ioctl(fd, SIOCGIFFLAGS, &r) == 0);
	check("gifflags-up", (r.ifr_flags & IFF_UP) != 0);
	check("gifflags-loop", (r.ifr_flags & IFF_LOOPBACK) != 0);

	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	check("gifmtu", ioctl(fd, SIOCGIFMTU, &r) == 0);
	check("gifmtu-val", r.ifr_mtu == 1500);

	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	check("gifindex", ioctl(fd, SIOCGIFINDEX, &r) == 0);
	check("gifindex-pos", r.ifr_ifindex >= 1);

	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "lo");
	check("gifhwaddr", ioctl(fd, SIOCGIFHWADDR, &r) == 0);

	/* unknown interface → ENODEV */
	memset(&r, 0, sizeof(r)); strcpy(r.ifr_name, "nope0");
	check("gifaddr-enodev", ioctl(fd, SIOCGIFADDR, &r) < 0);

	close(fd);
	printf(fail ? "FAIL\n" : "OK\n");
	return fail;
}
