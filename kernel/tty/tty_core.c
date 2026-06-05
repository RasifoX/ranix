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
#define TOSTOP  0x00000100
#define IEXTEN  0x00008000

/* c_cc indices (termios) */
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

typedef struct {
    char     input_buf[TTY_BUF_SIZE];
    int      in_head, in_tail;

    char     line_buf[TTY_BUF_SIZE];
    int      line_pos;
    int      esc_state;     /* 0=none 1=got-ESC 2=got-ESC-[ */

    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];

    pid_t    fg_pgrp;
    uint16_t ws_row, ws_col;   /* TIOCSWINSZ override; 0 = use the framebuffer size */
    spinlock_t lock;
} tty_t;

static tty_t tty0 = {
    .c_iflag = 0x0500,
    .c_oflag = 0x0005,
    .c_cflag = 0x04BF,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
    .c_cc    = { 0x03, 0x1C, 0x7F, 0x15, 0x04,
                 0x00, 0x01, 0x00, 0x11, 0x13,
                 0x1A, 0x00, 0x12, 0x0F, 0x17,
                 0x16, 0x00, 0x00, 0x00 },
    .fg_pgrp = 1,
    .lock    = SPINLOCK_INIT,
};

static void tty_echo_str(tty_t *t, const char *s, int len)
{
    (void)t;
    fb_tty_write(s, len);
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n') uart_putc('\r');
        uart_putc(s[i]);
    }
}

static void tty_echo_char(tty_t *t, char c)
{
    tty_echo_str(t, &c, 1);
}

static void buf_push(tty_t *t, char c)
{
    int next = (t->in_head + 1) % TTY_BUF_SIZE;
    if (next != t->in_tail) {
        t->input_buf[t->in_head] = c;
        t->in_head = next;
        poll_notify();   /* wake a blocked tty read / poller */
    }
}

void tty_input_char(char c)
{
    tty_t *t = &tty0;
    uint64_t f = spin_lock_irq(&t->lock);

    if (t->c_lflag & ISIG) {
        unsigned char uc = (unsigned char)c;
        if (uc == t->c_cc[VINTR] || uc == t->c_cc[VQUIT] || uc == t->c_cc[VSUSP]) {
            int sig = (uc == t->c_cc[VINTR]) ? 2 :
                      (uc == t->c_cc[VQUIT]) ? 3 : 20;
            spin_unlock_irq(&t->lock, f);
            task_signal_pgrp(t->fg_pgrp, sig);
            return;
        }
    }

    input_inject_ascii((uint8_t)c);

    if (!(t->c_lflag & ICANON)) {
        buf_push(t, c);
        spin_unlock_irq(&t->lock, f);
        return;
    }

    /* Filter terminal escape responses (e.g. \033[row;colR) */
    unsigned char uc = (unsigned char)c;
    if (t->esc_state == 1) {
        t->esc_state = (uc == '[' || uc == ']') ? 2 : 0;
        spin_unlock_irq(&t->lock, f);
        return;
    }
    if (t->esc_state == 2) {
        if ((uc >= 0x40 && uc <= 0x7E) || uc == 0x07) t->esc_state = 0;
        spin_unlock_irq(&t->lock, f);
        return;
    }
    if (uc == 0x1B) {
        t->esc_state = 1;
        spin_unlock_irq(&t->lock, f);
        return;
    }

    if (t->c_cc[VKILL] && c == t->c_cc[VKILL]) {
        /* line kill (Ctrl-U): erase the whole pending line, char by char so the
         * echo (and any multibyte run) unwinds exactly like repeated erase. */
        while (t->line_pos > 0) {
            int n = 1;
            while (n < t->line_pos && (t->line_buf[t->line_pos - n] & 0xC0) == 0x80)
                n++;
            t->line_pos -= n;
            if (t->c_lflag & ECHO) tty_echo_str(t, "\b \b", 3);
        }
    } else if (c == 127 || c == '\b' || (t->c_cc[VERASE] && c == t->c_cc[VERASE])) {
        if (t->line_pos > 0) {
            int n = 1;
            while (n < t->line_pos && (t->line_buf[t->line_pos - n] & 0xC0) == 0x80)
                n++;
            t->line_pos -= n;
            if (t->c_lflag & ECHO) tty_echo_str(t, "\b \b", 3);
        }
    } else if (c == '\r' || c == '\n') {
        if (t->c_lflag & ECHO) tty_echo_char(t, '\n');
        t->line_buf[t->line_pos++] = '\n';
        for (int i = 0; i < t->line_pos; i++)
            buf_push(t, t->line_buf[i]);
        t->line_pos = 0;
    } else {
        if (t->line_pos < TTY_BUF_SIZE - 1) {
            t->line_buf[t->line_pos++] = c;
            if (t->c_lflag & ECHO) tty_echo_char(t, c);
        }
    }
    spin_unlock_irq(&t->lock, f);
}

