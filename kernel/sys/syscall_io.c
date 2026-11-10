/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── ioctl ───────────────────────────────────────────────── */

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401
#define TIOCSPROMPT 0x54FF  /* Ranix: set TTY prompt string */
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F
#define FIONCLEX   0x5450
#define FIOCLEX    0x5451
#define FIONBIO    0x5421

static pid_t tty_fg_pgrp = 1;

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } linux_winsize_t;

static long sys_ioctl(int fd, uint32_t cmd, uint64_t arg)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);   /* pin: ioctl fops may block (tty, block dev) */
	if (f) {
		long r;
		switch (cmd) {
		case FIONBIO: {   /* any file: libuv sets stdout nonblocking this way */
			int on;
			r = copy_from_user(&on, (void *)(uintptr_t)arg, sizeof(on)) ? -14 : 0;
			if (r == 0) {
				if (on) f->flags |=  O_NONBLOCK;
				else    f->flags &= ~O_NONBLOCK;
			}
			break;
		}
		case FIOCLEX:
		case FIONCLEX:
			fd_set_cloexec(p->fds, fd, cmd == FIOCLEX);
			r = 0;
			break;
		default:
			r = (f->inode && f->inode->fops && f->inode->fops->ioctl)
			  ? f->inode->fops->ioctl(f, cmd, arg)
			  : -25; /* fd exists but has no ioctl (pipe, regular file…) */
		}
		fput(f);
		return r;
	}
	/* fd not in table — fallback for stdin/stdout before /dev/console open */
	if (fd < 0 || fd > 2) return -EBADF;
	switch (cmd) {
	case TIOCGWINSZ: {
		linux_winsize_t ws = {24, 80, 0, 0};
		return copy_to_user((void *)(uintptr_t)arg, &ws, sizeof(ws)) ? -14 : 0;
	}
	case TCGETS: {
		uint8_t termios[36]; memset(termios, 0, 36);
		return copy_to_user((void *)(uintptr_t)arg, termios, 36) ? -14 : 0;
	}
	case TCSETS: case TCSETSW: case TCSETSF: return 0;
	case TIOCGPGRP: { int pgid = (int)tty_fg_pgrp; return copy_to_user((void *)(uintptr_t)arg, &pgid, 4) ? -14 : 0; }
	case TIOCSPGRP: { int pgid; if (copy_from_user(&pgid, (void *)(uintptr_t)arg, 4) != 0) return -EFAULT; tty_fg_pgrp = pgid; return 0; }
	case TIOCSPROMPT: {
		return 0;
	}
	default: return -ENOTTY;
	}
}

/* ── poll / select ───────────────────────────────────────── */

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

typedef struct { int fd; short events; short revents; } linux_pollfd_t;

/* The file's own edge counters (poll_gens fop): 1 if it has them */
static int fd_poll_gens(int fd, uint64_t *in_gen, uint64_t *out_gen)
{
	process_t  *p = task_current();
	vfs_file_t *f = p->fds ? fget(p->fds, fd) : NULL;
	if (!f) return 0;
	int have = f->inode && f->inode->fops && f->inode->fops->poll_gens;
	if (have) f->inode->fops->poll_gens(f, in_gen, out_gen);
	fput(f);
	return have;
}

static int fd_poll_bits(int fd)
{
	process_t  *p = task_current();
	/* pinned: a sibling thread may close fd while the poll fop runs */
	vfs_file_t *f = p->fds ? fget(p->fds, fd) : NULL;
	if (!f) {
		if (fd == 0) return uart_has_data() ? 0x0001 : 0;
		return POLLNVAL;
	}
	int bits = 0x0005; /* regular file: always readable+writable */
	if (f->inode->fops && f->inode->fops->poll) {
		poll_note_key(f->inode->fops->poll_keyed ? f->inode->priv : NULL);
		bits = f->inode->fops->poll(f);
	}
	fput(f);
	return bits;
}

