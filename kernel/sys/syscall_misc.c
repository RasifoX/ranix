#include <kernel/capabilities.h>

/* ── statfs ──────────────────────────────────────────────── */

typedef struct {
	long     f_type;
	long     f_bsize;
	uint64_t f_blocks;
	uint64_t f_bfree;
	uint64_t f_bavail;
	uint64_t f_files;
	uint64_t f_ffree;
	int      f_fsid[2];  /* __kernel_fsid_t = 2 x int = 8 bytes */
	long     f_namelen;
	long     f_frsize;
	long     f_flags;
	long     f_spare[4];
} kernel_statfs_t;

static long sys_statfs(const char *path, kernel_statfs_t *buf)
{
	(void)path;
	if (!access_ok(buf, sizeof(*buf))) return -14;
	kernel_statfs_t st;
	memset(&st, 0, sizeof(st));
	st.f_type    = 0x858458f6; /* RAMFS_MAGIC */
	st.f_bsize   = 4096;
	st.f_blocks  = 1024;
	st.f_bfree   = 512;
	st.f_bavail  = 512;
	st.f_files   = 1024;
	st.f_ffree   = 512;
	st.f_namelen = 255;
	copy_to_user(buf, &st, sizeof(st));
	return 0;
}

static long sys_fstatfs(int fd, kernel_statfs_t *buf)
{
	(void)fd;
	return sys_statfs(NULL, buf);
}

/* ── capabilities (root always has all) ─────────────────── */

static long sys_capget(void *hdrp, void *datap)
{
	if (!hdrp || !access_ok(hdrp, 8)) return -14;

	uint32_t version;
	if (copy_from_user(&version, hdrp, 4)) return -14;
	/* always report v3 back */
	version = _LINUX_CAPABILITY_VERSION_3;
	if (copy_to_user(hdrp, &version, 4)) return -14;

	if (!datap) return 0;
	if (!access_ok(datap, 24)) return -14;

	process_t *p = task_current();
	/* v3 layout: two cap_user_data structs (caps 0-31, caps 32-63) */
	uint32_t data[6] = {
		(uint32_t)(p->cap_effective  & 0xffffffff),
		(uint32_t)(p->cap_permitted  & 0xffffffff),
		(uint32_t)(p->cap_inheritable & 0xffffffff),
		(uint32_t)(p->cap_effective  >> 32),
		(uint32_t)(p->cap_permitted  >> 32),
		(uint32_t)(p->cap_inheritable >> 32),
	};
	return copy_to_user(datap, data, 24) == 0 ? 0 : -14;
}

static long sys_capset(void *hdrp, const void *datap)
{
	if (!hdrp || !access_ok(hdrp, 8)) return -14;
	process_t *p = task_current();
	/* only permitted to drop or set within permitted set; root can do anything */
	if (!datap) return -14;
	if (!access_ok(datap, 24)) return -14;

	uint32_t data[6];
	if (copy_from_user(data, datap, 24)) return -14;

	uint64_t new_eff  = (uint64_t)data[0] | ((uint64_t)data[3] << 32);
	uint64_t new_perm = (uint64_t)data[1] | ((uint64_t)data[4] << 32);
	uint64_t new_inh  = (uint64_t)data[2] | ((uint64_t)data[5] << 32);

	/* new_permitted must be subset of current permitted */
	if ((new_perm & ~p->cap_permitted) != 0) return -1; /* EPERM */
	/* new_effective must be subset of new permitted */
	if ((new_eff & ~new_perm) != 0) return -1;
	/* inheritable: unprivileged can only inherit from current inheritable ∪ permitted */
	if (p->euid != 0 && (new_inh & ~(p->cap_inheritable | p->cap_permitted)) != 0)
		return -1;

	p->cap_permitted   = new_perm;
	p->cap_effective   = new_eff;
	p->cap_inheritable = new_inh;
	return 0;
}

/* ── xattr — generic per-inode key/value store (vfs_inode_*xattr) ──
 * Namespaces follow Linux: only user./trusted./security./system. are valid;
 * the privileged namespaces need euid 0. Symlink (l*) variants currently
 * resolve like the plain ones — user.* xattrs are not allowed on symlinks
 * anyway, so this only differs for the rarely-used privileged namespaces. */
#define XATTR_NAME_MAX_LEN 255
#define XATTR_VALUE_MAX    65536