void tty_input_str(const char *s)
{
    while (*s) tty_input_char(*s++);
}

static void tty_poll_uart(void)
{
    int got = 0;
    while (uart_has_data()) {
        tty_input_char(uart_getc());
        got = 1;
    }
    if (got) fb_tty_flush();
}

static vfs_fops_t tty_fops;

static ssize_t tty_read(vfs_file_t *f, void *buf, size_t len)
{
    (void)f;
    tty_t *t = &tty0;
    char *b = buf;
    size_t copied = 0;

    for (;;) {
        int jc = tty_jobctl_check(t, &tty_fops, t->fg_pgrp, 21 /* SIGTTIN */);
        if (jc < 0) return jc;
        if (jc == 0) break;
    }

    while (copied < len) {
        tty_poll_uart();

        uint64_t flags = spin_lock_irq(&t->lock);
        if (t->in_head != t->in_tail) {
            b[copied++] = t->input_buf[t->in_tail];
            t->in_tail = (t->in_tail + 1) % TTY_BUF_SIZE;
            spin_unlock_irq(&t->lock, flags);
        } else {
            spin_unlock_irq(&t->lock, flags);
            if (copied > 0) break;
            if (f->flags & O_NONBLOCK) return -11;
            fb_tty_flush();
            poll_block((uint64_t)-1);
        }
    }
    return copied;
}

static void tty_inject_uint(tty_t *t, unsigned int v)
{
    char tmp[12]; int i = 0;
    if (v == 0) { buf_push(t, '0'); return; }
    while (v > 0 && i < 12) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = i - 1; j >= 0; j--) buf_push(t, tmp[j]);
}

static ssize_t tty_write(vfs_file_t *f, const void *buf, size_t len)
{
    (void)f;
    tty_t *t = &tty0;
    const char *p = buf;

    if (t->c_lflag & TOSTOP) {
        for (;;) {
            int jc = tty_jobctl_check(t, &tty_fops, t->fg_pgrp, 22 /* SIGTTOU */);
            if (jc < 0) return jc;
            if (jc == 0) break;
        }
    }

    for (size_t i = 0; i + 3 < len; i++) {
        if (p[i] == '\033' && p[i+1] == '[' && p[i+2] == '6' && p[i+3] == 'n') {
            int col = 1, row = 1;
            fb_tty_get_cursor(&col, &row);
            uint64_t flags = spin_lock_irq(&t->lock);
            const char *pre = "\033[";
            for (const char *s = pre; *s; s++) buf_push(t, *s);
            tty_inject_uint(t, (unsigned int)(row + 1));
            buf_push(t, ';');
            tty_inject_uint(t, (unsigned int)(col + 1));
            buf_push(t, 'R');
            spin_unlock_irq(&t->lock, flags);
            break;
        }
    }
    tty_echo_str(t, buf, len);
    fb_tty_flush();
    return len;
}

static int tty_poll(vfs_file_t *f)
{
    (void)f;
    tty_poll_uart();
    tty_t *t = &tty0;
    return (t->in_head != t->in_tail) ? 1 : 0;
}

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540f
#define TIOCSPGRP  0x5410
#define TIOCSCTTY  0x540e
#define TIOCNOTTY  0x5422