static long sys_poll(linux_pollfd_t *fds, uint64_t nfds, int timeout_ms)
{
	/* nfds is unbounded user input: bound it before the size multiplication */
	if (nfds > FD_MAX) return -EINVAL;
	if (nfds && !access_ok(fds, nfds * sizeof(linux_pollfd_t))) return -EFAULT;

	linux_pollfd_t *kfds = NULL;
	if (nfds) {
		kfds = kmalloc(nfds * sizeof(linux_pollfd_t));
		if (!kfds) return -ENOMEM;
		if (copy_from_user(kfds, fds, nfds * sizeof(linux_pollfd_t)) != 0) {
			kfree(kfds); return -EFAULT;
		}
	}

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
	int ready = 0;
	while (1) {
		poll_arm_scan();
		ready = 0;
		for (uint64_t i = 0; i < nfds; i++) {
			kfds[i].revents = 0;
			if (kfds[i].fd < 0) continue;
			int bits = fd_poll_bits(kfds[i].fd);
			if ((kfds[i].events & POLLIN)  && (bits & POLLIN))  kfds[i].revents |= POLLIN;
			if ((kfds[i].events & POLLOUT) && (bits & POLLOUT)) kfds[i].revents |= POLLOUT;
			if ((kfds[i].events & 0x2000) && (bits & 0x2000)) kfds[i].revents |= 0x2000; /* POLLRDHUP */
			if ((kfds[i].events & 0x0002) && (bits & 0x0002)) kfds[i].revents |= 0x0002; /* POLLPRI */
			/* POLLHUP/POLLERR are reported regardless of the requested events
			 * (Linux semantics): a poller waiting only for POLLIN must still
			 * wake when the peer closes, or it blocks forever. */
			if (bits & POLLHUP) kfds[i].revents |= POLLHUP;
			if (bits & POLLERR) kfds[i].revents |= POLLERR;
			if (bits & POLLNVAL) kfds[i].revents |= POLLNVAL;
			if (kfds[i].revents) ready++;
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		if (task_has_actionable_signal()) { kfree(kfds); return -EINTR; }
		poll_block(deadline);
	}

	if (nfds && copy_to_user(fds, kfds, nfds * sizeof(linux_pollfd_t)) != 0) ready = -14;
	kfree(kfds);
	return ready;
}

static long select_until(int nfds, uint64_t *readfds, uint64_t *writefds, uint64_t deadline)
{
	if (nfds < 0) return -EINVAL;

	uint64_t rfds = 0, wfds = 0;
	if (nfds && readfds  && copy_from_user(&rfds, readfds,  8) != 0) return -EFAULT;
	if (nfds && writefds && copy_from_user(&wfds, writefds, 8) != 0) return -EFAULT;

	int ready = 0;
	uint64_t rres = 0, wres = 0;
	while (1) {
		poll_arm_scan();
		rres = 0; wres = 0; ready = 0;
		for (int i = 0; i < nfds && i < 64; i++) {
			if (!((rfds | wfds) & (1ULL << i))) continue;
			int bits = fd_poll_bits(i);
			if (bits & POLLNVAL) return -EBADF;
			if ((rfds & (1ULL << i)) && (bits & POLLIN))  { rres |= (1ULL << i); ready++; }
			if ((wfds & (1ULL << i)) && (bits & POLLOUT)) { wres |= (1ULL << i); ready++; }
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		if (task_has_actionable_signal()) return -EINTR;
		poll_block(deadline);
	}

	if (nfds && readfds  && copy_to_user(readfds,  &rres, 8) != 0) return -EFAULT;
	if (nfds && writefds && copy_to_user(writefds, &wres, 8) != 0) return -EFAULT;
	return ready;
}

static long sys_select(int nfds, uint64_t *readfds, uint64_t *writefds,
                       uint64_t *exceptfds, linux_timeval_t *timeout)
{
	(void)exceptfds;
	uint64_t deadline = (uint64_t)-1;
	if (timeout) {
		linux_timeval_t tv;
		if (copy_from_user(&tv, timeout, sizeof(tv)) != 0) return -EFAULT;
		if (tv.tv_sec < 0 || tv.tv_usec < 0) return -EINVAL;
		if (tv.tv_sec || tv.tv_usec)
			deadline = kernel_ticks + (uint64_t)tv.tv_sec * TIMER_HZ +
			           (uint64_t)tv.tv_usec * TIMER_HZ / 1000000 + 1;
		else
			deadline = 0;
	}
	return select_until(nfds, readfds, writefds, deadline);
}

/* pselect6 / ppoll */
/* The mask a pselect, ppoll or epoll_pwait waits under. make's jobserver blocks
 * SIGCHLD except in here, so a child's exit has no other way to wake it. On EINTR
 * the mask stays until signal_dispatch, as the handler has to run under it. */
static long wait_mask_enter(const void *umask)
{
	uint64_t m;
	if (copy_from_user(&m, umask, sizeof(m)) != 0) return -EFAULT;
	process_t *p = task_current();
	p->sig_saved_mask = p->sig_mask;
	p->sig_mask = m & ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	p->sig_mask_saved = 1;
	return 0;
}

static long wait_mask_leave(long ret)
{
	process_t *p = task_current();
	if (ret != -4 && p->sig_mask_saved) {
		p->sig_mask = p->sig_saved_mask;
		p->sig_mask_saved = 0;
	}
	return ret;
}

static long sys_pselect6(int nfds, uint64_t *r, uint64_t *w, uint64_t *e,
                         linux_timespec_t *ts, void *sigarg)
{
	struct { const void *set; size_t len; } sa = { NULL, 0 };
	if (sigarg && copy_from_user(&sa, sigarg, sizeof(sa)) != 0) return -EFAULT;
	(void)e;
	uint64_t deadline = (uint64_t)-1;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) != 0) return -EFAULT;
		if (k.tv_sec < 0 || k.tv_nsec < 0) return -EINVAL;
		deadline = k.tv_sec || k.tv_nsec
		         ? kernel_ticks + (uint64_t)k.tv_sec * TIMER_HZ +
		           (uint64_t)k.tv_nsec * TIMER_HZ / 1000000000 + 1
		         : 0;
	}
	if (sa.set) {
		long m = wait_mask_enter(sa.set);
		if (m) return m;
	}
	return wait_mask_leave(select_until(nfds, r, w, deadline));
}

