/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Claims (kernel/claim.c, include/uapi/claim.h). Uses resource names under
 * "test." so it never collides with real claims, except the reboot gate, which
 * needs "reboot" itself and holds it for well under a second.
 *  - exclusive vs exclusive: EBUSY naming holder and purpose; shared + shared ok
 *  - same holder re-takes; detached needs an expiry and survives close; expiry
 *  - attached claim ends when its fd closes and when its holder dies
 *  - JSON snapshot lists claims; poll() wakes on a change
 *  - STOP ends the claim and its holder (SIGTERM); EPERM for another uid
 *  - WAIT sleeps until free; CHECK
 *  - reboot(2) from a non-holder fails EBUSY while another holds "reboot" (the
 *    caller is uid 1000, so if the gate ever failed it gets EPERM, never a reboot) */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdint.h>
#include <grp.h>
#define GID_CLAIM 950

#define CLAIM_F_SHARED 0x1
#define CLAIM_F_DETACH 0x2
#define CLAIM_F_WAIT   0x4
struct claim_req {
	char res[16]; char holder[32]; char purpose[80];
	uint32_t secs, flags; int32_t stop_pid; uint32_t pad; uint64_t id;
	char busy_holder[32]; char busy_purpose[80]; int64_t busy_until; uint64_t busy_id;
};
#define TAKE 0xC1A0
#define RELEASE 0xC1A1
#define EXTEND 0xC1A2
#define STOP 0xC1A3
#define CHECK 0xC1A4

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

static int take(int fd, const char *res, const char *holder, const char *purpose, unsigned secs,
                unsigned flags, struct claim_req *out)
{
	struct claim_req q; memset(&q, 0, sizeof q);
	snprintf(q.res, sizeof q.res, "%s", res);
	snprintf(q.holder, sizeof q.holder, "%s", holder);
	snprintf(q.purpose, sizeof q.purpose, "%s", purpose);
	q.secs = secs; q.flags = flags;
	int r = ioctl(fd, TAKE, &q);
	if (out) *out = q;
	return r < 0 ? -errno : 0;
}

static int by_id(int fd, unsigned cmd, uint64_t id, unsigned secs)
{
	struct claim_req q; memset(&q, 0, sizeof q); q.id = id; q.secs = secs;
	return ioctl(fd, cmd, &q) < 0 ? -errno : 0;
}

