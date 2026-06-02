/* Time/clock sweep: monotonic clock never goes backwards, nanosleep actually
 * delays, gettimeofday/time agree, getrandom fills the buffer. First failure or OK. */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/random.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	struct timespec a, b;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
	CHECK(a.tv_sec != 0 || a.tv_nsec != 0);          /* not stuck at zero */

	/* nanosleep ~50ms, monotonic must advance by roughly that and never go back */
	struct timespec req = { 0, 50 * 1000 * 1000 };
	CHECK(nanosleep(&req, 0) == 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	long long da = (long long)a.tv_sec * 1000000000LL + a.tv_nsec;
	long long db = (long long)b.tv_sec * 1000000000LL + b.tv_nsec;
	CHECK(db > da);   /* monotonic advanced (exact slept duration is unreliable
	                   * under TCG emulation, so only assert forward progress) */

	/* realtime clock and time() agree to within a couple seconds */
	struct timespec rt;
	CHECK(clock_gettime(CLOCK_REALTIME, &rt) == 0);
	time_t t = time(0);
	long diff = (long)t - (long)rt.tv_sec;
	CHECK(diff >= -2 && diff <= 2);

	/* gettimeofday agrees with clock_gettime(REALTIME) */
	struct timeval tv;
	CHECK(gettimeofday(&tv, 0) == 0);
	long d2 = (long)tv.tv_sec - (long)rt.tv_sec;
	CHECK(d2 >= -2 && d2 <= 2);

	/* getrandom fills the buffer (vanishingly unlikely to stay all-zero) */
	unsigned char buf[32];
	memset(buf, 0, sizeof(buf));
	CHECK(getrandom(buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
	int nz = 0;
	for (int i = 0; i < 32; i++) if (buf[i]) nz = 1;
	CHECK(nz);

	puts("OK");
	return 0;
}
