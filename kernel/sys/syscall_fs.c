/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── path helper ─────────────────────────────────────────── */


/* Copy a user path into a PATH_MAX kernel buffer. -EFAULT on a bad pointer,
 * -ENAMETOOLONG if it does not fit (no NUL within PATH_MAX). */
static int copy_path_from_user(char *dst, const char *upath)
{
	long r = strncpy_from_user(dst, upath, PATH_MAX);
	if (r < 0) return -EFAULT;
	if (r >= PATH_MAX) return -ENAMETOOLONG;
	return 0;
}

/* Strip trailing '/' from a kernel path, keeping a lone "/". Linux's path walk
 * ignores trailing slashes ("a/b/" names "b"); without this a tool that appends
 * one (busybox mkdir -p, apk's dir extraction) makes the parent split see an
 * empty final component. Operates in place on a mutable kernel buffer. */
static void path_trim_trailing_slash(char *p)
{
	int n = (int)strlen(p);
	while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
}

/* The path of the directory dirfd names, into base (PATH_MAX), for a walk:
 * from where it stands now (vfs_file_dirpath), or by its opening path where the
 * cache keeps no position. *top: as vfs_file_dirpath. */
static int dirfd_base(process_t *p, int dirfd, char *base, vfs_dentry_t **top)
{
	vfs_file_t *f = (p && p->fds) ? fget(p->fds, dirfd) : NULL;
	int r = 0;
	*top = NULL;
	if (!f) return -EBADF;
	if (!f->inode)                     r = -9;
	else if (!S_ISDIR(f->inode->mode)) r = -20;                /* ENOTDIR */
	else if ((r = vfs_file_dirpath(f, base, top)) == 1) {
		r = f->path[0] == '/' ? 0 : -9;
		if (!r) memcpy(base, f->path, strlen(f->path) + 1);
	}
	fput(f);
	return r;
}

/* resolve a (possibly dirfd-relative) user path to an absolute kernel path.
 * `out` must hold PATH_MAX bytes. A relative path is joined onto its base in a
 * heap scratch buffer so this stays a single PATH_MAX stack frame. */
static int resolve_path(char *out, int dirfd, const char *upath)
{
	int r = copy_path_from_user(out, upath);
	if (r) return r;
	/* "" names nothing; joined onto the base it would silently mean the cwd */
	if (!out[0]) return -ENOENT;
	process_t *p = task_current();
	if (out[0] == '/') { path_trim_trailing_slash(out); return vfs_walk_from(p, NULL); }
	char *tmp = kmalloc(PATH_MAX);
	if (!tmp) return -ENOMEM;
	vfs_dentry_t *top = NULL;
	if (dirfd == AT_FDCWD) {
		fs_path_t *cwd = fs_cwd_get(p->fs);   /* held: a CLONE_FS sibling may chdir meanwhile */
		memcpy(tmp, cwd->s, strlen(cwd->s) + 1);
		fs_path_put(cwd);
	} else if (!(r = dirfd_base(p, dirfd, tmp, &top))) {
		vfs_walk_anchor(p, tmp);
	}
	if (!r) r = vfs_walk_from(p, top);
	int blen = r ? 0 : (int)strlen(tmp), plen = (int)strlen(out);
	if (!r && blen + 1 + plen >= PATH_MAX) r = -36;
	if (r) { kfree(tmp); return r; }
	if (blen == 0 || tmp[blen - 1] != '/') tmp[blen++] = '/';
	memcpy(tmp + blen, out, plen);
	tmp[blen + plen] = '\0';
	memcpy(out, tmp, blen + plen + 1);
	kfree(tmp);
	path_trim_trailing_slash(out);
	return 0;
}

/* ── fs syscalls ─────────────────────────────────────────── */

static long sys_mkdir(const char *path, mode_t mode)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
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

/* A directory holds only "." and ".." when empty. The cursor belongs to the
 * filesystem (a byte offset on ext2), so step by next_off as getdents does. */
static int dir_is_empty(const char *kp)
{
	vfs_dirent_t de;
	for (uint32_t idx = 0; vfs_readdir(kp, idx, &de) == 0; idx = de.next_off)
		if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0) return 0;
	return 1;
}

static long sys_unlink(const char *path)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -ENOENT;
	if (S_ISDIR(vs.mode)) return -EISDIR;
	r = vfs_unlink(kp);
	return r == -30 || r == -16 || r == -13 || r == -1 ? r : (r == 0 ? 0 : -2);
}

