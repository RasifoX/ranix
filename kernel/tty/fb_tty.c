#include <kernel/tty/fb_tty.h>
#include <kernel/fb.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include "font_spleen.h"
#include <kernel/board.h>
#include <kernel/sync.h>
#include <kernel/tick.h>

#define FONT_W  8
#define FONT_H  16

static fb_info_t *_fb;
static u32  _cols, _rows;
static u32  _cur_col, _cur_row;
static u8  *shadow;
static u32  _dirty_y0, _dirty_y1;

static spinlock_t fb_tty_lock = SPINLOCK_INIT;

/* cursor state — drawn directly on fb, not in shadow */
static int           _cursor_visible  = 0;
static u32           _cursor_fb_col   = 0;
static u32           _cursor_fb_row   = 0;
static int           _cursor_on_fb    = 0;
static int           _blink_ticks     = 0;
static int           _blink_state     = 1;
static volatile int  _blink_dirty     = 0;

/* color state */
static u32 _def_fg = 0xFFFFFFFF;
static u32 _def_bg = 0xFF000000;
static u32 _cur_fg = 0xFFFFFFFF;
static u32 _cur_bg = 0xFF000000;
static int _bold   = 0;

static inline u32 make_px(u8 r, u8 g, u8 b)
{
    return 0xFF000000u
         | ((u32)r << _fb->red_shift)
         | ((u32)g << _fb->green_shift)
         | ((u32)b << _fb->blue_shift);
}
#define RGB(r,g,b) make_px((r),(g),(b))

static u32 _ansi16[16]; /* populated in fb_tty_init after _fb is set */

/* ANSI escape state */
#define ESC_NONE  0
#define ESC_ESC   1
#define ESC_CSI   2
#define ESC_OSC   3
#define MAX_PARAMS 8

static int _esc_state   = ESC_NONE;
static int _csi_params[MAX_PARAMS];
static int _csi_n;
static int _csi_private;


/* LOCK HELD */
static void flush_region(u32 y_start, u32 y_end)
{
    if (y_end > _fb->height) y_end = _fb->height;
    if (y_start >= y_end) return;
    u8 *dst = (u8 *)_fb->fb + y_start * _fb->pitch;
    u8 *src = shadow + y_start * _fb->pitch;
    memcpy(dst, src, (y_end - y_start) * _fb->pitch);
}

/* LOCK HELD */
static void flush_all(void)
{
    memcpy(_fb->fb, shadow, _fb->height * _fb->pitch);
}

/* LOCK HELD */
static void cursor_erase_from_fb(void)
{
    if (!_cursor_on_fb || !_fb) return;
    u32 px = _cursor_fb_col * FONT_W;
    u32 py = _cursor_fb_row * FONT_H;
    for (u32 y = 0; y < FONT_H && py + y < _fb->height; y++) {
        u32 *dst = (u32 *)((u8 *)_fb->fb + (py + y) * _fb->pitch) + px;
        u32 *src = (u32 *)(shadow         + (py + y) * _fb->pitch) + px;
        for (u32 x = 0; x < FONT_W; x++) dst[x] = src[x];
    }
    _cursor_on_fb = 0;
}

/* LOCK HELD */
static void cursor_draw_on_fb(void)
{
    if (!_cursor_visible || !_fb || !_blink_state) return;
    u32 px = _cur_col * FONT_W;
    u32 py = _cur_row * FONT_H;
    for (u32 y = 0; y < FONT_H && py + y < _fb->height; y++) {
        u32 *line = (u32 *)((u8 *)_fb->fb + (py + y) * _fb->pitch) + px;
        for (u32 x = 0; x < FONT_W; x++) line[x] ^= 0x00FFFFFF;
    }
    _cursor_fb_col = _cur_col;
    _cursor_fb_row = _cur_row;
    _cursor_on_fb  = 1;
}

/* LOCK HELD */
static void dirty_flush_locked(void)
{
    if (!_fb) return;
    if (_cursor_visible) {
        u32 py = _cur_row * FONT_H;
        if (py < _dirty_y0) _dirty_y0 = py;
        if (py + FONT_H > _dirty_y1) _dirty_y1 = py + FONT_H;
    }
    if (_dirty_y1 > _dirty_y0) {
        cursor_erase_from_fb();
        flush_region(_dirty_y0, _dirty_y1);
        cursor_draw_on_fb();
    }
    _dirty_y0 = _fb->height;
    _dirty_y1 = 0;
}

/* LOCK HELD */
static void scroll_up(void)
{
    u32 row_bytes = FONT_H * _fb->pitch;
    memcpy(shadow, shadow + row_bytes, (_rows - 1) * row_bytes);
    u64 bg = (u64)_cur_bg | ((u64)_cur_bg << 32);
    u64 *last = (u64 *)(shadow + (_rows - 1) * row_bytes);
    u32 count = row_bytes / 8;
    for (u32 i = 0; i < count; i++) last[i] = bg;
    /* dirty region covers entire screen after a scroll */
    _dirty_y0 = 0;
    _dirty_y1 = _fb->height;
}

