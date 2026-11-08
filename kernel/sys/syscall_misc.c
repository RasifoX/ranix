#include <kernel/pid_namespace.h>
#include <kernel/capabilities.h>
#include <kernel/block.h>
#include <kernel/cpumask.h>

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

/* filesystem-type → statfs f_type magic (Linux values, what programs branch on) */
static long fstype_to_magic(const char *fs)
{
	if (!fs) return 0x858458f6;
	if (!strcmp(fs, "proc"))     return 0x9fa0;
	if (!strcmp(fs, "sysfs"))    return 0x62656572;
	if (!strcmp(fs, "ext2") || !strcmp(fs, "ext4")) return 0xEF53;
	if (!strcmp(fs, "vfat") || !strcmp(fs, "fat"))  return 0x4d44;
	if (!strcmp(fs, "iso9660"))  return 0x9660;
	if (!strcmp(fs, "exfat"))    return 0x2011BAB0;
	if (!strcmp(fs, "ntfs"))     return 0x5346544e;
	if (!strcmp(fs, "tmpfs"))    return 0x01021994;
	if (!strcmp(fs, "cgroup2"))  return 0x63677270;
	if (!strcmp(fs, "devfs") || !strcmp(fs, "devtmpfs")) return 0x1cd1;
	return 0x858458f6; /* RAMFS_MAGIC */
}

static void fill_statfs(kernel_statfs_t *st, const char *fs)
{
	memset(st, 0, sizeof(*st));
	st->f_type    = fstype_to_magic(fs);
	st->f_bsize   = 4096;
	st->f_blocks  = 1024;          /* block counts are still nominal (no per-fs statfs op yet) */
	st->f_bfree   = 512;
	st->f_bavail  = 512;
	st->f_files   = 1024;
	st->f_ffree   = 512;
	st->f_namelen = 255;
}

/* overlay the nominal counts with a filesystem's real statfs op, if it has one */
static void apply_fs_statfs(kernel_statfs_t *st, const vfs_statfs_t *vs)
{
	st->f_bsize  = vs->bsize ? (long)vs->bsize : st->f_bsize;
	st->f_frsize = st->f_bsize;
	st->f_blocks = vs->blocks;
	st->f_bfree  = vs->bfree;
	st->f_bavail = vs->bavail;
	st->f_files  = vs->files;
	st->f_ffree  = vs->ffree;
	if (vs->namelen) st->f_namelen = vs->namelen;
}

static long sys_statfs(const char *path, kernel_statfs_t *buf)
{
	if (!access_ok(buf, sizeof(*buf))) return -14;
	char kp[PATH_MAX];
	const char *fs = "ramfs";
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	vfs_stat_t ls;
	if (vfs_stat(kp, &ls) != 0) return -2;
	fs = vfs_path_fstype(kp);
	kernel_statfs_t st;
	fill_statfs(&st, fs);
	vfs_statfs_t vs;
	if (vfs_statfs_path(kp, &vs) == 0) apply_fs_statfs(&st, &vs);
	if (task_current()->walk_denied) return -2;
	return copy_to_user(buf, &st, sizeof(st)) ? -14 : 0;
}

