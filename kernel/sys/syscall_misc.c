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
	if (!hdrp) return -14;
	/* write back version = 0x20080522, pid = 0 */
	uint32_t hdr[2] = { 0x20080522, 0 };
	if (access_ok(hdrp, 8)) copy_to_user(hdrp, hdr, 8);
	if (datap && access_ok(datap, 24)) {
		/* two sets (effective, permitted, inheritable) x2 (32-bit halves) */
		uint32_t data[6] = { 0xffffffff, 0xffffffff, 0xffffffff,
		                     0xffffffff, 0xffffffff, 0xffffffff };
		copy_to_user(datap, data, 24);
	}
	return 0;
}
static long sys_capset(void *hdrp, const void *datap)
{ (void)hdrp; (void)datap; return 0; }

/* ── xattr (stubs: ENODATA) ──────────────────────────────── */
static long sys_getxattr(const char *p, const char *n, void *v, size_t s)
{ (void)p;(void)n;(void)v;(void)s; return -61; } /* ENODATA */
static long sys_lgetxattr(const char *p, const char *n, void *v, size_t s)
{ (void)p;(void)n;(void)v;(void)s; return -61; }
static long sys_fgetxattr(int fd, const char *n, void *v, size_t s)
{ (void)fd;(void)n;(void)v;(void)s; return -61; }
static long sys_setxattr(const char *p, const char *n, const void *v, size_t s, int f)
{ (void)p;(void)n;(void)v;(void)s;(void)f; return -1; }
static long sys_lsetxattr(const char *p, const char *n, const void *v, size_t s, int f)
{ (void)p;(void)n;(void)v;(void)s;(void)f; return -1; }
static long sys_listxattr(const char *p, char *l, size_t s)
{ (void)p;(void)l;(void)s; return 0; }
static long sys_llistxattr(const char *p, char *l, size_t s)
{ (void)p;(void)l;(void)s; return 0; }
static long sys_removexattr(const char *p, const char *n)
{ (void)p;(void)n; return -1; }

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
	task_sleep();
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
	(void)fd; (void)operation;
	return 0;
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
	if (ruid != (uid_t)-1) p->uid = ruid;
	if (euid != (uid_t)-1) p->uid = euid;
	return 0;
}

static long sys_setregid(gid_t rgid, gid_t egid)
{
	process_t *p = task_current();
	if (rgid != (gid_t)-1) p->gid = rgid;
	if (egid != (gid_t)-1) p->gid = egid;
	return 0;
}

static long sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	process_t *p = task_current();
	if (ruid != (uid_t)-1) p->uid = ruid;
	if (euid != (uid_t)-1) p->uid = euid;
	(void)suid;
	return 0;
}

static long sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	uid_t u = task_current()->uid;
	if (copy_to_user(ruid, &u, sizeof(u))) return -14;
	if (copy_to_user(euid, &u, sizeof(u))) return -14;
	if (copy_to_user(suid, &u, sizeof(u))) return -14;
	return 0;
}

static long sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
	process_t *p = task_current();
	if (rgid != (gid_t)-1) p->gid = rgid;
	if (egid != (gid_t)-1) p->gid = egid;
	(void)sgid;
	return 0;
}

static long sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	gid_t g = task_current()->gid;
	if (copy_to_user(rgid, &g, sizeof(g))) return -14;
	if (copy_to_user(egid, &g, sizeof(g))) return -14;
	if (copy_to_user(sgid, &g, sizeof(g))) return -14;
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
	task_sleep();
	p->sig_mask = oldmask;
	return -4; /* EINTR */
}

static long sys_mknod(const char *path, mode_t mode, uint64_t dev)
{
	(void)dev;
	if ((mode & 0xF000) == 0x8000 /* S_IFREG */)
		return sys_open(path, 0101 | 01000, mode & 0777);
	return -1; /* EPERM for device nodes */
}

static long sys_mknodat(int dirfd, const char *path, mode_t mode, uint64_t dev)
{
	char kpath[256];
	if (resolve_path(kpath, dirfd, path) != 0) return -2;
	return sys_mknod(kpath, mode, dev);
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

static long sys_sethostname(const char *name, size_t len)
{
	if (len >= sizeof(ranix_hostname)) return -22;
	if (!access_ok(name, len)) return -14;
	if (copy_from_user(ranix_hostname, name, len) != 0) return -14;
	ranix_hostname[len] = '\0';
	return 0;
}

static long sys_tkill(pid_t tid, int sig) { return sys_kill(tid, sig); }

static long sys_time_syscall(long *tloc)
{
	long t = (long)(kernel_ticks / TIMER_HZ);
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

