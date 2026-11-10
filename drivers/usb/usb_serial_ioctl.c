/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB serial ports: the terminal ioctls. termios in and out (TCGETS, TCSETS*,
 * and TCGETS2/TCSETS2 for a speed with no Bxxx name, such as 250000), the modem
 * wires (TIOCM*), break, flush, exclusive use and how many bytes wait.
 *
 * Part of the xHCI driver: included by drivers/usb/usb_serial.c, which is itself
 * compiled inside drivers/usb/xhci.c, and not built on its own.
 */

#define US_TCGETS   0x5401
#define US_TCSETS   0x5402
#define US_TCSETSW  0x5403
#define US_TCSETSF  0x5404
#define US_TCSBRK   0x5409
#define US_TCXONC   0x540A
#define US_TCFLSH   0x540B
#define US_TIOCEXCL 0x540C
#define US_TIOCNXCL 0x540D
#define US_TIOCOUTQ 0x5411
#define US_TIOCGWINSZ 0x5413
#define US_TIOCSWINSZ 0x5414
#define US_TIOCMGET 0x5415
#define US_TIOCMBIS 0x5416
#define US_TIOCMBIC 0x5417
#define US_TIOCMSET 0x5418
#define US_FIONREAD 0x541B
#define US_TCSBRKP  0x5425
#define US_TIOCSBRK 0x5427
#define US_TIOCCBRK 0x5428
#define US_TCGETS2  0x802C542Au
#define US_TCSETS2  0x402C542Bu
#define US_TCSETSW2 0x402C542Cu
#define US_TCSETSF2 0x402C542Du

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32le(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* termios (36 bytes) or termios2 (44: with the two speeds) */
static void ser_pack(userial_t *s, uint8_t *t, int two)
{
	memset(t, 0, 44);
	uint64_t f = spin_lock_irq(&s->lock);
	wr32le(t, s->iflag);
	wr32le(t + 4, s->oflag);
	wr32le(t + 8, s->cflag);
	wr32le(t + 12, s->lflag);
	t[16] = s->line;
	memcpy(t + 17, s->cc, US_NCCS);
	if (two) {
		wr32le(t + 36, s->ispeed);
		wr32le(t + 40, s->ospeed);
	}
	spin_unlock_irq(&s->lock, f);
}

/* A new termios: keep what the port does, tell the chip, move the wires on B0. */
static int ser_set(userial_t *s, const uint8_t *t, int two, int flush)
{
	if (mutex_lock_killable(&g_xhci.lock)) return -EINTR;
	if (!s->dev) {
		mutex_unlock(&g_xhci.lock);
		return -EIO;
	}
	uint32_t was = us_cflag_baud(s->cflag, s->ospeed);
	uint32_t cflag = us_cflag_fix(s->desc.kind, rd32le(t + 8)) & ~US_CIBAUD;
	uint64_t f = spin_lock_irq(&s->lock);
	s->iflag = rd32le(t) & US_IFLAGS;
	s->oflag = rd32le(t + 4) & (US_OPOST | US_ONLCR);
	s->lflag = rd32le(t + 12) & US_ICANON;
	s->line = t[16];
	memcpy(s->cc, t + 17, US_NCCS);
	s->cflag = cflag;
	s->ospeed = us_cflag_baud(cflag, two ? rd32le(t + 40) : s->ospeed);
	s->ispeed = s->ospeed;
	if (flush) s->rh = s->rt;
	spin_unlock_irq(&s->lock, f);
	if (flush && s->rx_paused) {
		s->rx_paused = 0;
		ser_queue_rx(s->dev);
	}
	int bad = ser_line(s);
	if (!s->ospeed || !was) {
		s->dtr = s->rts = s->ospeed != 0;
		bad += ser_modem(s);
	}
	mutex_unlock(&g_xhci.lock);
	return bad ? -EIO : 0;
}

/* The input wires, asked of the chips that answer; ACM and FTDI keep what they sent. */
static uint32_t ser_mget(userial_t *s)
{
	usb_dev_t *d = s->dev;
	if (d && s->desc.kind == US_CP210X &&
	    ctrl_xfer(d, 0xC1, 0x08, 0, s->desc.ctrl_iface, 1, 1) == 0) {
		uint8_t b = d->dma[0];
		s->msr = (b & 0x10 ? US_TIOCM_CTS : 0) | (b & 0x20 ? US_TIOCM_DSR : 0) |
		         (b & 0x40 ? US_TIOCM_RNG : 0) | (b & 0x80 ? US_TIOCM_CAR : 0);
	} else if (d && s->desc.kind == US_CH341 && ctrl_xfer(d, 0xC0, 0x95, 0x0706, 0, 2, 1) == 0) {
		uint8_t b = (uint8_t)~d->dma[0];      /* active low */
		s->msr = (b & 0x01 ? US_TIOCM_CTS : 0) | (b & 0x02 ? US_TIOCM_DSR : 0) |
		         (b & 0x04 ? US_TIOCM_RNG : 0) | (b & 0x08 ? US_TIOCM_CAR : 0);
	}
	return s->msr | (s->dtr ? US_TIOCM_DTR : 0) | (s->rts ? US_TIOCM_RTS : 0);
}

static int ser_wires(userial_t *s, uint32_t cmd, uint64_t arg)
{
	uint32_t v = 0;
	if (cmd != US_TIOCMGET && copy_from_user(&v, (const void *)(uintptr_t)arg, 4)) return -EFAULT;
	if (mutex_lock_killable(&g_xhci.lock)) return -EINTR;
	int r = 0;
	if (!s->dev) {
		r = -EIO;
	} else if (cmd == US_TIOCMGET) {
		v = ser_mget(s);
	} else {
		uint32_t m = (s->dtr ? US_TIOCM_DTR : 0) | (s->rts ? US_TIOCM_RTS : 0);
		if (cmd == US_TIOCMBIS) m |= v;
		else if (cmd == US_TIOCMBIC) m &= ~v;
		else m = v;
		s->dtr = !!(m & US_TIOCM_DTR);
		s->rts = !!(m & US_TIOCM_RTS);
		r = ser_modem(s) ? -EIO : 0;
	}
	mutex_unlock(&g_xhci.lock);
	if (r == 0 && cmd == US_TIOCMGET && copy_to_user((void *)(uintptr_t)arg, &v, 4)) return -EFAULT;
	return r;
}

static int ser_break(userial_t *s, int on)
{
	us_req_t r[US_MAXREQ];
	if (mutex_lock_killable(&g_xhci.lock)) return -EINTR;
	int n = s->dev ? us_break_reqs(&s->desc, on, r) : 0;
	int rc = !s->dev ? -EIO : n < 0 ? -EOPNOTSUPP : ser_send(s->dev, r, n) ? -EIO : 0;
	mutex_unlock(&g_xhci.lock);
	return rc;
}

/* A break of ms milliseconds; the sleep is outside the controller lock. */
static int ser_break_for(userial_t *s, uint32_t ms)
{
	int r = ser_break(s, 1);
	if (r) return r;
	task_sleep_until(kernel_ticks + (uint64_t)ms * TIMER_HZ / 1000);
	return ser_break(s, 0);
}

static int ser_flush(userial_t *s, uint64_t which)
{
	if (which > 2) return -EINVAL;
	if (which == 1) return 0;                 /* output: writes are already on the wire */
	if (mutex_lock_killable(&g_xhci.lock)) return -EINTR;
	uint64_t f = spin_lock_irq(&s->lock);
	s->rh = s->rt;
	spin_unlock_irq(&s->lock, f);
	if (s->dev && s->rx_paused) {
		s->rx_paused = 0;
		ser_queue_rx(s->dev);
	}
	mutex_unlock(&g_xhci.lock);
	return 0;
}

static int ser_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	userial_t *s = f->priv;
	void *ua = (void *)(uintptr_t)arg;
	uint8_t t[44];
	int two = cmd == US_TCGETS2 || cmd == US_TCSETS2 || cmd == US_TCSETSW2 || cmd == US_TCSETSF2;
	switch (cmd) {
	case US_TCGETS: case US_TCGETS2:
		ser_pack(s, t, two);
		return copy_to_user(ua, t, two ? 44 : 36) ? -EFAULT : 0;
	case US_TCSETS: case US_TCSETSW: case US_TCSETSF:
	case US_TCSETS2: case US_TCSETSW2: case US_TCSETSF2:
		if (copy_from_user(t, ua, two ? 44 : 36)) return -EFAULT;
		return ser_set(s, t, two, cmd == US_TCSETSF || cmd == US_TCSETSF2);
	case US_TIOCMGET: case US_TIOCMBIS: case US_TIOCMBIC: case US_TIOCMSET:
		return ser_wires(s, cmd, arg);
	case US_TCSBRK:
		return arg ? 0 : ser_break_for(s, 250);  /* nonzero: drain, and writes never wait */
	case US_TCSBRKP:
		return ser_break_for(s, arg ? (uint32_t)(arg > 100 ? 100 : arg) * 100 : 250);
	case US_TIOCSBRK: case US_TIOCCBRK:
		return ser_break(s, cmd == US_TIOCSBRK);
	case US_TCFLSH:
		return ser_flush(s, arg);
	case US_TCXONC:
		return arg == 1 || arg == 3 ? 0 : -EOPNOTSUPP;   /* nothing is ever stopped */
	case US_TIOCEXCL: case US_TIOCNXCL:
		s->excl = cmd == US_TIOCEXCL;
		return 0;
	case US_FIONREAD: case US_TIOCOUTQ: {
		uint64_t fl = spin_lock_irq(&s->lock);
		int n = cmd == US_FIONREAD ? (int)ser_avail(s) : 0;
		spin_unlock_irq(&s->lock, fl);
		return copy_to_user(ua, &n, 4) ? -EFAULT : 0;
	}
	case US_TIOCGWINSZ:
		return copy_to_user(ua, s->win, sizeof s->win) ? -EFAULT : 0;
	case US_TIOCSWINSZ:
		return copy_from_user(s->win, ua, sizeof s->win) ? -EFAULT : 0;
	}
	return -ENOTTY;
}
