#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/config.h>

/*
 * Unix98 pseudo-terminals. Opening /dev/ptmx allocates a master/slave pair and
 * returns the master; the slave is /dev/pts/N (N from TIOCGPTN). The two ends
 * are connected by a pair of ring buffers — master writes reach slave reads and
 * vice versa. The slave carries an N_TTY line discipline: master writes pass
 * through canonical line buffering (ICANON), echo, ERASE/^U editing, ^D EOF and
 * ISIG signal chars before a slave read sees them; slave writes get ONLCR output
 * processing on the way back to the master.
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
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

/* c_iflag */
#define ICRNL   0x0100
#define INLCR   0x0040
#define IGNCR   0x0080
/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004
/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
/* c_cc */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VSUSP   10

typedef struct {
	uint8_t      m2s[PTY_BUF], s2m[PTY_BUF];   /* master→slave, slave→master */
	uint32_t     m2s_h, m2s_t, s2m_h, s2m_t;
	int          in_use, master_open, slave_open;
	int          index;
	spinlock_t   lock;
	wait_queue_t m2s_wait;   /* slave readers / master writers */
	wait_queue_t s2m_wait;   /* master readers / slave writers */

	uint32_t     c_iflag, c_oflag, c_cflag, c_lflag;
	uint8_t      c_line;
	uint8_t      c_cc[NCCS];

	uint8_t      line[PTY_BUF];   /* pending canonical line (not yet committed) */
	uint32_t     line_len;
	int          eof;             /* ^D with an empty line → a 0-byte read (EOF) */

	uint16_t     win[4];     /* rows, cols, xpix, ypix */
	pid_t        fg_pgrp;    /* foreground process group (job control) */
} pty_t;

static pty_t      ptys[PTY_MAX];
static spinlock_t pool_lock = SPINLOCK_INIT;

static uint32_t r_avail(uint32_t h, uint32_t t) { return (h - t) % PTY_BUF; }
static uint32_t r_free(uint32_t h, uint32_t t)  { return PTY_BUF - 1 - r_avail(h, t); }

static void ring_put(uint8_t *ring, uint32_t *h, uint32_t t, uint8_t c)
{
	if (r_free(*h, t) == 0) return;
	ring[*h] = c; *h = (*h + 1) % PTY_BUF;
}

extern int task_has_actionable_signal(void);

/* Read from ring [buf,h,t]; block on `wq` until data, EOF when `peer_open`==0,
 * or interrupt on a pending signal (EINTR). */
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
			if (task_has_actionable_signal()) return got ? (ssize_t)got : -4;
			continue;
		}
		uint32_t n = avail < (len - got) ? avail : (len - got);
		for (uint32_t i = 0; i < n; i++) { out[got++] = ring[*t]; *t = (*t + 1) % PTY_BUF; }
		spin_unlock_irq(&p->lock, f);
		break;   /* return what we have (tty semantics: don't wait for full len) */
	}
	return (ssize_t)got;
}

/* Read from the slave end: an N_TTY-cooked m2s stream. In canonical mode a read
 * returns at most one line and blocks until a line (or EOF/signal) is ready. */
static ssize_t slave_cooked_read(pty_t *p, void *dst, size_t len, int nb)
{
	if (!(p->c_lflag & ICANON))
		return ring_read(p, p->m2s, &p->m2s_h, &p->m2s_t, &p->m2s_wait,
		                 &p->master_open, dst, len, nb);

	uint8_t *out = dst;
	size_t got = 0;
	while (got < len) {
		uint64_t f = spin_lock_irq(&p->lock);
		uint32_t avail = r_avail(p->m2s_h, p->m2s_t);
		if (avail == 0) {
			if (p->eof) { p->eof = 0; spin_unlock_irq(&p->lock, f); break; }
			if (!p->master_open) { spin_unlock_irq(&p->lock, f); break; }
			if (nb) { spin_unlock_irq(&p->lock, f); return got ? (ssize_t)got : -11; }
			sleep_on_locked(&p->m2s_wait, &p->lock, f);
			if (task_has_actionable_signal()) return got ? (ssize_t)got : -4;
			continue;
		}
		uint8_t c = p->m2s[p->m2s_t];
		p->m2s_t = (p->m2s_t + 1) % PTY_BUF;
		out[got++] = c;
		spin_unlock_irq(&p->lock, f);
		if (c == '\n') break;   /* one line per canonical read */
	}
	return (ssize_t)got;
}

