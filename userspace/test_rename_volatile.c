/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* rename between two directories that are not in the dentry cache (procfs pid
 * directories, overlay subdirectories) looks at both, not twice at the second:
 * - rename("/proc/A/nosuch", "/proc/B/nosuch") for two live pids fails (Linux:
 *   ENOENT); it must not report success for a file that does not exist;
 * - in an overlay (tmpfs layers under DIR, when DIR is given), renaming
 *   d1/x to d2/x either moves it or fails: never 0 with x still in d1.
 * Both names resolve through the task's single slot for uncached entries; a
 * kernel that keeps a pointer to the first answer sees the second there and
 * takes the two directories for one.
 * Prints OK, or FAIL with the first case that differs. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); puts(""); fails++; } while (0)

static void procfs(void)
{
	pid_t c = fork();
	if (c == 0) { pause(); _exit(0); }
	char a[64], b[64];
	snprintf(a, sizeof(a), "/proc/%d/nosuch", (int)c);
	snprintf(b, sizeof(b), "/proc/%d/nosuch", (int)getpid());
	errno = 0;
	int r = rename(a, b);
	if (r == 0) FAIL("rename %s -> %s reported success", a, b);
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void overlay(const char *dir)
{
	char p[256], q[256], opt[512];
	mkdir(dir, 0755);
	if (mount("tmpfs", dir, "tmpfs", 0, "size=1m")) { FAIL("mount tmpfs errno %d", errno); return; }
	snprintf(p, sizeof(p), "%s/lower", dir); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/lower/d1", dir); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/lower/d2", dir); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/lower/d1/x", dir); close(open(p, O_CREAT | O_WRONLY, 0644));
	snprintf(p, sizeof(p), "%s/up", dir); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/merged", dir); mkdir(p, 0755);
	snprintf(opt, sizeof(opt), "lowerdir=%s/lower,upperdir=%s/up", dir, dir);
	if (mount("overlay", p, "overlay", 0, opt)) { FAIL("mount overlay errno %d", errno); umount(dir); return; }
	snprintf(p, sizeof(p), "%s/merged/d1/x", dir);
	snprintf(q, sizeof(q), "%s/merged/d2/x", dir);
	int r = rename(p, q);
	if (r == 0 && (exists(p) || !exists(q)))
		FAIL("overlay rename d1/x -> d2/x reported success, d1/x %s, d2/x %s",
		     exists(p) ? "still there" : "gone", exists(q) ? "there" : "missing");
	snprintf(p, sizeof(p), "%s/merged", dir);
	if (umount(p)) FAIL("umount overlay errno %d", errno);
	if (umount(dir)) FAIL("umount tmpfs errno %d", errno);
}

int main(int argc, char **argv)
{
	procfs();
	if (argc > 1) overlay(argv[1]);
	if (!fails) puts("OK");
	return fails != 0;
}
