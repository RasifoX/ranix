/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/mixer, the OSS mixer: master and PCM are the one level /sys/kernel/volume
 * shows, set from either side. root:audio 0660 like /dev/dsp. No sound device,
 * nothing to test. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int sys_get(void)
{
	char b[16] = {0};
	int fd = open("/sys/kernel/volume", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? atoi(b) : -1;
}

static void sys_set(int v)
{
	char b[16];
	int fd = open("/sys/kernel/volume", O_WRONLY), n = snprintf(b, sizeof b, "%d", v);
	if (fd >= 0) { if (write(fd, b, (size_t)n) < 0) {} close(fd); }
}

int main(void)
{
	if (access("/dev/dsp", F_OK) != 0) { puts("OK"); return 0; }
	struct stat st;
	if (stat("/dev/mixer", &st) != 0) { printf("FAIL no /dev/mixer errno=%d\n", errno); return 1; }
	if ((st.st_mode & 0777) != 0660 || st.st_gid != 18) { printf("FAIL mode %o gid %d\n", st.st_mode & 0777, (int)st.st_gid); return 1; }
	int fd = open("/dev/mixer", O_RDWR);
	if (fd < 0) { printf("FAIL open errno=%d\n", errno); return 1; }
	int was = sys_get(), v = 0;
	if (ioctl(fd, SOUND_MIXER_READ_DEVMASK, &v) || !(v & SOUND_MASK_VOLUME) || !(v & SOUND_MASK_PCM)) {
		printf("FAIL devmask %x\n", v); return 1;
	}
	mixer_info mi;
	if (ioctl(fd, SOUND_MIXER_INFO, &mi) || !mi.id[0]) { puts("FAIL info"); return 1; }
	v = 50 | 50 << 8;
	if (ioctl(fd, SOUND_MIXER_WRITE_PCM, &v) || v != (50 | 50 << 8)) { printf("FAIL write pcm %x\n", v); return 1; }
	if (sys_get() != 50) { printf("FAIL sysfs after mixer %d\n", sys_get()); return 1; }
	sys_set(30);
	if (ioctl(fd, SOUND_MIXER_READ_VOLUME, &v) || v != (30 | 30 << 8)) { printf("FAIL read after sysfs %x\n", v); return 1; }
	v = 0;
	if (ioctl(fd, SOUND_MIXER_READ_BASS, &v) == 0) { puts("FAIL bass is not a control"); return 1; }
	close(fd);
	if (was >= 0) sys_set(was);
	puts("OK");
	return 0;
}