static int xattr_ns(const char *name)
{
	if (!strncmp(name, "user.", 5))     return 1;
	if (!strncmp(name, "trusted.", 8))  return 2;
	if (!strncmp(name, "security.", 9)) return 2;
	if (!strncmp(name, "system.", 7))   return 2;
	return 0; /* unknown namespace → EOPNOTSUPP */
}

static vfs_inode_t *xattr_resolve(const char *upath, char *kpath)
{
	if (resolve_path(kpath, AT_FDCWD, upath)) return NULL;
	vfs_dentry_t *d = vfs_dentry_resolve(kpath);
	return (d && d->inode) ? d->inode : NULL;
}

static long xattr_set(const char *upath, const char *uname, const void *uval,
                      size_t size, int flags)
{
	char kpath[PROC_NAME_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;
	int ns = xattr_ns(kname);
	if (!ns) return -95; /* EOPNOTSUPP */
	process_t *cp = task_current();
	if (ns == 2 && (!cp || cp->euid != 0)) return -1; /* EPERM */
	if (size > XATTR_VALUE_MAX) return -7; /* E2BIG */
	vfs_inode_t *in = xattr_resolve(upath, kpath);
	if (!in) return -2;
	if (ns == 1 && !S_ISREG(in->mode) && !S_ISDIR(in->mode)) return -95;
	void *kv = NULL;
	if (size) {
		kv = kmalloc(size);
		if (!kv) return -12;
		if (copy_from_user(kv, uval, size) != 0) { kfree(kv); return -14; }
	}
	int r = vfs_inode_setxattr(in, kname, kv, size, flags);
	if (kv) kfree(kv);
	return r;
}

static long xattr_get_inode(vfs_inode_t *in, const char *kname, void *uval, size_t size)
{
	if (size == 0) return vfs_inode_getxattr(in, kname, NULL, 0);
	if (size > XATTR_VALUE_MAX) size = XATTR_VALUE_MAX;
	void *kv = kmalloc(size);
	if (!kv) return -12;
	ssize_t r = vfs_inode_getxattr(in, kname, kv, size);
	if (r >= 0 && copy_to_user(uval, kv, (size_t)r) != 0) r = -14;
	kfree(kv);
	return r;
}

static long xattr_get(const char *upath, const char *uname, void *uval, size_t size)
{
	char kpath[PROC_NAME_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;
	vfs_inode_t *in = xattr_resolve(upath, kpath);
	if (!in) return -2;
	return xattr_get_inode(in, kname, uval, size);
}

static long xattr_list(const char *upath, char *ulist, size_t size)
{
	char kpath[PROC_NAME_MAX];
	vfs_inode_t *in = xattr_resolve(upath, kpath);
	if (!in) return -2;
	if (size == 0) return vfs_inode_listxattr(in, NULL, 0);
	if (size > XATTR_VALUE_MAX) size = XATTR_VALUE_MAX;
	char *kl = kmalloc(size);
	if (!kl) return -12;
	ssize_t r = vfs_inode_listxattr(in, kl, size);
	if (r >= 0 && copy_to_user(ulist, kl, (size_t)r) != 0) r = -14;
	kfree(kl);
	return r;
}

static long sys_getxattr(const char *p, const char *n, void *v, size_t s) { return xattr_get(p, n, v, s); }
static long sys_lgetxattr(const char *p, const char *n, void *v, size_t s) { return xattr_get(p, n, v, s); }
static long sys_setxattr(const char *p, const char *n, const void *v, size_t s, int f) { return xattr_set(p, n, v, s, f); }
static long sys_lsetxattr(const char *p, const char *n, const void *v, size_t s, int f) { return xattr_set(p, n, v, s, f); }
static long sys_listxattr(const char *p, char *l, size_t s) { return xattr_list(p, l, s); }
static long sys_llistxattr(const char *p, char *l, size_t s) { return xattr_list(p, l, s); }

static long sys_removexattr(const char *p, const char *n)
{
	char kpath[PROC_NAME_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, n, sizeof(kname)) < 0) return -14;
	vfs_inode_t *in = xattr_resolve(p, kpath);
	if (!in) return -2;
	return vfs_inode_removexattr(in, kname);
}

static long sys_fgetxattr(int fd, const char *n, void *v, size_t s)
{
	process_t *cp = task_current();
	vfs_file_t *f = (cp && cp->fds) ? fd_get(cp->fds, fd) : NULL;
	if (!f || !f->inode) return -9; /* EBADF */
	char kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, n, sizeof(kname)) < 0) return -14;
	return xattr_get_inode(f->inode, kname, v, s);
}