static long sys_fstatfs(int fd, kernel_statfs_t *buf)
{
	if (!access_ok(buf, sizeof(*buf))) return -14;
	const char *fs = "ramfs";
	vfs_file_t *f = fget(task_current()->fds, fd);
	if (!f) return -9;
	if (f->path[0] == '/') fs = vfs_path_fstype(f->path);
	kernel_statfs_t st;
	fill_statfs(&st, fs);
	if (f->inode) {
		vfs_statfs_t vs;
		if (vfs_statfs_inode(f->inode, &vs) == 0) apply_fs_statfs(&st, &vs);
	}
	fput(f);
	return copy_to_user(buf, &st, sizeof(st)) ? -14 : 0;
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

static vfs_inode_t *xattr_resolve(const char *upath, char *kpath, int *ro)
{
	if (resolve_path(kpath, AT_FDCWD, upath)) return NULL;
	vfs_dentry_t *d = vfs_dentry_resolve(kpath);
	if (d && ro) *ro = vfs_dentry_readonly(d);
	return (d && d->inode) ? d->inode : NULL;
}

static long xattr_set(const char *upath, const char *uname, const void *uval,
                      size_t size, int flags)
{
	char kpath[PATH_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;
	int ns = xattr_ns(kname);
	if (!ns) return -95; /* EOPNOTSUPP */
	process_t *cp = task_current();
	if (ns == 2 && (!cp || cp->euid != 0)) return -1; /* EPERM */
	if (size > XATTR_VALUE_MAX) return -7; /* E2BIG */
	int ro = 0;
	vfs_inode_t *in = xattr_resolve(upath, kpath, &ro);
	if (!in) return -2;
	if (ro) return -30; /* EROFS */
	if (ns == 1 && !S_ISREG(in->mode) && !S_ISDIR(in->mode)) return -95;
	void *kv = NULL;
	if (size) {
		kv = kmalloc(size);
		if (!kv) return -12;
		if (copy_from_user(kv, uval, size) != 0) { kfree(kv); return -14; }
	}
	vfs_iattr_t a = { .valid = ATTR_XATTR_SET, .xname = kname, .xval = kv, .xlen = size, .xflags = flags };
	int r = vfs_inode_setattr(in, &a);
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
	char kpath[PATH_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;
	vfs_inode_t *in = xattr_resolve(upath, kpath, NULL);
	if (!in) return -2;
	return xattr_get_inode(in, kname, uval, size);
}

static long xattr_list(const char *upath, char *ulist, size_t size)
{
	char kpath[PATH_MAX];
	vfs_inode_t *in = xattr_resolve(upath, kpath, NULL);
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
	char kpath[PATH_MAX], kname[XATTR_NAME_MAX_LEN + 1];
	if (strncpy_from_user(kname, n, sizeof(kname)) < 0) return -14;
	int ro = 0;
	vfs_inode_t *in = xattr_resolve(p, kpath, &ro);
	if (!in) return -2;
	if (ro) return -30; /* EROFS */
	vfs_iattr_t a = { .valid = ATTR_XATTR_REMOVE, .xname = kname };
	return vfs_inode_setattr(in, &a);
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
	if (offset < 0) return -22;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return saved;
	vfs_seek(fd, offset, 0);
	long n = vfs_read(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_pwrite64(int fd, const void *buf, size_t count, off_t offset)
{
	if (!access_ok(buf, count)) return -14;
	if (offset < 0) return -22;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return saved;
	vfs_seek(fd, offset, 0);
	long n = vfs_write(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size,
                       int flags, uint64_t new_addr)
{
	(void)new_addr;
	process_t *p = task_current();
	if (new_size == 0 || (old_addr & (PAGE_SIZE - 1))) return -22;
	old_size = (old_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
	new_size = (new_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
	if (new_size == old_size) return (long)old_addr;
	if (new_size < old_size) {
		sys_munmap(old_addr + new_size, old_size - new_size);
		return (long)old_addr;
	}
	/* Without MREMAP_MAYMOVE the mapping stays where it is. musl walks down the
	 * main stack this way, reading ENOMEM as "mapped" and EFAULT as "the end". */
	if (!(flags & 1)) {
		extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
		/* the probe below walks page by page and cannot be interrupted */
		if (new_size - old_size > (1UL << 30)) return -12;
		vma_t *v = p->vm->vmas;
		while (v && !(old_addr >= v->vaddr && old_addr < v->vaddr + v->size)) v = v->next;
		if (!v && !arch_vm_get_paddr(p->vm->root, old_addr)) return -14; /* EFAULT */
		for (uint64_t a = old_addr + old_size; a < old_addr + new_size; a += PAGE_SIZE) {
			if (arch_vm_get_paddr(p->vm->root, a)) return -12;
			for (vma_t *n = p->vm->vmas; n; n = n->next)
				if (a >= n->vaddr && a < n->vaddr + n->size) return -12;
		}
		if (!v || !(v->flags & VMA_ANON) || (v->flags & VMA_SHARED)) return -12;
		int prot = (v->flags & VMA_READ ? PROT_READ : 0) | (v->flags & VMA_WRITE ? PROT_WRITE : 0) |
		           (v->flags & VMA_EXEC ? PROT_EXEC : 0);
		long r = sys_mmap(old_addr + old_size, new_size - old_size, prot,
		                  0x32 /*MAP_ANON|MAP_PRIVATE|MAP_FIXED*/, -1, 0);
		return r < 0 ? r : (long)old_addr;
	}
	/* growing: allocate new, copy, unmap old */
	long na = sys_mmap(0, new_size, 3 /*RW*/, 0x22 /*MAP_ANON|MAP_PRIVATE*/, -1, 0);
	if (na < 0) return -12;
	/* Through a buffer with fault fixups: the destination is demand-zero, and a
	 * fault refused by memory.max must not land in a bare kernel copy. */
	char *b = kmalloc(PAGE_SIZE);
	if (!b) { sys_munmap((uint64_t)na, new_size); return -12; }
	long r = na;
	for (size_t off = 0; off < old_size && r == na; off += PAGE_SIZE) {
		size_t n = old_size - off < PAGE_SIZE ? old_size - off : PAGE_SIZE;
		if (copy_from_user(b, (const void *)(old_addr + off), n) != 0) r = -14;
		else if (copy_to_user((void *)(na + off), b, n) != 0) r = -12;
	}
	kfree(b);
	sys_munmap(r == na ? old_addr : (uint64_t)na, r == na ? old_size : new_size);
	return r;
}

static long sys_pause(void)
{
	process_t *p = task_current();
	while (!(p->sig_pending & ~p->sig_mask))
		task_sleep_if(!(p->sig_pending & ~p->sig_mask));
	return -4; /* EINTR */
}

static long sys_alarm(unsigned int seconds)
{
	process_t *p = task_current();
	/* read-and-replace under sched_lock so it can't race sched_tick's decrement
	 * (a nested tick on aarch64, or a cross-CPU tick) and report/leave a torn value. */
	extern spinlock_t sched_lock;
	uint64_t sf = spin_lock_irq(&sched_lock);
	unsigned int remaining = (unsigned int)(p->itimer_value / TIMER_HZ);
	uint64_t nv = (uint64_t)seconds * TIMER_HZ;
	sched_itimer_account(p->itimer_value, nv);
	p->itimer_value    = nv;
	p->itimer_interval = 0;
	spin_unlock_irq(&sched_lock, sf);
	return (long)remaining;
}

static long sys_socketpair(int domain, int type, int protocol, int *sv)
{
    (void)protocol;
    int sflags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    type &= SOCK_TYPE_MASK;
    if (domain != AF_UNIX) return -97;
    int seqpacket = type == 5;
    if (type == 5 /* SOCK_SEQPACKET */) type = SOCK_DGRAM;   /* a pair keeps its boundaries */
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -22;

    socket_t *s0 = sock_alloc(AF_UNIX, type, 0);
    socket_t *s1 = sock_alloc(AF_UNIX, type, 0);
    if (!s0 || !s1 || sock_own(s0) || sock_own(s1)) { sock_free(s0); sock_free(s1); return -12; }

    s0->unix_peer = s1;
    s1->unix_peer = s0;
    {   /* SO_PEERCRED: both ends belong to the caller */
        process_t *me = task_current();
        s0->pc_set = s1->pc_set = 1;
        s0->pc_pid = s1->pc_pid = (int32_t)me->tgid;
        s0->pc_uid = s1->pc_uid = me->euid;
        s0->pc_gid = s1->pc_gid = me->egid;
    }
    s0->unix_seqpacket = s1->unix_seqpacket = (uint8_t)seqpacket;
    s0->tcp_state = TCP_ESTABLISHED;
    s1->tcp_state = TCP_ESTABLISHED;

    int fd0 = sock_to_fd(s0, sflags);
    if (fd0 < 0) { sock_free(s0); sock_free(s1); return fd0; }
    int fd1 = sock_to_fd(s1, sflags);
    if (fd1 < 0) {
        /* fd0 already allocated; free s1, let fd0 cleanup handle s0 */
        sock_free(s1); s0->unix_peer = NULL;
        return fd1;
    }

    int kbuf[2] = { fd0, fd1 };
    if (copy_to_user(sv, kbuf, sizeof(kbuf)) != 0) {
        vfs_close(fd0);
        vfs_close(fd1);
        return -14;
    }
    return 0;
}

static long sys_flock(int fd, int operation)
{
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
	process_t *p = task_current();
	vfs_file_t *f = (p && p->fds) ? fget(p->fds, fd) : NULL;  /* pin across the wait loop */
	if (!f) return -9; /* EBADF */
	int cmd = operation & ~LOCK_NB;
	if (cmd == LOCK_UN) { vfs_flock_release(f); fput(f); return 0; }
	if (cmd != LOCK_SH && cmd != LOCK_EX) { fput(f); return -22; } /* EINVAL */
	for (;;) {
		int r = vfs_flock_try(f, cmd);
		if (r == 0) { fput(f); return 0; }
		if (operation & LOCK_NB) { fput(f); return -11; } /* EWOULDBLOCK */
		yield();                              /* block: retry when holder releases */
		if (task_has_actionable_signal()) { fput(f); return -4; } /* EINTR */
	}
}

static long sys_fsync(int fd)
{
	process_t *p = task_current();
	vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, fd) : NULL;
	if (!f) return -9;
	int r = 0;
#if IS_ENABLED(CONFIG_FS_EXT2)
	r = ext2_sync_all();
#endif
	int b = block_flush_all();
	return r ? r : b;
}
static long sys_fdatasync(int fd) { return sys_fsync(fd); }

/* sync_file_range(fd, offset, nbytes, flags): a writeback hint for a byte range.
 * Ranix-backed files live in RAM with no deferred writeback, so initiating it is
 * a no-op that completes immediately. Validate the fd like fsync. */
static long sys_sync_file_range(int fd, off_t offset, off_t nbytes, unsigned int flags)
{
	(void)offset; (void)nbytes; (void)flags;
	return sys_fsync(fd);
}

static long sys_fchdir(int fd)
{
	process_t *p = task_current();
	if (!p->fds) return -9;
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	if (!f->inode || (f->inode->mode & 0xF000) != 0x4000) return -20; /* ENOTDIR */
	if (vfs_may_enter(f->inode)) return -13;   /* EACCES, as chdir() */
	const char *fp = f->lpath ? f->lpath : f->path;   /* lpath: too long for path[] */
	if (fp[0] != '/') return -20;
	size_t fl = strlen(fp);
	if (fl >= sizeof(p->cwd)) return -36; /* ENAMETOOLONG */
	memcpy(p->cwd, fp, fl + 1);
	return 0;
}

static long sys_creat(const char *path, mode_t mode)
{
	return sys_open(path, 0101 /* O_CREAT|O_WRONLY */ | 01000 /* O_TRUNC */, mode);
}

static long sys_lchown(const char *path, uid_t uid, gid_t gid)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	return vfs_lchown(kp, uid, gid);
}

static long sys_getrusage(int who, void *usage)
{
	if (!usage) return -14;
	process_t *p = task_current();
	uint64_t ut, st;
	if (who != 0 && who != -1 && who != 1) return -22;
	if (who == -1 /* RUSAGE_CHILDREN */) { ut = p->cutime_ticks; st = p->cstime_ticks; }
	else                                  { ut = p->utime_ticks;  st = p->stime_ticks; }  /* SELF / THREAD */
	int64_t r[18];   /* struct rusage = 18 longs (144 bytes) */
	memset(r, 0, sizeof(r));
	uint64_t hz = TIMER_HZ;
	r[0] = (int64_t)(ut / hz);                       /* ru_utime.tv_sec  */
	r[1] = (int64_t)((ut % hz) * (1000000 / hz));    /* ru_utime.tv_usec */
	r[2] = (int64_t)(st / hz);                       /* ru_stime.tv_sec  */
	r[3] = (int64_t)((st % hz) * (1000000 / hz));    /* ru_stime.tv_usec */
	if (copy_to_user(usage, r, sizeof(r)) != 0) return -14;
	return 0;
}

static long sys_setreuid(uid_t ruid, uid_t euid)
{
	process_t *p = task_current();
	uid_t old_euid = p->euid;
	int was_priv = (p->uid == 0 || p->euid == 0 || p->suid == 0);
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
	if (ruid != (uid_t)-1 || (euid != (uid_t)-1 && euid != old_euid))
		p->suid = new_e;
	cred_setxuid_fixup(p, was_priv, old_euid);
	return 0;
}

static long sys_setregid(gid_t rgid, gid_t egid)
{
	process_t *p = task_current();
	gid_t new_r = (rgid != (gid_t)-1) ? rgid : p->gid;
	gid_t new_e = (egid != (gid_t)-1) ? egid : p->egid;
	gid_t old_e = p->egid;
	if (p->egid != 0) {
		if (new_r != p->gid && new_r != p->egid && new_r != p->sgid) return -1;
		if (new_e != p->gid && new_e != p->egid && new_e != p->sgid) return -1;
	}
	p->gid  = new_r;
	p->egid = new_e;
	if (rgid != (gid_t)-1 || (egid != (gid_t)-1 && egid != old_e))
		p->sgid = new_e;
	return 0;
}

static long sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	process_t *p = task_current();
	uid_t old_euid = p->euid;
	int was_priv = (p->uid == 0 || p->euid == 0 || p->suid == 0);
	uid_t new_r = (ruid != (uid_t)-1) ? ruid : p->uid;
	uid_t new_e = (euid != (uid_t)-1) ? euid : p->euid;
	uid_t new_s = (suid != (uid_t)-1) ? suid : p->suid;
	if (p->euid != 0) {
		if (new_r != p->uid && new_r != p->euid && new_r != p->suid) return -1;
		if (new_e != p->uid && new_e != p->euid && new_e != p->suid) return -1;
		if (new_s != p->uid && new_s != p->euid && new_s != p->suid) return -1;
	}
	p->uid = new_r; p->euid = new_e; p->suid = new_s;
	cred_setxuid_fixup(p, was_priv, old_euid);
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

/* fsuid/fsgid: the credential used for filesystem access checks. An unprivileged
 * process may only set it to one of ruid/euid/suid (or fsuid); root anything.
 * Returns the *previous* fsuid, which is euid until first overridden. */
static long sys_setfsuid(uid_t fsuid)
{
	process_t *p = task_current();
	uid_t prev = p->fs_uid_set ? p->fsuid : p->euid;
	if (fsuid == p->uid || fsuid == p->euid || fsuid == p->suid ||
	    (p->fs_uid_set && fsuid == p->fsuid) || p->euid == 0) {
		p->fsuid = fsuid;
		p->fs_uid_set = 1;
	}
	return (long)prev;
}

static long sys_setfsgid(gid_t fsgid)
{
	process_t *p = task_current();
	gid_t prev = p->fs_gid_set ? p->fsgid : p->egid;
	if (fsgid == p->gid || fsgid == p->egid || fsgid == p->sgid ||
	    (p->fs_gid_set && fsgid == p->fsgid) || p->euid == 0) {
		p->fsgid = fsgid;
		p->fs_gid_set = 1;
	}
	return (long)prev;
}

#define PER_LINUX        0x0000
#define ADDR_NO_RANDOMIZE 0x0040000
#define PER_MASK         0x00ffffff
/* personality(2): a process can read its current execution domain by passing
 * 0xffffffff, or set a new one. Only ADDR_NO_RANDOMIZE has a cheap effect here
 * (it disables ASLR for the next exec); the rest is stored and echoed back. */
static long sys_personality(unsigned int persona)
{
	process_t *p = task_current();
	uint32_t old = p->personality;
	if (persona != 0xffffffffu)
		p->personality = persona;
	return (long)old;
}

/* getcpu(2): which CPU the caller is running on, and its NUMA node (always 0). */
static long sys_getcpu(unsigned int *cpu, unsigned int *node, void *tcache)
{
	(void)tcache;
	unsigned int c = (unsigned int)arch_get_cpu_ctx()->cpu_id;
	unsigned int n = 0;
	if (cpu  && copy_to_user(cpu,  &c, sizeof(c))) return -14;
	if (node && copy_to_user(node, &n, sizeof(n))) return -14;
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
	p->sig_saved_mask = p->sig_mask;
	p->sig_mask = newmask & ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	p->sig_mask_saved = 1;   /* the handler must run under newmask */
	while (!(p->sig_pending & ~p->sig_mask))
		task_sleep_if(!(p->sig_pending & ~p->sig_mask));
	return -4; /* EINTR */
}

/* kp is a resolved kernel-absolute path. dev = major:minor for char/block. */
static long do_mknod(const char *kp, mode_t mode, uint64_t dev)
{
	int type = mode & 0xF000;
	if (type == 0x1000 /* S_IFIFO */) return vfs_mkfifo(kp, mode);
	if (type == 0x2000 /* S_IFCHR */ || type == 0x6000 /* S_IFBLK */)
		return vfs_mknod(kp, mode, (dev_t)dev);
	if (type == 0 || type == 0x8000 /* S_IFREG */) {
		int fd = vfs_open(kp, 0x41 /*O_CREAT|O_WRONLY*/, mode & 0777);
		if (fd < 0) return fd;
		vfs_close(fd);
		return 0;
	}
	return -22; /* S_IFSOCK etc.: not supported */
}

static long sys_mknod(const char *path, mode_t mode, uint64_t dev)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	return do_mknod(kp, mode, dev);
}

static long sys_mknodat(int dirfd, const char *path, mode_t mode, uint64_t dev)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return do_mknod(kp, mode, dev);
}

static long sys_sync(void)
{
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_sync_all();
#endif
	block_flush_all();
	return 0;
}

static long sys_mount(const char *source, const char *target, const char *fstype,
                      unsigned long flags, const void *data)
{
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1; /* EPERM */
	char ktarget[PATH_MAX]; char kfstype[64];
	int tr = resolve_path(ktarget, AT_FDCWD, target);
	if (tr) return tr;

	if (flags & MS_REMOUNT)
		return vfs_remount(ktarget, flags) == 0 ? 0 : -22;

	/* mount --bind DIR TARGET: the same tree seen at a second place, as chroots and
	 * containers build their /dev, /home and sockets. Submounts are not carried
	 * (MS_REC is taken as plain), and a single file cannot be bound yet. */
	if (flags & 0x1000 /* MS_BIND */) {
		char ksrc[PATH_MAX];
		int sr = resolve_path(ksrc, AT_FDCWD, source);
		if (sr) return sr;
		vfs_dentry_t *src = vfs_dentry_resolve(ksrc);
		if (!src || !src->inode || vfs_dentry_is_volatile(src)) return -2;
		vfs_dentry_t *mp = vfs_dentry_resolve(ktarget);
		if (!mp || !mp->inode || vfs_dentry_is_volatile(mp)) return -2;
		if (!S_ISDIR(src->inode->mode)) return -22;
		if (!S_ISDIR(mp->inode->mode)) return -20;          /* ENOTDIR */
		unsigned long bf = flags & (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC);
		return vfs_bind(current_mnt_ns(), mp, src, ktarget, bf);
	}

	if (strncpy_from_user(kfstype, fstype, sizeof(kfstype)) < 0) return -14;
	unsigned long mflags = flags & (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC);

	/* sysfs/cgroup/devtmpfs etc. → accept silently */
	const char *fsname = kfstype;
	if (strcmp(kfstype, "cgroup") == 0)  return 0;
	if (strcmp(kfstype, "mqueue") == 0)  return 0;
	if (strcmp(kfstype, "hugetlbfs") == 0) return 0;
	if (strcmp(kfstype, "binfmt_misc") == 0) return 0;
	if (strcmp(kfstype, "pstore") == 0)  return 0;
	if (strcmp(kfstype, "efivarfs") == 0) return 0;
	if (strcmp(kfstype, "securityfs") == 0) return 0;
	if (strcmp(kfstype, "configfs") == 0) return 0;

	/* block-backed filesystems take a block_device as the fill data; resolve
	 * source ("/dev/usb0") to its registered device. */
	if (strcmp(fsname, "ext2") == 0 || strcmp(fsname, "ext4") == 0 ||
	    strcmp(fsname, "vfat") == 0 || strcmp(fsname, "fat") == 0 || strcmp(fsname, "msdos") == 0 ||
	    strcmp(fsname, "exfat") == 0 || strcmp(fsname, "iso9660") == 0 || strcmp(fsname, "iso") == 0 ||
	    strcmp(fsname, "ntfs") == 0) {
		char ksrc[256];
		if (!source || strncpy_from_user(ksrc, source, sizeof(ksrc)) < 0) return -14;
		const char *name = ksrc;
		for (const char *p = ksrc; *p; p++)
			if (*p == '/') name = p + 1;
		block_device_t *bd = block_device_find(name);
		if (bd && bd->kernel_only) bd = NULL;
		if (!bd) return -19;   /* ENODEV */
		/* -o offset=BYTES: a filesystem further into the device, as losetup -o
		 * would give. A device of its own, so the bytes before it stay out of reach. */
		char kopt[32];
		if (data && strncpy_from_user(kopt, data, sizeof(kopt)) >= 0 &&
		    strncmp(kopt, "offset=", 7) == 0) {
			static int slices;
			uint64_t off = 0;
			for (const char *p = kopt + 7; *p >= '0' && *p <= '9'; p++) off = off * 10 + (uint64_t)(*p - '0');
			uint64_t lba = off / BLOCK_SIZE;
			if (!off || (off % BLOCK_SIZE) || lba >= bd->size_blocks || slices >= 99) return -22;
			bd = block_partition_add(bd, lba, bd->size_blocks - lba, ++slices, NULL);
			if (!bd) return -12;
		}
		return vfs_mount_flags(ktarget, fsname, bd, mflags) == 0 ? 0 : -22;
	}

	/* mount options string (tmpfs size=, overlay lowerdir/upperdir/workdir).
	 * The fill callback consumes it synchronously, so a stack copy suffices. */
	char kdata[256];
	const char *kdp = NULL;
	if (data && strncpy_from_user(kdata, data, sizeof(kdata)) >= 0) kdp = kdata;

	int r = vfs_mount_flags(ktarget, fsname, (void *)kdp, mflags);
	return r == 0 ? 0 : -22;
}

static long sys_umount2(const char *target, int flags)
{
	(void)flags;
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1; /* EPERM */
	char kpath[PATH_MAX];
	int r = resolve_path(kpath, AT_FDCWD, target);
	if (r) return r;
	r = vfs_umount(kpath);
	return r == 0 || r == -16 ? r : -22;
}

static long sys_sethostname(const char *name, size_t len)
{
	uts_namespace_t *ns = current_uts();
	/* The name belongs to the caller's UTS namespace: a private one needs only
	 * the namespace-local capability. */
	process_t *cur = task_current();
	if (!capable_task(cur, CAP_SYS_ADMIN) &&
	    !(cap_has(cur->cap_effective, CAP_SYS_ADMIN) && cur->uts_ns && cur->uts_ns != &init_uts_ns))
		return -1; /* EPERM */
	if (len >= sizeof(ns->nodename)) return -22;
	if (!access_ok(name, len)) return -14;
	if (copy_from_user(ns->nodename, name, len) != 0) return -14;
	ns->nodename[len] = '\0';
	return 0;
}

static long sys_time_syscall(long *tloc)
{
	int64_t s, n; ranix_walltime(&s, &n);   /* real wall-clock (RTC-based) */
	long t = (long)s;
	if (tloc && copy_to_user(tloc, &t, sizeof(t)) != 0) return -14;
	return t;
}

static long sys_clock_nanosleep(int clkid, int flags,
                                linux_timespec_t *req, linux_timespec_t *rem)
{
	if (flags & ~1) return -22;
	linux_timespec_t k, now;
	if (!req) return -14;
	if (copy_from_user(&k, req, sizeof(k)) != 0) return -14;
	if (k.tv_sec < 0 || k.tv_nsec < 0 || k.tv_nsec >= 1000000000L) return -22;
	int nowr = 0;
	if (clkid == CLOCK_REALTIME || clkid == CLOCK_TAI) real_time(&now.tv_sec, &now.tv_nsec);
	else if (clkid == CLOCK_MONOTONIC || clkid == CLOCK_BOOTTIME) mono_time(&now.tv_sec, &now.tv_nsec);
	else nowr = -22;
	if (nowr) return nowr;
	if (flags & 1) {                 /* TIMER_ABSTIME: an instant, not a span */
		k.tv_sec  -= now.tv_sec;
		k.tv_nsec -= now.tv_nsec;
		if (k.tv_nsec < 0) { k.tv_sec--; k.tv_nsec += 1000000000L; }
		if (k.tv_sec < 0) return 0;  /* already past */
		rem = NULL;                  /* an instant needs no remainder */
	}
	return sleep_for(&k, rem);
}

static long sys_waitid(int idtype, int id, void *infop, int options, void *rusage)
{
	if (idtype < 0 || idtype > 2) return -22;
	pid_t wpid = (idtype == 0) ? -1 : (idtype == 1) ? (pid_t)id : -(pid_t)id;
	int status = 0;
	long r = do_waitpid(wpid, &status, options, rusage);
	if (r < 0) return r;
	if (infop) {
		/* siginfo_t for SIGCHLD; si_pid stays 0 when WNOHANG found nothing */
		int32_t si[8];
		memset(si, 0, sizeof(si));
		if (r) {
			si[0] = SIGCHLD;
			si[4] = (int32_t)r;
			si[2] = (status & 0x7f) ? 2 /* CLD_KILLED */ : 1 /* CLD_EXITED */;
			si[6] = (status & 0x7f) ? (status & 0x7f) : ((status >> 8) & 0xff);
		}
		if (copy_to_user(infop, si, sizeof(si)) != 0) return -14;
	}
	return 0;
}

static long sys_splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                       size_t len, unsigned int flags)
{
	(void)flags;
	char buf[4096];
	size_t done = 0;
	off_t oin = 0, oout = 0;
	if (off_in  && copy_from_user(&oin,  off_in,  sizeof(oin))  != 0) return -14;
	if (off_out && copy_from_user(&oout, off_out, sizeof(oout)) != 0) return -14;
	if (oin < 0 || oout < 0) return -22;
	/* an endless source and sink must stay killable */
	if (len > 0x7ffff000UL) len = 0x7ffff000UL;
	while (done < len) {
		if (task_has_actionable_signal()) { if (done == 0) return -4; break; }
		size_t chunk = len - done;
		if (chunk > sizeof(buf)) chunk = sizeof(buf);
		if (off_in) vfs_seek(fd_in, oin, 0);
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) { if (done == 0) return r; break; }
		oin += r;
		if (off_out) vfs_seek(fd_out, oout, 0);
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		oout += w;
		done += (size_t)w;
		if (w < r) break;
	}
	if (off_in  && copy_to_user(off_in,  &oin,  sizeof(oin))  != 0) return -14;
	if (off_out && copy_to_user(off_out, &oout, sizeof(oout)) != 0) return -14;
	return (long)done;
}

#define FALLOC_FL_KEEP_SIZE  1
#define FALLOC_FL_PUNCH_HOLE 2

static long sys_fallocate(int fd, int mode, off_t offset, off_t len)
{
	if (offset < 0 || len <= 0) return -22;        /* EINVAL */
	/* hole-punch / collapse / zero / insert: no block-level support */
	if (mode & ~FALLOC_FL_KEEP_SIZE) return -95;   /* EOPNOTSUPP */

	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f || !f->inode) return -9;                /* EBADF */
	if ((f->flags & 3) == O_RDONLY) return -9;     /* EBADF: not open for writing */
	vfs_inode_t *in = f->inode;
	if (S_ISDIR(in->mode)) return -21;             /* EISDIR */
	if (!S_ISREG(in->mode)) return -9;             /* ESPIPE-ish; Linux: EBADF/ENODEV */
	if (!in->iops || !in->iops->truncate) return -95;

	/* KEEP_SIZE only reserves blocks (no ramfs concept) → honest success. */
	if (mode & FALLOC_FL_KEEP_SIZE) return 0;

	uint64_t need = (uint64_t)offset + (uint64_t)len;
	if (need > 0x7fffffffffffffffUL) return -27;   /* EFBIG */
	if (need <= in->size) return 0;                /* already covered */
	return in->iops->truncate(in, (off_t)need);    /* grows + zero-fills */
}

#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
#define RENAME_WHITEOUT  4

static long sys_renameat2(int odfd, const char *op, int ndfd, const char *np, unsigned int flags)
{
	if (flags & ~(unsigned)(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT)) return -22;
	if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) return -22;
	if (flags & RENAME_WHITEOUT) return -95;   /* EOPNOTSUPP: no overlay whiteouts */
	if (!flags) return sys_renameat(odfd, op, ndfd, np);

	char *ko = kmalloc(PATH_MAX);
	char *kn = kmalloc(PATH_MAX);
	char *tmp = kmalloc(PATH_MAX);
	long ret;
	int r;
	vfs_stat_t st = {0}, sst = {0};
	int dst_exists;
	if (!ko || !kn || !tmp) { ret = -12; goto out; }

	if ((r = resolve_path(ko, odfd, op)) || (r = resolve_path(kn, ndfd, np))) {
		ret = r; goto out;
	}

	dst_exists = (vfs_stat(kn, &st) == 0);

	if (flags & RENAME_NOREPLACE) {
		if (dst_exists) ret = -17;                  /* EEXIST */
		else            ret = vfs_rename(ko, kn);
		goto out;
	}

	/* RENAME_EXCHANGE: both must exist; swap via a temporary name. Not a single
	 * atomic step (no fs primitive), but the data of neither side is lost. */
	if (vfs_stat(ko, &sst) != 0 || !dst_exists) { ret = -2; goto out; }

	int tn = 0;
	for (const char *c = kn; *c && tn < PATH_MAX - 16; c++) tmp[tn++] = *c;
	for (const char *c = ".rx-exch~"; *c; c++) tmp[tn++] = *c;
	tmp[tn] = '\0';

	if (vfs_rename(kn, tmp) != 0)      { ret = -2; goto out; }
	if (vfs_rename(ko, kn) != 0)       { vfs_rename(tmp, kn); ret = -2; goto out; }
	if (vfs_rename(tmp, ko) != 0)      { ret = -2; goto out; }
	ret = 0;
out:
	kfree(ko); kfree(kn); kfree(tmp);
	return ret;
}

