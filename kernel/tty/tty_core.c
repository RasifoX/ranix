#include <kernel/tty/tty_core.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/kprintf.h>
#include <drivers/serial/uart.h>
#include <kernel/fs/devfs.h>
#include <kernel/proc.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/input.h>
#include <kernel/tick.h>

/* c_lflag bits */
#define ISIG    0x00000001
#define ICANON  0x00000002
#define ECHO    0x00000008
#define ECHOE   0x00000010
#define ECHOK   0x00000020
#define ECHOCTL 0x00000200
#define ECHOKE  0x00000800
#define IEXTEN  0x00008000

/* c_cc indices (Linux AArch64) */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSUSP   10
#define NCCS    19

#define TTY_BUF_SIZE 65536

static char input_buf[TTY_BUF_SIZE];
static int  in_head = 0;
static int  in_tail = 0;

static char line_buf[TTY_BUF_SIZE];
static int  line_pos = 0;
static int  icanon_esc = 0;

static uint32_t c_iflag = 0x0500;   /* ICRNL | IXON */
static uint32_t c_oflag = 0x0005;   /* OPOST | ONLCR */
static uint32_t c_cflag = 0x04BF;   /* B38400 | CS8 | CREAD | HUPCL | CLOCAL */
static uint32_t c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
static uint8_t  c_line  = 0;
static uint8_t  c_cc[NCCS] = {
	0x03, 0x1C, 0x7F, 0x15, 0x04,   /* VINTR VQUIT VERASE VKILL VEOF */
	0x00, 0x01, 0x00, 0x11, 0x13,   /* VTIME VMIN VSWTC VSTART VSTOP */
	0x1A, 0x00, 0x12, 0x0F, 0x17,   /* VSUSP VEOL VREPRINT VDISCARD VWERASE */
	0x16, 0x00, 0x00, 0x00,          /* VLNEXT VEOL2 ... */
};

static pid_t fg_pgrp = 1;

static spinlock_t tty_lock = SPINLOCK_INIT;

static void tty_echo_str(const char *s, int len)
{
	fb_tty_write(s, len);
	for (int i = 0; i < len; i++) {
		if (s[i] == '\n') uart_putc('\r');
		uart_putc(s[i]);
	}
}

static void tty_echo_char(char c)
{
	tty_echo_str(&c, 1);
}

void tty_input_char(char c)
{
	uint64_t f = spin_lock_irq(&tty_lock);
	
	/* Signal generation (both canonical and raw modes) */
	if (c_lflag & ISIG) {
		unsigned char uc2 = (unsigned char)c;
		if (uc2 == c_cc[VINTR] || uc2 == c_cc[VQUIT] || uc2 == c_cc[VSUSP]) {
			int sig = (uc2 == c_cc[VINTR]) ? 2 :
			          (uc2 == c_cc[VQUIT]) ? 3 : 20;
			spin_unlock_irq(&tty_lock, f);
			task_signal_pgrp(fg_pgrp, sig);
			return;
		}
	}

	input_inject_ascii((uint8_t)c);

	if (!(c_lflag & ICANON)) {
		int next = (in_head + 1) % TTY_BUF_SIZE;
		if (next != in_tail) {
			input_buf[in_head] = c;
			in_head = next;
		}
		spin_unlock_irq(&tty_lock, f);
		return;
	}

	/* Discard terminal responses (e.g. \033[row;colR from PuTTY) in ICANON */
	unsigned char uc = (unsigned char)c;
	if (icanon_esc == 1) {
		icanon_esc = (uc == '[' || uc == ']') ? 2 : 0;
		spin_unlock_irq(&tty_lock, f);
		return;
	}
	if (icanon_esc == 2) {
		if ((uc >= 0x40 && uc <= 0x7E) || uc == 0x07) icanon_esc = 0;
		spin_unlock_irq(&tty_lock, f);
		return;
	}
	if (uc == 0x1B) {
		icanon_esc = 1;
		spin_unlock_irq(&tty_lock, f);
		return;
	}

	if (c == 127 || c == '\b') {
		if (line_pos > 0) {
			int n_bytes = 1;
			while (n_bytes < line_pos && (line_buf[line_pos - n_bytes] & 0xC0) == 0x80)
				n_bytes++;
			line_pos -= n_bytes;
			if (c_lflag & ECHO) tty_echo_str("\b \b", 3);
		}
	} else if (c == '\r' || c == '\n') {
		if (c_lflag & ECHO) tty_echo_char('\n');
		line_buf[line_pos++] = '\n';
		for (int i = 0; i < line_pos; i++) {
			int next = (in_head + 1) % TTY_BUF_SIZE;
			if (next != in_tail) {
				input_buf[in_head] = line_buf[i];
				in_head = next;
			}
		}
		line_pos = 0;
	} else {
		if (line_pos < TTY_BUF_SIZE - 1) {
			line_buf[line_pos++] = c;
			if (c_lflag & ECHO) tty_echo_char(c);
		}
	}
	spin_unlock_irq(&tty_lock, f);
}

#if defined(__x86_64__)
extern char ps2_kbd_getc(void);
extern int  ps2_kbd_has_data(void);
#endif

void tty_poll_inputs(void)
{
	int got = 0;
	while (uart_has_data()) {
		char c = uart_getc();
		tty_input_char(c);
		got = 1;
	}
#if defined(__x86_64__)
	while (ps2_kbd_has_data()) {
		char c = ps2_kbd_getc();
		tty_input_char(c);
		got = 1;
	}
#endif
	if (got) fb_tty_flush();
}