static long sys_unlinkat(int dirfd, const char *path, int flags)
{
	if (flags & ~AT_REMOVEDIR) return -EINVAL;
	char kp[PATH_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -ENOENT;
	if (flags & AT_REMOVEDIR) {
		if (!S_ISDIR(vs.mode)) return -ENOTDIR;
		if (vfs_path_mnt_flags(kp) & MS_RDONLY) return -EROFS;
		if (!dir_is_empty(kp)) return -ENOTEMPTY;
	} else {
		if (S_ISDIR(vs.mode)) return -EISDIR;
	}
	r = vfs_unlink(kp);
	return r == -30 || r == -16 || r == -13 || r == -1 ? r : (r == 0 ? 0 : -2);
}

/* rename takes two paths; stage them on the heap so the kernel-stack frame
 * stays a single pointer pair, not 2×PATH_MAX. */
static long sys_rename(const char *old, const char *new)
{
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -ENOMEM;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -ENOMEM; }
	long ret;
	int r;
	if ((r = resolve_path(ko, AT_FDCWD, old)) || (r = resolve_path(kn, AT_FDCWD, new)))
		ret = r;
	else
		ret = vfs_rename(ko, kn);
	kfree(ko); kfree(kn);
	return ret;
}

static long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *newp)
{
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -ENOMEM;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -ENOMEM; }
	long ret;
	int r;
	if ((r = resolve_path(ko, olddirfd, old)) || (r = resolve_path(kn, newdirfd, newp)))
		ret = r;
	else
		ret = vfs_rename(ko, kn);
	kfree(ko); kfree(kn);
	return ret;
}

static long sys_rmdir(const char *path)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -ENOENT;
	if (!S_ISDIR(vs.mode)) return -ENOTDIR;
	if (vfs_path_mnt_flags(kp) & MS_RDONLY) return -EROFS;
	if (!dir_is_empty(kp)) return -ENOTEMPTY;
	r = vfs_unlink(kp);
	return r == -30 || r == -16 || r == -13 || r == -1 ? r : (r == 0 ? 0 : -2);
}

/* ── stat syscalls ───────────────────────────────────────── */

static void fill_linux_stat(linux_stat_t *st, const vfs_stat_t *vs)
{
	memset(st, 0, sizeof(*st));
	st->st_dev     = vs->sdev ? ((vs->sdev & 0xff) | ((uint64_t)(vs->sdev & ~0xffu) << 12)) : 1;
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

static long sys_fstat(int fd, linux_stat_t *st);

/* stat() through /proc/<self>/fd/N of a file with no path (pipe, socket,
 * eventfd, memfd): the link names no file to follow, so stat the open file
 * itself, as Linux's magic links do. Chromium's sandbox fstatat()s every
 * entry of /proc/self/fd and CHECKs that it works. -2 when kp is not one. */
static long proc_fd_stat(const char *kp, linux_stat_t *st)
{
	const char *p = kp;
	process_t *cur = task_current();
	if (strncmp(p, "/proc/", 6) != 0) return -ENOENT;
	p += 6;
	if (strncmp(p, "self/fd/", 8) == 0) {
		p += 8;
	} else {
		long pid = 0;
		while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
		if (pid != task_vpid(cur) && pid != (long)cur->tgid) return -ENOENT;
		if (strncmp(p, "/fd/", 4) != 0) return -ENOENT;
		p += 4;
	}
	int n = 0;
	if (*p < '0' || *p > '9') return -ENOENT;
	while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
	if (*p || !fd_get(cur->fds, n)) return -ENOENT;
	return sys_fstat(n, st);
}

static long sys_stat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -EFAULT;
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_stat(kp, &vs) != 0) return proc_fd_stat(kp, st);
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks)) ? -14 : 0;
}

/* lstat: same as stat but does not follow a final symlink. */
static long sys_lstat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -EFAULT;
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_lstat(kp, &vs) != 0) return -ENOENT;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks)) ? -14 : 0;
}

static long sys_newfstatat(int dirfd, const char *path, linux_stat_t *st, int flags)
{
	if (!st) return -EFAULT;
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
	if (sr != 0) return (flags & AT_SYMLINK_NOFOLLOW) ? -2 : proc_fd_stat(kp, st);
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks)) ? -14 : 0;
}

/* R_OK=4 W_OK=2 X_OK=1 equal the FS_MAY_* bits. access() asks for the REAL
 * uid/gid, with capabilities only when the real uid is 0; AT_EACCESS asks for
 * the task as it is (fsuid/fsgid, effective capabilities), as Linux. */
static long access_mode(const vfs_stat_t *vs, int mode, int flags)
{
	if (mode & ~7) return -EINVAL;
	if (mode == 0) return 0;                        /* F_OK: exists */
	fs_cred_t c;
	if (flags & AT_EACCESS) fs_cred_task(task_current(), &c);
	else fs_cred_access(task_current(), &c);
	return fs_permission(&c, vs->mode, vs->uid, vs->gid, mode);
}

/* flags: AT_EACCESS, AT_SYMLINK_NOFOLLOW (ask about the link itself) */
static long access_check(const char *kp, int mode, int flags)
{
	if (mode & ~7) return -EINVAL;
	vfs_stat_t vs = {0};
	int sr = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kp, &vs) : vfs_stat(kp, &vs);
	if (sr != 0) return -ENOENT;
	return access_mode(&vs, mode, flags);
}