static long sys_copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                                size_t len, unsigned int flags)
{
	if (flags) return -22;   /* EINVAL: no flags defined */
	char buf[4096];
	/* When an offset pointer is given, read/write at that offset and advance the
	 * caller's copy without disturbing the fd's own position; otherwise use the
	 * fd position (and let vfs_read/write advance it). */
	off_t in_off = 0, out_off = 0;
	if (off_in  && copy_from_user(&in_off,  off_in,  sizeof(in_off))  != 0) return -14;
	if (off_out && copy_from_user(&out_off, off_out, sizeof(out_off)) != 0) return -14;
	if (in_off < 0 || out_off < 0) return -22;

	size_t done = 0;
	/* an endless source and sink must stay killable */
	if (len > 0x7ffff000UL) len = 0x7ffff000UL;
	while (done < len) {
		if (task_has_actionable_signal()) { if (done == 0) return -4; break; }
		size_t chunk = len - done; if (chunk > sizeof(buf)) chunk = sizeof(buf);
		if (off_in) vfs_seek(fd_in, in_off, SEEK_SET);
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) { if (done == 0 && r < 0) return r; break; }
		if (off_out) vfs_seek(fd_out, out_off, SEEK_SET);
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		in_off  += w;
		out_off += w;
		done    += (size_t)w;
		if (w < r) break;
	}

	if (off_in  && copy_to_user(off_in,  &in_off,  sizeof(in_off))  != 0) return -14;
	if (off_out && copy_to_user(off_out, &out_off, sizeof(out_off)) != 0) return -14;
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
	/* kernel/claim.c: somebody else holds "reboot" (an agent's test, a person's
	 * download): a machine reboot by anyone but init (fjord-sys runs its own check,
	 * with a person's override) fails with EBUSY. Before the capability check, so
	 * the refusal names the claim even to an unprivileged caller (claims are public
	 * in /dev/claim), and a test can prove the gate without the power to reboot. */
	if ((uint32_t)magic1 == 0xfee1dead &&
	    ((uint32_t)cmd == 0x01234567u || (uint32_t)cmd == 0xCDEF0123u || (uint32_t)cmd == 0x4321FEDCu) &&
	    current_pid_ns() == &init_pid_ns) {
		extern int claim_reboot_check(process_t *caller);
		process_t *me = task_current();
		if (me->tgid != 1 && claim_reboot_check(me)) return -16;   /* EBUSY */
	}
	if (!capable_task(task_current(), CAP_SYS_BOOT)) return -1; /* EPERM */
	if ((uint32_t)magic1 != 0xfee1dead) return -22;
	if ((uint32_t)magic2 != 0x28121969 &&
	    (uint32_t)magic2 != 0x05121996 &&
	    (uint32_t)magic2 != 0x16041998 &&
	    (uint32_t)magic2 != 0x20112000) return -22;
	/* Inside a pid namespace reboot ends that namespace, not the machine: its init
	 * is killed and the rest goes with it (Linux reboot_pid_ns). A container must
	 * not be able to turn the host off. */
	{
		pid_namespace_t *ns = current_pid_ns();
		if (ns && ns != &init_pid_ns) {
			if (cmd == 0 || (uint32_t)cmd == 0x89ABCDEFu) return 0;
			if ((uint32_t)cmd != 0x01234567u && (uint32_t)cmd != 0xCDEF0123u &&
			    (uint32_t)cmd != 0x4321FEDCu) return -22;
			process_t *init = ns->reaper;
			if (init) task_signal(init, 9);
			return 0;
		}
	}
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_sync_all();
#endif
	/* Ctrl-Alt-Del policy: nothing here to turn on or off. init sets it at boot,
	 * so it must not fall through to a reset. */
	if (cmd == 0 || (uint32_t)cmd == 0x89ABCDEFu) return 0;
	if ((uint32_t)cmd != 0x01234567u && (uint32_t)cmd != 0xCDEF0123u &&
	    (uint32_t)cmd != 0x4321FEDCu) return -22;
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_shutdown_all();   /* only now: the commands above leave the system running */
#endif
	block_flush_all();
	kprintf(KERN_INFO "reboot: system going down\n");
	if ((uint32_t)cmd != 0x01234567u) /* HALT, POWER_OFF */
		board_poweroff();
	board_reset();
	for (;;) __asm__ volatile("" ::: "memory");
	return 0;
}

