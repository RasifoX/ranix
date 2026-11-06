/*
 * Thin entry points libc and programs expect, each over an existing primitive:
 * positioned vectored I/O, the older time-setting calls, fd forms of xattr calls.
 */

typedef struct { int64_t tv_sec, tv_usec; } ktv_t;

static long iov_at(int fd, const iovec_t *iov, int cnt, off_t pos, int write)
{
	if (cnt < 0 || cnt > 1024) return -22;   /* IOV_MAX */
	if (pos < 0) return -22;
	long total = 0;
	for (int i = 0; i < cnt; i++) {
		iovec_t v;
		if (copy_from_user(&v, &iov[i], sizeof(v)) != 0) return total ? total : -14;
		if (!v.iov_base || v.iov_len == 0) continue;
		long r = write ? sys_pwrite64(fd, v.iov_base, v.iov_len, pos + total)
		               : sys_pread64(fd, v.iov_base, v.iov_len, pos + total);
		if (r < 0) return total ? total : r;
		total += r;
		if ((size_t)r < v.iov_len) break;
	}
	return total;
}

static long sys_preadv(int fd, const iovec_t *iov, int cnt, off_t pos)  { return iov_at(fd, iov, cnt, pos, 0); }
static long sys_pwritev(int fd, const iovec_t *iov, int cnt, off_t pos) { return iov_at(fd, iov, cnt, pos, 1); }

/* offset -1 means the file position; the RWF_ flags are only hints here */
static long sys_preadv2(int fd, const iovec_t *iov, int cnt, off_t pos)
{
	return pos == -1 ? sys_readv(fd, iov, cnt) : iov_at(fd, iov, cnt, pos, 0);
}
static long sys_pwritev2(int fd, const iovec_t *iov, int cnt, off_t pos)
{
	return pos == -1 ? sys_writev(fd, iov, cnt) : iov_at(fd, iov, cnt, pos, 1);
}

static long utimes_common(int dirfd, const char *path, const ktv_t *utv)
{
	if (!utv) return do_utimensat(dirfd, path, NULL, 0, 0);
	ktv_t tv[2];
	if (copy_from_user(tv, utv, sizeof(tv)) != 0) return -14;
	if (tv[0].tv_usec < 0 || tv[0].tv_usec >= 1000000 || tv[1].tv_usec < 0 || tv[1].tv_usec >= 1000000)
		return -22;
	struct { int64_t sec, nsec; } ts[2] = {
		{ tv[0].tv_sec, tv[0].tv_usec * 1000 }, { tv[1].tv_sec, tv[1].tv_usec * 1000 } };
	return do_utimensat(dirfd, path, ts, 0, 1);
}

#ifdef SYS_ARCH_UTIME
static long sys_utimes(const char *path, const ktv_t *utv) { return utimes_common(AT_FDCWD, path, utv); }
static long sys_futimesat(int dirfd, const char *path, const ktv_t *utv) { return utimes_common(dirfd, path, utv); }
static long sys_utime(const char *path, const int64_t *ubuf)
{
	if (!ubuf) return do_utimensat(AT_FDCWD, path, NULL, 0, 0);
	int64_t b[2];
	if (copy_from_user(b, ubuf, sizeof(b)) != 0) return -14;
	struct { int64_t sec, nsec; } ts[2] = { { b[0], 0 }, { b[1], 0 } };
	return do_utimensat(AT_FDCWD, path, ts, 0, 1);
}
#endif

static long sys_mincore(uint64_t addr, size_t len, uint8_t *uvec)
{
	if (addr & (PAGE_SIZE - 1)) return -22;
	if (!access_ok((void *)addr, len)) return -12;
	size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
	vm_space_t *vm = task_current()->vm;
	for (size_t i = 0; i < pages; i++) {
		uint64_t va = addr + i * PAGE_SIZE;
		uint8_t in = user_pages_mapped((void *)va, 1) ? 1 : 0;
		if (!in) {
			int known = 0;
			uint64_t f = spin_lock_irq(&vm->fault_lock);
			for (vma_t *v = vm->vmas; v && !known; v = v->next)
				known = va >= v->vaddr && va < v->vaddr + v->size;
			spin_unlock_irq(&vm->fault_lock, f);
			if (!known) return -12;   /* ENOMEM: nothing mapped there */
		}
		if (copy_to_user(uvec + i, &in, 1) != 0) return -14;
	}
	return 0;
}