/* ── batch: missing syscalls ─────────────────────────────── */

static long sys_pread64(int fd, void *buf, size_t count, off_t offset)
{
	if (!access_ok(buf, count)) return -14;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return -9;
	vfs_seek(fd, offset, 0);
	long n = vfs_read(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_pwrite64(int fd, const void *buf, size_t count, off_t offset)
{
	if (!access_ok(buf, count)) return -14;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return -9;
	vfs_seek(fd, offset, 0);
	long n = vfs_write(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size,
                       int flags, uint64_t new_addr)
{
	(void)flags; (void)new_addr;
	if (new_size == 0) return -22;
	if (new_size <= old_size) return (long)old_addr;
	/* growing: allocate new, copy, unmap old */
	long na = sys_mmap(0, new_size, 3 /*RW*/, 0x22 /*MAP_ANON|MAP_PRIVATE*/, -1, 0);
	if (na < 0) return -12;
	memcpy((void *)na, (void *)old_addr, old_size);
	sys_munmap(old_addr, old_size);
	return na;
}

static long sys_pause(void)
{
	process_t *p = task_current();
	while (!(p->sig_pending & ~p->sig_mask))
		task_sleep_interruptible();
	return -4; /* EINTR */
}

static long sys_alarm(unsigned int seconds)
{
	process_t *p = task_current();
	unsigned int remaining = (unsigned int)(p->itimer_value / TIMER_HZ);
	p->itimer_value    = (uint64_t)seconds * TIMER_HZ;
	p->itimer_interval = 0;
	return (long)remaining;
}

static long sys_socketpair(int domain, int type, int protocol, int *sv)
{
    (void)protocol;
    if (domain != AF_UNIX) return -97;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -22;

    socket_t *s0 = sock_alloc(AF_UNIX, type, 0);
    socket_t *s1 = sock_alloc(AF_UNIX, type, 0);
    if (!s0 || !s1) { sock_free(s0); sock_free(s1); return -12; }

    s0->unix_peer = s1;
    s1->unix_peer = s0;
    s0->tcp_state = TCP_ESTABLISHED;
    s1->tcp_state = TCP_ESTABLISHED;

    int fd0 = sock_to_fd(s0);
    if (fd0 < 0) { sock_free(s0); sock_free(s1); return fd0; }
    int fd1 = sock_to_fd(s1);
    if (fd1 < 0) {
        /* fd0 already allocated; free s1, let fd0 cleanup handle s0 */
        sock_free(s1); s0->unix_peer = NULL;
        return fd1;
    }

    int kbuf[2] = { fd0, fd1 };
    if (copy_to_user(sv, kbuf, sizeof(kbuf)) != 0) return -14;
    return 0;
}

static long sys_flock(int fd, int operation)
{
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
	process_t *p = task_current();
	vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, fd) : NULL;
	if (!f) return -9; /* EBADF */
	int cmd = operation & ~LOCK_NB;
	if (cmd == LOCK_UN) { vfs_flock_release(f); return 0; }
	if (cmd != LOCK_SH && cmd != LOCK_EX) return -22; /* EINVAL */
	for (;;) {
		int r = vfs_flock_try(f, cmd);
		if (r == 0) return 0;
		if (operation & LOCK_NB) return -11; /* EWOULDBLOCK */
		yield();                              /* block: retry when holder releases */
		if (task_has_actionable_signal()) return -4; /* EINTR */
	}
}

static long sys_fsync(int fd)    { (void)fd; return 0; }
static long sys_fdatasync(int fd){ (void)fd; return 0; }

static long sys_fchdir(int fd)
{
	process_t *p = task_current();
	if (!p->fds) return -9;
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	if (!f->inode || (f->inode->mode & 0xF000) != 0x4000) return -20; /* ENOTDIR */
	memcpy(p->cwd, f->path, sizeof(p->cwd));
	p->cwd[sizeof(p->cwd) - 1] = '\0';
	return 0;
}

static long sys_creat(const char *path, mode_t mode)
{
	return sys_open(path, 0101 /* O_CREAT|O_WRONLY */ | 01000 /* O_TRUNC */, mode);
}

static long sys_lchown(const char *path, uid_t uid, gid_t gid)
{
	return sys_chown(path, uid, gid);
}

static long sys_getrusage(int who, void *usage)
{
	(void)who;
	if (!access_ok(usage, 144)) return -14;
	memset(usage, 0, 144); /* struct rusage is 144 bytes */
	return 0;
}

static long sys_setreuid(uid_t ruid, uid_t euid)
{
	process_t *p = task_current();
	uid_t new_r = (ruid != (uid_t)-1) ? ruid : p->uid;
	uid_t new_e = (euid != (uid_t)-1) ? euid : p->euid;
	/* unprivileged: new values must come from the current triplet */
	if (p->euid != 0) {
		if (new_r != p->uid && new_r != p->euid && new_r != p->suid) return -1;
		if (new_e != p->uid && new_e != p->euid && new_e != p->suid) return -1;
	}
	p->uid  = new_r;
	p->euid = new_e;
	/* setreuid: if ruid set or euid != old euid, update suid to new euid */
	if (ruid != (uid_t)-1 || (euid != (uid_t)-1 && euid != p->euid))
		p->suid = new_e;
	return 0;
}

static long sys_setregid(gid_t rgid, gid_t egid)
{
	process_t *p = task_current();
	gid_t new_r = (rgid != (gid_t)-1) ? rgid : p->gid;
	gid_t new_e = (egid != (gid_t)-1) ? egid : p->egid;
	if (p->egid != 0) {
		if (new_r != p->gid && new_r != p->egid && new_r != p->sgid) return -1;
		if (new_e != p->gid && new_e != p->egid && new_e != p->sgid) return -1;
	}
	p->gid  = new_r;
	p->egid = new_e;
	if (rgid != (gid_t)-1 || (egid != (gid_t)-1 && egid != p->egid))
		p->sgid = new_e;
	return 0;
}

static long sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	process_t *p = task_current();
	uid_t new_r = (ruid != (uid_t)-1) ? ruid : p->uid;
	uid_t new_e = (euid != (uid_t)-1) ? euid : p->euid;
	uid_t new_s = (suid != (uid_t)-1) ? suid : p->suid;
	if (p->euid != 0) {
		if (new_r != p->uid && new_r != p->euid && new_r != p->suid) return -1;
		if (new_e != p->uid && new_e != p->euid && new_e != p->suid) return -1;
		if (new_s != p->uid && new_s != p->euid && new_s != p->suid) return -1;
	}
	p->uid = new_r; p->euid = new_e; p->suid = new_s;
	return 0;
}

static long sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	process_t *p = task_current();
	if (copy_to_user(ruid, &p->uid,  sizeof(uid_t))) return -14;
	if (copy_to_user(euid, &p->euid, sizeof(uid_t))) return -14;
	if (copy_to_user(suid, &p->suid, sizeof(uid_t))) return -14;
	return 0;
}