static long sys_syslog(int type, char *buf, int len)
{
	extern int dmesg_restrict;
	if (type < 0 || type > 10) return -22;
	if (type >= 2 && type <= 4 && len < 0) return -22;
	/* same gate as /dev/kmsg; everything but the size query touches the log */
	if (type != 10 && dmesg_restrict && !capable_task(task_current(), CAP_SYSLOG))
		return -1;   /* EPERM */
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

static long do_utimensat(int dirfd, const char *path, const void *times, int flags, int kernel_times)
{
	vfs_inode_t *in = NULL;
	char lpath[PATH_MAX];
	int nofollow = path && (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */);
	if (!path) {                                   /* futimens: dirfd is the fd */
		process_t *p = task_current();
		vfs_file_t *f = (p && p->fds) ? fd_get(p->fds, dirfd) : NULL;
		if (!f || !f->inode) return -9;            /* EBADF */
		if (f->mnt_flags & MS_RDONLY) return -30; /* EROFS */
		in = f->inode;
	} else if (nofollow) {                         /* the link itself; tar sets it dangling */
		int r = resolve_path(lpath, dirfd, path);
		if (r) return r;
	} else {
		char kp[PATH_MAX];
		int r = resolve_path(kp, dirfd, path);
		if (r) return r;
		if (vfs_path_mnt_flags(kp) & MS_RDONLY) return -30; /* EROFS */
		vfs_dentry_t *d = vfs_dentry_resolve(kp);
		if (!d || !d->inode) return -2;            /* ENOENT */
		if (vfs_dentry_readonly(d)) return -30;    /* EROFS */
		in = d->inode;
	}

	int64_t now_s, now_ns; ranix_walltime(&now_s, &now_ns);
	int64_t at_s = now_s, mt_s = now_s;
	uint32_t at_n = (uint32_t)now_ns, mt_n = (uint32_t)now_ns;
	int set_at = 1, set_mt = 1, explicit_times = 0;
	if (times) {
		struct { int64_t sec, nsec; } ts[2];
		if (kernel_times) memcpy(ts, times, sizeof(ts));
		else if (copy_from_user(ts, times, sizeof(ts)) != 0) return -14;
		for (int i = 0; i < 2; i++)
			if (ts[i].nsec != 0x3FFFFFFE && ts[i].nsec != 0x3FFFFFFF &&
			    (ts[i].nsec < 0 || ts[i].nsec >= 1000000000L)) return -22;
		if (ts[0].nsec == 0x3FFFFFFE) set_at = 0;          /* UTIME_OMIT */
		else if (ts[0].nsec != 0x3FFFFFFF) { at_s = ts[0].sec; at_n = (uint32_t)ts[0].nsec; explicit_times = 1; }
		if (ts[1].nsec == 0x3FFFFFFE) set_mt = 0;
		else if (ts[1].nsec != 0x3FFFFFFF) { mt_s = ts[1].sec; mt_n = (uint32_t)ts[1].nsec; explicit_times = 1; }
	}
	vfs_iattr_t a = { .atime = at_s, .atime_nsec = at_n, .mtime = mt_s, .mtime_nsec = mt_n };
	if (set_at) a.valid |= ATTR_ATIME;
	if (set_mt) a.valid |= ATTR_MTIME;
	return nofollow ? vfs_lutimes(lpath, &a, explicit_times) : vfs_utimes_inode(in, &a, explicit_times);
}

static long sys_utimensat(int dirfd, const char *path, const void *times, int flags)
{
	return do_utimensat(dirfd, path, times, flags, 0);
}

static long sys_prlimit64(pid_t pid, int resource, const void *new_limit, void *old_limit)
{
	if (resource < 0 || resource >= RLIM_NLIMITS) return -22;
	if (old_limit && !access_ok(old_limit, 16)) return -14;
	if (new_limit && !access_ok(new_limit, 16)) return -14;
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -3;
	if (pid && !task_may_control(p, CAP_SYS_RESOURCE)) { proc_put(p); return -1; } /* EPERM */
	rlimit_seed(p);
	if (old_limit) {
		uint64_t cur[2] = { p->rlim_cur[resource], p->rlim_max[resource] };
		if (copy_to_user(old_limit, cur, 16)) { if (pid) proc_put(p); return -14; }
	}
	long rc = 0;
	if (new_limit) {
		uint64_t nl[2];
		if (copy_from_user(nl, new_limit, 16)) rc = -14;
		else rc = do_setrlimit(p, resource, nl[0], nl[1]);
	}
	if (pid) proc_put(p);
	return rc;
}

static long sys_sched_yield(void) { yield(); return 0; }

static long sys_sched_setaffinity(pid_t pid, size_t len, const void *mask)
{
	extern int smp_ncpus;
	if (len < 8) return -22;
	if (!access_ok(mask, len)) return -14;

	/* Pull the user cpu_set_t into a kernel cpumask, ignoring bits past NR_CPUS. */
	cpumask_t req;
	cpumask_clear(&req);
	size_t n = len < sizeof(req) ? len : sizeof(req);
	if (copy_from_user(&req, mask, n)) return -14;

	cpumask_t online;
	cpumask_set_first_n(smp_ncpus, &online);
	for (int i = 0; i < CPUMASK_LONGS; i++) req.bits[i] &= online.bits[i];
	if (cpumask_empty(&req)) return -22;            /* selects no online CPU */

	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -3;
	if (pid && !task_may_control(p, CAP_SYS_NICE)) { proc_put(p); return -1; } /* EPERM */

	int all = cpumask_weight(&req) == smp_ncpus;
	if (all) {
		if (p->cpu_affinity) { kfree(p->cpu_affinity); p->cpu_affinity = NULL; }
	} else {
		cpumask_t *aff = p->cpu_affinity;
		if (!aff) aff = kmalloc(sizeof(*aff));
		if (!aff) { if (pid) proc_put(p); return -12; }
		*aff = req;
		p->cpu_affinity = aff;
	}

	int is_self = (p == task_current());
	if (pid) proc_put(p);
	if (is_self && !all) {
		/* re-evaluate placement: if the current CPU is now excluded, yield so
		 * the next enqueue reassigns us to an allowed CPU. */
		int cid = (int)arch_get_cpu_ctx()->cpu_id;
		if (!cpumask_test_cpu(cid, &req)) yield();
	}
	return 0;
}

static long sys_sched_getaffinity(pid_t pid, size_t len, void *mask)
{
	if (len < 8) return -22;
	if (!access_ok(mask, len)) return -14;
	extern int smp_ncpus;
	process_t *p = pid ? proc_get_user(pid) : task_current();
	if (!p) return -3;
	cpumask_t m;
	if (p->cpu_affinity) m = *p->cpu_affinity;
	else cpumask_set_first_n(smp_ncpus, &m);
	if (pid) proc_put(p);
	size_t n = len < sizeof(m) ? len : sizeof(m);
	return copy_to_user(mask, &m, n) ? -14 : (long)n;
}

#define PR_SET_NAME    15
#define PR_GET_NAME    16
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_PDEATHSIG 1
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP 22

struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };

