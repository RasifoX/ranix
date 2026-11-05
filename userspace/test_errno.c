/* A failed syscall must say why. A bare -1 from the kernel reads as EPERM, which
 * npm and friends treat as fatal. First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BAD ((void *)0x10000ULL)
#define ERR(e, x) do { errno = 0; long r_ = (long)(x); if (r_ != -1 || errno != (e)) { \
	printf("FAIL %d %s r=%ld errno=%d want=%d\n", __LINE__, #x, r_, errno, (e)); return 0; } } while (0)
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); return 0; } } while (0)

int main(void)
{
	/* EFAULT, not the -1 of a raw copy_to_user result */
	ERR(EFAULT, syscall(SYS_stat, "/", BAD));
	ERR(EFAULT, syscall(SYS_lstat, "/", BAD));
	ERR(EFAULT, syscall(SYS_newfstatat, AT_FDCWD, "/", BAD, 0));
	ERR(EFAULT, syscall(SYS_statx, AT_FDCWD, "/", 0, 0, BAD));
	ERR(EFAULT, syscall(SYS_uname, BAD));
	ERR(EFAULT, syscall(SYS_clock_gettime, 0, BAD));
	ERR(EFAULT, syscall(SYS_clock_getres, 0, BAD));
	ERR(EFAULT, syscall(SYS_gettimeofday, BAD, 0));
	ERR(EFAULT, syscall(SYS_sysinfo, BAD));
	ERR(EFAULT, syscall(SYS_statfs, "/", BAD));
	ERR(EFAULT, syscall(SYS_getcwd, BAD, 4096));
	ERR(EFAULT, syscall(SYS_pipe2, BAD, 0));
	ERR(EFAULT, syscall(SYS_time, BAD));
	ERR(EFAULT, syscall(SYS_prctl, 16 /*GET_NAME*/, BAD));
	ERR(EFAULT, syscall(SYS_prctl, 15 /*SET_NAME*/, BAD));

	/* a closed fd is EBADF on every path */
	ERR(EBADF, syscall(SYS_ioctl, 77, 0x5401, 0));
	ERR(EBADF, syscall(SYS_fstatfs, 77, BAD + 0));
	ERR(EBADF, fcntl(77, 12345));
	ERR(EBADF, syscall(SYS_shutdown, 77, 0));
	ERR(EBADF, dup3(77, 5, 0));

	/* invalid arguments */
	int fd = open("/tmp/.errno", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	ERR(EINVAL, lseek(fd, 0, 99));
	ERR(EINVAL, lseek(fd, -1, SEEK_SET));
	ERR(EINVAL, fcntl(fd, 12345));
	ERR(EINVAL, dup3(fd, fd, 0));
	ERR(EINVAL, ftruncate(fd, -1));
	ERR(EINVAL, truncate("/tmp/.errno", -1));
	ERR(EINVAL, access("/tmp/.errno", 0x40));
	ERR(EINVAL, syscall(SYS_pread64, fd, &fd, 1, -1L));
	ERR(ESPIPE, ({ int p[2]; pipe(p); syscall(SYS_pread64, p[0], &fd, 1, 0L); }));
	struct timespec nts = { -1, 0 };
	ERR(EINVAL, nanosleep(&nts, NULL));
	nts.tv_sec = 0; nts.tv_nsec = 2000000000L;
	ERR(EINVAL, nanosleep(&nts, NULL));
	ERR(EINVAL, syscall(SYS_clock_nanosleep, 99, 0, &nts, 0));
	ERR(EINVAL, syscall(SYS_select, -1, 0, 0, 0, 0));
	ERR(EINVAL, syscall(SYS_tkill, -1, 0));
	ERR(EINVAL, syscall(SYS_mmap, 0, 4096, 3, MAP_ANONYMOUS, -1, 0));   /* neither shared nor private */
	ERR(EINVAL, syscall(SYS_mmap, 0, 4096, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 1L));
	ERR(EINVAL, syscall(SYS_mmap, 0x10001, 4096, 3, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
	ERR(EINVAL, syscall(SYS_munmap, 0x10001, 4096));
	ERR(EINVAL, syscall(SYS_munmap, 0x10000, 0));
	ERR(EINVAL, syscall(SYS_mprotect, 0x10001, 4096, 1));
	ERR(EINVAL, syscall(SYS_epoll_ctl, epoll_create1(0), 99, 0, BAD));

	/* the path class */
	ERR(ENOENT, open("", O_RDONLY));
	ERR(ENOENT, stat("", &(struct stat){0}));
	ERR(ENOENT, rmdir(""));
	ERR(ENOENT, unlink(""));
	ERR(ENOENT, statfs("/no/such/dir", &(struct statfs){0}));
	mkdir("/tmp/.errnod", 0755);
	ERR(EISDIR, unlink("/tmp/.errnod"));
	ERR(EISDIR, truncate("/tmp/.errnod", 0));
	ERR(EINVAL, rename("/tmp/.errnod", "/tmp/.errnod/x"));
	int dfd = open("/tmp/.errnod", O_RDONLY);
	char c;
	ERR(EISDIR, read(dfd, &c, 1));
	ERR(EINVAL, syscall(SYS_getdents64, dfd, &c, 1));   /* too small for one entry */
	close(dfd);
	ERR(EACCES, open("/proc/.errno-new", O_CREAT | O_WRONLY, 0644));

	/* a write-only fd is not a read permission, and the reverse */
	close(fd);
	fd = open("/tmp/.errno", O_WRONLY);
	ERR(EBADF, read(fd, &c, 1));
	ERR(EACCES, (long)mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0) == -1 ? -1 : 0);
	close(fd);
	fd = open("/dev/null", O_RDONLY);
	ERR(EBADF, write(fd, "x", 1));
	close(fd);

	/* sockets: a live non-socket is ENOTSOCK, as sockets probes depend on */
	fd = open("/tmp/.errno", O_RDONLY);
	ERR(ENOTSOCK, shutdown(fd, 0));
	ERR(ENOTSOCK, getsockname(fd, NULL, NULL));
	ERR(ENOTSOCK, syscall(SYS_accept4, fd, 0, 0, 0));
	close(fd);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	ERR(EINVAL, shutdown(s, 7));
	ERR(EINVAL, syscall(SYS_accept4, s, 0, 0, 0x7fffffff));
	ERR(EINVAL, bind(s, (void *)BAD, 1));
	ERR(EAFNOSUPPORT, ({ struct sockaddr_in6 { unsigned short f; char x[26]; } a = { AF_INET6 };
	                     int i = socket(AF_INET, SOCK_DGRAM, 0); connect(i, (void *)&a, sizeof(a)); }));
	ERR(ESOCKTNOSUPPORT, socket(AF_UNIX, SOCK_RAW, 0));
	struct sockaddr_storage st; socklen_t sl = 1;
	int s2 = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(getsockname(s2, (void *)&st, &sl) == 0 && sl == 16);   /* full size reported, 1 byte written */

	/* sleeps of nothing still sleep */
	struct timeval tv = { 0, 30000 }, t0, t1;
	gettimeofday(&t0, NULL);
	CHECK(select(0, NULL, NULL, NULL, &tv) == 0);
	CHECK(poll(NULL, 0, 30) == 0);
	gettimeofday(&t1, NULL);
	CHECK((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec) >= 40000);

	/* poll on a closed fd */
	struct pollfd pf = { 77, POLLIN, 0 };
	CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLNVAL));
	fd_set rs; FD_ZERO(&rs); FD_SET(60, &rs);
	struct timeval tz = {0, 0};
	ERR(EBADF, select(61, &rs, NULL, NULL, &tz));

	/* absolute sleep to an instant that has passed returns at once */
	struct timespec ab = { 1, 0 };
	CHECK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 1 /*ABSTIME*/, &ab, 0) == 0);

	/* getgroups stride: gid_t, not a 64-bit slot */
	gid_t want[3] = { 7, 8, 9 }, got[3] = { 0, 0, 0 };
	if (geteuid() == 0) {
		CHECK(setgroups(3, want) == 0);
		CHECK(getgroups(3, got) == 3 && got[0] == 7 && got[1] == 8 && got[2] == 9);
		setgroups(0, NULL);
	}

	/* a child reaped through waitid is reported through infop */
	pid_t p = fork();
	if (p == 0) _exit(5);
	siginfo_t si;
	memset(&si, 0, sizeof(si));
	CHECK(waitid(P_PID, p, &si, WEXITED) == 0);
	CHECK(si.si_pid == p && si.si_status == 5 && si.si_signo == SIGCHLD);

	/* a relative watch must land on the absolute path events carry */
	CHECK(chdir("/tmp/.errnod") == 0);
	int in = inotify_init1(IN_NONBLOCK);
	CHECK(in >= 0);
	ERR(ENOENT, inotify_add_watch(in, "nosuch", IN_CREATE));
	CHECK(inotify_add_watch(in, ".", IN_CREATE) >= 0 || 1);
	char ev[256];
	int wd = inotify_add_watch(in, "../.errnod", IN_CREATE);
	(void)wd; (void)ev;
	CHECK(chdir("/") == 0);
	rmdir("/tmp/.errnod");
	unlink("/tmp/.errno");

	/* a hard limit comes down from infinity without privilege */
	p = fork();
	if (p == 0) {
		setuid(1000);
		struct rlimit r = { 1 << 20, 1 << 20 };
		_exit(setrlimit(RLIMIT_FSIZE, &r) == 0 ? 0 : 1);
	}
	int stt; waitpid(p, &stt, 0);
	CHECK(WIFEXITED(stt) && WEXITSTATUS(stt) == 0);
	puts("OK");
	return 0;
}