/* Write to ring [buf,h,t]; wake `wq` readers. Drops on full; returns bytes
 * accepted. */
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

/* echo a byte back toward the master (slave→master ring) */
static void echo_byte(pty_t *p, uint8_t c)
{
	if (c == '\n') ring_put(p->s2m, &p->s2m_h, p->s2m_t, '\r');
	ring_put(p->s2m, &p->s2m_h, p->s2m_t, c);
}

static void echo_erase(pty_t *p)
{
	ring_put(p->s2m, &p->s2m_h, p->s2m_t, '\b');
	ring_put(p->s2m, &p->s2m_h, p->s2m_t, ' ');
	ring_put(p->s2m, &p->s2m_h, p->s2m_t, '\b');
}

/* Master write: feed bytes through the line discipline into the slave's m2s
 * stream. Returns bytes consumed (always len; full drops are silent like the
 * raw path). */
static ssize_t ldisc_input(pty_t *p, const void *src, size_t len)
{
	const uint8_t *in = src;
	int wake_slave = 0, wake_master = 0;
	uint64_t f = spin_lock_irq(&p->lock);

	for (size_t i = 0; i < len; i++) {
		uint8_t c = in[i];

		if (c == '\r') {
			if (p->c_iflag & IGNCR) continue;
			if (p->c_iflag & ICRNL) c = '\n';
		} else if (c == '\n' && (p->c_iflag & INLCR)) {
			c = '\r';
		}

		if (p->c_lflag & ISIG) {
			int sig = 0;
			if (c == p->c_cc[VINTR])      sig = 2;
			else if (c == p->c_cc[VQUIT]) sig = 3;
			else if (c == p->c_cc[VSUSP]) sig = 20;
			if (sig) {
				pid_t pg = p->fg_pgrp;
				spin_unlock_irq(&p->lock, f);
				if (pg > 0) task_signal_pgrp(pg, sig);
				f = spin_lock_irq(&p->lock);
				continue;
			}
		}

		if (!(p->c_lflag & ICANON)) {
			ring_put(p->m2s, &p->m2s_h, p->m2s_t, c); wake_slave = 1;
			if (p->c_lflag & ECHO) { echo_byte(p, c); wake_master = 1; }
			continue;
		}

		if (c == p->c_cc[VERASE] || c == 0x7F || c == '\b') {
			if (p->line_len > 0) {
				p->line_len--;
				if (p->c_lflag & ECHO) { echo_erase(p); wake_master = 1; }
			}
			continue;
		}
		if (c == p->c_cc[VKILL]) {
			while (p->line_len > 0) {
				p->line_len--;
				if (p->c_lflag & ECHO) { echo_erase(p); wake_master = 1; }
			}
			continue;
		}
		if (c == p->c_cc[VEOF]) {
			if (p->line_len == 0) { p->eof = 1; wake_slave = 1; }
			else {
				for (uint32_t j = 0; j < p->line_len; j++)
					ring_put(p->m2s, &p->m2s_h, p->m2s_t, p->line[j]);
				p->line_len = 0; wake_slave = 1;
			}
			continue;
		}
		if (c == '\n') {
			if (p->c_lflag & ECHO) { echo_byte(p, '\n'); wake_master = 1; }
			for (uint32_t j = 0; j < p->line_len; j++)
				ring_put(p->m2s, &p->m2s_h, p->m2s_t, p->line[j]);
			ring_put(p->m2s, &p->m2s_h, p->m2s_t, '\n');
			p->line_len = 0; wake_slave = 1;
			continue;
		}
		if (p->line_len < sizeof(p->line)) {
			p->line[p->line_len++] = c;
			if (p->c_lflag & ECHO) { echo_byte(p, c); wake_master = 1; }
		}
	}

	spin_unlock_irq(&p->lock, f);
	if (wake_slave)  wake_up(&p->m2s_wait);
	if (wake_master) wake_up(&p->s2m_wait);
	return (ssize_t)len;
}

/* Slave write: OPOST/ONLCR cook on the way back to the master. */
static ssize_t ldisc_output(pty_t *p, const void *src, size_t len)
{
	const uint8_t *in = src;
	if (!(p->c_oflag & OPOST))
		return ring_write(p, p->s2m, &p->s2m_h, &p->s2m_t, &p->s2m_wait, src, len);

	uint64_t f = spin_lock_irq(&p->lock);
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '\n' && (p->c_oflag & ONLCR))
			ring_put(p->s2m, &p->s2m_h, p->s2m_t, '\r');
		ring_put(p->s2m, &p->s2m_h, p->s2m_t, in[i]);
	}
	spin_unlock_irq(&p->lock, f);
	wake_up(&p->s2m_wait);
	return (ssize_t)len;
}