static long sys_ppoll(linux_pollfd_t *fds, uint64_t nfds,
                      linux_timespec_t *ts, void *sigmask)
{
	int timeout_ms = -1;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) != 0) return -EFAULT;
		if (k.tv_sec < 0 || k.tv_nsec < 0) return -EINVAL;
		/* a long timeout must not wrap into a negative (infinite) one */
		timeout_ms = k.tv_sec >= 0x7fffffffL / 1000 ? 0x7fffffff
		           : (int)(k.tv_sec * 1000 + k.tv_nsec / 1000000);
	}
	if (sigmask) {
		long m = wait_mask_enter(sigmask);
		if (m) return m;
	}
	return wait_mask_leave(sys_poll(fds, nfds, timeout_ms));
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
	uint64_t   seen[EPOLL_MAX_WATCH];       /* poll_gen when last reported */
	uint64_t   seen_in[EPOLL_MAX_WATCH];    /* the file's own edge counters when last reported */
	uint64_t   seen_out[EPOLL_MAX_WATCH];
	int        next;                        /* where the next scan starts: ready fds take turns */
} epoll_t;

static int epoll_close(vfs_file_t *file)
{
	epoll_t     *ep = (epoll_t *)file->inode->priv;
	kfree(ep);
	file->inode->priv = NULL;   /* vfs_close frees the inode (avoid double free) */
	return 0;
}

static vfs_fops_t epoll_fops = { .close = epoll_close };

/* Pins the epoll file: epoll_close frees the watch table, and a sibling thread
 * can close epfd during a wait. Release with fput(*held). */
static epoll_t *epfd_get(int epfd, vfs_file_t **held)
{
	process_t  *p = task_current();
	vfs_file_t *f = p->fds ? fget(p->fds, epfd) : NULL;
	if (!f) return NULL;
	if (!f->inode || f->inode->fops != &epoll_fops) { fput(f); return NULL; }
	*held = f;
	return (epoll_t *)f->inode->priv;
}

/* Why epfd_get failed: no such fd, or one that is not an epoll */
static long epfd_errno(int epfd)
{
	process_t *p = task_current();
	return p->fds && fd_get(p->fds, epfd) ? -EINVAL : -EBADF;
}

/* Watching epoll t from ep: a way back to ep is a cycle (ELOOP), and a chain of
 * more than 5 epolls is EINVAL, as on Linux. A watch is an fd of this process,
 * so the walk follows the fds. */
