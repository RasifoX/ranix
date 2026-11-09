/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/self/mounts can be polled: a mount or umount since it was opened (or
 * last polled) gives EPOLLPRI, as Linux's mounts_poll does (LTP epoll_wait10).
 * Run in a private mount namespace so nothing reaches the rest of the system. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

static void expect(int ep, int pri, const char *when)
{
	struct epoll_event ev = { 0 };
	int n = epoll_wait(ep, &ev, 1, pri ? 1000 : 0);
	if (pri && (n != 1 || !(ev.events & EPOLLPRI))) { printf("after %s: %d events 0x%x, want EPOLLPRI\n", when, n, ev.events); bad++; }
	if (!pri && n != 0) { printf("%s: %d events 0x%x, want none\n", when, n, ev.events); bad++; }
}

static int child(void)
{
	if (unshare(CLONE_NEWNS)) { printf("unshare(CLONE_NEWNS): errno %d\n", errno); return 1; }
	/* Linux shares mounts with the parent namespace unless made private;
	 * Ranix has no propagation (each mount namespace is a private copy) and
	 * does not take the propagation flags, so a failure here is fine */
	mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
	mkdir("/tmp/mountspoll", 0755);
	int fd = open("/proc/self/mounts", O_RDONLY), ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLPRI };
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev)) { printf("epoll_ctl ADD /proc/self/mounts: errno %d\n", errno); return 1; }
	expect(ep, 0, "nothing changed");
	if (mount("none", "/tmp/mountspoll", "tmpfs", 0, NULL)) { printf("mount: errno %d\n", errno); return 1; }
	expect(ep, 1, "mount");
	expect(ep, 0, "the same change again");
	umount("/tmp/mountspoll");
	expect(ep, 1, "umount");
	rmdir("/tmp/mountspoll");
	return bad;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	pid_t c = fork();
	if (c == 0) _exit(child());
	int st;
	waitpid(c, &st, 0);
	puts(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "OK" : "FAIL");
	return 0;
}