static int tty_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
    (void)f;
    tty_t *t = &tty0;

    if (cmd == TCGETS) {
        uint8_t termios[36];
        memset(termios, 0, sizeof(termios));
        ((uint32_t *)termios)[0] = t->c_iflag;
        ((uint32_t *)termios)[1] = t->c_oflag;
        ((uint32_t *)termios)[2] = t->c_cflag;
        ((uint32_t *)termios)[3] = t->c_lflag;
        termios[16] = t->c_line;
        memcpy(termios + 17, t->c_cc, NCCS);
        return copy_to_user((void *)(uintptr_t)arg, termios, sizeof(termios)) ? -14 : 0;
    }
    if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
        uint8_t termios[36];
        if (copy_from_user(termios, (const void *)(uintptr_t)arg, sizeof(termios))) return -14;
        uint64_t flags = spin_lock_irq(&t->lock);
        t->c_iflag = ((uint32_t *)termios)[0];
        t->c_oflag = ((uint32_t *)termios)[1];
        t->c_cflag = ((uint32_t *)termios)[2];
        t->c_lflag = ((uint32_t *)termios)[3];
        t->c_line  = termios[16];
        memcpy(t->c_cc, termios + 17, NCCS);
        spin_unlock_irq(&t->lock, flags);
        return 0;
    }
    if (cmd == TIOCGWINSZ) {
        int cols = 80, rows = 24;
        fb_tty_get_size(&cols, &rows);
        /* An explicit TIOCSWINSZ override wins over the framebuffer geometry. */
        if (t->ws_row && t->ws_col) { rows = t->ws_row; cols = t->ws_col; }
        uint16_t ws[4] = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
        return copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws)) ? -14 : 0;
    }
    if (cmd == TIOCSWINSZ) {
        uint16_t ws[4];
        if (copy_from_user(ws, (const void *)(uintptr_t)arg, sizeof(ws))) return -14;
        uint64_t flags = spin_lock_irq(&t->lock);
        t->ws_row = ws[0]; t->ws_col = ws[1];
        pid_t pg = t->fg_pgrp;
        spin_unlock_irq(&t->lock, flags);
        if (pg > 0) task_signal_pgrp(pg, 28);   /* SIGWINCH */
        return 0;
    }
    if (cmd == TIOCGPGRP)
        return copy_to_user((void *)(uintptr_t)arg, &t->fg_pgrp, sizeof(t->fg_pgrp)) ? -14 : 0;
    if (cmd == TIOCSPGRP) {
        pid_t pgrp;
        if (copy_from_user(&pgrp, (const void *)(uintptr_t)arg, sizeof(pgrp))) return -14;
        t->fg_pgrp = pgrp;
        return 0;
    }
    if (cmd == TIOCSCTTY) {
        process_t *cur = task_current();
        if (cur) {
            if (cur->pgrp > 0) t->fg_pgrp = cur->pgrp;
            cur->ctty = t;
            cur->ctty_fops = &tty_fops;
        }
        return 0;
    }
    if (cmd == TIOCNOTTY) {
        process_t *cur = task_current();
        if (cur && cur->ctty == t) { cur->ctty = NULL; cur->ctty_fops = NULL; }
        return 0;
    }
    return -25;
}

static vfs_fops_t tty_fops = {
    .read  = tty_read,
    .write = tty_write,
    .poll  = tty_poll,
    .ioctl = tty_ioctl,
};

/* /dev/tty — the calling process's controlling terminal. Routes read/write/
 * ioctl/poll to whatever the session set via TIOCSCTTY (a pts slave or the
 * console); with no ctty it falls back to the console line discipline. */
static vfs_fops_t *ctty_fops(void **obj)
{
    process_t *cur = task_current();
    if (cur && cur->ctty && cur->ctty_fops) { *obj = cur->ctty; return cur->ctty_fops; }
    *obj = &tty0;
    return &tty_fops;
}

static ssize_t ctty_read(vfs_file_t *f, void *buf, size_t len)
{
    void *obj; vfs_fops_t *fo = ctty_fops(&obj);
    if (!fo->read) return -22;
    vfs_file_t tmp = *f; tmp.priv = obj;
    return fo->read(&tmp, buf, len);
}
static ssize_t ctty_write(vfs_file_t *f, const void *buf, size_t len)
{
    void *obj; vfs_fops_t *fo = ctty_fops(&obj);
    if (!fo->write) return -22;
    vfs_file_t tmp = *f; tmp.priv = obj;
    return fo->write(&tmp, buf, len);
}
static int ctty_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
    void *obj; vfs_fops_t *fo = ctty_fops(&obj);
    if (!fo->ioctl) return -25;
    vfs_file_t tmp = *f; tmp.priv = obj;
    return fo->ioctl(&tmp, cmd, arg);
}
static int ctty_poll(vfs_file_t *f)
{
    void *obj; vfs_fops_t *fo = ctty_fops(&obj);
    if (!fo->poll) return 1;
    vfs_file_t tmp = *f; tmp.priv = obj;
    return fo->poll(&tmp);
}

static vfs_fops_t ctty_dev_fops = {
    .read  = ctty_read,
    .write = ctty_write,
    .poll  = ctty_poll,
    .ioctl = ctty_ioctl,
};

void tty_core_init(void)
{
    devfs_register_device("tty",     S_IFCHR | 0666, 0x0500, &ctty_dev_fops);
    devfs_register_device("console", S_IFCHR | 0600, 0x0501, &tty_fops);
    devfs_register_device("tty0",    S_IFCHR | 0666, 0x0400, &tty_fops);
    /* /dev/tty1..tty6 — the virtual-console nodes Linux exposes (getty/login,
     * `openvt`, container consoles expect them). We have a single physical
     * console, so they share its line discipline; the nodes exist for ABI
     * compatibility rather than independent VTs. */
    static const char *vtn[] = { "tty1","tty2","tty3","tty4","tty5","tty6" };
    for (int i = 0; i < 6; i++)
        devfs_register_device(vtn[i], S_IFCHR | 0620, 0x0400 | (i + 1), &tty_fops);
    tick_register(tty_poll_uart);
}
