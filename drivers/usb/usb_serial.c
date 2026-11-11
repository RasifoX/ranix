/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB serial ports: /dev/ttyACM0.. for CDC-ACM boards (Arduino, RP2040, STM32),
 * /dev/ttyUSB0.. for CH340/CH341, CP210x and FTDI adapters. Nodes are 0660
 * root:dialout (gid 20): only root and members of dialout reach a board.
 *
 * The bytes: one bulk IN transfer stays queued, like a keyboard's interrupt
 * endpoint; evt_next hands its completion to ser_rx_event, which moves the bytes
 * into the port's buffer and queues the next one. A full buffer leaves the
 * transfer unqueued, so the device waits (it is told NAK) until a reader makes
 * room. A write is a bulk OUT transfer under the controller lock, in pieces of
 * about half a second of line time so a slow line cannot hold the lock for long.
 *
 * The line: termios (TCSETS, TCSETS2 for any speed) becomes the chip's control
 * requests (userial_proto.c). The first open raises DTR and RTS, which resets an
 * Arduino; the last close drops them when HUPCL is set. TIOCMGET/TIOCMSET and
 * friends move the wires by hand.
 *
 * The terminal: echo, canonical editing and signal characters work as on Linux
 * (usb_serial_ldisc.c), and the port can be a session's controlling terminal
 * (TIOCSCTTY, TIOCSPGRP), so a getty or a shell can run on a board's console.
 * Not here: software flow control (IXON/IXOFF), IEXTEN's ^W ^V ^R, and stopping
 * a background reader (SIGTTIN/SIGTTOU). Bits that are not done are cleared, so
 * TCGETS shows what the port really does.
 *
 * Plug and pull: a node, once made, stays registered (like usb0); it is hidden
 * while nothing is behind it, and a new board takes the first free name of its
 * kind. A board pulled while open leaves its readers at end of file and its
 * writers with EIO, and hangs up its session (SIGHUP); its name is free again
 * after the last close.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

#define US_PORTS  8
#define US_RXBUF  8192
#define US_RXOFF  2048          /* in d->dma: the bulk IN buffer */
#define US_RXLEN  512
#define US_TXOFF  3072          /* in d->dma: the bulk OUT buffer */
#define US_TXLEN  1024
#define US_NCCS   19
#define US_VTIME  5
#define US_VMIN   6
#define US_LINE   4096          /* a canonical line held until it ends */
#define US_ECHOBUF 512

/* termios bits the port acts on */
#define US_ISTRIP 0x0020
#define US_INLCR  0x0040
#define US_IGNCR  0x0080
#define US_ICRNL  0x0100
/* kept: the four above, and IGNBRK, IGNPAR, INPCK (nothing to ignore or check here) */
#define US_IFLAGS (US_ISTRIP | US_INLCR | US_IGNCR | US_ICRNL | 0x0001 | 0x0004 | 0x0010)
#define US_OPOST  0x0001
#define US_ONLCR  0x0004
#define US_ICANON 0x0002

typedef struct userial {
	char       name[12];        /* "ttyACM0": devfs keeps a pointer to it */
	uint8_t    registered, acm;
	usb_dev_t *dev;             /* NULL while nothing is plugged in; under the controller lock */
	us_desc_t  desc;
	uint8_t    rx_started;      /* the first open queued the IN transfer */
	uint8_t    rx_paused;       /* the buffer was full: no transfer queued */
	uint8_t    dtr, rts, excl;
	int        opens;
	uint32_t   msr;             /* TIOCM_CTS/DSR/RNG/CAR as the chip last told */

	spinlock_t lock;            /* the buffer and termios */
	wait_queue_t rxq;
	uint32_t   rh, rt;
	uint8_t    rx[US_RXBUF];
	uint32_t   iflag, oflag, cflag, lflag, ispeed, ospeed;
	uint8_t    line, cc[US_NCCS];
	uint16_t   win[4];
	uint8_t    lbuf[US_LINE];   /* canonical: the line being typed */
	uint32_t   llen;
	uint8_t    eof;             /* ^D on an empty line: the next read returns 0 */
	uint8_t    echo[US_ECHOBUF];
	uint32_t   eh, et;
	pid_t      fg_pgrp;         /* foreground group, when a session's terminal */
} userial_t;

static userial_t g_ser[US_PORTS];
static vfs_fops_t ser_fops;

