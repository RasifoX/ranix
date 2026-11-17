/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A directory renamed under itself. The check compared path strings, so a path
 * that reaches the directory again through a symlink got past it: the directory
 * went into its own child list, a cycle cut off from the tree, and a later walk
 * read a freed dentry (a kernel page fault in the path walk, found by
 * test_fuzz). Linux answers EINVAL however the new path is spelled. Two tasks
 * renaming two directories into each other at once must not build the cycle
 * either. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define B "/tmp/renameloop"

static int bad;

static void want_einval(const char *what, int r)
{
	if (r == 0 || errno != EINVAL) {
		printf("FAIL: %s: r=%d %s, want EINVAL\n", what, r, r == 0 ? "ok" : strerror(errno));
		bad++;
	}
}

int main(void)
{
	struct stat st;
	setvbuf(stdout, NULL, _IONBF, 0);   /* a timeout must still show how far it got */
	mkdir(B, 0755);
	mkdir(B "/d", 0755);
	mkdir(B "/d/sub", 0755);
	symlink("d", B "/l");
	symlink("d/sub", B "/ls");

	want_einval("into itself through a symlink", rename(B "/d", B "/l/x"));
	want_einval("into its child through a symlink", rename(B "/d", B "/ls/x"));
	want_einval("into itself through ..", rename(B "/d", B "/d/sub/../x"));
	if (stat(B "/d/sub", &st) != 0) { printf("FAIL: d/sub lost: %s\n", strerror(errno)); bad++; }

	/* two directories swapped into each other from two tasks */
	mkdir(B "/p", 0755);
	mkdir(B "/q", 0755);
	pid_t c = fork();
	if (c == 0) {
		for (int i = 0; i < 3000; i++) {
			rename(B "/p", B "/q/p");
			rename(B "/q/p", B "/p");
		}
		_exit(0);
	}
	for (int i = 0; i < 3000; i++) {
		rename(B "/q", B "/p/q");
		rename(B "/p/q", B "/q");
	}
	waitpid(c, NULL, 0);
	printf("swaps done\n");
	/* both still reachable from the top, whichever way they ended */
	int found = 0;
	const char *where[] = { B "/p", B "/q", B "/p/q", B "/q/p" };
	for (int i = 0; i < 4; i++) found += stat(where[i], &st) == 0;
	if (found != 2) { printf("FAIL: %d of p and q reachable, want 2 (a cycle cut them off)\n", found); bad++; }

	rmdir(B "/p/q"); rmdir(B "/q/p"); rmdir(B "/p"); rmdir(B "/q");
	unlink(B "/l"); unlink(B "/ls");
	rmdir(B "/d/sub"); rmdir(B "/d"); rmdir(B);
	if (!bad) printf("OK\n");
	return bad != 0;
}
