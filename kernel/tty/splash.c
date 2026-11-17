/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The boot and shutdown screen. With "quiet" on the command line the screen
 * shows the Ranix orca on a dark field and a thin progress bar instead of the
 * scrolling log, from the first frame the kernel draws until the desktop takes
 * the display, and again while the machine shuts down or restarts.
 *
 * Only the screen changes. Every message still goes to the log ring (dmesg,
 * /dev/kmsg, pstore) and to the console's shadow. The log comes back when:
 *   - Esc is pressed on a real keyboard,
 *   - a KERN_ERR line is logged (an error is never hidden),
 *   - "log" is written to /proc/sys/kernel/ranix_splash,
 *   - the boot or the shutdown stalls: nothing new for STALL_S seconds.
 * No boot screen with fjord.rescue (a shell on the console at once); a panic
 * turns it off for good. The reveal puts what programs last wrote to the
 * console (a prompt, an fsck question) back under the log.
 * ranix.verbose or ranix.splash=0 keeps the classic log; "quiet" alone then
 * only lowers the console level, as on Linux.
 *
 * /proc/sys/kernel/ranix_splash (write: root only)
 *   read   "<state> <percent>": off | boot | down | log
 *   write  N (0-100)   progress of the boot, from init; it never goes back
 *          done        the boot is over; with no desktop on the display after
 *                      DONE_S seconds, the text console comes back
 *          log         show the log now
 *          poweroff | reboot   the shutdown screen (quiet boots only) */
#include <kernel/tty/splash.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/cmdline.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/tick.h>
#include <kernel/time.h>
#include <kernel/board.h>
#include <kernel/errno.h>
#include <kernel/lib/string.h>
#include "baron_rle.h"

#define STALL_S  90     /* boot or shutdown screen with nothing new this long: show the log
                         * (ranix.splash_stall=N seconds sets it) */
#define DONE_S   15     /* "done" and no desktop this long: show the text console */

enum { SPL_OFF, SPL_BOOT, SPL_DOWN, SPL_LOG };
static const char *const state_name[] = { "off", "boot", "down", "log" };

static fb_info_t *fb;
static int  wanted = -1;          /* -1 not asked yet */
static int  state;                /* SPL_* */
static int  percent;
static int  down_reboot;
static int  replay_pending;       /* show the log once the program gives the display back */
static volatile int esc_flag, err_flag;
static uint64_t last_change, done_at;
static uint32_t stall_s = STALL_S;
static spinlock_t splash_lock = SPINLOCK_INIT;

/* layout, from the screen size */
static u32 bar_x, bar_y, bar_w, bar_h, msg_y;

int splash_wanted(void)
{
	if (wanted < 0) {
		char b[4];
		/* fjord.rescue starts a shell on the console at once: it must be seen */
		wanted = cmdline_has("quiet") && !cmdline_has("ranix.verbose") && !cmdline_has("fjord.rescue") &&
		         !(cmdline_get("ranix.splash", b, sizeof(b)) && b[0] == '0');
	}
	return wanted;
}

/* ---- drawing, straight onto the framebuffer ---- */

static u32 px(u32 rgb)
{
	return 0xFF000000u | (((rgb >> 16) & 0xFF) << fb->red_shift) |
	       (((rgb >> 8) & 0xFF) << fb->green_shift) | ((rgb & 0xFF) << fb->blue_shift);
}

#define C_BG    0x0B121A
#define C_BODY  0x2E4862
#define C_WHITE 0xE8EEF4
#define C_TEXT  0xC8D4E0
#define C_HINT  0x5A6B7C
#define C_TRACK 0x1C2A38
#define C_FILL  0x9CC2E2

static void fill(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
	u32 c = px(rgb);
	if (x >= fb->width || y >= fb->height) return;
	if (x + w > fb->width) w = fb->width - x;
	if (y + h > fb->height) h = fb->height - y;
	for (u32 j = 0; j < h; j++) {
		u32 *row = (u32 *)((u8 *)fb->fb + (y + j) * fb->pitch) + x;
		for (u32 i = 0; i < w; i++) row[i] = c;
	}
}

