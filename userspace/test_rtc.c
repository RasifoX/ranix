/* real wall-clock from the RTC: CLOCK_REALTIME and file mtimes are real dates
 * (after 2020-01-01 = 1577836800), not uptime/1970. clock_settime round-trips
 * (then restores so the rest of the suite keeps a sane clock). */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define Y2020 1577836800L

int main(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now) != 0) { printf("rtc-fail gettime\n"); return 0; }
	if (now.tv_sec < Y2020) { printf("rtc-fail realdate sec=%ld\n", (long)now.tv_sec); return 0; }

	/* a freshly created file gets a real-date mtime, not uptime */
	int fd = open("/tmp/rtcf", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) { printf("rtc-fail create\n"); return 0; }
	write(fd, "x", 1); close(fd);
	struct stat s;
	if (stat("/tmp/rtcf", &s) != 0 || s.st_mtim.tv_sec < Y2020) {
		printf("rtc-fail mtime=%ld\n", (long)s.st_mtim.tv_sec); return 0;
	}

	/* clock_settime round-trip, then restore the real time */
	struct timespec set = { 1700000000L, 0 };
	if (clock_settime(CLOCK_REALTIME, &set) == 0) {
		struct timespec rb;
		clock_gettime(CLOCK_REALTIME, &rb);
		long d = (long)rb.tv_sec - 1700000000L;
		struct timespec back = { now.tv_sec + 1, 0 };
		clock_settime(CLOCK_REALTIME, &back);       /* restore */
		if (d < 0 || d > 5) { printf("rtc-fail settime d=%ld\n", d); return 0; }
	}

	printf("OK\n");
	return 0;
}
