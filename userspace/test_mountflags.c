/* per-mount flag enforcement (MS_RDONLY/NOEXEC/NOSUID/NODEV). Runs as root so it
 * can mount(2); each case mounts a fresh tmpfs and asserts Linux semantics. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	int in = open(src, O_RDONLY);
	if (in < 0) return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) { close(in); return -1; }
	char buf[4096]; ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) { close(in); close(out); return -1; }
	}
	close(in); close(out);
	return n < 0 ? -1 : 0;
}

static int helper_euid(const char *path)
{
	int pfd[2];
	if (pipe(pfd)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		dup2(pfd[1], 1);
		close(pfd[0]); close(pfd[1]);
		setgid(65534); setuid(65534);
		execl(path, "helper", (char *)0);
		_exit(127);
	}
	close(pfd[1]);
	char buf[64]; int n = (int)read(pfd[0], buf, sizeof(buf) - 1);
	close(pfd[0]);
	waitpid(pid, NULL, 0);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *eq = strchr(buf, '=');
	return eq ? atoi(eq + 1) : -1;
}

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); return 1; } while (0)

int main(void)
{
	if (geteuid() != 0) FAIL("not-root\n");

	/* ── MS_RDONLY ──────────────────────────────────────── */
	/* a fresh rdonly tmpfs starts empty, so seed via a rw mount then remount
	 * rdonly in place (MS_REMOUNT) to keep the seeded file readable. */
	mkdir("/tmp/mf_ro", 0755);
	if (mount("tmpfs", "/tmp/mf_ro", "tmpfs", 0, NULL) != 0) FAIL("mount-ro-rw errno=%d\n", errno);
	{
		int fd = open("/tmp/mf_ro/seed", O_WRONLY | O_CREAT, 0644);
		if (fd < 0) FAIL("ro-seed errno=%d\n", errno);
		write(fd, "data", 4); close(fd);
	}
	if (mount("tmpfs", "/tmp/mf_ro", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL) != 0)
		FAIL("remount-ro errno=%d\n", errno);

	int fd = open("/tmp/mf_ro/seed", O_RDONLY);
	if (fd < 0) FAIL("ro-read errno=%d\n", errno);
	close(fd);

	if (open("/tmp/mf_ro/new", O_WRONLY | O_CREAT, 0644) >= 0 || errno != EROFS)
		FAIL("ro-create errno=%d\n", errno);
	if (open("/tmp/mf_ro/seed", O_WRONLY) >= 0 || errno != EROFS)
		FAIL("ro-open-wr errno=%d\n", errno);
	if (unlink("/tmp/mf_ro/seed") == 0 || errno != EROFS)
		FAIL("ro-unlink errno=%d\n", errno);
	if (mkdir("/tmp/mf_ro/d", 0755) == 0 || errno != EROFS)
		FAIL("ro-mkdir errno=%d\n", errno);
	if (chmod("/tmp/mf_ro/seed", 0600) == 0 || errno != EROFS)
		FAIL("ro-chmod errno=%d\n", errno);
	if (symlink("x", "/tmp/mf_ro/l") == 0 || errno != EROFS)
		FAIL("ro-symlink errno=%d\n", errno);
	mount("tmpfs", "/tmp/mf_ro", "tmpfs", MS_REMOUNT, NULL); /* clear rdonly so umount can clean */
	umount("/tmp/mf_ro");

	/* ── MS_NOEXEC ──────────────────────────────────────── */
	mkdir("/tmp/mf_noexec", 0755);
	if (mount("tmpfs", "/tmp/mf_noexec", "tmpfs", MS_NOEXEC, NULL) != 0)
		FAIL("mount-noexec errno=%d\n", errno);
	if (copy_file("/bin/test_suidhelper", "/tmp/mf_noexec/h", 0755) != 0)
		FAIL("noexec-copy errno=%d\n", errno);
	pid_t pid = fork();
	if (pid == 0) { execl("/tmp/mf_noexec/h", "h", (char *)0); _exit(errno); }
	int st = 0; waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != EACCES)
		FAIL("noexec-exec code=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	umount("/tmp/mf_noexec");

	/* ── MS_NOSUID ──────────────────────────────────────── */
	mkdir("/tmp/mf_nosuid", 0755);
	if (mount("tmpfs", "/tmp/mf_nosuid", "tmpfs", MS_NOSUID, NULL) != 0)
		FAIL("mount-nosuid errno=%d\n", errno);
	if (copy_file("/bin/test_suidhelper", "/tmp/mf_nosuid/h", 0755) != 0)
		FAIL("nosuid-copy errno=%d\n", errno);
	if (chown("/tmp/mf_nosuid/h", 0, 0) != 0) FAIL("nosuid-chown errno=%d\n", errno);
	if (chmod("/tmp/mf_nosuid/h", 04755) != 0) FAIL("nosuid-chmod errno=%d\n", errno);
	int e = helper_euid("/tmp/mf_nosuid/h");
	if (e != 65534) FAIL("nosuid-elevated euid=%d\n", e);
	umount("/tmp/mf_nosuid");

	/* a plain (suid-honouring) mount must still elevate — confirms the helper
	 * itself is genuinely setuid-root and the nosuid result wasn't a fluke. */
	mkdir("/tmp/mf_suid", 0755);
	if (mount("tmpfs", "/tmp/mf_suid", "tmpfs", 0, NULL) != 0) FAIL("mount-suid errno=%d\n", errno);
	if (copy_file("/bin/test_suidhelper", "/tmp/mf_suid/h", 0755) != 0)
		FAIL("suid-copy errno=%d\n", errno);
	chown("/tmp/mf_suid/h", 0, 0);
	chmod("/tmp/mf_suid/h", 04755);
	e = helper_euid("/tmp/mf_suid/h");
	if (e != 0) FAIL("suid-not-elevated euid=%d\n", e);
	umount("/tmp/mf_suid");

	/* ── MS_NODEV ───────────────────────────────────────── */
	mkdir("/tmp/mf_nodev", 0755);
	if (mount("tmpfs", "/tmp/mf_nodev", "tmpfs", MS_NODEV, NULL) != 0)
		FAIL("mount-nodev errno=%d\n", errno);
	if (mknod("/tmp/mf_nodev/null", S_IFCHR | 0666, makedev(1, 3)) != 0)
		FAIL("nodev-mknod errno=%d\n", errno);
	if (open("/tmp/mf_nodev/null", O_RDWR) >= 0 || errno != EACCES)
		FAIL("nodev-open errno=%d\n", errno);
	umount("/tmp/mf_nodev");

	puts("OK");
	return 0;
}
