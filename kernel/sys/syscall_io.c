/* ── ioctl ───────────────────────────────────────────────── */

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401
#define TIOCSPROMPT 0x54FF  /* Ranix: set TTY prompt string */
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F

static pid_t tty_fg_pgrp = 1;

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } linux_winsize_t;

static long sys_ioctl(int fd, uint32_t cmd, uint64_t arg)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (f) {
		if (f->inode && f->inode->fops && f->inode->fops->ioctl)
			return f->inode->fops->ioctl(f, cmd, arg);
		return -25; /* fd exists but has no ioctl (pipe, regular file…) */
	}
	/* fd not in table — fallback for stdin/stdout before /dev/console open */
	switch (cmd) {
	case TIOCGWINSZ: {
		linux_winsize_t ws = {24, 80, 0, 0};
		return copy_to_user((void *)(uintptr_t)arg, &ws, sizeof(ws));
	}
	case TCGETS: {
		uint8_t termios[36]; memset(termios, 0, 36);
		return copy_to_user((void *)(uintptr_t)arg, termios, 36);
	}
	case TCSETS: case TCSETSW: case TCSETSF: return 0;
	case TIOCGPGRP: { int pgid = (int)tty_fg_pgrp; return copy_to_user((void *)(uintptr_t)arg, &pgid, 4); }
	case TIOCSPGRP: { int pgid; if (copy_from_user(&pgid, (void *)(uintptr_t)arg, 4) != 0) return -14; tty_fg_pgrp = pgid; return 0; }
	case TIOCSPROMPT: {
		return 0;
	}
	default: return -25; /* ENOTTY */
	}
}

/* ── poll / select ───────────────────────────────────────── */

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

typedef struct { int fd; short events; short revents; } linux_pollfd_t;

static int fd_poll_bits(int fd)
{
	if (fd == 0) {
		process_t  *p = task_current();
		vfs_file_t *f = p->fds ? fd_get(p->fds, 0) : NULL;
		if (f) {
			if (f->inode->fops && f->inode->fops->poll)
				return f->inode->fops->poll(f);
			return 0x0005; /* POLLIN|POLLOUT */
		}
		return uart_has_data() ? 0x0001 : 0;
	}
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return 0;
	if (f->inode->fops && f->inode->fops->poll)
		return f->inode->fops->poll(f);
	return 0x0005; /* regular file: always readable+writable */
}

static int fd_is_readable(int fd) { return fd_poll_bits(fd) & 0x0001; }
static int fd_is_writable(int fd) { return fd_poll_bits(fd) & 0x0004; }