/* faccessat2(fd, "", mode, AT_EMPTY_PATH): the open file itself (the cwd for AT_FDCWD) */
static long access_fd(int fd, int mode, int flags)
{
	process_t *p = task_current();
	vfs_stat_t vs = {0};
	if (fd == AT_FDCWD) {
		fs_path_t *cwd = fs_cwd_get(p->fs);
		int r = vfs_stat(cwd->s, &vs);
		fs_path_put(cwd);
		if (r) return -ENOENT;
	} else {
		vfs_file_t *f = fget(p->fds, fd);
		if (!f) return -EBADF;
		vs.mode = f->inode->mode;
		vs.uid  = f->inode->uid;
		vs.gid  = f->inode->gid;
		fput(f);
	}
	return access_mode(&vs, mode, flags);
}

static long sys_access(const char *path, int mode)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	return access_check(kp, mode, 0);
}

/* faccessat(2) has three arguments; the flags are faccessat2's (Linux 5.8) */
static long sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
	if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
	char kp[PATH_MAX];
	if (flags & AT_EMPTY_PATH) {
		if (strncpy_from_user(kp, path, 2) >= 0 && kp[0] == '\0') return access_fd(dirfd, mode, flags);
	}
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return access_check(kp, mode, flags);
}

static long sys_truncate(const char *path, off_t len)
{
	if (len < 0) return -EINVAL;
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	if (vfs_path_mnt_flags(kp) & MS_RDONLY) return -EROFS;
	vfs_dentry_t *de = vfs_dentry_resolve(kp);
	if (!de || !de->inode) return -ENOENT;
	if (vfs_dentry_readonly(de)) return -EROFS;
	vfs_inode_t *in = de->inode;
	if (S_ISDIR(in->mode)) return -EISDIR;
	r = vfs_may_write(in);
	if (r) return r;
	if (!in->iops || !in->iops->truncate) return -EINVAL;
	return in->iops->truncate(in, len);
}

static long sys_ftruncate(int fd, off_t len)
{
	if (len < 0) return -EINVAL;
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);   /* pinned: a truncate can block on the disk */
	if (!f) return -EBADF;
	vfs_inode_t *in = f->inode;
	long r = -22;
	if ((f->flags & 3) != O_RDONLY && !S_ISDIR(in->mode) && in->iops && in->iops->truncate)
		r = in->iops->truncate(in, len);
	fput(f);
	return r;
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
	if (!access_ok(buf, bufsize)) return -EFAULT;
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);   /* pinned: readdir can block on the disk */
	if (!f) return -EBADF;
	vfs_inode_t *inode = f->inode;
	if (!S_ISDIR(inode->mode)) { fput(f); return -ENOTDIR; }
	if (!inode->iops || !inode->iops->readdir) { fput(f); return -EOPNOTSUPP; }
	if (!bufsize) { fput(f); return -EINVAL; }

	uint8_t *kbuf = kmalloc(bufsize > 4096 ? 4096 : bufsize);
	if (!kbuf) { fput(f); return -ENOMEM; }
	size_t bufcap = bufsize > 4096 ? 4096 : bufsize;

	size_t  offset = 0;
	long    total  = 0;
	uint32_t idx   = (uint32_t)f->pos;

	while (1) {
		vfs_dirent_t vde;
		int rrc = inode->iops->readdir(inode, idx, &vde);
		if (rrc != 0) {
			/* -1 is the end; anything else is a directory that could not be read
			 * whole, and a caller copying a tree must hear of it */
			if (rrc != -1 && !total) total = rrc;
			break;
		}
		int namelen = strlen(vde.name);
		size_t reclen = (19 + namelen + 1 + 7) & ~7UL;
		if (offset + reclen > bufcap) {
			/* 0 would read as end of directory */
			if (!total) total = -22;
			break;
		}

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
	if (total > 0 && copy_to_user(buf, kbuf, offset) != 0) total = -14;
	kfree(kbuf);
	fput(f);
	return total;
}

/* ── dup / fcntl ─────────────────────────────────────────── */