static uint32_t ser_avail(const userial_t *s) { return (s->rh - s->rt) % US_RXBUF; }
static uint32_t ser_room(const userial_t *s)  { return US_RXBUF - 1 - ser_avail(s); }
static int ser_xmit(userial_t *s, const uint8_t *p, uint32_t n);

#include "usb_serial_ldisc.c"

/* What a fresh port is, as on Linux: 9600 8N1, CREAD, HUPCL, CLOCAL, ICRNL,
 * OPOST+ONLCR, canonical input with echo and signal characters. Raw programs
 * set their own. */
static void ser_defaults(userial_t *s)
{
	s->iflag = US_ICRNL;
	s->oflag = US_OPOST | US_ONLCR;
	s->cflag = 13 | 0x30 | US_CREAD | US_HUPCL | US_CLOCAL;
	s->lflag = US_ISIG | US_ICANON | US_ECHO | US_ECHOE | US_ECHOK | US_ECHOCTL | US_ECHOKE;
	s->ispeed = s->ospeed = 9600;
	s->line = 0;
	memset(s->cc, 0, sizeof s->cc);
	s->cc[0] = 3; s->cc[1] = 0x1C; s->cc[2] = 0x7F; s->cc[3] = 0x15; s->cc[4] = 4;
	s->cc[US_VMIN] = 1;
	s->cc[US_VSUSP] = 0x1A;
	s->llen = 0;
	s->eof = 0;
	s->eh = s->et = 0;
	memset(s->win, 0, sizeof s->win);
}

/* ---- the chip ---- */

/* Send requests in order; 0 when every one went through. Controller lock held. */
static int ser_send(usb_dev_t *d, const us_req_t *r, int n)
{
	int bad = 0;
	for (int i = 0; i < n; i++) {
		int in = r[i].rtype & 0x80;
		if (!in && r[i].len) memcpy(d->dma, r[i].data, r[i].len);
		if (ctrl_xfer(d, r[i].rtype, r[i].req, r[i].val, r[i].idx, r[i].len, in ? 1 : 0) != 0)
			bad++;
	}
	return bad;
}

static int ser_modem(userial_t *s)
{
	us_req_t r[US_MAXREQ];
	return ser_send(s->dev, r, us_modem_reqs(&s->desc, s->dtr, s->rts, r));
}

static int ser_line(userial_t *s)
{
	us_req_t r[US_MAXREQ];
	us_line_t l;
	us_line_from_cflag(s->cflag, s->ospeed, &l);
	return ser_send(s->dev, r, us_line_reqs(&s->desc, &l, r));
}

