/* /dev/dsp plays at the sample rate: a second of samples takes about a second,
 * which only a running DMA stream can give. No sound device, nothing to test. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>

static short buf[48000 * 2];

int main(void)
{
	int fd = open("/dev/dsp", O_WRONLY);
	if (fd < 0) { puts("OK"); return 0; }
	int v = AFMT_S16_LE;
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &v)) { puts("FAIL fmt"); return 1; }
	v = 2;
	if (ioctl(fd, SNDCTL_DSP_CHANNELS, &v) || v != 2) { puts("FAIL channels"); return 1; }
	v = 48000;
	if (ioctl(fd, SNDCTL_DSP_SPEED, &v) || v != 48000) { puts("FAIL speed"); return 1; }
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	if (write(fd, buf, sizeof buf) != sizeof buf) { puts("FAIL write"); return 1; }
	ioctl(fd, SNDCTL_DSP_SYNC, 0);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	if (ms < 700) { printf("FAIL played in %ld ms\n", ms); return 1; }
	close(fd);
	int bad = open("/dev/dsp", O_WRONLY);
	if (bad < 0) { puts("FAIL reopen"); return 1; }
	close(bad);
	puts("OK");
	return 0;
}
