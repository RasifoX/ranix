/* A person's session (D-04): the desktop starts as root, takes on uid 1000 with
 * the video, input and audio groups, and from then on:
 *  - its ids and groups are what it set, children inherit them, and there is no
 *    way back to root (setuid 0, setgroups both refused);
 *  - the screen, GPU, input and sound nodes are root:<group> 0660, so the
 *    member opens them and a user outside the groups (nobody) gets EACCES;
 *  - rawkbd (fake key injection) stays root only, for members too;
 *  - the volume knob in /sys/kernel opens for writing to an audio member only;
 *  - nobody cannot open the console (tty0) or resize the hugepage pool.
 * Prints OK, or FAIL with the first thing that went wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define GID_AUDIO 18
#define GID_INPUT 23
#define GID_VIDEO 27

static const struct { const char *path; gid_t gid; } NODES[] = {
	{ "/dev/fb0", GID_VIDEO }, { "/dev/dri/card0", GID_VIDEO }, { "/dev/dri/renderD128", GID_VIDEO },
	{ "/dev/input/event0", GID_INPUT }, { "/dev/input/mice", GID_INPUT },
	{ "/dev/input/rawpad", GID_INPUT }, { "/dev/dsp", GID_AUDIO },
};
#define HUGE_NR "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
#define NNODES (int)(sizeof NODES / sizeof NODES[0])

/* child exit codes name the failure */
static const char *const WHY[] = {
	"ok", "setgroups", "setgid", "setuid", "ids after drop", "groups after drop",
	"setuid 0 not refused", "setgroups not refused", "member open refused",
	"rawkbd opened by a member", "nobody opened a node", "volume not writable by member",
	"volume writable by nobody", "child lost groups", "status Groups line",
	"member cannot open its own pty slave", "another user opened the slave",
	"nobody opened the console (tty0)", "nobody resized the hugepage pool",
};

static int as_member(void)
{
	gid_t want[3] = { GID_VIDEO, GID_INPUT, GID_AUDIO };
	if (setgroups(3, want) != 0) return 1;
	if (setgid(1000) != 0) return 2;
	if (setuid(1000) != 0) return 3;
	if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000 || getegid() != 1000) return 4;
	gid_t g[8];
	int n = getgroups(8, g);
	if (n != 3 || g[0] != GID_VIDEO || g[1] != GID_INPUT || g[2] != GID_AUDIO) return 5;
	if (setuid(0) == 0 || errno != EPERM) return 6;
	if (setgroups(0, NULL) == 0) return 7;
	char line[256];
	FILE *st = fopen("/proc/self/status", "r");
	int seen = 0;
	while (st && fgets(line, sizeof line, st))
		if (!strncmp(line, "Groups:", 7) && strstr(line, "27") && strstr(line, "23") && strstr(line, "18")) seen = 1;
	if (st) fclose(st);
	if (!seen) return 14;
	for (int i = 0; i < NNODES; i++) {
		struct stat s;
		if (stat(NODES[i].path, &s) != 0) continue;   /* not on this machine */
		int fd = open(NODES[i].path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		/* the driver may say busy or no device; only EACCES is the gate */
		if (fd < 0 && errno == EACCES) { fprintf(stderr, "member: %s\n", NODES[i].path); return 8; }
		if (fd >= 0) close(fd);
	}
	if (access("/dev/input/rawkbd", F_OK) == 0) {
		int fd = open("/dev/input/rawkbd", O_WRONLY | O_CLOEXEC);
		if (fd >= 0 || errno != EACCES) return 9;
	}
	if (access("/sys/kernel/volume", F_OK) == 0) {
		int fd = open("/sys/kernel/volume", O_WRONLY | O_CLOEXEC);
		if (fd < 0) return 11;
		close(fd);
	}
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) return 15;
	char *sn = ptsname(m);
	int sl = sn ? open(sn, O_RDWR | O_NOCTTY) : -1;
	if (sl < 0) return 15;
	close(sl);
	close(m);
	pid_t c = fork();
	if (c == 0) {
		gid_t h[8];
		_exit(getgroups(8, h) == 3 && getuid() == 1000 ? 0 : 1);
	}
	int w;
	if (c < 0 || waitpid(c, &w, 0) != c || !WIFEXITED(w) || WEXITSTATUS(w)) return 13;
	return 0;
}

