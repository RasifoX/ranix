/* The thin calls: positioned vectored I/O, the older time setters, fd forms of the
 * xattr calls, mincore. Each is a library's first choice on some path. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sched.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utime.h>

#define WANT(c) do { if (!(c)) { printf("FAIL line %d errno=%d\n", __LINE__, errno); return 1; } } while (0)

int main(void)
{
	const char *p = "/tmp/compat_t";
	unlink(p);
	int fd = open(p, O_CREAT | O_RDWR, 0644);
	WANT(fd >= 0);
	char a[4] = "abcd", b[4] = "efgh", x[4], y[4];
	struct iovec w[2] = { {a, 4}, {b, 4} }, r[2] = { {x, 4}, {y, 4} };
	WANT(pwritev(fd, w, 2, 100) == 8);
	WANT(preadv(fd, r, 2, 100) == 8 && !memcmp(x, a, 4) && !memcmp(y, b, 4));
	WANT(lseek(fd, 0, SEEK_CUR) == 0);
	WANT(syscall(SYS_preadv2, fd, r, 2, 102L, 0L, 0) == 6 && !memcmp(x, "cdef", 4));
	WANT(lseek(fd, 100, SEEK_SET) == 100);
	WANT(syscall(SYS_preadv2, fd, r, 1, -1L, -1L, 0) == 4 && lseek(fd, 0, SEEK_CUR) == 104);

	struct timeval tv[2] = { {1000, 0}, {2000, 500000} };
	struct stat st;
	WANT(utimes(p, tv) == 0 && stat(p, &st) == 0 && st.st_mtime == 2000 && st.st_mtim.tv_nsec == 500000000);
	struct utimbuf ub = { 3000, 4000 };
	WANT(utime(p, &ub) == 0 && stat(p, &st) == 0 && st.st_mtime == 4000 && st.st_atime == 3000);
#ifdef SYS_futimesat
	WANT(syscall(SYS_futimesat, AT_FDCWD, p, tv) == 0 && stat(p, &st) == 0 && st.st_mtime == 2000);
#endif

	char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned char v[2] = {9, 9};
	WANT(mincore(m, 8192, v) == 0 && v[0] == 0);
	m[0] = 1;
	WANT(mincore(m, 8192, v) == 0 && v[0] == 1 && v[1] == 0);
	WANT(munmap(m, 8192) == 0 && mincore(m, 4096, v) == -1 && errno == ENOMEM);

	WANT(readahead(fd, 0, 4096) == 0);
	WANT(syncfs(fd) == 0);
	WANT(syscall(SYS_ioprio_get, 1, 0) >= 0);

	if (fsetxattr(fd, "user.k", "v1", 2, 0) == 0) {
		char l[64], val[8];
		WANT(fgetxattr(fd, "user.k", val, 8) == 2 && !memcmp(val, "v1", 2));
		WANT(flistxattr(fd, l, sizeof l) > 0 && strstr(l, "user.k"));
		WANT(fremovexattr(fd, "user.k") == 0);
		WANT(fgetxattr(fd, "user.k", val, 8) == -1);
	} else {
		WANT(errno == ENOTSUP || errno == EOPNOTSUPP);
	}

	struct { unsigned long long flags, mode, resolve; } how = { O_RDONLY, 0, 0 };
	int o2 = syscall(SYS_openat2, AT_FDCWD, p, &how, sizeof how);
	WANT(o2 >= 0);
	close(o2);
	how.resolve = 2;
	WANT(syscall(SYS_openat2, AT_FDCWD, p, &how, sizeof how) == -1 && errno == ENOSYS);
	WANT(syscall(452 /* fchmodat2 */, AT_FDCWD, p, 0600, 0) == 0 && stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);

	WANT(syscall(SYS_pkey_alloc, 0, 0) == -1 && errno == ENOSPC);
	struct timespec ts;
	WANT(sched_rr_get_interval(0, &ts) == 0 && ts.tv_nsec > 0);
	close(fd);
	unlink(p);
	puts("OK");
	return 0;
}
