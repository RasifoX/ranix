/* user namespace: after unshare(CLONE_NEWUSER) the caller's id is unmapped
 * (reads as the overflow id) until a uid_map is written. Mapping ns-uid 100
 * to the caller's real uid (kuid 0) makes getuid() report 100 — the kuid
 * model in action (rootless-container basis). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>

int main(void)
{
	if (unshare(CLONE_NEWUSER) != 0) { printf("userns-fail unshare\n"); return 0; }

	int before = (int)getuid();   /* unmapped -> overflow id 65534 */

	int fd = open("/proc/self/uid_map", O_WRONLY);
	if (fd < 0) { printf("userns-fail open\n"); return 0; }
	write(fd, "100 0 1\n", 8);    /* ns-uid 100 <- kuid 0 */
	close(fd);

	int after = (int)getuid();    /* kuid 0 now reads as ns-uid 100 */

	if (before == 65534 && after == 100)
		printf("userns-ok\n");
	else
		printf("userns-fail before=%d after=%d\n", before, after);
	return 0;
}