static long sys_poll(linux_pollfd_t *fds, uint64_t nfds, int timeout_ms)
{
	if (!fds || nfds == 0) return 0;
	if (!access_ok(fds, nfds * sizeof(linux_pollfd_t))) return -14;

	linux_pollfd_t *kfds = kmalloc(nfds * sizeof(linux_pollfd_t));
	if (!kfds) return -12;
	if (copy_from_user(kfds, fds, nfds * sizeof(linux_pollfd_t)) != 0) {
		kfree(kfds); return -14;
	}

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
	int ready = 0;
	while (1) {
		ready = 0;
		for (uint64_t i = 0; i < nfds; i++) {
			kfds[i].revents = 0;
			if (kfds[i].fd < 0) continue;
			int bits = fd_poll_bits(kfds[i].fd);
			if ((kfds[i].events & POLLIN)  && (bits & POLLIN))  kfds[i].revents |= POLLIN;
			if ((kfds[i].events & POLLOUT) && (bits & POLLOUT)) kfds[i].revents |= POLLOUT;
			if (kfds[i].revents) ready++;
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		if (task_has_actionable_signal()) { kfree(kfds); return -4; }
		poll_block(deadline);
	}

	copy_to_user(fds, kfds, nfds * sizeof(linux_pollfd_t));
	kfree(kfds);
	return ready;
}

static long sys_select(int nfds, uint64_t *readfds, uint64_t *writefds,
                       uint64_t *exceptfds, linux_timeval_t *timeout)
{
	(void)exceptfds;
	if (nfds <= 0) return 0;

	uint64_t rfds = 0, wfds = 0;
	if (readfds  && copy_from_user(&rfds, readfds,  8) != 0) return -14;
	if (writefds && copy_from_user(&wfds, writefds, 8) != 0) return -14;

	uint64_t deadline = (uint64_t)-1;
	if (timeout) {
		linux_timeval_t tv;
		if (copy_from_user(&tv, timeout, sizeof(tv)) == 0 && (tv.tv_sec || tv.tv_usec))
			deadline = kernel_ticks + (uint64_t)tv.tv_sec * TIMER_HZ +
			           (uint64_t)tv.tv_usec * TIMER_HZ / 1000000 + 1;
		else if (timeout)
			deadline = 0;
	}

	int ready = 0;
	uint64_t rres = 0, wres = 0;
	while (1) {
		rres = 0; wres = 0; ready = 0;
		for (int i = 0; i < nfds && i < 64; i++) {
			if ((rfds & (1ULL << i)) && fd_is_readable(i)) { rres |= (1ULL << i); ready++; }
			if ((wfds & (1ULL << i)) && fd_is_writable(i)) { wres |= (1ULL << i); ready++; }
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		if (task_has_actionable_signal()) return -4;
		poll_block(deadline);
	}

	if (readfds)  copy_to_user(readfds,  &rres, 8);
	if (writefds) copy_to_user(writefds, &wres, 8);
	return ready;
}

/* pselect6 / ppoll */
static long sys_pselect6(int nfds, uint64_t *r, uint64_t *w, uint64_t *e,
                         linux_timespec_t *ts, void *sigmask)
{
	(void)sigmask;
	linux_timeval_t tv = {0};
	linux_timeval_t *tvp = NULL;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) == 0) {
			tv.tv_sec = k.tv_sec; tv.tv_usec = k.tv_nsec / 1000;
			tvp = &tv;
		}
	}
	return sys_select(nfds, r, w, e, tvp);
}

static long sys_ppoll(linux_pollfd_t *fds, uint64_t nfds,
                      linux_timespec_t *ts, void *sigmask)
{
	(void)sigmask;
	int timeout_ms = -1;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) == 0)
			timeout_ms = (int)(k.tv_sec * 1000 + k.tv_nsec / 1000000);
	}
	return sys_poll(fds, nfds, timeout_ms);
}

/* ── epoll ───────────────────────────────────────────────── */

#define EPOLLIN    0x00000001
#define EPOLLOUT   0x00000004
#define EPOLLERR   0x00000008
#define EPOLLHUP   0x00000010
#define EPOLLRDHUP 0x00002000
#define EPOLLET    0x80000000
#define EPOLLONESHOT 0x40000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef struct {
	uint32_t events;
	uint64_t data;
} __attribute__((packed)) linux_epoll_event_t;

#define EPOLL_MAX_WATCH 64

typedef struct {
	spinlock_t lock;
	int        fds[EPOLL_MAX_WATCH];
	uint32_t   ev[EPOLL_MAX_WATCH];
	uint64_t   data[EPOLL_MAX_WATCH];
	uint32_t   last_ready[EPOLL_MAX_WATCH]; /* for EPOLLET edge tracking */
} epoll_t;

static int epoll_close(vfs_file_t *file)
{
	epoll_t     *ep = (epoll_t *)file->inode->priv;
	kfree(ep);
	file->inode->priv = NULL;   /* vfs_close frees the inode (avoid double free) */
	return 0;
}

static vfs_fops_t epoll_fops = { .close = epoll_close };

static epoll_t *epfd_get(int epfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = p->fds ? fd_get(p->fds, epfd) : NULL;
	if (!f || !f->inode || f->inode->fops != &epoll_fops) return NULL;
	return (epoll_t *)f->inode->priv;
}

static long sys_epoll_create1(int flags)
{
	(void)flags;
	epoll_t *ep = kmalloc(sizeof(*ep));
	if (!ep) return -12;
	memset(ep, 0, sizeof(*ep));
	ep->lock = (spinlock_t)SPINLOCK_INIT;
	for (int i = 0; i < EPOLL_MAX_WATCH; i++) ep->fds[i] = -1;

	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) { kfree(ep); return -12; }
	memset(in, 0, sizeof(*in));
	in->fops     = &epoll_fops;
	in->priv     = ep;
	in->refcount = 1;

	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(ep); kfree(in); return -12; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->refcount = 0;   /* fd_alloc takes the +1; a single close() then frees */

	process_t *p = task_current();
	int fd = fd_alloc(p->fds, f);
	if (fd < 0) { kfree(f); kfree(ep); kfree(in); return -12; }
	return fd;
}

