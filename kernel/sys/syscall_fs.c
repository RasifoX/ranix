/* ── path helper ─────────────────────────────────────────── */

extern volatile uint64_t kernel_ticks;

/* Copy a user path into a PATH_MAX kernel buffer. -EFAULT on a bad pointer,
 * -ENAMETOOLONG if it does not fit (no NUL within PATH_MAX). */
static int copy_path_from_user(char *dst, const char *upath)
{
	long r = strncpy_from_user(dst, upath, PATH_MAX);
	if (r < 0) return -14;
	if (r >= PATH_MAX) return -36;
	return 0;
}

/* resolve a (possibly dirfd-relative) user path to an absolute kernel path.
 * `out` must hold PATH_MAX bytes. A relative path is joined onto cwd in a heap
 * scratch buffer so this stays a single PATH_MAX stack frame. */
static int resolve_path(char *out, int dirfd, const char *upath)
{
	int r = copy_path_from_user(out, upath);
	if (r) return r;
	if (out[0] == '/') return 0;
	process_t *p = task_current();
	const char *base;
	if (dirfd == AT_FDCWD) {
		base = p->cwd;
	} else {
		vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, dirfd) : NULL;
		if (!f || !f->inode) return -9;             /* EBADF */
		if (!S_ISDIR(f->inode->mode)) return -20;   /* ENOTDIR */
		if (f->path[0] != '/') return -9;
		base = f->path;
	}
	int blen = strlen(base), plen = strlen(out);
	if (blen + 1 + plen >= PATH_MAX) return -36;
	char *tmp = kmalloc(PATH_MAX);
	if (!tmp) return -12;
	memcpy(tmp, base, blen);
	if (blen == 0 || tmp[blen - 1] != '/') tmp[blen++] = '/';
	memcpy(tmp + blen, out, plen);
	tmp[blen + plen] = '\0';
	memcpy(out, tmp, blen + plen + 1);
	kfree(tmp);
	return 0;
}

/* ── fs syscalls ─────────────────────────────────────────── */

static long sys_mkdir(const char *path, mode_t mode)
{
	char kp[PATH_MAX];
	int r = copy_path_from_user(kp, path);
	if (r) return r;
	return vfs_mkdir(kp, mode);
}

static long sys_mkdirat(int dirfd, const char *path, mode_t mode)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return vfs_mkdir(kp, mode);
}

static long sys_unlink(const char *path)
{
	char kp[PATH_MAX];
	int r = copy_path_from_user(kp, path);
	if (r) return r;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

static long sys_unlinkat(int dirfd, const char *path, int flags)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -2;         /* ENOENT */
	if (flags & AT_REMOVEDIR) {
		if (!S_ISDIR(vs.mode)) return -20;          /* ENOTDIR */
	} else {
		if (S_ISDIR(vs.mode)) return -21;           /* EISDIR */
	}
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

/* rename takes two paths; stage them on the heap so the kernel-stack frame
 * stays a single pointer pair, not 2×PATH_MAX. */
static long sys_rename(const char *old, const char *new)
{
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -12;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -12; }
	long ret;
	int r;
	if ((r = copy_path_from_user(ko, old)) || (r = copy_path_from_user(kn, new)))
		ret = r;
	else
		ret = vfs_rename(ko, kn) == 0 ? 0 : -2;
	kfree(ko); kfree(kn);
	return ret;
}

static long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *newp)
{
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -12;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -12; }
	long ret;
	int r;
	if ((r = resolve_path(ko, olddirfd, old)) || (r = resolve_path(kn, newdirfd, newp)))
		ret = r;
	else
		ret = vfs_rename(ko, kn) == 0 ? 0 : -2;
	kfree(ko); kfree(kn);
	return ret;
}