static void ser_queue_rx(usb_dev_t *d)
{
	ring_push(&d->ep_in, d->dma_phys + US_RXOFF, US_RXLEN, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
	ring_doorbell(d->x, d->slot, (uint32_t)d->dci_in);
}

/* Received bytes through the line discipline. The last signal they raised, or 0. */
static int ser_put(userial_t *s, const uint8_t *p, uint32_t n)
{
	int sig = 0;
	for (uint32_t i = 0; i < n && ser_room(s); i++) {
		int g = ser_input(s, p[i]);
		if (g) sig = g;
	}
	return sig;
}

/* From evt_next, controller lock held: the IN transfer finished. */
static void ser_rx_event(usb_dev_t *d, uint32_t status)
{
	userial_t *s = d->ser;
	uint32_t cc = TRB_GET_CC(status);
	if (cc != CC_SUCCESS && cc != CC_SHORT) {
		d->ser_reset = 1;                  /* usbd clears the halt, outside this wait */
		return;
	}
	d->ser_resets = 0;
	uint32_t got = US_RXLEN - (status & 0xFFFFFF);
	if (got > US_RXLEN) got = 0;
	uint32_t msr = s->msr;
	uint32_t n = us_rx_payload(s->desc.kind, s->desc.mps_in, d->dma + US_RXOFF, got, &msr);
	uint64_t f = spin_lock_irq(&s->lock);
	s->msr = msr;
	int sig = s->opens ? ser_put(s, d->dma + US_RXOFF, n) : 0;   /* nobody listening: dropped */
	int more = ser_room(s) >= US_RXLEN;
	if (!more) s->rx_paused = 1;
	pid_t fg = s->fg_pgrp;
	spin_unlock_irq(&s->lock, f);
	if (sig && fg > 0) task_signal_pgrp(fg, sig);
	if (more) ser_queue_rx(d);
	if (n) {
		wake_up(&s->rxq);
		poll_notify();
	}
}

/* From the usbd thread: an IN endpoint that failed, still plugged in. */
static void ser_recover(usb_dev_t *d)
{
	d->ser_reset = 0;
	if (++d->ser_resets > 8) return;
	ep_recover(d, 1, 1);
	ser_queue_rx(d);
}

/* ---- plug and pull (controller lock held) ---- */

/* A free name of the kind: one that nothing holds, or a new one. */
static userial_t *ser_slot(int acm)
{
	int count = 0;
	for (int i = 0; i < US_PORTS; i++) {
		userial_t *s = &g_ser[i];
		if (!s->registered || s->acm != acm) continue;
		count++;
		if (!s->dev && !s->opens) return s;
	}
	for (int i = 0; i < US_PORTS; i++) {
		userial_t *s = &g_ser[i];
		if (s->registered) continue;
		const char *base = acm ? "ttyACM" : "ttyUSB";
		memcpy(s->name, base, 6);
		s->name[6] = (char)('0' + count);
		s->name[7] = 0;
		s->acm = (uint8_t)acm;
		s->lock = (spinlock_t)SPINLOCK_INIT;
		wait_queue_init(&s->rxq);
		dev_t rdev = (dev_t)((acm ? 166u : 188u) << 8 | (uint32_t)count);
		devfs_register_device_gid_priv(s->name, S_IFCHR | 0660, DEVFS_GID_DIALOUT, rdev, &ser_fops, s);
		s->registered = 1;
		return s;
	}
	return 0;
}

static int serial_attach(usb_dev_t *d, const us_desc_t *u, uint8_t cfgval)
{
	xhci_t *x = d->x;
	userial_t *s = ser_slot(u->kind == US_ACM);
	if (!s) return -1;
	if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, 0) != 0) return -1;   /* SET_CONFIGURATION */
	uint32_t *ic = ctx_ctrl(d), *sc = ctx_slot(d);
	memset(ic, 0, x->ctxsize);
	int dci_in = setup_bulk_ep(d, u->ep_in, u->mps_in, 0, &d->ep_in);
	int dci_out = setup_bulk_ep(d, u->ep_out, u->mps_out, 0, &d->ep_out);
	if (dci_in < 0 || dci_out < 0) return -1;
	int maxdci = dci_in > dci_out ? dci_in : dci_out;
	ic[1] = 1u | 1u << dci_in | 1u << dci_out;
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | (uint32_t)maxdci << 27;
	if (cmd_exec(x, d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)d->slot << 24), 0)
	    != CC_SUCCESS) {
		kprintf(KERN_ERR "xhci: configure serial failed\n");
		return -1;
	}
	d->dci_in = dci_in;
	d->dci_out = dci_out;
	d->addr_in = u->ep_in;
	d->addr_out = u->ep_out;
	d->mps_in = u->mps_in;
	d->mps_out = u->mps_out;
	d->iface = u->data_iface;
	d->ser = s;

	uint64_t f = spin_lock_irq(&s->lock);
	ser_defaults(s);
	s->rh = s->rt = 0;
	spin_unlock_irq(&s->lock, f);
	s->desc = *u;
	s->dtr = s->rts = 0;
	s->excl = 0;
	s->msr = 0;
	s->rx_started = s->rx_paused = 0;
	s->dev = d;
	devfs_set_hidden(s->name, 0);
	static const char *const kind[] = { "", "cdc-acm", "ch341", "cp210x", "ftdi" };
	kprintf(KERN_INFO "xhci: port %d %s is /dev/%s\n", d->port + 1, kind[u->kind], s->name);
	return 0;
}

static void ser_unplug(usb_dev_t *d)
{
	userial_t *s = d->ser;
	s->dev = NULL;
	d->ser = NULL;
	devfs_set_hidden(s->name, 1);
	kprintf(KERN_INFO "xhci: /dev/%s pulled%s\n", s->name, s->opens ? ", still open" : "");
	pid_t fg = s->fg_pgrp;
	s->fg_pgrp = 0;
	tty_hangup(s, fg);                     /* its session loses its terminal: SIGHUP */
	wake_up(&s->rxq);
	poll_notify();
}

