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
		task_sleep();
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
		task_sleep();
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
} epoll_t;

static int epoll_close(vfs_file_t *file)
{
	epoll_t     *ep = (epoll_t *)file->inode->priv;
	vfs_inode_t *in = file->inode;
	kfree(ep);
	kfree(in);
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
	f->refcount = 1;

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
				ep->fds[i]  = fd;
				ep->ev[i]   = ev.events;
				ep->data[i] = ev.data;
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
			ep->fds[i] = -1;
		} else if (op == EPOLL_CTL_MOD) {
			ep->ev[i]   = ev.events;
			ep->data[i] = ev.data;
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
			if (ep->fds[i] < 0) continue;
			int bits = fd_poll_bits(ep->fds[i]);
			int r = 0;
			if ((ep->ev[i] & EPOLLIN)  && (bits & POLLIN))  r |= EPOLLIN;
			if ((ep->ev[i] & EPOLLOUT) && (bits & POLLOUT)) r |= EPOLLOUT;
			if (r) {
				kev[ready].events = (uint32_t)r;
				kev[ready].data   = ep->data[i];
				ready++;
			}
		}
		spin_unlock_irq(&ep->lock, f);
		if (ready > 0 || kernel_ticks >= deadline) break;
		task_sleep();
	}

	copy_to_user(uevents, kev, (size_t)ready * sizeof(*kev));
	kfree(kev);
	return ready;
}

/* ── symlink / readlink ───────────────────────────────────── */

static long sys_symlink(const char *t, const char *lp)
{
	char kt[512], klp[512];
	if (strncpy_from_user(kt,  t,  sizeof(kt))  < 0) return -14;
	if (strncpy_from_user(klp, lp, sizeof(klp)) < 0) return -14;
	return vfs_symlink(kt, klp);
}
static long sys_symlinkat(const char *t, int d, const char *lp)
{
	char kt[512];
	if (strncpy_from_user(kt, t, sizeof(kt)) < 0) return -14;
	char klp[512];
	int r = resolve_path(klp, d, lp);
	if (r) return r;
	return vfs_symlink(kt, klp);
}
static long sys_link(const char *o, const char *n) { (void)o; (void)n; return -1; }
static long sys_linkat(int od, const char *o, int nd, const char *n, int f)
{
	(void)od; (void)nd; (void)f; return sys_link(o, n);
}

/* ── chmod / chown (stubs) ───────────────────────────────── */

static long sys_chmod(const char *p, mode_t m)  { (void)p; (void)m; return 0; }
static long sys_fchmod(int fd, mode_t m)         { (void)fd; (void)m; return 0; }
static long sys_fchmodat(int d, const char *p, mode_t m, int f)
{ (void)d; (void)p; (void)m; (void)f; return 0; }
static long sys_chown(const char *p, uid_t u, gid_t g)  { (void)p; (void)u; (void)g; return 0; }
static long sys_fchown(int fd, uid_t u, gid_t g)         { (void)fd; (void)u; (void)g; return 0; }
static long sys_fchownat(int d, const char *p, uid_t u, gid_t g, int f)
{ (void)d; (void)p; (void)u; (void)g; (void)f; return 0; }