static long sys_rmdir(const char *path)
{
	char kp[PATH_MAX];
	int r = copy_path_from_user(kp, path);
	if (r) return r;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

/* ── stat syscalls ───────────────────────────────────────── */

static void fill_linux_stat(linux_stat_t *st, const vfs_stat_t *vs)
{
	memset(st, 0, sizeof(*st));
	st->st_dev     = 1;
	st->st_mode    = vs->mode;
	st->st_ino     = vs->ino;
	st->st_size    = (int64_t)vs->size;
	st->st_nlink   = vs->nlink ? vs->nlink : 1;
	st->st_uid     = vs->uid;
	st->st_gid     = vs->gid;
	st->st_blksize = 4096;
	st->st_blocks  = (vs->size + 511) / 512;
	st->st_rdev    = vs->rdev;
	st->st_atime_sec = vs->atime; st->st_atime_nsec = vs->atime_nsec;
	st->st_mtime_sec = vs->mtime; st->st_mtime_nsec = vs->mtime_nsec;
	st->st_ctime_sec = vs->ctime; st->st_ctime_nsec = vs->ctime_nsec;
}

static long sys_stat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -14;
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_stat(kp, &vs) != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

/* lstat: same as stat but does not follow a final symlink. */
static long sys_lstat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -14;
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

static long sys_newfstatat(int dirfd, const char *path, linux_stat_t *st, int flags)
{
	if (!st) return -14;
	char kp[PATH_MAX];
	if (flags & AT_EMPTY_PATH) {
		/* stat the fd itself */
		if (strncpy_from_user(kp, path, 2) >= 0 && kp[0] == '\0')
			return sys_fstat(dirfd, st);
	}
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	int sr = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kp, &vs) : vfs_stat(kp, &vs);
	if (sr != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

/* R_OK=4 W_OK=2 X_OK=1 happen to equal the rwx permission bits. access() uses
 * the REAL uid/gid (faccessat with AT_EACCESS would use effective — minor). */
static long access_check(const char *kp, int mode, int use_eff)
{
	vfs_stat_t vs = {0};
	if (vfs_stat(kp, &vs) != 0) return -2;          /* ENOENT */
	if (mode == 0) return 0;                        /* F_OK: exists */
	process_t *p = task_current();
	uid_t uid = !p ? 0 : (use_eff ? p->euid : p->uid);
	gid_t gid = !p ? 0 : (use_eff ? p->egid : p->gid);
	if (uid == 0) {                                 /* root: r/w always, x if any x bit */
		if ((mode & 1) && !(vs.mode & 0111)) return -13; /* EACCES */
		return 0;
	}
	int shift;
	if (vs.uid == uid) shift = 6;
	else {
		int in_group = (vs.gid == gid);
		for (int i = 0; !in_group && p && i < p->ngroups; i++)
			if (p->groups[i] == vs.gid) in_group = 1;
		shift = in_group ? 3 : 0;
	}
	int want = mode & 7;
	return (((vs.mode >> shift) & 7) & want) == want ? 0 : -13;
}

static long sys_access(const char *path, int mode)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	return access_check(kp, mode, 0);
}

static long sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return access_check(kp, mode, flags & 0x200 /*AT_EACCESS*/);
}

static long sys_truncate(const char *path, off_t len)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_dentry_t *de = vfs_dentry_resolve(kp);
	if (!de || !de->inode) return -2;
	vfs_inode_t *in = de->inode;
	if (!in->iops || !in->iops->truncate) return -22;
	return in->iops->truncate(in, len);
}

static long sys_ftruncate(int fd, off_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	vfs_inode_t *in = f->inode;
	if (!in->iops || !in->iops->truncate) return -22;
	return in->iops->truncate(in, len);
}

/* ── getdents64 ──────────────────────────────────────────── */

#define DT_FIFO  1
#define DT_CHR   2
#define DT_DIR   4
#define DT_BLK   6
#define DT_REG   8
#define DT_LNK  10
#define DT_SOCK 12