/* The orca from its run-length mask, top-left at (x0, y0). */
static void draw_logo(const unsigned char *rle, size_t n, u32 size, u32 x0, u32 y0)
{
	u32 body = px(C_BODY), white = px(C_WHITE), at = 0;
	for (size_t k = 0; k < n; k++) {
		u32 cls = rle[k] >> 6, run = (u32)(rle[k] & 63) + 1;
		if (cls) {
			u32 c = cls == 1 ? body : white;
			for (u32 i = 0; i < run; i++) {
				u32 x = x0 + (at + i) % size, y = y0 + (at + i) / size;
				if (x < fb->width && y < fb->height)
					((u32 *)((u8 *)fb->fb + y * fb->pitch))[x] = c;
			}
		}
		at += run;
	}
}

/* UTF-8 text in the console font, k times its size, centred on cx. */
static u32 text_cells(const char *s)
{
	u32 n = 0;
	for (; *s; s++) if (((u8)*s & 0xC0) != 0x80) n++;
	return n;
}

static void draw_text(const char *s, u32 cx, u32 y, u32 k, u32 rgb)
{
	u32 c = px(rgb), tw = text_cells(s) * 8 * k;
	if (tw > fb->width) return;
	u32 x = cx - tw / 2;
	while (*s) {
		u32 cp = (u8)*s++;
		if (cp >= 0xE0 && s[0] && s[1]) { cp = ((cp & 0x0F) << 12) | (((u8)s[0] & 0x3F) << 6) | ((u8)s[1] & 0x3F); s += 2; }
		else if (cp >= 0xC0 && s[0])    { cp = ((cp & 0x1F) << 6) | ((u8)s[0] & 0x3F); s += 1; }
		const u8 *g = fb_tty_glyph(cp);
		for (u32 j = 0; j < 16 * k && y + j < fb->height; j++) {
			u32 *row = (u32 *)((u8 *)fb->fb + (y + j) * fb->pitch);
			for (u32 i = 0; i < 8 * k; i++)
				if ((g[j / k] & (0x80 >> (i / k))) && x + i < fb->width) row[x + i] = c;
		}
		x += 8 * k;
	}
}

/* LOCK HELD */
static void paint_bar(void)
{
	u32 done = bar_w * (u32)percent / 100;
	fill(bar_x, bar_y, done, bar_h, C_FILL);
	fill(bar_x + done, bar_y, bar_w - done, bar_h, C_TRACK);
}

/* LOCK HELD. The whole screen: field, orca, name, bar or words, hint. */
static void paint(void)
{
	u32 W = fb->width, H = fb->height, cx = W / 2;
	fill(0, 0, W, H, C_BG);
	u32 size = H >= 900 ? BARON_SIZE2 : H >= 560 ? BARON_SIZE : 0;
	u32 top = H * 2 / 5 > size / 2 ? H * 2 / 5 - size / 2 : 0;
	if (size == BARON_SIZE2) draw_logo(BARON_RLE2, sizeof(BARON_RLE2), size, cx - size / 2, top);
	else if (size)           draw_logo(BARON_RLE, sizeof(BARON_RLE), size, cx - size / 2, top);
	u32 ty = top + size + 16;
	draw_text("Ranix", cx, ty, 2, C_TEXT);
	bar_w = W / 6 < 240 ? W / 6 : 240;
	bar_h = 4;
	bar_x = cx - bar_w / 2;
	bar_y = ty + 32 + 28;
	msg_y = bar_y - 6;
	if (state == SPL_DOWN)
		draw_text(down_reboot ? "Restarting" : "Shutting down", cx, msg_y, 1, C_TEXT);
	else
		paint_bar();
	if (H >= 200) draw_text("Esc for details", cx, H - 40, 1, C_HINT);
}

/* ---- the log back on the screen ---- */

/* Where the newest whole lines of buf[0..n) start so that they fill at most
 * maxrows screen rows of cols cells; *used gets the rows they take. */
static size_t tail_start(const char *buf, size_t n, size_t whole_from, int cols, int maxrows, int *used)
{
	size_t start = n, e = n;
	*used = 0;
	if (e && buf[e - 1] == '\n') e--;                    /* the last line's newline */
	while (e > 0 && maxrows > 0) {
		size_t b = e;
		while (b > 0 && buf[b - 1] != '\n') b--;
		if (b < whole_from) break;                       /* cut by the buffer: not a whole line */
		int r = (int)((e - b + (size_t)cols - 1) / (size_t)cols);
		if (r < 1) r = 1;
		if (*used + r > maxrows) break;
		*used += r;
		start = b;
		if (b == 0) break;
		e = b - 1;
	}
	return start;
}