/* LOCK HELD */
static void draw_glyph(u32 col, u32 row, u32 cp)
{
    const u8 *glyph = spleen_lookup(cp);
    u32 px = col * FONT_W;
    u32 py = row * FONT_H;
    for (u32 y = 0; y < FONT_H; y++) {
        u32 *line = (u32 *)(shadow + (py + y) * _fb->pitch);
        u8 bits = glyph[y];
        for (u32 x = 0; x < FONT_W; x++)
            line[px + x] = (bits & (0x80 >> x)) ? _cur_fg : _cur_bg;
    }
    if (py < _dirty_y0) _dirty_y0 = py;
    if (py + FONT_H > _dirty_y1) _dirty_y1 = py + FONT_H;
}

/* LOCK HELD */
static void put_codepoint(u32 cp)
{
    if (!_fb) return;
    if (cp == '\n') {
        _cur_col = 0; _cur_row++;
    } else if (cp == '\r') {
        _cur_col = 0;
    } else if (cp == '\b') {
        if (_cur_col > 0) { _cur_col--; draw_glyph(_cur_col, _cur_row, ' '); }
    } else {
        draw_glyph(_cur_col, _cur_row, cp);
        if (++_cur_col >= _cols) { _cur_col = 0; _cur_row++; }
    }
    if (_cur_row >= _rows) { scroll_up(); _cur_row = _rows - 1; }
    _blink_state = 1;
    _blink_ticks = 0;
}

/* LOCK HELD */
static u32 ansi256(int n)
{
    if (n < 16) return _ansi16[n];
    if (n >= 232) {
        u8 v = (u8)(8 + (n - 232) * 10);
        return RGB(v, v, v);
    }
    n -= 16;
    u8 r = (u8)((n / 36) * 51);
    u8 g = (u8)(((n / 6) % 6) * 51);
    u8 b = (u8)((n % 6) * 51);
    return RGB(r, g, b);
}

/* LOCK HELD — process SGR params accumulated in _csi_params[0.._csi_n-1] */
static void handle_sgr(void)
{
    if (_csi_n == 0) { _cur_fg = _def_fg; _cur_bg = _def_bg; _bold = 0; return; }
    int i = 0;
    while (i < _csi_n) {
        int p = _csi_params[i++];
        switch (p) {
        case 0:  _cur_fg = _def_fg; _cur_bg = _def_bg; _bold = 0; break;
        case 1:  _bold = 1; break;
        case 22: _bold = 0; break;
        case 39: _cur_fg = _def_fg; break;
        case 49: _cur_bg = _def_bg; break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            _cur_fg = _ansi16[p - 30 + (_bold ? 8 : 0)]; break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            _cur_bg = _ansi16[p - 40]; break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            _cur_fg = _ansi16[p - 90 + 8]; break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            _cur_bg = _ansi16[p - 100 + 8]; break;
        case 38:
            if (i + 1 < _csi_n && _csi_params[i] == 5) {
                _cur_fg = ansi256(_csi_params[i + 1]); i += 2;
            } else if (i + 3 < _csi_n && _csi_params[i] == 2) {
                _cur_fg = RGB(_csi_params[i+1], _csi_params[i+2], _csi_params[i+3]); i += 4;
            }
            break;
        case 48:
            if (i + 1 < _csi_n && _csi_params[i] == 5) {
                _cur_bg = ansi256(_csi_params[i + 1]); i += 2;
            } else if (i + 3 < _csi_n && _csi_params[i] == 2) {
                _cur_bg = RGB(_csi_params[i+1], _csi_params[i+2], _csi_params[i+3]); i += 4;
            }
            break;
        default: break;
        }
    }
}