static long sys_getdents64(int fd, void *buf, size_t bufsize)
{
	if (!access_ok(buf, bufsize)) return -14;
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	vfs_inode_t *inode = f->inode;
	if (!S_ISDIR(inode->mode)) return -20;
	if (!inode->iops || !inode->iops->readdir) return -95;

	uint8_t *kbuf = kmalloc(bufsize > 4096 ? 4096 : bufsize);
	if (!kbuf) return -12;
	size_t bufcap = bufsize > 4096 ? 4096 : bufsize;

	size_t  offset = 0;
	long    total  = 0;
	uint32_t idx   = (uint32_t)f->pos;

	while (1) {
		vfs_dirent_t vde;
		if (inode->iops->readdir(inode, idx, &vde) != 0) break;
		int namelen = strlen(vde.name);
		size_t reclen = (19 + namelen + 1 + 7) & ~7UL;
		if (offset + reclen > bufcap) break;

		uint8_t *e = kbuf + offset;
		memset(e, 0, reclen);
		*(uint64_t *)(e + 0)  = vde.ino;
		*(int64_t  *)(e + 8)  = (int64_t)vde.next_off;
		*(uint16_t *)(e + 16) = (uint16_t)reclen;
		e[18] = S_ISREG(vde.mode)  ? DT_REG  :
		        S_ISDIR(vde.mode)  ? DT_DIR  :
		        S_ISLNK(vde.mode)  ? DT_LNK  :
		        S_ISCHR(vde.mode)  ? DT_CHR  :
		        S_ISFIFO(vde.mode) ? DT_FIFO : 0;
		memcpy(e + 19, vde.name, namelen);
		e[19 + namelen] = '\0';
		offset += reclen;
		total  += (long)reclen;
		idx = vde.next_off;
	}

	f->pos = (off_t)idx;
	if (total > 0)
		copy_to_user(buf, kbuf, offset);
	kfree(kbuf);
	return total;
}

/* ── dup / fcntl ─────────────────────────────────────────── */

static long sys_dup(int oldfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, oldfd);
	if (!f) return -9;
	int newfd = fd_alloc(p->fds, f);
	if (newfd < 0) return -24;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup2(int oldfd, int newfd)
{
	if (newfd < 0) return -9;
	process_t *p = task_current();
	vfs_file_t *src = p->fds ? fd_get(p->fds, oldfd) : NULL;
	if (!src) return -9;
	if (oldfd == newfd) return newfd;
	if (fd_get(p->fds, newfd)) vfs_close(newfd);
	if (fd_alloc_at(p->fds, src, newfd) < 0) return -24;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup3(int oldfd, int newfd, int flags)
{
	long r = sys_dup2(oldfd, newfd);
	if (r >= 0) {
		process_t *p = task_current();
		fd_set_cloexec(p->fds, newfd, !!(flags & O_CLOEXEC));
	}
	return r;
}

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
#define F_DUPFD_CLOEXEC 1030
#define F_GETLK    5
#define F_SETLK    6
#define F_SETLKW   7
typedef struct { int16_t l_type; int16_t l_whence; int32_t _p; int64_t l_start; int64_t l_len; int32_t l_pid; int32_t _p2; } k_flock;
#define FD_CLOEXEC 1

static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
	process_t  *p = task_current();
	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		/* dup to first fd >= arg */
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		int nfd = fd_alloc_from(p->fds, f, (int)arg);
		if (nfd < 0) return -24;
		if (cmd == F_DUPFD_CLOEXEC) fd_set_cloexec(p->fds, nfd, 1);
		return nfd;
	}
	case F_GETFD: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		return fd_is_cloexec(p->fds, fd) ? FD_CLOEXEC : 0;
	}
	case F_SETFD: {
		if (!fd_get(p->fds, fd)) return -9;
		fd_set_cloexec(p->fds, fd, !!(arg & FD_CLOEXEC));
		return 0;
	}
	case F_GETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		return f ? (long)f->flags : -9;
	}
	case F_SETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		f->flags = (f->flags & ~0xFFF) | ((int)arg & 0xFFF);
		return 0;
	}
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f || !f->inode) return -9;
		k_flock fl;
		if (copy_from_user(&fl, (const void *)arg, sizeof(fl)) != 0) return -14;
		int64_t start = fl.l_start;
		if (fl.l_whence == 1) start += (int64_t)f->pos;             /* SEEK_CUR */
		else if (fl.l_whence == 2) start += (int64_t)f->inode->size; /* SEEK_END */
		int64_t end = (fl.l_len == 0) ? 0x7FFFFFFFFFFFFFFFLL : (start + fl.l_len - 1);
		int pid = (int)p->pid;
		if (cmd == F_GETLK) {
			int t = (fl.l_type == 1) ? 1 : 0; int64_t s = start, e = end, hp = 0; int hpid = 0;
			(void)hp;
			if (vfs_posix_getlk(f->inode, pid, &t, &s, &e, &hpid)) {
				fl.l_type = (int16_t)t; fl.l_start = s;
				fl.l_len  = (e == 0x7FFFFFFFFFFFFFFFLL) ? 0 : (e - s + 1);
				fl.l_pid  = hpid;
			} else {
				fl.l_type = 2; /* F_UNLCK: range is free */
			}
			return copy_to_user((void *)arg, &fl, sizeof(fl)) == 0 ? 0 : -14;
		}
		if (fl.l_type == 2) { vfs_posix_unlock(f->inode, pid, start, end); return 0; } /* F_UNLCK */
		int type = (fl.l_type == 1) ? 1 : 0;   /* F_WRLCK : F_RDLCK */
		for (;;) {
			int r = vfs_posix_setlk(f->inode, pid, type, start, end);
			if (r == 0) return 0;
			if (cmd != F_SETLKW) return r;     /* F_SETLK: EAGAIN on conflict */
			yield();
			if (task_has_actionable_signal()) return -4; /* EINTR */
		}
	}
	default: return 0;
	}
}