/* A chain, newest first. A node never changes once installed, so forks and
 * threads share it; each task holds one reference, to its newest node. */
struct seccomp_filter {
	struct seccomp_filter *prev;    /* owns a reference */
	volatile uint32_t refs;
	unsigned len;
	unsigned path_len;              /* cost of running this node and all older ones */
	struct sock_filter insns[];
};

#define SECCOMP_MAX_PATH 32768      /* every filter runs on every syscall */

void seccomp_fork(process_t *child, process_t *parent)
{
	child->seccomp_mode   = parent->seccomp_mode;
	child->seccomp_filter = parent->seccomp_filter;
	if (child->seccomp_filter) atomic_fetch_add32(&child->seccomp_filter->refs, 1);
}

void seccomp_put(struct seccomp_filter *f)
{
	while (f && atomic_sub_fetch32(&f->refs, 1) == 0) {
		struct seccomp_filter *prev = f->prev;
		kfree(f);
		f = prev;
	}
}

static long seccomp_set_strict(process_t *p)
{
	if (p->seccomp_mode == 2) return -22;
	p->seccomp_mode = 1;
	return 0;
}

/* Install a SECCOMP_MODE_FILTER program from a user sock_fprog {len, filter}. */
static long seccomp_install(process_t *p, uint64_t fprog_uptr)
{
	struct { uint16_t len; uint64_t filter; } fp;
	if (copy_from_user(&fp, (void *)(uintptr_t)fprog_uptr, sizeof(fp)) != 0) return -14;
	if (fp.len == 0 || fp.len > 4096) return -22;
	if (p->seccomp_mode == 1) return -22;
	/* a filter could otherwise make a setuid program fail in ways it never handles */
	if (!p->no_new_privs && !capable_task(p, CAP_SYS_ADMIN)) return -13;
	unsigned path_len = fp.len + 4 + (p->seccomp_filter ? p->seccomp_filter->path_len : 0);
	if (path_len > SECCOMP_MAX_PATH) return -12;
	size_t sz = (size_t)fp.len * sizeof(struct sock_filter);
	struct seccomp_filter *flt = kmalloc(sizeof(*flt) + sz);
	if (!flt) return -12;
	if (copy_from_user(flt->insns, (void *)(uintptr_t)fp.filter, sz) != 0) { kfree(flt); return -14; }
	flt->prev     = p->seccomp_filter;   /* takes over the task's reference */
	flt->refs     = 1;
	flt->len      = fp.len;
	flt->path_len = path_len;
	p->seccomp_filter = flt;
	p->seccomp_mode   = 2;
	return 0;
}