static long sys_epoll_ctl(int epfd, int op, int fd, linux_epoll_event_t *uevent)
{
	epoll_t *ep = epfd_get(epfd);
	if (!ep) return -9;

	linux_epoll_event_t ev = {0};
	if (op != EPOLL_CTL_DEL && uevent)
		copy_from_user(&ev, uevent, sizeof(ev));

	uint64_t f = spin_lock_irq(&ep->lock);

	if (op == EPOLL_CTL_ADD) {
		for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
			if (ep->fds[i] == fd) { spin_unlock_irq(&ep->lock, f); return -17; }
		}
		for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
			if (ep->fds[i] < 0) {
				ep->fds[i]        = fd;
				ep->ev[i]         = ev.events;
				ep->data[i]       = ev.data;
				ep->last_ready[i] = 0;
				spin_unlock_irq(&ep->lock, f);
				return 0;
			}
		}
		spin_unlock_irq(&ep->lock, f);
		return -28;
	}

	for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
		if (ep->fds[i] != fd) continue;
		if (op == EPOLL_CTL_DEL) {
			ep->fds[i]        = -1;
			ep->last_ready[i] = 0;
		} else if (op == EPOLL_CTL_MOD) {
			ep->ev[i]         = ev.events;
			ep->data[i]       = ev.data;
			ep->last_ready[i] = 0; /* re-arm clears edge state */
		}
		spin_unlock_irq(&ep->lock, f);
		return 0;
	}

	spin_unlock_irq(&ep->lock, f);
	return (op == EPOLL_CTL_MOD || op == EPOLL_CTL_DEL) ? -2 : -22;
}

static long sys_epoll_pwait(int epfd, linux_epoll_event_t *uevents, int maxevents,
                            int timeout_ms, void *sigmask)
{
	(void)sigmask;
	epoll_t *ep = epfd_get(epfd);
	if (!ep) return -9;
	if (!uevents || maxevents <= 0) return -22;
	if (!access_ok(uevents, (size_t)maxevents * sizeof(linux_epoll_event_t))) return -14;

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;

	linux_epoll_event_t *kev = kmalloc((size_t)maxevents * sizeof(*kev));
	if (!kev) return -12;

	int ready = 0;
	while (1) {
		ready = 0;
		uint64_t f = spin_lock_irq(&ep->lock);
		for (int i = 0; i < EPOLL_MAX_WATCH && ready < maxevents; i++) {
			if (ep->fds[i] < 0 || ep->ev[i] == 0) continue;
			int bits = fd_poll_bits(ep->fds[i]);
			uint32_t r = 0;
			if ((ep->ev[i] & EPOLLIN)  && (bits & POLLIN))  r |= EPOLLIN;
			if ((ep->ev[i] & EPOLLOUT) && (bits & POLLOUT)) r |= EPOLLOUT;

			int report;
			if (ep->ev[i] & EPOLLET) {
				/* edge-trigger: only report newly ready bits */
				report = (r & ~ep->last_ready[i]) != 0;
				ep->last_ready[i] = r;
			} else {
				/* level-trigger: report whenever ready */
				report = (r != 0);
				ep->last_ready[i] = 0;
			}

			if (report) {
				kev[ready].events = r;
				kev[ready].data   = ep->data[i];
				ready++;
				if (ep->ev[i] & EPOLLONESHOT)
					ep->ev[i] = 0; /* disarm until re-armed with MOD */
			}
		}
		spin_unlock_irq(&ep->lock, f);
		if (ready > 0 || kernel_ticks >= deadline) break;
		poll_block(deadline);
	}

	copy_to_user(uevents, kev, (size_t)ready * sizeof(*kev));
	kfree(kev);
	return ready;
}

/* ── symlink / readlink ───────────────────────────────────── */