/* ── chdir / getcwd ──────────────────────────────────────── */

/* Lexically collapse "." and ".." in an absolute path (no dentry/mount walk —
 * the previous tree walk relied on parent linkage that resolved dentries don't
 * always have, so chdir into a plain dir could leave cwd at "/"). */
static void path_normalize(char *path)
{
	char *buf = kmalloc(PATH_MAX);
	if (!buf) return;
	int n = 0;
	const char *s = path;
	while (*s) {
		while (*s == '/') s++;
		if (!*s) break;
		const char *comp = s;
		while (*s && *s != '/') s++;
		int len = (int)(s - comp);
		if (len == 1 && comp[0] == '.') continue;
		if (len == 2 && comp[0] == '.' && comp[1] == '.') {
			while (n > 0 && buf[n - 1] != '/') n--;   /* drop last component */
			if (n > 0) n--;                            /* drop its leading slash */
			continue;
		}
		if (n + 1 + len >= PATH_MAX) break;
		buf[n++] = '/';
		memcpy(buf + n, comp, (size_t)len);
		n += len;
	}
	if (n == 0) buf[n++] = '/';
	buf[n] = '\0';
	memcpy(path, buf, (size_t)(n + 1));
	kfree(buf);
}

static long sys_chdir(const char *path)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	vfs_stat_t st;
	if (vfs_stat(kp, &st) != 0) return -2;      /* ENOENT */
	if (!S_ISDIR(st.mode)) return -20;          /* ENOTDIR */
	path_normalize(kp);
	process_t *p = task_current();
	int n = strlen(kp);
	if ((size_t)(n + 1) > sizeof(p->cwd)) return -36;
	memcpy(p->cwd, kp, (size_t)(n + 1));
	return 0;
}

