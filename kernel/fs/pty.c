#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>

/*
 * Unix98 pseudo-terminals. Opening /dev/ptmx allocates a master/slave pair and
 * returns the master; the slave is /dev/pts/N (N from TIOCGPTN). The two ends
 * are connected by a pair of ring buffers — master writes reach slave reads and
 * vice versa (raw passthrough; canonical line discipline lives in tty_core and
 * is a future refinement). Enough for openpty/forkpty and SSH-style relaying.
 */

#define PTY_MAX   CONFIG_PTY_MAX
#define PTY_BUF   4096
#define NCCS      19

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSCTTY  0x540E
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

typedef struct {
	uint8_t      m2s[PTY_BUF], s2m[PTY_BUF];   /* master→slave, slave→master */
	uint32_t     m2s_h, m2s_t, s2m_h, s2m_t;
	int          in_use, master_open, slave_open;
	int          index;
	spinlock_t   lock;
	wait_queue_t m2s_wait;   /* slave readers / master writers */
	wait_queue_t s2m_wait;   /* master readers / slave writers */
	uint8_t      termios[36];
	uint16_t     win[4];     /* rows, cols, xpix, ypix */
} pty_t;

static pty_t      ptys[PTY_MAX];
static spinlock_t pool_lock = SPINLOCK_INIT;

static uint32_t r_avail(uint32_t h, uint32_t t) { return (h - t) % PTY_BUF; }
static uint32_t r_free(uint32_t h, uint32_t t)  { return PTY_BUF - 1 - r_avail(h, t); }

/* Read from ring [buf,h,t]; block on `wq` until data, or EOF when `peer_open`==0. */
static ssize_t ring_read(pty_t *p, uint8_t *ring, uint32_t *h, uint32_t *t,
                         wait_queue_t *wq, int *peer_open, void *dst, size_t len, int nb)
{
	uint8_t *out = dst;
	size_t got = 0;
	while (got < len) {
		uint64_t f = spin_lock_irq(&p->lock);
		uint32_t avail = r_avail(*h, *t);
		if (avail == 0) {
			if (!*peer_open) { spin_unlock_irq(&p->lock, f); break; }   /* EOF */
			if (nb) { spin_unlock_irq(&p->lock, f); return got ? (ssize_t)got : -11; }
			sleep_on_locked(wq, &p->lock, f);
			continue;
		}
		uint32_t n = avail < (len - got) ? avail : (len - got);
		for (uint32_t i = 0; i < n; i++) { out[got++] = ring[*t]; *t = (*t + 1) % PTY_BUF; }
		spin_unlock_irq(&p->lock, f);
		break;   /* return what we have (tty semantics: don't wait for full len) */
	}
	return (ssize_t)got;
}

/* Write to ring [buf,h,t]; wake `wq` readers. Drops on full (no blocking write
 * needed for our buffer size); returns bytes accepted. */
static ssize_t ring_write(pty_t *p, uint8_t *ring, uint32_t *h, uint32_t *t,
                          wait_queue_t *wq, const void *src, size_t len)
{
	const uint8_t *in = src;
	size_t put = 0;
	uint64_t f = spin_lock_irq(&p->lock);
	uint32_t fr = r_free(*h, *t);
	uint32_t n = fr < len ? fr : len;
	for (uint32_t i = 0; i < n; i++) { ring[*h] = in[put++]; *h = (*h + 1) % PTY_BUF; }
	spin_unlock_irq(&p->lock, f);
	if (put) wake_up(wq);
	return (ssize_t)put;
}

static int pty_common_ioctl(pty_t *p, uint32_t cmd, uint64_t arg)
{
	switch (cmd) {
	case TCGETS:
		return copy_to_user((void *)(uintptr_t)arg, p->termios, 36) ? -14 : 0;
	case TCSETS: case TCSETSW: case TCSETSF:
		return copy_from_user(p->termios, (const void *)(uintptr_t)arg, 36) ? -14 : 0;
	case TIOCGWINSZ:
		return copy_to_user((void *)(uintptr_t)arg, p->win, sizeof(p->win)) ? -14 : 0;
	case TIOCSWINSZ:
		return copy_from_user(p->win, (const void *)(uintptr_t)arg, sizeof(p->win)) ? -14 : 0;
	case TIOCSCTTY:
		return 0;
	}
	return 0;   /* lenient: unknown tty ioctls succeed as no-ops */
}

/* ── master (/dev/ptmx) ──────────────────────────────────────── */

static int ptmx_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	(void)inode; (void)flags;
	uint64_t f = spin_lock_irq(&pool_lock);
	for (int i = 0; i < PTY_MAX; i++) {
		if (!ptys[i].in_use) {
			pty_t *p = &ptys[i];
			p->in_use = 1; p->master_open = 1; p->slave_open = 0;
			p->m2s_h = p->m2s_t = p->s2m_h = p->s2m_t = 0;
			p->index = i;
			memset(p->termios, 0, sizeof(p->termios));
			p->win[0] = 24; p->win[1] = 80; p->win[2] = p->win[3] = 0;
			spin_unlock_irq(&pool_lock, f);
			file->priv = p;
			return 0;
		}
	}
	spin_unlock_irq(&pool_lock, f);
	return -24;   /* EMFILE: pty pool exhausted */
}

