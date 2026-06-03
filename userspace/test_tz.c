/* timezone: musl honours a POSIX TZ string (no data files needed), a named
 * zone read from /usr/share/zoneinfo (tzdata), and the default /etc/localtime
 * when TZ is unset. Istanbul is UTC+3 year-round, so tm_gmtoff is 10800s. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
	time_t t = 1700000000; /* 2023-11-14, fixed for determinism */

	/* POSIX TZ string: "GMT-3" means local = UTC+3, no data file required */
	setenv("TZ", "GMT-3", 1); tzset();
	struct tm a = *localtime(&t);
	if (a.tm_gmtoff != 3 * 3600) { printf("tz-fail posix gmtoff=%ld\n", a.tm_gmtoff); return 0; }

	/* named zone from tzdata (/usr/share/zoneinfo/Europe/Istanbul) */
	setenv("TZ", "Europe/Istanbul", 1); tzset();
	struct tm b = *localtime(&t);
	if (b.tm_gmtoff != 3 * 3600) { printf("tz-fail zoneinfo gmtoff=%ld\n", b.tm_gmtoff); return 0; }

	/* default zone from /etc/localtime (TZ unset) */
	unsetenv("TZ"); tzset();
	struct tm c = *localtime(&t);
	if (c.tm_gmtoff != 3 * 3600) { printf("tz-fail localtime gmtoff=%ld\n", c.tm_gmtoff); return 0; }

	printf("OK\n");
	return 0;
}
