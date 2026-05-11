#include <kernel/tty/tty_core.h>
#include <kernel/tty/fb_tty.h>
#include <drivers/serial/uart.h>
#include <kernel/fs/devfs.h>
#include <kernel/proc.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

#define ICANON 0x00000002
#define ECHO   0x00000008

#define TTY_BUF_SIZE 1024

static char input_buf[TTY_BUF_SIZE];
static int  in_head = 0;
static int  in_tail = 0;

static char line_buf[TTY_BUF_SIZE];
static int  line_pos = 0;

static uint32_t c_lflag = ICANON | ECHO | 0x8A30;

static spinlock_t tty_lock = SPINLOCK_INIT;

static void tty_echo_str(const char *s, int len)
{
	fb_tty_write(s, len);
	fb_tty_flush();
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
	
	if (!(c_lflag & ICANON)) {
		int next = (in_head + 1) % TTY_BUF_SIZE;
		if (next != in_tail) {
			input_buf[in_head] = c;
			in_head = next;
		}
		spin_unlock_irq(&tty_lock, f);
		return;
	}

	if (c == 127 || c == '\b') {
		if (line_pos > 0) {
			line_pos--;
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

static void tty_poll_inputs(void)
{
	while (uart_has_data()) {
		char c = uart_getc();
		tty_input_char(c);
	}
#if defined(__x86_64__)
	while (ps2_kbd_has_data()) {
		char c = ps2_kbd_getc();
		tty_input_char(c);
	}
#endif
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
			task_sleep();
		}
	}
	return copied;
}

static ssize_t tty_core_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	tty_echo_str(buf, len);
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
#define TCSETS   0x5402
#define TCSETSW  0x5403
#define TCSETSF  0x5404
#define TIOCGWINSZ 0x5413

static int tty_core_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	if (cmd == TCGETS) {
		uint8_t termios[60];
		memset(termios, 0, sizeof(termios));
		((uint32_t *)termios)[3] = c_lflag;
		if (copy_to_user((void *)(uintptr_t)arg, termios, sizeof(termios)) != 0) return -14;
		return 0;
	}
	if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
		uint8_t termios[60];
		if (copy_from_user(termios, (const void *)(uintptr_t)arg, sizeof(termios)) != 0) return -14;
		uint64_t flags = spin_lock_irq(&tty_lock);
		c_lflag = ((uint32_t *)termios)[3];
		spin_unlock_irq(&tty_lock, flags);
		return 0;
	}
	if (cmd == TIOCGWINSZ) {
		uint16_t ws[4] = {24, 80, 0, 0}; /* fb_tty height/width is dynamic but standard fallback is fine */
		if (copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws)) != 0) return -14;
		return 0;
	}
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
}
