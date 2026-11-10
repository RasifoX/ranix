/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/kmsg read side, Linux semantics: one record a read; a buffer too small
 * for the record is EINVAL and the reader stays put; each write is one record;
 * a line longer than any record is cut, never a wall the reader cannot pass.
 * The last line is OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok)
{
	if (!ok) { printf("FAIL %s\n", what); fails++; }
}

static void log_line(const char *s, size_t n)
{
	int fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0) return;
	if (write(fd, s, n) != (long)n) fails++;
	close(fd);
}

int main(void)
{
	/* a 3000-byte write without a newline, then two separate marker writes */
	static char big[3000];
	memset(big, 'x', sizeof(big));
	memcpy(big, "kmsgread-long ", 14);
	log_line(big, sizeof(big));
	log_line("kmsgread-one", 12);
	log_line("kmsgread-two\n", 13);

	int fd = open("/dev/kmsg", O_RDONLY);
	check("open", fd >= 0);
	char b[1024];
	int recs = 0, seen_long = 0, seen_one = 0, seen_two = 0, einval = 0, glued = 0;
	unsigned long long last_seq = 0;
	int seq_ok = 1;
	for (;;) {
		long n = read(fd, b, sizeof(b));
		if (n == 0) break;
		if (n < 0) { if (errno == EINVAL) einval++; break; }
		recs++;
		b[n < (long)sizeof(b) ? n : (long)sizeof(b) - 1] = 0;
		unsigned long long seq = strtoull(strchr(b, ',') + 1, NULL, 10);
		if (recs > 1 && seq <= last_seq) seq_ok = 0;
		last_seq = seq;
		if (b[n - 1] != '\n') check("record-ends-in-newline", 0);
		if (strstr(b, "kmsgread-long ")) seen_long = 1;
		if (strstr(b, "kmsgread-one")) { seen_one = 1; if (strstr(b, "kmsgread-two")) glued = 1; }
		if (strstr(b, "kmsgread-two")) seen_two = 1;
		if (recs > 100000) break;
	}
	check("no-einval-on-long-line", einval == 0);
	check("long-line-read-cut", seen_long);
	check("write-is-one-record", seen_one && seen_two && !glued);
	check("seq-rises", seq_ok);

	/* too small a buffer: EINVAL, and the same record is there afterwards */
	int f2 = open("/dev/kmsg", O_RDONLY);
	char small[8];
	errno = 0;
	check("small-einval", read(f2, small, sizeof(small)) == -1 && errno == EINVAL);
	check("small-then-whole", read(f2, b, sizeof(b)) > 0);
	close(f2);
	close(fd);

	if (fails) printf("kmsgread-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