/* ---- open and close ---- */

static int ser_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	userial_t *s = in->priv;
	xhci_t *x = &g_xhci;
	if (mutex_lock_killable(&x->lock)) return -EINTR;
	usb_dev_t *d = s->dev;
	int r = 0;
	if (!d) {
		r = -ENXIO;
	} else if (s->excl && s->opens && !capable_task(task_current(), CAP_SYS_ADMIN)) {
		r = -EBUSY;
	} else if (s->opens++ == 0) {
		us_req_t q[US_MAXREQ];
		uint64_t fl = spin_lock_irq(&s->lock);
		s->rh = s->rt = 0;
		s->llen = 0;
		s->eof = 0;
		s->eh = s->et = 0;
		spin_unlock_irq(&s->lock, fl);
		ser_send(d, q, us_open_reqs(&s->desc, q));
		ser_line(s);
		s->dtr = s->rts = us_cflag_baud(s->cflag, s->ospeed) != 0;
		ser_modem(s);
		if (!s->rx_started) {
			s->rx_started = 1;
			ser_queue_rx(d);
		}
	}
	mutex_unlock(&x->lock);
	if (r == 0) f->priv = s;
	return r;
}

static int ser_close(vfs_file_t *f)
{
	userial_t *s = f->priv;
	if (!s) return 0;
	xhci_t *x = &g_xhci;
	mutex_lock(&x->lock);
	if (--s->opens == 0) {
		s->excl = 0;
		if (s->dev) {
			us_req_t q[US_MAXREQ];
			if (s->cflag & US_HUPCL) {
				s->dtr = s->rts = 0;
				ser_modem(s);
			}
			ser_send(s->dev, q, us_close_reqs(&s->desc, q));
		}
	}
	mutex_unlock(&x->lock);
	f->priv = 0;
	return 0;
}

/* ---- read, write, poll ---- */

/* Bytes a read may take now: a whole line in canonical mode (or a full buffer),
 * else what is there once VMIN is met. 0 = wait. */
static uint32_t ser_ready(const userial_t *s, size_t want)
{
	uint32_t n = ser_avail(s);
	if (s->lflag & US_ICANON) {          /* the buffer holds ended lines only */
		for (uint32_t i = 0, p = s->rt; i < n; i++, p = (p + 1) % US_RXBUF) {
			uint8_t c = s->rx[p];
			if (c == '\n' || is_cc(s, US_VEOL, c) || is_cc(s, US_VEOL2, c)) return i + 1;
		}
		return n;                        /* a line ended by ^D, or left from raw mode */
	}
	uint32_t min = s->cc[US_VMIN];
	if (min > want) min = (uint32_t)want;
	return n && n >= min ? n : 0;
}

/* Wait until a read can return; the port lock is held on return. 0, or -errno. */
static int ser_wait(userial_t *s, vfs_file_t *f, size_t want, uint64_t *fl)
{
	uint64_t vt = (uint64_t)s->cc[US_VTIME] * 100000000ULL, end = 0;
	uint32_t seen = 0;
	int raw = !(s->lflag & US_ICANON);
	if (raw && vt && !s->cc[US_VMIN]) end = mono_ns() + vt;   /* overall timer */
	for (;;) {
		if (ser_ready(s, want) || !s->dev || ((s->lflag & US_ICANON) && s->eof)) return 0;
		if (raw && !s->cc[US_VMIN] && !vt) return 0;           /* polling read */
		uint32_t n = ser_avail(s);
		if (raw && vt && s->cc[US_VMIN] && n > seen) {          /* gap timer */
			seen = n;
			end = mono_ns() + vt;
		}
		if (end && mono_ns() >= end) return 0;
		if (f->flags & O_NONBLOCK) return -EAGAIN;
		if (task_has_actionable_signal()) return -EINTR;
		if (end) sleep_on_locked_until_ns(&s->rxq, &s->lock, *fl, end);
		else     sleep_on_locked(&s->rxq, &s->lock, *fl);
		*fl = spin_lock_irq(&s->lock);
	}
}

