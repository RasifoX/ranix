/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* splice and copy_file_range follow Linux's rules on what they join. splice
 * needs a pipe at one end (EINVAL otherwise, ESPIPE for an offset on the pipe
 * end) and returns once data moved instead of waiting on a full or empty pipe;
 * copy_file_range joins regular files only. Ranix copied between any two fds,
 * so one unprivileged call poured up to 2 GB of /dev/zero into a RAM-backed
 * file, in the kernel the whole time (test_fuzz: soft lockups, then the OOM
 * killer took bystanders). Every call runs in a child ended after 3 s, so a
 * hang is reported, not waited for. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

static int zero, null_fd, file;
static int pp[2];

enum { SPLICE_FILES, SPLICE_ZERO_FILE, SPLICE_OFF_PIPE, SPLICE_FULL_PIPE,
       SPLICE_EMPTY_PIPE, CFR_ZERO, CFR_FILES, NCASE };
static const char *const name[] = {
	"splice file to /dev/null (no pipe)", "splice /dev/zero to a file (no pipe)",
	"splice with an offset on the pipe end", "splice 1 MB of /dev/zero into a pipe",
	"splice from a pipe holding 3 bytes, asking 1 MB", "copy_file_range from /dev/zero",
	"copy_file_range file to file" };

/* the child's verdict: 0 right, 1 wrong; it prints what it saw when wrong */
static int one(int c)
{
	ssize_t r;
	loff_t off = 0;
	switch (c) {
	case SPLICE_FILES:
		r = splice(file, NULL, null_fd, NULL, 4096, 0);
		if (r == -1 && errno == EINVAL) return 0;
		break;
	case SPLICE_ZERO_FILE:
		r = splice(zero, NULL, file, NULL, 1 << 20, 0);
		if (r == -1 && errno == EINVAL) return 0;
		break;
	case SPLICE_OFF_PIPE:
		r = splice(zero, NULL, pp[1], &off, 16, 0);
		if (r == -1 && errno == ESPIPE) return 0;
		break;
	case SPLICE_FULL_PIPE:   /* fills what fits, then returns */
		r = splice(zero, NULL, pp[1], NULL, 1 << 20, 0);
		if (r > 0 && r < (1 << 20)) return 0;
		break;
	case SPLICE_EMPTY_PIPE:  /* the writer stays open: takes 3, returns */
		if (write(pp[1], "abc", 3) != 3) return 1;
		r = splice(pp[0], NULL, file, NULL, 1 << 20, 0);
		if (r == 3) return 0;
		break;
	case CFR_ZERO:
		r = copy_file_range(zero, NULL, file, NULL, 1 << 20, 0);
		if (r == -1 && errno == EINVAL) return 0;
		break;
	default: {
		int dst = open("/tmp/splicerule.dst", O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (dst < 0 || lseek(file, 0, SEEK_SET) != 0) return 1;
		r = copy_file_range(file, NULL, dst, NULL, 100, 0);
		if (r == 5) return 0;
		break; }
	}
	printf("  got %zd errno %d\n", r, r < 0 ? errno : 0);
	return 1;
}

int main(void)
{
	zero = open("/dev/zero", O_RDONLY);
	null_fd = open("/dev/null", O_WRONLY);
	for (int c = 0; c < NCASE; c++) {
		file = open("/tmp/splicerule.src", O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (zero < 0 || null_fd < 0 || file < 0 || write(file, "hello", 5) != 5 || pipe(pp)) {
			FAIL("setup: %s", strerror(errno));
			return 1;
		}
		fflush(stdout);
		pid_t p = fork();
		if (p == 0) _exit(one(c));
		int st = 0, done = 0;
		for (int i = 0; i < 300 && !done; i++) {
			if (waitpid(p, &st, WNOHANG) == p) done = 1;
			else usleep(10000);
		}
		if (!done) {
			kill(p, SIGKILL);
			waitpid(p, NULL, 0);
			FAIL("%s hangs", name[c]);
		} else if (!WIFEXITED(st) || WEXITSTATUS(st)) {
			FAIL("%s", name[c]);
		}
		close(file);
		close(pp[0]);
		close(pp[1]);
	}
	unlink("/tmp/splicerule.src");
	unlink("/tmp/splicerule.dst");
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
