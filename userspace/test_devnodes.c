/* /dev/kmsg (kernel log ring, dmesg) and /dev/rtc0 (RTC_RD_TIME). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct rtc_time {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};
#define RTC_RD_TIME _IOR('p', 0x09, struct rtc_time)

static int fail = 0;
static void check(const char *n, int ok)
{
	if (!ok) { printf("  FAIL: %s\n", n); fail = 1; }
}

int main(void)
{
	/* /dev/kmsg: write a marker, then read the log and find it */
	int wfd = open("/dev/kmsg", O_WRONLY);
	check("kmsg-open-w", wfd >= 0);
	const char *marker = "ranix-kmsg-marker-42\n";
	if (wfd >= 0) { write(wfd, marker, strlen(marker)); close(wfd); }

	int rfd = open("/dev/kmsg", O_RDONLY);
	check("kmsg-open-r", rfd >= 0);
	/* The log ring can be larger than any reasonable fixed buffer and is
	 * returned oldest-first, so the marker we just wrote is at the tail.
	 * Drain incrementally, carrying a small overlap so the marker can't be
	 * split across reads, instead of trying to hold the whole ring. */
	char buf[8192];
	int total = 0, n, found = 0;
	const char *mk = "ranix-kmsg-marker-42";
	size_t mklen = strlen(mk);
	int carry = 0;
	while (rfd >= 0 && (n = read(rfd, buf + carry, sizeof(buf) - 1 - carry)) > 0) {
		int len = carry + n;
		buf[len] = '\0';
		total += n;
		if (strstr(buf, mk)) { found = 1; break; }
		/* keep the last (mklen-1) bytes so a split marker is still found */
		carry = (len >= (int)mklen - 1) ? (int)mklen - 1 : len;
		memmove(buf, buf + len - carry, carry);
	}
	if (rfd >= 0) close(rfd);
	check("kmsg-nonempty", total > 0);
	check("kmsg-has-marker", found);

	/* /dev/rtc0: RTC_RD_TIME gives a plausible date (year >= 2020 = 120) */
	int dfd = open("/dev/rtc0", O_RDONLY);
	check("rtc-open", dfd >= 0);
	struct rtc_time t;
	memset(&t, 0, sizeof(t));
	check("rtc-rdtime", dfd >= 0 && ioctl(dfd, RTC_RD_TIME, &t) == 0);
	if (dfd >= 0) close(dfd);
	check("rtc-year", t.tm_year >= 120);          /* >= 2020 */
	check("rtc-mon", t.tm_mon >= 0 && t.tm_mon <= 11);
	check("rtc-mday", t.tm_mday >= 1 && t.tm_mday <= 31);
	check("rtc-hour", t.tm_hour >= 0 && t.tm_hour <= 23);

	printf(fail ? "FAIL\n" : "OK\n");
	return fail;
}