/* LOCK HELD */
static void execute_csi(char cmd)
{
    int p0 = (_csi_n > 0) ? _csi_params[0] : 0;
    int p1 = (_csi_n > 1) ? _csi_params[1] : 0;

    if (_csi_private) {
        if (cmd == 'h' && p0 == 25) { _cursor_visible = 1; cursor_draw_on_fb(); }
        if (cmd == 'l' && p0 == 25) { cursor_erase_from_fb(); _cursor_visible = 0; }
        return;
    }

    switch (cmd) {
    case 'A': {
        u32 n = p0 ? (u32)p0 : 1u;
        _cur_row = (n > _cur_row) ? 0 : _cur_row - n;
        break;
    }
    case 'B': {
        u32 n = p0 ? (u32)p0 : 1u;
        _cur_row = (_cur_row + n >= _rows) ? _rows - 1 : _cur_row + n;
        break;
    }
    case 'C': {
        u32 n = p0 ? (u32)p0 : 1u;
        _cur_col = (_cur_col + n >= _cols) ? _cols - 1 : _cur_col + n;
        break;
    }
    case 'D': {
        u32 n = p0 ? (u32)p0 : 1u;
        _cur_col = (n > _cur_col) ? 0 : _cur_col - n;
        break;
    }
    case 'H': case 'f': {
        u32 row = p0 ? (u32)(p0 - 1) : 0u;
        u32 col = p1 ? (u32)(p1 - 1) : 0u;
        _cur_row = (row >= _rows) ? _rows - 1 : row;
        _cur_col = (col >= _cols) ? _cols - 1 : col;
        break;
    }
    case 'J': {
        if (p0 == 2 || p0 == 3) {
            u32 shadow_size = _fb->pitch * _fb->height;
            u64 bg = (u64)_cur_bg | ((u64)_cur_bg << 32);
            u64 *ptr = (u64 *)shadow;
            for (u32 i = 0; i < shadow_size / 8; i++) ptr[i] = bg;
            _dirty_y0 = 0; _dirty_y1 = _fb->height;
            _cur_col = 0; _cur_row = 0;
        } else if (p0 == 0) {
            for (u32 c = _cur_col; c < _cols; c++) draw_glyph(c, _cur_row, ' ');
            for (u32 r = _cur_row + 1; r < _rows; r++)
                for (u32 c = 0; c < _cols; c++) draw_glyph(c, r, ' ');
        } else if (p0 == 1) {
            for (u32 r = 0; r < _cur_row; r++)
                for (u32 c = 0; c < _cols; c++) draw_glyph(c, r, ' ');
            for (u32 c = 0; c <= _cur_col; c++) draw_glyph(c, _cur_row, ' ');
        }
        break;
    }
    case 'K': {
        u32 from, to;
        if (p0 == 0)      { from = _cur_col; to = _cols; }
        else if (p0 == 1) { from = 0; to = _cur_col + 1; }
        else              { from = 0; to = _cols; }
        for (u32 c = from; c < to; c++) draw_glyph(c, _cur_row, ' ');
        break;
    }
    case 'm': handle_sgr(); break;
    case 'n': break;  /* DSR ignored — busybox uses TIOCGWINSZ */
    default: break;
    }
}

void fb_tty_tick(void);

void fb_tty_init(fb_info_t *fb)
{
    _fb       = fb;
    _cols     = fb->width  / FONT_W;
    _rows     = fb->height / FONT_H;
    _cur_col  = 0;
    _cur_row  = 0;
    _dirty_y0 = fb->height;
    _dirty_y1 = 0;

    /* Build ANSI palette using actual framebuffer channel layout */
    _ansi16[0]  = RGB(0x1C,0x1C,0x1C); _ansi16[1]  = RGB(0xCC,0x00,0x00);
    _ansi16[2]  = RGB(0x4E,0x9A,0x06); _ansi16[3]  = RGB(0xC4,0xA0,0x00);
    _ansi16[4]  = RGB(0x34,0x65,0xA4); _ansi16[5]  = RGB(0x75,0x50,0x7B);
    _ansi16[6]  = RGB(0x06,0x98,0x9A); _ansi16[7]  = RGB(0xD3,0xD7,0xCF);
    _ansi16[8]  = RGB(0x55,0x57,0x53); _ansi16[9]  = RGB(0xEF,0x29,0x29);
    _ansi16[10] = RGB(0x8A,0xE2,0x34); _ansi16[11] = RGB(0xFC,0xE9,0x4F);
    _ansi16[12] = RGB(0x72,0x9F,0xCF); _ansi16[13] = RGB(0xAD,0x7F,0xA8);
    _ansi16[14] = RGB(0x34,0xE2,0xE2); _ansi16[15] = RGB(0xEE,0xEE,0xEC);

    _def_fg = RGB(0xFF,0xFF,0xFF);
    _def_bg = RGB(0x00,0x00,0x00);
    _cur_fg = _def_fg;
    _cur_bg = _def_bg;

    u32 shadow_size = fb->pitch * fb->height;
    shadow = kmalloc(shadow_size);
    if (!shadow) return;
    _cursor_visible = 1;
    _blink_state = 1;
    fb_tty_clear();
    tick_register(fb_tty_tick);
}

void fb_tty_get_size(int *cols, int *rows)
{
    if (cols) *cols = (int)_cols;
    if (rows) *rows = (int)_rows;
}

void fb_tty_get_cursor(int *col, int *row)
{
    if (col) *col = (int)_cur_col;
    if (row) *row = (int)_cur_row;
}

void fb_tty_flush(void)
{
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    dirty_flush_locked();
    spin_unlock_irq(&fb_tty_lock, f);
}

static u32  utf8_state = 0;
static u32  utf8_cp = 0;

