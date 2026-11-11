/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The power button as a key, and the power files on a machine without a
 * battery. Run by tests/acpievents.py, which presses the button from the QEMU
 * monitor (system_powerdown) after "READY":
 *   - /dev/input/event0 gets KEY_POWER (116) down and up;
 *   - /sys/class/power_supply exists and is empty: QEMU has no battery, and
 *     nothing is made up;
 *   - /sys/kernel/lid says "unknown" (no lid driver here).
 * Prints "OK" or "FAIL <why>" as its last line; argv[1], if given, goes in front
 * of every line. */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct ev { int64_t s, us; uint16_t type, code; int32_t value; };

static int first_line(const char *p, char *b, size_t n)
{
	FILE *f = fopen(p, "r");
	if (!f) return -1;
	if (!fgets(b, (int)n, f)) b[0] = 0;
	fclose(f);
	b[strcspn(b, "\n")] = 0;
	return 0;
}

static const char *pfx = "";
#define SAY(...) do { printf("%s%s", pfx, *pfx ? " " : ""); printf(__VA_ARGS__); fflush(stdout); } while (0)

int main(int argc, char **argv)
{
	char b[64] = "", why[160] = "";
	if (argc > 1) pfx = argv[1];
	DIR *d = opendir("/sys/class/power_supply");
	int n = 0;
	struct dirent *e;
	if (!d) snprintf(why, sizeof why, "no /sys/class/power_supply; ");
	while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
	if (d) closedir(d);
	if (n) snprintf(why + strlen(why), sizeof why - strlen(why), "%d supplies on a machine without any; ", n);
	if (first_line("/sys/kernel/lid", b, sizeof b) || strcmp(b, "unknown"))
		snprintf(why + strlen(why), sizeof why - strlen(why), "lid '%s'; ", b);

	/* the button is checked whatever came before, so a kernel without it shows that too */
	int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { SAY("FAIL open event0\n"); return 1; }
	struct ev ev[32];
	while (read(fd, ev, sizeof ev) > 0) { }       /* whatever came before */
	SAY("READY\n");
	int down = 0, up = 0;
	time_t end = time(NULL) + 40;
	while (time(NULL) < end && !(down && up)) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 500) <= 0) continue;
		ssize_t r = read(fd, ev, sizeof ev);
		for (ssize_t i = 0; r > 0 && (i + 1) * (ssize_t)sizeof ev[0] <= r; i++)
			if (ev[i].type == 1 && ev[i].code == 116) { if (ev[i].value == 1) down = 1; else if (!ev[i].value) up = 1; }
	}
	close(fd);
	if (!(down && up)) snprintf(why + strlen(why), sizeof why - strlen(why), "no KEY_POWER (down %d up %d)", down, up);
	usleep(1500000);                                /* init says SIGPWR on the same console: not on top of us */
	if (why[0]) { SAY("FAIL %s\n", why); return 1; }
	SAY("OK\n");
	return 0;
}