static char *snap(void)
{
	static char b[32768];
	int fd = open("/dev/claim", O_RDONLY);
	int n = 0, r;
	while ((r = (int)read(fd, b + n, sizeof b - 1 - n)) > 0) n += r;
	b[n] = 0; close(fd);
	return b;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int a = open("/dev/claim", O_RDWR), b = open("/dev/claim", O_RDWR);
	if (a < 0 || b < 0) FAIL("open /dev/claim: %s", strerror(errno));
	struct claim_req q;
	/* exclusive vs exclusive */
	if (take(a, "test.gpu", "Agent T", "kernel test", 60, 0, &q)) FAIL("take");
	uint64_t id1 = q.id;
	int r = take(b, "test.gpu", "Agent U", "other", 60, 0, &q);
	if (r != -EBUSY) FAIL("second exclusive: %d", r);
	if (strcmp(q.busy_holder, "Agent T") || strcmp(q.busy_purpose, "kernel test") || q.busy_id != id1 || q.busy_until <= 0)
		FAIL("busy info: %s / %s", q.busy_holder, q.busy_purpose);
	printf("ok exclusive: busy -> %s (%s)\n", q.busy_holder, q.busy_purpose);
	if (take(a, "test.gpu", "Agent T", "kernel test, longer", 120, 0, &q) || q.id != id1) FAIL("re-take by same holder");
	if (take(b, "test.gpu", "Agent U", "x", 0, CLAIM_F_SHARED, NULL) != -EBUSY) FAIL("shared vs exclusive");
	if (take(a, "test.disp", "Agent T", "s1", 30, CLAIM_F_SHARED, NULL) || take(b, "test.disp", "Agent U", "s2", 30, CLAIM_F_SHARED, NULL))
		FAIL("shared + shared");
	if (take(a, "Bad Name", "x", "x", 5, 0, NULL) != -EINVAL) FAIL("bad name accepted");
	if (take(a, "test.det", "x", "x", 0, CLAIM_F_DETACH, NULL) != -EINVAL) FAIL("detached without expiry");
	printf("ok shared, names, re-take\n");
	/* JSON */
	char *j = snap();
	if (!strstr(j, "\"claims\":[") || !strstr(j, "\"holder\":\"Agent T\"") || !strstr(j, "\"purpose\":\"kernel test, longer\""))
		FAIL("json: %.300s", j);
	/* poll wakes on change */
	int w = open("/dev/claim", O_RDONLY);
	char buf[8192]; while (read(w, buf, sizeof buf) > 0) ;
	struct pollfd p = { w, POLLIN, 0 };
	if (poll(&p, 1, 0) != 0) FAIL("poll readable with no change");
	pid_t ch = fork();
	if (ch == 0) { usleep(200000); int c = open("/dev/claim", O_RDWR); take(c, "test.poll", "P", "p", 5, 0, NULL); close(c); _exit(0); }
	struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
	if (poll(&p, 1, 3000) != 1) FAIL("poll did not wake");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	waitpid(ch, NULL, 0);
	int nr = (int)read(w, buf, sizeof buf - 1);
	if (nr <= 0) FAIL("no fresh snapshot after poll");
	printf("ok poll woke in %ld ms\n", (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
	close(w);
	/* the claim's clock is wall time: left_s drops by ~2 over 2 s */
	{
		char *q1 = strstr(snap(), "\"left_s\":");
		long l1 = q1 ? strtol(q1 + 9, NULL, 10) : -1;
		sleep(2);
		char *q2 = strstr(snap(), "\"left_s\":");
		long l2 = q2 ? strtol(q2 + 9, NULL, 10) : -1;
		if (l1 < 0 || l2 < 0 || l1 - l2 < 1 || l1 - l2 > 3) FAIL("claim clock: left_s %ld -> %ld over 2 s", l1, l2);
		printf("ok claim clock (left_s %ld -> %ld over 2 s)\n", l1, l2);
	}
	/* close releases an attached claim */
	close(a);
	if (take(b, "test.gpu", "Agent U", "after close", 30, 0, &q)) FAIL("attached claim survived close");
	if (by_id(b, RELEASE, q.id, 0)) FAIL("release");
	/* holder death releases */
	ch = fork();
	if (ch == 0) { int c = open("/dev/claim", O_RDWR); take(c, "test.dead", "D", "dies", 0, 0, NULL); for (;;) pause(); }
	usleep(150000);
	if (take(b, "test.dead", "E", "x", 5, 0, NULL) != -EBUSY) FAIL("child's claim missing");
	kill(ch, SIGKILL); waitpid(ch, NULL, 0);
	if (take(b, "test.dead", "E", "x", 5, 0, &q)) FAIL("dead holder's claim survived");
	by_id(b, RELEASE, q.id, 0);
	printf("ok close and death release\n");
	/* detached + expiry */
	int c = open("/dev/claim", O_RDWR);
	if (take(c, "test.exp", "X", "short", 2, CLAIM_F_DETACH, &q)) FAIL("detached take");
	close(c);
	if (take(b, "test.exp", "Y", "x", 5, 0, NULL) != -EBUSY) FAIL("detached did not survive close");
	sleep(3);
	if (take(b, "test.exp", "Y", "x", 5, 0, &q)) FAIL("expired claim still there");
	by_id(b, RELEASE, q.id, 0);
	printf("ok detached + expiry\n");
	/* WAIT */
	c = open("/dev/claim", O_RDWR);
	take(c, "test.wait", "W1", "brief", 1, CLAIM_F_DETACH, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (take(b, "test.wait", "W2", "waiter", 5, CLAIM_F_WAIT, &q)) FAIL("wait take");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	by_id(b, RELEASE, q.id, 0); close(c);
	printf("ok wait (%ld ms)\n", (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
	/* STOP: claim gone, holder's process group gets SIGTERM */
	ch = fork();
	if (ch == 0) {
		setpgid(0, 0);
		int cc = open("/dev/claim", O_RDWR);
		take(cc, "test.stop", "Agent S", "long job", 600, CLAIM_F_DETACH, NULL);
		for (;;) pause();
	}
	usleep(200000);
	if (take(b, "test.stop", "Human", "x", 5, 0, &q) != -EBUSY) FAIL("stop target missing");
	uint64_t sid = q.busy_id;
	/* another uid may not stop it */
	pid_t o = fork();
	if (o == 0) {
		gid_t g = GID_CLAIM;
		setgroups(1, &g); setgid(1000); setuid(1000);
		int oc = open("/dev/claim", O_RDWR);
		_exit(oc >= 0 && by_id(oc, STOP, sid, 0) == -EPERM ? 0 : 1);
	}
	int st; waitpid(o, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("uid 1000 could stop root's claim");
	if (by_id(b, STOP, sid, 0)) FAIL("stop");
	clock_gettime(CLOCK_MONOTONIC, &t0);
	waitpid(ch, &st, 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) FAIL("holder not stopped by SIGTERM");
	if (take(b, "test.stop", "Human", "x", 5, 0, &q)) FAIL("stopped claim still there");
	by_id(b, RELEASE, q.id, 0);
	printf("ok stop (holder ended in %ld ms), EPERM for another uid\n",
	       (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
	/* EXTEND */
	if (take(b, "test.ext", "E", "x", 2, 0, &q) || by_id(b, EXTEND, q.id, 60)) FAIL("extend");
	sleep(3);
	if (take(open("/dev/claim", O_RDWR), "test.ext", "F", "x", 5, 0, NULL) != -EBUSY) FAIL("extend did not hold");
	by_id(b, RELEASE, q.id, 0);
	/* CHECK */
	struct claim_req k; memset(&k, 0, sizeof k);
	take(b, "test.chk", "Agent C", "gpu test", 30, 0, &q);
	snprintf(k.res, sizeof k.res, "test.chk"); snprintf(k.holder, sizeof k.holder, "Agent C");
	if (ioctl(b, CHECK, &k)) FAIL("check by holder");
	snprintf(k.holder, sizeof k.holder, "Agent D");
	if (ioctl(b, CHECK, &k) == 0 || errno != EBUSY || strcmp(k.busy_holder, "Agent C")) FAIL("check by other");
	by_id(b, RELEASE, q.id, 0);
	printf("ok extend, check\n");
	/* reboot gate: a sibling holds "reboot"; we are not its descendant */
	ch = fork();
	if (ch == 0) {
		int cc = open("/dev/claim", O_RDWR);
		if (take(cc, "reboot", "Agent R", "reboot gate test", 30, 0, NULL)) _exit(2);
		for (;;) pause();
	}
	usleep(200000);
	if (strstr(snap(), "\"res\":\"reboot\"") == NULL) { kill(ch, SIGKILL); waitpid(ch, NULL, 0); FAIL("reboot claim not visible"); }
	o = fork();
	if (o == 0) {
		setgid(1000); setuid(1000);
		long rr = syscall(SYS_reboot, 0xfee1dead, 0x28121969, 0x01234567, 0);
		_exit(rr < 0 && errno == EBUSY ? 0 : rr < 0 && errno == EPERM ? 3 : 4);
	}
	waitpid(o, &st, 0);
	int gate = WIFEXITED(st) ? WEXITSTATUS(st) : 9;
	kill(ch, SIGKILL); waitpid(ch, NULL, 0);
	if (gate != 0) FAIL("reboot gate: child exit %d (3 = got EPERM, gate not hit)", gate);
	o = fork();
	if (o == 0) {
		setgid(1000); setuid(1000);
		long rr = syscall(SYS_reboot, 0xfee1dead, 0x28121969, 0x01234567, 0);
		_exit(rr < 0 && errno == EPERM ? 0 : 5);
	}
	waitpid(o, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("after release, unprivileged reboot should be EPERM");
	printf("ok reboot gate (EBUSY while held, EPERM after)\n");
	/* closed wall: no group claim, no /dev/claim */
	o = fork();
	if (o == 0) {
		setgroups(0, NULL); setgid(1000); setuid(1000);
		_exit(open("/dev/claim", O_RDWR) < 0 && errno == EACCES ? 0 : 1);
	}
	waitpid(o, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("a user outside group claim opened /dev/claim");
	/* stop_pid: a member may not name a process it could not signal (root's) */
	ch = fork();
	if (ch == 0) { for (;;) pause(); }                 /* root's */
	o = fork();
	if (o == 0) {
		gid_t g = GID_CLAIM;
		setgroups(1, &g); setgid(1000); setuid(1000);
		int oc = open("/dev/claim", O_RDWR);
		struct claim_req x; memset(&x, 0, sizeof x);
		snprintf(x.res, sizeof x.res, "test.kill"); snprintf(x.holder, sizeof x.holder, "M");
		x.stop_pid = ch;
		int rr = ioctl(oc, TAKE, &x) < 0 ? errno : 0;
		_exit(oc >= 0 && rr == EPERM ? 0 : 10 + rr);
	}
	waitpid(o, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("member named root's pid as stop_pid: exit %d", WEXITSTATUS(st));
	if (kill(ch, 0) != 0) FAIL("root's process died");
	/* a reused pid is not the process taken: the target exits, STOP signals nobody */
	pid_t victim = fork();
	if (victim == 0) { for (;;) pause(); }
	struct claim_req x; memset(&x, 0, sizeof x);
	snprintf(x.res, sizeof x.res, "test.gone"); snprintf(x.holder, sizeof x.holder, "G");
	x.stop_pid = victim; x.secs = 30; x.flags = CLAIM_F_DETACH;
	if (ioctl(b, TAKE, &x)) FAIL("take with own child as stop_pid: %s", strerror(errno));
	kill(victim, SIGKILL); waitpid(victim, NULL, 0);
	if (by_id(b, STOP, x.id, 0)) FAIL("stop after target gone");
	if (kill(ch, 0) != 0) FAIL("bystander hit");
	kill(ch, SIGKILL); waitpid(ch, NULL, 0);
	/* a stop_pid that names no process is refused */
	memset(&x, 0, sizeof x);
	snprintf(x.res, sizeof x.res, "test.srch"); snprintf(x.holder, sizeof x.holder, "S");
	x.stop_pid = 999999;
	if (ioctl(b, TAKE, &x) == 0 || errno != ESRCH) FAIL("stop_pid of no process accepted");
	printf("ok closed wall: group claim, stop_pid checked, pid reuse safe\n");
	close(b);
	if (strstr(snap(), "\"res\":\"test.")) FAIL("test claims left behind: %.300s", snap());
	printf("OK\n");
	return 0;
}