static long sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
	process_t *p = task_current();
	gid_t new_r = (rgid != (gid_t)-1) ? rgid : p->gid;
	gid_t new_e = (egid != (gid_t)-1) ? egid : p->egid;
	gid_t new_s = (sgid != (gid_t)-1) ? sgid : p->sgid;
	if (p->egid != 0) {
		if (new_r != p->gid && new_r != p->egid && new_r != p->sgid) return -1;
		if (new_e != p->gid && new_e != p->egid && new_e != p->sgid) return -1;
		if (new_s != p->gid && new_s != p->egid && new_s != p->sgid) return -1;
	}
	p->gid = new_r; p->egid = new_e; p->sgid = new_s;
	return 0;
}

static long sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	process_t *p = task_current();
	if (copy_to_user(rgid, &p->gid,  sizeof(gid_t))) return -14;
	if (copy_to_user(egid, &p->egid, sizeof(gid_t))) return -14;
	if (copy_to_user(sgid, &p->sgid, sizeof(gid_t))) return -14;
	return 0;
}

static long sys_rt_sigpending(uint64_t *uset, size_t sigsetsize)
{
	(void)sigsetsize;
	if (!access_ok(uset, sizeof(uint64_t))) return -14;
	uint64_t pending = task_current()->sig_pending;
	return copy_to_user(uset, &pending, sizeof(pending)) == 0 ? 0 : -14;
}

static long sys_rt_sigsuspend(const uint64_t *unewmask, size_t sigsetsize)
{
	(void)sigsetsize;
	uint64_t newmask;
	if (copy_from_user(&newmask, unewmask, sizeof(newmask)) != 0) return -14;
	process_t *p = task_current();
	uint64_t oldmask = p->sig_mask;
	p->sig_mask = newmask;
	while (!(p->sig_pending & ~p->sig_mask))
		task_sleep_interruptible();
	p->sig_mask = oldmask;
	return -4; /* EINTR */
}