static ssize_t ser_read(vfs_file_t *f, void *buf, size_t len)
{
	userial_t *s = f->priv;
	uint8_t k[512];
	size_t want = len > sizeof k ? sizeof k : len;
	if (!want) return 0;
	uint64_t fl = spin_lock_irq(&s->lock);
	int r = ser_wait(s, f, want, &fl);
	if (r < 0) {
		spin_unlock_irq(&s->lock, fl);
		return r;
	}
	uint32_t n = ser_ready(s, want);
	if (!n && (s->lflag & US_ICANON) && s->eof) s->eof = 0;  /* ^D: this read is the end */
	if (!n && !(s->lflag & US_ICANON)) n = ser_avail(s);   /* VTIME ran out, or gone */
	if (!n && !s->dev) n = ser_avail(s);                    /* gone: what is left */
	if (n > want) n = (uint32_t)want;
	for (uint32_t i = 0; i < n; i++) {
		k[i] = s->rx[s->rt];
		s->rt = (s->rt + 1) % US_RXBUF;
	}
	int resume = s->rx_paused && ser_room(s) >= US_RXLEN;
	spin_unlock_irq(&s->lock, fl);
	if (resume) {
		mutex_lock(&g_xhci.lock);
		if (s->dev && s->rx_paused) {
			s->rx_paused = 0;
			ser_queue_rx(s->dev);
		}
		mutex_unlock(&g_xhci.lock);
	}
	if (n && copy_fop_to_user(buf, k, n)) return -EFAULT;
	return (ssize_t)n;
}

/* One piece on the wire. Controller lock held. 0 or -EIO. */
static int ser_xmit(userial_t *s, const uint8_t *p, uint32_t n)
{
	usb_dev_t *d = s->dev;
	if (!d) return -EIO;
	memcpy(d->dma + US_TXOFF, p, n);
	uint32_t baud = s->ospeed ? s->ospeed : 9600, got = 0;
	uint32_t ms = 2000 + (uint32_t)((uint64_t)n * 10000 / baud);
	int rc = bulk_xfer_ms(d, 0, d->dma_phys + US_TXOFF, n, &got, ms);
	if (rc == XFER_OK && got == n) return 0;
	ep_recover(d, 0, rc == XFER_STALL);
	return -EIO;
}

static ssize_t ser_write(vfs_file_t *f, const void *buf, size_t len)
{
	userial_t *s = f->priv;
	const uint8_t *u = buf;
	uint8_t in[US_TXLEN / 2], out[US_TXLEN];
	size_t done = 0;
	while (done < len) {
		/* about half a second of line time, so the lock is never held for long */
		uint32_t chunk = (s->ospeed ? s->ospeed : 9600) / 20;
		if (chunk < 16) chunk = 16;
		if (chunk > sizeof in) chunk = sizeof in;
		uint32_t n = len - done < chunk ? (uint32_t)(len - done) : chunk, m = 0;
		if (copy_fop_from_user(in, u + done, n)) return done ? (ssize_t)done : -EFAULT;
		int crlf = (s->oflag & US_OPOST) && (s->oflag & US_ONLCR);
		for (uint32_t i = 0; i < n; i++) {
			if (crlf && in[i] == '\n') out[m++] = '\r';
			out[m++] = in[i];
		}
		if (mutex_lock_killable(&g_xhci.lock)) return done ? (ssize_t)done : -EINTR;
		int r = ser_xmit(s, out, m);
		mutex_unlock(&g_xhci.lock);
		if (r < 0) return done ? (ssize_t)done : r;
		done += n;
		if (done < len && task_has_actionable_signal()) break;
	}
	return (ssize_t)done;
}

static int ser_poll(vfs_file_t *f)
{
	userial_t *s = f->priv;
	if (!s->dev) return 0x0001 | 0x0010;                    /* POLLIN | POLLHUP */
	uint64_t fl = spin_lock_irq(&s->lock);
	/* with VTIME a read returns on the first byte, so one byte is readable */
	int in = (s->lflag & US_ICANON) ? ser_avail(s) != 0 || s->eof :
	         !s->cc[US_VTIME] ? ser_ready(s, US_RXBUF) != 0 : ser_avail(s) != 0;
	spin_unlock_irq(&s->lock, fl);
	return (in ? 0x0001 : 0) | 0x0004;                      /* POLLIN, POLLOUT */
}

#include "usb_serial_ioctl.c"

static vfs_fops_t ser_fops = {
	.open = ser_open, .close = ser_close, .read = ser_read, .write = ser_write,
	.poll = ser_poll, .ioctl = ser_ioctl,
};