#define CON_ROWS 8     /* at most this much of programs' console output goes back under the log */

/* The newest lines of the kernel log, then what programs last wrote to the
 * console (a shell prompt, an fsck question), so nothing written behind the
 * cover is lost; then the console as usual, the cursor after that text. */
static void replay_log(void)
{
	static char tail[16384], con[2048];
	int cols = 0, rows = 0, crow = 0, krow = 0;
	fb_tty_get_size(&cols, &rows);
	if (cols <= 0 || rows <= 1) { fb_tty_clear(); return; }
	size_t cn = fb_tty_console_tail(con, sizeof(con));
	size_t cs = cn ? tail_start(con, cn, cn == sizeof(con) ? 1 : 0, cols, CON_ROWS, &crow) : cn;
	if (cn && cs == cn && con[cn - 1] != '\n') {      /* a prompt with no newline yet: its line alone */
		cs = cn;
		while (cs > 0 && con[cs - 1] != '\n' && cn - cs < (size_t)cols) cs--;
		crow = 1;
	}
	int sep = cs < cn ? 1 : 0;
	size_t n = kprintf_read_log(tail, sizeof(tail));
	size_t ks = tail_start(tail, n, n == sizeof(tail) ? 1 : 0, cols, rows - 1 - crow - sep, &krow);
	fb_tty_clear();
	fb_tty_write(tail + ks, n - ks);
	if (sep) {
		static const char line[] = "-- konsol --\n";
		if (n && tail[n - 1] != '\n') fb_tty_write("\n", 1);
		fb_tty_write(line, sizeof(line) - 1);
		fb_tty_write(con + cs, cn - cs);
	}
	fb_tty_flush();
}

/* LOCK HELD. The log replaces the boot screen; from now on every line shows. */
static void reveal_locked(void)
{
	if (state == SPL_OFF || state == SPL_LOG) return;
	state = SPL_LOG;
	klog_set_console_level(KLOG_INFO);
	if (fb_tty_get_graphics()) { replay_pending = 1; return; }
	replay_log();
}

/* ---- events ---- */

static void splash_tick(void)
{
	if (state != SPL_BOOT && state != SPL_DOWN) return;
	if (kernel_ticks % 10) return;           /* a hundred looks a second are plenty */
	uint64_t f = spin_lock_irq(&splash_lock);
	int visible = fb_tty_get_cover() && !fb_tty_get_graphics();
	if (err_flag || (esc_flag && visible)) {
		reveal_locked();
	} else if (state == SPL_BOOT && done_at && kernel_ticks - done_at > (uint64_t)DONE_S * TIMER_HZ) {
		state = SPL_OFF;                     /* no desktop came: the text console, as it is */
		fb_tty_set_cover(0);
	} else if (kernel_ticks - last_change > (uint64_t)stall_s * TIMER_HZ) {   /* a boot or a shutdown that hangs */
		kprintf(KERN_WARN "splash: %s stalled for %u s, showing the log\n", state == SPL_BOOT ? "boot" : "shutdown", stall_s);
		reveal_locked();
	}
	esc_flag = err_flag = 0;
	spin_unlock_irq(&splash_lock, f);
}

void splash_init(fb_info_t *info)
{
	if (!splash_wanted()) return;
	if (!info || info->depth != 32 || !info->fb || !fb_tty_get_cover()) {
		wanted = 0;                  /* cannot draw it here: the console as usual */
		fb_tty_set_cover(0);
		return;
	}
	fb = info;
	char v[8];
	if (cmdline_get("ranix.splash_stall", v, sizeof(v))) {
		uint32_t x = 0;
		for (char *p = v; *p >= '0' && *p <= '9'; p++) x = x * 10 + (uint32_t)(*p - '0');
		if (x >= 1 && x <= 3600) stall_s = x;
	}
	uint64_t f = spin_lock_irq(&splash_lock);
	state = SPL_BOOT;
	last_change = kernel_ticks;
	fb_tty_set_cover(1);
	paint();
	spin_unlock_irq(&splash_lock, f);
	tick_register(splash_tick);
}