static long sys_dup(int oldfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, oldfd);
	if (!f) return -EBADF;
	int newfd = fd_alloc(p->fds, f);
	fput(f);
	if (newfd < 0) return -EMFILE;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup2(int oldfd, int newfd)
{
	if (newfd < 0) return -EBADF;
	process_t *p = task_current();
	vfs_file_t *src = p->fds ? fget(p->fds, oldfd) : NULL;
	if (!src) return -EBADF;
	if (oldfd == newfd) { fput(src); return newfd; }
	if (newfd >= p->fds->rlim_nofile) { fput(src); return -EBADF; }
	if (fd_get(p->fds, newfd)) vfs_close(newfd);
	int r = fd_alloc_at(p->fds, src, newfd);
	fput(src);
	if (r < 0) return -EMFILE;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup3(int oldfd, int newfd, int flags)
{
	if (oldfd == newfd || (flags & ~O_CLOEXEC)) return -EINVAL;
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
#define O_ASYNC_FL 020000
#define SETFL_MASK (O_APPEND | O_NONBLOCK | O_ASYNC_FL)
#define F_DUPFD_CLOEXEC 1030
#define F_GETLK    5
#define F_SETLK    6
#define F_SETLKW   7
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38
typedef struct { int16_t l_type; int16_t l_whence; int32_t _p; int64_t l_start; int64_t l_len; int32_t l_pid; int32_t _p2; } k_flock;
#define FD_CLOEXEC 1

/* Who-waits-for-whom for F_SETLKW, kept on the group leader (a record lock
 * belongs to the process). Linux posix_locks_deadlock follows it from the
 * holder; reaching the caller means the wait would never end. */
static void plock_wait_set(pid_t tgid, pid_t holder)
{
	process_t *q = proc_get(tgid);
	if (q) q->plock_wait = holder;
	proc_put(q);
}

static int plock_would_deadlock(pid_t me, pid_t holder)
{
	for (int i = 0; i < 16 && holder > 0; i++) {   /* Linux gives up after 10 */
		if (holder == me) return 1;
		process_t *q = proc_get(holder);
		pid_t next = q ? q->plock_wait : 0;
		proc_put(q);
		holder = next;
	}
	return 0;
}

/* F_GETLK/F_SETLK/F_SETLKW and the F_OFD_ forms on a pinned file */
static long fcntl_lock(vfs_file_t *f, int cmd, uint64_t arg)
{
	if (!f->inode) return -EBADF;
	k_flock fl;
	if (copy_from_user(&fl, (const void *)arg, sizeof(fl)) != 0) return -EFAULT;
	/* OFD locks belong to the open file, so a fork child or a dup shares them
	 * and a second open() of the same file conflicts even in one process. */
	int ofd = cmd >= F_OFD_GETLK;
	if (ofd && fl.l_pid != 0) return -EINVAL;
	if (ofd) cmd = (cmd == F_OFD_GETLK) ? F_GETLK : (cmd == F_OFD_SETLK) ? F_SETLK : F_SETLKW;
	if (fl.l_whence < 0 || fl.l_whence > 2 || fl.l_type < 0 || fl.l_type > 2) return -EINVAL;
	int64_t start = fl.l_start, end;
	/* an off_t that does not fit is EOVERFLOW, as on Linux; a wrapped sum
	 * would otherwise name some other range */
	if (fl.l_whence == 1 && __builtin_add_overflow(start, (int64_t)f->pos, &start)) return -EOVERFLOW;
	if (fl.l_whence == 2 && __builtin_add_overflow(start, (int64_t)f->inode->size, &start)) return -EOVERFLOW;
	if (fl.l_len == 0) end = 0x7FFFFFFFFFFFFFFFLL;
	else if (fl.l_len > 0) { if (__builtin_add_overflow(start, fl.l_len - 1, &end)) return -EOVERFLOW; }
	else { end = start - 1; if (__builtin_add_overflow(start, fl.l_len, &start)) return -EINVAL; } /* POSIX: range ends at start */
	if (start < 0 || end < start) return -EINVAL;
	/* a record lock belongs to the process, not the thread that took it (Linux:
	 * the files_struct). SQLite runs queries on worker threads; per-thread
	 * owners made VS Code's own threads lock each other out (SQLITE_BUSY). */
	long pid = ofd ? (long)(uintptr_t)f : (long)task_current()->tgid;
	if (cmd == F_GETLK) {
		int t = (fl.l_type == 1) ? 1 : 0; int64_t s = start, e = end; int hpid = 0;
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
	pid_t me = task_current()->tgid;
	for (;;) {
		int r = vfs_posix_setlk(f->inode, pid, type, start, end);
		if (r == 0 || cmd != F_SETLKW) {   /* F_SETLK: EAGAIN on conflict */
			if (!ofd) plock_wait_set(me, 0);
			return r;
		}
		if (!ofd) {   /* OFD locks have no deadlock check, as on Linux */
			int t = type, holder = 0; int64_t s = start, e = end;
			vfs_posix_getlk(f->inode, pid, &t, &s, &e, &holder);
			if (plock_would_deadlock(me, holder)) { plock_wait_set(me, 0); return -EDEADLK; }
			plock_wait_set(me, holder);
		}
		yield();
		if (task_has_actionable_signal()) { if (!ofd) plock_wait_set(me, 0); return -EINTR; }
	}
}

#define F_SETOWN     8
#define F_GETOWN     9
#define F_SETSIG     10
#define F_GETSIG     11
#define F_SETOWN_EX  15
#define F_GETOWN_EX  16
#define F_SETLEASE   1024
#define F_GETLEASE   1025
#define F_NOTIFY     1026
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define F_ADD_SEALS  1033
#define F_GET_SEALS  1034
#define F_OWNER_TID  0
#define F_OWNER_PID  1
#define F_OWNER_PGRP 2

struct f_owner_ex { int type; int pid; };

/* Who an O_ASYNC file signals, and with what. Kept as global pids; read back as
 * the caller's namespace numbers them. */
static long fcntl_owner(int fd, int cmd, uint64_t arg)
{
	vfs_file_t *f = fd_get(task_current()->fds, fd);
	if (!f) return -EBADF;
	switch (cmd) {
	case F_SETOWN: {
		int v = (int)arg;
		if (v == 0) { f->owner = 0; return 0; }
		pid_t g = pid_from_user(v < 0 ? -v : v);
		if (!g) return -ESRCH;
		f->owner = v < 0 ? -g : g;
		f->owner_type = v < 0 ? F_OWNER_PGRP : F_OWNER_PID;
		return 0;
	}
	case F_GETOWN:
		if (!f->owner) return 0;
		return f->owner > 0 ? pid_to_user(f->owner) : -pid_to_user(-f->owner);
	case F_SETSIG:
		if ((int)arg < 0 || (int)arg > NSIG) return -EINVAL;
		f->owner_sig = (int)arg;
		return 0;
	case F_GETSIG:
		return f->owner_sig;
	case F_SETOWN_EX: {
		struct f_owner_ex ex;
		if (copy_from_user(&ex, (const void *)arg, sizeof(ex)) != 0) return -EFAULT;
		if (ex.type != F_OWNER_TID && ex.type != F_OWNER_PID && ex.type != F_OWNER_PGRP) return -EINVAL;
		if (ex.pid == 0) { f->owner = 0; f->owner_type = ex.type; return 0; }
		pid_t g = pid_from_user(ex.pid < 0 ? -ex.pid : ex.pid);
		if (!g || ex.pid < 0) return -ESRCH;
		f->owner = ex.type == F_OWNER_PGRP ? -g : g;
		f->owner_type = ex.type;
		return 0;
	}
	case F_GETOWN_EX: {
		struct f_owner_ex ex = { f->owner ? f->owner_type : F_OWNER_PID, 0 };
		if (f->owner) ex.pid = pid_to_user(f->owner > 0 ? f->owner : -f->owner);
		return copy_to_user((void *)arg, &ex, sizeof(ex)) ? -EFAULT : 0;
	}
	}
	return -EINVAL;
}

/* Leases are not kept: a lease that is never broken would promise what it
 * cannot. One that could not be had anyway gets Linux's answer, EAGAIN (the
 * file is open for writing, or open elsewhere for a write lease); the rest is
 * EINVAL, as on a Linux with leases off. There is never one to report. */
static long fcntl_lease(int fd, int cmd, uint64_t arg)
{
	vfs_file_t *f = fd_get(task_current()->fds, fd);
	if (!f) return -EBADF;
	if (cmd == F_GETLEASE) return 2;   /* F_UNLCK */
	if (!f->inode || !S_ISREG(f->inode->mode)) return -EINVAL;
	if (arg == 2) return 0;            /* F_UNLCK: there is none to drop */
	if (arg != 0 && arg != 1) return -EINVAL;
	int writable = (f->flags & 3) != 0;
	if (arg == 0 && writable) return -EAGAIN;                    /* F_RDLCK */
	if (arg == 1 && f->inode->open_refs > 1) return -EAGAIN;     /* F_WRLCK */
	return -EINVAL;
}

static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
	process_t  *p = task_current();
	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		/* dup to first fd >= arg */
		if ((int)arg < 0) return -EINVAL;
		vfs_file_t *f = fget(p->fds, fd);
		if (!f) return -EBADF;
		int nfd = fd_alloc_from(p->fds, f, (int)arg);
		fput(f);
		if (nfd < 0) return -EMFILE;
		if (cmd == F_DUPFD_CLOEXEC) fd_set_cloexec(p->fds, nfd, 1);
		return nfd;
	}
	case F_GETFD: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -EBADF;
		return fd_is_cloexec(p->fds, fd) ? FD_CLOEXEC : 0;
	}
	case F_SETFD: {
		if (!fd_get(p->fds, fd)) return -EBADF;
		fd_set_cloexec(p->fds, fd, !!(arg & FD_CLOEXEC));
		return 0;
	}
	case F_GETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		return f ? (long)f->flags : -9;
	}
	case F_SETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -EBADF;
		/* the access mode is fixed at open: pipe_close keys off it to tell a
		 * reader from a writer */
		int was = f->flags;
		f->flags = (f->flags & ~SETFL_MASK) | ((int)arg & SETFL_MASK);
		if ((was ^ f->flags) & O_ASYNC_FL) pipe_fcntl_async(f);
		return 0;
	}
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
	case F_OFD_GETLK: case F_OFD_SETLK: case F_OFD_SETLKW: {
		/* pinned: F_SETLKW sleeps, and a sibling close() must not free the
		 * file under it (an OFD lock taken after that would outlive it) */
		vfs_file_t *f = fget(p->fds, fd);
		if (!f) return -EBADF;
		long r = fcntl_lock(f, cmd, arg);
		fput(f);
		return r;
	}
	case F_SETOWN: case F_GETOWN: case F_SETSIG: case F_GETSIG:
	case F_SETOWN_EX: case F_GETOWN_EX:
		return fcntl_owner(fd, cmd, arg);
	case F_SETLEASE: case F_GETLEASE:
		return fcntl_lease(fd, cmd, arg);
	case F_GETPIPE_SZ: case F_SETPIPE_SZ: {
		vfs_file_t *f = fget(p->fds, fd);
		if (!f) return -EBADF;
		long r = pipe_fcntl_size(f, cmd == F_SETPIPE_SZ, arg);
		fput(f);
		return r;
	}
	/* Not here, and saying so: a success that does nothing would let a caller
	 * believe it is notified (F_NOTIFY, dnotify) or that a memfd is sealed. */
	case F_NOTIFY: case F_ADD_SEALS: case F_GET_SEALS:
	default: return fd_get(p->fds, fd) ? -22 : -9;
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
	if (vfs_stat(kp, &st) != 0) return -ENOENT;
	if (!S_ISDIR(st.mode)) return -ENOTDIR;
	path_normalize(kp);
	process_t *p = task_current();
	if (access_check(kp, 1, AT_EACCESS)) return -EACCES;
	return fs_set_cwd(p->fs, kp);
}