static long sys_chroot(const char *path)
{
	process_t *p = task_current();
	if (p->euid != 0) return -1;                /* EPERM (CAP_SYS_CHROOT) */
	char kp[PATH_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	vfs_dentry_t *d = vfs_dentry_resolve(kp);
	if (!d || !d->inode) return -2;             /* ENOENT */
	if (!S_ISDIR(d->inode->mode)) return -20;   /* ENOTDIR */
	p->root = d;
	/* cwd is now relative to the new root; reset to "/" so a path that pointed
	 * outside the jail can't be reached (simple, escape-safe). */
	p->cwd[0] = '/'; p->cwd[1] = '\0';
	return 0;
}

static long sys_pivot_root(const char *new_root, const char *put_old)
{
	process_t *p = task_current();
	if (p->euid != 0) return -1;                       /* EPERM */
	char *knew = kmalloc(PATH_MAX);
	if (!knew) return -12;
	char *kold = kmalloc(PATH_MAX);
	if (!kold) { kfree(knew); return -12; }
	long ret = 0;
	int rr;
	if ((rr = resolve_path(knew, -100, new_root)) || (rr = resolve_path(kold, -100, put_old))) {
		ret = rr; goto pr_out;
	}
	vfs_dentry_t *dn = vfs_dentry_resolve(knew);
	if (!dn || !dn->inode) { ret = -2; goto pr_out; }          /* ENOENT */
	if (!S_ISDIR(dn->inode->mode)) { ret = -20; goto pr_out; } /* ENOTDIR */
	vfs_dentry_t *dold = vfs_dentry_resolve(kold);
	if (!dold || !dold->inode || !S_ISDIR(dold->inode->mode)) { ret = -22; goto pr_out; }
	/* put_old must lie at or under new_root (as Linux requires) */
	size_t nl = strlen(knew);
	if (strncmp(kold, knew, nl) != 0 || (kold[nl] != '/' && kold[nl] != '\0')) {
		ret = -22; goto pr_out;
	}
	/* Switch the caller's root to new_root. Children inherit it, so an init that
	 * pivot_roots and then execs the real init moves the whole process tree to
	 * the new root without global mount-tree surgery (initramfs→disk switch). */
	p->root = dn;
	p->cwd[0] = '/'; p->cwd[1] = '\0';
pr_out:
	kfree(knew); kfree(kold);
	return ret;
}

static long sys_getcwd(char *buf, size_t size)
{
	if (!buf || size == 0) return -14;
	process_t *p = task_current();
	int n = strlen(p->cwd);
	if ((size_t)(n + 1) > size) return -34; /* ERANGE */
	copy_to_user(buf, p->cwd, n + 1);
	return (long)(n + 1);
}

/* ── process info ────────────────────────────────────────── */

static long sys_getuid(void)  { return (long)from_kuid(current_user_ns(), task_current()->uid); }
static long sys_geteuid(void) { return (long)from_kuid(current_user_ns(), task_current()->euid); }
static long sys_getgid(void)  { return (long)from_kgid(current_user_ns(), task_current()->gid); }
static long sys_getegid(void) { return (long)from_kgid(current_user_ns(), task_current()->egid); }
static long sys_getppid(void)
{
	process_t *p = task_current();
	process_t *parent = proc_get(p->ppid);
	long r = parent ? (long)task_pid_nr_ns(parent, current_pid_ns()) : 0;
	proc_put(parent);
	return r;
}
static long sys_gettid(void)  { return (long)task_current()->pid; }
static long sys_getgroups(int size, uint64_t *list) {
	process_t *p = task_current();
	int n = p->ngroups;
	if (size == 0) return n;
	if (size < n) return -22; /* EINVAL */
	if (!access_ok(list, (size_t)n * sizeof(gid_t))) return -14;
	for (int i = 0; i < n; i++) {
		gid_t g = p->groups[i];
		if (copy_to_user(list + i, &g, sizeof(g))) return -14;
	}
	return n;
}

static long sys_setgroups(int size, const gid_t *list) {
	process_t *p = task_current();
	if (p->euid != 0) return -1; /* EPERM: only root */
	if (size < 0 || size > 32) return -22; /* EINVAL */
	if (size > 0 && !access_ok(list, (size_t)size * sizeof(gid_t))) return -14;
	for (int i = 0; i < size; i++) {
		gid_t g;
		if (copy_from_user(&g, list + i, sizeof(g))) return -14;
		p->groups[i] = g;
	}
	p->ngroups = size;
	return 0;
}
/* RLIMIT_* indices we treat specially */
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9
#define RLIM_INFINITY ((uint64_t)-1)

/* seed per-process rlimits on first use: infinite except NOFILE (FD_MAX) */
static void rlimit_seed(process_t *p)
{
	if (p->rlim_set) return;
	for (int i = 0; i < 16; i++) { p->rlim_cur[i] = RLIM_INFINITY; p->rlim_max[i] = RLIM_INFINITY; }
	p->rlim_cur[RLIMIT_NOFILE] = FD_MAX;
	p->rlim_max[RLIMIT_NOFILE] = FD_MAX;
	if (p->fds) p->rlim_cur[RLIMIT_NOFILE] = p->fds->rlim_nofile;
	p->rlim_set = 1;
}

static long do_setrlimit(process_t *p, int res, uint64_t cur, uint64_t max)
{
	if (res < 0 || res >= 16) return -22;            /* EINVAL */
	rlimit_seed(p);
	if (cur != RLIM_INFINITY && max != RLIM_INFINITY && cur > max) return -22;
	/* raising the hard limit needs privilege */
	if (max != p->rlim_max[res] &&
	    (p->rlim_max[res] == RLIM_INFINITY || (max != RLIM_INFINITY && max > p->rlim_max[res])) &&
	    p->euid != 0)
		return -1;                                   /* EPERM */
	p->rlim_cur[res] = cur;
	p->rlim_max[res] = max;
	if (res == RLIMIT_NOFILE && p->fds) {
		int n = (cur == RLIM_INFINITY || cur > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int)cur;
		p->fds->rlim_nofile = n;
	}
	return 0;
}

static long sys_getrlimit(int res, void *rlim) {
	if (!access_ok(rlim, 16)) return -14;
	if (res < 0 || res >= 16) return -22;
	process_t *p = task_current();
	rlimit_seed(p);
	uint64_t pair[2] = { p->rlim_cur[res], p->rlim_max[res] };
	return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14;
}
static long sys_setrlimit(int res, const void *rlim) {
	if (!access_ok(rlim, 16)) return -14;
	uint64_t pair[2];
	if (copy_from_user(pair, rlim, 16)) return -14;
	return do_setrlimit(task_current(), res, pair[0], pair[1]);
}
static long sys_getpgrp(void) { return (long)task_current()->pgrp; }
static long sys_getpgid(pid_t pid) {
	process_t *p = pid ? proc_get(pid) : task_current();
	long r = p ? (long)p->pgrp : -3;
	if (pid) proc_put(p);
	return r;
}
static long sys_getsid(pid_t pid) {
	process_t *p = pid ? proc_get(pid) : task_current();
	long r = p ? (long)p->sid : -3;
	if (pid) proc_put(p);
	return r;
}
static long sys_setsid(void) {
	process_t *p = task_current();
	p->pgrp = (pid_t)p->pid;
	p->sid  = (pid_t)p->pid;
	p->ctty = NULL;
	p->ctty_fops = NULL;
	return (long)p->pid;
}
static long sys_setpgid(pid_t pid, pid_t pgid) {
	process_t *p = pid ? proc_get(pid) : task_current();
	if (!p) return -3;
	p->pgrp = pgid ? pgid : (pid_t)p->pid;
	if (pid) proc_put(p);
	return 0;
}
static long sys_setuid(uid_t uid)
{
	process_t *p = task_current();
	if (p->euid == 0) {
		p->uid = p->euid = p->suid = uid;
	} else if (uid == p->uid || uid == p->suid) {
		p->euid = uid;
	} else {
		return -1; /* EPERM */
	}
	return 0;
}

static long sys_setgid(gid_t gid)
{
	process_t *p = task_current();
	if (p->egid == 0) {
		p->gid = p->egid = p->sgid = gid;
	} else if (gid == p->gid || gid == p->sgid) {
		p->egid = gid;
	} else {
		return -1; /* EPERM */
	}
	return 0;
}

static long sys_getpriority(int which, int who)
{
	(void)which; (void)who;
	return 20 - task_current()->nice;  /* POSIX: returns 20 - nice */
}
static long sys_setpriority(int which, int who, int prio)
{
	(void)which; (void)who;
	int nice = 20 - prio;
	if (nice < -20) nice = -20;
	if (nice >  19) nice =  19;
	task_current()->nice = nice;
	return 0;
}

static long sys_sigaltstack(const stack_t *uss, stack_t *uoss)
{
	process_t *p = task_current();
	if (uoss) {
		if (copy_to_user(uoss, &p->altstack, sizeof(stack_t)) != 0)
			return -14;
	}
	if (uss) {
		stack_t ss;
		if (copy_from_user(&ss, uss, sizeof(stack_t)) != 0)
			return -14;
		if (ss.ss_flags & SS_DISABLE) {
			p->altstack.ss_flags = SS_DISABLE;
			p->altstack.ss_sp    = NULL;
			p->altstack.ss_size  = 0;
		} else {
			if (!ss.ss_sp || ss.ss_size < 2048) return -22;
			p->altstack = ss;
		}
	}
	return 0;
}
static long sys_umask(mode_t mask)
{
	process_t *p = task_current();
	mode_t old = p->umask;
	p->umask = mask & 0777;
	return (long)old;
}