#define EPOLL_MAX_NESTS 4
static long epoll_check_nest(epoll_t *ep, epoll_t *t, int depth)
{
	if (t == ep) return -ELOOP;
	if (depth > EPOLL_MAX_NESTS) return -EINVAL;
	for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
		uint64_t f = spin_lock_irq(&t->lock);
		int wfd = t->fds[i];
		spin_unlock_irq(&t->lock, f);
		vfs_file_t *held;
		epoll_t *sub = wfd >= 0 ? epfd_get(wfd, &held) : NULL;
		if (!sub) continue;
		long r = epoll_check_nest(ep, sub, depth + 1);
		fput(held);
		if (r) return r;
	}
	return 0;
}

static long sys_epoll_create1(int flags)
{
	if (flags & ~O_CLOEXEC) return -EINVAL;
	epoll_t *ep = kmalloc(sizeof(*ep));
	if (!ep) return -ENOMEM;
	memset(ep, 0, sizeof(*ep));
	ep->lock = (spinlock_t)SPINLOCK_INIT;
	for (int i = 0; i < EPOLL_MAX_WATCH; i++) ep->fds[i] = -1;

	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) { kfree(ep); return -ENOMEM; }
	memset(in, 0, sizeof(*in));
	in->fops     = &epoll_fops;
	in->priv     = ep;
	in->refcount = 1;

	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(ep); kfree(in); return -ENOMEM; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->refcount = 0;   /* fd_alloc takes the +1; a single close() then frees */

	process_t *p = task_current();
	int fd = fd_alloc(p->fds, f);
	if (fd < 0) { kfree(f); kfree(ep); kfree(in); return fd == -1 ? -12 : fd; }
	if (flags & O_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
	return fd;
}

