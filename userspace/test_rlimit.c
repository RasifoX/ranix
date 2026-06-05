/* setrlimit(RLIMIT_NOFILE) actually lowers the limit: after dropping it, opening
 * past the cap returns EMFILE, and getrlimit round-trips the stored value. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>

int main(void)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) { printf("fail getrlimit\n"); return 0; }

	/* lower the soft cap to 8; subsequent opens must fail with EMFILE once we
	 * run out of slots below the cap. */
	rl.rlim_cur = 8;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) { printf("fail setrlimit\n"); return 0; }

	struct rlimit chk;
	if (getrlimit(RLIMIT_NOFILE, &chk) != 0 || chk.rlim_cur != 8) {
		printf("fail roundtrip cur=%lu\n", (unsigned long)chk.rlim_cur); return 0;
	}

	int got_emfile = 0;
	int fds[32];
	int n = 0;
	for (int i = 0; i < 32; i++) {
		int fd = open("/dev/null", O_RDONLY);
		if (fd < 0) {
			if (errno == EMFILE) got_emfile = 1;
			break;
		}
		fds[n++] = fd;
	}
	for (int i = 0; i < n; i++) close(fds[i]);

	if (!got_emfile) { printf("fail no-emfile n=%d\n", n); return 0; }
	if (n >= 8) { printf("fail too-many n=%d\n", n); return 0; }

	/* raising the soft limit back (within the hard limit) must succeed */
	rl.rlim_cur = chk.rlim_max < 64 ? chk.rlim_max : 64;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) { printf("fail raise\n"); return 0; }

	printf("OK\n");
	return 0;
}