static long sys_seccomp(unsigned int op, unsigned int flags, uint64_t args)
{
	process_t *p = task_current();
	if (flags) return -22;   /* ignoring TSYNC would leave sibling threads unfiltered */
	if (op == 0) return seccomp_set_strict(p);        /* SECCOMP_SET_MODE_STRICT */
	if (op == 1) return seccomp_install(p, args);     /* SECCOMP_SET_MODE_FILTER */
	return -22;
}

static long sys_prctl(int option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;
	process_t *p = task_current();
	switch (option) {
	case 0x52580010: /* PR_RANIX_UNAME_LINUX: arg2 1 = uname says Linux for this tree, 0 = Ranix */
		if (arg2 > 1) return -22;
		p->uname_linux = (int)arg2;
		return 0;
	case 0x52580011: /* PR_RANIX_GET_UNAME_LINUX */
		return p->uname_linux;
	case PR_SET_NAME: {
		char name[16];
		if (strncpy_from_user(name, (const void *)(uintptr_t)arg2, 16) < 0) return -14;
		name[15] = '\0';
		int len = 0;
		while (name[len] && len < PROC_NAME_MAX - 1) len++;
		memcpy(p->name, name, len);
		p->name[len] = '\0';
		return 0;
	}
	case PR_GET_NAME: {
		char name[16];
		memset(name, 0, sizeof(name));
		int len = 0;
		while (p->name[len] && len < 15) { name[len] = p->name[len]; len++; }
		return copy_to_user((void *)(uintptr_t)arg2, name, 16) ? -14 : 0;
	}
	case PR_SET_DUMPABLE:
		if (arg2 > 1) return -22;
		p->dumpable = (int)arg2;
		return 0;
	case PR_GET_DUMPABLE:
		return p->dumpable;
	case PR_SET_SECCOMP:
		if (arg2 == 1) return seccomp_set_strict(p);
		if (arg2 == 2) return seccomp_install(p, arg3);     /* FILTER, arg3=sock_fprog */
		return -22;
	case PR_GET_SECCOMP:
		return p->seccomp_mode;
	case 65: /* PR_SET_MDWE: arg2 bit 0 = refuse W+X; one way, as on Linux */
		if (arg2 & ~1UL) return -22;
		if (p->mdwe && !(arg2 & 1)) return -1;   /* EPERM */
		p->mdwe = arg2 & 1;
		return 0;
	case 66: /* PR_GET_MDWE */
		return p->mdwe;
	case 38: /* PR_SET_NO_NEW_PRIVS */
		if (arg2 != 1 || arg3 || arg4 || arg5) return -22;
		p->no_new_privs = 1;
		return 0;
	case 39: /* PR_GET_NO_NEW_PRIVS */
		return p->no_new_privs;
	case 1:  /* PR_SET_PDEATHSIG */
		if (arg2 > 64) return -22;
		p->pdeathsig = (int)arg2;
		return 0;
	case 2:  /* PR_GET_PDEATHSIG */
		return copy_to_user((void *)(uintptr_t)arg2, &p->pdeathsig, sizeof(int)) == 0 ? 0 : -14;
	case PR_RANIX_ASESS_ID:
		return (long)asess_id_of(p);
	default:
		return -22; /* EINVAL: unhandled option (do not claim success) */
	}
}

