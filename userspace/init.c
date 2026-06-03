#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <string.h>

#define TIOCSPGRP 0x5410

/* switch_root: if the auto-mounted disk at /mnt carries a full rootfs and we
 * have not pivoted yet, make the disk the real root (pivot_root) and re-exec
 * init from it. The kernel mounts /dev /proc /sys on the old (ramfs) root, so
 * re-mount the ones the system needs onto the new root after pivoting.
 * Guard: after pivot the old root is at /oldroot, so /oldroot/bin/init exists —
 * that stops the re-exec'd init from pivoting again. */
static void maybe_switch_root(void)
{
	if (access("/oldroot/bin/init", F_OK) == 0) return;   /* already pivoted */
	if (access("/mnt/bin/init", F_OK) != 0)     return;   /* no disk rootfs */

	if (syscall(SYS_pivot_root, "/mnt", "/mnt/oldroot") != 0) return;
	chdir("/");
	mount("none", "/dev",  "devtmpfs", 0, NULL);
	mount("none", "/proc", "proc",     0, NULL);

	char *av[] = { "/bin/init", NULL };
	char *ev[] = { "PATH=/bin", "HOME=/root", "PWD=/", "TERM=vt100", NULL };
	execve("/bin/init", av, ev);
	/* exec failed: we are already on the disk root — fall through and run it */
}

/* Mount everything in /etc/fstab, as a Linux init/rc would. busybox mount -a
 * parses fstab and skips noauto / already-mounted entries. */
static void mount_all(void)
{
	if (access("/etc/fstab", F_OK) != 0) return;
	pid_t pid = fork();
	if (pid == 0) {
		char *av[] = { "/bin/mount", "-a", NULL };
		char *ev[] = { "PATH=/bin", NULL };
		execve("/bin/mount", av, ev);
		_exit(127);
	}
	if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

static void do_poweroff(void)
{
    sync();
    reboot(RB_POWER_OFF);
    _exit(0);
}

int main(void)
{
    maybe_switch_root();   /* may not return (re-execs from the disk root) */

    close(0); close(1); close(2);
    int tfd = open("/dev/console", O_RDWR);
    if (tfd != 0) { dup2(tfd, 0); close(tfd); }
    dup2(0, 1);
    dup2(0, 2);

    mount_all();   /* process /etc/fstab */

    /* start echo_server in background */
    {
        pid_t spid = fork();
        if (spid == 0) {
            setsid();
            char *argv[] = { "/bin/echo_server", NULL };
            char *envp[] = { NULL };
            execve("/bin/echo_server", argv, envp);
            _exit(1);
        }
    }

    char *envp[] = { "PATH=/bin:/mnt/usr/bin:/mnt/bin", "HOME=/root", "PWD=/", "TERM=vt100", NULL };

    /* if /test.sh exists, run it once and poweroff */
    {
        int fd = open("/test.sh", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int t = open("/dev/console", O_RDWR);
                if (t >= 0) { dup2(t, 0); dup2(t, 1); dup2(t, 2); if (t > 2) close(t); }
                char *argv[] = { "/bin/bash", "--norc", "--noprofile", "/test.sh", NULL };
                execve("/bin/bash", argv, envp);
                _exit(127);
            }
            if (pid > 0) { int st; waitpid(pid, &st, 0); }
            do_poweroff();
        }
    }

    while (1) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int t = open("/dev/console", O_RDWR);
            if (t >= 0) {
                dup2(t, 0); dup2(t, 1); dup2(t, 2);
                if (t > 2) close(t);
            }
            pid_t mypid = getpid();
            ioctl(0, TIOCSPGRP, &mypid);

            char *bash_argv[] = { "/bin/bash", "--norc", "--noprofile", NULL };
            execve("/bin/bash", bash_argv, envp);
            char *sh_argv[] = { "/bin/sh", NULL };
            execve("/bin/sh", sh_argv, envp);
            _exit(127);
        }
        if (pid < 0) { sleep(1); continue; }
        int status;
        waitpid(pid, &status, 0);
    }
}