static long sys_symlink(const char *t, const char *lp)
{
	char *kt = kmalloc(PATH_MAX);
	if (!kt) return -12;
	char *klp = kmalloc(PATH_MAX);
	if (!klp) { kfree(kt); return -12; }
	long ret;
	int r;
	if ((r = copy_path_from_user(kt, t)) || (r = copy_path_from_user(klp, lp)))
		ret = r;
	else
		ret = vfs_symlink(kt, klp);
	kfree(kt); kfree(klp);
	return ret;
}
static long sys_symlinkat(const char *t, int d, const char *lp)
{
	char *kt = kmalloc(PATH_MAX);
	if (!kt) return -12;
	char *klp = kmalloc(PATH_MAX);
	if (!klp) { kfree(kt); return -12; }
	long ret;
	int r;
	if ((r = copy_path_from_user(kt, t)) || (r = resolve_path(klp, d, lp)))
		ret = r;
	else
		ret = vfs_symlink(kt, klp);
	kfree(kt); kfree(klp);
	return ret;
}
static long sys_linkat(int od, const char *o, int nd, const char *n, int f)
{
	if (f & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -22; /* EINVAL */
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -12;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -12; }
	long ret;
	int r;
	if ((r = resolve_path(ko, od, o)) || (r = resolve_path(kn, nd, n)))
		ret = r;
	else {
		vfs_stat_t ls = {0};
		if (!(f & AT_SYMLINK_FOLLOW) && vfs_lstat(ko, &ls) == 0 && S_ISLNK(ls.mode))
			ret = -22;                          /* hardlink to a symlink unsupported */
		else
			ret = vfs_link(ko, kn);
	}
	kfree(ko); kfree(kn);
	return ret;
}

/* ── chmod / chown ───────────────────────────────────────── */

static long sys_chmod(const char *p, mode_t m)
{
	char kp[PATH_MAX];
	int r = copy_path_from_user(kp, p);
	if (r) return r;
	return vfs_chmod(kp, m);
}
static long sys_fchmod(int fd, mode_t m)
{
	process_t *proc = task_current();
	vfs_file_t *f = fd_get(proc->fds, fd);
	if (!f || !f->inode) return -9;
	f->inode->mode = (f->inode->mode & 0xF000) | (m & 07777);
	return 0;
}
static long sys_fchmodat(int d, const char *p, mode_t m, int f)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, d, p);
	if (r) return r;
	return (f & AT_SYMLINK_NOFOLLOW) ? vfs_lchmod(kp, m) : vfs_chmod(kp, m);
}
static long sys_chown(const char *p, uid_t u, gid_t g)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, AT_FDCWD, p);
	if (r) return r;
	return vfs_chown(kp, u, g);
}
static long sys_fchown(int fd, uid_t u, gid_t g)
{
	process_t *proc = task_current();
	vfs_file_t *f = (proc && proc->fds) ? fd_get(proc->fds, fd) : NULL;
	if (!f || !f->inode) return -9;
	return vfs_chown_inode(f->inode, u, g);
}
static long sys_fchownat(int d, const char *p, uid_t u, gid_t g, int flags)
{
	if (flags & AT_EMPTY_PATH) {
		char t[2];
		if (strncpy_from_user(t, p, 2) >= 0 && t[0] == '\0') return sys_fchown(d, u, g);
	}
	char kp[PATH_MAX];
	int r = resolve_path(kp, d, p);
	if (r) return r;
	return (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lchown(kp, u, g) : vfs_chown(kp, u, g);
}

/* ── read / write / open / stat ──────────────────────────────────────────────
 * Thin wrappers over the VFS: validate the user pointer, then call vfs_*.
 * writev/readv loop over iovecs reusing the single-buffer paths; sendfile
 * copies in-kernel between two fds. resolve_path (sys/syscall_fs.c) turns a
 * dirfd + user path into a kernel path for the path-based opens.
 * ───────────────────────────────────────────────────────────────────────── */

static long sys_write(int fd, const void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;
	return vfs_write(fd, buf, len);
}

static long sys_read(int fd, void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;
	return vfs_read(fd, buf, len);
}

static long sys_writev(int fd, const iovec_t *iov, int cnt)
{
	if (cnt < 0 || cnt > 1024) return -22;   /* IOV_MAX */

	long total = 0;
	for (int i = 0; i < cnt; i++) {
		iovec_t v;
		if (copy_from_user(&v, &iov[i], sizeof(v)) != 0) return -14;
		if (!v.iov_base || v.iov_len == 0) continue;
		long r = sys_write(fd, v.iov_base, v.iov_len);
		if (r < 0) return r;
		total += r;
	}
	return total;
}

static long sys_sendfile(int out_fd, int in_fd, long *offset, size_t count)
{
	char buf[4096];
	size_t sent = 0;
	if (offset) {
		long off;
		if (copy_from_user(&off, offset, sizeof(off)) != 0) return -14;
		vfs_seek(in_fd, off, 0);
	}
	while (sent < count) {
		size_t n = count - sent;
		if (n > sizeof(buf)) n = sizeof(buf);
		long r = vfs_read(in_fd, buf, n);
		if (r <= 0) break;
		long w = vfs_write(out_fd, buf, (size_t)r);
		if (w < 0) return w;
		sent += (size_t)w;
	}
	if (offset) {
		long off = (long)vfs_seek(in_fd, 0, 1);
		copy_to_user(offset, &off, sizeof(off));
	}
	return (long)sent;
}

static long sys_readv(int fd, const iovec_t *iov, int cnt)
{
	if (cnt < 0 || cnt > 1024) return -22;   /* IOV_MAX */

	long total = 0;
	for (int i = 0; i < cnt; i++) {
		iovec_t v;
		if (copy_from_user(&v, &iov[i], sizeof(v)) != 0) return -14;
		if (!v.iov_base || v.iov_len == 0) continue;
		long r = sys_read(fd, v.iov_base, v.iov_len);
		if (r < 0) return r;
		if (r == 0) break;
		total += r;
	}
	return total;
}

static long sys_open(const char *path, int flags, mode_t mode)
{
	char kpath[PATH_MAX];
	int r = resolve_path(kpath, AT_FDCWD, path);
	if (r) return r;
	return vfs_open(kpath, flags, mode);
}

static long sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	char kpath[PATH_MAX];
	int r = resolve_path(kpath, dirfd, path);
	if (r) return r;
	return vfs_open(kpath, flags, mode);
}