/* kp is a resolved kernel-absolute path */
static long do_mknod(const char *kp, mode_t mode)
{
	int type = mode & 0xF000;
	if (type == 0x1000 /* S_IFIFO */) return vfs_mkfifo(kp, mode);
	if (type == 0 || type == 0x8000 /* S_IFREG */) {
		int fd = vfs_open(kp, 0x41 /*O_CREAT|O_WRONLY*/, mode & 0777);
		if (fd < 0) return fd;
		vfs_close(fd);
		return 0;
	}
	return -1; /* device/socket nodes: EPERM for now */
}

static long sys_mknod(const char *path, mode_t mode, uint64_t dev)
{
	(void)dev;
	char kp[PROC_NAME_MAX];
	if (resolve_path(kp, -100 /*AT_FDCWD*/, path) != 0) return -14;
	return do_mknod(kp, mode);
}

static long sys_mknodat(int dirfd, const char *path, mode_t mode, uint64_t dev)
{
	(void)dev;
	char kp[PROC_NAME_MAX];
	if (resolve_path(kp, dirfd, path) != 0) return -14;
	return do_mknod(kp, mode);
}

static long sys_mlock(uint64_t addr, size_t len)   { (void)addr; (void)len; return 0; }
static long sys_munlock(uint64_t addr, size_t len) { (void)addr; (void)len; return 0; }
static long sys_mlock2(uint64_t addr, size_t len, int flags)
	{ (void)addr; (void)len; (void)flags; return 0; }

static long sys_sync(void) { return 0; }

static long sys_mount(const char *source, const char *target, const char *fstype,
                      unsigned long flags, const void *data)
{
	(void)source; (void)flags; (void)data;
	char ktarget[256]; char kfstype[64];
	if (strncpy_from_user(ktarget, target, sizeof(ktarget)) < 0) return -14;
	if (strncpy_from_user(kfstype, fstype, sizeof(kfstype)) < 0) return -14;

	/* map tmpfs → ramfs; sysfs/cgroup/devtmpfs → accept silently */
	const char *fsname = kfstype;
	if (strcmp(kfstype, "tmpfs") == 0)   fsname = "ramfs";
	if (strcmp(kfstype, "sysfs") == 0)   return 0;
	if (strcmp(kfstype, "cgroup") == 0)  return 0;
	if (strcmp(kfstype, "cgroup2") == 0) return 0;
	if (strcmp(kfstype, "devtmpfs") == 0) fsname = "devfs";
	if (strcmp(kfstype, "mqueue") == 0)  return 0;
	if (strcmp(kfstype, "hugetlbfs") == 0) return 0;
	if (strcmp(kfstype, "binfmt_misc") == 0) return 0;
	if (strcmp(kfstype, "pstore") == 0)  return 0;
	if (strcmp(kfstype, "efivarfs") == 0) return 0;
	if (strcmp(kfstype, "securityfs") == 0) return 0;
	if (strcmp(kfstype, "configfs") == 0) return 0;

	int r = vfs_mount(ktarget, fsname, NULL);
	return r == 0 ? 0 : -1;
}

static long sys_umount2(const char *target, int flags)
{
	(void)flags;
	char kpath[256];
	if (strncpy_from_user(kpath, target, sizeof(kpath)) < 0) return -14;
	int r = vfs_umount(kpath);
	return r == 0 ? 0 : -22;
}

static long sys_sethostname(const char *name, size_t len)
{
	uts_namespace_t *ns = current_uts();
	if (len >= sizeof(ns->nodename)) return -22;
	if (!access_ok(name, len)) return -14;
	if (copy_from_user(ns->nodename, name, len) != 0) return -14;
	ns->nodename[len] = '\0';
	return 0;
}

static long sys_tkill(pid_t tid, int sig) { return sys_kill(tid, sig); }

static long sys_time_syscall(long *tloc)
{
	int64_t s, n; ranix_walltime(&s, &n);   /* real wall-clock (RTC-based) */
	long t = (long)s;
	if (tloc && access_ok(tloc, sizeof(long)))
		copy_to_user(tloc, &t, sizeof(t));
	return t;
}

static long sys_clock_nanosleep(int clkid, int flags,
                                linux_timespec_t *req, linux_timespec_t *rem)
{
	(void)clkid; (void)flags;
	return sys_nanosleep(req, rem);
}