static long epoll_ctl_do(epoll_t *ep, int op, int fd, linux_epoll_event_t ev)
{
	uint64_t f = spin_lock_irq(&ep->lock);

	if (op == EPOLL_CTL_ADD) {
		for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
			if (ep->fds[i] == fd) { spin_unlock_irq(&ep->lock, f); return -EEXIST; }
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
		return -ENOSPC;
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

static long sys_epoll_ctl(int epfd, int op, int fd, linux_epoll_event_t *uevent)
{
	if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_DEL && op != EPOLL_CTL_MOD) return -EINVAL;
	linux_epoll_event_t ev = {0};
	if (op != EPOLL_CTL_DEL && copy_from_user(&ev, uevent, sizeof(ev)) != 0)
		return -EFAULT;
	vfs_file_t *held, *target, *theld;
	epoll_t *ep = epfd_get(epfd, &held);
	if (!ep) return epfd_errno(epfd);
	long r = 0;
	if (fd == epfd) r = -22;
	else if (!(target = fd_get(task_current()->fds, fd))) r = -9;
	else if (op == EPOLL_CTL_ADD) {
		/* a file or directory opened by its path is always ready: Linux will
		 * not watch it. Anonymous inodes (io_uring's) may say S_IFREG too. */
		mode_t m = target->inode ? target->inode->mode : 0;
		int pollable = target->inode && target->inode->fops && target->inode->fops->poll;
		epoll_t *t = epfd_get(fd, &theld);
		if ((S_ISREG(m) || S_ISDIR(m)) && !pollable && target->path[0] == '/') r = -EPERM;
		else if (t) { r = epoll_check_nest(ep, t, 1); fput(theld); }
	}
	if (!r) r = epoll_ctl_do(ep, op, fd, ev);
	fput(held);
	return r;
}

static long epoll_wait_events(int epfd, linux_epoll_event_t *uevents, int maxevents,
                              int timeout_ms)
{
	if (!uevents || maxevents <= 0) return -EINVAL;
	if (!access_ok(uevents, (size_t)maxevents * sizeof(linux_epoll_event_t))) return -EFAULT;
	/* at most EPOLL_MAX_WATCH can be ready, whatever the caller asked for */
	if (maxevents > EPOLL_MAX_WATCH) maxevents = EPOLL_MAX_WATCH;
	vfs_file_t *held;
	epoll_t *ep = epfd_get(epfd, &held);
	if (!ep) return epfd_errno(epfd);

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;

	linux_epoll_event_t *kev = kmalloc((size_t)maxevents * sizeof(*kev));
	if (!kev) { fput(held); return -ENOMEM; }

	int ready = 0;
	while (1) {
		poll_arm_scan();
		ready = 0;
		/* start after the last fd reported, as Linux rotates its ready list:
		 * with more ready than maxevents, every one gets its turn */
		int first = ep->next, last = -1;
		for (int k = 0; k < EPOLL_MAX_WATCH && ready < maxevents; k++) {
			int i = (first + k) % EPOLL_MAX_WATCH;
			uint64_t f = spin_lock_irq(&ep->lock);
			int wfd = ep->fds[i];
			if (wfd < 0 || ep->ev[i] == 0) { spin_unlock_irq(&ep->lock, f); continue; }
			spin_unlock_irq(&ep->lock, f);
			/* a poll fop may block or take other locks: not under ep->lock */
			uint64_t gen = poll_gen, gin = 0, gout = 0;
			int bits = fd_poll_bits(wfd);
			int own = (ep->ev[i] & EPOLLET) && fd_poll_gens(wfd, &gin, &gout);
			f = spin_lock_irq(&ep->lock);
			if (ep->fds[i] != wfd) { spin_unlock_irq(&ep->lock, f); continue; }
			uint32_t r = 0;
			if ((ep->ev[i] & EPOLLIN)  && (bits & POLLIN))  r |= EPOLLIN;
			if ((ep->ev[i] & EPOLLOUT) && (bits & POLLOUT)) r |= EPOLLOUT;
			if ((ep->ev[i] & EPOLLRDHUP) && (bits & EPOLLRDHUP)) r |= EPOLLRDHUP;
			if ((ep->ev[i] & 0x0002) && (bits & 0x0002)) r |= 0x0002;   /* EPOLLPRI */
			if (bits & POLLHUP) r |= EPOLLHUP;   /* always reported (Linux) */
			if (bits & POLLERR) r |= EPOLLERR;

			int report;
			if (ep->ev[i] & EPOLLET) {
				/* edge-trigger: newly ready bits, or new data after a read that
				 * left the fd looking the same: the reader saw EAGAIN and sleeps
				 * until told, so a second arrival must count as an edge */
				report = (r & ~ep->last_ready[i]) != 0;
				if (own)   /* the file says which direction moved (Linux: the wakeup's key) */
					report |= ((r & EPOLLIN) && gin != ep->seen_in[i]) ||
					          ((r & EPOLLOUT) && gout != ep->seen_out[i]);
				else
					report |= r && gen != ep->seen[i];
				ep->last_ready[i] = r;
				if (report) { ep->seen[i] = gen; ep->seen_in[i] = gin; ep->seen_out[i] = gout; }
			} else {
				/* level-trigger: report whenever ready */
				report = (r != 0);
				ep->last_ready[i] = 0;
			}

			if (report) {
				kev[ready].events = r;
				kev[ready].data   = ep->data[i];
				ready++;
				last = i;
				if (ep->ev[i] & EPOLLONESHOT)
					ep->ev[i] = 0; /* disarm until re-armed with MOD */
			}
			spin_unlock_irq(&ep->lock, f);
		}
		if (last >= 0) ep->next = (last + 1) % EPOLL_MAX_WATCH;
		if (ready > 0 || kernel_ticks >= deadline) break;
		if (task_has_actionable_signal()) { kfree(kev); fput(held); return -EINTR; }
		poll_block(deadline);
	}

	if (ready && copy_to_user(uevents, kev, (size_t)ready * sizeof(*kev)) != 0) ready = -14;
	kfree(kev);
	fput(held);
	return ready;
}

static long sys_epoll_pwait(int epfd, linux_epoll_event_t *uevents, int maxevents,
                            int timeout_ms, void *sigmask)
{
	if (sigmask) {
		long m = wait_mask_enter(sigmask);
		if (m) return m;
	}
	return wait_mask_leave(epoll_wait_events(epfd, uevents, maxevents, timeout_ms));
}

/* epoll_pwait2: a timespec instead of milliseconds (NULL waits forever). Rounded
 * up to whole ms, so a short wait never turns into a poll. */
static long sys_epoll_pwait2(int epfd, linux_epoll_event_t *uevents, int maxevents,
                             const linux_timespec_t *ts, void *sigmask)
{
	int ms = -1;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) != 0) return -EFAULT;
		if (k.tv_sec < 0 || k.tv_nsec < 0 || k.tv_nsec >= 1000000000) return -EINVAL;
		uint64_t t = (uint64_t)k.tv_sec * 1000 + ((uint64_t)k.tv_nsec + 999999) / 1000000;
		ms = t > 0x7fffffff ? 0x7fffffff : (int)t;
	}
	return sys_epoll_pwait(epfd, uevents, maxevents, ms, sigmask);
}

