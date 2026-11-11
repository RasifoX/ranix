/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB serial ports: the line discipline, byte by byte as it arrives. Input
 * flags (ISTRIP, IGNCR, ICRNL, INLCR); signal characters (ISIG: ^C, ^\, ^Z to
 * the foreground group of the session this port is the terminal of); canonical
 * editing (ICANON: a line is held until newline, EOL or ^D, and ^? and ^U edit
 * it); echo (ECHO, ECHOE, ECHOK, ECHOKE, ECHONL, ECHOCTL).
 *
 * This runs while a transfer completes, where nothing may be sent: echoed bytes
 * wait in a small queue that the usbd thread writes out (ser_kick), and a
 * signal is returned to the caller to send once the port's lock is let go.
 *
 * Part of the xHCI driver: included by drivers/usb/usb_serial.c, itself
 * compiled inside drivers/usb/xhci.c, and not built on its own.
 */

#define US_ISIG   0x0001
#define US_ECHO   0x0008
#define US_ECHOE  0x0010
#define US_ECHOK  0x0020
#define US_ECHONL 0x0040
#define US_NOFLSH 0x0080
#define US_ECHOCTL 0x0200
#define US_ECHOKE 0x0800
#define US_LFLAGS (US_ISIG | US_ICANON | US_ECHO | US_ECHOE | US_ECHOK | US_ECHONL | US_NOFLSH | \
                   US_ECHOCTL | US_ECHOKE)
#define US_VINTR 0
#define US_VQUIT 1
#define US_VERASE 2
#define US_VKILL 3
#define US_VEOF 4
#define US_VSUSP 10
#define US_VEOL 11
#define US_VEOL2 16

static volatile int ser_kick;   /* echoed bytes wait for usbd */

static void echo_raw(userial_t *s, uint8_t c)
{
	uint32_t next = (s->eh + 1) % US_ECHOBUF;
	if (next == s->et) return;                 /* a full queue drops echo, never input */
	s->echo[s->eh] = c;
	s->eh = next;
	ser_kick = 1;
}

/* A character as the terminal shows it: ^X for a control one (ECHOCTL), CR LF
 * for a newline (OPOST, ONLCR). */
static void echo_char(userial_t *s, uint8_t c)
{
	if ((s->lflag & US_ECHOCTL) && (c < 32 || c == 127) && c != '\t' && c != '\n') {
		echo_raw(s, '^');
		echo_raw(s, c == 127 ? '?' : (uint8_t)(c + 64));
		return;
	}
	if (c == '\n' && (s->oflag & US_OPOST) && (s->oflag & US_ONLCR)) echo_raw(s, '\r');
	echo_raw(s, c);
}

static int is_cc(const userial_t *s, int i, uint8_t c) { return s->cc[i] && s->cc[i] == c; }

static void ring_put_byte(userial_t *s, uint8_t c)
{
	if (!ser_room(s)) return;
	s->rx[s->rh] = c;
	s->rh = (s->rh + 1) % US_RXBUF;
}

/* The held line into the buffer readers see. */
static void line_commit(userial_t *s)
{
	for (uint32_t i = 0; i < s->llen; i++) ring_put_byte(s, s->lbuf[i]);
	s->llen = 0;
}

/* A signal character: the signal number, after flushing unless NOFLSH. */
static int ser_signal_char(userial_t *s, uint8_t c)
{
	int sig = is_cc(s, US_VINTR, c) ? 2 : is_cc(s, US_VQUIT, c) ? 3 : is_cc(s, US_VSUSP, c) ? 20 : 0;
	if (!sig) return 0;
	if (!(s->lflag & US_NOFLSH)) { s->llen = 0; s->rt = s->rh; }
	if (s->lflag & US_ECHO) echo_char(s, c);
	return sig;
}

/* Canonical input: edit the held line, or end it. */
static void ser_canon(userial_t *s, uint8_t c)
{
	int echo = s->lflag & US_ECHO;
	if (is_cc(s, US_VERASE, c)) {
		if (s->llen) {
			s->llen--;
			if (echo && (s->lflag & US_ECHOE)) { echo_raw(s, '\b'); echo_raw(s, ' '); echo_raw(s, '\b'); }
			else if (echo) echo_char(s, c);
		}
		return;
	}
	if (is_cc(s, US_VKILL, c)) {
		if (echo && (s->lflag & US_ECHOKE))
			for (uint32_t i = 0; i < s->llen; i++) { echo_raw(s, '\b'); echo_raw(s, ' '); echo_raw(s, '\b'); }
		else if (echo && (s->lflag & US_ECHOK)) echo_char(s, '\n');
		s->llen = 0;
		return;
	}
	if (is_cc(s, US_VEOF, c)) {
		if (!s->llen) s->eof = 1;              /* ^D on an empty line: a read returns 0 */
		line_commit(s);
		return;
	}
	int end = c == '\n' || is_cc(s, US_VEOL, c) || is_cc(s, US_VEOL2, c);
	if (s->llen < US_LINE - 1 || (end && s->llen < US_LINE)) s->lbuf[s->llen++] = c;
	if (echo || (c == '\n' && (s->lflag & US_ECHONL))) echo_char(s, c);
	if (end) line_commit(s);
}

/* One received byte, port lock held. Returns a signal for the caller to send. */
static int ser_input(userial_t *s, uint8_t c)
{
	if (s->iflag & US_ISTRIP) c &= 0x7F;
	if (c == '\r') {
		if (s->iflag & US_IGNCR) return 0;
		if (s->iflag & US_ICRNL) c = '\n';
	} else if (c == '\n' && (s->iflag & US_INLCR)) {
		c = '\r';
	}
	if (s->lflag & US_ISIG) {
		int sig = ser_signal_char(s, c);
		if (sig) return sig;
	}
	if (s->lflag & US_ICANON) {
		ser_canon(s, c);
		return 0;
	}
	ring_put_byte(s, c);
	if (s->lflag & US_ECHO) echo_char(s, c);
	return 0;
}

/* From usbd, controller lock held: what each port has to echo, onto the wire. */
static void ser_echo_out(void)
{
	for (int i = 0; i < US_PORTS; i++) {
		userial_t *s = &g_ser[i];
		uint8_t b[US_ECHOBUF];
		uint32_t n = 0;
		if (!s->registered) continue;
		uint64_t f = spin_lock_irq(&s->lock);
		while (s->et != s->eh) { b[n++] = s->echo[s->et]; s->et = (s->et + 1) % US_ECHOBUF; }
		spin_unlock_irq(&s->lock, f);
		if (n && s->dev) ser_xmit(s, b, n);
	}
}