/* membarrier(2). QUERY returns the supported-command bitmask; GLOBAL and the
 * PRIVATE/GLOBAL _EXPEDITED commands run a real cross-CPU full barrier via the
 * IPI broadcast. The REGISTER_* variants are no-op success (no per-process
 * registration state needed for our unconditional barrier). */
#define MEMBARRIER_CMD_QUERY                       0
#define MEMBARRIER_CMD_GLOBAL                      (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED            (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED   (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED           (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED  (1 << 4)
static long sys_membarrier(int cmd, unsigned int flags)
{
	if (flags) return -22;
	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED |
		       MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |
		       MEMBARRIER_CMD_PRIVATE_EXPEDITED |
		       MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
	case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
		return 0;
	case MEMBARRIER_CMD_GLOBAL:
	case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		smp_membarrier();
		return 0;
	default:
		return -22;
	}
}

/* GRND_NONBLOCK=1 GRND_RANDOM=2 GRND_INSECURE=4 */
static long sys_getrandom(void *buf, size_t len, unsigned int flags)
{
	extern void csprng_bytes(void *, size_t);
	if (flags & ~0x7u) return -22;                  /* EINVAL: unknown flag */
	if (!access_ok(buf, len)) return -14;
	/* the DRBG is seeded at boot, so GRND_RANDOM never blocks here. */
	for (size_t i = 0; i < len; ) {
		uint8_t kb[256];
		size_t chunk = len - i < sizeof(kb) ? len - i : sizeof(kb);
		csprng_bytes(kb, chunk);
		if (copy_to_user((uint8_t *)buf + i, kb, chunk) != 0) return -14;
		i += chunk;
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
	char kp[PATH_MAX];
	vfs_stat_t ks = {0};   /* a filesystem need not fill all of it; it reaches sx */

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
			ks.sdev = f->sdev;
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
	sx.stx_dev_minor  = ks.sdev ? ks.sdev : 1;
	sx.stx_rdev_major = (uint32_t)(ks.rdev >> 8);
	sx.stx_rdev_minor = (uint32_t)(ks.rdev & 0xFF);
	sx.stx_atime_sec = ks.atime; sx.stx_atime_nsec = ks.atime_nsec;
	sx.stx_mtime_sec = ks.mtime; sx.stx_mtime_nsec = ks.mtime_nsec;
	sx.stx_ctime_sec = ks.ctime; sx.stx_ctime_nsec = ks.ctime_nsec;
	sx.stx_btime_sec = ks.ctime; sx.stx_btime_nsec = ks.ctime_nsec;
	return copy_to_user(buf, &sx, sizeof(sx)) ? -14 : 0;
}