static long sys_waitid(int idtype, int id, void *infop, int options, void *rusage)
{
	(void)infop; (void)rusage;
	pid_t wpid = (idtype == 0) ? -1 : (idtype == 1) ? (pid_t)id : -(pid_t)id;
	int status;
	long r = sys_waitpid(wpid, &status, options);
	return (r < 0) ? r : 0;
}

static long sys_splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                       size_t len, unsigned int flags)
{
	(void)flags;
	char buf[4096];
	size_t done = 0;
	while (done < len) {
		size_t chunk = len - done;
		if (chunk > sizeof(buf)) chunk = sizeof(buf);
		if (off_in) {
			off_t o; copy_from_user(&o, off_in, sizeof(o));
			vfs_seek(fd_in, o, 0);
		}
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) { if (done == 0) return r; break; }
		if (off_in) {
			off_t o; copy_from_user(&o, off_in, sizeof(o));
			o += r; copy_to_user(off_in, &o, sizeof(o));
		}
		if (off_out) {
			off_t o; copy_from_user(&o, off_out, sizeof(o));
			vfs_seek(fd_out, o, 0);
		}
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		if (off_out) {
			off_t o; copy_from_user(&o, off_out, sizeof(o));
			o += w; copy_to_user(off_out, &o, sizeof(o));
		}
		done += (size_t)w;
	}
	return (long)done;
}

static long sys_fallocate(int fd, int mode, off_t offset, off_t len)
{
	(void)fd; (void)mode; (void)offset; (void)len;
	return 0;
}

static long sys_renameat2(int odfd, const char *op, int ndfd, const char *np, unsigned int flags)
{
	(void)flags;
	return sys_renameat(odfd, op, ndfd, np);
}

static long sys_copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                                size_t len, unsigned int flags)
{
	(void)off_in; (void)off_out; (void)flags;
	char buf[4096];
	size_t done = 0;
	while (done < len) {
		size_t chunk = len - done; if (chunk > sizeof(buf)) chunk = sizeof(buf);
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) break;
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		done += (size_t)w;
	}
	return (long)done;
}

/* ── system control & odds and ends ──────────────────────────────────────────
 * reboot/poweroff, the kernel log (syslog), resource limits (prlimit64),
 * scheduler affinity (a stub — we don't pin), prctl (mostly process name),
 * getrandom (drains the splitmix64 PRNG in syscall.c), and statx. Grouped here
 * as the miscellaneous tail of the syscall surface.
 * ───────────────────────────────────────────────────────────────────────── */

static long sys_reboot(int magic1, int magic2, int cmd, void *arg)
{
	(void)arg;
	if ((uint32_t)magic1 != 0xfee1dead) return -22;
	if ((uint32_t)magic2 != 0x28121969 &&
	    (uint32_t)magic2 != 0x05121996 &&
	    (uint32_t)magic2 != 0x16041998 &&
	    (uint32_t)magic2 != 0x20112000) return -22;
	kprintf("reboot: system going down\n");
	if (cmd == 0x4321fedc) /* LINUX_REBOOT_CMD_POWER_OFF */
		board_poweroff();
	board_reset();
	for (;;) __asm__ volatile("" ::: "memory");
	return 0;
}

static long sys_syslog(int type, char *buf, int len)
{
	if ((type == 2 || type == 3 || type == 4) && buf && len > 0) {
		uint64_t phys = (uint64_t)buddy_alloc(1);
		if (!phys) return -12;
		char *tmp = (char *)PHYS_TO_VIRT(phys);
		size_t cap = (size_t)len < 8192 ? (size_t)len : 8192;
		size_t n = kprintf_read_log(tmp, cap);
		long r = copy_to_user(buf, tmp, n) == 0 ? (long)n : -14;
		buddy_free(phys, 1);
		return r;
	}
	if (type == 10)
		return (long)kprintf_log_len();
	return 0;
}