static ssize_t master_read(vfs_file_t *file, void *buf, size_t len)
{
	pty_t *p = file->priv;
	return ring_read(p, p->s2m, &p->s2m_h, &p->s2m_t, &p->s2m_wait,
	                 &p->slave_open, buf, len, file->flags & O_NONBLOCK);
}
static ssize_t master_write(vfs_file_t *file, const void *buf, size_t len)
{
	pty_t *p = file->priv;
	return ring_write(p, p->m2s, &p->m2s_h, &p->m2s_t, &p->m2s_wait, buf, len);
}
static int master_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
	pty_t *p = file->priv;
	if (cmd == TIOCGPTN) {
		uint32_t n = (uint32_t)p->index;
		return copy_to_user((void *)(uintptr_t)arg, &n, sizeof(n)) ? -14 : 0;
	}
	if (cmd == TIOCSPTLCK) return 0;   /* we don't lock slaves */
	return pty_common_ioctl(p, cmd, arg);
}
static int master_poll(vfs_file_t *file)
{
	pty_t *p = file->priv;
	return r_avail(p->s2m_h, p->s2m_t) > 0 || !p->slave_open;
}
static int master_close(vfs_file_t *file)
{
	pty_t *p = file->priv;
	/* Clear master_open under p->lock (the slave's EOF read + sleep use it),
	 * but decide slot reuse under pool_lock so it's atomic against a concurrent
	 * pts_open / ptmx_open. Order is always pool_lock -> p->lock. */
	uint64_t pf = spin_lock_irq(&pool_lock);
	uint64_t f  = spin_lock_irq(&p->lock);
	p->master_open = 0;
	int both_closed = !p->slave_open;
	spin_unlock_irq(&p->lock, f);
	if (both_closed) p->in_use = 0;
	spin_unlock_irq(&pool_lock, pf);
	wake_up(&p->m2s_wait);   /* slave readers see EOF */
	return 0;
}

/* ── slave (/dev/pts/N) ──────────────────────────────────────── */

static int pts_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	(void)flags;
	int idx = (int)(inode->rdev & 0xFF);
	if (idx < 0 || idx >= PTY_MAX) return -6;   /* ENXIO */
	pty_t *p = &ptys[idx];
	/* Check in_use and mark the slave attached under pool_lock so a concurrent
	 * master_close + ptmx_open can't free and reuse the slot underneath us. */
	uint64_t pf = spin_lock_irq(&pool_lock);
	if (!p->in_use) { spin_unlock_irq(&pool_lock, pf); return -6; }
	uint64_t f = spin_lock_irq(&p->lock);
	p->slave_open = 1;
	spin_unlock_irq(&p->lock, f);
	spin_unlock_irq(&pool_lock, pf);
	file->priv = p;
	return 0;
}
static ssize_t slave_read(vfs_file_t *file, void *buf, size_t len)
{
	pty_t *p = file->priv;
	return ring_read(p, p->m2s, &p->m2s_h, &p->m2s_t, &p->m2s_wait,
	                 &p->master_open, buf, len, file->flags & O_NONBLOCK);
}
static ssize_t slave_write(vfs_file_t *file, const void *buf, size_t len)
{
	pty_t *p = file->priv;
	return ring_write(p, p->s2m, &p->s2m_h, &p->s2m_t, &p->s2m_wait, buf, len);
}
static int slave_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
	return pty_common_ioctl(file->priv, cmd, arg);
}
static int slave_poll(vfs_file_t *file)
{
	pty_t *p = file->priv;
	return r_avail(p->m2s_h, p->m2s_t) > 0 || !p->master_open;
}
static int slave_close(vfs_file_t *file)
{
	pty_t *p = file->priv;
	uint64_t pf = spin_lock_irq(&pool_lock);
	uint64_t f  = spin_lock_irq(&p->lock);
	p->slave_open = 0;
	int both_closed = !p->master_open;
	spin_unlock_irq(&p->lock, f);
	if (both_closed) p->in_use = 0;
	spin_unlock_irq(&pool_lock, pf);
	wake_up(&p->s2m_wait);   /* master readers see EOF */
	return 0;
}

static vfs_fops_t ptmx_fops = {
	.open = ptmx_open, .read = master_read, .write = master_write,
	.ioctl = master_ioctl, .poll = master_poll, .close = master_close,
};
static vfs_fops_t pts_fops = {
	.open = pts_open, .read = slave_read, .write = slave_write,
	.ioctl = slave_ioctl, .poll = slave_poll, .close = slave_close,
};

void pty_init(void)
{
	extern void devfs_register_device(const char *, mode_t, dev_t, vfs_fops_t *);
	devfs_register_device("ptmx", S_IFCHR | 0666, (5 << 8) | 2, &ptmx_fops);
	static char names[PTY_MAX][8];
	for (int i = 0; i < PTY_MAX; i++) {
		wait_queue_init(&ptys[i].m2s_wait);
		wait_queue_init(&ptys[i].s2m_wait);
		ptys[i].lock = (spinlock_t)SPINLOCK_INIT;
		names[i][0] = 'p'; names[i][1] = 't'; names[i][2] = 's'; names[i][3] = '/';
		names[i][4] = (char)('0' + i); names[i][5] = '\0';
		devfs_register_device(names[i], S_IFCHR | 0620, (136 << 8) | i, &pts_fops);
	}
}
