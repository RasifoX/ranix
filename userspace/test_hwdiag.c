/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/hwdiag: the hardware readings behind fjord doctor.
 * Format: first line "hwdiag 1", then "<class> <name> key=value ..." lines.
 * Under the test VM (ich9-ahci disk, qemu-xhci with a USB stick) this checks:
 * every line parses; thermal and power lines exist; the SATA disk line has
 * SMART read from the drive with health ok and no errors, also on a second
 * read (a SMART command must not count as a disk error); the xHCI line sees
 * the connected stick and no over-current. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static char buf[8192];

static int rd(void)
{
	int fd = open("/proc/hwdiag", O_RDONLY);
	if (fd < 0) return -1;
	int n = 0, r;
	while (n < (int)sizeof buf - 1 && (r = (int)read(fd, buf + n, sizeof buf - 1 - n)) > 0)
		n += r;
	close(fd);
	buf[n] = 0;
	return n;
}

/* value of key on the line starting with "cls name ", or NULL */
static const char *get(const char *cls, const char *name, const char *key, char *out, size_t cap)
{
	char head[64];
	snprintf(head, sizeof head, "%s %s ", cls, name);
	for (const char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (strncmp(l, head, strlen(head))) continue;
		const char *e = strchr(l, '\n');
		size_t kl = strlen(key);
		for (const char *p = l; p && p < e; p = strchr(p + 1, ' ')) {
			if (*p == ' ') p++;
			if (!strncmp(p, key, kl) && p[kl] == '=') {
				size_t n = strcspn(p + kl + 1, " \n");
				if (n >= cap) n = cap - 1;
				memcpy(out, p + kl + 1, n);
				out[n] = 0;
				return out;
			}
		}
		return NULL;
	}
	return NULL;
}

static int is_key(const char *s, size_t n)
{
	if (!n) return 0;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
	return 1;
}

static int check_format(void)
{
	if (strncmp(buf, "hwdiag 1\n", 9)) { printf("hwdiag-fail header\n"); return -1; }
	int lines = 0;
	for (char *l = buf + 9; *l; ) {
		char *e = strchr(l, '\n');
		if (!e) { printf("hwdiag-fail unterminated line\n"); return -1; }
		static const char *cls[] = { "thermal ", "power ", "net ", "disk ", "usb " };
		int ok = 0;
		for (unsigned i = 0; i < 5; i++) if (!strncmp(l, cls[i], strlen(cls[i]))) ok = 1;
		if (!ok) { printf("hwdiag-fail class %.20s\n", l); return -1; }
		char *p = strchr(l, ' ') + 1;            /* name */
		char *sp = memchr(p, ' ', (size_t)(e - p));
		if (!sp || sp == p) { printf("hwdiag-fail name %.30s\n", l); return -1; }
		for (p = sp + 1; p < e; ) {
			char *q = memchr(p, ' ', (size_t)(e - p));
			if (!q) q = e;
			char *eq = memchr(p, '=', (size_t)(q - p));
			if (!eq || !is_key(p, (size_t)(eq - p)) || eq + 1 == q) {
				printf("hwdiag-fail token %.*s\n", (int)(q - p), p);
				return -1;
			}
			p = q + 1;
		}
		lines++;
		l = e + 1;
	}
	return lines;
}

int main(void)
{
	char v[64];
	for (int pass = 0; pass < 2; pass++) {
		if (rd() <= 0) { printf("hwdiag-fail open\n"); return 0; }
		if (check_format() < 3) { printf("hwdiag-fail lines\n"); return 0; }
		if (!get("thermal", "cpu", "available", v, sizeof v)) { printf("hwdiag-fail thermal\n"); return 0; }
		if (!get("power", "supply", "available", v, sizeof v)) { printf("hwdiag-fail power\n"); return 0; }
		const char *need[][2] = { { "smart", "1" }, { "smart_read", "1" }, { "smart_health", "ok" },
		                          { "errors", "0" }, { "timeouts", "0" }, { "retries", "0" } };
		for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++) {
			if (!get("disk", "sata0", need[i][0], v, sizeof v) || strcmp(v, need[i][1])) {
				printf("hwdiag-fail pass%d disk %s=%s\n", pass, need[i][0],
				       get("disk", "sata0", need[i][0], v, sizeof v) ? v : "(none)");
				return 0;
			}
		}
		if (!get("disk", "sata0", "power_on_h", v, sizeof v)) { printf("hwdiag-fail smart attrs\n"); return 0; }
		if (!get("usb", "xhci0", "connected", v, sizeof v) || atoi(v) < 1) {
			printf("hwdiag-fail usb connected\n"); return 0;
		}
		if (!get("usb", "xhci0", "overcurrent_now", v, sizeof v) || strcmp(v, "0")) {
			printf("hwdiag-fail usb overcurrent\n"); return 0;
		}
	}
	printf("OK\n");
	return 0;
}
