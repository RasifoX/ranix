/* The syscall gate: default deny, argument checks at the right place, and no way
 * to change the answer from inside. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include "asess_test.h"

#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define UNIMPLEMENTED_NR 1999

static const int ns_flags[] = {
	CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWNET, CLONE_NEWUTS, CLONE_NEWIPC,
	CLONE_NEWCGROUP,
};
#define NS_FLAGS_N ((int)(sizeof(ns_flags) / sizeof(ns_flags[0])))

static void all_denied(const char *when)
{
	char what[80];
	long ns_fd = open("/proc/self/ns/net", O_RDONLY);

	for (int i = 0; i < NS_FLAGS_N; i++) {
		snprintf(what, sizeof(what), "unshare %#x %s", ns_flags[i], when);
		DENIED(unshare(ns_flags[i]), EPERM, what);
	}
	snprintf(what, sizeof(what), "setns %s", when);
	DENIED(setns((int)ns_fd, 0), EPERM, what);
	close((int)ns_fd);
	DENIED(mount("none", "/tmp", "tmpfs", 0, NULL), EPERM, "mount");
	DENIED(mount("/", "/tmp", NULL, MS_BIND, NULL), EPERM, "bind mount");
	DENIED(umount2("/tmp", 0), EPERM, "umount");
	DENIED(chroot("/tmp"), EPERM, "chroot");
	DENIED(syscall(SYS_pivot_root, "/tmp", "/tmp"), EPERM, "pivot_root");
	DENIED(syscall(SYS_reboot, 0xfee1dead, 672274793, 0x1234567, 0), EPERM, "reboot");
	DENIED(sethostname("x", 1), EPERM, "sethostname");
	struct timespec ts = { 1, 0 };
	DENIED(clock_settime(CLOCK_REALTIME, &ts), EPERM, "clock_settime");
	struct timeval tv = { 1, 0 };
	DENIED(settimeofday(&tv, NULL), EPERM, "settimeofday");
	DENIED(syscall(SYS_syslog, 3, NULL, 0), EPERM, "syslog");
	uint32_t uring_params[30] = { 0 };
	DENIED(syscall(SYS_io_uring_setup, 4, uring_params), EPERM, "io_uring_setup");
	uint32_t hdr[2] = { 0x20080522, 0 }, d[6] = { 0 };
	DENIED(syscall(SYS_capset, hdr, d), EPERM, "capset");
	DENIED(inotify_init1(0), EPERM, "inotify");
	DENIED(setpriority(PRIO_PROCESS, 0, -10), EPERM, "setpriority");
	struct { int prio; } sp = { 50 };
	DENIED(syscall(SYS_sched_setscheduler, 0, 1, &sp), EPERM, "SCHED_FIFO");
}

static volatile int flip_stop;

static void *flipper(void *arg)
{
	volatile uint64_t *flags = arg;
	while (!flip_stop) { *flags = 0; *flags = CLONE_NEWUSER; }
	return NULL;
}

static void clone_flags(void)
{
	for (int i = 0; i < NS_FLAGS_N; i++) {
		DENIED(syscall(SYS_clone, ns_flags[i] | SIGCHLD, 0, 0, 0, 0), EPERM, "clone with CLONE_NEW*");
		struct { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; } ca;
		memset(&ca, 0, sizeof(ca));
		ca.flags = (uint64_t)ns_flags[i];
		ca.exit_signal = SIGCHLD;
		DENIED(syscall(SYS_clone3, &ca, sizeof(ca)), EPERM, "clone3 with CLONE_NEW*");
	}

	/* clone3 reads its flags from user memory, which a sibling thread can flip.
	 * The check runs on the kernel's copy, so no child may appear in a new user
	 * namespace, however the race falls. */
	static volatile struct {
		uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
	} ca;
	ca.exit_signal = SIGCHLD;
	pthread_t t;
	CHECK(pthread_create(&t, NULL, flipper, (void *)&ca.flags) == 0, "start the flipper");
	for (int i = 0; i < 200; i++) {
		long p = syscall(SYS_clone3, (void *)&ca, sizeof(ca));
		if (p == 0) _exit(asess_self() && geteuid() == UID ? 0 : 1);
		if (p > 0) {
			int st = 1;
			waitpid((pid_t)p, &st, 0);
			CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clone3 race child confined");
		}
	}
	flip_stop = 1;
	pthread_join(t, NULL);
}

static void args_checked(void)
{
	DENIED(socket(AF_INET, SOCK_RAW, 1), EPERM, "raw IP socket");
	DENIED(socket(AF_INET6, SOCK_RAW, 58), EPERM, "raw IPv6 socket");
	DENIED(socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, 1), EPERM, "raw socket with SOCK_CLOEXEC");
	int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, 0);
	CHECK(nl >= 0, "netlink stays usable");
	if (nl >= 0) close(nl);
	DENIED(mknod("/tmp/blk", S_IFBLK | 0600, 0x0800), EPERM, "mknod block");
	DENIED(mknod("/tmp/chr", S_IFCHR | 0600, 0x0103), EPERM, "mknod char");
	DENIED(mknodat(AT_FDCWD, "/tmp/chr2", S_IFCHR | 0600, 0x0103), EPERM, "mknodat char");
	CHECK(mkfifo("/tmp/fifo", 0600) == 0, "FIFO allowed");
}

static void seccomp_cannot_widen(void)
{
	/* ALLOW everything, then try again */
	struct sock_filter f[] = { { 0x06, 0, 0, 0x7fff0000u } };
	struct sock_fprog p = { 1, f };
	CHECK(prctl(PR_SET_SECCOMP, 2, &p) == 0, "own seccomp filter installs");
	all_denied("under an allow-all filter");
	DENIED(prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0), EINVAL, "clear no_new_privs");
}

static void denied_is_recorded(void)
{
	DENIED(syscall(UNIMPLEMENTED_NR), EPERM, "number outside the table");
	DENIED(syscall(-1L), EPERM, "negative number");
	DENIED(syscall(ASESS_SYSCALL + 1), EPERM, "number past the table");

	char buf[2048], want[64];
	int fd = open("/proc/self/asess/denied", O_RDONLY);
	CHECK(fd >= 0, "open denied");
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	CHECK(n > 0, "read denied");
	buf[n > 0 ? n : 0] = '\0';
	snprintf(want, sizeof(want), "rule=gate syscall=%d pid=1\n", UNIMPLEMENTED_NR);
	CHECK(strstr(buf, want) != NULL, "denied names the syscall");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int keep[] = { 1 };
	long sfd = sess_create("/", UID, keep, 1, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		unsigned long long id = asess_self();
		all_denied("directly");
		clone_flags();
		args_checked();
		seccomp_cannot_widen();
		denied_is_recorded();
		CHECK(asess_self() == id && id, "still the same session");
		CHECK(geteuid() == UID, "still unprivileged");
		_exit(fails ? 1 : 0);
	}
	sess_reap((pid_t)pid);
	return finish();
}