static ssize_t tty_core_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	char *b = buf;
	size_t copied = 0;

	while (copied < len) {
		tty_poll_inputs();
		
		uint64_t flags = spin_lock_irq(&tty_lock);
		if (in_head != in_tail) {
			b[copied++] = input_buf[in_tail];
			in_tail = (in_tail + 1) % TTY_BUF_SIZE;
			spin_unlock_irq(&tty_lock, flags);
		} else {
			spin_unlock_irq(&tty_lock, flags);
			if (copied > 0) break;
			if (f->flags & O_NONBLOCK) return -11; /* EAGAIN */
			fb_tty_flush(); /* display pending text before sleeping */
			task_sleep();
		}
	}
	return copied;
}

static void tty_inject_str(const char *s)
{
	while (*s) {
		int next = (in_head + 1) % TTY_BUF_SIZE;
		if (next == in_tail) break;
		input_buf[in_head] = *s++;
		in_head = next;
	}
}

static void tty_inject_uint(unsigned int v)
{
	char tmp[12]; int i = 0;
	if (v == 0) { tty_inject_str("0"); return; }
	while (v > 0 && i < 12) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	for (int j = i - 1; j >= 0; j--) {
		int next = (in_head + 1) % TTY_BUF_SIZE;
		if (next == in_tail) break;
		input_buf[in_head] = tmp[j];
		in_head = next;
	}
}

static ssize_t tty_core_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	const char *p = buf;
	/* scan for \033[6n (cursor position query) and inject response */
	for (size_t i = 0; i + 2 < len; i++) {
		if (p[i] == '\033' && p[i+1] == '[' && p[i+2] == '6' &&
		    i + 3 < len && p[i+3] == 'n') {
			int col = 1, row = 1;
			fb_tty_get_cursor(&col, &row);
			uint64_t flags = spin_lock_irq(&tty_lock);
			tty_inject_str("\033[");
			tty_inject_uint((unsigned int)(row + 1));
			tty_inject_str(";");
			tty_inject_uint((unsigned int)(col + 1));
			tty_inject_str("R");
			spin_unlock_irq(&tty_lock, flags);
			break;
		}
	}
	tty_echo_str(buf, len);
	fb_tty_flush();
	return len;
}

static int tty_core_poll(vfs_file_t *f)
{
	(void)f;
	tty_poll_inputs();
	return (in_head != in_tail) ? 1 : 0;
}

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

#define TCGETS   0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540f
#define TIOCSPGRP  0x5410
#define TIOCSCTTY  0x540e

static int tty_core_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	if (cmd == TCGETS) {
		/* kernel struct termios: 4×uint32 flags + 1 c_line + 19 c_cc = 36 bytes */
		uint8_t termios[36];
		memset(termios, 0, sizeof(termios));
		((uint32_t *)termios)[0] = c_iflag;
		((uint32_t *)termios)[1] = c_oflag;
		((uint32_t *)termios)[2] = c_cflag;
		((uint32_t *)termios)[3] = c_lflag;
		termios[16] = c_line;
		memcpy(termios + 17, c_cc, NCCS);
		if (copy_to_user((void *)(uintptr_t)arg, termios, sizeof(termios)) != 0) return -14;
		return 0;
	}
	if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
		uint8_t termios[36];
		if (copy_from_user(termios, (const void *)(uintptr_t)arg, sizeof(termios)) != 0) return -14;
		uint64_t flags = spin_lock_irq(&tty_lock);
		c_iflag = ((uint32_t *)termios)[0];
		c_oflag = ((uint32_t *)termios)[1];
		c_cflag = ((uint32_t *)termios)[2];
		c_lflag = ((uint32_t *)termios)[3];
		c_line  = termios[16];
		memcpy(c_cc, termios + 17, NCCS);
		spin_unlock_irq(&tty_lock, flags);
		return 0;
	}
	if (cmd == TIOCGWINSZ) {
		int cols = 80, rows = 24;
		fb_tty_get_size(&cols, &rows);
		uint16_t ws[4] = {(uint16_t)rows, (uint16_t)cols, 0, 0};
		if (copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws)) != 0) return -14;
		return 0;
	}
	if (cmd == TIOCGPGRP) {
		if (copy_to_user((void *)(uintptr_t)arg, &fg_pgrp, sizeof(fg_pgrp)) != 0) return -14;
		return 0;
	}
	if (cmd == TIOCSPGRP) {
		pid_t pgrp;
		if (copy_from_user(&pgrp, (const void *)(uintptr_t)arg, sizeof(pgrp)) != 0) return -14;
		fg_pgrp = pgrp;
		return 0;
	}
	if (cmd == TIOCSCTTY)
		return 0;
	return -25;
}

static vfs_fops_t tty_core_fops = {
	.read   = tty_core_read,
	.write  = tty_core_write,
	.poll   = tty_core_poll,
	.ioctl  = tty_core_ioctl,
};

void tty_core_init(void)
{
	devfs_register_device("tty", S_IFCHR | 0666, 0x0500, &tty_core_fops);
	devfs_register_device("console", S_IFCHR | 0600, 0x0501, &tty_core_fops);
	devfs_register_device("tty0", S_IFCHR | 0666, 0x0400, &tty_core_fops);
	tick_register(tty_poll_inputs);
}
