/* ── path helper ─────────────────────────────────────────── */

extern volatile uint64_t kernel_ticks;

static int resolve_path(char *out, int dirfd, const char *upath)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, upath, PROC_NAME_MAX) < 0) return -14;
	if (kp[0] == '/') { memcpy(out, kp, PROC_NAME_MAX); return 0; }
	process_t *p = task_current();
	const char *base = (dirfd == AT_FDCWD) ? p->cwd : "/";
	int blen = strlen(base), plen = strlen(kp);
	if (blen + 1 + plen >= PROC_NAME_MAX) return -36;
	memcpy(out, base, blen);
	if (blen == 0 || out[blen - 1] != '/') out[blen++] = '/';
	memcpy(out + blen, kp, plen);
	out[blen + plen] = '\0';
	return 0;
}

/* ── fs syscalls ─────────────────────────────────────────── */

static long sys_mkdir(const char *path, mode_t mode)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
	return vfs_mkdir(kp, mode);
}

static long sys_mkdirat(int dirfd, const char *path, mode_t mode)
{
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return vfs_mkdir(kp, mode);
}

static long sys_unlink(const char *path)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

static long sys_unlinkat(int dirfd, const char *path, int flags)
{
	(void)flags;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

static long sys_rename(const char *old, const char *new)
{
	char ko[PROC_NAME_MAX], kn[PROC_NAME_MAX];
	if (strncpy_from_user(ko, old, PROC_NAME_MAX) < 0) return -14;
	if (strncpy_from_user(kn, new, PROC_NAME_MAX) < 0) return -14;
	return vfs_rename(ko, kn) == 0 ? 0 : -2;
}

static long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *newp)
{
	char ko[PROC_NAME_MAX], kn[PROC_NAME_MAX];
	int r = resolve_path(ko, olddirfd, old);  if (r) return r;
	    r = resolve_path(kn, newdirfd, newp); if (r) return r;
	return vfs_rename(ko, kn) == 0 ? 0 : -2;
}

static long sys_rmdir(const char *path)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
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
}

static long sys_stat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -14;
	char kp[PROC_NAME_MAX];
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
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

#define AT_EMPTY_PATH        0x1000
#define AT_SYMLINK_NOFOLLOW  0x0100

static long sys_newfstatat(int dirfd, const char *path, linux_stat_t *st, int flags)
{
	if (!st) return -14;
	char kp[PROC_NAME_MAX];
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

static long sys_access(const char *path, int mode)
{
	(void)mode;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	return vfs_stat(kp, &vs) == 0 ? 0 : -2;
}

static long sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
	(void)flags; (void)mode;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	return vfs_stat(kp, &vs) == 0 ? 0 : -2;
}

static long sys_truncate(const char *path, off_t len)
{
	char kp[256];
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
	default: return 0;
	}
}

/* ── chdir / getcwd ──────────────────────────────────────── */

/* Lexically collapse "." and ".." in an absolute path (no dentry/mount walk —
 * the previous tree walk relied on parent linkage that resolved dentries don't
 * always have, so chdir into a plain dir could leave cwd at "/"). */
static void path_normalize(char *path)
{
	char buf[PROC_NAME_MAX];
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
		if (n + 1 + len >= PROC_NAME_MAX) break;
		buf[n++] = '/';
		memcpy(buf + n, comp, (size_t)len);
		n += len;
	}
	if (n == 0) buf[n++] = '/';
	buf[n] = '\0';
	memcpy(path, buf, (size_t)(n + 1));
}

static long sys_chdir(const char *path)
{
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	vfs_stat_t st;
	if (vfs_stat(kp, &st) != 0) return -2;      /* ENOENT */
	if (!S_ISDIR(st.mode)) return -20;          /* ENOTDIR */
	path_normalize(kp);
	process_t *p = task_current();
	int n = strlen(kp);
	if (n >= 256) return -36;
	memcpy(p->cwd, kp, (size_t)(n + 1));
	return 0;
}

static long sys_chroot(const char *path)
{
	process_t *p = task_current();
	if (p->euid != 0) return -1;                /* EPERM (CAP_SYS_CHROOT) */
	char kp[PROC_NAME_MAX];
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
	process_t *parent = proc_find(p->ppid);
	return parent ? (long)task_pid_nr_ns(parent, current_pid_ns()) : 0;
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
static long sys_getrlimit(int res, void *rlim) {
	if (!access_ok(rlim, 16)) return -14;
	uint64_t inf = (uint64_t)-1;
	if (res == 7) { uint64_t pair[2] = {FD_MAX, FD_MAX}; return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14; }
	uint64_t pair[2] = {inf, inf};
	return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14;
}
static long sys_setrlimit(int res, const void *rlim) { (void)res; (void)rlim; return 0; }
static long sys_getpgrp(void) { return (long)task_current()->pgrp; }
static long sys_getpgid(pid_t pid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	return p ? (long)p->pgrp : -3;
}
static long sys_getsid(pid_t pid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	return p ? (long)p->sid : -3;
}
static long sys_setsid(void) {
	process_t *p = task_current();
	p->pgrp = (pid_t)p->pid;
	p->sid  = (pid_t)p->pid;
	return (long)p->pid;
}
static long sys_setpgid(pid_t pid, pid_t pgid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	if (!p) return -3;
	p->pgrp = pgid ? pgid : (pid_t)p->pid;
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