static void termios_pack(pty_t *p, uint8_t t[36])
{
	memset(t, 0, 36);
	((uint32_t *)t)[0] = p->c_iflag;
	((uint32_t *)t)[1] = p->c_oflag;
	((uint32_t *)t)[2] = p->c_cflag;
	((uint32_t *)t)[3] = p->c_lflag;
	t[16] = p->c_line;
	memcpy(t + 17, p->c_cc, NCCS);
}

static void termios_unpack(pty_t *p, const uint8_t t[36])
{
	p->c_iflag = ((const uint32_t *)t)[0];
	p->c_oflag = ((const uint32_t *)t)[1];
	p->c_cflag = ((const uint32_t *)t)[2];
	p->c_lflag = ((const uint32_t *)t)[3];
	p->c_line  = t[16];
	memcpy(p->c_cc, t + 17, NCCS);
}

static int pty_common_ioctl(pty_t *p, uint32_t cmd, uint64_t arg)
{
	switch (cmd) {
	case TCGETS: {
		uint8_t t[36];
		termios_pack(p, t);
		return copy_to_user((void *)(uintptr_t)arg, t, 36) ? -14 : 0;
	}
	case TCSETS: case TCSETSW: case TCSETSF: {
		uint8_t t[36];
		if (copy_from_user(t, (const void *)(uintptr_t)arg, 36)) return -14;
		uint64_t f = spin_lock_irq(&p->lock);
		termios_unpack(p, t);
		spin_unlock_irq(&p->lock, f);
		return 0;
	}
	case TIOCGWINSZ:
		return copy_to_user((void *)(uintptr_t)arg, p->win, sizeof(p->win)) ? -14 : 0;
	case TIOCSWINSZ:
		return copy_from_user(p->win, (const void *)(uintptr_t)arg, sizeof(p->win)) ? -14 : 0;
	case TIOCSCTTY: {
		/* Acquiring the controlling terminal makes the caller's process group
		 * the foreground group, so an interactive shell's tcgetpgrp() matches
		 * its own pgrp and it skips the SIGTTIN job-control wait. */
		extern vfs_fops_t *pts_fops_ptr(void);
		process_t *cur = task_current();
		if (cur && cur->pgrp > 0) p->fg_pgrp = cur->pgrp;
		if (cur) { cur->ctty = p; cur->ctty_fops = pts_fops_ptr(); }
		return 0;
	}
	case TIOCGPGRP:
		return copy_to_user((void *)(uintptr_t)arg, &p->fg_pgrp, sizeof(p->fg_pgrp)) ? -14 : 0;
	case TIOCSPGRP: {
		pid_t pgrp;
		if (copy_from_user(&pgrp, (const void *)(uintptr_t)arg, sizeof(pgrp))) return -14;
		p->fg_pgrp = pgrp;
		return 0;
	}
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
			p->fg_pgrp = 0;
			p->line_len = 0; p->eof = 0;
			p->c_iflag = ICRNL;
			p->c_oflag = OPOST | ONLCR;
			p->c_cflag = 0x04BF;
			p->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
			p->c_line  = 0;
			static const uint8_t def_cc[NCCS] = {
				0x03, 0x1C, 0x7F, 0x15, 0x04, 0x00, 0x01, 0x00, 0x11, 0x13,
				0x1A, 0x00, 0x12, 0x0F, 0x17, 0x16, 0x00, 0x00, 0x00 };
			memcpy(p->c_cc, def_cc, NCCS);
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
	return ldisc_input(p, buf, len);
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
	return slave_cooked_read(p, buf, len, file->flags & O_NONBLOCK);
}
static ssize_t slave_write(vfs_file_t *file, const void *buf, size_t len)
{
	pty_t *p = file->priv;
	return ldisc_output(p, buf, len);
}
static int slave_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
	return pty_common_ioctl(file->priv, cmd, arg);
}
static int slave_poll(vfs_file_t *file)
{
	pty_t *p = file->priv;
	if (p->c_lflag & ICANON) {
		/* readable only once a full line (or EOF) is committed to m2s */
		return r_avail(p->m2s_h, p->m2s_t) > 0 || p->eof || !p->master_open;
	}
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

vfs_fops_t *pts_fops_ptr(void) { return &pts_fops; }

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