static char g_root_pts[64];   /* a slave root opened: nobody must not get in */

static int as_nobody(void)
{
	if (setgroups(0, NULL) != 0) return 1;
	if (setgid(65534) != 0) return 2;
	if (setuid(65534) != 0) return 3;
	for (int i = 0; i < NNODES; i++) {
		struct stat s;
		if (stat(NODES[i].path, &s) != 0) continue;
		int fd = open(NODES[i].path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0 || errno != EACCES) { fprintf(stderr, "nobody: %s\n", NODES[i].path); return 10; }
	}
	if (access("/sys/kernel/volume", F_OK) == 0) {
		int fd = open("/sys/kernel/volume", O_WRONLY | O_CLOEXEC);
		if (fd >= 0) return 12;
	}
	if (g_root_pts[0]) {
		int fd = open(g_root_pts, O_RDWR | O_NOCTTY);
		if (fd >= 0 || errno != EACCES) return 16;
	}
	int fd = open("/dev/tty0", O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0 || errno != EACCES) return 17;
	fd = open(HUGE_NR, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {   /* the mode bits must refuse; if not, the write must */
		int wrote = write(fd, "0\n", 2) == 2;
		close(fd);
		if (wrote) return 18;
	}
	return 0;
}

static int run(int (*fn)(void), const char *who)
{
	pid_t p = fork();
	if (p == 0) _exit(fn());
	int st;
	if (p < 0 || waitpid(p, &st, 0) != p) { printf("FAIL fork %s\n", who); return 1; }
	int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
	if (code) {
		printf("FAIL %s: %s\n", who, code < (int)(sizeof WHY / sizeof WHY[0]) ? WHY[code] : "crashed");
		return 1;
	}
	return 0;
}

int main(void)
{
	/* the nodes themselves: root-owned, 0660, the right group */
	for (int i = 0; i < NNODES; i++) {
		struct stat s;
		if (stat(NODES[i].path, &s) != 0) continue;
		if ((s.st_mode & 07777) != 0660 || s.st_uid != 0 || s.st_gid != NODES[i].gid) {
			printf("FAIL %s mode %o owner %d:%d\n", NODES[i].path, s.st_mode & 07777, (int)s.st_uid, (int)s.st_gid);
			return 0;
		}
	}
	struct stat k;
	if (stat("/dev/input/rawkbd", &k) == 0 && (k.st_mode & 0077)) {
		printf("FAIL rawkbd mode %o\n", k.st_mode & 07777);
		return 0;
	}
	if (stat("/sys/kernel/volume", &k) == 0 && ((k.st_mode & 0777) != 0664 || k.st_gid != GID_AUDIO)) {
		printf("FAIL volume mode %o gid %d\n", k.st_mode & 0777, (int)k.st_gid);
		return 0;
	}
	/* tmpfs honours mode=: /tmp is 1777, a plain mount stays closed (0755) */
	char dir[] = "/tmp/ucmntXXXXXX", in[40];
	if (mkdtemp(dir)) {
		struct stat t;
		snprintf(in, sizeof in, "%s/.", dir);   /* the mounted root, not the dir under it */
		int bad = 0;
		if (mount("tmpfs", dir, "tmpfs", 0, "mode=1777") == 0) {
			bad = stat(in, &t) != 0 || (t.st_mode & 07777) != 01777;
			umount(dir);
		}
		if (!bad && mount("tmpfs", dir, "tmpfs", 0, NULL) == 0) {
			bad = stat(in, &t) != 0 || (t.st_mode & 07777) != 0755;
			umount(dir);
		}
		rmdir(dir);
		if (bad) { printf("FAIL tmpfs mode %o\n", t.st_mode & 07777); return 0; }
	}
	int rm = posix_openpt(O_RDWR | O_NOCTTY);
	if (rm >= 0 && grantpt(rm) == 0 && unlockpt(rm) == 0 && ptsname(rm))
		snprintf(g_root_pts, sizeof g_root_pts, "%s", ptsname(rm));
	int r = run(as_member, "member") || run(as_nobody, "nobody");
	if (rm >= 0) close(rm);
	if (r) return 0;
	puts("OK");
	return 0;
}