/* LOCK HELD */
static void _putchar_locked(unsigned char u)
{
    if (_esc_state == ESC_ESC) {
        if (u == '[') {
            _esc_state = ESC_CSI;
            _csi_n = 0; _csi_private = 0;
            for (int i = 0; i < MAX_PARAMS; i++) _csi_params[i] = 0;
        } else if (u == ']') {
            _esc_state = ESC_OSC;
        } else {
            _esc_state = ESC_NONE;
        }
        return;
    }

    if (_esc_state == ESC_CSI) {
        if (u == '?') { _csi_private = 1; return; }
        if (u >= '0' && u <= '9') {
            if (_csi_n == 0) _csi_n = 1;
            _csi_params[_csi_n - 1] = _csi_params[_csi_n - 1] * 10 + (u - '0');
            return;
        }
        if (u == ';') {
            if (_csi_n == 0) _csi_n = 1;
            if (_csi_n < MAX_PARAMS) { _csi_n++; _csi_params[_csi_n - 1] = 0; }
            return;
        }
        if (u >= 0x40 && u <= 0x7E) execute_csi((char)u);
        _esc_state = ESC_NONE;
        return;
    }

    if (_esc_state == ESC_OSC) {
        if (u == 0x07 || u == 0x9C) {
            _esc_state = ESC_NONE;
        } else if (u == 0x1B) {
            _esc_state = ESC_ESC;
        }
        return;
    }

    if (u == 0x1B) {
        _esc_state = ESC_ESC;
        utf8_state = 0;
        return;
    }

    if (u < 0x80) {
        utf8_state = 0;
        put_codepoint(u);
    } else if (u >= 0xC0 && u <= 0xDF) {
        utf8_cp = u & 0x1F;
        utf8_state = 1;
    } else if (u >= 0xE0 && u <= 0xEF) {
        utf8_cp = u & 0x0F;
        utf8_state = 2;
    } else if (u >= 0x80 && u <= 0xBF) {
        if (utf8_state > 0) {
            utf8_cp = (utf8_cp << 6) | (u & 0x3F);
            if (--utf8_state == 0) put_codepoint(utf8_cp);
        } else {
            put_codepoint('?');
        }
    } else {
        utf8_state = 0;
        put_codepoint('?');
    }
}

void fb_tty_putchar(char c)
{
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    _putchar_locked((unsigned char)c);
    spin_unlock_irq(&fb_tty_lock, f);
    if (c == '\n') fb_tty_flush();
}

void fb_tty_puts(const char *s) { while (*s) fb_tty_putchar(*s++); }

void fb_tty_write(const char *buf, size_t len)
{
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    for (size_t i = 0; i < len; i++)
        _putchar_locked((unsigned char)buf[i]);
    spin_unlock_irq(&fb_tty_lock, f);
}

void fb_tty_clear(void)
{
    if (!_fb) return;
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    u32 shadow_size = _fb->pitch * _fb->height;
    u64 bg = (u64)_def_bg | ((u64)_def_bg << 32);
    u64 *p = (u64 *)shadow;
    for (u32 i = 0; i < shadow_size / 8; i++) p[i] = bg;
    _cur_col  = 0;
    _cur_row  = 0;
    _cur_fg   = _def_fg;
    _cur_bg   = _def_bg;
    _bold     = 0;
    _dirty_y0 = _fb->height;
    _dirty_y1 = 0;
    flush_all();
    cursor_draw_on_fb();
    spin_unlock_irq(&fb_tty_lock, f);
}

void fb_tty_show_cursor(int show)
{
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    if (!show) cursor_erase_from_fb();
    _cursor_visible = show;
    if (show) cursor_draw_on_fb();
    spin_unlock_irq(&fb_tty_lock, f);
}

void fb_tty_erase_eol(void)
{
    if (!_fb) return;
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    u32 saved = _cur_col;
    for (u32 c = _cur_col; c < _cols; c++) draw_glyph(c, _cur_row, ' ');
    _cur_col = saved;
    spin_unlock_irq(&fb_tty_lock, f);
}

void fb_tty_tick(void)
{
    if (!_cursor_visible || !_fb) return;
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    if (++_blink_ticks < (TIMER_HZ / 2)) {
        spin_unlock_irq(&fb_tty_lock, f);
        return;
    }
    _blink_ticks = 0;
    _blink_state ^= 1;
    _blink_dirty = 1;
    cursor_erase_from_fb();
    cursor_draw_on_fb();
    spin_unlock_irq(&fb_tty_lock, f);
}

void fb_tty_set_col(u32 col)
{
    uint64_t f = spin_lock_irq(&fb_tty_lock);
    cursor_erase_from_fb();
    _cur_col = col;
    cursor_draw_on_fb();
    spin_unlock_irq(&fb_tty_lock, f);
}