static long sys_utimensat(int dirfd, const char *path, const void *times, int flags)
{
	(void)flags;
	vfs_inode_t *in = NULL;
	if (!path) {                                   /* futimens: dirfd is the fd */
		process_t *p = task_current();
		vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, dirfd) : NULL;
		if (!f || !f->inode) return -9;            /* EBADF */
		in = f->inode;
	} else {
		char kp[PROC_NAME_MAX];
		int r = resolve_path(kp, dirfd, path);
		if (r) return r;
		vfs_dentry_t *d = vfs_dentry_resolve(kp);
		if (!d || !d->inode) return -2;            /* ENOENT */
		in = d->inode;
	}

	int64_t now_s, now_ns; ranix_walltime(&now_s, &now_ns);
	int64_t at_s = now_s, mt_s = now_s;
	uint32_t at_n = (uint32_t)now_ns, mt_n = (uint32_t)now_ns;
	int set_at = 1, set_mt = 1;
	if (times) {
		struct { int64_t sec, nsec; } ts[2];
		if (copy_from_user(ts, times, sizeof(ts)) != 0) return -14;
		if (ts[0].nsec == 0x3FFFFFFE) set_at = 0;          /* UTIME_OMIT */
		else if (ts[0].nsec != 0x3FFFFFFF) { at_s = ts[0].sec; at_n = (uint32_t)ts[0].nsec; }
		if (ts[1].nsec == 0x3FFFFFFE) set_mt = 0;
		else if (ts[1].nsec != 0x3FFFFFFF) { mt_s = ts[1].sec; mt_n = (uint32_t)ts[1].nsec; }
	}
	if (set_at) { in->atime = at_s; in->atime_nsec = at_n; }
	if (set_mt) { in->mtime = mt_s; in->mtime_nsec = mt_n; }
	in->ctime = now_s; in->ctime_nsec = (uint32_t)now_ns;
	return 0;
}

static long sys_prlimit64(pid_t pid, int resource, const void *new_limit, void *old_limit)
{
	(void)pid; (void)new_limit;
	if (old_limit) {
		uint64_t limits[2];
		switch (resource) {
		case 3:  /* RLIMIT_STACK  */ limits[0] = limits[1] = 8UL << 20; break;
		case 7:  /* RLIMIT_NOFILE */ limits[0] = limits[1] = FD_MAX; break;
		default: limits[0] = limits[1] = (uint64_t)-1; break; /* RLIM_INFINITY */
		}
		copy_to_user(old_limit, limits, 16);
	}
	return 0;
}

static long sys_sched_yield(void) { yield(); return 0; }

static long sys_sched_setaffinity(pid_t pid, size_t len, const void *mask)
{
	(void)pid; (void)len; (void)mask;
	return 0;
}

static long sys_sched_getaffinity(pid_t pid, size_t len, void *mask)
{
	(void)pid;
	if (len >= 8 && access_ok(mask, len)) {
		extern int smp_ncpus;
		uint64_t m = (smp_ncpus < 64) ? ((1ULL << smp_ncpus) - 1) : ~0ULL;
		copy_to_user(mask, &m, 8);
		return 8;
	}
	return -22; /* EINVAL */
}

#define PR_SET_NAME    15
#define PR_GET_NAME    16
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_PDEATHSIG 1
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP 22

/* Install a SECCOMP_MODE_FILTER program from a user sock_fprog {len, filter}. */
static long seccomp_install(process_t *p, uint64_t fprog_uptr)
{
	struct { uint16_t len; uint64_t filter; } fp;
	if (copy_from_user(&fp, (void *)(uintptr_t)fprog_uptr, sizeof(fp)) != 0) return -14;
	if (fp.len == 0 || fp.len > 4096) return -22;
	size_t sz = (size_t)fp.len * 8;   /* sizeof(struct sock_filter) */
	void *prog = kmalloc(sz);
	if (!prog) return -12;
	if (copy_from_user(prog, (void *)(uintptr_t)fp.filter, sz) != 0) { kfree(prog); return -14; }
	if (p->seccomp_filter) kfree(p->seccomp_filter);
	p->seccomp_filter = prog;
	p->seccomp_flen   = fp.len;
	p->seccomp_mode   = 2;
	return 0;
}

static long sys_seccomp(unsigned int op, unsigned int flags, uint64_t args)
{
	(void)flags;
	process_t *p = task_current();
	if (op == 0) { p->seccomp_mode = 1; return 0; }   /* SECCOMP_SET_MODE_STRICT */
	if (op == 1) return seccomp_install(p, args);     /* SECCOMP_SET_MODE_FILTER */
	return -22;
}

