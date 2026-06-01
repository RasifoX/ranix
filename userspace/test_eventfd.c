/* eventfd: write a counter, poll reports it readable, read returns the value
 * and resets. Exercises the eventfd read/write/poll path. */
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>

int main(void)
{
	int efd = eventfd(0, 0);
	if (efd < 0) { puts("FAIL create"); return 1; }

	uint64_t v = 5;
	if (write(efd, &v, 8) != 8) { puts("FAIL write"); return 1; }

	struct pollfd pf = { .fd = efd, .events = POLLIN, .revents = 0 };
	if (poll(&pf, 1, 1000) != 1 || !(pf.revents & POLLIN)) { puts("FAIL poll"); return 1; }

	uint64_t out = 0;
	if (read(efd, &out, 8) != 8 || out != 5) { printf("FAIL read out=%llu\n", (unsigned long long)out); return 1; }

	close(efd);
	puts("OK");
	return 0;
}