void splash_progress(int pct)
{
	if (state != SPL_BOOT) return;
	uint64_t f = spin_lock_irq(&splash_lock);
	if (pct > 100) pct = 100;
	if (state == SPL_BOOT && pct > percent) {
		percent = pct;
		last_change = kernel_ticks;
		if (fb_tty_get_cover() && !fb_tty_get_graphics()) paint_bar();
	}
	spin_unlock_irq(&splash_lock, f);
}

void splash_display_taken(void)
{
	if (state != SPL_BOOT) return;
	uint64_t f = spin_lock_irq(&splash_lock);
	if (state == SPL_BOOT) {
		state = SPL_OFF;             /* the desktop's first frame replaces ours: no black between */
		fb_tty_set_cover(0);         /* a program owns the pixels: nothing is drawn */
	}
	spin_unlock_irq(&splash_lock, f);
}

void splash_display_back(void)
{
	if (state == SPL_OFF) return;
	uint64_t f = spin_lock_irq(&splash_lock);
	if (state == SPL_DOWN) {
		fb_tty_set_cover(1);
		paint();
	} else if (state == SPL_LOG && replay_pending) {
		replay_pending = 0;
		replay_log();
	}
	spin_unlock_irq(&splash_lock, f);
}

/* A panic paints its own screen: nothing here may draw over it, from any CPU. */
void splash_panic(void) { state = SPL_OFF; replay_pending = 0; }

void splash_error_line(void) { if (state == SPL_BOOT || state == SPL_DOWN) err_flag = 1; }
void splash_key_esc(void)    { if (state == SPL_BOOT || state == SPL_DOWN) esc_flag = 1; }

static int word_is(const char *w, size_t n, const char *s)
{
	size_t l = strlen(s);
	return n == l && !memcmp(w, s, l);
}

long splash_ctl_write(const char *w, size_t n)
{
	if (n && w[n - 1] == '\n') n--;
	if (!n) return -EINVAL;
	if (w[0] >= '0' && w[0] <= '9') {
		int v = 0;
		for (size_t i = 0; i < n; i++) {
			if (w[i] < '0' || w[i] > '9' || i > 2) return -EINVAL;
			v = v * 10 + (w[i] - '0');
		}
		if (v > 100) return -EINVAL;
		splash_progress(v);
		return 0;
	}
	int down = word_is(w, n, "poweroff") ? 1 : word_is(w, n, "reboot") ? 2 : 0;
	if (!down && !word_is(w, n, "done") && !word_is(w, n, "log")) return -EINVAL;
	if (!fb) return 0;                       /* no boot screen on this boot: nothing to do */
	uint64_t f = spin_lock_irq(&splash_lock);
	if (word_is(w, n, "log")) {
		reveal_locked();
	} else if (word_is(w, n, "done")) {
		if (state == SPL_BOOT) {
			percent = 100;
			if (fb_tty_get_cover() && !fb_tty_get_graphics()) paint_bar();
			if (!done_at) done_at = kernel_ticks ? kernel_ticks : 1;
		}
	} else {
		/* shutdown is quiet again even after Esc at boot; errors still break through */
		state = SPL_DOWN;
		last_change = kernel_ticks;          /* the shutdown's own stall timer starts here */
		down_reboot = down == 2;
		replay_pending = 0;
		klog_set_console_level(KLOG_WARN);
		fb_tty_set_cover(1);                 /* set before the desktop lets go: no text between */
		if (!fb_tty_get_graphics()) paint();
	}
	spin_unlock_irq(&splash_lock, f);
	return 0;
}

int splash_ctl_read(char *buf, size_t cap)
{
	const char *s = state_name[state];
	size_t l = strlen(s);
	if (cap < l + 6) return 0;
	memcpy(buf, s, l);
	buf[l++] = ' ';
	int p = percent;
	if (p >= 100) buf[l++] = '1';
	if (p >= 10) buf[l++] = (char)('0' + (p / 10) % 10);
	buf[l++] = (char)('0' + p % 10);
	buf[l++] = '\n';
	return (int)l;
}
