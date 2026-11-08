/* Input source marking (docs: arastirma/ajan-insan-ozellikleri.md, item 3).
 * Bytes software writes into /dev/input/rawkbd and /dev/input/mice come back
 * through the i8042 like a device's; the kernel must count them as source=injected,
 * never as a person. Checks:
 *  - /dev/input/source gives one line of counters, EOF after it, fresh after lseek;
 *  - an injected key press+release counts 2 injected keys, no hw keys, last=injected;
 *  - evdev puts EV_MSC/MSC_RANIX_SOURCE=1 in front of each injected key;
 *  - an injected mouse packet counts as an injected pointer event;
 *  - write "injected" counts a mark; anything else is EINVAL;
 *  - poll on the source fd stays quiet for injected input (it is for people only);
 * The hw side (a real key counts as hw and wakes poll) needs a
 * real keyboard and is checked on Casper. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>



struct ev { int64_t s, us; uint16_t type, code; int32_t value; };

static int fail(const char *m, int e)
{
	printf("FAIL %s errno=%d\n", m, e);
	return 1;
}

static long field(const char *line, const char *key)
{
	char k[32];
	snprintf(k, sizeof k, "%s=", key);
	const char *p = strstr(line, k);
	return p ? strtol(p + strlen(k), NULL, 10) : -999;
}

static int snap(char *out, size_t cap)
{
	int fd = open("/dev/input/source", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, out, cap - 1);
	close(fd);
	if (n <= 0) return -1;
	out[n] = 0;
	return 0;
}

static void msleep(int ms)
{
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

int main(void)
{
	char a[256], b[256];
	int sfd = open("/dev/input/source", O_RDWR);
	if (sfd < 0) return fail("open source", errno);
	ssize_t n = read(sfd, a, sizeof a - 1);
	if (n <= 0) return fail("read source", errno);
	a[n] = 0;
	if (field(a, "hw_keys") < 0 || field(a, "inj_marks") < 0 || !strstr(a, "last=")) {
		printf("FAIL line '%s'\n", a);
		return 1;
	}
	if (read(sfd, b, sizeof b) != 0) return fail("no EOF after the line", errno);
	pid_t c = fork();                       /* closed wall: not for everyone */
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(2);
		int f = open("/dev/input/source", O_RDONLY);
		_exit(f < 0 && errno == EACCES ? 0 : 3);
	}
	int cst = 0;
	waitpid(c, &cst, 0);
	if (!WIFEXITED(cst) || WEXITSTATUS(cst) != 0) return fail("non-root open", WEXITSTATUS(cst));

	int kfd = open("/dev/input/rawkbd", O_WRONLY);
	int efd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	int mfd = open("/dev/input/mice", O_RDWR | O_NONBLOCK);
	if (kfd < 0) return fail("open rawkbd", errno);
	if (efd < 0) return fail("open event0", errno);
	struct ev evs[64];
	while (read(efd, evs, sizeof evs) > 0)
		;
	if (snap(a, sizeof a)) return fail("snap", errno);

	/* F13 (set 1 0x64): no character, nothing on the console */
	unsigned char press = 0x64, rel = 0xE4;
	if (write(kfd, &press, 1) != 1 || write(kfd, &rel, 1) != 1) return fail("write rawkbd", errno);
	msleep(150);
	if (snap(b, sizeof b)) return fail("snap2", errno);
	if (field(b, "inj_keys") - field(a, "inj_keys") != 2) { printf("FAIL inj_keys %s -> %s", a, b); return 1; }
	if (field(b, "hw_keys") != field(a, "hw_keys")) { printf("FAIL hw_keys moved %s -> %s", a, b); return 1; }
	if (!strstr(b, "last=injected")) { printf("FAIL last %s", b); return 1; }

	ssize_t got = read(efd, evs, sizeof evs);
	int tagged = 0, keys = 0;
	for (int i = 0; got > 0 && i < (int)(got / (ssize_t)sizeof evs[0]); i++)
		if (evs[i].type == 1 && evs[i].code == 0x64) {
			keys++;
			if (i > 0 && evs[i - 1].type == 4 && evs[i - 1].code == 6 && evs[i - 1].value == 1) tagged++;
		}
	if (keys != 2 || tagged != 2) { printf("FAIL evdev keys=%d tagged=%d\n", keys, tagged); return 1; }

	/* poll is for people: injected input leaves it quiet */
	struct pollfd p = {sfd, POLLIN, 0};
	if (poll(&p, 1, 100) != 0) return fail("poll woke for injected input", 0);

	if (mfd >= 0) {
		unsigned char m[96];
		while (read(mfd, m, sizeof m) > 0)
			;
		static const unsigned char pk[3] = {0x08, 0x01, 0x00};
		if (snap(a, sizeof a)) return fail("snap3", errno);
		if (write(mfd, pk, 3) != 3) return fail("write mice", errno);
		msleep(150);
		while (read(mfd, m, sizeof m) > 0)
			;
		if (snap(b, sizeof b)) return fail("snap4", errno);
		if (field(b, "inj_ptr") - field(a, "inj_ptr") != 1 || field(b, "hw_ptr") != field(a, "hw_ptr")) {
			printf("FAIL pointer %s -> %s", a, b);
			return 1;
		}
	}

	if (snap(a, sizeof a)) return fail("snap5", errno);
	if (write(sfd, "injected\n", 9) != 9) return fail("write injected", errno);
	if (write(sfd, "hw\n", 3) != -1 || errno != EINVAL) return fail("write hw not EINVAL", errno);
	if (write(sfd, "injectedx", 9) != -1 || errno != EINVAL) return fail("write injectedx not EINVAL", errno);
	if (snap(b, sizeof b)) return fail("snap6", errno);
	if (field(b, "inj_marks") - field(a, "inj_marks") != 1) { printf("FAIL mark %s -> %s", a, b); return 1; }

	while (read(efd, evs, sizeof evs) > 0)
		;
	close(efd);
	close(kfd);
	if (mfd >= 0) close(mfd);
	close(sfd);
	printf("OK\n");
	return 0;
}