static vfs_file_t *fd_file(int fd)
{
	process_t *p = task_current();
	vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, fd) : NULL;
	return (f && f->inode) ? f : NULL;
}

static long sys_readahead(int fd, off_t off, size_t count)
{
	(void)off; (void)count;
	return fd_file(fd) ? 0 : -9;
}

static long sys_syncfs(int fd)
{
	if (!fd_file(fd)) return -9;
	return sys_sync();
}

static long sys_fsetxattr(int fd, const char *n, const void *v, size_t s, int flags)
{
	vfs_file_t *f = fd_file(fd);
	if (!f) return -9;
	if (f->mnt_flags & MS_RDONLY) return -30;
	char kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, n, sizeof(kname)) < 0) return -14;
	if (s > XATTR_VALUE_MAX) return -7;
	void *kv = NULL;
	if (s) {
		kv = kmalloc(s);
		if (!kv) return -12;
		if (copy_from_user(kv, v, s) != 0) { kfree(kv); return -14; }
	}
	vfs_iattr_t a = { .valid = ATTR_XATTR_SET, .xname = kname, .xval = kv, .xlen = s, .xflags = flags };
	int r = vfs_inode_setattr(f->inode, &a);
	if (kv) kfree(kv);
	return r;
}

static long sys_fremovexattr(int fd, const char *n)
{
	vfs_file_t *f = fd_file(fd);
	if (!f) return -9;
	if (f->mnt_flags & MS_RDONLY) return -30;
	char kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, n, sizeof(kname)) < 0) return -14;
	vfs_iattr_t a = { .valid = ATTR_XATTR_REMOVE, .xname = kname };
	return vfs_inode_setattr(f->inode, &a);
}

static long sys_flistxattr(int fd, char *ulist, size_t size)
{
	vfs_file_t *f = fd_file(fd);
	if (!f) return -9;
	if (size == 0) return vfs_inode_listxattr(f->inode, NULL, 0);
	if (size > XATTR_VALUE_MAX) size = XATTR_VALUE_MAX;
	char *kl = kmalloc(size);
	if (!kl) return -12;
	ssize_t r = vfs_inode_listxattr(f->inode, kl, size);
	if (r >= 0 && copy_to_user(ulist, kl, (size_t)r) != 0) r = -14;
	kfree(kl);
	return r;
}

static long sys_sched_rr_get_interval(pid_t pid, int64_t *uts)
{
	(void)pid;
	int64_t ts[2] = { 0, 100000000 };   /* the default slice */
	return copy_to_user(uts, ts, sizeof(ts)) == 0 ? 0 : -14;
}

static long sys_setdomainname(const char *name, size_t len)
{
	uts_namespace_t *ns = current_uts();
	process_t *cur = task_current();
	if (!capable_task(cur, CAP_SYS_ADMIN) &&
	    !(cap_has(cur->cap_effective, CAP_SYS_ADMIN) && cur->uts_ns && cur->uts_ns != &init_uts_ns))
		return -1;
	if (len >= sizeof(ns->domainname)) return -22;
	if (copy_from_user(ns->domainname, name, len) != 0) return -14;
	ns->domainname[len] = '\0';
	return 0;
}

/* No protection keys here: the honest answer to "give me one" */
static long sys_pkey_alloc(unsigned long flags, unsigned long rights)
{
	(void)rights;
	return flags ? -22 : -28;   /* ENOSPC */
}

static long sys_pkey_mprotect(uint64_t addr, size_t len, int prot, int pkey)
{
	return pkey == -1 ? sys_mprotect(addr, len, prot) : -22;
}

/* RESOLVE_ restrictions are not enforced, and silently ignoring one would defeat
 * its caller: such a request fails so it falls back to its own checks. */
static long sys_openat2(int dirfd, const char *path, const void *uhow, size_t size)
{
	struct { uint64_t flags, mode, resolve; } how;
	if (size < sizeof(how)) return -22;
	if (copy_from_user(&how, uhow, sizeof(how)) != 0) return -14;
	if (how.resolve) return -38;
	return sys_openat(dirfd, path, (int)how.flags, (mode_t)how.mode);
}

static long sys_fchmodat2(int d, const char *p, mode_t m, int flags)
{
	if (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) return -95;   /* links have no mode here */
	return sys_fchmodat(d, p, m, 0);
}
