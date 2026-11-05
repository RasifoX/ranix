/* KDSETMODE keeps the console off the framebuffer.
 *
 * In KD_GRAPHICS a line of console text must not touch the pixels, even when
 * it scrolls, which repaints the whole screen. KD_TEXT brings the console back,
 * and so does the death of the program that asked for graphics.
 *
 * Needs /dev/fb0. Without one it says so on stderr and passes. */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define FBIOGET_FSCREENINFO 0x4602
#define MARK 0x00123456u

struct fix_info {
	char id[16];
	unsigned long smem_start;
	uint32_t smem_len, type, type_aux, visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length;
	unsigned long mmio_start;
	uint32_t mmio_len, accel;
	uint16_t capabilities, reserved[2];
};

int main(void)
{
	int fb = open("/dev/fb0", O_RDWR);
	if (fb < 0) {
		fputs("test_kdmode: no /dev/fb0, nothing tested\n", stderr);
		puts("OK");
		return 0;
	}
	int con = open("/dev/console", O_RDWR);
	struct fix_info fx;
	memset(&fx, 0, sizeof fx);
	if (con < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fx) != 0) { puts("FAIL setup"); return 0; }
	int mode = -1;
	if (ioctl(con, KDGETMODE, &mode) != 0 || mode != 0) { puts("FAIL getmode"); return 0; }
	if (ioctl(con, KDSETMODE, 7L) == 0) { puts("FAIL bad mode taken"); return 0; }
	volatile uint32_t *m = mmap(NULL, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (m == MAP_FAILED) { puts("FAIL mmap"); return 0; }
	size_t last = (size_t)fx.line_length / 4 * ((fx.smem_len / fx.line_length) - 8) + 4;

	if (ioctl(con, KDSETMODE, 1L) != 0) { puts("FAIL set graphics"); return 0; }
	if (ioctl(con, KDGETMODE, &mode) != 0 || mode != 1) { puts("FAIL mode not graphics"); return 0; }
	m[0] = MARK;
	m[last] = MARK;
	/* more lines than any screen has rows: every one of them scrolls */
	for (int i = 0; i < 400; i++) {
		char line[] = "test_kdmode line\n";
		if (write(con, line, strlen(line)) < 0) break;
	}
	usleep(1200000);        /* a cursor blink */
	if (m[0] != MARK || m[last] != MARK) {
		ioctl(con, KDSETMODE, 0L);
		puts("FAIL console reached the screen");
		return 0;
	}
	if (ioctl(con, KDSETMODE, 0L) != 0) { puts("FAIL set text"); return 0; }
	if (m[0] == MARK && m[last] == MARK) { puts("FAIL text mode did not repaint"); return 0; }

	/* Killed while holding the screen: the console comes back by itself. */
	pid_t pid = fork();
	if (pid == 0) {
		ioctl(con, KDSETMODE, 1L);
		for (;;) pause();
	}
	usleep(200000);
	if (ioctl(con, KDGETMODE, &mode) != 0 || mode != 1) { puts("FAIL child mode"); return 0; }
	kill(pid, SIGKILL);
	int st;
	waitpid(pid, &st, 0);
	if (ioctl(con, KDGETMODE, &mode) != 0 || mode != 0) { puts("FAIL console left dark"); return 0; }
	puts("OK");
	return 0;
}