static long sys_prctl(int option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;
	process_t *p = task_current();
	switch (option) {
	case PR_SET_NAME: {
		char name[16];
		if (strncpy_from_user(name, (const void *)(uintptr_t)arg2, 16) >= 0) {
			name[15] = '\0';
			int len = 0;
			while (name[len] && len < PROC_NAME_MAX - 1) len++;
			memcpy(p->name, name, len);
			p->name[len] = '\0';
		}
		return 0;
	}
	case PR_GET_NAME: {
		char name[16];
		memset(name, 0, sizeof(name));
		int len = 0;
		while (p->name[len] && len < 15) { name[len] = p->name[len]; len++; }
		copy_to_user((void *)(uintptr_t)arg2, name, 16);
		return 0;
	}
	case PR_SET_DUMPABLE:
		p->dumpable = (arg2 == 1) ? 1 : 0;
		return 0;
	case PR_GET_DUMPABLE:
		return p->dumpable;
	case PR_SET_SECCOMP:
		if (arg2 == 1) { p->seccomp_mode = 1; return 0; }   /* STRICT */
		if (arg2 == 2) return seccomp_install(p, arg3);     /* FILTER, arg3=sock_fprog */
		return -22;
	case PR_GET_SECCOMP:
		return p->seccomp_mode;
	default: return 0;
	}
}

static long sys_getrandom(void *buf, size_t len, unsigned int flags)
{
	(void)flags;
	if (!access_ok(buf, len)) return -14;
	uint8_t *dst = buf;
	for (size_t i = 0; i < len; ) {
		uint64_t r = rng_next();
		size_t chunk = len - i < 8 ? len - i : 8;
		for (size_t j = 0; j < chunk; j++, i++)
			dst[i] = (uint8_t)(r >> (j * 8));
	}
	return (long)len;
}

typedef struct {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __pad1;
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	int64_t  stx_atime_sec; uint32_t stx_atime_nsec; int32_t _a;
	int64_t  stx_btime_sec; uint32_t stx_btime_nsec; int32_t _b;
	int64_t  stx_ctime_sec; uint32_t stx_ctime_nsec; int32_t _c;
	int64_t  stx_mtime_sec; uint32_t stx_mtime_nsec; int32_t _m;
	uint32_t stx_rdev_major;
	uint32_t stx_rdev_minor;
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
} linux_statx_t;

static long sys_statx(int dirfd, const char *path, int flags, unsigned int mask, linux_statx_t *buf)
{
	(void)mask;
	char kp[PROC_NAME_MAX];
	vfs_stat_t ks;

	if (flags & AT_EMPTY_PATH) {
		char tmp[2];
		if (strncpy_from_user(tmp, path, 2) >= 0 && tmp[0] == '\0') {
			process_t *p = task_current();
			vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, dirfd) : NULL;
			if (!f || !f->inode) return -9; /* EBADF */
			memset(&ks, 0, sizeof(ks));
			ks.ino = f->inode->ino; ks.mode = f->inode->mode;
			ks.uid = f->inode->uid; ks.gid = f->inode->gid;
			ks.size = (uint64_t)f->inode->size; ks.nlink = f->inode->nlink;
			ks.rdev = f->inode->rdev;
			goto fill;
		}
	}
	{
		int r = resolve_path(kp, dirfd, path);
		if (r) return r;
		int sr = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kp, &ks) : vfs_stat(kp, &ks);
		if (sr < 0) return -2;
	}
fill:;

	linux_statx_t sx;
	memset(&sx, 0, sizeof(sx));
	sx.stx_mask       = 0x7FF;
	sx.stx_blksize    = 4096;
	sx.stx_nlink      = ks.nlink;
	sx.stx_uid        = ks.uid;
	sx.stx_gid        = ks.gid;
	sx.stx_mode       = (uint16_t)ks.mode;
	sx.stx_ino        = ks.ino;
	sx.stx_size       = (uint64_t)ks.size;
	sx.stx_blocks     = (ks.size + 511) / 512;
	sx.stx_dev_major  = 0;
	sx.stx_dev_minor  = 1;
	sx.stx_rdev_major = (uint32_t)(ks.rdev >> 8);
	sx.stx_rdev_minor = (uint32_t)(ks.rdev & 0xFF);
	sx.stx_atime_sec = ks.atime; sx.stx_atime_nsec = ks.atime_nsec;
	sx.stx_mtime_sec = ks.mtime; sx.stx_mtime_nsec = ks.mtime_nsec;
	sx.stx_ctime_sec = ks.ctime; sx.stx_ctime_nsec = ks.ctime_nsec;
	sx.stx_btime_sec = ks.ctime; sx.stx_btime_nsec = ks.ctime_nsec;
	return copy_to_user(buf, &sx, sizeof(sx));
}