static long sys_chroot(const char *path)
{
	process_t *p = task_current();
	/* in the caller's own user namespace, as Linux: a rootless sandbox may
	 * chroot (its paths stay under the old root; setuid there is inert) */
	if (!capable_own_ns((struct process *)p, CAP_SYS_CHROOT)) return -EPERM;
	char kp[PATH_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	vfs_dentry_t *d = vfs_dentry_resolve(kp);
	if (!d || !d->inode) return -ENOENT;
	if (!S_ISDIR(d->inode->mode)) return -ENOTDIR;
	/* a generated directory has no dentry to hold: an empty root stands in */
	if (vfs_dentry_is_volatile(d)) d = vfs_empty_root();
	else vfs_dentry_keep(d);   /* the root points at it: the dcache must not drop it */
	/* cwd is now relative to the new root; reset to "/" so a path that pointed
	 * outside the jail can't be reached (simple, escape-safe). Shared under
	 * CLONE_FS: the tasks sharing it move with it, as on Linux. */
	p->fs->root = d;
	return fs_set_cwd(p->fs, "/");
}

static long sys_pivot_root(const char *new_root, const char *put_old)
{
	process_t *p = task_current();
	if (p->euid != 0) return -EPERM;
	char *knew = kmalloc(PATH_MAX);
	if (!knew) return -ENOMEM;
	char *kold = kmalloc(PATH_MAX);
	if (!kold) { kfree(knew); return -ENOMEM; }
	long ret = 0;
	int rr;
	if ((rr = resolve_path(knew, -100, new_root)) || (rr = resolve_path(kold, -100, put_old))) {
		ret = rr; goto pr_out;
	}
	vfs_dentry_t *dn = vfs_dentry_resolve(knew);
	if (!dn || !dn->inode) { ret = -2; goto pr_out; }          /* ENOENT */
	if (!S_ISDIR(dn->inode->mode)) { ret = -20; goto pr_out; } /* ENOTDIR */
	if (vfs_dentry_is_volatile(dn)) { ret = -22; goto pr_out; } /* p->root outlives it */
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
	vfs_dentry_keep(dn);
	p->fs->root = dn;
	ret = fs_set_cwd(p->fs, "/");
pr_out:
	kfree(knew); kfree(kold);
	return ret;
}

static long sys_getcwd(char *buf, size_t size)
{
	if (!buf || size == 0) return -EFAULT;
	fs_path_t *cwd = fs_cwd_get(task_current()->fs);
	int n = strlen(cwd->s);
	long r = (size_t)(n + 1) > size ? -34                         /* ERANGE */
	       : copy_to_user(buf, cwd->s, n + 1) != 0 ? -14 : (long)(n + 1);
	fs_path_put(cwd);
	return r;
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
static long sys_gettid(void)  { return (long)task_vpid(task_current()); }
static long sys_getgroups(int size, gid_t *list) {
	process_t *p = task_current();
	int n = p->ngroups;
	if (size == 0) return n;
	if (size < n) return -EINVAL;
	if (!access_ok(list, (size_t)n * sizeof(gid_t))) return -EFAULT;
	for (int i = 0; i < n; i++) {
		gid_t g = p->groups[i];
		if (copy_to_user(list + i, &g, sizeof(g))) return -EFAULT;
	}
	return n;
}

static long sys_setgroups(int size, const gid_t *list) {
	process_t *p = task_current();
	if (p->euid != 0) return -EPERM;   /* only root */
	if (p->user_ns && p->user_ns->setgroups_denied) return -EPERM;   /* /proc/<pid>/setgroups "deny" */
	if (size < 0 || size > 32) return -EINVAL;
	if (size > 0 && !access_ok(list, (size_t)size * sizeof(gid_t))) return -EFAULT;
	for (int i = 0; i < size; i++) {
		gid_t g;
		if (copy_from_user(&g, list + i, sizeof(g))) return -EFAULT;
		p->groups[i] = g;
	}
	p->ngroups = size;
	return 0;
}

/* seed per-process rlimits on first use: infinite except NOFILE (FD_MAX) */
static void rlimit_seed(process_t *p)
{
	if (p->rlim_set) return;
	for (int i = 0; i < RLIM_NLIMITS; i++) { p->rlim_cur[i] = RLIM_INFINITY; p->rlim_max[i] = RLIM_INFINITY; }
	p->rlim_cur[RLIMIT_NOFILE] = FD_MAX;
	p->rlim_max[RLIMIT_NOFILE] = FD_MAX;
	if (p->fds) p->rlim_cur[RLIMIT_NOFILE] = p->fds->rlim_nofile;
	p->rlim_set = 1;
}

static long do_setrlimit(process_t *p, int res, uint64_t cur, uint64_t max)
{
	if (res < 0 || res >= RLIM_NLIMITS) return -EINVAL;
	rlimit_seed(p);
	if (max != RLIM_INFINITY && cur > max) return -EINVAL;
	/* raising the hard limit needs privilege; RLIM_INFINITY is the top value, so
	 * lowering from it is always allowed. The caller's, not the target's, for
	 * prlimit on another task. */
	if (max > p->rlim_max[res] && task_current()->euid != 0)
		return -EPERM;
	p->rlim_cur[res] = cur;
	p->rlim_max[res] = max;
	if (res == RLIMIT_NOFILE && p->fds) {
		int n = (cur == RLIM_INFINITY || cur > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int)cur;
		p->fds->rlim_nofile = n;
	}
	rlimit_store_group(p);   /* the group's limit, not this thread's */
	return 0;
}

static long sys_getrlimit(int res, void *rlim) {
	if (!access_ok(rlim, 16)) return -EFAULT;
	if (res < 0 || res >= RLIM_NLIMITS) return -EINVAL;
	process_t *p = task_current();
	rlimit_seed(p);
	uint64_t pair[2] = { p->rlim_cur[res], p->rlim_max[res] };
	return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14;
}
static long sys_setrlimit(int res, const void *rlim) {
	if (!access_ok(rlim, 16)) return -EFAULT;
	uint64_t pair[2];
	if (copy_from_user(pair, rlim, 16)) return -EFAULT;
	return do_setrlimit(task_current(), res, pair[0], pair[1]);
}
static long sys_getpgrp(void) { return (long)pid_to_user(task_current()->pgrp); }
static long sys_getpgid(pid_t pid) {
	process_t *p = pid ? proc_get_user(pid) : task_current();
	long r = p ? (long)pid_to_user(p->pgrp) : -3;
	if (pid) proc_put(p);
	return r;
}
static long sys_getsid(pid_t pid) {
	process_t *p = pid ? proc_get_user(pid) : task_current();
	long r = p ? (long)pid_to_user(p->sid) : -3;
	if (pid) proc_put(p);
	return r;
}
static long sys_setsid(void) {
	process_t *p = task_current();
	p->pgrp = (pid_t)p->pid;
	p->sid  = (pid_t)p->pid;
	p->ctty = NULL;
	p->ctty_fops = NULL;
	return (long)task_vpid(p);
}
/* POSIX: only the caller or a child in its session, never a session leader, and
 * only into a group of that session. */
static long sys_setpgid(pid_t pid, pid_t pgid) {
	process_t *cur = task_current();
	if (pgid < 0) return -EINVAL;
	process_t *p = pid ? proc_get_user(pid) : cur;
	if (!p) return -ESRCH;
	process_t *par = proc_get(p->ppid);
	pid_t pgrp = pgid ? pid_from_user(pgid) : (pid_t)p->pid;
	long r = 0;
	if (p->tgid != cur->tgid && (!par || par->tgid != cur->tgid))
		r = -3;
	else if (p->sid != cur->sid || p->sid == (pid_t)p->pid)
		r = -1;
	else if (pgrp != (pid_t)p->pid && (!pgrp || !task_pgrp_in_session(pgrp, p->sid)))
		r = -1;
	else
		p->pgrp = pgrp;
	proc_put(par);
	if (pid) proc_put(p);
	return r;
}
/* set*id: a process may move among the ids it holds; any other id takes
 * CAP_SETUID / CAP_SETGID (capable_setid), whatever the current ids are. */
static int uid_held(const process_t *p, uid_t id)
{
	return id == p->uid || id == p->euid || id == p->suid;
}

static int gid_held(const process_t *p, gid_t id)
{
	return id == p->gid || id == p->egid || id == p->sgid;
}

static int may_take_uid(process_t *p, uid_t id)
{
	return uid_held(p, id) || capable_setid((struct process *)p, CAP_SETUID, id);
}

static int may_take_gid(process_t *p, gid_t id)
{
	return gid_held(p, id) || capable_setid((struct process *)p, CAP_SETGID, id);
}

/* Linux __sys_setuid: with CAP_SETUID all of r/e/s; else only the effective
 * uid, and only to the real or saved one. */
static long sys_setuid(uid_t uid)
{
	process_t *p = task_current();
	uid_t old_euid = p->euid;
	int was_priv = (p->uid == 0 || p->euid == 0 || p->suid == 0);
	if (uid == (uid_t)-1) return -EINVAL;
	if (capable_setid((struct process *)p, CAP_SETUID, uid)) {
		p->uid = p->euid = p->suid = uid;
	} else if (uid == p->uid || uid == p->suid) {
		p->euid = uid;
	} else {
		return -EPERM;
	}
	p->fs_uid_set = 0;   /* the fsuid follows the euid again */
	cred_setxuid_fixup(p, was_priv, old_euid);
	return 0;
}

static long sys_setgid(gid_t gid)
{
	process_t *p = task_current();
	if (gid == (gid_t)-1) return -EINVAL;
	if (capable_setid((struct process *)p, CAP_SETGID, gid)) {
		p->gid = p->egid = p->sgid = gid;
	} else if (gid == p->gid || gid == p->sgid) {
		p->egid = gid;
	} else {
		return -EPERM;
	}
	p->fs_gid_set = 0;
	return 0;
}

/* PRIO_PROCESS with a pid acts on that process, and only if the caller may
 * control it (task_may_control: not a setuid program the caller started).
 * PRIO_PGRP/PRIO_USER act on the caller alone, and only when they name its own
 * group or user (0 = own). */
static process_t *prio_target(int which, int who, int *err)
{
	process_t *cur = task_current();
	*err = 0;
	if (which < 0 || which > 2) { *err = -22; return NULL; }
	if (who == 0) return cur;
	if (which == 0) {
		process_t *t = proc_get_user(who);
		if (!t) { *err = -3; return NULL; }                  /* ESRCH */
		if (t == cur) proc_put(t);   /* callers hold a reference only on others */
		return t;
	}
	if ((which == 1 && (pid_t)who == cur->pgrp) || (which == 2 && (uid_t)who == cur->uid)) return cur;
	*err = -3;
	return NULL;
}

static long sys_getpriority(int which, int who)
{
	int err;
	process_t *t = prio_target(which, who, &err);
	if (!t) return err;
	long r = 20 - t->nice;  /* POSIX: returns 20 - nice */
	if (t != task_current()) proc_put(t);
	return r;
}
static long sys_setpriority(int which, int who, int prio)
{
	int err;
	process_t *t = prio_target(which, who, &err);
	if (!t) return err;
	process_t *cur = task_current();
	/* the raw nice value: only getpriority applies the 20-nice offset */
	int nice = prio;
	if (nice < -20) nice = -20;
	if (nice >  19) nice =  19;
	long r = 0;
	if (t != cur && !task_may_control(t, CAP_SYS_NICE)) r = -1;                   /* EPERM */
	else if (nice < t->nice && !capable_task(cur, CAP_SYS_NICE)) r = -13;          /* EACCES */
	else t->nice = nice;
	if (t != cur) proc_put(t);
	return r;
}

static long sys_sigaltstack(const stack_t *uss, stack_t *uoss, uint64_t usp)
{
	process_t *p = task_current();
	uint64_t lo = (uint64_t)p->altstack.ss_sp;
	int on = p->altstack.ss_size && usp >= lo && usp - lo < p->altstack.ss_size;
	if (uoss) {
		stack_t o = p->altstack;
		o.ss_flags = on ? SS_ONSTACK : (o.ss_size ? 0 : SS_DISABLE);
		if (copy_to_user(uoss, &o, sizeof(stack_t)) != 0)
			return -EFAULT;
	}
	if (uss) {
		stack_t ss;
		if (copy_from_user(&ss, uss, sizeof(stack_t)) != 0)
			return -EFAULT;
		if (on) return -EPERM;   /* cannot swap the stack being run on */
		if (ss.ss_flags & SS_DISABLE) {
			p->altstack.ss_flags = SS_DISABLE;
			p->altstack.ss_sp    = NULL;
			p->altstack.ss_size  = 0;
		} else {
			if (!ss.ss_sp) return -EINVAL;
			if (ss.ss_size < 2048) return -ENOMEM;   /* < MINSIGSTKSZ */
			p->altstack = ss;
		}
	}
	return 0;
}
static long sys_umask(mode_t mask)
{
	process_t *p = task_current();
	mode_t old = p->fs->umask;
	p->fs->umask = mask & 0777;
	return (long)old;
}