/* ── symlink / readlink ───────────────────────────────────── */

static long sys_symlink(const char *t, const char *lp)
{
	char *kt = kmalloc(PATH_MAX);
	if (!kt) return -ENOMEM;
	char *klp = kmalloc(PATH_MAX);
	if (!klp) { kfree(kt); return -ENOMEM; }
	long ret;
	int r;
	if ((r = copy_path_from_user(kt, t)) || (r = resolve_path(klp, AT_FDCWD, lp)))
		ret = r;
	else
		ret = vfs_symlink(kt, klp);
	kfree(kt); kfree(klp);
	return ret;
}
static long sys_symlinkat(const char *t, int d, const char *lp)
{
	char *kt = kmalloc(PATH_MAX);
	if (!kt) return -ENOMEM;
	char *klp = kmalloc(PATH_MAX);
	if (!klp) { kfree(kt); return -ENOMEM; }
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
	if (f & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -EINVAL;
	char *ko = kmalloc(PATH_MAX);
	if (!ko) return -ENOMEM;
	char *kn = kmalloc(PATH_MAX);
	if (!kn) { kfree(ko); return -ENOMEM; }
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
	int r = resolve_path(kp, AT_FDCWD, p);
	if (r) return r;
	return vfs_chmod(kp, m);
}
static long sys_fchmod(int fd, mode_t m)
{
	process_t *proc = task_current();
	vfs_file_t *f = fd_get(proc->fds, fd);
	if (!f || !f->inode) return -EBADF;
	if (f->mnt_flags & MS_RDONLY) return -EROFS;
	return vfs_chmod_inode(f->inode, m);
}
/* fchmodat takes three arguments in the Linux ABI: it has no flags, so whatever
 * a libc left in the fourth register means nothing. fchmodat2 has the flags. */
static long sys_fchmodat(int d, const char *p, mode_t m)
{
	char kp[PATH_MAX];
	int r = resolve_path(kp, d, p);
	if (r) return r;
	return vfs_chmod(kp, m);
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
	if (!f || !f->inode) return -EBADF;
	if (f->mnt_flags & MS_RDONLY) return -EROFS;
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
	if (!access_ok(buf, len)) return -EFAULT;
	return vfs_write(fd, buf, len);
}

static long sys_read(int fd, void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -EFAULT;
	return vfs_read(fd, buf, len);
}

/* writev to a pipe of at most what the pipe takes atomically (PIPE_BUF_SIZE - 1,
 * pipe_write's rule) is one write, as on Linux: gathered here and written once.
 * Written iovec by iovec, a reader could take the first part and close the pipe
 * before the rest came, and the writer died of SIGPIPE although its whole record
 * was sent (musl's printf ends a line with a two-iovec writev). 1: handled. */
static int writev_pipe_atomic(int fd, const iovec_t *iov, int cnt, long *ret)
{
	process_t *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return 0;
	int fifo = f->inode && S_ISFIFO(f->inode->mode);
	fput(f);
	if (!fifo || cnt < 2 || cnt > 8) return 0;   /* stdio uses 2 */

	iovec_t v[8];
	size_t total = 0;
	for (int i = 0; i < cnt; i++) {
		if (copy_from_user(&v[i], &iov[i], sizeof(v[i])) != 0) { *ret = -EFAULT; return 1; }
		if (v[i].iov_len > PIPE_BUF_SIZE - 1 - total) return 0;   /* too big: not atomic */
		total += v[i].iov_len;
	}
	if (total == 0) { *ret = 0; return 1; }
	uint8_t *kbuf = kmalloc(total);
	if (!kbuf) { *ret = -ENOMEM; return 1; }
	size_t off = 0;
	for (int i = 0; i < cnt; i++) {
		if (v[i].iov_len && copy_from_user(kbuf + off, v[i].iov_base, v[i].iov_len) != 0) {
			kfree(kbuf);
			*ret = -EFAULT;
			return 1;
		}
		off += v[i].iov_len;
	}
	*ret = vfs_write(fd, kbuf, total);
	kfree(kbuf);
	return 1;
}

static long sys_writev(int fd, const iovec_t *iov, int cnt)
{
	if (cnt < 0 || cnt > 1024) return -EINVAL;   /* IOV_MAX */

	long total = 0;
	if (writev_pipe_atomic(fd, iov, cnt, &total)) return total;
	for (int i = 0; i < cnt; i++) {
		iovec_t v;
		if (copy_from_user(&v, &iov[i], sizeof(v)) != 0) return -EFAULT;
		if (!v.iov_base || v.iov_len == 0) continue;
		long r = sys_write(fd, v.iov_base, v.iov_len);
		if (r < 0) return total ? total : r;
		total += r;
		if ((size_t)r < v.iov_len) break;   /* later iovecs must not overtake the gap */
	}
	return total;
}

static long sys_sendfile(int out_fd, int in_fd, long *offset, size_t count)
{
	char buf[4096];
	size_t sent = 0;
	long err = 0;
	process_t *p = task_current();
	vfs_file_t *in = fget(p->fds, in_fd);
	if (!in) return -EBADF;
	/* bytes read from these cannot be put back if the write side then fails */
	mode_t m = in->inode->mode;
	int stream = !S_ISREG(m) && !S_ISBLK(m) && !S_ISCHR(m);
	int in_wronly = (in->flags & 3) == O_WRONLY;
	fput(in);
	vfs_file_t *out = fget(p->fds, out_fd);
	mode_t om = out ? out->inode->mode : 0;   /* access mode as vfs_write checks it */
	int out_rdonly = out && (S_ISREG(om) || S_ISBLK(om) || S_ISCHR(om) || S_ISFIFO(om)) &&
	                 (out->flags & 3) == O_RDONLY;
	if (out) fput(out);
	if (!out || out_rdonly || in_wronly) return -EBADF;
	if (stream) return -EINVAL;
	/* With an offset the read starts there and in_fd's own position stays */
	long pos = 0, start = 0;
	if (offset) {
		if (copy_from_user(&start, offset, sizeof(start)) != 0) return -EFAULT;
		if (start < 0) return -EINVAL;
		pos = (long)vfs_seek(in_fd, 0, 1);
		vfs_seek(in_fd, start, 0);
	}
	/* /dev/zero to /dev/null never runs dry: stop at Linux's MAX_RW_COUNT and on
	 * a signal, or this loop cannot be killed */
	if (count > 0x7ffff000UL) count = 0x7ffff000UL;
	while (sent < count) {
		if (task_has_actionable_signal()) { err = -4; break; }
		size_t n = count - sent;
		if (n > sizeof(buf)) n = sizeof(buf);
		long r = vfs_read(in_fd, buf, n);
		if (r <= 0) { err = r; break; }
		long w = vfs_write(out_fd, buf, (size_t)r);
		if (w < r) {
			if (w < 0) err = w;
			else sent += (size_t)w;
			vfs_seek(in_fd, (w < 0 ? 0 : w) - r, 1);
			break;
		}
		sent += (size_t)w;
	}
	if (offset) {
		long off = start + (long)sent;
		vfs_seek(in_fd, pos, 0);
		if (copy_to_user(offset, &off, sizeof(off)) != 0 && !sent) return -EFAULT;
	}
	return sent ? (long)sent : err;
}

static long sys_readv(int fd, const iovec_t *iov, int cnt)
{
	if (cnt < 0 || cnt > 1024) return -EINVAL;   /* IOV_MAX */

	long total = 0;
	for (int i = 0; i < cnt; i++) {
		iovec_t v;
		if (copy_from_user(&v, &iov[i], sizeof(v)) != 0) return -EFAULT;
		if (!v.iov_base || v.iov_len == 0) continue;
		long r = sys_read(fd, v.iov_base, v.iov_len);
		if (r < 0) return total ? total : r;
		total += r;
		if ((size_t)r < v.iov_len) break;   /* a further read could block with data in hand */
	}
	return total;
}

static long proc_fd_reopen(const char *kp, int flags);

static long sys_open(const char *path, int flags, mode_t mode)
{
	char kpath[PATH_MAX];
	int r = resolve_path(kpath, AT_FDCWD, path);
	if (r) return r;
	long fd = vfs_open(kpath, flags, mode);
	if (fd == -2 || fd == -1) {
		long re = proc_fd_reopen(kpath, flags);
		if (re != -2) return re;
	}
	return fd;
}

/* /proc/self/fd/N opens the file itself, not a path to it: a memfd has none. This
 * is how a process makes a read-only descriptor of its own shared memory. */
static long proc_fd_reopen(const char *kp, int flags)
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
	if (*p) return -ENOENT;
	vfs_file_t *of = fd_get(cur->fds, n);
	if (!of || !of->inode || !S_ISREG(of->inode->mode)) return -ENOENT;
	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) return -ENOMEM;
	memset(f, 0, sizeof(*f));
	f->inode = of->inode;
	f->flags = flags & ~0100000;         /* the rest of O_CLOEXEC is handled below */
	atomic_fetch_add32((volatile uint32_t *)&f->inode->open_refs, 1);
	/* a new open of the file: its filesystem counts it as one, close uncounts it */
	vfs_fops_t *ops = f->inode->fops;
	int orc = ops && ops->open ? ops->open(f->inode, f, f->flags) : 0;
	int fd = orc ? orc : fd_alloc(cur->fds, f);
	if (fd < 0) {
		if (!orc && ops && ops->close) ops->close(f);
		atomic_sub_fetch32((volatile uint32_t *)&f->inode->open_refs, 1);
		kfree(f);
		return fd == -1 ? -12 : fd;
	}
	if (flags & 02000000) fd_set_cloexec(cur->fds, fd, 1);
	return fd;
}