static long sys_close(int fd)
{
	return vfs_close(fd) == 0 ? 0 : -9;
}

/* close_range(first, last, flags): close every open fd in [first,last].
 * CLOSE_RANGE_CLOEXEC(4) marks them close-on-exec instead; CLOSE_RANGE_UNSHARE(2)
 * is accepted but the table is not unshared (single-table semantics). */
static long sys_close_range(unsigned int first, unsigned int last, unsigned int flags)
{
	if (first > last || (flags & ~6u)) return -22; /* EINVAL */
	process_t *p = task_current();
	if (!p->fds) return 0;
	unsigned int cap = (unsigned int)p->fds->cap;
	unsigned int hi  = (last >= cap) ? (cap ? cap - 1 : 0) : last;
	for (unsigned int fd = first; fd < cap && fd <= hi; fd++) {
		if (flags & 4u) {                       /* CLOSE_RANGE_CLOEXEC */
			if (fd_get(p->fds, (int)fd)) fd_set_cloexec(p->fds, (int)fd, 1);
		} else {
			vfs_close((int)fd);                 /* already-closed fds return -1, ignored */
		}
	}
	return 0;
}

static long sys_lseek(int fd, off_t off, int whence)
{
	return (long)vfs_seek(fd, off, whence);
}

static long sys_fstat(int fd, linux_stat_t *st)
{
	if (!st) return -14;
	memset(st, 0, sizeof(*st));

	if (fd >= 0 && fd <= 2) {
		st->st_mode     = 0x2190;
		st->st_nlink    = 1;
		st->st_rdev     = 0x0501;
		st->st_blksize  = 1024;
		return 0;
	}

	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;

	vfs_stat_t vs = {0};
	if (f->inode->iops && f->inode->iops->stat)
		f->inode->iops->stat(f->inode, &vs);

	st->st_dev     = 1;  /* single root device */
	st->st_rdev    = vs.rdev;
	st->st_mode    = vs.mode;
	st->st_ino     = vs.ino;
	st->st_size    = (int64_t)vs.size;
	st->st_nlink   = vs.nlink ? vs.nlink : 1;
	st->st_uid     = vs.uid;
	st->st_gid     = vs.gid;
	st->st_blksize = 4096;
	st->st_blocks  = (vs.size + 511) / 512;
	return 0;
}