static long sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	char kpath[PATH_MAX];
	int r = resolve_path(kpath, dirfd, path);
	if (r) return r;
	long fd = vfs_open(kpath, flags, mode);
	if (fd == -2 || fd == -1) {
		long re = proc_fd_reopen(kpath, flags);
		if (re != -2) return re;
	}
	return fd;
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
	if (first > last || (flags & ~6u)) return -EINVAL;
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
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -EBADF;
	linux_stat_t ks;
	memset(&ks, 0, sizeof(ks));

	vfs_stat_t vs = {0};
	if (f->inode->iops && f->inode->iops->stat) {
		f->inode->iops->stat(f->inode, &vs);
	} else {
		/* Same fallback as vfs_stat, or procfs reports mode 0 through fstat
		 * and programs that check S_ISREG refuse the file. */
		vs.ino   = f->inode->ino;
		vs.mode  = f->inode->mode;
		vs.nlink = f->inode->nlink;
		vs.uid   = f->inode->uid;
		vs.gid   = f->inode->gid;
		vs.size  = f->inode->size;
		vs.rdev  = f->inode->rdev;
	}

	uint32_t f_sdev = f->sdev;
	fput(f);
	ks.st_atime_sec = vs.atime; ks.st_atime_nsec = vs.atime_nsec;
	ks.st_mtime_sec = vs.mtime; ks.st_mtime_nsec = vs.mtime_nsec;
	ks.st_ctime_sec = vs.ctime; ks.st_ctime_nsec = vs.ctime_nsec;
	ks.st_dev     = f_sdev ? ((f_sdev & 0xff) | ((uint64_t)(f_sdev & ~0xffu) << 12)) : 1;
	ks.st_rdev    = vs.rdev;
	ks.st_mode    = vs.mode;
	ks.st_ino     = vs.ino;
	ks.st_size    = (int64_t)vs.size;
	ks.st_nlink   = vs.nlink ? vs.nlink : 1;
	ks.st_uid     = vs.uid;
	ks.st_gid     = vs.gid;
	ks.st_blksize = 4096;
	ks.st_blocks  = (vs.size + 511) / 512;
	return copy_to_user(st, &ks, sizeof(ks)) ? -14 : 0;
}

